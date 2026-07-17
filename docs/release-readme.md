# melonDS Remote

Play Nintendo DS games on your TV through melonDS while using a Steam
Deck (or any Linux machine with a gamepad) as the handheld controller
and bottom screen.

## Quick start

**Host** (the Linux PC/HTPC connected to your TV, running melonDS):
double-click `host/melonds-remote-host.sh`. Pick "Launch melonDS now"
the first time -- it sets up anything it needs automatically, including
on Bazzite/immutable systems. Pick "Add to Steam" if you want to launch
it from Steam Big Picture/Gaming Mode with just a controller.

**Client** (your Steam Deck, or any Linux x86_64 machine with a
gamepad): double-click `client/melonds-remote-client.sh`. Same menu,
same idea -- "Launch now" connects to a host on your network (it scans
and shows a pick-a-host list), "Add to Steam" registers a Gaming Mode
shortcut.

Both menus also have "Check for updates" -- if a newer release exists,
it offers to download and install it right there, no manual steps.

On SteamOS Desktop Mode or Bazzite (both KDE Plasma/Dolphin),
double-clicking an executable `.sh` file just runs it directly -- no
terminal, no typing required anywhere in this quick start. See
`docs/steam-deck-setup.md` and `docs/bazzite-host-setup.md` for the
full walkthrough, including the one manual step neither menu automates
(setting the Steam shortcut's Controller Layout to a plain "Gamepad"
template).

## First connection

The first time a client connects to a host, a human needs to approve it
at the host (shown as a popup if you're at the melonDS window, or in
its terminal output otherwise) -- nothing to type on the client side.
After that, it's remembered automatically.

## What's inside

- `host/` -- the host side (patched melonDS plus everything needed to
  install and run it). `host/internal/` holds the scripts the menu uses
  internally -- you generally won't need to open anything in there
  directly.
- `client/` -- the Steam Deck client, laid out the same way.
- `docs/` -- setup guides, the wire protocol reference, and
  `known-limitations.md` (an honest list of what is and isn't verified
  in this specific build).
- `RELEASE_NOTES.md` -- exactly which commit this build was made from.
- `VERSION` / `check-for-updates.sh` -- what the "Check for updates"
  menu choice uses; read-only if run directly, never downloads or
  installs anything by itself.

## License

GPLv3 -- see `LICENSE`.
