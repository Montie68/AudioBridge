using System.Collections.ObjectModel;
using System.Windows.Input;
using System.Windows.Threading;
using AudioBridgeUI.Models;
using AudioBridgeUI.Services;
using Microsoft.Win32;

namespace AudioBridgeUI.ViewModels;

/// <summary>
/// Primary view model for the AudioBridge main window.
/// Manages device list, bridge status, and periodic engine polling.
/// </summary>
public class MainViewModel : ViewModelBase, IDisposable
{
    private const string StartupRegistryKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Run";
    private const string AppRegistryName = "AudioBridge";

    // Cached frozen brushes for the status indicator to avoid GC pressure.
    private static readonly System.Windows.Media.SolidColorBrush BrushRunning;
    private static readonly System.Windows.Media.SolidColorBrush BrushConnected;
    private static readonly System.Windows.Media.SolidColorBrush BrushDisconnected;

    static MainViewModel()
    {
        BrushRunning = new System.Windows.Media.SolidColorBrush(
            System.Windows.Media.Color.FromRgb(0x4C, 0xAF, 0x50));
        BrushRunning.Freeze();

        BrushConnected = new System.Windows.Media.SolidColorBrush(
            System.Windows.Media.Color.FromRgb(0xFF, 0xA0, 0x00));
        BrushConnected.Freeze();

        BrushDisconnected = new System.Windows.Media.SolidColorBrush(
            System.Windows.Media.Color.FromRgb(0xF4, 0x43, 0x36));
        BrushDisconnected.Freeze();
    }

    private readonly EngineIpcClient _ipcClient;
    private readonly SettingsService _settingsService;
    private readonly DispatcherTimer _pollTimer;
    private bool _isBridgeRunning;
    private bool _isEngineConnected;
    private string _statusText = "Disconnected";
    private bool _startWithWindows;
    private bool _autoStartBridge;
    private bool _disposed;

    /// <summary>
    /// The list of audio devices to display in the UI.
    /// </summary>
    public ObservableCollection<AudioDeviceViewModel> Devices { get; } = new();

    /// <summary>
    /// Whether the audio bridge is currently routing audio.
    /// </summary>
    public bool IsBridgeRunning
    {
        get => _isBridgeRunning;
        private set
        {
            if (SetProperty(ref _isBridgeRunning, value))
            {
                OnPropertyChanged(nameof(ToggleBridgeButtonText));
                OnPropertyChanged(nameof(StatusIndicatorBrush));
            }
        }
    }

    /// <summary>
    /// Whether the UI has an active IPC connection to the engine.
    /// </summary>
    public bool IsEngineConnected
    {
        get => _isEngineConnected;
        private set => SetProperty(ref _isEngineConnected, value);
    }

    /// <summary>
    /// Human-readable status text shown in the header bar.
    /// </summary>
    public string StatusText
    {
        get => _statusText;
        private set => SetProperty(ref _statusText, value);
    }

    /// <summary>
    /// Text for the start/stop toggle button.
    /// </summary>
    public string ToggleBridgeButtonText =>
        _isBridgeRunning ? "Stop Bridge" : "Start Bridge";

    /// <summary>
    /// Color brush for the status indicator circle in the header.
    /// </summary>
    public System.Windows.Media.SolidColorBrush StatusIndicatorBrush =>
        _isBridgeRunning
            ? BrushRunning
            : IsEngineConnected
                ? BrushConnected
                : BrushDisconnected;

    /// <summary>
    /// Whether the application should start with Windows.
    /// Persists the setting immediately on change.
    /// </summary>
    public bool StartWithWindows
    {
        get => _startWithWindows;
        set
        {
            if (SetProperty(ref _startWithWindows, value))
            {
                SetStartWithWindows(value);
                SaveCurrentSettings();
            }
        }
    }

    /// <summary>
    /// Whether the bridge should start automatically on application launch.
    /// Persists the setting immediately on change.
    /// </summary>
    public bool AutoStartBridge
    {
        get => _autoStartBridge;
        set
        {
            if (SetProperty(ref _autoStartBridge, value))
                SaveCurrentSettings();
        }
    }

    public ICommand ToggleBridgeCommand { get; }
    public ICommand RefreshDevicesCommand { get; }

