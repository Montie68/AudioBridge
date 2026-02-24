using System.Text.Json.Serialization;

namespace AudioBridgeUI.Models;

/// <summary>
/// Represents a physical audio output device reported by the engine.
/// </summary>
public class AudioDevice
{
    /// <summary>
    /// Unique system identifier for the audio device.
    /// </summary>
    [JsonPropertyName("id")]
    public string DeviceId { get; set; } = string.Empty;

    /// <summary>
    /// Human-readable device name (e.g., "Speakers (Realtek Audio)").
    /// </summary>
    [JsonPropertyName("name")]
    public string DeviceName { get; set; } = string.Empty;

    /// <summary>
    /// Whether the device is physically present and available.
    /// </summary>
    [JsonPropertyName("active")]
    public bool IsActive { get; set; }

    /// <summary>
    /// Whether this is the current default audio output device (the source AudioBridge captures from).
    /// </summary>
    [JsonPropertyName("is_default")]
    public bool IsDefault { get; set; }

    /// <summary>
    /// Whether the device is currently added to the audio bridge.
    /// </summary>
    public bool IsBridged { get; set; }

    /// <summary>
    /// Per-device volume level, ranging from 0.0 (mute) to 1.0 (full).
    /// Deserialized from the engine's get_devices response (Windows system volume).
    /// </summary>
    [JsonPropertyName("volume")]
    public float Volume { get; set; } = 1.0f;
}
