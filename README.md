# Thunderbird calendars in KDE Plasma's Digital Clock

A read-only integration that exposes calendars configured in Mozilla Thunderbird as a native Plasma 6 `CalendarEvents` provider. It is designed to sit beside KDE's Akonadi/PIM Events provider; it does not modify Plasma's Digital Clock and does not require Akonadi.

**Status: early prototype (0.1.0).** The core build and event-export path have been smoke-tested with Thunderbird and a Plasma plugin test harness, but the project has not been validated across distributions or full Plasma 6 desktop sessions. Treat it as development software, not a stable release. See [Known limitations](#known-limitations).

## Architecture

```text
Thunderbird MailExtension
  ├─ Experiment API reads cal.manager / calICalendar
  ├─ serialises calendars and event occurrences
  └─ runtime.connectNative()
           │ Mozilla native-messaging framing (JSON)
           ▼
Native messaging host (C++ / Qt)
  ├─ validates a snapshot-only message
  ├─ atomically writes ~/.cache/plasma-thunderbird-calendar/events.json
  └─ emits org.kde.ThunderbirdCalendar.EventsChanged on the session bus
           │ cache file + D-Bus notification
           ▼
Plasma CalendarEvents plugin (C++ / Qt)
  ├─ implements CalendarEvents::CalendarEventsPlugin
  └─ publishes CalendarEvents::EventData to Digital Clock
```

The native host is **not a permanently-running daemon**. Thunderbird launches it when the add-on opens its native messaging connection. It stays alive for the duration of that connection and exits when Thunderbird closes. Plasma reads the last cache written by Thunderbird, so already-synced events remain visible when Thunderbird is not running. The cache is refreshed by the extension every three minutes.

The XPI build helper writes ZIP entries with timestamps clamped to the ZIP format's supported range. This matters for Nix builds, where source files commonly have the Unix epoch timestamp (1970-01-01), which ZIP cannot represent. The helper also honours `SOURCE_DATE_EPOCH` for reproducible XPI output.

The bridge only accepts a snapshot message; it does not accept file paths, shell commands or arbitrary method names from the extension. The host manifest restricts access to this add-on's exact extension ID.

## What is included

- `extension/`: Thunderbird MV2 MailExtension plus a narrow Experiment API for reading calendar data.
- `src/nativehost.cpp`: native-messaging host, atomic cache writer and D-Bus notifier.
- `src/calendareventplugin.*`: Plasma CalendarEvents plugin discovered as `thunderbirdcalendar`.
- `src/calendarconfigmodel.*` and `ThunderbirdCalendarConfig.qml`: per-calendar selection in the Digital Clock's Calendar settings.
- `flake.nix` / `nix/package.nix`: Nix build for NixOS. The flake also exports `nixosModules.default` for use as a flake module.

The integration is read-only. Event editing and two-way synchronisation are deliberately out of scope for this first version.

## Build and install without Nix

The project uses CMake and Ninja. It requires a C++20 compiler, Qt 6.5 or newer,
KDE Frameworks 6, Extra CMake Modules (ECM) 6.0 or newer, and Python 3.

The QML settings module explicitly adds `src/` to its private include path. Qt's
QML type registrar compiles a generated C++ translation unit which must be able
to include `calendarconfigmodel.h`; without that include path, the generated
registration code fails with `CalendarConfigModel was not declared` even though
the model's own source file compiles.

The KDE dependencies are:

- **KCoreAddons** — KPlugin metadata and plugin support.
- **KDeclarative** — includes the `CalendarEvents` plugin API used by Plasma's Digital Clock.
- **KConfig** — stores the calendar-selection settings.
- **Qt Base and Qt Declarative** — Qt Core, D-Bus, QML and the QML tooling.

For example, on Arch Linux (including an Arch-based development container):

```sh
sudo pacman -S --needed \
  base-devel cmake ninja extra-cmake-modules python \
  qt6-base qt6-declarative kcoreaddons kdeclarative kconfig
```

On Debian-family distributions with Qt 6 and KF6 development packages available:

```sh
sudo apt install \
  build-essential cmake ninja-build extra-cmake-modules python3 \
  qt6-base-dev qt6-declarative-dev \
  libkf6coreaddons-dev libkf6declarative-dev libkf6config-dev
```

Package names can differ between distribution releases. In particular, use the
Qt 6 / KF6 development packages, not the older Qt 5 / KF5 equivalents.

### Compile and install system-wide

From the repository root:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
sudo cmake --install build
```

The install includes the Plasma CalendarEvents plugin, its QML settings module,
the native-messaging host, a Mozilla native-host manifest, and the Thunderbird
XPI.

For a normal (non-Flatpak) Thunderbird installation, register the native host
manifest for your user. The system install places the manifest at
`/usr/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json`.
Run the following as your normal user, **not** with `sudo`:

```sh
mkdir -p ~/.mozilla/native-messaging-hosts
ln -sf /usr/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json \
  ~/.mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json
```

This creates the per-user manifest link at:

```text
~/.mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json
```

The manifest points to the native host installed under `/usr`; do not move the
executable independently of the package. If the manifest was installed
elsewhere, locate it with:

```sh
find /usr/share -name org.kde.plasma.thunderbird_calendar.json -print
```

Install the extension in Thunderbird using **Add-ons and Themes → Extensions →
gear menu → Install Add-on From File**, selecting:

```text
/usr/share/thunderbird-plasma-calendar/thunderbird-plasma-calendar.xpi
```

The exact data directory follows the CMake install prefix and distribution's
`datadir` convention. To locate the XPI after installation, use:

```sh
find /usr/share -name thunderbird-plasma-calendar.xpi -print
```

Thunderbird Experiment APIs can access internal application interfaces.
Thunderbird therefore asks for the add-on's **full, unrestricted access to
Thunderbird and your computer** permission. This is expected for this prototype:
Thunderbird does not currently expose a supported calendar API to ordinary
MailExtensions. Only install the add-on if you trust the source.

The generated XPI is unsigned development output. Thunderbird's signing policy
differs between distribution builds and channels. If your build rejects the
XPI, check its extension-signing policy; do not change security settings in your
everyday profile without understanding the consequences.

Restart Thunderbird after installing the extension and registering the native
host manifest. Wait for the initial calendar refresh (the extension requests a
180-day look-behind and 730-day look-ahead window).

### Install into a user prefix instead

A user-local installation can be built without root access:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel
cmake --install build
```

Register the native host manifest for your user:

```sh
mkdir -p ~/.mozilla/native-messaging-hosts
ln -sf "$HOME/.local/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json" \
  ~/.mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json
```

Qt does not necessarily search a custom user prefix for plugins automatically.
If Plasma does not discover the plugin, inspect the installed plugin directory
and add its Qt plugin root to `QT_PLUGIN_PATH` for the Plasma session. The root
is the directory containing `plasmacalendarplugins/`, not the plugin directory
itself. For example, if the plugin is installed in
`~/.local/lib/qt6/plugins/plasmacalendarplugins/`, the Qt plugin root is
`~/.local/lib/qt6/plugins/`.

## Build on NixOS

From this repository:

```sh
nix build
```

The Nix expression uses `kdePackages.extra-cmake-modules` (the Qt 6 / KF6
package); the removed top-level `extra-cmake-modules` alias must not be used.
It also includes `qt6.wrapQtAppsHook` because this derivation installs both a
Qt-based native-messaging executable and a Plasma plugin. The hook wraps the
native host executable so Qt can locate its runtime plugins and QML modules;
the Plasma plugin shared library itself is not wrapped and is loaded by the
already-running Plasma process.

The result contains:

- the `thunderbirdcalendar` shared library under Qt 6's
  `plasmacalendarplugins/` plugin directory;
- `bin/thunderbird-calendar-native-host`;
- `share/thunderbird-plasma-calendar/thunderbird-plasma-calendar.xpi`;
- `share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json`.

Add the output to your NixOS system packages or install it into a user profile.
To use the exported NixOS module, add this flake to your inputs and import
`thunderbird-plasma-calendar.nixosModules.default`; then set
`programs.thunderbird-plasma-calendar.enable = true;`. For Flatpak Thunderbird,
also set `programs.thunderbird-plasma-calendar.flatpakProxy.enable = true;` as
described below. The flake also exports the proxy itself as
`packages.<system>.xdg-native-messaging-proxy`.

Register the native-messaging manifest for your user. When using the result of
`nix build` from this repository:

```sh
mkdir -p ~/.mozilla/native-messaging-hosts
ln -sf "$PWD/result/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json" \
  ~/.mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json
```

If the package is installed in a profile instead, link the copy under
`~/.nix-profile/share/mozilla/native-messaging-hosts/` (or
`/run/current-system/sw/share/mozilla/native-messaging-hosts/` for a system
installation) using the same command. The manifest itself points at the
executable in the Nix store, so don't move the executable independently of the
package.

To install the add-on, open Thunderbird → Add-ons and Themes → Extensions →
gear menu → Install Add-on From File, then select the XPI from the Nix result:

```text
result/share/thunderbird-plasma-calendar/thunderbird-plasma-calendar.xpi
```

Restart Thunderbird after installing the extension and native host manifest.

## Flatpak Thunderbird

A Flatpak Thunderbird runs inside a sandbox. It cannot see the host's
`~/.mozilla/native-messaging-hosts` directory or execute a host binary directly.
The supported approach is to run `xdg-native-messaging-proxy` on the host;
Thunderbird talks to it over the session D-Bus.

The proxy is supplied by
[`flatpak/xdg-native-messaging-proxy`](https://github.com/flatpak/xdg-native-messaging-proxy).
Its upstream README warns that exposing native messaging hosts to a sandbox can
weaken the sandbox boundary. This project limits the exposure to one manifest
with the add-on's exact `allowed_extensions` ID and a host protocol that only
accepts calendar snapshots.

There are four requirements:

1. Thunderbird must contain Gecko's native-messaging-proxy client.
2. `xdg-native-messaging-proxy` must be installed on the host.
3. The Flatpak must be allowed to talk to
   `org.freedesktop.NativeMessagingProxy`.
4. `widget.use-xdg-desktop-portal.native-messaging-proxy` must be enabled in
   Thunderbird.

**Version check (as of 17 November 2026):** Flathub stable is Thunderbird 160.0
under the application ID `org.mozilla.thunderbird`, which includes the proxy
client. Flathub ESR is Thunderbird 153.7.0esr under
`org.mozilla.thunderbird_esr`; it does not yet include the client. Verify with
`flatpak list --app` and Help → About. Mozilla documents the channel IDs in its
[channel guide](https://support.mozilla.org/en-US/kb/channel-choices-thunderbird-snap-and-flatpak).

Do **not** copy the native-host manifest into
`~/.var/app/org.mozilla.thunderbird/.mozilla/`. That is the app's private data
directory on the host, not the sandbox's `$HOME/.mozilla`, and the proxy does not
read it.

### Typical Linux distributions

This covers non-NixOS systems where the proxy comes from the distribution, the
AUR, or a local build.

#### 1. Install the proxy on the host

Arch Linux (AUR):

```sh
paru -S xdg-native-messaging-proxy
systemctl --user daemon-reload
```

Fedora Silverblue and other rpm-ostree systems:

```sh
rpm-ostree install xdg-native-messaging-proxy
```

If your distribution does not package it, build it from source using its Meson
build instructions. The runtime dependencies are GLib/GIO, JSON-GLib and
libdex; the build dependencies include Meson, Ninja and pkg-config.

The package installs a D-Bus-activated service, so it does not need to run
permanently. After installation, check that it is available or already running:

```sh
busctl --user list | grep NativeMessagingProxy
```

An entry marked `(activatable)` is sufficient; the proxy starts on the first
request from Thunderbird.

#### 2. Make the native-host manifest available to the proxy

The project installs the manifest under `share/mozilla/native-messaging-hosts`.
That is not one of Thunderbird's or the proxy's standard search locations, so
create a link in one of the locations they do search. The examples below assume
a system-wide install under `/usr`; for a user-prefix install, substitute
`$HOME/.local/share/mozilla/native-messaging-hosts` as the source path.

Recommended for a system-wide project install, and avoids creating `~/.mozilla`:

```sh
sudo mkdir -p /usr/lib/mozilla/native-messaging-hosts
sudo ln -sf /usr/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json \
  /usr/lib/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json
```

On distributions that use `/usr/lib64`, use
`/usr/lib64/mozilla/native-messaging-hosts` instead. This location is searched by
normal Thunderbird and by `xdg-native-messaging-proxy`.

For a Flatpak-only setup where you do not want a system-wide link, the proxy also
searches `$XDG_CONFIG_HOME/mozilla/native-messaging-hosts`, normally
`~/.config/mozilla/native-messaging-hosts`:

```sh
mkdir -p ~/.config/mozilla/native-messaging-hosts
ln -sf /usr/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json \
  ~/.config/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json
```

The `~/.config` location is used by the Flatpak proxy, but not by a normal
non-Flatpak Thunderbird. If you use both, prefer the system-wide location or use
`~/.mozilla/native-messaging-hosts`, which is searched by both.

If you previously copied the manifest into the Flatpak data directory, remove it:

```sh
rm -rf ~/.var/app/org.mozilla.thunderbird/.mozilla
```

#### 3. Grant the Flatpak D-Bus permission

Use the application ID reported by `flatpak list --app`:

```sh
flatpak override --user \
  --talk-name=org.freedesktop.NativeMessagingProxy \
  org.mozilla.thunderbird
```

For the ESR ID, substitute `org.mozilla.thunderbird_esr` once that channel gains
proxy support. Verify the override with:

```sh
flatpak info --show-permissions org.mozilla.thunderbird
```

#### 4. Enable the Gecko preference

In Thunderbird's `about:config`, create this **integer** preference:

```text
widget.use-xdg-desktop-portal.native-messaging-proxy = 2
```

A value of `2` asks Gecko to auto-detect the proxy. Gecko's built-in default is
`0` (“never”), so this preference is required.

#### 5. Install the extension and restart

Install the XPI using **Add-ons and Themes → Extensions → gear menu → Install
Add-on From File**. For a system install, the file is normally:

```text
/usr/share/thunderbird-plasma-calendar/thunderbird-plasma-calendar.xpi
```

Use `find /usr/share -name thunderbird-plasma-calendar.xpi -print` if the path
differs. If the Flatpak file chooser cannot see the file, copy it to your
Downloads directory first.

Restart Thunderbird completely:

```sh
flatpak kill org.mozilla.thunderbird
flatpak run org.mozilla.thunderbird
```

Then check the proxy and the extension:

```sh
busctl --user list | grep NativeMessagingProxy
journalctl --user -u xdg-native-messaging-proxy -n 50
```

The proxy should log a `GetManifest` or `Start` request for
`org.kde.plasma.thunderbird_calendar` when the add-on connects.

### NixOS

Nixpkgs does not currently package `xdg-native-messaging-proxy`. This flake
packages it and can run it as a systemd user service, so no host manifest inside
`~/.mozilla` or `/usr/lib` is needed.

Add this flake as an input and import its NixOS module as described in
[Build on NixOS](#build-on-nixos), then enable the proxy option:

```nix
services.flatpak.enable = true;

programs.thunderbird-plasma-calendar = {
  enable = true;
  flatpakProxy.enable = true;
};
```

The `flatpakProxy` option:

- installs the `xdg-native-messaging-proxy` package built by this flake;
- starts `xdg-native-messaging-proxy.service` in the user session;
- sets `XNMP_HOST_LOCATIONS` to the project's manifest directory in the Nix
  store.

Apply the configuration and verify the service:

```sh
sudo nixos-rebuild switch
systemctl --user status xdg-native-messaging-proxy
busctl --user list | grep NativeMessagingProxy
```

Then apply the Flatpak override and Gecko preference from steps 3 and 4 above.
The app ID is still reported by `flatpak list --app`; the stable channel is
currently `org.mozilla.thunderbird`.

Install the XPI from the system profile:

```text
/run/current-system/sw/share/thunderbird-plasma-calendar/thunderbird-plasma-calendar.xpi
```

Restart Thunderbird and inspect the proxy logs with:

```sh
journalctl --user -u xdg-native-messaging-proxy -n 50
```

If you do not use the NixOS module, `nix build .#xdg-native-messaging-proxy`
builds the proxy directly. You must then run it in your user session with
`XNMP_HOST_LOCATIONS` pointing at a directory containing this project's manifest:

```sh
proxy=$(nix build .#xdg-native-messaging-proxy --print-out-paths)
manifests=$(nix build .#default --print-out-paths)/share/mozilla/native-messaging-hosts
XNMP_HOST_LOCATIONS="$manifests" "$proxy/libexec/xdg-native-messaging-proxy"
```

That command runs in the foreground; for normal use, wrap it in a systemd user
service or use the NixOS module.

## Troubleshooting

- **No events appear:** check Thunderbird's Error Console for `Thunderbird Plasma Calendar` messages. Confirm the add-on can connect to `org.kde.plasma.thunderbird_calendar` and that the user cache at `$XDG_CACHE_HOME/plasma-thunderbird-calendar/events.json` (normally `~/.cache/plasma-thunderbird-calendar/events.json`) exists.
- **No calendar list in plugin settings:** start Thunderbird, wait for a refresh, then reopen the settings or press **Refresh list**.
- **Native host not found:** make sure the manifest is in a location the relevant client searches: `/usr/lib/mozilla/native-messaging-hosts` or `/usr/lib64/mozilla/native-messaging-hosts` system-wide, `~/.config/mozilla/native-messaging-hosts` for the Flatpak proxy, or `~/.mozilla/native-messaging-hosts` for normal Thunderbird or a per-user setup. Inspect the manifest; its `path` must exist and be executable.
- **Flatpak cannot connect:** ensure the host has `xdg-native-messaging-proxy` installed (on Arch: `paru -S xdg-native-messaging-proxy`; on NixOS enable `flatpakProxy`), the manifest is in a searched host location, the Flatpak has `--talk-name=org.freedesktop.NativeMessagingProxy`, and `widget.use-xdg-desktop-portal.native-messaging-proxy` is set to `2` in Thunderbird. Verify with `busctl --user list | grep NativeMessagingProxy`, `flatpak info --show-permissions org.mozilla.thunderbird`, and `flatpak list --app`. A manifest under `~/.var/app/org.mozilla.thunderbird/.mozilla/` does not work; that path is not the sandbox's `$HOME/.mozilla`.
- **Flatpak proxy not running:** start it with `systemctl --user start xdg-native-messaging-proxy` (or let D-Bus activate it), then inspect `systemctl --user status xdg-native-messaging-proxy` and `journalctl --user -u xdg-native-messaging-proxy -n 50`. On NixOS, also confirm `programs.thunderbird-plasma-calendar.flatpakProxy.enable = true;` and run `nixos-rebuild switch`.
- **Plugin is not listed:** verify that the `.so` is installed in a Qt 6 `plasmacalendarplugins` directory and restart Plasma.

## Known limitations

- This is an initial prototype. The core build and event-export path have been smoke-tested, but the project has not been validated across Plasma and Thunderbird versions or distributions.
- The extension uses Thunderbird's Experiment API because a supported built-in MailExtension calendar API is not available. Internal Thunderbird interfaces can change between major releases.
- The calendar snapshot is refreshed every three minutes; updates are not event-pushed yet.
- The snapshot covers 180 days in the past and 730 days in the future. Events outside that window are not available to Plasma until the window is extended.
- The bridge is read-only. It does not create, edit, delete or RSVP to events.
- The current cache contains event titles/descriptions and is stored unencrypted in the user cache. The host restricts the cache directory and file to the current user (0700/0600-equivalent permissions). Consider this before enabling calendars containing sensitive data.
- Tasks are mapped to Plasma's Todo event type on a best-effort basis. Reminder/alarm data, attendees, attachments, recurrence metadata and advanced scheduling details are not exposed to the clock.
- Some Thunderbird calendar providers may reject broad range queries or behave differently with recurring events; those providers need testing.
- The QML settings UI and native messaging proxy integration need validation against the exact Plasma and Thunderbird versions being used.

## Development

On NixOS, enter the development shell with `nix develop`. On other distributions,
install the dependencies listed above. Then use:

```sh
cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX="$PWD/_install"
cmake --build build --parallel
cmake --install build
```

For a native install, the host is launched by Thunderbird through native
messaging. It expects Mozilla's 4-byte-length-prefixed JSON protocol on
stdin/stdout. Do not launch it from a terminal to inspect its output because
stdout is reserved for that protocol.

## Licensing

GPL-2.0-or-later. C++ and JavaScript source files include SPDX identifiers.
