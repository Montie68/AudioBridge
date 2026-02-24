using System.Diagnostics;
using System.IO;

namespace AudioBridgeUI.Services;

/// <summary>
/// Manages the lifecycle of the AudioBridgeEngine C++ process,
/// including startup, monitoring, and auto-restart on crash.
/// </summary>
public sealed class EngineProcessManager : IDisposable
{
    private const string EngineFileName = "AudioBridgeEngine.exe";
    private const int MaxAutoRestarts = 3;
    private const int ShutdownWaitMs = 3000;

    private readonly EngineIpcClient _ipcClient;
    private readonly object _lock = new();
    private Process? _engineProcess;
    private int _restartCount;
    private bool _intentionalShutdown;
    private bool _disposed;

    /// <summary>
    /// Raised when the engine process starts or stops.
    /// The bool parameter indicates whether the engine is now running.
    /// </summary>
    public event EventHandler<bool>? EngineStatusChanged;

    /// <summary>
    /// Whether the engine process is currently alive.
    /// </summary>
    public bool IsEngineRunning =>
        _engineProcess is not null && !_engineProcess.HasExited;

    public EngineProcessManager(EngineIpcClient ipcClient)
    {
        _ipcClient = ipcClient;
    }

    /// <summary>
    /// Launches the AudioBridgeEngine process from the same directory as the UI executable.
    /// Returns true if the process started successfully.
    /// </summary>
    public bool StartEngine()
    {
        lock (_lock)
        {
            if (IsEngineRunning)
                return true;

            string exeDir = AppContext.BaseDirectory;
            string enginePath = Path.Combine(exeDir, EngineFileName);

            if (!File.Exists(enginePath))
            {
                System.Diagnostics.Debug.WriteLine($"Engine not found at: {enginePath}");
                return false;
            }

            try
            {
                var startInfo = new ProcessStartInfo
                {
                    FileName = enginePath,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    WorkingDirectory = exeDir
                };

                _engineProcess = new Process { StartInfo = startInfo, EnableRaisingEvents = true };
                _engineProcess.Exited += OnEngineExited;
                _engineProcess.Start();

                _intentionalShutdown = false;
                EngineStatusChanged?.Invoke(this, true);
                return true;
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to start engine: {ex.Message}");
                return false;
            }
        }
    }

    /// <summary>
    /// Gracefully stops the engine by sending a shutdown command via IPC.
    /// Falls back to killing the process if it does not exit within the timeout.
    /// </summary>
    public async Task StopEngineAsync()
    {
        lock (_lock)
        {
            _intentionalShutdown = true;
        }

        if (_ipcClient.IsConnected)
        {
            try
            {
                await _ipcClient.ShutdownEngineAsync();
            }
            catch (Exception)
            {
                // Shutdown command may fail if pipe is broken; continue to force kill.
            }
        }

        lock (_lock)
        {
            if (_engineProcess is not null && !_engineProcess.HasExited)
            {
                try
                {
                    _engineProcess.WaitForExit(ShutdownWaitMs);
                }
                catch (Exception)
                {
                    // Ignore wait errors.
                }

                if (!_engineProcess.HasExited)
                {
                    try
                    {
                        _engineProcess.Kill(entireProcessTree: true);
                    }
                    catch (Exception)
                    {
                        // Process may have already exited.
                    }
                }
            }

            CleanupProcess();
        }

        EngineStatusChanged?.Invoke(this, false);
    }

    private void OnEngineExited(object? sender, EventArgs e)
    {
        bool shouldRestart = false;
        int attempt = 0;

        lock (_lock)
        {
            CleanupProcess();

            if (!_intentionalShutdown && _restartCount < MaxAutoRestarts)
            {
                _restartCount++;
                attempt = _restartCount;
                shouldRestart = true;
            }
        }

        EngineStatusChanged?.Invoke(this, false);

        if (shouldRestart)
        {
            System.Diagnostics.Debug.WriteLine(
                $"Engine crashed. Restart attempt {attempt}/{MaxAutoRestarts}.");

            // Brief delay before restart to avoid tight restart loops.
            Task.Delay(500).ContinueWith(_ => StartEngine());
        }
    }

    private void CleanupProcess()
    {
        if (_engineProcess is not null)
        {
            _engineProcess.Exited -= OnEngineExited;
            _engineProcess.Dispose();
            _engineProcess = null;
        }
    }

    public void Dispose()
    {
        lock (_lock)
        {
            if (_disposed)
                return;

            _disposed = true;
            _intentionalShutdown = true;
            CleanupProcess();
        }
    }
}
