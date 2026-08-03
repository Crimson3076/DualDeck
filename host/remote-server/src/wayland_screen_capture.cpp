#include "host/wayland_screen_capture.h"

#include <cstdio>

#if defined(DUALDECK_HAVE_WAYLAND_SCREEN_CAPTURE)
#include <fcntl.h>
#include <unistd.h>

#include <dbus/dbus.h>
#include <pipewire/pipewire.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#endif

namespace melonds_remote::host {

#if defined(DUALDECK_HAVE_WAYLAND_SCREEN_CAPTURE)

namespace {

// How long the constructor waits for each portal Request/Response round
// trip (CreateSession/SelectSources/Start) before giving up -- Start in
// particular is the one showing the interactive permission prompt (see
// this class's header comment), so this has to be generous enough for a
// human to actually see and click it, not just network/IPC latency.
constexpr int kPortalRequestTimeoutMs = 30000;
// Separate, shorter bound on how long the constructor waits for
// PipeWire to actually negotiate a format and deliver a first frame
// once OpenPipeWireRemote hands back a connected stream -- this is pure
// local IPC/negotiation, not waiting on a human.
constexpr int kPipewireFirstFrameTimeoutMs = 5000;

std::string randomToken() {
    // Real requirement, not a style choice: the portal spec requires
    // handle_token/session_handle_token to each be unique per call and
    // usable as one path segment of a D-Bus object path -- alphanumeric
    // only, no dots/slashes.
    static std::atomic<uint64_t> counter{0};
    return "dualdeck_" + std::to_string(::getpid()) + "_" + std::to_string(counter++);
}

void appendStringOption(DBusMessageIter& arrayIter, const char* key, const std::string& value) {
    DBusMessageIter dictIter, variantIter;
    dbus_message_iter_open_container(&arrayIter, DBUS_TYPE_DICT_ENTRY, nullptr, &dictIter);
    dbus_message_iter_append_basic(&dictIter, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&dictIter, DBUS_TYPE_VARIANT, "s", &variantIter);
    const char* val = value.c_str();
    dbus_message_iter_append_basic(&variantIter, DBUS_TYPE_STRING, &val);
    dbus_message_iter_close_container(&dictIter, &variantIter);
    dbus_message_iter_close_container(&arrayIter, &dictIter);
}

void appendUint32Option(DBusMessageIter& arrayIter, const char* key, uint32_t value) {
    DBusMessageIter dictIter, variantIter;
    dbus_message_iter_open_container(&arrayIter, DBUS_TYPE_DICT_ENTRY, nullptr, &dictIter);
    dbus_message_iter_append_basic(&dictIter, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&dictIter, DBUS_TYPE_VARIANT, "u", &variantIter);
    dbus_message_iter_append_basic(&variantIter, DBUS_TYPE_UINT32, &value);
    dbus_message_iter_close_container(&dictIter, &variantIter);
    dbus_message_iter_close_container(&arrayIter, &dictIter);
}

void appendBoolOption(DBusMessageIter& arrayIter, const char* key, bool value) {
    DBusMessageIter dictIter, variantIter;
    dbus_message_iter_open_container(&arrayIter, DBUS_TYPE_DICT_ENTRY, nullptr, &dictIter);
    dbus_message_iter_append_basic(&dictIter, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&dictIter, DBUS_TYPE_VARIANT, "b", &variantIter);
    dbus_bool_t v = value ? TRUE : FALSE;
    dbus_message_iter_append_basic(&variantIter, DBUS_TYPE_BOOLEAN, &v);
    dbus_message_iter_close_container(&dictIter, &variantIter);
    dbus_message_iter_close_container(&arrayIter, &dictIter);
}

} // namespace

struct WaylandScreenCapture::Impl {
    DBusConnection* bus = nullptr;
    std::string sessionHandle;

    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook streamListener{};
    int pipewireFd = -1;

    std::mutex frameMutex;
    std::vector<uint8_t> latestFrame;
    uint16_t width = 0;
    uint16_t height = 0;
    // Set from the actually-negotiated SPA_PARAM_Format in
    // onStreamParamChanged() -- see onStreamProcess()'s own comment for
    // why this can't just be assumed to always be BGRx.
    spa_video_format format = SPA_VIDEO_FORMAT_UNKNOWN;
    bool haveFrame = false;
    bool ready = false;

