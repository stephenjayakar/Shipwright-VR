#pragma once
// RTXDiagLog.h — Diagnostic logging for RTX initialization and runtime debugging.
//
// Usage: RTX_DIAG("format string %d %s", arg1, arg2);
//
// All messages are prefixed with "[RTX] " and output to:
//   - OutputDebugString() on Windows (visible in VS Output / DebugView)
//   - printf() to stdout (visible if running with a console)
//   - A dedicated log file "logs/rtx_diag.log" (always captured, even for GUI apps)
//
// This header is safe to include from any RTX .cpp file. It requires <cstdio>
// and <Windows.h> (on Win32). No spdlog dependency — these are low-level diagnostics
// that work even before spdlog is initialized.

#ifndef RTX_DIAG_LOG_H
#define RTX_DIAG_LOG_H

#include <cstdio>
#include <cstdarg>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

// Get or create the RTX diagnostic log file handle. Lazily opens the file on first call.
inline FILE* RTX_DiagGetLogFile() {
    static FILE* s_logFile = nullptr;
    static bool s_attempted = false;
    if (!s_attempted) {
        s_attempted = true;
#ifdef _WIN32
        // Ensure logs/ directory exists
        CreateDirectoryA("logs", nullptr);
#endif
        s_logFile = fopen("logs/rtx_diag.log", "w");
    }
    return s_logFile;
}

// RTX_DIAG: printf-style diagnostic log that goes to OutputDebugString, stdout, and a log file.
// The "[RTX] " prefix is prepended automatically.
inline void RTX_DiagLog(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);

    // Prefix
    int prefixLen = snprintf(buf, sizeof(buf), "[RTX] ");
    int bodyLen = vsnprintf(buf + prefixLen, sizeof(buf) - prefixLen, fmt, args);
    va_end(args);

    // Ensure newline
    int totalLen = prefixLen + (bodyLen > 0 ? bodyLen : 0);
    if (totalLen > 0 && totalLen < (int)(sizeof(buf) - 2) && buf[totalLen - 1] != '\n') {
        buf[totalLen] = '\n';
        buf[totalLen + 1] = '\0';
    }

    // Output to console (stdout)
    printf("%s", buf);
    fflush(stdout);

    // Output to dedicated log file (always captured, even for GUI subsystem apps)
    FILE* logFile = RTX_DiagGetLogFile();
    if (logFile) {
        fprintf(logFile, "%s", buf);
        fflush(logFile);
    }

#ifdef _WIN32
    // Output to debug output (visible in Visual Studio Output window / DebugView)
    OutputDebugStringA(buf);
#endif
}

#define RTX_DIAG(...) RTX_DiagLog(__VA_ARGS__)

#endif // RTX_DIAG_LOG_H
