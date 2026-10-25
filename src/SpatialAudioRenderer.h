#pragma once

#include "LibISDB/LibISDB/Windows/Viewer/DirectShow/AudioDecoders/AudioDecoderFilter.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <wrl/client.h>
#include <spatialaudioclient.h>

namespace TVTest {

class CSpatialAudioRenderer final
    : public LibISDB::DirectShow::AudioDecoderFilter::SampleCallback
{
public:
    CSpatialAudioRenderer() = default;
    ~CSpatialAudioRenderer() override;

    bool Initialize();
    void Finalize();
    bool IsInitialized() const noexcept { return m_Stream != nullptr; }

    void OnSamples(short *pData, size_t Length, int Channels) override;
    void SetVolume(float Volume);

private:
    static constexpr int InputChannels = 24;
    void RenderThread();
    bool ActivateObjects();
    bool ResetStream();
    bool WriteObject(ISpatialAudioObject *pObject, const short *pData, size_t Frames, int Channel);
    bool WriteLFEObject(ISpatialAudioObject *pObject, const short *pData, size_t Frames);

    Microsoft::WRL::ComPtr<ISpatialAudioObjectRenderStream> m_Stream;
    Microsoft::WRL::ComPtr<ISpatialAudioObject> m_StaticObjects[12];
    Microsoft::WRL::ComPtr<ISpatialAudioObject> m_DynamicObjects[11];
    HANDLE m_Event = nullptr;
    std::thread m_Thread;
    std::mutex m_Mutex;
    std::vector<short> m_Queue;
    std::vector<short> m_FrameBuffer;
    size_t m_ReadFrame = 0;
    std::atomic<bool> m_Stop{false};
    std::atomic<float> m_Volume{1.0f};
    bool m_ObjectsActivated = false;
    bool m_FloatFormat = false;
};

} // namespace TVTest
