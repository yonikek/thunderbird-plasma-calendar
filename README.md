# Thunderbird calendars in KDE Plasma's Digital Clock

<img width="2085" height="1011" alt="Screenshot_20261118_000610" src="https://github.com/user-attachments/assets/3ae24623-3175-4b41-9679-12a62800785b" />


A read-only integration that shows calendars configured in Mozilla Thunderbird in
the Plasma 6 Digital Clock, as a `CalendarEvents` provider. It sits beside KDE's
Akonadi/PIM Events provider; it does not modify Plasma and does not need Akonadi.

**Status: early prototype (0.1.0).** The build and event-export path have been
smoke-tested with Thunderbird and a Plasma plugin test harness, but not across
distributions or full desktop sessions. See [Known limitations](#known-limitations).

## How it works

```text
Thunderbird MailExtension
  ├─ Experiment API reads cal.manager / calICalendar
  └─ runtime.connectNative()  ── Mozilla native-messaging framing (JSON)
           ▼
Native messaging host (C++ / Qt)
  ├─ validates a snapshot-only message
  ├─ atomically writes ~/.cache/plasma-thunderbird-calendar/events.json
  └─ emits org.kde.ThunderbirdCalendar.EventsChanged on the session bus
           ▼
Plasma CalendarEvents plugin (C++ / Qt)
  └─ publishes events to the Digital Clock
```

- The host is not a daemon. Thunderbird starts it when the add-on connects and it
  exits when Thunderbird closes. Plasma reads the last cache written, so synced
  events stay visible while Thunderbird is closed.
- The Experiment API observes Thunderbird's calendar manager and notifies the
  extension when calendars or items change. The extension debounces bursts (such
  as a CalDAV sync): it refreshes after 2.5 seconds of quiet, and at most 30
  seconds after the first change. A 15-minute poll remains as a safety net.
  The exported window defaults to 90 days back and 360 days ahead and can be
  changed in the add-on's options (see [Options](#options)).
- The host skips the cache write and the D-Bus signal when a snapshot differs
  from the cached one only in timestamps, so Plasma does not re-parse the file
  needlessly.
- If the host keeps failing, reconnect attempts back off exponentially from 15
  seconds up to 15 minutes.
- The host accepts only a `snapshot` message: no paths, commands or method names.
  The host manifest allows only this add-on's exact extension ID.
- The XPI builder clamps ZIP timestamps to the supported range (Nix sources use
  the 1970 epoch) and honours `SOURCE_DATE_EPOCH`.

Repository layout:

| Path | Purpose |
| --- | --- |
| `extension/` | MV2 MailExtension, options page and a narrow Experiment API (kept in a private closure) |
| `src/nativehost.cpp` | native-messaging host, cache writer, D-Bus notifier |
| `src/calendareventplugin.*` | Plasma plugin (`thunderbirdcalendar`) |
| `src/calendarconfigmodel.*`, `ThunderbirdCalendarConfig.qml` | per-calendar selection in Calendar settings |
| `nix/`, `flake.nix` | Nix package, NixOS module, `xdg-native-messaging-proxy` package |

## Install

Requirements: C++20 compiler, Qt 6.5+, KDE Frameworks 6 (KCoreAddons, KDeclarative,
KConfig), Extra CMake Modules 6.0+, CMake, Ninja, Python 3. Use Qt 6 / KF6
packages, not Qt 5 / KF5.

```sh
# Arch
sudo pacman -S --needed base-devel cmake ninja extra-cmake-modules python \
  qt6-base qt6-declarative kcoreaddons kdeclarative kconfig
# Debian family (names vary by release)
sudo apt install build-essential cmake ninja-build extra-cmake-modules python3 \
  qt6-base-dev qt6-declarative-dev \
  libkf6coreaddons-dev libkf6declarative-dev libkf6config-dev
```

### System-wide

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
sudo cmake --install build
```

### User prefix

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel
cmake --install build
```

Qt may not search a custom prefix for plugins. If Plasma does not find the plugin,
add the directory *containing* `plasmacalendarplugins/` (for example
`~/.local/lib/qt6/plugins`) to `QT_PLUGIN_PATH` for the Plasma session.

### Register the host and install the add-on (native Thunderbird)

Run as your normal user, not with `sudo`. For a user-prefix install, replace
`/usr` with `$HOME/.local`:

```sh
mkdir -p ~/.mozilla/native-messaging-hosts
ln -sf /usr/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json \
  ~/.mozilla/native-messaging-hosts/
```

The manifest embeds the absolute path of the host executable, so do not move the
executable separately from the package. Then in Thunderbird choose **Add-ons and
Themes → Extensions → gear menu → Install Add-on From File** and select
`/usr/share/thunderbird-plasma-calendar/thunderbird-plasma-calendar.xpi`
(`find /usr/share -name thunderbird-plasma-calendar.xpi` if it differs). Restart
Thunderbird and wait for the first refresh.

Thunderbird will ask for **full, unrestricted access to Thunderbird and your
computer**. That is inherent to Experiment APIs: there is no supported calendar
API for ordinary MailExtensions. Only install the add-on if you trust the source.

The XPI is **unsigned**. It installed on the Flathub release build in testing, but
whether unsigned permanent installs are accepted may depend on the build. If the
add-on is disabled or disappears after a restart, see Troubleshooting.

## NixOS

```sh
nix build
```

The result contains the plugin, `bin/thunderbird-calendar-native-host`, the host
manifest under `share/mozilla/native-messaging-hosts/` and the XPI under
`share/thunderbird-plasma-calendar/`. The derivation uses
`kdePackages.extra-cmake-modules` and `qt6.wrapQtAppsHook` (the hook wraps the
host executable; the plugin is loaded by the running Plasma process).

With the flake's NixOS module (`thunderbird-plasma-calendar.nixosModules.default`):

```nix
programs.thunderbird-plasma-calendar.enable = true;
```

This installs the package and links `/share/mozilla/native-messaging-hosts` and
`/share/thunderbird-plasma-calendar` into the system profile. For **native**
Thunderbird, register the manifest as above, using
`/run/current-system/sw/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json`
as the link source (or `result/share/…` after a plain `nix build`), and install
`/run/current-system/sw/share/thunderbird-plasma-calendar/thunderbird-plasma-calendar.xpi`.

For **Flatpak** Thunderbird see the next section.

## Flatpak Thunderbird

A Flatpak Thunderbird cannot see host manifests or run host binaries. It talks to
[`xdg-native-messaging-proxy`](https://github.com/flatpak/xdg-native-messaging-proxy)
on the host over the session D-Bus. The proxy's upstream README warns that this
can weaken the sandbox boundary; this project limits the exposure to one manifest
pinned to the add-on's ID and a host that accepts only calendar snapshots.

Requirements:

1. A Thunderbird build that includes Gecko's proxy client. At the time of writing
   the Flathub stable channel (`org.mozilla.thunderbird`) does; the ESR channel
   (`org.mozilla.thunderbird_esr`) did not. Check with `flatpak list --app` and
   **Help → About**.
2. `xdg-native-messaging-proxy` running on the host.
3. A host manifest the proxy can find (see below).
4. Permission for the Flatpak to talk to `org.freedesktop.NativeMessagingProxy`.
5. The Gecko preference enabled.

### 1. Proxy and manifest

**Most distributions.** Install the proxy from your distribution/AUR, or build it
(Meson; needs GLib/GIO, JSON-GLib, libdex). It is D-Bus activated, so it does not
need to run permanently. `busctl --user list | grep NativeMessagingProxy` should
show it, marked `(activatable)` if idle.

The proxy searches `~/.mozilla/native-messaging-hosts`,
`$XDG_CONFIG_HOME/mozilla/native-messaging-hosts` (normally `~/.config/…`) and the
`/usr/lib{,64}/mozilla/native-messaging-hosts` directories. The project installs
its manifest under `share/mozilla/…`, which is **not** one of them, so link it
into one. Recommended for a system install:

```sh
sudo mkdir -p /usr/lib/mozilla/native-messaging-hosts
sudo ln -sf /usr/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json \
  /usr/lib/mozilla/native-messaging-hosts/
```

(Use `/usr/lib64` where applicable.) Without a system-wide link, use
`~/.mozilla/native-messaging-hosts` (also searched by native Thunderbird) or
`~/.config/mozilla/native-messaging-hosts` (proxy only).

Do **not** copy the manifest into the Flatpak's data directory under
`~/.var/app/org.mozilla.thunderbird/`: the proxy does not read it, and the host
binary would not be reachable from inside the sandbox anyway.

**NixOS.** Nixpkgs does not package the proxy (check whether that has changed);
this flake does. Enable the module option:

```nix
services.flatpak.enable = true;
programs.thunderbird-plasma-calendar = {
  enable = true;
  flatpakProxy.enable = true;
  # flatpakProxy.extraHostLocations = [ "/some/other/manifest/dir" ];
};
```

This builds the proxy, starts `xdg-native-messaging-proxy.service` as a user
service, and sets `XNMP_HOST_LOCATIONS`. That variable **replaces** the proxy's
default search path, so the module lists `~/.mozilla/native-messaging-hosts`,
`$XDG_CONFIG_HOME/mozilla/native-messaging-hosts`, the system profile and this
package's manifest directory itself; add anything else with `extraHostLocations`.
No manual manifest link is needed. Check it:

```sh
sudo nixos-rebuild switch
systemctl --user status xdg-native-messaging-proxy
busctl --user list | grep NativeMessagingProxy
```

Without the module, run the proxy yourself in the user session:

```sh
proxy=$(nix build .#xdg-native-messaging-proxy --print-out-paths)
manifests=$(nix build .#default --print-out-paths)/share/mozilla/native-messaging-hosts
XNMP_HOST_LOCATIONS="$manifests" "$proxy/libexec/xdg-native-messaging-proxy"
```

### 2. Flatpak permission

```sh
flatpak override --user --talk-name=org.freedesktop.NativeMessagingProxy \
  org.mozilla.thunderbird
flatpak info --show-permissions org.mozilla.thunderbird
```

### 3. Gecko preference

In Thunderbird's `about:config`, create an **integer** preference:

```text
widget.use-xdg-desktop-portal.native-messaging-proxy = 2
```

`2` means auto-detect; the built-in default `0` disables the proxy.

### 4. Install the add-on and restart

Install the XPI as above (system path, or
`/run/current-system/sw/share/thunderbird-plasma-calendar/thunderbird-plasma-calendar.xpi`
on NixOS). If the Flatpak file chooser cannot see it, copy it to `~/Downloads`
first. Then restart fully:

```sh
flatpak kill org.mozilla.thunderbird
flatpak run org.mozilla.thunderbird
journalctl --user -u xdg-native-messaging-proxy -n 50
```

The proxy log should show a `GetManifest` or `Start` request for
`org.kde.plasma.thunderbird_calendar` when the add-on connects.

## Options

Open **Add-ons and Themes**, find *Thunderbird Calendar for KDE Plasma* and choose
**Options** (or **Preferences**). You can set:

- how many days into the past (0 to 365, default 90) and the future (1 to 1095,
  default 360) are exported;
- whether event descriptions are exported (default on). Descriptions longer than
  300 characters are truncated with an ellipsis; line breaks and spacing are kept;
- whether event locations are exported (default off, truncated at 200 characters).
  The plugin does not display locations yet, since current Plasma versions have no
  place for them.

Values are saved automatically, out-of-range values are adjusted, and a change
reaches Plasma within a few seconds. Large windows increase memory use and
snapshot size; the host rejects snapshots over 32 MiB. Turning descriptions and
locations off makes refreshes lighter, because they are then not read from
Thunderbird at all.

## Troubleshooting

- **No events:** check Thunderbird's Error Console for `Thunderbird Plasma
  Calendar` messages, and that `$XDG_CACHE_HOME/plasma-thunderbird-calendar/events.json`
  (normally under `~/.cache`) exists.
- **"snapshot too large":** the host rejects snapshots over 32 MiB. Disable
  calendars with very many recurring events or shorten the window in
  `extension/background.js`.
- **No calendars in plugin settings:** start Thunderbird, wait for a refresh, then
  reopen the settings or press **Refresh list**.
- **Native host not found:** the manifest must be in a directory the relevant
  client searches (see above) and its `path` must exist and be executable.
- **Flatpak cannot connect:** check, in order, that the proxy is running
  (`busctl --user list | grep NativeMessagingProxy`), the manifest is in a
  searched location (on NixOS: `systemctl --user cat xdg-native-messaging-proxy`
  and look at `XNMP_HOST_LOCATIONS`), the override is applied, the Gecko
  preference is `2`, and the proxy journal.
- **The add-on disappears after restarting Thunderbird:** quit with File → Quit
  rather than `flatpak kill`, and wait a few seconds after installing before
  closing, because Thunderbird writes its add-on registry shortly after a
  change. Check `about:profiles` that you are using the same profile each time.
- **Plugin not listed in Plasma:** verify the `.so` is in a Qt 6
  `plasmacalendarplugins` directory and restart Plasma.

## Known limitations

- Prototype: not validated across Plasma/Thunderbird versions or distributions;
  the QML settings UI and proxy integration need testing against your versions.
- The Experiment API uses internal Thunderbird interfaces that can change between
  major releases.
- Thunderbird has signalled that it intends to stop honouring Experiment APIs on
  release builds (gated by `extensions.experiments.suppressed`, reportedly
  targeting the 2027 ESR). This project depends on Experiments to read the
  calendar, so check the Thunderbird release notes before upgrading.
- Updates are as fast as Thunderbird's own: local edits appear within seconds, but
  changes on a server appear only when Thunderbird next syncs that calendar.
  Thunderbird must be running.
- Only events inside the configured window are exported (default 90 days back, 360 days ahead).
- Read-only: no creating, editing, deleting or RSVP.
- The cache holds event titles and descriptions **unencrypted** in the user cache
  (directory 0700, file 0600). Consider this for calendars with sensitive data.
- Tasks are mapped to Plasma's Todo type on a best-effort basis; alarms,
  attendees, attachments and recurrence metadata are not exposed.
- Once you save a calendar selection, calendars added to Thunderbird later stay
  hidden until you enable them in the settings.
- Some calendar providers may reject broad range queries or behave differently
  with recurring events.

## Development

On NixOS: `nix develop`. Elsewhere install the dependencies above, then:

```sh
cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX="$PWD/_install"
cmake --build build --parallel
cmake --install build
```

The host speaks Mozilla's length-prefixed JSON protocol on stdin/stdout, so do not
run it from a terminal to inspect output.

## License

GPL-2.0-or-later. C++, QML and JavaScript sources carry SPDX identifiers.