    ~Impl() {
        if (loop) {
            pw_thread_loop_lock(loop);
            if (stream) pw_stream_destroy(stream);
            if (core) pw_core_disconnect(core);
            if (context) pw_context_destroy(context);
            pw_thread_loop_unlock(loop);
            pw_thread_loop_destroy(loop);
        }
        if (pipewireFd >= 0) ::close(pipewireFd);
        if (bus) {
            // Best-effort: tell the portal we're done with this
            // ScreenCast session so the compositor can drop its
            // permission-grant/UI state immediately, rather than only
            // noticing once the D-Bus peer disconnects. Fire-and-forget
            // (dbus_connection_send, not send_with_reply_and_block) --
            // we're tearing down and don't need to wait for a reply,
            // just flush() to make sure it actually reaches the bus
            // before the connection below gets closed.
            if (!sessionHandle.empty()) {
                DBusMessage* closeMsg = dbus_message_new_method_call(
                    "org.freedesktop.portal.Desktop", sessionHandle.c_str(),
                    "org.freedesktop.portal.Session", "Close");
                if (closeMsg) {
                    dbus_connection_send(bus, closeMsg, nullptr);
                    dbus_connection_flush(bus);
                    dbus_message_unref(closeMsg);
                }
            }
            // Real bug, 2026-08-03: dbus_bus_get_private() (used above
            // instead of the ordinary shared dbus_bus_get()) hands back
            // a *private* connection that libdbus's own documented
            // contract requires the owner to dbus_connection_close()
            // before the final unref -- dropping the last reference on
            // an unclosed private connection is treated as a fatal
            // application programming error and made libdbus abort the
            // whole process with an assertion. This was the actual
            // cause of the reported "D-Bus assertion" crash during Host
            // Control startup/cleanup; it reproduced on essentially any
            // real desktop with a reachable session bus (including
            // early-return/partial-failure constructor paths, since
            // `bus` is set as soon as dbus_bus_get_private() succeeds,
            // before any of the portal/PipeWire setup that can fail),
            // which is why it didn't show up in the unit tests (they
            // force dbus_bus_get_private() itself to fail via
            // ScopedUnsetEnv specifically to avoid popping a real
            // permission dialog).
            dbus_connection_close(bus);
            dbus_connection_unref(bus);
        }
    }

    // Calls a portal method that follows the Request/Response pattern
    // (CreateSession/SelectSources/Start all do -- see this class's own
    // header comment): sends `methodName` with `options` as the last
    // a{sv} argument (plus any positional args already appended to
    // `msg` before calling this), reads the synchronous reply for the
    // Request object path, then blocks dispatching the connection until
    // that exact Request's Response signal arrives or `timeoutMs`
    // elapses. On success, returns true and sets outResults to the
    // Response signal's own a{sv} results dict (borrowed from
    // *outResponseMsg, which the caller must unref once done reading
    // it). Returns false on any failure (D-Bus error reply, timeout, or
    // a non-zero Response code meaning the user cancelled/denied).
    bool callAndAwaitResponse(DBusMessage* msg, int timeoutMs, DBusMessage** outResponseMsg) {
        DBusError error;
        dbus_error_init(&error);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(bus, msg, timeoutMs, &error);
        dbus_message_unref(msg);
        if (!reply) {
            std::fprintf(stderr, "WaylandScreenCapture: portal call failed: %s\n",
                          error.message ? error.message : "(no message)");
            dbus_error_free(&error);
            return false;
        }
        const char* requestPath = nullptr;
        if (!dbus_message_get_args(reply, &error, DBUS_TYPE_OBJECT_PATH, &requestPath, DBUS_TYPE_INVALID)) {
            std::fprintf(stderr, "WaylandScreenCapture: malformed portal reply: %s\n",
                          error.message ? error.message : "(no message)");
            dbus_error_free(&error);
            dbus_message_unref(reply);
            return false;
        }
        std::string expectedPath = requestPath;
        dbus_message_unref(reply);

        // Blocking dispatch loop, bounded by timeoutMs -- appropriate
        // here because this whole sequence (CreateSession ->
        // SelectSources -> Start) is a one-time setup step, not
        // something on any per-frame hot path.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            dbus_connection_read_write_dispatch(bus, 100);
            DBusMessage* msg2 = dbus_connection_pop_message(bus);
            if (!msg2) continue;
            if (dbus_message_is_signal(msg2, "org.freedesktop.portal.Request", "Response") &&
                dbus_message_get_path(msg2) && expectedPath == dbus_message_get_path(msg2)) {
                DBusMessageIter iter;
                dbus_message_iter_init(msg2, &iter);
                uint32_t responseCode = 0;
                dbus_message_iter_get_basic(&iter, &responseCode);
                if (responseCode != 0) {
                    std::fprintf(stderr,
                                  "WaylandScreenCapture: portal request denied/cancelled (code %u) -- if this "
                                  "was the first time, this is the one-time permission prompt being declined, "
                                  "not an error\n",
                                  responseCode);
                    dbus_message_unref(msg2);
                    return false;
                }
                *outResponseMsg = msg2; // caller takes ownership
                return true;
            }
            dbus_message_unref(msg2);
        }
        std::fprintf(stderr, "WaylandScreenCapture: timed out waiting for portal response\n");
        return false;
    }

