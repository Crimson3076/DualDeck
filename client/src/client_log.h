#pragma once

#include <functional>
#include <string>

namespace melonds_remote::client {

// Opens the persistent log file (~/.config/dualdeck-client/client.log,
// truncated fresh at the start of every run) that logLine() appends to
// alongside stderr. Steam Big Picture/Gaming Mode has no visible
// terminal, so without this the only diagnostic trail a user or a
// developer chasing a bug report has ever had was whatever stderr
// happened to be connected to (usually nothing). Call once, early in
// main(), before any other client code logs anything. Safe to skip --
// logLine() silently falls back to stderr-only -- if $HOME isn't set or
// the directory can't be created.
void initClientLog();

// Drop-in replacement for std::fprintf(stderr, fmt, ...), used
// throughout this client in place of that call: writes the same
// formatted line to stderr (unchanged behavior) and, if initClientLog()
// succeeded, appends it to the persistent log file too. Thread-safe --
// net_client.cpp's background receive threads and main.cpp's main
// thread all call this concurrently.
void logLine(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// Registers a sink that every subsequent logLine() call also forwards
// its fully-formatted line to, in addition to stderr/the log file --
// wired up in main.cpp to NetClient::sendClientLog() once a NetClient
// exists, so a host operator/developer can see what a specific client
// is doing (host debugging and app development) without needing
// physical access to the Deck or a visible terminal on it. Pass an
// empty std::function to stop forwarding again (main.cpp does this via
// a scope guard tied to the NetClient's own lifetime, since a sink
// capturing a reference to a destroyed NetClient would dangle).
// Forwarding a line is always best-effort and non-blocking on the
// sink's own terms (see NetClient::sendClientLog()) -- logLine() itself
// never blocks or drops stderr/file output because of it.
void setLogForwardSink(std::function<void(const std::string&)> sink);

} // namespace melonds_remote::client
