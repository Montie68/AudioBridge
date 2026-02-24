// ipc_server.cpp - Named-pipe IPC server (newline-delimited JSON protocol).

#include "ipc_server.h"
#include "common.h"
#include "json.hpp"

#include <sddl.h>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

using json = nlohmann::json;

// Maximum size (in bytes) for a single newline-delimited JSON message.
// If a client sends more data than this without a newline, the connection
// is dropped to prevent memory-exhaustion denial-of-service attacks.
static constexpr size_t MAX_MESSAGE_SIZE = 65536; // 64 KB

// ---------------------------------------------------------------------------
// Wide string <-> UTF-8 conversion helpers
// ---------------------------------------------------------------------------

static std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return {};

    // Guard against integer overflow when casting size_t to int.
    if (ws.size() > static_cast<size_t>(INT_MAX)) {
        LOG_ERROR("WideToUtf8: input string too large (%zu chars).", ws.size());
        return {};
    }

    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(),
                                  static_cast<int>(ws.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        LOG_ERROR("WideCharToMultiByte (size query) failed: %lu", GetLastError());
        return {};
    }

    std::string s(static_cast<size_t>(len), '\0');
    int written = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(),
                                      static_cast<int>(ws.size()),
                                      s.data(), len, nullptr, nullptr);
    if (written <= 0) {
        LOG_ERROR("WideCharToMultiByte (conversion) failed: %lu", GetLastError());
        return {};
    }

    return s;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};

    // Guard against integer overflow when casting size_t to int.
    if (s.size() > static_cast<size_t>(INT_MAX)) {
        LOG_ERROR("Utf8ToWide: input string too large (%zu bytes).", s.size());
        return {};
    }

    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()),
                                  nullptr, 0);
    if (len <= 0) {
        LOG_ERROR("MultiByteToWideChar (size query) failed: %lu", GetLastError());
        return {};
    }

    std::wstring ws(static_cast<size_t>(len), L'\0');
    int written = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()),
                                      ws.data(), len);
    if (written <= 0) {
        LOG_ERROR("MultiByteToWideChar (conversion) failed: %lu", GetLastError());
        return {};
    }

    return ws;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

IpcServer::IpcServer(AudioEngine* engine, DeviceManager* device_mgr)
    : engine_(engine), device_mgr_(device_mgr) {}

IpcServer::~IpcServer() {
    Stop();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void IpcServer::Start() {
    if (running_.load()) return;
    running_.store(true);
    pipe_thread_ = std::thread(&IpcServer::PipeThreadProc, this);
    LOG_INFO("IPC server started.");
}

void IpcServer::Stop() {
    if (!running_.load()) return;
    running_.store(false);

    // If the pipe thread is blocked on ConnectNamedPipe, closing the handle
    // will unblock it with ERROR_OPERATION_ABORTED.  Use atomic exchange to
    // avoid racing with the pipe thread.
    HANDLE h = current_pipe_.exchange(INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        // Disconnect any connected client, then close.
        DisconnectNamedPipe(h);
        CloseHandle(h);
    }

    if (pipe_thread_.joinable()) pipe_thread_.join();
    LOG_INFO("IPC server stopped.");
}

bool IpcServer::IsShutdownRequested() const {
    return shutdown_requested_.load();
}

// ---------------------------------------------------------------------------
// Pipe security helper
// ---------------------------------------------------------------------------

// Creates a SECURITY_ATTRIBUTES structure that restricts pipe access to the
// current user's SID only.  This prevents other users on the machine from
// connecting to (or squatting on) the AudioBridge pipe.
//
// The caller must call LocalFree(sa.lpSecurityDescriptor) when done.
// Returns true on success; on failure the SA is zero-initialized and the pipe
// will fall back to a less restrictive NULL descriptor (logged as an error).
static bool CreateCurrentUserPipeSA(SECURITY_ATTRIBUTES& sa) {
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;

    // Build a SDDL string granting full access to the current user (owner)
    // and the SYSTEM account.  "O:CU" = owner is current user,
    // "D:(A;;GA;;;CU)(A;;GA;;;SY)" = DACL granting Generic All to current
    // user and SYSTEM.
    const wchar_t* sddl = L"D:(A;;GA;;;CU)(A;;GA;;;SY)";

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr)) {
        LOG_ERROR("ConvertStringSecurityDescriptorToSecurityDescriptor "
                  "failed: %lu. Pipe will use default security.",
                  GetLastError());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Pipe thread
// ---------------------------------------------------------------------------

void IpcServer::PipeThreadProc() {
    while (running_.load()) {
        // Build a security descriptor restricting access to the current user.
        SECURITY_ATTRIBUTES sa;
        bool has_sa = CreateCurrentUserPipeSA(sa);

        // Create a new pipe instance for each client connection.
        // PIPE_REJECT_REMOTE_CLIENTS prevents connections from remote machines.
        HANDLE pipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                | PIPE_REJECT_REMOTE_CLIENTS,
            1,      // max instances
            4096,   // output buffer
            4096,   // input buffer
            0,      // default timeout
            has_sa ? &sa : nullptr
        );

        // Free the security descriptor now that the pipe is created.
        if (has_sa && sa.lpSecurityDescriptor) {
            LocalFree(sa.lpSecurityDescriptor);
        }

        if (pipe == INVALID_HANDLE_VALUE) {
            LOG_ERROR("CreateNamedPipe failed: %lu", GetLastError());
            Sleep(1000); // Back off before retrying.
            continue;
        }

        current_pipe_.store(pipe);

        // Block until a client connects (or until Stop() closes the handle).
        BOOL connected = ConnectNamedPipe(pipe, nullptr);
        DWORD err = GetLastError();
        if (!connected && err != ERROR_PIPE_CONNECTED) {
            // ERROR_OPERATION_ABORTED or ERROR_NO_DATA means Stop() was called.
            // Use compare_exchange to avoid closing a handle that Stop() already closed.
            HANDLE expected = pipe;
            if (current_pipe_.compare_exchange_strong(expected, INVALID_HANDLE_VALUE)) {
                CloseHandle(pipe);
            }
            if (!running_.load()) break;
            continue;
        }

        LOG_INFO("IPC client connected.");

        // ---- Read loop: accumulate bytes until we see a newline ----
        std::string line_buf;
        char read_chunk[4096];

        while (running_.load()) {
            DWORD bytes_read = 0;
            BOOL ok = ReadFile(pipe, read_chunk, sizeof(read_chunk),
                               &bytes_read, nullptr);
            if (!ok || bytes_read == 0) {
                // Client disconnected or pipe error.
                break;
            }

            line_buf.append(read_chunk, bytes_read);

            // Guard against memory exhaustion: if the accumulated buffer
            // exceeds our limit without a complete message, a misbehaving
            // or malicious client is sending oversized data.  Disconnect.
            if (line_buf.size() > MAX_MESSAGE_SIZE && line_buf.find('\n') == std::string::npos) {
                LOG_ERROR("IPC client exceeded max message size (%zu bytes) "
                          "without a newline. Disconnecting.",
                          MAX_MESSAGE_SIZE);
                break;
            }

            // Process all complete newline-delimited messages.
            size_t pos;
            while ((pos = line_buf.find('\n')) != std::string::npos) {
                std::string msg = line_buf.substr(0, pos);
                line_buf.erase(0, pos + 1);

                // Trim trailing \r if present (Windows line endings).
                if (!msg.empty() && msg.back() == '\r') msg.pop_back();
                if (msg.empty()) continue;

                std::string response = ProcessCommand(msg);
                response.push_back('\n'); // Newline-delimited response.

                DWORD bytes_written = 0;
                WriteFile(pipe, response.c_str(),
                          static_cast<DWORD>(response.size()),
                          &bytes_written, nullptr);
                FlushFileBuffers(pipe);

                // If the command was "shutdown", break out after sending response.
                if (shutdown_requested_.load()) break;
            }

            if (shutdown_requested_.load()) break;
        }

        DisconnectNamedPipe(pipe);
        // Use compare_exchange to avoid double-close if Stop() already closed it.
        HANDLE expected_end = pipe;
        if (current_pipe_.compare_exchange_strong(expected_end, INVALID_HANDLE_VALUE)) {
            CloseHandle(pipe);
        }

        LOG_INFO("IPC client disconnected.");

        if (shutdown_requested_.load()) break;
    }
}

