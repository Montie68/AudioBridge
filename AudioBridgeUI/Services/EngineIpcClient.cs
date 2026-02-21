using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using AudioBridgeUI.Models;

namespace AudioBridgeUI.Services;

/// <summary>
/// Communicates with the AudioBridgeEngine C++ process over a named pipe
/// using newline-delimited JSON messages.
/// </summary>
public sealed class EngineIpcClient : IDisposable
{
    private const string PipeName = "AudioBridge";
    private const int ConnectTimeoutMs = 3000;

    private NamedPipeClientStream? _pipe;
    private StreamReader? _reader;
    private StreamWriter? _writer;
    private readonly SemaphoreSlim _sendLock = new(1, 1);
    private bool _disposed;

    private static readonly JsonSerializerOptions _jsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        PropertyNameCaseInsensitive = true
    };

    /// <summary>
    /// Whether the pipe is currently connected to the engine.
    /// </summary>
    public bool IsConnected => _pipe?.IsConnected == true;

    /// <summary>
    /// Attempts to connect to the engine's named pipe.
    /// Returns true on success, false if the engine is not available.
    /// </summary>
    public async Task<bool> ConnectAsync(CancellationToken cancellationToken = default)
    {
        try
        {
            Disconnect();

            _pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            await _pipe.ConnectAsync(ConnectTimeoutMs, cancellationToken);

            _reader = new StreamReader(_pipe, Encoding.UTF8, leaveOpen: true);
            _writer = new StreamWriter(_pipe, Encoding.UTF8, leaveOpen: true) { AutoFlush = true };

            return true;
        }
        catch (Exception) when (!cancellationToken.IsCancellationRequested)
        {
            Disconnect();
            return false;
        }
    }

    /// <summary>
    /// Sends a JSON command to the engine and reads the JSON response.
    /// Returns null if the communication fails.
    /// </summary>
    public async Task<JsonDocument?> SendCommandAsync(object command, CancellationToken cancellationToken = default)
    {
        if (!IsConnected)
            return null;

        await _sendLock.WaitAsync(cancellationToken);
        try
        {
            string json = JsonSerializer.Serialize(command, _jsonOptions);
            await _writer!.WriteLineAsync(json.AsMemory(), cancellationToken);

            string? responseLine = await _reader!.ReadLineAsync(cancellationToken);
            if (responseLine is null)
                return null;

            return JsonDocument.Parse(responseLine);
        }
        catch (Exception)
        {
            return null;
        }
        finally
        {
            _sendLock.Release();
        }
    }

    /// <summary>
    /// Requests the list of audio output devices from the engine.
    /// </summary>
    public async Task<List<AudioDevice>> GetDevicesAsync(CancellationToken cancellationToken = default)
    {
        var command = new { Command = "get_devices" };
        using var response = await SendCommandAsync(command, cancellationToken);

        if (response is null)
            return new List<AudioDevice>();

        if (response.RootElement.TryGetProperty("devices", out var devicesElement))
        {
            return JsonSerializer.Deserialize<List<AudioDevice>>(devicesElement.GetRawText(), _jsonOptions)
                   ?? new List<AudioDevice>();
        }

        return new List<AudioDevice>();
    }

    /// <summary>
    /// Adds a device to the audio bridge by its system ID.
    /// </summary>
    public async Task<bool> AddDeviceAsync(string deviceId, CancellationToken cancellationToken = default)
    {
        var command = new { Command = "add_device", DeviceId = deviceId };
        using var response = await SendCommandAsync(command, cancellationToken);
        return IsSuccessResponse(response);
    }

    /// <summary>
    /// Removes a device from the audio bridge by its system ID.
    /// </summary>
    public async Task<bool> RemoveDeviceAsync(string deviceId, CancellationToken cancellationToken = default)
    {
        var command = new { Command = "remove_device", DeviceId = deviceId };
        using var response = await SendCommandAsync(command, cancellationToken);
        return IsSuccessResponse(response);
    }

    /// <summary>
    /// Sets the per-device volume (0.0 to 1.0) for a bridged device.
    /// </summary>
    public async Task<bool> SetVolumeAsync(string deviceId, float volume, CancellationToken cancellationToken = default)
    {
        var command = new { Command = "set_volume", DeviceId = deviceId, Volume = volume };
        using var response = await SendCommandAsync(command, cancellationToken);
        return IsSuccessResponse(response);
    }

    /// <summary>
    /// Starts the audio bridge (begins routing audio to bridged devices).
    /// </summary>
    public async Task<bool> StartBridgeAsync(CancellationToken cancellationToken = default)
    {
        var command = new { Command = "start" };
        using var response = await SendCommandAsync(command, cancellationToken);
        return IsSuccessResponse(response);
    }

    /// <summary>
    /// Stops the audio bridge (stops routing audio).
    /// </summary>
    public async Task<bool> StopBridgeAsync(CancellationToken cancellationToken = default)
    {
        var command = new { Command = "stop" };
        using var response = await SendCommandAsync(command, cancellationToken);
        return IsSuccessResponse(response);
    }

    /// <summary>
    /// Queries the engine for current bridge status.
    /// Returns a tuple of (isRunning, activeDeviceIds) or null on failure.
    /// </summary>
    public async Task<(bool IsRunning, List<string> ActiveDeviceIds)?> GetStatusAsync(
        CancellationToken cancellationToken = default)
    {
        var command = new { Command = "get_status" };
        using var response = await SendCommandAsync(command, cancellationToken);

        if (response is null)
            return null;

        bool running = false;
        var activeIds = new List<string>();

        if (response.RootElement.TryGetProperty("running", out var runningElement))
            running = runningElement.GetBoolean();

        if (response.RootElement.TryGetProperty("active_devices", out var devicesElement))
        {
            foreach (var item in devicesElement.EnumerateArray())
            {
                var id = item.GetString();
                if (id is not null)
                    activeIds.Add(id);
            }
        }

        return (running, activeIds);
    }

    /// <summary>
    /// Sends a shutdown command to gracefully terminate the engine process.
    /// </summary>
    public async Task ShutdownEngineAsync(CancellationToken cancellationToken = default)
    {
        var command = new { Command = "shutdown" };
        await SendCommandAsync(command, cancellationToken);
        Disconnect();
    }

    /// <summary>
    /// Closes the pipe connection and releases stream resources.
    /// </summary>
    public void Disconnect()
    {
        _reader?.Dispose();
        _reader = null;

        _writer?.Dispose();
        _writer = null;

        _pipe?.Dispose();
        _pipe = null;
    }

    public void Dispose()
    {
        if (_disposed)
            return;

        _disposed = true;
        Disconnect();
        _sendLock.Dispose();
    }

    private static bool IsSuccessResponse(JsonDocument? response)
    {
        if (response is null)
            return false;

        if (response.RootElement.TryGetProperty("success", out var successElement))
            return successElement.GetBoolean();

        return false;
    }
}
