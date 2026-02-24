// audio_engine.cpp - Core WASAPI loopback capture + multi-device render.

#include "audio_engine.h"
#include "common.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// How many float samples the ring buffer should hold.  We multiply the frame
// count by the channel count (stereo = 2) and then give generous headroom so
// that a stall in one render thread doesn't immediately cause drops.
static constexpr size_t RingCapacitySamples(UINT32 channels) {
    return static_cast<size_t>(BUFFER_SIZE_FRAMES) * channels * 4;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AudioEngine::AudioEngine(DeviceManager* device_mgr)
    : device_mgr_(device_mgr) {}

AudioEngine::~AudioEngine() {
    Stop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool AudioEngine::Start() {
    if (capture_running_.load()) {
        LOG_INFO("AudioEngine already running.");
        return true;
    }

    capture_running_.store(true);
    capture_thread_ = std::thread(&AudioEngine::CaptureThreadProc, this);
    LOG_INFO("AudioEngine started.");
    return true;
}

void AudioEngine::Stop() {
    if (!capture_running_.load()) return;

    capture_running_.store(false);
    restart_capture_.store(false);
    if (capture_thread_.joinable()) capture_thread_.join();

    // Stop all render targets.
    {
        std::lock_guard<std::mutex> lock(targets_mutex_);
        for (auto& t : targets_) {
            t->running.store(false);
            if (t->thread.joinable()) t->thread.join();
            if (t->audio_client)  t->audio_client->Stop();
            if (t->render_client) { t->render_client->Release(); t->render_client = nullptr; }
            if (t->audio_client)  { t->audio_client->Release();  t->audio_client  = nullptr; }
            if (t->event)         { CloseHandle(t->event);       t->event         = nullptr; }
        }
        targets_.clear();
    }

    LOG_INFO("AudioEngine stopped.");
}

bool AudioEngine::AddRenderDevice(const std::wstring& device_id) {
    // Check for duplicates AND mark this device as pending so that a
    // concurrent call with the same ID cannot pass the duplicate check
    // while we are setting up the device (TOCTOU fix).
    {
        std::lock_guard<std::mutex> lock(targets_mutex_);
        for (auto& t : targets_) {
            if (t->device_id == device_id) {
                LOG_INFO("Render device already added.");
                return true;
            }
        }
        if (pending_ids_.count(device_id)) {
            LOG_INFO("Render device is already being added.");
            return true;
        }
        pending_ids_.insert(device_id);
    }

    // RAII guard to remove the device from pending_ids_ on any exit path.
    struct PendingGuard {
        AudioEngine* self;
        const std::wstring& id;
        ~PendingGuard() {
            std::lock_guard<std::mutex> lock(self->targets_mutex_);
            self->pending_ids_.erase(id);
        }
    } pending_guard{this, device_id};

    IMMDevice* device = OpenDeviceById(device_id);
    if (!device) {
        LOG_ERROR("Failed to open render device.");
        return false;
    }

    // Get friendly name for logging.
    std::wstring friendly_name;
    {
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)) &&
                varName.vt == VT_LPWSTR && varName.pwszVal) {
                friendly_name = varName.pwszVal;
            }
            PropVariantClear(&varName);
            props->Release();
        }
    }

    // Activate IAudioClient.
    IAudioClient* audio_client = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                  nullptr, reinterpret_cast<void**>(&audio_client));
    device->Release();
    if (FAILED(hr) || !audio_client) {
        LOG_ERROR("Activate IAudioClient (render) failed: 0x%08lX",
                  static_cast<unsigned long>(hr));
        return false;
    }

    // Get the device mix format.
    WAVEFORMATEX* mix_fmt = nullptr;
    hr = audio_client->GetMixFormat(&mix_fmt);
    if (FAILED(hr) || !mix_fmt) {
        LOG_ERROR("GetMixFormat (render) failed: 0x%08lX",
                  static_cast<unsigned long>(hr));
        audio_client->Release();
        return false;
    }

    UINT32 channels = mix_fmt->nChannels;

    // Initialize in shared mode with auto-conversion so the engine always
    // feeds the same format from the capture side and WASAPI resamples as
    // needed.
    const DWORD stream_flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                               AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    // Request 100 ms buffer (in 100-ns units).
    const REFERENCE_TIME buffer_duration = 1000000; // 100 ms
    hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags,
                                  buffer_duration, 0, mix_fmt, nullptr);
    CoTaskMemFree(mix_fmt);
    if (FAILED(hr)) {
        LOG_ERROR("IAudioClient::Initialize (render) failed: 0x%08lX",
                  static_cast<unsigned long>(hr));
        audio_client->Release();
        return false;
    }

    // Create event for event-driven rendering.
    HANDLE render_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!render_event) {
        LOG_ERROR("CreateEvent for render failed.");
        audio_client->Release();
        return false;
    }
    hr = audio_client->SetEventHandle(render_event);
    if (FAILED(hr)) {
        LOG_ERROR("SetEventHandle (render) failed: 0x%08lX",
                  static_cast<unsigned long>(hr));
        CloseHandle(render_event);
        audio_client->Release();
        return false;
    }

    IAudioRenderClient* render_client = nullptr;
    hr = audio_client->GetService(__uuidof(IAudioRenderClient),
                                  reinterpret_cast<void**>(&render_client));
    if (FAILED(hr) || !render_client) {
        LOG_ERROR("GetService(IAudioRenderClient) failed: 0x%08lX",
                  static_cast<unsigned long>(hr));
        CloseHandle(render_event);
        audio_client->Release();
        return false;
    }

    // Build RenderTarget.
    auto target = std::make_unique<RenderTarget>();
    target->device_id     = device_id;
    target->device_name   = friendly_name;
    target->audio_client  = audio_client;
    target->render_client = render_client;
    target->channel_count = channels;
    target->event         = render_event;
    target->ring = std::make_unique<RingBuffer<float>>(
        RingCapacitySamples(channels));
    // Start playback BEFORE launching the render thread so the thread does
    // not call GetCurrentPadding on an un-started audio client.
    hr = audio_client->Start();
    if (FAILED(hr)) {
        LOG_ERROR("IAudioClient::Start (render) failed: 0x%08lX",
                  static_cast<unsigned long>(hr));
        render_client->Release();
        CloseHandle(render_event);
        audio_client->Release();
        return false;
    }

    target->running.store(true);

    // Start the render thread -- pass raw pointer (safe: we join before
    // destroying the target).
    RenderTarget* raw = target.get();
    target->thread = std::thread([this, raw]() {
        this->RenderThreadProc(raw);
    });

    {
        std::lock_guard<std::mutex> lock(targets_mutex_);
        targets_.push_back(std::move(target));
    }

    LOG_INFO("Render device added.");
    return true;
}