    bool createSession() {
        DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                          "/org/freedesktop/portal/desktop",
                                                          "org.freedesktop.portal.ScreenCast", "CreateSession");
        DBusMessageIter iter, arrayIter;
        dbus_message_iter_init_append(msg, &iter);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &arrayIter);
        appendStringOption(arrayIter, "handle_token", randomToken());
        appendStringOption(arrayIter, "session_handle_token", randomToken());
        dbus_message_iter_close_container(&iter, &arrayIter);

        DBusMessage* response = nullptr;
        if (!callAndAwaitResponse(msg, kPortalRequestTimeoutMs, &response)) return false;

        DBusMessageIter respIter, resultsIter;
        dbus_message_iter_init(response, &respIter);
        dbus_message_iter_next(&respIter); // skip response code (u), already checked
        dbus_message_iter_recurse(&respIter, &resultsIter);
        bool found = false;
        while (dbus_message_iter_get_arg_type(&resultsIter) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entryIter;
            dbus_message_iter_recurse(&resultsIter, &entryIter);
            const char* key = nullptr;
            dbus_message_iter_get_basic(&entryIter, &key);
            if (std::strcmp(key, "session_handle") == 0) {
                dbus_message_iter_next(&entryIter);
                DBusMessageIter variantIter;
                dbus_message_iter_recurse(&entryIter, &variantIter);
                const char* handle = nullptr;
                dbus_message_iter_get_basic(&variantIter, &handle);
                sessionHandle = handle;
                found = true;
            }
            dbus_message_iter_next(&resultsIter);
        }
        dbus_message_unref(response);
        if (!found) {
            std::fprintf(stderr, "WaylandScreenCapture: CreateSession response had no session_handle\n");
        }
        return found;
    }

    // Reads the ScreenCast interface's AvailableCursorModes property (a
    // bitmask: Hidden=1, Embedded=2, Metadata=4) via a plain synchronous
    // org.freedesktop.DBus.Properties.Get call -- NOT the Request/
    // Response pattern callAndAwaitResponse() handles, since a property
    // read is an ordinary D-Bus method call with a direct reply, not
    // something the portal defers via a Request object. Returns 0 (no
    // modes) on any failure -- an older portal without this property,
    // a malformed reply, anything -- which selectSources() below
    // treats as "can't confirm Embedded is safe, don't ask for it",
    // never as a reason to fail the whole capture setup.
    uint32_t queryAvailableCursorModes() {
        DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                          "/org/freedesktop/portal/desktop",
                                                          "org.freedesktop.DBus.Properties", "Get");
        DBusMessageIter iter;
        dbus_message_iter_init_append(msg, &iter);
        const char* interfaceName = "org.freedesktop.portal.ScreenCast";
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interfaceName);
        const char* propertyName = "AvailableCursorModes";
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &propertyName);

        DBusError error;
        dbus_error_init(&error);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(bus, msg, kPortalRequestTimeoutMs, &error);
        dbus_message_unref(msg);
        if (!reply) {
            dbus_error_free(&error);
            return 0;
        }
        DBusMessageIter replyIter, variantIter;
        dbus_message_iter_init(reply, &replyIter);
        uint32_t modes = 0;
        if (dbus_message_iter_get_arg_type(&replyIter) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&replyIter, &variantIter);
            if (dbus_message_iter_get_arg_type(&variantIter) == DBUS_TYPE_UINT32) {
                dbus_message_iter_get_basic(&variantIter, &modes);
            }
        }
        dbus_message_unref(reply);
        return modes;
    }

    bool selectSources() {
        DBusMessage* msg = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                          "/org/freedesktop/portal/desktop",
                                                          "org.freedesktop.portal.ScreenCast", "SelectSources");
        DBusMessageIter iter, arrayIter;
        dbus_message_iter_init_append(msg, &iter);
        const char* sessionHandleCStr = sessionHandle.c_str();
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &sessionHandleCStr);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &arrayIter);
        appendStringOption(arrayIter, "handle_token", randomToken());
        appendUint32Option(arrayIter, "types", 1); // MONITOR only, not individual windows
        appendBoolOption(arrayIter, "multiple", false);
        // Real user report, 2026-08-03: the mouse cursor was never
        // visible in the mirrored stream at all. Root cause: this
        // option was never set, so the portal defaulted to
        // cursor_mode Hidden (bit 1) -- the ScreenCast interface's
        // three modes are Hidden=1, Embedded=2 (cursor rendered
        // directly into the video frames PipeWire delivers), and
        // Metadata=4 (cursor position/image delivered as separate
        // spa_meta_cursor stream metadata instead of baked into the
        // pixels). Embedded is the simpler of the two real options for
        // this project's existing "just get a flat BGRA frame out"
        // pipeline -- Metadata would need onStreamProcess() to also
        // parse and manually composite spa_meta_cursor, more code for
        // no benefit over just asking the portal to embed it directly.
        //
        // MUST be gated on AvailableCursorModes, not just requested
        // unconditionally: the portal spec is explicit that "setting a
        // cursor mode not advertised will cause the screen cast session
        // to be closed" -- a hard failure of the whole capture session,
        // not a harmless no-op, on any compositor/portal that doesn't
        // advertise Embedded support. Confirmed directly against the
        // spec (xdg-desktop-portal's own ScreenCast interface
        // documentation) before writing this, specifically to avoid
        // trading "no cursor" for "no capture at all" on a portal this
        // wasn't tested against.
        uint32_t availableCursorModes = queryAvailableCursorModes();
        constexpr uint32_t kCursorModeEmbedded = 2;
        if (availableCursorModes & kCursorModeEmbedded) {
            appendUint32Option(arrayIter, "cursor_mode", kCursorModeEmbedded);
        } else {
            std::fprintf(stderr,
                          "WaylandScreenCapture: portal doesn't advertise Embedded cursor mode (available=0x%x) "
                          "-- mirroring will work but the mouse cursor won't be visible in it\n",
                          availableCursorModes);
        }
        dbus_message_iter_close_container(&iter, &arrayIter);

        DBusMessage* response = nullptr;
        bool ok = callAndAwaitResponse(msg, kPortalRequestTimeoutMs, &response);
        if (ok) dbus_message_unref(response); // no fields this class needs from SelectSources itself
        return ok;
    }

    // Returns the first stream's PipeWire node id on success, or
    // std::nullopt-equivalent (0, an id no real node ever uses) on
    // failure -- Start's results dict is `streams: a(ua{sv})`, an array
    // of (node_id, properties) structs; only the id is needed here.
    uint32_t start() {
        DBusMessage* msg = dbus_message_new_method_call(
            "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.ScreenCast",
            "Start");
        DBusMessageIter iter, arrayIter;
        dbus_message_iter_init_append(msg, &iter);
        const char* sessionHandleCStr = sessionHandle.c_str();
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &sessionHandleCStr);
        const char* parentWindow = ""; // no parent window -- this is a background daemon, not a GUI app
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &parentWindow);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &arrayIter);
        appendStringOption(arrayIter, "handle_token", randomToken());
        dbus_message_iter_close_container(&iter, &arrayIter);

        DBusMessage* response = nullptr;
        // This is the call that shows the interactive one-time
        // permission prompt on the host's own screen -- see this
        // class's header comment.
        if (!callAndAwaitResponse(msg, kPortalRequestTimeoutMs, &response)) return 0;

        DBusMessageIter respIter, resultsIter;
        dbus_message_iter_init(response, &respIter);
        dbus_message_iter_next(&respIter);
        dbus_message_iter_recurse(&respIter, &resultsIter);
        uint32_t nodeId = 0;
        while (dbus_message_iter_get_arg_type(&resultsIter) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entryIter;
            dbus_message_iter_recurse(&resultsIter, &entryIter);
            const char* key = nullptr;
            dbus_message_iter_get_basic(&entryIter, &key);
            if (std::strcmp(key, "streams") == 0 && nodeId == 0) {
                dbus_message_iter_next(&entryIter);
                DBusMessageIter variantIter, streamsArrayIter, streamStructIter;
                dbus_message_iter_recurse(&entryIter, &variantIter); // variant -> a(ua{sv})
                dbus_message_iter_recurse(&variantIter, &streamsArrayIter); // array -> first (u a{sv}) struct
                if (dbus_message_iter_get_arg_type(&streamsArrayIter) == DBUS_TYPE_STRUCT) {
                    dbus_message_iter_recurse(&streamsArrayIter, &streamStructIter);
                    dbus_message_iter_get_basic(&streamStructIter, &nodeId);
                }
            }
            dbus_message_iter_next(&resultsIter);
        }
        dbus_message_unref(response);
        if (nodeId == 0) {
            std::fprintf(stderr, "WaylandScreenCapture: Start response had no usable stream node id\n");
        }
        return nodeId;
    }

    // OpenPipeWireRemote is NOT a Request/Response call like the three
    // above -- it returns its result (a UNIX file descriptor) directly
    // in the synchronous method reply, per the portal spec. This fd
    // carries the actual authorization for this specific session's
    // PipeWire access -- connecting to the ambient/default local
    // PipeWire socket instead would not be authorized to see this
    // stream at all.
    int openPipewireRemote() {
        DBusMessage* msg =
            dbus_message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                          "org.freedesktop.portal.ScreenCast", "OpenPipeWireRemote");
        DBusMessageIter iter, arrayIter;
        dbus_message_iter_init_append(msg, &iter);
        const char* sessionHandleCStr = sessionHandle.c_str();
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &sessionHandleCStr);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &arrayIter);
        dbus_message_iter_close_container(&iter, &arrayIter); // no options needed

        DBusError error;
        dbus_error_init(&error);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(bus, msg, kPortalRequestTimeoutMs, &error);
        dbus_message_unref(msg);
        if (!reply) {
            std::fprintf(stderr, "WaylandScreenCapture: OpenPipeWireRemote failed: %s\n",
                          error.message ? error.message : "(no message)");
            dbus_error_free(&error);
            return -1;
        }
        int fd = -1;
        if (!dbus_message_get_args(reply, &error, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID)) {
            std::fprintf(stderr, "WaylandScreenCapture: malformed OpenPipeWireRemote reply: %s\n",
                          error.message ? error.message : "(no message)");
            dbus_error_free(&error);
            fd = -1;
        }
        dbus_message_unref(reply);
        return fd;
    }

    static void onStreamParamChanged(void* userdata, uint32_t id, const spa_pod* param) {
        auto* self = static_cast<Impl*>(userdata);
        if (!param || id != SPA_PARAM_Format) return;
        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) return;
        std::lock_guard<std::mutex> lock(self->frameMutex);
        self->width = static_cast<uint16_t>(info.size.width);
        self->height = static_cast<uint16_t>(info.size.height);
        self->format = info.format;
    }

    // Real bug, 2026-08-03: the format request this constructor sends
    // PipeWire used to offer exactly one format (BGRx) with no
    // alternatives -- on any compositor whose screen-capture source
    // doesn't happen to natively produce BGRx (RGBx/RGBA/BGRA are all
    // just as common depending on compositor/GPU), SPA_PARAM_EnumFormat
    // negotiation had nothing to intersect against and silently never
    // completed: onStreamParamChanged() never fired with a usable
    // Format, width/height stayed 0, no frame was ever produced, the
    // 5-second wait in the constructor timed out, isReady() stayed
    // false, and host_control_adapter.cpp's fallback chain dropped back
    // to X11ScreenCapture -- which "succeeds" against XWayland's own
    // empty root and streams solid black. Symptom exactly matched a real
    // report: the portal's one-time permission prompt appeared and was
    // accepted (proving Start() succeeded), but the client still showed
    // a black screen. The constructor's format offer now lists BGRx,
    // BGRA, RGBx, and RGBA as acceptable alternatives (see the
    // SPA_POD_CHOICE_ENUM_Id call below), so this now negotiates
    // successfully against whatever the compositor actually produces.
    // Whichever one gets negotiated is converted to this project's
    // BGRA8888 wire convention here: BGRx/BGRA need no per-pixel work
    // (straight row copy, same reasoning as X11ScreenCapture's own
    // row-copy comment -- the unused 4th byte is harmless padding
    // either way); RGBx/RGBA have their R and B bytes swapped per pixel,
    // since copying those straight would silently swap red and blue in
    // every streamed frame instead of failing loudly.
    static void onStreamProcess(void* userdata) {
        auto* self = static_cast<Impl*>(userdata);
        pw_buffer* b = pw_stream_dequeue_buffer(self->stream);
        if (!b) return;
        spa_buffer* buf = b->buffer;
        if (buf->datas[0].data && self->width > 0 && self->height > 0) {
            std::lock_guard<std::mutex> lock(self->frameMutex);
            size_t pixelCount = static_cast<size_t>(self->width) * self->height;
            self->latestFrame.resize(pixelCount * 4);
            const uint8_t* src = static_cast<const uint8_t*>(buf->datas[0].data);
            uint32_t stride = buf->datas[0].chunk->stride > 0
                                   ? static_cast<uint32_t>(buf->datas[0].chunk->stride)
                                   : self->width * 4;
            const bool needsRbSwap =
                self->format == SPA_VIDEO_FORMAT_RGBx || self->format == SPA_VIDEO_FORMAT_RGBA;
            if (needsRbSwap) {
                for (uint16_t y = 0; y < self->height; ++y) {
                    const uint8_t* srcRow = src + static_cast<size_t>(y) * stride;
                    uint8_t* dstRow = self->latestFrame.data() + static_cast<size_t>(y) * self->width * 4;
                    for (uint16_t x = 0; x < self->width; ++x) {
                        dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // B <- R
                        dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G
                        dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // R <- B
                        dstRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A/x
                    }
                }
            } else {
                for (uint16_t y = 0; y < self->height; ++y) {
                    std::memcpy(self->latestFrame.data() + static_cast<size_t>(y) * self->width * 4,
                                src + static_cast<size_t>(y) * stride, static_cast<size_t>(self->width) * 4);
                }
            }
            self->haveFrame = true;
        }
        pw_stream_queue_buffer(self->stream, b);
    }

    // Defined out-of-line just below (needs onStreamParamChanged/
    // onStreamProcess to already be declared, and -- since Impl itself
    // is a private nested type of WaylandScreenCapture -- must be
    // defined within Impl's own qualified scope rather than in the
    // enclosing anonymous namespace, which has no access to a private
    // nested type name.
    static const pw_stream_events kStreamEvents;
};

