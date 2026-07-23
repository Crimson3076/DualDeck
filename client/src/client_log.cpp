#include "client_log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

namespace melonds_remote::client {

namespace {
std::mutex g_logMutex;
std::FILE* g_logFile = nullptr;
std::function<void(const std::string&)> g_forwardSink;
} // namespace

void initClientLog() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return;

    std::string dir = std::string(home) + "/.config/dualdeck-client";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return;

    std::lock_guard<std::mutex> lock(g_logMutex);
    // "w", not "a": each run gets its own fresh log rather than an
    // unbounded, ever-growing history file nobody would trim on their
    // own -- matches this client already never persisting more than one
    // run's worth of anything else diagnostic.
    g_logFile = std::fopen((dir + "/client.log").c_str(), "w");
}

void logLine(const char* fmt, ...) {
    // Formatted once into a fixed buffer rather than calling vfprintf
    // per destination -- also what makes forwarding to the host
    // possible at all, since that needs the fully-rendered line as a
    // single std::string, not a (fmt, va_list) pair a receiver could
    // reformat differently. A line longer than the buffer is simply
    // truncated (vsnprintf never overflows it) -- acceptable for a
    // diagnostic log line, unlike wire-protocol data.
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int written = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    size_t len = written < 0 ? 0 : std::min(static_cast<size_t>(written), sizeof(buf) - 1);

    std::function<void(const std::string&)> sinkCopy;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        std::fwrite(buf, 1, len, stderr);
        if (g_logFile) {
            std::fwrite(buf, 1, len, g_logFile);
            // Flushed per line rather than left to libc's default
            // buffering -- this log's whole purpose is surviving a
            // crash or a killed Gaming-Mode session so its last lines
            // can actually be read afterward; none of this client's log
            // call sites are hot-path per-frame code, so the extra
            // syscall per line is not a real cost.
            std::fflush(g_logFile);
        }
        // Copied out and invoked outside the lock: the sink (indirectly
        // NetClient::sendClientLog()) takes its own separate lock, and
        // calling it while still holding g_logMutex would be an
        // unnecessary lock-ordering risk for no benefit.
        sinkCopy = g_forwardSink;
    }
    if (sinkCopy) {
        sinkCopy(std::string(buf, len));
    }
}

void setLogForwardSink(std::function<void(const std::string&)> sink) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_forwardSink = std::move(sink);
}

} // namespace melonds_remote::client