void AudioEngine::RemoveRenderDevice(const std::wstring& device_id) {
    std::unique_ptr<RenderTarget> removed;
    {
        std::lock_guard<std::mutex> lock(targets_mutex_);
        auto it = std::find_if(targets_.begin(), targets_.end(),
                               [&](const std::unique_ptr<RenderTarget>& t) {
                                   return t->device_id == device_id;
                               });
        if (it == targets_.end()) return;
        removed = std::move(*it);
        targets_.erase(it);
    }

    // Tear down outside the lock.
    removed->running.store(false);
    if (removed->thread.joinable()) removed->thread.join();
    if (removed->audio_client) removed->audio_client->Stop();
    if (removed->render_client) { removed->render_client->Release(); removed->render_client = nullptr; }
    if (removed->audio_client)  { removed->audio_client->Release();  removed->audio_client  = nullptr; }
    if (removed->event)         { CloseHandle(removed->event);       removed->event         = nullptr; }
    LOG_INFO("Render device removed.");
}

void AudioEngine::SetDeviceVolume(const std::wstring& device_id, float volume) {
    std::lock_guard<std::mutex> lock(targets_mutex_);
    for (auto& t : targets_) {
        if (t->device_id == device_id) {
            t->volume.store(std::clamp(volume, 0.0f, 1.0f));
            return;
        }
    }
}

std::vector<std::wstring> AudioEngine::GetActiveDeviceIds() {
    std::lock_guard<std::mutex> lock(targets_mutex_);
    std::vector<std::wstring> ids;
    ids.reserve(targets_.size());
    for (auto& t : targets_) ids.push_back(t->device_id);
    return ids;
}

bool AudioEngine::IsRunning() const {
    return capture_running_.load();
}

void AudioEngine::RequestCaptureRestart() {
    restart_capture_.store(true);
    LOG_INFO("Capture restart requested (default device changed).");
}

// ---------------------------------------------------------------------------
// Capture thread
// ---------------------------------------------------------------------------

