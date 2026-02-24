using AudioBridgeUI.Models;

namespace AudioBridgeUI.Services;

/// <summary>
/// Periodically checks the engine's device list and automatically re-adds
/// previously bridged devices when they become available again.
/// </summary>
public sealed class DeviceReconnectService : IDisposable
{
    private const int PollIntervalMs = 2000;
    private const int MaxRetries = 3;
    private static readonly int[] BackoffMs = { 100, 200, 400 };

    private readonly EngineIpcClient _ipcClient;
    private readonly SettingsService _settingsService;
    private readonly Dictionary<string, int> _retryAttempts = new();
    private CancellationTokenSource? _cts;
    private Task? _pollingTask;
    private bool _disposed;

    public DeviceReconnectService(EngineIpcClient ipcClient, SettingsService settingsService)
    {
        _ipcClient = ipcClient;
        _settingsService = settingsService;
    }

    /// <summary>
    /// Begins the periodic reconnection polling loop.
    /// </summary>
    public void Start()
    {
        if (_pollingTask is not null)
            return;

        _cts = new CancellationTokenSource();
        _pollingTask = PollLoopAsync(_cts.Token);
    }

    /// <summary>
    /// Stops the polling loop.
    /// </summary>
    public void Stop()
    {
        _cts?.Cancel();
        _pollingTask = null;
        _cts?.Dispose();
        _cts = null;
        _retryAttempts.Clear();
    }

    private async Task PollLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(PollIntervalMs, cancellationToken);
                await TryReconnectDevicesAsync(cancellationToken);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Reconnect poll error: {ex.Message}");
            }
        }
    }

    private async Task TryReconnectDevicesAsync(CancellationToken cancellationToken)
    {
        if (!_ipcClient.IsConnected)
            return;

        BridgeSettings settings = _settingsService.LoadSettings();
        if (!settings.AutoReconnect || settings.BridgedDevices.Count == 0)
            return;

        List<AudioDevice> currentDevices = await _ipcClient.GetDevicesAsync(cancellationToken);
        if (currentDevices.Count == 0)
            return;

        // Fetch the set of device IDs already active in the bridge to avoid
        // duplicate add_device calls (IsBridged from get_devices is unreliable).
        var status = await _ipcClient.GetStatusAsync(cancellationToken);
        HashSet<string> activeIds = status.HasValue
            ? new HashSet<string>(status.Value.ActiveDeviceIds, StringComparer.OrdinalIgnoreCase)
            : new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        foreach (BridgedDeviceInfo wanted in settings.BridgedDevices)
        {
            AudioDevice? match = currentDevices.Find(d =>
                d.DeviceId == wanted.DeviceId && d.IsActive && !activeIds.Contains(d.DeviceId));

            if (match is null)
                continue;

            int attempts = _retryAttempts.GetValueOrDefault(wanted.DeviceId, 0);
            if (attempts >= MaxRetries)
                continue;

            // Apply exponential backoff before retrying.
            if (attempts > 0)
            {
                int delay = BackoffMs[Math.Min(attempts, BackoffMs.Length - 1)];
                await Task.Delay(delay, cancellationToken);
            }

            bool added = await _ipcClient.AddDeviceAsync(wanted.DeviceId, cancellationToken);
            if (added)
            {
                // Restore the persisted volume level.
                await _ipcClient.SetVolumeAsync(wanted.DeviceId, wanted.Volume, cancellationToken);
                _retryAttempts.Remove(wanted.DeviceId);
            }
            else
            {
                _retryAttempts[wanted.DeviceId] = attempts + 1;
            }
        }
    }

    public void Dispose()
    {
        if (_disposed)
            return;

        _disposed = true;
        Stop();
    }
}
