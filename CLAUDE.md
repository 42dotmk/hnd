# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

hnd is a single-file X11 notification daemon (`hnd.c`): it owns `org.freedesktop.Notifications` on the D-Bus session bus (exits if another daemon does) and shows each notification as an override-redirect popup stacked down a screen corner. Text only — icons and body markup are deliberately ignored (markup is stripped, entities decoded). Suckless-style: configuration is the block of `static` values at the top of `hnd.c`, not a config file — changing settings means editing those and recompiling. The only runtime overrides are the dmenu-style width flags `-w <px>` (minimum width) and `-W <pct>` (maximum width as a percent of the monitor, default 50); anything else stays compiled in.

A user D-Bus service file (`~/.local/share/dbus-1/services/org.freedesktop.Notifications.service`, pointing at `~/.local/bin/hnd`) makes the bus auto-start hnd on demand, shadowing the system dunst/xfce4-notifyd activation files.

## Build

```sh
make            # builds ./hnd (needs libX11, libXrandr, dbus-1 headers)
make install    # symlinks it into ~/.local/bin
make clean
```

There are no tests or lint targets; the compiler flags (`-std=c99 -pedantic -Wall -Wextra`) are the lint. Keep the build warning-free.

`vendor/stb_ds.h` provides the dynamic array (`arrput`/`arrdel`/`arrlen`) used for the notification list; libdbus-1 is used directly (low-level API, no GLib).

## Architecture

Everything lives in `hnd.c`, structured as:

- `setup()` requests the bus name with `DBUS_NAME_FLAG_DO_NOT_QUEUE` (dies if taken), then opens the display, allocates the per-urgency color pixels, and loads the core font.
- `run()` is a `select()` loop over the X connection fd and the D-Bus fd, with the timeout set to the nearest notification deadline. D-Bus messages are handled by draining `dbus_connection_pop_message()` after a zero-timeout `read_write` — no watch/dispatch machinery.
- `handlemsg()` routes the spec methods: `Notify` (strict `susssasa{sv}i` signature check, then plain iterator walking), `CloseNotification`, `GetCapabilities` (`body`, `actions`), `GetServerInformation`, plus `Introspect`. `NotificationClosed` and `ActionInvoked` signals are emitted with the spec reason codes (1 expired, 2 dismissed, 3 closed by call).
- Each notification is a `Notif` in a stb_ds array with its own window. Text is wrapped once at `Notify` time (`stripmarkup` → `wrapappend`, greedy per-char `XTextWidth` measuring) into a `'\n'`-separated buffer; the first `sumlines` lines are the summary and draw in a brighter color. Each window is sized to its longest line, between `minwidth` and `maxwpct`% of the monitor width (the wrap limit). Replacement (`replaces_id`) keeps the id but destroys and recreates the window, since size/urgency may change.
- `layout()` re-picks the monitor via RandR (second monitor when there is one, same rule as htray) and slots windows from the top (or bottom, per `atbottom`) right corner; it ends by `XMapRaised`-ing every window, which doubles as restacking.
- Like htray, the daemon re-raises its windows when a `ConfigureNotify`/`MapNotify` on the root shows another window restacking above (guarded so its own windows don't re-trigger it), and the X error handler ignores everything.
- Left click dismisses one notification (emitting `ActionInvoked("default")` first if the sender offered that action), right click dismisses all, `SIGUSR1` (e.g. `pkill -USR1 -x hnd` from a WM keybind) also dismisses all; the handler is installed without `SA_RESTART` on purpose so it interrupts `select`.
- Urgency is the only hint read; per-urgency colors and default timeouts (`defaulttimeout`, 0 = sticky — the default for critical) live in the config block.

## Style

Follows suckless/OpenBSD C style like htray: C99, tabs, return type on its own line, no dynamic allocation beyond stb_ds, fixed-size buffers with `snprintf`/`memcpy`. Match it.