void AudioEngine::CaptureThreadProc() {
    // Initialize COM for this thread.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LOG_ERROR("CoInitializeEx (capture thread) failed: 0x%08lX",
                  static_cast<unsigned long>(hr));
        capture_running_.store(false);
        return;
    }

    // Elevate to real-time audio priority.
    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);

    // Outer loop: restarts capture when the default device changes.
    while (capture_running_.load()) {
        restart_capture_.store(false);

        // Open the default render endpoint in loopback mode.
        IMMDeviceEnumerator* enumerator = nullptr;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr) || !enumerator) {
            LOG_ERROR("CoCreateInstance(MMDeviceEnumerator) in capture thread failed.");
            break;
        }

        IMMDevice* capture_device = nullptr;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &capture_device);
        enumerator->Release();
        if (FAILED(hr) || !capture_device) {
            LOG_ERROR("GetDefaultAudioEndpoint (loopback) failed: 0x%08lX",
                      static_cast<unsigned long>(hr));
            break;
        }

        IAudioClient* capture_client = nullptr;
        hr = capture_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                       nullptr, reinterpret_cast<void**>(&capture_client));
        capture_device->Release();
        if (FAILED(hr) || !capture_client) {
            LOG_ERROR("Activate IAudioClient (loopback) failed.");
            break;
        }

        WAVEFORMATEX* cap_fmt = nullptr;
        hr = capture_client->GetMixFormat(&cap_fmt);
        if (FAILED(hr) || !cap_fmt) {
            LOG_ERROR("GetMixFormat (loopback) failed.");
            capture_client->Release();
            break;
        }

        const UINT32 cap_channels = cap_fmt->nChannels;
        const WORD   cap_bits     = cap_fmt->wBitsPerSample;

        // Determine if the format is float or PCM.
        bool is_float = false;
        if (cap_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            is_float = true;
        } else if (cap_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(cap_fmt);
            if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
                is_float = true;
        }

        const DWORD loopback_flags = AUDCLNT_STREAMFLAGS_LOOPBACK |
                                     AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        const REFERENCE_TIME buffer_duration = 1000000; // 100 ms
        hr = capture_client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback_flags,
                                        buffer_duration, 0, cap_fmt, nullptr);
        CoTaskMemFree(cap_fmt);
        if (FAILED(hr)) {
            LOG_ERROR("IAudioClient::Initialize (loopback) failed: 0x%08lX",
                      static_cast<unsigned long>(hr));
            capture_client->Release();
            break;
        }

        // Event-driven capture.
        HANDLE capture_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!capture_event) {
            LOG_ERROR("CreateEvent (capture) failed.");
            capture_client->Release();
            break;
        }
        hr = capture_client->SetEventHandle(capture_event);
        if (FAILED(hr)) {
            LOG_ERROR("SetEventHandle (capture) failed: 0x%08lX",
                      static_cast<unsigned long>(hr));
            CloseHandle(capture_event);
            capture_client->Release();
            break;
        }

        IAudioCaptureClient* capture_service = nullptr;
        hr = capture_client->GetService(__uuidof(IAudioCaptureClient),
                                        reinterpret_cast<void**>(&capture_service));
        if (FAILED(hr) || !capture_service) {
            LOG_ERROR("GetService(IAudioCaptureClient) failed: 0x%08lX",
                      static_cast<unsigned long>(hr));
            CloseHandle(capture_event);
            capture_client->Release();
            break;
        }

        hr = capture_client->Start();
        if (FAILED(hr)) {
            LOG_ERROR("IAudioClient::Start (loopback) failed: 0x%08lX",
                      static_cast<unsigned long>(hr));
            capture_service->Release();
            CloseHandle(capture_event);
            capture_client->Release();
            break;
        }

        LOG_INFO("Capture thread started (channels=%u, bits=%u, float=%d).",
                 cap_channels, cap_bits, is_float ? 1 : 0);

        // Temporary buffer for format conversion.
        std::vector<float> convert_buf;

        // ---- Inner capture loop (exits on shutdown OR restart request) ----
        while (capture_running_.load() && !restart_capture_.load()) {
            DWORD wait_result = WaitForSingleObject(capture_event, 100);
            if (wait_result == WAIT_TIMEOUT) continue;
            if (wait_result != WAIT_OBJECT_0) break;

            BYTE*  data           = nullptr;
            UINT32 frames_avail   = 0;
            DWORD  flags          = 0;

            while (true) {
                hr = capture_service->GetBuffer(&data, &frames_avail, &flags,
                                                nullptr, nullptr);
                if (hr == AUDCLNT_S_BUFFER_EMPTY || frames_avail == 0) break;
                if (FAILED(hr)) {
                    LOG_ERROR("GetBuffer (capture) failed: 0x%08lX",
                              static_cast<unsigned long>(hr));
                    break;
                }

                const size_t total_samples = static_cast<size_t>(frames_avail) * cap_channels;
                const float* float_data = nullptr;

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    // Produce silence.
                    convert_buf.assign(total_samples, 0.0f);
                    float_data = convert_buf.data();
                } else if (is_float) {
                    // Data is already float -- just reinterpret.
                    float_data = reinterpret_cast<const float*>(data);
                } else if (cap_bits == 16) {
                    // 16-bit PCM -> float conversion.
                    convert_buf.resize(total_samples);
                    const int16_t* pcm = reinterpret_cast<const int16_t*>(data);
                    for (size_t i = 0; i < total_samples; ++i) {
                        convert_buf[i] = static_cast<float>(pcm[i]) / 32768.0f;
                    }
                    float_data = convert_buf.data();
                } else {
                    // Unsupported PCM format (24-bit, 32-bit int, etc.) -- produce silence.
                    LOG_ERROR("Unsupported capture PCM bit depth: %u. Producing silence.", cap_bits);
                    convert_buf.assign(total_samples, 0.0f);
                    float_data = convert_buf.data();
                }

                // Fan-out to all render ring buffers.
                {
                    std::lock_guard<std::mutex> lock(targets_mutex_);
                    for (auto& target : targets_) {
                        if (target->running.load()) {
                            target->ring->push(float_data, total_samples);
                        }
                    }
                }

                capture_service->ReleaseBuffer(frames_avail);
            }
        }

        // ---- Cleanup current capture session ----
        capture_client->Stop();
        capture_service->Release();
        CloseHandle(capture_event);
        capture_client->Release();

        if (restart_capture_.load() && capture_running_.load()) {
            LOG_INFO("Restarting capture for new default device...");
            Sleep(200);
            continue;
        }
    } // end outer while

    capture_running_.store(false);

    if (task) AvRevertMmThreadCharacteristics(task);
    CoUninitialize();

    LOG_INFO("Capture thread exited.");
}

