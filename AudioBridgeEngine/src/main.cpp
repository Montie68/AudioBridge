// main.cpp - AudioBridge Engine entry point.
//
// This is a standalone console application that captures system audio via
// WASAPI loopback and routes it to multiple output devices simultaneously.
// It communicates with the C# UI process over a named pipe (JSON protocol).

#include "common.h"
#include "device_manager.h"
#include "audio_engine.h"
#include "ipc_server.h"

#include <atomic>
#include <cstdio>

#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------------------
// Global shutdown flag
// ---------------------------------------------------------------------------

static std::atomic<bool> g_shutdown_requested{false};

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        LOG_INFO("Shutdown signal received (ctrl=%lu).", ctrl_type);
        g_shutdown_requested.store(true);
        return TRUE;
    default:
        return FALSE;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    // Initialize COM on the main thread.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LOG_ERROR("CoInitializeEx failed: 0x%08lX",
                  static_cast<unsigned long>(hr));
        return 1;
    }

    // Set up console control handler for graceful shutdown.
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // ---- Create components ----
    DeviceManager device_mgr;
    if (!device_mgr.Initialize()) {
        LOG_ERROR("DeviceManager::Initialize failed.");
        CoUninitialize();
        return 1;
    }

    AudioEngine engine(&device_mgr);

    // When the default audio device changes:
    // 1. Check/remove the new default from render targets (feedback prevention)
    //    and auto-bridge the old default.
    // 2. Restart capture to switch loopback to the new default.
    // Both actions are done in a single callback to guarantee ordering.
    device_mgr.SetDefaultDeviceChangedCallback([&engine]() {
        engine.CheckAndRemoveDefaultDevice();
        if (engine.IsRunning()) {
            engine.RequestCaptureRestart();
        }
    });

    IpcServer   ipc(&engine, &device_mgr);

    ipc.Start();

    std::printf("AudioBridge Engine running. Waiting for commands...\n");
    std::fflush(stdout);

    // ---- Main loop: wait for shutdown ----
    while (!g_shutdown_requested.load() && !ipc.IsShutdownRequested()) {
        Sleep(200);
    }

    // ---- Cleanup in reverse order ----
    LOG_INFO("Shutting down...");
    ipc.Stop();
    engine.Stop();
    device_mgr.Shutdown();

    CoUninitialize();
    LOG_INFO("AudioBridge Engine exited cleanly.");
    return 0;
}
