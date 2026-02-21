#pragma once
// common.h - Shared types, constants, and logging macros for AudioBridge Engine.

#include <cstdio>
#include <windows.h>

// ---- Named pipe for IPC with the UI process ----
constexpr const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\AudioBridge";

// ---- Default ring-buffer size (100 ms at 48 kHz) ----
constexpr unsigned int BUFFER_SIZE_FRAMES = 4800;

// ---- Logging helpers ----
// LOG_INFO / LOG_ERROR write to both OutputDebugString and stdout/stderr so
// that messages are visible in the Visual Studio Output window and in the
// console when the engine runs standalone.

#define AB_LOG(prefix, stream, fmt, ...)                                      \
    do {                                                                       \
        char _ab_buf[1024];                                                    \
        std::snprintf(_ab_buf, sizeof(_ab_buf), prefix fmt "\n",              \
                      ##__VA_ARGS__);                                          \
        OutputDebugStringA(_ab_buf);                                           \
        std::fputs(_ab_buf, (stream));                                         \
    } while (0)

#define LOG_INFO(fmt, ...)  AB_LOG("[AudioBridge] ",       stdout, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) AB_LOG("[AudioBridge ERROR] ", stderr, fmt, ##__VA_ARGS__)

// ---- HRESULT check helper ----
// Use as:  HR_CHECK(hr, "SomeCall"); which logs and returns false on failure.
#define HR_CHECK(hr, msg)                                                     \
    do {                                                                       \
        if (FAILED(hr)) {                                                      \
            LOG_ERROR("%s failed: 0x%08lX", (msg), static_cast<unsigned long>(hr)); \
            return false;                                                      \
        }                                                                      \
    } while (0)
