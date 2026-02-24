#pragma once
// audio_engine.h - Core WASAPI loopback capture + multi-device render engine.

#include "common.h"
#include "ring_buffer.h"
#include "device_manager.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// ---- Per-output-device state ----

struct RenderTarget {
    std::wstring                       device_id;
    std::wstring                       device_name;
    IAudioClient*                      audio_client  = nullptr;
    IAudioRenderClient*                render_client = nullptr;
    std::unique_ptr<RingBuffer<float>> ring;
    std::thread                        thread;
    std::atomic<bool>                  running{false};
    std::atomic<float>                 volume{1.0f};
    HANDLE                             event = nullptr;

    // Number of channels the render device is using.
    UINT32                             channel_count = 0;
};

// ---- AudioEngine ----

class AudioEngine {
public:
    explicit AudioEngine(DeviceManager* device_mgr);
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Start the loopback capture pipeline.
    bool Start();
    // Stop capture and all render targets.
    void Stop();

    // Manage render outputs.
    bool AddRenderDevice(const std::wstring& device_id);
    void RemoveRenderDevice(const std::wstring& device_id);
    void SetDeviceVolume(const std::wstring& device_id, float volume);

    // Queries.
    std::vector<std::wstring> GetActiveDeviceIds();
    bool IsRunning() const;

    // Signals the capture thread to restart (e.g., after default device change).
    void RequestCaptureRestart();

private:
    // Thread entry points.
    void CaptureThreadProc();
    void RenderThreadProc(RenderTarget* target);

    // Opens a WASAPI device by ID, returning the IMMDevice.  Caller owns it.
    IMMDevice* OpenDeviceById(const std::wstring& id);

    DeviceManager*                              device_mgr_;

    // Capture state.
    std::atomic<bool>                           capture_running_{false};
    std::atomic<bool>                           restart_capture_{false};
    std::thread                                 capture_thread_;

    // Render targets, guarded by mutex.  pending_ids_ tracks device IDs
    // that are currently being set up (between the duplicate check and the
    // insertion) to prevent TOCTOU races when AddRenderDevice is called
    // concurrently with the same device ID.
    std::mutex                                  targets_mutex_;
    std::vector<std::unique_ptr<RenderTarget>>  targets_;
    std::set<std::wstring>                      pending_ids_;
};
