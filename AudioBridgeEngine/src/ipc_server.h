#pragma once
// ipc_server.h - Named-pipe IPC server for communication with the C# UI.

#include "common.h"
#include "audio_engine.h"
#include "device_manager.h"

#include <atomic>
#include <thread>

class IpcServer {
public:
    IpcServer(AudioEngine* engine, DeviceManager* device_mgr);
    ~IpcServer();

    IpcServer(const IpcServer&)            = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    void Start();
    void Stop();

    // Returns true when the remote client has sent the "shutdown" command.
    bool IsShutdownRequested() const;

private:
    void PipeThreadProc();
    std::string ProcessCommand(const std::string& json_str);

    AudioEngine*     engine_;
    DeviceManager*   device_mgr_;

    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::thread       pipe_thread_;

    // Handle to the most recently created pipe instance so Stop() can break
    // a blocking ConnectNamedPipe.  Atomic to avoid a data race between the
    // pipe thread and Stop().
    std::atomic<HANDLE> current_pipe_{INVALID_HANDLE_VALUE};
};
