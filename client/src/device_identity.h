#pragma once

// Persists this client's own persistent device identity: a random value
// generated once and reused forever, on every Hello to every host, as the
// authToken field (see melonds_remote::HelloPayload::authToken and
// docs/protocol.md's "Authentication and device approval" section).
//
// Replaces the earlier per-host 6-digit pairing code / issued-token
// scheme (see git history if it's ever needed again) -- Steam Input
// doesn't reliably bring up a virtual keyboard in Gaming Mode, so typing
// anything on the client isn't a workable UX. Instead, a human at the
// host approves or denies this identity once (see
// host/remote-server/include/host/device_approval_manager.h); the
// identity itself, once approved, is the ongoing credential -- there is
// nothing further for the client to remember per-host.
//
// Plain text, single line, not a secrets vault -- same trust model as
// melonDS's own on-disk config (local, single-user, LAN-only tool).

#include <string>

namespace melonds_remote::client {

// $HOME/.config/melonds-remote-client/device_id.txt, or empty if $HOME
// isn't set (persistence then silently unavailable -- a fresh random
// identity is used for the life of the process instead, meaning it'll
// need re-approval next launch).
std::string defaultDeviceIdentityStorePath();

// Loads the persisted device identity at `storePath`, or generates a
// fresh one (32 random hex characters) and persists it there if none
// exists yet (or `storePath` is empty, in which case the fresh value just
// isn't persisted). Always returns a non-empty identity.
std::string loadOrCreateDeviceIdentity(const std::string& storePath);

} // namespace melonds_remote::client
