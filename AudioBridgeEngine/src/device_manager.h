#pragma once
// device_manager.h - WASAPI device enumeration and change-notification.

#include "common.h"

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// ---- Public data type ----

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
    bool         active = false;   // true if the device state is DEVICE_STATE_ACTIVE
};

// ---- DeviceManager ----
// Enumerates audio render endpoints and receives plug/unplug notifications via
// IMMNotificationClient.  The wanted-device list is a simple in-memory set of
// device IDs that the user has chosen as output targets.

class DeviceManager final : public IMMNotificationClient {
public:
    DeviceManager();
    ~DeviceManager();

    // Non-copyable / non-movable.
    DeviceManager(const DeviceManager&)            = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    // ---- Lifecycle ----
    bool Initialize();
    void Shutdown();

    // ---- Enumeration ----
    std::vector<DeviceInfo> GetOutputDevices();
    std::wstring            GetDefaultOutputDeviceId();

    // ---- Notification callback ----
    void SetDeviceChangeCallback(std::function<void()> cb);

    // ---- Wanted-device tracking ----
    void AddWantedDevice(const std::wstring& id, const std::wstring& name);
    void RemoveWantedDevice(const std::wstring& id);
    std::vector<DeviceInfo> GetWantedDevices();
    bool IsDeviceWanted(const std::wstring& id);

    // ---- IUnknown ----
    ULONG STDMETHODCALLTYPE AddRef()  override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;

    // ---- IMMNotificationClient ----
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState)       override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId)                                override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId)                              override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override;

private:
    std::atomic<ULONG>       ref_count_{1};
    IMMDeviceEnumerator*     enumerator_ = nullptr;

    std::mutex               cb_mutex_;
    std::function<void()>    device_change_cb_;

    std::mutex               wanted_mutex_;
    std::vector<DeviceInfo>  wanted_devices_;
};
