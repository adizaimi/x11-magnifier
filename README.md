# x11-magnifier

Full-screen magnifier for X11. Ctrl+Shift + two-finger scroll.

- **Scroll up** zooms in around the pointer (content under the pointer stays put).
- **Push the pointer to a screen edge** to pan the view that way.
- **Scroll down** zooms out; at 1.0x the overlay disappears (the daemon keeps waiting).
- A **mouse click** also leaves the zoomed view (there is deliberately no keyboard
  exit, so every key still reaches the app you are typing in).

The zoomed view is live: windows are redirected with the Composite extension and
repainted over the initial snapshot when XDamage reports changes (at most every
200 ms), so typing in a terminal shows up. `-p` gives the plain static snapshot.
Windows that close while zoomed linger until you zoom out, and GL/video windows
may not update.

## Install as a Debian package

    sudo apt install build-essential debhelper libx11-dev libxrender-dev libxcomposite-dev libxdamage-dev
    dpkg-buildpackage -us -uc -b
    sudo dpkg -i ../x11-magnifier_*.deb

## Build / run without packaging

    sudo apt install build-essential libx11-dev libxrender-dev libxcomposite-dev libxdamage-dev
    make && make install          # installs to /usr/local/bin (PREFIX=$HOME/.local for a user install)
    x11-magnifier &                       # e.g. from ~/.icewm/startup

No xbindkeys or icewm key config is needed: x11-magnifier grabs the scroll buttons itself
(icewm's `keys` file can't bind mouse buttons anyway).

## Configuration

Everything below can also live in `~/.config/x11-magnifier/config` (`key = value` lines;
see `x11-magnifier.conf.example`, installed under `/usr/share/doc/x11-magnifier/examples/`).
Keys: `modifiers`, `step`, `max_zoom`, `edge_margin`, `refresh_ms`,
`live`, `nearest`, `invert`. Flags override the file; `-c FILE` picks another file.

## Options

    -m MODS   ctrl+shift (default), super, ctrl+alt, ...
    -c FILE   config file
    -s STEP   zoom factor per scroll tick (default 1.08)
    -M MAX    max zoom (default 12)
    -e PX     edge-pan margin (default 40)
    -r MS     min ms between live refreshes (default 200)
    -p        plain static snapshot, no live refresh
    -n        nearest-neighbour scaling instead of bilinear
    -i        invert direction (natural scrolling)

While running, the chosen modifier+scroll is swallowed system-wide, so
Ctrl+Shift+scroll won't reach other apps. Plain and Ctrl-only scroll are unaffected.

## License

Licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE))
- MIT license ([LICENSE-MIT](LICENSE-MIT))

at your option.