// ---------------------------------------------------------------------------
// Command dispatcher
// ---------------------------------------------------------------------------

std::string IpcServer::ProcessCommand(const std::string& json_str) {
    json response;
    try {
        json cmd = json::parse(json_str);
        std::string action = cmd.value("command", "");

        if (action == "get_devices") {
            auto devices = device_mgr_->GetOutputDevices();
            std::wstring default_id = device_mgr_->GetDefaultOutputDeviceId();
            json arr = json::array();
            for (auto& d : devices) {
                arr.push_back({
                    {"id",         WideToUtf8(d.id)},
                    {"name",       WideToUtf8(d.name)},
                    {"active",     d.active},
                    {"is_default", d.id == default_id},
                    {"volume",     device_mgr_->GetDeviceVolume(d.id)}
                });
            }
            response = {{"success", true}, {"devices", arr}};

        } else if (action == "add_device") {
            std::wstring id   = Utf8ToWide(cmd.value("device_id", ""));
            std::wstring name = Utf8ToWide(cmd.value("device_name", ""));
            if (id.empty()) {
                response = {{"success", false}, {"error", "device_id required"}};
            } else {
                bool ok = engine_->AddRenderDevice(id);
                if (ok) device_mgr_->AddWantedDevice(id, name);
                response = {{"success", ok}};
            }

        } else if (action == "remove_device") {
            std::wstring id = Utf8ToWide(cmd.value("device_id", ""));
            if (id.empty()) {
                response = {{"success", false}, {"error", "device_id required"}};
            } else {
                engine_->RemoveRenderDevice(id);
                device_mgr_->RemoveWantedDevice(id);
                response = {{"success", true}};
            }

        } else if (action == "set_volume") {
            std::wstring id = Utf8ToWide(cmd.value("device_id", ""));
            float vol = cmd.value("volume", 1.0f);
            if (id.empty()) {
                response = {{"success", false}, {"error", "device_id required"}};
            } else {
                bool ok = device_mgr_->SetDeviceVolume(id, vol);
                response = {{"success", ok}};
            }

        } else if (action == "start") {
            bool ok = engine_->Start();
            response = {{"success", ok}};

        } else if (action == "stop") {
            engine_->Stop();
            response = {{"success", true}};

        } else if (action == "get_status") {
            auto ids = engine_->GetActiveDeviceIds();
            json id_arr = json::array();
            for (auto& wid : ids) id_arr.push_back(WideToUtf8(wid));
            response = {
                {"success",    true},
                {"running",    engine_->IsRunning()},
                {"active_devices", id_arr}
            };

        } else if (action == "shutdown") {
            engine_->Stop();
            shutdown_requested_.store(true);
            response = {{"success", true}};

        } else {
            response = {{"success", false},
                        {"error", "unknown command: " + action}};
        }

    } catch (const json::exception& e) {
        response = {{"success", false},
                    {"error", std::string("JSON parse error: ") + e.what()}};
    }

    return response.dump();
}
