#include "stdafx.h"
#include "SpatialAudioRenderer.h"
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <ksmedia.h>
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


bool IsSupportedObjectFormat(const WAVEFORMATEX *pFormat, bool *pFloatFormat)
{
    if (!pFormat || pFormat->nChannels != 1 || pFormat->nSamplesPerSec != 48000)
        return false;

    if (pFormat->wFormatTag == WAVE_FORMAT_PCM && pFormat->wBitsPerSample == 16) {
        *pFloatFormat = false;
        return true;
    }
    if (pFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && pFormat->wBitsPerSample == 32) {
        *pFloatFormat = true;
        return true;
    }
    if (pFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE
            && pFormat->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto *pExtensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(pFormat);
        if (pExtensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM
                && pFormat->wBitsPerSample == 16) {
            *pFloatFormat = false;
            return true;
        }
        if (pExtensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                && pFormat->wBitsPerSample == 32) {
            *pFloatFormat = true;
            return true;
        }
    }
    return false;
}

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

void CSpatialAudioRenderer::SetVolume(float Volume)
{
    m_Volume = std::clamp(Volume, 0.0f, 1.0f);
}

bool CSpatialAudioRenderer::Initialize()
{
    Finalize();

    WAVEFORMATEX DefaultFormat{};
    DefaultFormat.wFormatTag = WAVE_FORMAT_PCM;
    DefaultFormat.nChannels = 1;
    DefaultFormat.nSamplesPerSec = 48000;
    DefaultFormat.wBitsPerSample = 16;
    DefaultFormat.nBlockAlign = 2;
    DefaultFormat.nAvgBytesPerSec = 96000;

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
            || !Client)
        return false;

    std::vector<BYTE> FormatStorage;
    bool FloatFormat = false;
    bool FormatSelected = false;
    Microsoft::WRL::ComPtr<IAudioFormatEnumerator> FormatEnumerator;
    if (SUCCEEDED(Client->GetSupportedAudioObjectFormatEnumerator(&FormatEnumerator))) {
        for (UINT32 Index = 0; ; Index++) {
            WAVEFORMATEX *pFormat = nullptr;
            if (FAILED(FormatEnumerator->GetFormat(Index, &pFormat)))
                break;
            bool CandidateFloat = false;
            if (!FormatSelected && IsSupportedObjectFormat(pFormat, &CandidateFloat)) {
                const size_t FormatSize = sizeof(WAVEFORMATEX) + pFormat->cbSize;
                FormatStorage.resize(FormatSize);
                std::memcpy(FormatStorage.data(), pFormat, FormatSize);
                FloatFormat = CandidateFloat;
                FormatSelected = true;
            }
            ::CoTaskMemFree(pFormat);
        }
    }
    if (!FormatSelected) {
        bool CandidateFloat = false;
        if (!SUCCEEDED(Client->IsAudioObjectFormatSupported(&DefaultFormat))
                || !IsSupportedObjectFormat(&DefaultFormat, &CandidateFloat))
            return false;
        FormatStorage.resize(sizeof(DefaultFormat));
        std::memcpy(FormatStorage.data(), &DefaultFormat, sizeof(DefaultFormat));
        FloatFormat = CandidateFloat;
    }
    auto *pObjectFormat = reinterpret_cast<WAVEFORMATEX *>(FormatStorage.data());

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
    Params.ObjectFormat = pObjectFormat;
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
    m_FloatFormat = FloatFormat;

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
    if (!pData || m_Stop || Length == 0)
        return;

    std::vector<short> Expanded(Length * InputChannels);
    if (Channels == InputChannels) {
        std::memcpy(
            Expanded.data(), pData,
            Expanded.size() * sizeof(short));
    } else {
        const auto CopyChannel = [&](int Source, int Destination) {
            for (size_t i = 0; i < Length; i++)
                Expanded[i * InputChannels + Destination] =
                    pData[i * Channels + Source];
        };

        switch (Channels) {
        case 1:
            CopyChannel(0, 2);  // Mono -> FC
            break;
        case 2:
            CopyChannel(0, 6);  // Stereo FL -> FrontLeft bed
            CopyChannel(1, 7);  // Stereo FR -> FrontRight bed
            break;
        case 6:
            // 5.1: FL, FR, FC, LFE, SiL, SiR -> corresponding BED objects.
            CopyChannel(0, 6);
            CopyChannel(1, 7);
            CopyChannel(2, 2);
            CopyChannel(3, 3);
            CopyChannel(4, 10);
            CopyChannel(5, 11);
            break;
        default:
            return;
        }
    }

    constexpr size_t MaxFrames = 48000;
    const size_t StartFrame = Length > MaxFrames ? Length - MaxFrames : 0;
    const size_t FramesToInsert = Length - StartFrame;

    std::lock_guard Lock(m_Mutex);
    const size_t CurrentFrames = m_Queue.size() / InputChannels - m_ReadFrame;
    if (CurrentFrames + FramesToInsert > MaxFrames) {
        const size_t Drop = CurrentFrames + FramesToInsert - MaxFrames;
        m_ReadFrame += std::min(Drop, CurrentFrames);
    }

    m_Queue.insert(
        m_Queue.end(),
        Expanded.begin() + StartFrame * InputChannels,
        Expanded.begin() + Length * InputChannels);
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

