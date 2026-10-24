#include "stdafx.h"
#include "SpatialAudioRenderer.h"
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "Mmdevapi.lib")

namespace TVTest {

namespace {

constexpr float LFE_GAIN = 0.707945784f;

struct DynamicChannel {
    int Channel;
    float X, Y, Z;
};

constexpr DynamicChannel DynamicChannels[] = {
    {0, -2.165f, 0.0f, -1.25f},
    {1,  2.165f, 0.0f, -1.25f},
    {8,  0.0f,   0.0f,  1.524f},
    {14, 0.0f,   1.238f, -0.876f},
    {15, 0.0f,   1.238f,  0.0f},
    {18, -0.876f, 1.238f, 0.0f},
    {19,  0.876f, 1.238f, 0.0f},
    {20, 0.0f,   1.238f, 0.876f},
    {21, 0.0f,  -1.238f, -2.5f},
    {22, -2.165f, -1.238f, -1.25f},
    {23,  2.165f, -1.238f, -1.25f},
};

template<typename T>
short Clamp16(T Value)
{
    if (Value > 32767)
        return 32767;
    if (Value < -32768)
        return -32768;
    return static_cast<short>(Value);
}

} // namespace

CSpatialAudioRenderer::~CSpatialAudioRenderer()
{
    Finalize();
}

bool CSpatialAudioRenderer::Initialize()
{
    Finalize();

    WAVEFORMATEX Format{};
    Format.wFormatTag = WAVE_FORMAT_PCM;
    Format.nChannels = 1;
    Format.nSamplesPerSec = 48000;
    Format.wBitsPerSample = 16;
    Format.nBlockAlign = 2;
    Format.nAvgBytesPerSec = 96000;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> Enumerator;
    Microsoft::WRL::ComPtr<IMMDevice> Device;
    Microsoft::WRL::ComPtr<ISpatialAudioClient> Client;
    if (FAILED(::CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(&Enumerator)))
            || FAILED(Enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &Device))
            || FAILED(Device->Activate(
                __uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                reinterpret_cast<void **>(Client.GetAddressOf())))
            || FAILED(Client->IsAudioObjectFormatSupported(&Format)))
        return false;

    AudioObjectType StaticMask =
        static_cast<AudioObjectType>(
            AudioObjectType_FrontLeft |
            AudioObjectType_FrontRight |
            AudioObjectType_FrontCenter |
            AudioObjectType_LowFrequency |
            AudioObjectType_SideLeft |
            AudioObjectType_SideRight |
            AudioObjectType_BackLeft |
            AudioObjectType_BackRight |
            AudioObjectType_TopFrontLeft |
            AudioObjectType_TopFrontRight |
            AudioObjectType_TopBackLeft |
            AudioObjectType_TopBackRight);

    AudioObjectType NativeMask = AudioObjectType_None;
    if (FAILED(Client->GetNativeStaticObjectTypeMask(&NativeMask))
            || (NativeMask & StaticMask) != StaticMask)
        return false;

    UINT32 MaxDynamicObjects = 0;
    if (FAILED(Client->GetMaxDynamicObjectCount(&MaxDynamicObjects))
            || MaxDynamicObjects < std::size(DynamicChannels))
        return false;

    m_Event = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_Event)
        return false;

    SpatialAudioObjectRenderStreamActivationParams Params{};
    Params.ObjectFormat = &Format;
    Params.StaticObjectTypeMask = StaticMask;
    Params.MinDynamicObjectCount = static_cast<UINT32>(std::size(DynamicChannels));
    Params.MaxDynamicObjectCount = static_cast<UINT32>(std::size(DynamicChannels));
    Params.Category = AudioCategory_GameMedia;
    Params.EventHandle = m_Event;

    PROPVARIANT Activation{};
    ::PropVariantInit(&Activation);
    Activation.vt = VT_BLOB;
    Activation.blob.cbSize = sizeof(Params);
    Activation.blob.pBlobData = reinterpret_cast<BYTE *>(&Params);

    HRESULT hr = Client->ActivateSpatialAudioStream(
        &Activation, IID_PPV_ARGS(m_Stream.GetAddressOf()));
    // Activation.blob.pBlobData points to the stack-local Params object.
    // Prevent PropVariantClear() from trying to free that non-owned memory.
    Activation.vt = VT_EMPTY;
    Activation.blob.pBlobData = nullptr;
    Activation.blob.cbSize = 0;
    ::PropVariantClear(&Activation);
    if (FAILED(hr) || !m_Stream || FAILED(m_Stream->Start()))
        return false;

    m_ObjectsActivated = false;

    {
        std::lock_guard Lock(m_Mutex);
        m_Queue.clear();
        m_ReadFrame = 0;
    }
    m_Stop = false;
    m_Thread = std::thread(&CSpatialAudioRenderer::RenderThread, this);
    return true;
}

