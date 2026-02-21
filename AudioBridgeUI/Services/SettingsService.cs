using System.IO;
using System.Text.Json;
using AudioBridgeUI.Models;

namespace AudioBridgeUI.Services;

/// <summary>
/// Persists and loads <see cref="BridgeSettings"/> as JSON
/// in the user's %APPDATA%\AudioBridge directory.
/// </summary>
public sealed class SettingsService
{
    private static readonly string _settingsDir =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "AudioBridge");

    private static readonly string _settingsPath =
        Path.Combine(_settingsDir, "settings.json");

    private static readonly JsonSerializerOptions _jsonOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true
    };

    /// <summary>
    /// Loads settings from disk. Returns a default instance if the file does not exist
    /// or cannot be deserialized.
    /// </summary>
    public BridgeSettings LoadSettings()
    {
        try
        {
            if (!File.Exists(_settingsPath))
                return new BridgeSettings();

            string json = File.ReadAllText(_settingsPath);
            return JsonSerializer.Deserialize<BridgeSettings>(json, _jsonOptions)
                   ?? new BridgeSettings();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Failed to load settings: {ex.Message}");
            return new BridgeSettings();
        }
    }

    /// <summary>
    /// Saves the given settings to disk, creating the directory if necessary.
    /// </summary>
    public void SaveSettings(BridgeSettings settings)
    {
        try
        {
            Directory.CreateDirectory(_settingsDir);
            string json = JsonSerializer.Serialize(settings, _jsonOptions);
            File.WriteAllText(_settingsPath, json);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Failed to save settings: {ex.Message}");
        }
    }
}
