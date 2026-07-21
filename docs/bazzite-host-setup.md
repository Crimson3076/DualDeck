# Bazzite Host Setup

The required "Bazzite host setup instructions" deliverable (`SPEC.md`
section 25), for the primary target platform (section 3: Bazzite,
Fedora-based, KDE Plasma, Wayland, AMD GPU preferred). **Status: written
from general Bazzite/Fedora Atomic knowledge, not verified on a real
Bazzite install** -- see `docs/known-limitations.md`. This covers
building and running the standalone `host/remote-server` prototype. A
real melonDS integration patch now exists
(`host/melonds-patches/0001-remote-server-integration.patch`) and has
been build/run-verified in this project's own development sandbox, but
*that specific verification* was on Ubuntu, not Bazzite -- building it
here follows the same Distrobox approach below, just against melonDS's
own (larger) Qt6/SDL2/OpenGL dependency list instead of this prototype's
none; see `host/melonds-patches/README.md`.

## Quickest path: prebuilt release, no terminal needed

Everything below this section is about building `host/remote-server` (the
standalone prototype) from source inside a Distrobox. If you just want
to run the real melonDS-integrated host without building anything,
download the latest release archive instead (see the top-level
`README.md`'s "Download a build") and extract it.

**Double-click `host/melonds-remote-host.sh`.** That's the only host
script you need to know about -- it shows a menu (a graphical one via
`kdialog` on Bazzite/SteamOS's KDE Plasma desktop, or a plain numbered
prompt if run from a terminal without `kdialog`) with four choices:

- **Launch melonDS now** -- works whether or not this is your first
  time: on a regular Linux system it just runs melonDS directly
  (auto-installing missing libraries first); on an immutable system
  like Bazzite it transparently sets up (or reuses) a Distrobox
  container with everything melonDS needs, then launches from there.
  Either way, running this again later (including from a newer
  release's extracted archive) picks up updates automatically -- there's
  no separate "install" step to remember.
- **Add to Steam (Big Picture / Gaming Mode)** -- registers a "melonDS
  Remote Host" entry in your Steam library, so it's launchable with just
  a controller. Safe to re-run from a newer release to update the
  existing entry in place.
- **Remove from Steam / uninstall** -- removes the Steam shortcut, the
  installed files, and the Distrobox container if one was created (with
  a confirmation first). Never touches ROMs, saves, or firmware.
- **Check for updates / update** -- checks the latest GitHub release
  against your installed version. If you're already current, that's all
  it does. If a newer version exists, it asks for confirmation before
  doing anything further -- say yes and it downloads that release and
  installs it over your current one (same stage-then-swap safety as
  everything else here). Steam does **not** need to be closed for this:
  a routine update never changes the Steam shortcut's Exe/name/launch
  options, so it never touches Steam's library file, only the installed
  program files. Say no, or there's nothing newer, and nothing is
  downloaded or changed.

Everything this menu does is really just a thin front end over the
individual scripts in `host/internal/` (`run-host.sh`,
`install-host-distrobox.sh`, `install-steam-shortcut.sh`, `apply-update.sh`,
etc.) -- those still exist and still work standalone if you want to
script something directly or troubleshoot a specific step, but there's
no need to open that folder or know which one applies to your system:
the menu figures that out for you. See "Easier Bazzite host install and
updates" below for what the Distrobox path actually does under the
hood, and "Launching the host from Steam Big Picture/Gaming Mode" for
the Steam-shortcut part.

On Bazzite's KDE Plasma/Dolphin desktop, double-clicking an executable
`.sh` file offers to run it directly -- no terminal or typing needed.
See `docs/steam-deck-setup.md`'s equivalent section for the client side.

## Easier Bazzite host install and updates: `install-host-distrobox.sh`

Addresses GitHub issue #10 ("simplify host installation and updates on
Bazzite and other supported Linux systems"): running the prebuilt
release binary directly on an immutable system wasn't really "quickest"
at all before this -- `run-host.sh`'s auto-install step refuses to touch
an rpm-ostree base image (correctly -- it would need a reboot to take
effect, and there's no way to layer the full Qt6/SDL2/X11/audio
dependency list unattended), so the only options were reading
`host/melonds-patches/README.md`'s from-source Distrobox instructions or
figuring out `rpm-ostree install` + reboot by hand.

`host/internal/install-host-distrobox.sh` (packaged alongside
`run-host.sh` in every release archive's `host/internal/`, and what
"Launch now" in `host/melonds-remote-host.sh`'s menu calls on an
immutable system) automates the Distrobox path end to end:

1. Detects whether this actually is an immutable/rpm-ostree system at
   all (same check `ensure_packages()` already uses elsewhere in this
   project) -- refuses to run and points you at `run-host.sh` instead if
   not, since there's no need for a container there.
2. Copies the whole `host/` directory (both the entry-point script and
   binary alongside it, and this internal/ folder) into a fixed
   location, `~/.config/melonds-remote/install/` -- this is what makes
   updates simple: downloading a newer release and running this again
   re-syncs that copy and re-verifies the container's packages, so
   there's nothing to redo by hand, and once set up once you can launch
   or update straight from that central directory instead of keeping the
   original download around.
3. Creates (or reuses -- safe to re-run) a Fedora-based Distrobox
   container named `melonds-remote-host`.
4. Installs the runtime library packages the host needs inside that
   container via `dnf` (skips anything already present).
5. Launches melonDS (with the remote server enabled) from inside the
   container -- Distrobox shares the host's X11/Wayland session and
   networking automatically, so the video output and LAN
   discovery/connections work the same as running natively.

**A failed update can't break a working install**: step 2's copy and
step 4's package install both happen in a separate staging location
first (`~/.config/melonds-remote/install.new`) -- only once both
succeed does the script swap it into place as the real
`~/.config/melonds-remote/install/`, keeping the just-replaced version
as a one-generation backup (`install.previous`) rather than deleting it.
If the copy or the package install fails partway (no disk space,
network drops mid-`dnf`, etc.), the previous, still-working install is
left completely untouched, not deleted-then-maybe-not-replaced.

**Uninstalling**: pick "Remove from Steam / uninstall" from
`host/melonds-remote-host.sh`'s menu, or double-click
`host/internal/uninstall-host-distrobox.sh` directly -- either removes
the Distrobox container and everything under
`~/.config/melonds-remote/install*`. Never touches ROMs, saves,
firmware, or any other melonDS data, since none of that lives in either
of those places -- it all stays in your normal home directory the whole
time (Distrobox shares it with the container automatically), regardless
of the container or central install directory being removed.

## Launching the host from Steam Big Picture/Gaming Mode

Addresses the last remaining piece of GitHub issue #10's acceptance
criteria: "the installed host can be launched from Steam Big Picture or
Gaming Mode and accept a client connection." Pick "Add to Steam" from
`host/melonds-remote-host.sh`'s menu (see above), or double-click
`host/internal/install-steam-shortcut.sh` directly (once, in Desktop
Mode, with Steam closed) -- either way it adds a **"melonDS Remote
Host"** entry to your Steam library, mirroring
`client/internal/install-steam-shortcut.sh`'s approach exactly, down to
the same stage-then-swap update safety and error-log/`kdialog` failure
visibility.

This registers the shortcut's `Exe` as `melonds-remote-host.sh` itself
-- the same menu you get from double-clicking it, including its
"Which system?" picker (DS/melonDS, 3DS/Azahar, host-control-only, or a
custom emulator via `scripts/patch-existing-emulator.sh`). Picking
"Nintendo DS (melonDS)" there dispatches to `internal/launch-host.sh`,
which in turn:

- **On an immutable system** (Bazzite, etc.): behaves like
  `install-host-distrobox.sh` -- prepares (or reuses) the Distrobox
  container the first time, then launches melonDS from inside it every
  time the shortcut is used.
- **On a regular Linux system**: just runs `run-host.sh` directly, same
  as double-clicking it yourself.

Like the client's shortcut, this copies the whole `host/` directory into
the same fixed central location (`~/.config/melonds-remote/install/`)
install-host-distrobox.sh already uses, so re-running
`install-steam-shortcut.sh` from a newer release's extracted archive
updates the same shortcut and the same install in place -- no
duplicates, and the download folder can be deleted afterward.

On an immutable system, the actual file staging and Distrobox/`dnf`
setup is entirely delegated to `install-host-distrobox.sh --install-only`
(prepares everything but doesn't launch melonDS immediately just because
you registered a shortcut) -- this matters for the same rollback-safety
reason install-host-distrobox.sh already has: the new files are only
swapped into place once the container's packages actually installed
successfully, so a failed `dnf install` during an update can never leave
half-applied, unverified files active in place of a working install.

**Uninstalling**: pick "Remove from Steam / uninstall" from
`host/melonds-remote-host.sh`'s menu, or double-click
`host/internal/uninstall-steam-shortcut.sh` directly -- either removes
the Steam shortcut, the central install directory, *and* the Distrobox
container if one was created, so this alone is a complete uninstall
once you've used the Steam-shortcut path. (If you only ever used
`install-host-distrobox.sh` directly and never installed the shortcut,
`uninstall-host-distrobox.sh` alone is equivalent -- it just doesn't
know about a shortcut that was never created.)

**Not verified on a real Bazzite install** (same caveat as everything
else Bazzite-specific here): exercised end-to-end in this project's own
sandbox using fake `distrobox`/`dnf`/`$HOME` stubs -- fresh install,
launch, update, a simulated `dnf` failure mid-update (confirmed the
previous working install is left completely untouched), uninstall, and
re-uninstall idempotency all pass, on both a simulated immutable system
and a regular one. Real Distrobox/`dnf` behavior against a genuine
Fedora/Bazzite image, and Steam's own real Big Picture/Gaming Mode UI,
are not exercised by this.

**`install-host-distrobox.sh` itself is also not verified on a real
Bazzite install** (no rpm-ostree/Distrobox environment in this project's
own sandbox -- see `docs/known-limitations.md` for the honest account of
what is and isn't confirmed here, matching this doc's own existing
disclaimer at the top). If `distrobox` isn't installed at all (Bazzite
ships it by default, but a different rpm-ostree derivative might not),
the script says so and points at
[the Distrobox project](https://github.com/89luca89/distrobox) rather
than guessing.

## Why Bazzite needs a different approach than a normal distro

Bazzite is an `rpm-ostree`-based (Fedora Atomic/Silverblue-derived)
immutable-filesystem distribution: `/usr` is read-only and you don't
`dnf install` build tooling directly onto the base image the way you
would on a traditional Fedora install. The two supported ways to get a
C++ toolchain for building this project are:

1. **A Distrobox/Toolbox container** (recommended for development). This
   keeps build tooling out of the immutable base image entirely.
2. **`rpm-ostree install`** for anything you genuinely want layered onto
   the base image permanently (requires a reboot to apply, and every
   layered package is rebuilt on top of each base image update -- fine
   for a handful of packages, cumbersome for a full dev environment).

## Option 1: build inside a Distrobox (recommended)

Bazzite ships Distrobox by default.

```sh
# Create a Fedora-based dev container (matches Bazzite's own base closely)
distrobox create --name melonds-remote-dev --image fedora:latest
distrobox enter melonds-remote-dev

# Inside the container:
sudo dnf install -y gcc-c++ cmake git python3
git clone <this-repo-url>
cd melonds-remote  # or whatever you've named the checkout
cmake -S . -B build -DMELONDS_REMOTE_BUILD_CLIENT=OFF
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The resulting `melonds-remote-server` binary is a normal dynamically
linked Linux executable; it runs fine from inside the Distrobox (Bazzite
exports host networking to Distrobox containers by default, so binding
to a LAN-reachable address works normally) or you can copy it out to run
directly on the host if you'd rather not keep the container around:

```sh
distrobox-export --bin build/host/remote-server/melonds-remote-server
```

## Option 2: layer build tools directly onto the base image

Only do this if you specifically want the toolchain available outside a
container (e.g. for a system service):

```sh
rpm-ostree install gcc-c++ cmake git
systemctl reboot
```

Then build exactly as in `docs/building.md`, no container needed.

## Running the host

```sh
./build/host/remote-server/melonds-remote-server \
    --state-dir ~/.config/melonds-remote
```

No `--bind` needed -- it defaults to `0.0.0.0` (all interfaces), so the
Steam Deck client can reach it, and so can its LAN discovery scan (the
client no longer needs to be given this machine's address at all; see
`docs/steam-deck-setup.md`). Pass `--bind <your-LAN-IP>` yourself only if
you specifically want to restrict which interface answers (e.g. this
machine has multiple NICs and you only want one reachable) -- find your
LAN IP with `ip addr` or `nmcli device show`. `--bind 127.0.0.1` is also
available if you only ever intend to connect from this same machine.
Without `--auth-token`, this runs in device-approval mode (spec section
13, adapted -- the recommended default): the first connection attempt
from the client gets a pending-request line logged here, naming the
client and its address; type `approve <device-id-prefix>` and press
Enter to let it connect (or `deny <device-id-prefix>` to refuse it, or
`list` to see everything pending). No code is ever typed on the client.
Add `--auth-token <a-shared-secret>` instead if you'd rather manage a
static shared secret yourself. See `docs/protocol.md`'s "Authentication
and device approval" section.

### Firewall

Bazzite uses `firewalld` by default (KDE Plasma's Wayland session doesn't
change this). Open the three ports this prototype uses (defaults: 8760
TCP control, 8761 UDP input, 8762 TCP video -- adjust if you passed
different `--*-port` flags):

```sh
sudo firewall-cmd --add-port=8760/tcp --add-port=8761/udp --add-port=8762/tcp --add-port=8763/udp
# add --permanent and reload if you want this to survive a reboot:
sudo firewall-cmd --permanent --add-port=8760/tcp --add-port=8761/udp --add-port=8762/tcp --add-port=8763/udp
sudo firewall-cmd --reload
```

(`8763/udp` is LAN discovery -- skip it if you started the host with
`--no-discovery`.)

### Launch script

`scripts/run-host.sh` builds (if needed) and runs the server, forwarding
any extra arguments -- e.g.:

```sh
./scripts/run-host.sh --auth-token some-shared-secret
```

## What's not covered here yet

- Actually *building* the patched melonDS *on Bazzite specifically*: the
  patch (`host/melonds-patches/0001-remote-server-integration.patch`)
  exists and has been verified in this project's own development sandbox
  (Ubuntu 24.04), but not on real Bazzite. Expect the same
  Distrobox-vs-layered-package tradeoff described above for melonDS's own
  (larger) Qt6/SDL2/OpenGL dependency list, plus GPU passthrough
  considerations specific to running a Qt/OpenGL application from inside
  a container (Bazzite's Distrobox setup generally handles this for AMD
  GPUs via Mesa, but this hasn't been tested against melonDS specifically
  on Bazzite). *Running* the prebuilt release binary on Bazzite is now
  automated by `install-host-distrobox.sh` (see above) -- same
  not-yet-verified-on-real-hardware caveat applies to it too.
- A systemd user service / autostart unit so the host server comes up
  automatically -- not implemented; `scripts/run-host.sh` is a manual
  foreground launch for now.
- RPM or Bazzite-specific packaging (`SPEC.md` section 17 lists this as
  later/target packaging, not required for the initial prototype).