    public MainViewModel(EngineIpcClient ipcClient, SettingsService settingsService)
    {
        _ipcClient = ipcClient;
        _settingsService = settingsService;

        ToggleBridgeCommand = new AsyncRelayCommand(ToggleBridgeAsync);
        RefreshDevicesCommand = new AsyncRelayCommand(RefreshDevicesAsync);

        // Load persisted preferences.
        BridgeSettings settings = _settingsService.LoadSettings();
        _startWithWindows = settings.StartWithWindows;
        _autoStartBridge = settings.AutoStartBridge;
        SetStartWithWindows(_startWithWindows);

        // Set up periodic polling.
        _pollTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromSeconds(2)
        };
        _pollTimer.Tick += async (_, _) =>
        {
            try
            {
                await PollEngineAsync();
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Poll engine error: {ex.Message}");
            }
        };
        _pollTimer.Start();
    }

    /// <summary>
    /// Attempts to connect the IPC client and performs initial device refresh.
    /// </summary>
    public async Task InitializeAsync()
    {
        bool connected = await _ipcClient.ConnectAsync();
        IsEngineConnected = connected;

        if (connected)
        {
            await RefreshDevicesAsync();
            await RefreshStatusAsync();
        }

        UpdateStatusText();
    }

    private async Task ToggleBridgeAsync()
    {
        try
        {
            bool success;
            if (_isBridgeRunning)
                success = await _ipcClient.StopBridgeAsync();
            else
                success = await _ipcClient.StartBridgeAsync();

            if (success)
            {
                IsBridgeRunning = !_isBridgeRunning;
                UpdateStatusText();
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Toggle bridge failed: {ex.Message}");
        }
    }

    private async Task RefreshDevicesAsync()
    {
        try
        {
            List<AudioDevice> devices = await _ipcClient.GetDevicesAsync();
            MergeDeviceList(devices);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Refresh devices failed: {ex.Message}");
        }
    }

    private async Task RefreshStatusAsync()
    {
        try
        {
            var status = await _ipcClient.GetStatusAsync();
            if (status.HasValue)
            {
                IsBridgeRunning = status.Value.IsRunning;
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Refresh status failed: {ex.Message}");
        }
    }

    private async Task PollEngineAsync()
    {
        if (!_ipcClient.IsConnected)
        {
            // Attempt reconnection.
            bool reconnected = await _ipcClient.ConnectAsync();
            IsEngineConnected = reconnected;

            if (!reconnected)
            {
                UpdateStatusText();
                return;
            }
        }

        IsEngineConnected = _ipcClient.IsConnected;
        await RefreshDevicesAsync();
        await RefreshStatusAsync();
        UpdateStatusText();
    }

    /// <summary>
    /// Merges the engine's device list into the observable collection,
    /// adding new devices, updating existing ones, and removing stale ones.
    /// </summary>
    private void MergeDeviceList(List<AudioDevice> devices)
    {
        // Build a lookup of existing VMs by device ID.
        var existingVms = new Dictionary<string, AudioDeviceViewModel>();
        foreach (var vm in Devices)
            existingVms[vm.DeviceId] = vm;

        var currentIds = new HashSet<string>();

        foreach (AudioDevice device in devices)
        {
            currentIds.Add(device.DeviceId);

            if (existingVms.TryGetValue(device.DeviceId, out var existing))
            {
                existing.UpdateFrom(device);
            }
            else
            {
                Devices.Add(new AudioDeviceViewModel(device, _ipcClient));
            }
        }

        // Remove devices that are no longer reported by the engine.
        for (int i = Devices.Count - 1; i >= 0; i--)
        {
            if (!currentIds.Contains(Devices[i].DeviceId))
                Devices.RemoveAt(i);
        }
    }

    private void UpdateStatusText()
    {
        if (!IsEngineConnected)
        {
            StatusText = "Engine Disconnected";
            OnPropertyChanged(nameof(StatusIndicatorBrush));
            return;
        }

        if (_isBridgeRunning)
        {
            int bridgedCount = 0;
            foreach (var d in Devices)
            {
                if (d.IsBridged)
                    bridgedCount++;
            }

            StatusText = bridgedCount == 1
                ? "Bridge Active - 1 device"
                : $"Bridge Active - {bridgedCount} devices";
        }
        else
        {
            StatusText = "Stopped";
        }

        OnPropertyChanged(nameof(StatusIndicatorBrush));
    }

    /// <summary>
    /// Builds the current settings from the device list and persists them.
    /// </summary>
    public void SaveCurrentSettings()
    {
        var settings = new BridgeSettings
        {
            StartWithWindows = _startWithWindows,
            AutoStartBridge = _autoStartBridge,
            AutoReconnect = true,
            BridgedDevices = new List<BridgedDeviceInfo>()
        };

        foreach (var device in Devices)
        {
            if (device.IsBridged)
            {
                settings.BridgedDevices.Add(new BridgedDeviceInfo
                {
                    DeviceId = device.DeviceId,
                    DeviceName = device.DeviceName,
                    Volume = device.Volume / 100f
                });
            }
        }

        _settingsService.SaveSettings(settings);
    }

    private static void SetStartWithWindows(bool enable)
    {
        try
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey(StartupRegistryKey, writable: true);
            if (key is null)
                return;

            if (enable)
            {
                string exePath = Environment.ProcessPath ?? string.Empty;
                if (!string.IsNullOrEmpty(exePath))
                    key.SetValue(AppRegistryName, $"\"{exePath}\"");
            }
            else
            {
                key.DeleteValue(AppRegistryName, throwOnMissingValue: false);
            }
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Failed to set startup registry: {ex.Message}");
        }
    }

    public void Dispose()
    {
        if (_disposed)
            return;

        _disposed = true;
        _pollTimer.Stop();
    }
}
