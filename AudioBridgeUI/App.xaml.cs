using System.Threading;
using System.Windows;
using H.NotifyIcon;
using AudioBridgeUI.Services;
using AudioBridgeUI.ViewModels;
using AudioBridgeUI.Models;

namespace AudioBridgeUI;

/// <summary>
/// Application entry point. Manages service lifetime, tray icon, and main window.
/// </summary>
public partial class App : Application
{
    private static readonly Mutex SingleInstanceMutex = new(true, "AudioBridge_SingleInstance");

    private SettingsService? _settingsService;
    private EngineIpcClient? _ipcClient;
    private EngineProcessManager? _processManager;
    private DeviceReconnectService? _reconnectService;
    private MainViewModel? _mainViewModel;
    private MainWindow? _mainWindow;
    private TaskbarIcon? _trayIcon;

    protected override async void OnStartup(StartupEventArgs e)
    {
        if (!SingleInstanceMutex.WaitOne(TimeSpan.Zero, exitContext: false))
        {
            // Another instance is already running.
            Shutdown();
            return;
        }

        base.OnStartup(e);

        // Create services.
        _settingsService = new SettingsService();
        _ipcClient = new EngineIpcClient();
        _processManager = new EngineProcessManager(_ipcClient);
        _reconnectService = new DeviceReconnectService(_ipcClient, _settingsService);

        // Start the engine process.
        _processManager.StartEngine();

        // Allow a brief moment for the engine to initialize its pipe server.
        await Task.Delay(500);

        // Create ViewModel and Window.
        _mainViewModel = new MainViewModel(_ipcClient, _settingsService);
        _mainWindow = new MainWindow { DataContext = _mainViewModel };

        // Initialize IPC connection and load initial data.
        await _mainViewModel.InitializeAsync();

        // Restore previously bridged devices from settings.
        await RestoreBridgedDevicesAsync();

        // Auto-start the bridge if the setting is enabled.
        BridgeSettings settings = _settingsService.LoadSettings();
        if (settings.AutoStartBridge && _ipcClient.IsConnected)
            await _ipcClient.StartBridgeAsync();

        // Start auto-reconnect monitoring.
        if (settings.AutoReconnect)
            _reconnectService.Start();

        // Listen for engine status changes.
        _processManager.EngineStatusChanged += OnEngineStatusChanged;

        // Resolve the tray icon from Application.Resources.
        _trayIcon = TryFindResource("TrayIcon") as TaskbarIcon;
        _trayIcon?.ForceCreate();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        // Save current settings.
        _mainViewModel?.SaveCurrentSettings();
        _mainViewModel?.Dispose();

        // Stop reconnect service.
        _reconnectService?.Dispose();

        // Stop the engine gracefully.
        if (_processManager is not null)
        {
            _processManager.StopEngineAsync().GetAwaiter().GetResult();
            _processManager.Dispose();
        }

        // Dispose IPC client.
        _ipcClient?.Dispose();

        // Dispose tray icon.
        _trayIcon?.Dispose();

        // Release the singleton mutex.
        try { SingleInstanceMutex.ReleaseMutex(); } catch (ApplicationException) { }

        base.OnExit(e);
    }

    /// <summary>
    /// Shows and activates the main window, restoring it from a minimized state if needed.
    /// </summary>
    private void ShowMainWindow()
    {
        if (_mainWindow is null)
            return;

        _mainWindow.Show();
        _mainWindow.WindowState = WindowState.Normal;
        _mainWindow.Activate();
    }

    /// <summary>
    /// Restores bridged devices from persisted settings on startup.
    /// </summary>
    private async Task RestoreBridgedDevicesAsync()
    {
        if (_ipcClient is null || _settingsService is null || !_ipcClient.IsConnected)
            return;

        BridgeSettings settings = _settingsService.LoadSettings();

        foreach (BridgedDeviceInfo device in settings.BridgedDevices)
        {
            bool added = await _ipcClient.AddDeviceAsync(device.DeviceId);
            if (added)
            {
                await _ipcClient.SetVolumeAsync(device.DeviceId, device.Volume);
            }
        }
    }

    private void OnEngineStatusChanged(object? sender, bool isRunning)
    {
        if (!isRunning && _ipcClient is not null)
        {
            // Engine stopped -- marshal to UI thread, wait for restart, then reconnect.
            Dispatcher.InvokeAsync(async () =>
            {
                try
                {
                    await Task.Delay(1000);
                    await (_mainViewModel?.InitializeAsync() ?? Task.CompletedTask);
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Debug.WriteLine($"Engine reconnect failed: {ex.Message}");
                }
            });
        }
    }

    // ----- Tray icon event handlers -----

    private void TrayIcon_TrayLeftMouseDown(object sender, RoutedEventArgs e)
    {
        ShowMainWindow();
    }

    private void TrayOpen_Click(object sender, RoutedEventArgs e)
    {
        ShowMainWindow();
    }

    private async void TrayToggleBridge_Click(object sender, RoutedEventArgs e)
    {
        if (_mainViewModel is null || _ipcClient is null)
            return;

        if (_mainViewModel.IsBridgeRunning)
        {
            await _ipcClient.StopBridgeAsync();
        }
        else
        {
            await _ipcClient.StartBridgeAsync();
        }

        // The polling timer in MainViewModel will pick up the new status.
    }

    private async void TrayExit_Click(object sender, RoutedEventArgs e)
    {
        // Stop the audio bridge if it is currently running.
        if (_ipcClient is not null && _mainViewModel is not null && _mainViewModel.IsBridgeRunning)
        {
            await _ipcClient.StopBridgeAsync();
        }

        // Shut down the engine process asynchronously to avoid deadlocking the UI thread.
        if (_processManager is not null)
        {
            await _processManager.StopEngineAsync();
            _processManager.Dispose();
            _processManager = null;
        }

        // Allow the main window to close instead of hiding to tray.
        if (_mainWindow is not null)
        {
            _mainWindow.AllowClose = true;
        }

        Shutdown();
    }
}