// Built via a value-initialized local rather than a designated-
// initializer list: pw_stream_events has several other function-pointer
// members this class doesn't need (destroy/state_changed/add_buffer/
// etc.), and a designated-initializer list that only names three of
// them triggers -Wmissing-field-initializers under this project's
// standard -Wextra build -- this achieves the identical result (every
// unset field zero-initialized) without that warning.
const pw_stream_events WaylandScreenCapture::Impl::kStreamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.param_changed = WaylandScreenCapture::Impl::onStreamParamChanged;
    events.process = WaylandScreenCapture::Impl::onStreamProcess;
    return events;
}();

WaylandScreenCapture::WaylandScreenCapture() : impl_(std::make_unique<Impl>()) {
    DBusError error;
    dbus_error_init(&error);
    impl_->bus = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (!impl_->bus) {
        // Expected and silent-by-design if there's genuinely no session
        // bus reachable (e.g. a from-source dev/CI environment) -- the
        // constructor's caller (HostControlAdapter) is the one that
        // logs, conditioned on whether mirroring was actually requested.
        dbus_error_free(&error);
        return;
    }
    dbus_connection_set_exit_on_disconnect(impl_->bus, FALSE);

    if (!impl_->createSession()) return;
    if (!impl_->selectSources()) return;
    uint32_t nodeId = impl_->start();
    if (nodeId == 0) return;
    impl_->pipewireFd = impl_->openPipewireRemote();
    if (impl_->pipewireFd < 0) return;

    static std::once_flag pwInitFlag;
    std::call_once(pwInitFlag, [] { pw_init(nullptr, nullptr); });

    impl_->loop = pw_thread_loop_new("dualdeck-wayland-capture", nullptr);
    if (!impl_->loop) {
        std::fprintf(stderr, "WaylandScreenCapture: pw_thread_loop_new failed\n");
        return;
    }
    pw_thread_loop_lock(impl_->loop);
    impl_->context = pw_context_new(pw_thread_loop_get_loop(impl_->loop), nullptr, 0);
    if (!impl_->context) {
        pw_thread_loop_unlock(impl_->loop);
        std::fprintf(stderr, "WaylandScreenCapture: pw_context_new failed\n");
        return;
    }
    // Connects using the portal-authorized fd from OpenPipeWireRemote
    // above -- NOT pw_context_connect(), which would reach for the
    // ambient default PipeWire socket with no authorization for this
    // specific screen-capture session at all.
    impl_->core = pw_context_connect_fd(impl_->context, ::fcntl(impl_->pipewireFd, F_DUPFD_CLOEXEC, 0), nullptr, 0);
    if (!impl_->core) {
        pw_thread_loop_unlock(impl_->loop);
        std::fprintf(stderr, "WaylandScreenCapture: pw_context_connect_fd failed\n");
        return;
    }

    impl_->stream = pw_stream_new(impl_->core, "dualdeck-host-control-mirror",
                                   pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                      PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    if (!impl_->stream) {
        pw_thread_loop_unlock(impl_->loop);
        std::fprintf(stderr, "WaylandScreenCapture: pw_stream_new failed\n");
        return;
    }
    pw_stream_add_listener(impl_->stream, &impl_->streamListener, &Impl::kStreamEvents, impl_.get());

    uint8_t podBuffer[1024];
    spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
    spa_rectangle defSize = SPA_RECTANGLE(1920, 1080);
    spa_rectangle minSize = SPA_RECTANGLE(1, 1);
    spa_rectangle maxSize = SPA_RECTANGLE(8192, 8192);
    spa_fraction defFramerate = SPA_FRACTION(0, 1);
    spa_fraction minFramerate = SPA_FRACTION(0, 1);
    spa_fraction maxFramerate = SPA_FRACTION(1000, 1);
    const spa_pod* params[2];
    params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &podBuilder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format,
        // See onStreamProcess()'s comment: offering only BGRx here used
        // to make negotiation silently fail (no frame, ever) against any
        // compositor whose capture source doesn't produce BGRx natively.
        // BGRx stays the preferred/default value (first argument after
        // the count -- see spa_choice_type's own "list: default,
        // alternative,..." doc comment, which is why it's listed twice:
        // once as the default slot, once as its own alternative-list
        // member), with BGRA/RGBx/RGBA offered as acceptable fallbacks.
        SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                               SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&defSize, &minSize, &maxSize), SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&defFramerate, &minFramerate, &maxFramerate)));
    // Second real bug behind the same "black after granting permission"
    // symptom, found after the format-choice fix above still wasn't
    // enough on its own: without this, PipeWire is free to negotiate
    // SPA_DATA_DmaBuf buffers (GPU memory, zero-copy, the type many
    // Wayland compositors' screencast sources prefer/default to,
    // especially with hardware-accelerated capture) -- onStreamProcess()
    // only ever reads buf->datas[0].data directly, which is NULL for a
    // DMA-BUF (that's GPU memory; CPU-mapping it needs GBM/EGL, not a
    // plain pointer), so a DMA-BUF buffer type makes onStreamProcess()
    // silently skip every single frame forever, exactly like the missing
    // format alternatives did: no frame ever completes, the constructor's
    // wait times out, isReady() stays false, and the same fallback chain
    // drops to X11ScreenCapture's black XWayland capture. This second
    // param object restricts SPA_PARAM_Buffers' negotiable dataType to
    // just MemPtr/MemFd (real CPU-mapped memory this code can actually
    // read), so PipeWire has nothing to offer here but the kind of buffer
    // onStreamProcess() already knows how to consume -- PW_STREAM_FLAG_
    // MAP_BUFFERS then does the actual mmap()'ing for MemFd.
    params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &podBuilder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemPtr) | (1u << SPA_DATA_MemFd))));

    int connectResult =
        pw_stream_connect(impl_->stream, PW_DIRECTION_INPUT, nodeId,
                           static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                           params, 2);
    pw_thread_loop_unlock(impl_->loop);
    if (connectResult < 0) {
        std::fprintf(stderr, "WaylandScreenCapture: pw_stream_connect failed (%d)\n", connectResult);
        return;
    }
    if (pw_thread_loop_start(impl_->loop) < 0) {
        std::fprintf(stderr, "WaylandScreenCapture: pw_thread_loop_start failed\n");
        return;
    }

    // Bounded wait for the first real frame -- confirms the whole
    // chain (portal grant -> PipeWire fd -> stream negotiation ->
    // actual frame delivery) genuinely worked end to end, rather than
    // reporting ready the instant pw_stream_connect() merely returned
    // 0 (which only means "request accepted," not "frames flowing").
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPipewireFirstFrameTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(impl_->frameMutex);
            if (impl_->haveFrame) {
                impl_->ready = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!impl_->ready) {
        std::fprintf(stderr, "WaylandScreenCapture: no frame received within %dms of connecting -- disabling\n",
                      kPipewireFirstFrameTimeoutMs);
    } else {
        std::fprintf(stderr, "WaylandScreenCapture: screen-mirror capture ready (Wayland portal, %ux%u)\n",
                      impl_->width, impl_->height);
    }
}

