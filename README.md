# AudioBridge

Play audio through multiple output devices at the same time on Windows. Headphones and speakers, two pairs of Bluetooth headphones, whatever combination you want.

Windows only lets you pick one output device at a time. AudioBridge works around this by running a lightweight C++ audio engine that captures the system output and mirrors it to every device you select.

## How it works

The app has two parts:

- **AudioBridgeEngine** (C++) handles the actual audio routing. It captures the default output stream via WASAPI and writes it to additional render devices. Runs as a background process.
- **AudioBridgeUI** (C#/WPF) sits in the system tray and lets you pick which devices to bridge. Communicates with the engine over a named pipe using JSON messages.

When you check a device in the UI, it tells the engine to add that device as a render target. Uncheck it and the engine drops it. Volume per device is adjustable independently.

## Building

You need Visual Studio 2022 with the C++ and .NET 8 workloads installed.

```
MSBuild AudioBridge.sln -p:Configuration=Release -p:Platform=x64
```

Output goes to `build/Release/`. Both `AudioBridgeEngine.exe` and `AudioBridgeUI.exe` (plus dependencies) land in the same folder.

## Running

Launch `AudioBridgeUI.exe`. It starts the engine automatically and puts an icon in the system tray. Click the tray icon to open the device list.

The app starts with Windows by default (registry run key in `HKCU\...\Run`). You can toggle this off in the UI.

Settings are saved to `%APPDATA%\AudioBridge\settings.json`. Bridged devices are restored on next launch.

## License

Apache 2.0. See [LICENSE](LICENSE).
