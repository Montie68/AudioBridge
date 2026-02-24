// device_manager.cpp - WASAPI device enumeration and change-notification.

#include "device_manager.h"
#include "common.h"

#include <endpointvolume.h>
#include <algorithm>
#include <cassert>

#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DeviceManager::DeviceManager() = default;

DeviceManager::~DeviceManager() {
    Shutdown();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool DeviceManager::Initialize() {
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator_));
    HR_CHECK(hr, "CoCreateInstance(MMDeviceEnumerator)");

    hr = enumerator_->RegisterEndpointNotificationCallback(this);
    if (FAILED(hr)) {
        LOG_ERROR("RegisterEndpointNotificationCallback failed: 0x%08lX",
                  static_cast<unsigned long>(hr));
        // Non-fatal -- we can still enumerate devices.
    }

    LOG_INFO("DeviceManager initialized.");
    return true;
}

void DeviceManager::Shutdown() {
    if (enumerator_) {
        enumerator_->UnregisterEndpointNotificationCallback(this);
        enumerator_->Release();
        enumerator_ = nullptr;
    }
    LOG_INFO("DeviceManager shut down.");
}

// ---------------------------------------------------------------------------
// Enumeration helpers
// ---------------------------------------------------------------------------

std::vector<DeviceInfo> DeviceManager::GetOutputDevices() {
    std::vector<DeviceInfo> result;
    if (!enumerator_) return result;

    IMMDeviceCollection* collection = nullptr;
    HRESULT hr = enumerator_->EnumAudioEndpoints(
        eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        LOG_ERROR("EnumAudioEndpoints failed: 0x%08lX",
                  static_cast<unsigned long>(hr));
        return result;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(i, &device)) || !device) continue;

        DeviceInfo info;
        info.active = true; // We only enumerated DEVICE_STATE_ACTIVE.

        // Device ID.
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            info.id = id;
            CoTaskMemFree(id);
        }

        // Friendly name via property store.
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)) &&
                varName.vt == VT_LPWSTR && varName.pwszVal) {
                info.name = varName.pwszVal;
            }
            PropVariantClear(&varName);
            props->Release();
        }

        device->Release();
        result.push_back(std::move(info));
    }

    collection->Release();
    return result;
}

std::wstring DeviceManager::GetDefaultOutputDeviceId() {
    std::wstring result;
    if (!enumerator_) return result;

    IMMDevice* device = nullptr;
    HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) return result;

    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        result = id;
        CoTaskMemFree(id);
    }
    device->Release();
    return result;
}

// ---------------------------------------------------------------------------
// System volume control
// ---------------------------------------------------------------------------

float DeviceManager::GetDeviceVolume(const std::wstring& device_id) {
    if (!enumerator_ || device_id.empty()) return 1.0f;

    IMMDevice* device = nullptr;
    HRESULT hr = enumerator_->GetDevice(device_id.c_str(), &device);
    if (FAILED(hr) || !device) return 1.0f;

    IAudioEndpointVolume* endpoint_vol = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                          nullptr, reinterpret_cast<void**>(&endpoint_vol));
    device->Release();
    if (FAILED(hr) || !endpoint_vol) return 1.0f;

    float level = 1.0f;
    endpoint_vol->GetMasterVolumeLevelScalar(&level);
    endpoint_vol->Release();
    return level;
}

bool DeviceManager::SetDeviceVolume(const std::wstring& device_id, float volume) {
    if (!enumerator_ || device_id.empty()) return false;

    IMMDevice* device = nullptr;
    HRESULT hr = enumerator_->GetDevice(device_id.c_str(), &device);
    if (FAILED(hr) || !device) return false;

    IAudioEndpointVolume* endpoint_vol = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                          nullptr, reinterpret_cast<void**>(&endpoint_vol));
    device->Release();
    if (FAILED(hr) || !endpoint_vol) return false;

    hr = endpoint_vol->SetMasterVolumeLevelScalar(
        std::clamp(volume, 0.0f, 1.0f), nullptr);
    endpoint_vol->Release();
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// Notification callback
// ---------------------------------------------------------------------------

void DeviceManager::SetDeviceChangeCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    device_change_cb_ = std::move(cb);
}

void DeviceManager::SetDefaultDeviceChangedCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    default_device_changed_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Wanted-device list
// ---------------------------------------------------------------------------

void DeviceManager::AddWantedDevice(const std::wstring& id,
                                     const std::wstring& name) {
    std::lock_guard<std::mutex> lock(wanted_mutex_);
    // Avoid duplicates.
    for (auto& d : wanted_devices_) {
        if (d.id == id) {
            d.name = name; // Update name in case it changed.
            return;
        }
    }
    wanted_devices_.push_back({id, name, true});
    LOG_INFO("Added wanted device.");
}

void DeviceManager::RemoveWantedDevice(const std::wstring& id) {
    std::lock_guard<std::mutex> lock(wanted_mutex_);
    wanted_devices_.erase(
        std::remove_if(wanted_devices_.begin(), wanted_devices_.end(),
                       [&](const DeviceInfo& d) { return d.id == id; }),
        wanted_devices_.end());
}

std::vector<DeviceInfo> DeviceManager::GetWantedDevices() {
    std::lock_guard<std::mutex> lock(wanted_mutex_);
    return wanted_devices_;
}

bool DeviceManager::IsDeviceWanted(const std::wstring& id) {
    std::lock_guard<std::mutex> lock(wanted_mutex_);
    return std::any_of(wanted_devices_.begin(), wanted_devices_.end(),
                       [&](const DeviceInfo& d) { return d.id == id; });
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------

ULONG STDMETHODCALLTYPE DeviceManager::AddRef() {
    return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE DeviceManager::Release() {
    assert(ref_count_.load(std::memory_order_relaxed) > 0);
    ULONG r = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    // Do NOT delete this -- the DeviceManager is stack/member-owned, not
    // COM-allocated. ref_count_ is only here to satisfy IUnknown's contract.
    return r;
}

HRESULT STDMETHODCALLTYPE DeviceManager::QueryInterface(REFIID riid,
                                                          void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IMMNotificationClient)) {
        *ppv = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

// ---------------------------------------------------------------------------
// IMMNotificationClient
// ---------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE DeviceManager::OnDeviceStateChanged(
    LPCWSTR /*pwstrDeviceId*/, DWORD /*dwNewState*/) {
    LOG_INFO("Device state changed.");
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb = device_change_cb_;
    }
    if (cb) cb();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceManager::OnDeviceAdded(LPCWSTR /*pwstrDeviceId*/) {
    LOG_INFO("Device added.");
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb = device_change_cb_;
    }
    if (cb) cb();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceManager::OnDeviceRemoved(LPCWSTR /*pwstrDeviceId*/) {
    LOG_INFO("Device removed.");
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb = device_change_cb_;
    }
    if (cb) cb();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceManager::OnDefaultDeviceChanged(
    EDataFlow flow, ERole /*role*/, LPCWSTR /*pwstrDefaultDeviceId*/) {
    if (flow == eRender) {
        LOG_INFO("Default render device changed.");
        std::function<void()> general_cb;
        std::function<void()> default_cb;
        {
            std::lock_guard<std::mutex> lock(cb_mutex_);
            general_cb = device_change_cb_;
            default_cb = default_device_changed_cb_;
        }
        if (general_cb) general_cb();
        if (default_cb) default_cb();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceManager::OnPropertyValueChanged(
    LPCWSTR /*pwstrDeviceId*/, const PROPERTYKEY /*key*/) {
    // Property changes (e.g. volume, name) -- no action needed.
    return S_OK;
}
