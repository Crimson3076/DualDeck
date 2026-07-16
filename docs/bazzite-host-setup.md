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
    --bind <your-LAN-IP> \
    --state-dir ~/.config/melonds-remote
```

Bind to your actual LAN-facing interface address (not `127.0.0.1`, which
only accepts connections from the same machine) so the Steam Deck client
can reach it. Find your LAN IP with `ip addr` or `nmcli device show`.
Without `--auth-token`, this runs in pairing mode (spec section 13's
six-digit pairing code, the recommended default) -- the first connection
attempt from the client gets a code logged here to enter once; add
`--auth-token <a-shared-secret>` instead if you'd rather manage a static
shared secret yourself. See `docs/protocol.md`'s "Authentication and
pairing" section.

### Firewall

Bazzite uses `firewalld` by default (KDE Plasma's Wayland session doesn't
change this). Open the three ports this prototype uses (defaults: 8760
TCP control, 8761 UDP input, 8762 TCP video -- adjust if you passed
different `--*-port` flags):

```sh
sudo firewall-cmd --add-port=8760/tcp --add-port=8761/udp --add-port=8762/tcp
# add --permanent and reload if you want this to survive a reboot:
sudo firewall-cmd --permanent --add-port=8760/tcp --add-port=8761/udp --add-port=8762/tcp
sudo firewall-cmd --reload
```

### Launch script

`scripts/run-host.sh` builds (if needed) and runs the server, forwarding
any extra arguments -- e.g.:

```sh
./scripts/run-host.sh --bind 192.168.1.50 --auth-token some-shared-secret
```

## What's not covered here yet

- Actually building/running the patched melonDS *on Bazzite specifically*:
  the patch (`host/melonds-patches/0001-remote-server-integration.patch`)
  exists and has been verified in this project's own development sandbox
  (Ubuntu 24.04), but not on real Bazzite. Expect the same
  Distrobox-vs-layered-package tradeoff described above for melonDS's own
  (larger) Qt6/SDL2/OpenGL dependency list, plus GPU passthrough
  considerations specific to running a Qt/OpenGL application from inside
  a container (Bazzite's Distrobox setup generally handles this for AMD
  GPUs via Mesa, but this hasn't been tested against melonDS specifically
  on Bazzite).
- A systemd user service / autostart unit so the host server comes up
  automatically -- not implemented; `scripts/run-host.sh` is a manual
  foreground launch for now.
- RPM or Bazzite-specific packaging (`SPEC.md` section 17 lists this as
  later/target packaging, not required for the initial prototype).
