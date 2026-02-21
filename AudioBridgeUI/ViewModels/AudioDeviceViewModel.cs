using System.Windows.Media;
using AudioBridgeUI.Models;
using AudioBridgeUI.Services;

namespace AudioBridgeUI.ViewModels;

/// <summary>
/// View model wrapping a single <see cref="AudioDevice"/>, exposing UI-friendly
/// properties for data binding in the device list.
/// </summary>
public class AudioDeviceViewModel : ViewModelBase
{
    // Cached brushes to avoid allocating a new SolidColorBrush on every property access.
    private static readonly SolidColorBrush BrushGreen = CreateFrozenBrush(0x4C, 0xAF, 0x50);
    private static readonly SolidColorBrush BrushRed   = CreateFrozenBrush(0xF4, 0x43, 0x36);
    private static readonly SolidColorBrush BrushGray  = CreateFrozenBrush(0x9E, 0x9E, 0x9E);

    private static SolidColorBrush CreateFrozenBrush(byte r, byte g, byte b)
    {
        var brush = new SolidColorBrush(Color.FromRgb(r, g, b));
        brush.Freeze();
        return brush;
    }

    private readonly EngineIpcClient _ipcClient;
    private bool _isActive;
    private bool _isBridged;
    private int _volume = 100;
    private bool _isTogglingBridge;

    /// <summary>
    /// Unique system identifier for the audio device.
    /// </summary>
    public string DeviceId { get; }

    /// <summary>
    /// Human-readable device name.
    /// </summary>
    public string DeviceName { get; }

    /// <summary>
    /// Whether the device is physically present and available.
    /// </summary>
    public bool IsActive
    {
        get => _isActive;
        set
        {
            if (SetProperty(ref _isActive, value))
                OnPropertyChanged(nameof(StatusBrush));
        }
    }

    /// <summary>
    /// Whether the device is currently added to the audio bridge.
    /// Setting this toggles the device in the engine via IPC.
    /// </summary>
    public bool IsBridged
    {
        get => _isBridged;
        set
        {
            if (_isBridged == value || _isTogglingBridge)
                return;

            _ = ToggleBridgeAsync(value);
        }
    }

    /// <summary>
    /// Per-device volume as an integer 0-100 (for slider binding).
    /// Internally converts to 0.0-1.0 when sending to the engine.
    /// </summary>
    public int Volume
    {
        get => _volume;
        set
        {
            int clamped = Math.Clamp(value, 0, 100);
            if (SetProperty(ref _volume, clamped) && _isBridged)
            {
                _ = SendVolumeAsync(clamped / 100f);
            }
        }
    }

    /// <summary>
    /// Whether the volume slider should be enabled (only when bridged and active).
    /// </summary>
    public bool IsVolumeEnabled => _isBridged && _isActive;

    /// <summary>
    /// A color brush reflecting the device state:
    /// green = bridged and active, gray = inactive, red = bridged but not active.
    /// </summary>
    public SolidColorBrush StatusBrush
    {
        get
        {
            if (_isBridged && _isActive)
                return BrushGreen;
            if (_isBridged && !_isActive)
                return BrushRed;
            return BrushGray;
        }
    }

    public AudioDeviceViewModel(AudioDevice device, EngineIpcClient ipcClient)
    {
        _ipcClient = ipcClient;
        DeviceId = device.DeviceId;
        DeviceName = device.DeviceName;
        _isActive = device.IsActive;
        _isBridged = device.IsBridged;
        _volume = (int)(device.Volume * 100);
    }

    /// <summary>
    /// Updates this view model's state from a fresh device model
    /// (used during periodic polling).
    /// </summary>
    public void UpdateFrom(AudioDevice device)
    {
        IsActive = device.IsActive;
        // IsBridged is not updated here — the engine's get_devices response does
        // not include bridge state.  That state is managed entirely by the UI
        // via add_device / remove_device IPC calls.
    }

    private async Task ToggleBridgeAsync(bool addToBridge)
    {
        _isTogglingBridge = true;
        try
        {
            bool success;
            if (addToBridge)
                success = await _ipcClient.AddDeviceAsync(DeviceId);
            else
                success = await _ipcClient.RemoveDeviceAsync(DeviceId);

            if (success)
            {
                _isBridged = addToBridge;
                OnPropertyChanged(nameof(IsBridged));
                OnPropertyChanged(nameof(IsVolumeEnabled));
                OnPropertyChanged(nameof(StatusBrush));
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Toggle bridge failed: {ex.Message}");
        }
        finally
        {
            _isTogglingBridge = false;
        }
    }

    private async Task SendVolumeAsync(float normalized)
    {
        try
        {
            await _ipcClient.SetVolumeAsync(DeviceId, normalized);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Set volume failed: {ex.Message}");
        }
    }
}
