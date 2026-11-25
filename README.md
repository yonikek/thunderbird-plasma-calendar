# Thunderbird Calendar for KDE Plasma

<img width="2085" height="1011" alt="Thunderbird calendars in the Plasma Digital Clock" src="https://github.com/user-attachments/assets/3ae24623-3175-4b41-9679-12a62800785b" />

A read-only bridge that makes calendars configured in Mozilla Thunderbird available in the KDE Plasma 6 Digital Clock. It uses Plasma's `CalendarEvents` plugin API, appears alongside other calendar providers, and does not require Akonadi or modify Plasma itself.

**Version 0.9.0** · **GPL-2.0-or-later** · Pre-1.0 release

> **Important compatibility and security notes**
>
> - The Thunderbird add-on uses an Experiment API to access Thunderbird's internal calendar interfaces. Thunderbird grants Experiment API add-ons broad, effectively unrestricted access to Thunderbird and your computer. Install it only when you trust the source.
> - Only the latest stable Thunderbird release is tested. Internal calendar APIs can change between releases; older releases, ESR, Beta, Daily/Nightly and downstream-patched builds are not covered by the regular test policy.
> - Some Plasma Workspace versions have an upstream `DaysModel` performance and stability issue when a calendar provider emits many `eventRemoved()` notifications. Changing calendar selection can trigger a large removal burst and cause high memory use or a Plasma Shell crash. See [KDE Bug 526769](https://bugs.kde.org/show_bug.cgi?id=526769) and [KDE Bug 523046](https://bugs.kde.org/show_bug.cgi?id=523046). This project does not patch that Plasma behaviour.

## Features

- Reads calendars and events from Thunderbird and displays them in the Plasma Digital Clock agenda.
- Uses Thunderbird calendar observers for incremental event additions, edits, removals and moves; ordinary changes do not require a complete calendar query.
- Keeps a local snapshot and append-only journal so Plasma can display the last synchronised events while Thunderbird is closed.
- Recovers from interrupted writes, missed D-Bus notifications and native-host restarts.
- Supports floating iCalendar times, UTC timestamps, timezone-qualified times, all-day events, recurring events and recurrence exceptions.
- Allows calendar selection and stale-event visibility to be configured in the Plasma Calendar Events settings.
- Supports native Thunderbird, NixOS packaging and Flatpak Thunderbird through `xdg-native-messaging-proxy`.

## How it works

```text
Thunderbird MailExtension
  ├─ Experiment API reads calendars and event data
  ├─ Calendar observers report add / modify / delete changes
  └─ Native messaging sends length-prefixed JSON
           │
           ▼
Native messaging host (C++ / Qt)
  ├─ validates and persists snapshots and event changes
  ├─ maintains the snapshot and append-only journal
  └─ publishes revisioned event deltas over session D-Bus
           │
           ▼
Plasma CalendarEvents plugin (C++ / Qt)
  ├─ loads and reconciles the persistent cache
  └─ publishes event additions, modifications and removals
           │
           ▼
Plasma Digital Clock agenda
```

### Synchronisation and recovery

The extension performs a full snapshot when it starts, when recovery is needed, after relevant settings or calendar metadata changes, and during a periodic safety refresh. The default export window covers **90 days in the past** and **360 days in the future**. Calendars are queried in 30-day chunks, with up to three calendars queried concurrently. A query timeout or provider error is isolated to that calendar; its last successful events are retained and marked stale instead of being replaced with an empty list.

For ordinary changes, Thunderbird's `onAddItem`, `onModifyItem` and `onDeleteItem` observer callbacks send incremental updates. The extension coalesces changes by calendar ID and event UID. The native host appends a checksummed journal record and synchronises it to disk before acknowledging the update or notifying Plasma.

The journal uses a versioned SHA-256 revision chain, so an individual update does not require the entire calendar snapshot to be serialised and hashed. The host and Plasma plugin independently verify journal records during recovery. The journal is compacted into the snapshot after 128 records or 1 MiB. Legacy version-1 journals remain readable and are migrated by the native host before the next direct update.

The native host is started by Thunderbird through native messaging; it is not a background daemon. Plasma can reconstruct the latest event state from the snapshot and journal while Thunderbird is closed. A per-user lock permits only one active Thunderbird profile to use the integration at a time.

### Event and time handling

- Timed events with UTC or timezone-qualified values are exported as instants and displayed in the system's local timezone.
- Floating iCalendar DATE-TIME values retain their wall-clock fields and are interpreted in the system's local timezone. A floating time inside a local spring-forward gap cannot be represented as a valid local time and is omitted rather than silently shifted. An ambiguous autumn time retains its displayed clock fields.
- All-day events use date-only values and exclusive end dates.
- Multi-day events are published once under their start date; they are not repeated on every date they span.
- Event moves are represented as removal of the old date-specific identity and insertion of the new one.
- Tasks are mapped to Plasma Todo items on a best-effort basis. Alarms, attendees, attachments and full recurrence metadata are not exposed to Plasma.

## Requirements

- Linux and KDE Plasma 6.
- C++20 compiler, CMake 3.21 or newer, Ninja and Python 3.
- Qt 6.5 or newer: Core, DBus and Qml.
- KDE Frameworks 6.0 or newer: KCoreAddons, KDeclarative and KConfig.
- Extra CMake Modules 6.0 or newer.

Use Qt 6 and KF6 packages, not Qt 5 or KF5.

**Thunderbird compatibility:** the add-on manifest declares Thunderbird 128.0 as its minimum install version, but this is not a promise that every intervening release is compatible. The regular test target is the latest stable Thunderbird release only.

### Build dependencies

Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake ninja extra-cmake-modules python \
  qt6-base qt6-declarative kcoreaddons kdeclarative kconfig
```

Debian-family distributions (package names vary by release):

```sh
sudo apt install build-essential cmake ninja-build extra-cmake-modules python3 \
  qt6-base-dev qt6-declarative-dev \
  libkf6coreaddons-dev libkf6declarative-dev libkf6config-dev
```

## Build and install

### Build from source

System-wide installation:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
sudo cmake --install build
```

User-prefix installation:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel
cmake --install build
```

The install contains the Plasma CalendarEvents plugin, the native-messaging host, its Mozilla host manifest and the Thunderbird XPI. If Plasma does not discover a user-prefix plugin, add the directory containing `plasmacalendarplugins/` (for example, `~/.local/lib/qt6/plugins`) to `QT_PLUGIN_PATH` for the Plasma session and restart Plasma.

### Install in native Thunderbird

Register the installed host manifest as your normal user. For a system installation:

```sh
mkdir -p ~/.mozilla/native-messaging-hosts
ln -sf /usr/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json \
  ~/.mozilla/native-messaging-hosts/
```

For a user-prefix installation, replace `/usr` with `$HOME/.local`. The manifest contains the host executable's absolute path; do not move the executable independently of the package.

In Thunderbird, open **Add-ons and Themes → Extensions → gear menu → Install Add-on From File** and select `thunderbird-plasma-calendar.xpi` (normally under `/usr/share/thunderbird-plasma-calendar/`). Restart Thunderbird and allow the initial calendar refresh to finish.

The generated XPI is unsigned. Permanent installation of an unsigned add-on depends on the Thunderbird build and its signing or sideloading policy.

### NixOS

The flake provides a package and a NixOS module. To build the package:

```sh
nix build
```

Enable the module in your NixOS configuration:

```nix
programs.thunderbird-plasma-calendar.enable = true;
```

Apply the configuration with `sudo nixos-rebuild switch`. The package is exposed in the system profile under `/run/current-system/sw/`. For native Thunderbird, register its host manifest:

```sh
mkdir -p ~/.mozilla/native-messaging-hosts
ln -sf /run/current-system/sw/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json \
  ~/.mozilla/native-messaging-hosts/
```

Install the XPI from `/run/current-system/sw/share/thunderbird-plasma-calendar/`.

### Flatpak Thunderbird

Flatpak Thunderbird needs a host-side native-messaging proxy because it cannot directly execute host binaries or read host native-messaging manifests. This project supports [`xdg-native-messaging-proxy`](https://github.com/flatpak/xdg-native-messaging-proxy). The proxy forwards native-messaging connections to the host over session D-Bus; this weakens the sandbox boundary, so install and configure it only if you trust the host integration.

Install `xdg-native-messaging-proxy` using your distribution or its upstream instructions. The proxy is D-Bus activated. Confirm that it is available:

```sh
busctl --user list | grep NativeMessagingProxy
```

The proxy searches standard Mozilla native-messaging host locations. The package installs its manifest under `share/mozilla/native-messaging-hosts/`; link it into a location searched by the proxy if needed. For a typical system installation:

```sh
sudo mkdir -p /usr/lib/mozilla/native-messaging-hosts
sudo ln -sf /usr/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json \
  /usr/lib/mozilla/native-messaging-hosts/
```

Use `/usr/lib64` where appropriate. A per-user location such as `~/.mozilla/native-messaging-hosts` or `$XDG_CONFIG_HOME/mozilla/native-messaging-hosts` can also be used. Do not put the host manifest inside the Flatpak data directory; the proxy must be able to find and execute the host binary on the host system.

For NixOS, the module can install and configure the proxy service:

```nix
services.flatpak.enable = true;
programs.thunderbird-plasma-calendar = {
  enable = true;
  flatpakProxy.enable = true;
  # flatpakProxy.extraHostLocations = [ "/some/other/manifest/dir" ];
};
```

The module sets `XNMP_HOST_LOCATIONS` to include the usual user and system directories and this package's manifest location. It also starts the proxy as a user service. Apply and check the configuration:

```sh
sudo nixos-rebuild switch
systemctl --user status xdg-native-messaging-proxy
busctl --user list | grep NativeMessagingProxy
```

Grant the Flatpak permission to talk to the proxy (adjust the application ID for another Thunderbird channel):

```sh
flatpak override --user --talk-name=org.freedesktop.NativeMessagingProxy \
  org.mozilla.thunderbird
```

In Thunderbird's `about:config`, create or set this integer preference:

```text
widget.use-xdg-desktop-portal.native-messaging-proxy = 2
```

`2` enables automatic detection. Install the XPI through **Add-ons and Themes → Extensions → gear menu → Install Add-on From File**, then fully restart Thunderbird. Check proxy logs with:

```sh
journalctl --user -u xdg-native-messaging-proxy -n 50
```

## Configuration

### Thunderbird extension settings

The extension options page controls the export window. **Show advanced settings** exposes:

- **Event details:** export descriptions (default on) and locations (default off). Locations are stored for potential future use but are not currently displayed by the Plasma provider.
- **Text truncation:** descriptions are truncated by default; location truncation is off by default. Each has an independent limit from 1 to 1000 characters.
- **Calendar query timeout:** 60 seconds per 30-day query by default; configurable from 5 to 300 seconds.
- **Debug logging:** off by default. When enabled, refresh and native-host timings are written to Thunderbird's Error Console (Ctrl+Shift+J).

Settings save automatically. Numeric values are clamped to their supported ranges. Older shared text-truncation and timing preferences are migrated to the current independent settings.

### Plasma calendar settings

Right-click the Digital Clock and open **Configure Digital Clock → Calendar Events**.

- **Failed calendar refreshes:** **Show the last synchronised events** is enabled by default. Disable it to hide events from calendars currently marked stale. This changes display only; it does not delete cached events.
- **Thunderbird calendars:** select which Thunderbird calendars are displayed. Newly discovered calendars are not automatically selected.

These settings are stored by Plasma, independently of Thunderbird's own calendar selection.

## Cache, privacy and profile behaviour

The persistent cache normally lives at `~/.cache/plasma-thunderbird-calendar/` (or `$XDG_CACHE_HOME/plasma-thunderbird-calendar/`):

```text
plasma-thunderbird-calendar/
├── events.json       # Last compacted snapshot
└── events.journal    # Changes since that snapshot
```

The cache directory is restricted to the user (0700), and the files are restricted to the user (0600). The native-host lock normally lives at `$XDG_RUNTIME_DIR/plasma-thunderbird-calendar/native-host.lock`, falling back to `$XDG_CONFIG_HOME/plasma-thunderbird-calendar/native-host.lock` if the runtime directory is unavailable.

Only one Thunderbird profile per Unix user can use the integration at a time, including across native, Flatpak and NixOS installations. Close the first Thunderbird instance completely before switching profiles. If another profile owns the lock, the second instance reports the conflict and retries synchronisation with bounded backoff.

The cache is **not encrypted** and contains exported event data, including descriptions when enabled. Protect your user account and consider this before exporting sensitive calendars. Removing the cache while the host is running is not recommended; stop Thunderbird first.

## Testing

### Extension tests

The JavaScript mock tests require Node.js but do not require Thunderbird:

```sh
node tests/experiment-api.cjs
node tests/settings.cjs
node tests/background-ack.cjs
node tests/background-errors.cjs
node tests/native-message-limit.cjs
```

They cover settings migration and clamping, event detail export/truncation, floating-time serialisation, observer coalescing, recurrence expansion limits, cross-calendar moves, snapshot/batch races, query concurrency and timeouts, direct-update delivery and acknowledgements, debug logging, payload limits, profile-lock recovery and bounded retries.

### C++ and native-host tests

Configure and build the project with testing enabled (the default when using CTest):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The Qt tests cover floating-time parsing under `Europe/London`, `America/New_York` and `Asia/Tokyo`, stale-event visibility, cache retry behaviour and deterministic revision-chain hashing.

After building the native host, run the frame-size and timing tests:

```sh
python3 tests/native-host-limit.py ./build/bin/thunderbird-calendar-native-host
python3 tests/native-host-timing.py ./build/bin/thunderbird-calendar-native-host
```

The D-Bus event-delta integration test requires `dbus-run-session`, `dbus-monitor` and `dbus-send`:

```sh
python3 tests/native-host-event-deltas.py ./build/bin/thunderbird-calendar-native-host
```

It exercises snapshot notification, the single-profile lock, snapshot diffing, direct add/modify/remove changes, date moves, batching, journal replay, interrupted appends, compaction, restart recovery, legacy journal migration and missing-calendar recovery. It also verifies revision-chain hashes and snapshot checkpoints against an independent Python implementation, and includes a 50,000-event performance fixture.

### Live integration check

With Thunderbird running and a calendar enabled in Plasma:

1. Create an event and confirm it appears in the Digital Clock agenda.
2. Edit its title and time; confirm it updates without duplication.
3. Move it to another date; confirm it disappears from the old date and appears on the new date.
4. Move it to another calendar and delete it; verify the corresponding changes.
5. Restart Plasma while Thunderbird is closed; confirm that cached events remain available.
6. Test floating, UTC and timezone-qualified events in more than one system timezone, including DST transitions.

Calendar-selection changes can trigger the upstream Plasma issue described at the top of this README on affected versions. Avoid treating a Plasma Shell crash during a bulk removal as evidence that Thunderbird synchronisation or journal recovery failed.

## Troubleshooting and diagnostics

- **No events:** enable **Enable debug logs**, inspect Thunderbird's Error Console, and check that `events.json` exists under `$XDG_CACHE_HOME/plasma-thunderbird-calendar/` (normally `~/.cache/`).
- **Payload too large:** native messaging is limited to 32 MiB per frame. Reduce the export window, disable description/location export or enable truncation. Both the extension and host enforce the limit.
- **No calendars in Plasma settings:** start Thunderbird, wait for an initial snapshot, then reopen the Calendar Events settings or select **Refresh list**.
- **Another profile is active:** close the other Thunderbird instance completely. Only one profile per Unix user can own the native-host lock.
- **Native host not found:** check that the manifest is installed in a directory searched by Thunderbird and that its executable path exists and is executable.
- **Flatpak cannot connect:** check the proxy on the session bus, the host manifest search path, the Flatpak `--talk-name=org.freedesktop.NativeMessagingProxy` permission and the Gecko preference value `2`.
- **Plasma does not list the provider:** confirm that the plugin is installed under a Qt 6 `plasmacalendarplugins` directory and restart Plasma.

### Native-host timing logs

1. Rebuild and reinstall the project so Thunderbird launches the updated native host. Rebuilding only the XPI does not update the host executable.
2. Enable **Enable debug logs** in the Thunderbird extension settings.
3. Open Thunderbird's Error Console with Ctrl+Shift+J.
4. Create, modify or delete an event. Direct updates report `native host direct-update phases (ms)`; full refreshes report `native host phases (ms)`. The diagnostics include validation, event indexing/application, revision hashing, journal serialisation, append, `fsync`, compaction, D-Bus notification and bytes written.

For a direct update, compare the extension's acknowledgement duration with the host's processing time. `wait before message` is idle time between requests, not processing time for the current message.

### Plasma-side logs

Start the capture before restarting Plasma Shell so the log includes the provider's startup message:

```sh
journalctl --user -f -o short-precise \
  | grep --line-buffered -F 'Thunderbird Calendar [Plasma]' \
  | tee "$HOME/thunderbird-plasma-calendar-plasma.log"
```

Then restart Plasma Shell from another terminal or log out and back in. Reproduce the operation and stop the capture with Ctrl+C. The provider logs initialisation, published date ranges, event-delta batches, completed revisions, configuration changes and cache reconciliation. It also reports the exact number of `eventRemoved()` signals emitted by each snapshot/configuration operation and by each delta batch, with running totals across a complete delta revision. For large removal loops it logs progress before the first signal and every 50 signals, so the log can still show progress if a receiving Plasma slot stalls. These diagnostics include revision IDs and counts, not event titles, descriptions or locations.

For a Plasma Shell crash, also capture the **unfiltered** user journal around the crash and inspect the core dump; the filtered command above intentionally excludes KDE, Qt and systemd crash messages:

```sh
journalctl --user -b --since "YYYY-MM-DD HH:MM:SS" \
  --until "YYYY-MM-DD HH:MM:SS" -o short-precise
coredumpctl info plasmashell
```

## Known limitations

- The integration is read-only. It does not create, edit, delete or RSVP to Thunderbird events.
- Only events inside the configured export window are included.
- Only the latest stable Thunderbird release is tested. The Experiment API depends on internal interfaces that may change.
- Remote calendar updates depend on Thunderbird's own provider synchronisation; Thunderbird must be running to discover new server-side changes.
- Floating events inside a local spring-forward gap are not published because the local wall-clock time does not exist. Repeated autumn times retain their wall-clock fields.
- Tasks are exported on a best-effort basis. Alarms, attendees, attachments and full recurrence metadata are not exposed to Plasma.
- Some providers may reject broad range queries or handle recurring events differently. The timeout bounds asynchronous stream reads but cannot interrupt synchronous provider work before a stream is returned; cancellation is best-effort.
- Event locations can be exported to the cache, but the current Plasma provider does not display them.
- The cache is unencrypted.
- On affected Plasma Workspace versions, bulk `eventRemoved()` notifications can trigger expensive `DaysModel` resets. This is an upstream Plasma issue tracked in [KDE Bug 526769](https://bugs.kde.org/show_bug.cgi?id=526769); this project does not currently implement a workaround.

## License

This project is licensed under **GPL-2.0-or-later**. The complete GNU GPL version 2 text and the later-version grant are included in [`LICENSE`](LICENSE). C++, QML and JavaScript sources carry SPDX license identifiers.
