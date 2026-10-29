# GnuChanSettings

GnuChanSettings is the control panel: one window from which the machine's
settings are read and changed. It is written in C against Xlib, Xft and freetype
for the window and the text, with Xrender for compositing. It is Debian-only, on
purpose.

## Building

    git clone <repository-url> GnuChanSettings
    cd GnuChanSettings/src
    python3 makefile.py

The makefile installs the build dependencies with apt-get, compiles the sources,
and re-runs itself through sudo to write into `/usr/local/bin`. The binary goes
to `/usr/local/bin/GnuChanSettings`; a desktop entry is written to
`/usr/share/applications/gnuchansettings.desktop`.
The install also provides a low-battery monitor and a privileged helper used
by the System category. In a GnuChanWM session the monitor starts automatically
and sends alerts through the session notification service.

The System category configures the low-battery alert threshold (10% by default),
TLP CPU profiles, a battery charge cap in Battery protection mode, the lid
sleep action (suspend or hibernate), and English/Turkish locale preferences.
Battery charge thresholds are applied only on hardware supported by TLP.
System changes requiring root use pkexec. Lid and locale changes take effect
after the next login or reboot; TLP changes are applied when saved. Language
choices configure system and per-user locales; applications without translated
message catalogs continue to display their existing strings.

The GnuChanRunner category has a `Run in GnuChanTerm` list. Use `+` to add a
program name or executable name and `-` to remove the last entry; selected
programs open in GnuChanTerm when launched from Runner. The separate Autostart
category edits GnuChanWM's login command list, including commands with
arguments, and saves it as `gcl_autostart.all`.

The makefile takes an action as its first argument: `install` (the default),
`build`, `run` and `uninstall`.

## Dependencies

The makefile probes for these headers and installs the matching package when one
is missing:

- `build-essential` (gcc)
- `pkg-config`
- `libx11-dev` — `X11/Xlib.h`
- `libxft-dev` — `X11/Xft/Xft.h`
- `libfreetype-dev` — `ft2build.h`
- `libxrender-dev` — `X11/extensions/Xrender.h`
- `libnotify-bin`, `locales`, `tlp`, `pkexec` and `mate-polkit` for the
  system settings and battery notification features

The linking is driven by pkg-config for the modules `x11`, `xft`, `xrender`,
`fontconfig` and `freetype2`.

## X11 only

This program is built directly against X11 (Xlib, Xft and the rest of the X
stack). It is designed for an X11 session and does not support Wayland: it was
never tested there, its window management assumes an X server, and no work has
been done to port it. On a Wayland session it may run through XWayland for basic
use, but that is not a supported configuration and behaviour there is undefined.
Run it on X11.

## License

GPL3.