// ---------------------------------------------------------------------------
// Render thread (one per output device)
// ---------------------------------------------------------------------------

void AudioEngine::RenderThreadProc(RenderTarget* target) {
    // Initialize COM for this thread.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LOG_ERROR("CoInitializeEx (render thread) failed.");
        target->running.store(false);
        return;
    }

    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);

    // Get the device buffer size.
    UINT32 buffer_frames = 0;
    hr = target->audio_client->GetBufferSize(&buffer_frames);
    if (FAILED(hr)) {
        LOG_ERROR("GetBufferSize (render) failed: 0x%08lX",
                  static_cast<unsigned long>(hr));
        target->running.store(false);
        if (task) AvRevertMmThreadCharacteristics(task);
        CoUninitialize();
        return;
    }

    const UINT32 channels = target->channel_count;
    std::vector<float> read_buf(static_cast<size_t>(buffer_frames) * channels);

    LOG_INFO("Render thread started (buffer_frames=%u, channels=%u).",
             buffer_frames, channels);

    // Event-driven render loop: WASAPI signals the event when it needs more
    // data.  We use WaitForSingleObject with a 100 ms timeout so that the
    // thread can exit promptly when running is set to false.
    while (target->running.load()) {
        DWORD wait_result = WaitForSingleObject(target->event, 100);
        if (wait_result == WAIT_TIMEOUT) continue;
        if (wait_result != WAIT_OBJECT_0) break;

        UINT32 padding = 0;
        hr = target->audio_client->GetCurrentPadding(&padding);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            LOG_ERROR("Render device invalidated.");
            break;
        }
        if (FAILED(hr)) continue;

        UINT32 frames_available = buffer_frames - padding;
        if (frames_available == 0) continue;

        BYTE* render_data = nullptr;
        hr = target->render_client->GetBuffer(frames_available, &render_data);
        if (FAILED(hr) || !render_data) continue;

        const size_t samples_needed = static_cast<size_t>(frames_available) * channels;
        const size_t samples_read   = target->ring->pop(read_buf.data(), samples_needed);

        // Copy captured audio into the render buffer (volume is controlled
        // via the Windows system volume on each endpoint, not a software
        // multiplier).
        float* dst = reinterpret_cast<float*>(render_data);
        std::memcpy(dst, read_buf.data(), samples_read * sizeof(float));

        // Fill any remaining samples with silence (ring buffer underrun).
        if (samples_read < samples_needed) {
            std::memset(dst + samples_read, 0,
                        (samples_needed - samples_read) * sizeof(float));
        }

        DWORD release_flags = 0;
        // If we wrote entirely silence (no data from ring), mark the buffer.
        if (samples_read == 0) release_flags = AUDCLNT_BUFFERFLAGS_SILENT;

        target->render_client->ReleaseBuffer(frames_available, release_flags);
    }

    // Cleanup.
    target->running.store(false);
    if (task) AvRevertMmThreadCharacteristics(task);
    CoUninitialize();
    LOG_INFO("Render thread exited.");
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

IMMDevice* AudioEngine::OpenDeviceById(const std::wstring& id) {
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) return nullptr;

    IMMDevice* device = nullptr;
    hr = enumerator->GetDevice(id.c_str(), &device);
    enumerator->Release();
    if (FAILED(hr)) return nullptr;
    return device;
}
