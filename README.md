# x11-magnifier

A simple full-screen magnifier for X11 activated by Ctrl+Shift + two-finger scroll.
Similar in behavior to MacOS Ctrl+scroll. Unlike other X11 magnifiers, which open a
small lens window, this one magnifies the entire visible screen and shows live
updates:

- **Scroll up** zooms in around the pointer (content under the pointer stays put).
- **Push the pointer to a screen edge** to pan the view that way.
- **Scroll down** zooms out; at 1.0x the overlay disappears (the daemon keeps waiting).
- A **mouse click** also leaves the zoomed view (there is deliberately no keyboard
  exit, so every key still reaches the app you are typing in).

The zoomed view is view-only: clicks do not reach the windows underneath. It is
X11 only (it will not work on Wayland) and treats the whole X screen as one image,
so multi-monitor setups may behave oddly.

The zoomed view is live: windows are redirected with the Composite extension and
repainted over the initial snapshot when XDamage reports changes (at most every
200 ms), so typing in a terminal shows up. `-p` gives the plain static snapshot.
When a window closes or moves, or you switch workspace, the view re-reads the real
screen after a short pause (about 0.25 s) so nothing stale lingers; set
`workspace_switch = exit` (or `-w exit`) to leave the zoom on a workspace switch
instead. GL/video windows may not update.

## Install as a Debian package

    sudo apt install build-essential debhelper libx11-dev libxrender-dev libxcomposite-dev libxdamage-dev
    dpkg-buildpackage -us -uc -b
    sudo dpkg -i ../x11-magnifier_*.deb

## Build / run without packaging

    sudo apt install build-essential libx11-dev libxrender-dev libxcomposite-dev libxdamage-dev
    make && make install          # installs to /usr/local/bin (PREFIX=$HOME/.local for a user install)
    x11-magnifier &               # e.g. from ~/.icewm/startup

No xbindkeys or window manager key config is needed: x11-magnifier grabs the
modifier+scroll combination itself. Start it from `~/.xsession`, or from your
window manager's autostart (for icewm, `~/.icewm/startup`).

## Configuration

Everything below can also live in `~/.config/x11-magnifier/config` (`key = value` lines;
see `x11-magnifier.conf.example`, installed under `/usr/share/doc/x11-magnifier/examples/`).
Keys: `modifiers`, `workspace_switch`, `step`, `max_zoom`, `edge_margin`, `refresh_ms`,
`live`, `nearest`, `invert`. Flags override the file; `-c FILE` picks another file.
`-p` is the same as `live = off`.

## Options

    -m MODS   ctrl+shift (default), super, ctrl+alt, ...
    -c FILE   config file
    -w MODE   workspace switch: refresh (default) or exit
    -s STEP   zoom factor per scroll tick (default 1.08)
    -M MAX    max zoom (default 12)
    -e PX     edge-pan margin (default 40)
    -r MS     min ms between live refreshes (default 200)
    -p        plain static snapshot, no live refresh
    -n        nearest-neighbour scaling instead of bilinear
    -i        invert direction (natural scrolling)

While running, the chosen modifier+scroll is swallowed system-wide, so with the
default modifiers Ctrl+Shift+scroll won't reach other apps (plain and Ctrl-only
scroll are unaffected).

## License

Licensed at your option under either of:

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE))
- MIT license ([LICENSE-MIT](LICENSE-MIT))