void CSpatialAudioRenderer::Finalize()
{
    m_Stop = true;
    if (m_Event)
        ::SetEvent(m_Event);
    if (m_Thread.joinable())
        m_Thread.join();
    if (m_Stream)
        m_Stream->Stop();
    for (auto &Object : m_StaticObjects)
        Object.Reset();
    for (auto &Object : m_DynamicObjects)
        Object.Reset();
    m_Stream.Reset();
    if (m_Event) {
        ::CloseHandle(m_Event);
        m_Event = nullptr;
    }
    std::lock_guard Lock(m_Mutex);
    m_Queue.clear();
    m_ReadFrame = 0;
}

bool CSpatialAudioRenderer::ResetStream()
{
    if (!m_Stream)
        return false;

    const HRESULT hr = m_Stream->Reset();
    for (auto &Object : m_StaticObjects)
        Object.Reset();
    for (auto &Object : m_DynamicObjects)
        Object.Reset();
    m_ObjectsActivated = false;

    std::lock_guard Lock(m_Mutex);
    m_Queue.clear();
    m_ReadFrame = 0;
    return SUCCEEDED(hr);
}

void CSpatialAudioRenderer::OnSamples(short *pData, size_t Length, int Channels)
{
    if (!pData || Channels != InputChannels || m_Stop)
        return;

    std::lock_guard Lock(m_Mutex);
    const size_t CurrentFrames = m_Queue.size() / InputChannels - m_ReadFrame;
    constexpr size_t MaxFrames = 48000;
    if (CurrentFrames + Length > MaxFrames) {
        const size_t Drop = CurrentFrames + Length - MaxFrames;
        m_ReadFrame += std::min(Drop, CurrentFrames);
    }
    m_Queue.insert(m_Queue.end(), pData, pData + Length * InputChannels);
}

bool CSpatialAudioRenderer::ActivateObjects()
{
    const AudioObjectType StaticTypes[] = {
        AudioObjectType_FrontLeft, AudioObjectType_FrontRight,
        AudioObjectType_FrontCenter, AudioObjectType_LowFrequency,
        AudioObjectType_BackLeft, AudioObjectType_BackRight,
        AudioObjectType_SideLeft, AudioObjectType_SideRight,
        AudioObjectType_TopFrontLeft, AudioObjectType_TopFrontRight,
        AudioObjectType_TopBackLeft, AudioObjectType_TopBackRight,
    };
    for (size_t i = 0; i < std::size(StaticTypes); i++) {
        if (FAILED(m_Stream->ActivateSpatialAudioObject(StaticTypes[i], &m_StaticObjects[i])))
            return false;
    }
    for (size_t i = 0; i < std::size(DynamicChannels); i++) {
        if (FAILED(m_Stream->ActivateSpatialAudioObject(
                    AudioObjectType_Dynamic, &m_DynamicObjects[i]))
                || FAILED(m_DynamicObjects[i]->SetPosition(
                    DynamicChannels[i].X, DynamicChannels[i].Y, DynamicChannels[i].Z)))
            return false;
    }
    m_ObjectsActivated = true;
    return true;
}