WaylandScreenCapture::~WaylandScreenCapture() = default;

bool WaylandScreenCapture::isReady() const { return impl_->ready; }

void WaylandScreenCapture::nativeSize(uint16_t& outWidth, uint16_t& outHeight) const {
    std::lock_guard<std::mutex> lock(impl_->frameMutex);
    outWidth = impl_->ready ? impl_->width : 0;
    outHeight = impl_->ready ? impl_->height : 0;
}

bool WaylandScreenCapture::capture(std::vector<uint8_t>& outFrame, uint16_t& outWidth, uint16_t& outHeight) {
    if (!impl_->ready) return false;
    std::lock_guard<std::mutex> lock(impl_->frameMutex);
    if (!impl_->haveFrame) return false;
    outFrame = impl_->latestFrame;
    outWidth = impl_->width;
    outHeight = impl_->height;
    return true;
}

#else // !DUALDECK_HAVE_WAYLAND_SCREEN_CAPTURE

struct WaylandScreenCapture::Impl {};

WaylandScreenCapture::WaylandScreenCapture() : impl_(std::make_unique<Impl>()) {}
WaylandScreenCapture::~WaylandScreenCapture() = default;
bool WaylandScreenCapture::isReady() const { return false; }
void WaylandScreenCapture::nativeSize(uint16_t& outWidth, uint16_t& outHeight) const {
    outWidth = 0;
    outHeight = 0;
}
bool WaylandScreenCapture::capture(std::vector<uint8_t>&, uint16_t&, uint16_t&) { return false; }

#endif

} // namespace melonds_remote::host