bool CSpatialAudioRenderer::WriteObject(
    ISpatialAudioObject *pObject, const short *pData, size_t Frames, int Channel)
{
    if (!pObject || !pData || Channel < 0 || Channel >= InputChannels)
        return false;

    BYTE *Buffer = nullptr;
    UINT32 Bytes = 0;
    if (FAILED(pObject->GetBuffer(&Buffer, &Bytes)) || !Buffer)
        return false;

    const size_t SampleSize = m_FloatFormat ? sizeof(float) : sizeof(short);
    const size_t Available = std::min<size_t>(Frames, Bytes / SampleSize);
    if (Available != Frames)
        return false;

    if (m_FloatFormat) {
        auto *Destination = reinterpret_cast<float *>(Buffer);
        for (size_t i = 0; i < Available; i++)
            Destination[i] = std::clamp(
                static_cast<float>(pData[i * InputChannels + Channel])
                    / 32768.0f * m_Volume.load(), -1.0f, 1.0f);
    } else {
        auto *Destination = reinterpret_cast<short *>(Buffer);
        for (size_t i = 0; i < Available; i++)
            Destination[i] = Clamp16(
                static_cast<float>(pData[i * InputChannels + Channel])
                    * m_Volume.load());
    }

    return true;
}

bool CSpatialAudioRenderer::WriteLFEObject(
    ISpatialAudioObject *pObject, const short *pData, size_t Frames)
{
    if (!pObject || !pData)
        return false;

    BYTE *Buffer = nullptr;
    UINT32 Bytes = 0;
    if (FAILED(pObject->GetBuffer(&Buffer, &Bytes)) || !Buffer)
        return false;

    const size_t SampleSize = m_FloatFormat ? sizeof(float) : sizeof(short);
    const size_t Available = std::min<size_t>(Frames, Bytes / SampleSize);
    if (Available != Frames)
        return false;

    if (m_FloatFormat) {
        auto *Destination = reinterpret_cast<float *>(Buffer);
        for (size_t i = 0; i < Available; i++) {
            const float Value =
                (static_cast<float>(pData[i * InputChannels + 3])
                 + static_cast<float>(pData[i * InputChannels + 9]))
                * LFE_GAIN / 32768.0f * m_Volume.load();
            Destination[i] = std::clamp(Value, -1.0f, 1.0f);
        }
    } else {
        auto *Destination = reinterpret_cast<short *>(Buffer);
        for (size_t i = 0; i < Available; i++) {
            const float Value =
                (static_cast<float>(pData[i * InputChannels + 3])
                 + static_cast<float>(pData[i * InputChannels + 9]))
                * LFE_GAIN * m_Volume.load();
            Destination[i] = Clamp16(Value);
        }
    }

    return true;
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

        if (AvailableObjects < std::size(DynamicChannels)) {
            m_Stream->EndUpdatingAudioObjects();
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

        bool WriteOK = true;
        WriteOK = WriteObject(m_StaticObjects[0].Get(), Data.data(), Frames, 6) && WriteOK;
        WriteOK = WriteObject(m_StaticObjects[1].Get(), Data.data(), Frames, 7) && WriteOK;
        WriteOK = WriteObject(m_StaticObjects[2].Get(), Data.data(), Frames, 2) && WriteOK;
        WriteOK = WriteLFEObject(m_StaticObjects[3].Get(), Data.data(), Frames) && WriteOK;
        WriteOK = WriteObject(m_StaticObjects[4].Get(), Data.data(), Frames, 4) && WriteOK;
        WriteOK = WriteObject(m_StaticObjects[5].Get(), Data.data(), Frames, 5) && WriteOK;
        WriteOK = WriteObject(m_StaticObjects[6].Get(), Data.data(), Frames, 10) && WriteOK;
        WriteOK = WriteObject(m_StaticObjects[7].Get(), Data.data(), Frames, 11) && WriteOK;
        WriteOK = WriteObject(m_StaticObjects[8].Get(), Data.data(), Frames, 12) && WriteOK;
        WriteOK = WriteObject(m_StaticObjects[9].Get(), Data.data(), Frames, 13) && WriteOK;
        WriteOK = WriteObject(m_StaticObjects[10].Get(), Data.data(), Frames, 16) && WriteOK;
        WriteOK = WriteObject(m_StaticObjects[11].Get(), Data.data(), Frames, 17) && WriteOK;
        for (size_t i = 0; i < std::size(DynamicChannels); i++)
            WriteOK = WriteObject(
                m_DynamicObjects[i].Get(), Data.data(), Frames,
                DynamicChannels[i].Channel) && WriteOK;

        m_Stream->EndUpdatingAudioObjects();
        if (!WriteOK)
            ResetStream();
    }

    ::CoUninitialize();
}

} // namespace TVTest
