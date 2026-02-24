namespace AudioBridgeUI.Models;

/// <summary>
/// Persisted application settings for the AudioBridge UI.
/// </summary>
public class BridgeSettings
{
    /// <summary>
    /// Devices that the user has added to the bridge, persisted across sessions.
    /// </summary>
    public List<BridgedDeviceInfo> BridgedDevices { get; set; } = new();

    /// <summary>
    /// Whether the application should launch at Windows startup.
    /// </summary>
    public bool StartWithWindows { get; set; } = true;

    /// <summary>
    /// Whether the audio bridge should start automatically when the application launches.
    /// </summary>
    public bool AutoStartBridge { get; set; }

    /// <summary>
    /// Whether devices should be automatically re-added when they become available.
    /// </summary>
    public bool AutoReconnect { get; set; } = true;
}

/// <summary>
/// Lightweight record capturing a bridged device's identity and volume for persistence.
/// </summary>
public record BridgedDeviceInfo
{
    /// <summary>
    /// Unique system identifier for the audio device.
    /// </summary>
    public string DeviceId { get; init; } = string.Empty;

    /// <summary>
    /// Human-readable device name at the time it was bridged.
    /// </summary>
    public string DeviceName { get; init; } = string.Empty;

    /// <summary>
    /// Per-device volume level, ranging from 0.0 to 1.0.
    /// </summary>
    public float Volume { get; init; } = 1.0f;
}