void CSpatialAudioRenderer::WriteObject(
    ISpatialAudioObject *pObject, const short *pData, size_t Frames, int Channel)
{
    BYTE *Buffer = nullptr;
    UINT32 Bytes = 0;
    if (FAILED(pObject->GetBuffer(&Buffer, &Bytes)))
        return;
    const size_t Available = std::min<size_t>(Frames, Bytes / sizeof(short));
    auto *Destination = reinterpret_cast<short *>(Buffer);
    for (size_t i = 0; i < Available; i++)
        Destination[i] = pData[i * InputChannels + Channel];
}

void CSpatialAudioRenderer::WriteLFEObject(
    ISpatialAudioObject *pObject, const short *pData, size_t Frames)
{
    BYTE *Buffer = nullptr;
    UINT32 Bytes = 0;
    if (FAILED(pObject->GetBuffer(&Buffer, &Bytes)))
        return;
    const size_t Available = std::min<size_t>(Frames, Bytes / sizeof(short));
    auto *Destination = reinterpret_cast<short *>(Buffer);
    for (size_t i = 0; i < Available; i++) {
        const float Value =
            (static_cast<float>(pData[i * InputChannels + 3])
             + static_cast<float>(pData[i * InputChannels + 9])) * LFE_GAIN;
        Destination[i] = Clamp16(Value);
    }
}

void CSpatialAudioRenderer::RenderThread()
{
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    unsigned int TimeoutCount = 0;
    while (!m_Stop) {
        const DWORD WaitResult = ::WaitForSingleObject(m_Event, 100);
        if (WaitResult != WAIT_OBJECT_0) {
            if (WaitResult == WAIT_TIMEOUT && ++TimeoutCount >= 10) {
                ResetStream();
                TimeoutCount = 0;
            }
            continue;
        }
        TimeoutCount = 0;

        UINT32 AvailableObjects = 0;
        UINT32 Frames = 0;
        if (FAILED(m_Stream->BeginUpdatingAudioObjects(&AvailableObjects, &Frames))) {
            ResetStream();
            continue;
        }

        if (!m_ObjectsActivated && !ActivateObjects()) {
            m_Stream->EndUpdatingAudioObjects();
            ResetStream();
            continue;
        }

        std::vector<short> Data(static_cast<size_t>(Frames) * InputChannels);
        {
            std::lock_guard Lock(m_Mutex);
            const size_t Available = m_Queue.size() / InputChannels - m_ReadFrame;
            const size_t CopyFrames = std::min<size_t>(Frames, Available);
            if (CopyFrames != 0) {
                std::memcpy(
                    Data.data(),
                    m_Queue.data() + m_ReadFrame * InputChannels,
                    CopyFrames * InputChannels * sizeof(short));
                m_ReadFrame += CopyFrames;
            }
            if (m_ReadFrame > 4096) {
                m_Queue.erase(m_Queue.begin(), m_Queue.begin() + m_ReadFrame * InputChannels);
                m_ReadFrame = 0;
            }
        }

        WriteObject(m_StaticObjects[0].Get(), Data.data(), Frames, 6);
        WriteObject(m_StaticObjects[1].Get(), Data.data(), Frames, 7);
        WriteObject(m_StaticObjects[2].Get(), Data.data(), Frames, 2);
        WriteLFEObject(m_StaticObjects[3].Get(), Data.data(), Frames);
        WriteObject(m_StaticObjects[4].Get(), Data.data(), Frames, 4);
        WriteObject(m_StaticObjects[5].Get(), Data.data(), Frames, 5);
        WriteObject(m_StaticObjects[6].Get(), Data.data(), Frames, 10);
        WriteObject(m_StaticObjects[7].Get(), Data.data(), Frames, 11);
        WriteObject(m_StaticObjects[8].Get(), Data.data(), Frames, 12);
        WriteObject(m_StaticObjects[9].Get(), Data.data(), Frames, 13);
        WriteObject(m_StaticObjects[10].Get(), Data.data(), Frames, 16);
        WriteObject(m_StaticObjects[11].Get(), Data.data(), Frames, 17);
        for (size_t i = 0; i < std::size(DynamicChannels); i++)
            WriteObject(m_DynamicObjects[i].Get(), Data.data(), Frames, DynamicChannels[i].Channel);

        m_Stream->EndUpdatingAudioObjects();
    }

    ::CoUninitialize();
}

} // namespace TVTest
