# Thunderbird calendars in KDE Plasma's Digital Clock

<img width="2085" height="1011" alt="Thunderbird calendars in the Plasma Digital Clock" src="https://github.com/user-attachments/assets/3ae24623-3175-4b41-9679-12a62800785b" />

> **Disclaimer:** This project is vibe coded with substantial AI assistance. It is an early prototype. Review the source, permissions and behaviour carefully before relying on it.

A read-only integration that exposes calendars configured in Mozilla Thunderbird to the Plasma 6 Digital Clock through KDE's `CalendarEvents` plugin API. It appears alongside KDE's Akonadi/PIM Events provider; it does not modify Plasma and does not require Akonadi.

**Current version: 0.4.7.** The extension-side mock tests and native-host protocol tests are included in the repository. The C++ components and live Plasma behaviour should still be tested on the target desktop. See [Testing](#testing) and [Known limitations](#known-limitations).

## Architecture

```text
Thunderbird MailExtension
  ├─ Experiment API reads cal.manager / calICalendar for full snapshots
  ├─ calendar observers serialise individual add / modify / delete changes
  └─ native messaging (length-prefixed JSON)
           │
           ▼
Native messaging host (C++ / Qt)
  ├─ validates and persists a full recovery snapshot
  ├─ applies direct item changes to the persisted snapshot
  └─ emits revisioned, batched event deltas over session D-Bus
           │
           ▼
Plasma CalendarEvents plugin (C++ / Qt)
  ├─ loads a snapshot for initial population and recovery
  └─ maps event deltas to dataReady / eventModified / eventRemoved
           │
           ▼
Plasma DaysModel owns the displayed event collection
```

### Synchronisation model

- **Full snapshots** are used for initial loading, recovery, calendar metadata or export-setting changes, provider errors, missed notifications and the periodic safety refresh. The default export window is 90 days back and 360 days ahead.
- **Direct item updates** use Thunderbird's `onAddItem`, `onModifyItem` and `onDeleteItem` observer callbacks. The extension serialises the affected event, coalesces changes by calendar ID and stable event UID, and sends them to the native host after a short debounce. Ordinary item edits do not trigger another `getItems()` query.
- The native host applies direct changes to its in-memory recovery state and appends a checksummed JSONL record to `events.journal`. The record is flushed and `fsync`'d before the host notifies Plasma or acknowledges the message. Atomic snapshot and journal replacement also sync the containing directory after the rename. Ordinary item updates do not rewrite the multi-megabyte `events.json` snapshot.
- The journal is compacted into `events.json` after 128 records or 1 MiB, whichever comes first. Compaction uses an atomic snapshot replacement followed by an atomic journal-header reset. If a crash occurs between those operations, the snapshot's revision no longer matches the old journal header, so the snapshot safely supersedes that journal.
- On native-host startup, the last compacted snapshot is loaded and complete journal records are replayed in revision order. Each record has a checksum and previous/current revision pair. An incomplete final record is treated as an unacknowledged torn append and discarded; invalid complete records require a full snapshot recovery.
- The Plasma plugin also replays the checksummed journal over `events.json` when it loads a date range or reconciles calendar selection. This keeps the displayed calendar current even if Plasma starts after Thunderbird has appended changes but before the next journal compaction. The plugin does not retain a second full event collection; it builds the requested event view from the recovered snapshot.
- The host is not a daemon. Thunderbird starts it when the extension connects. The compacted snapshot plus journal remain sufficient to reconstruct the latest calendar state while Thunderbird is closed.
- The host accepts only `snapshot` and `eventChanges` messages. It accepts no arbitrary commands, paths or method names. Messages are acknowledged only after a full snapshot is committed or a direct update has been durably appended. A missing calendar in the host cache causes `FULL_SNAPSHOT_REQUIRED` and a full resynchronisation.
- The background script retries failed direct deliveries with bounded backoff. The compacted snapshot, journal replay and periodic full refresh protect against missed observer notifications and restarts.

### Calendar queries and event handling

- Full snapshots query calendars in 30-day chunks, with up to three calendars queried concurrently. A per-chunk timeout (60 seconds by default) bounds asynchronous stream reads. A timeout or provider error is isolated to that calendar; its last successful events are retained and marked stale rather than being replaced by an empty list.
- Event processing yields to Thunderbird's event loop after approximately 10 ms of synchronous work. Duplicate occurrences returned at chunk boundaries are exported once.
- The exported date window is based on local midnight and advances daily. A 15-minute safety refresh checks for observer notifications that may have been missed.
- Multi-day events are supplied once under their start date, as required by the CalendarEvents API. `EventData::isAllDay` is set only for single-day all-day events; a multi-day event is not repeated on every date it spans.
- Event moves are represented as removal of the old date-specific event identity followed by insertion of the new one. The plugin emits the follow-up notification needed to invalidate Plasma's agenda cache for the affected event date.
- Calendar selection is managed in the plugin's settings UI. Once a selection is saved, newly added Thunderbird calendars remain unselected until enabled there.

### Storage and privacy

The recovery data is stored under:

```text
$XDG_CACHE_HOME/plasma-thunderbird-calendar/
├── events.json       # Last compacted full snapshot
└── events.journal    # Direct event changes since that snapshot
```

Normally this is `~/.cache/plasma-thunderbird-calendar/`. The directory is restricted to the user (0700), and both files are restricted to the user (0600). Journal records are appended and synchronously flushed before an update is acknowledged. The journal is compacted after 128 records or 1 MiB. Both the native host and the Plasma plugin verify journal record checksums and the revision chain before replaying it. An incomplete final line is ignored by the plugin and discarded by the host as an unacknowledged interrupted append; corruption in a complete record requires a new full snapshot. The cache is **not encrypted** and contains exported event details, including descriptions when enabled. Consider this before exporting sensitive calendars.

## Requirements

- Linux with KDE Plasma 6.
- C++20 compiler, CMake, Ninja and Python 3.
- Qt 6.5 or newer (Core, DBus, Qml).
- KDE Frameworks 6: KCoreAddons, KDeclarative and KConfig.
- Extra CMake Modules 6.0 or newer.
- Thunderbird 128 or newer for the extension's declared minimum, with Experiment API support enabled by the build.

Use Qt 6 / KF6 packages, not Qt 5 / KF5.

### Install build dependencies

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

### System-wide installation

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
sudo cmake --install build
```

### User-prefix installation

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build --parallel
cmake --install build
```

If Plasma does not discover the plugin from a custom prefix, add the directory *containing* `plasmacalendarplugins/` (for example `~/.local/lib/qt6/plugins`) to `QT_PLUGIN_PATH` for the Plasma session, then restart Plasma.

### Register the native host and install the add-on

For native Thunderbird, register the installed host manifest as your normal user:

```sh
mkdir -p ~/.mozilla/native-messaging-hosts
ln -sf /usr/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json \
  ~/.mozilla/native-messaging-hosts/
```

For a user-prefix install, replace `/usr` with `$HOME/.local`. The manifest contains the host executable's absolute path; do not move the executable independently of the package.

In Thunderbird, open **Add-ons and Themes → Extensions → gear menu → Install Add-on From File** and select the installed `thunderbird-plasma-calendar.xpi` (normally under `/usr/share/thunderbird-plasma-calendar/`). Restart Thunderbird and allow the initial calendar refresh to finish.

The generated XPI is unsigned. Whether an unsigned add-on can be installed permanently depends on the Thunderbird build and its policy; use a build that permits the development/sideloading workflow required by this project.

The add-on requests **full, unrestricted access to Thunderbird and your computer**. This is inherent to the Experiment API mechanism used to access Thunderbird's internal calendar interfaces. Only install it if you trust the source.

## NixOS

Build the package:

```sh
nix build
```

The result contains the Plasma plugin, `bin/thunderbird-calendar-native-host`, the host manifest under `share/mozilla/native-messaging-hosts/`, and the XPI under `share/thunderbird-plasma-calendar/`.

To use the NixOS module:

```nix
programs.thunderbird-plasma-calendar.enable = true;
```

For native Thunderbird, link the manifest from the system profile:

```sh
mkdir -p ~/.mozilla/native-messaging-hosts
ln -sf /run/current-system/sw/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json \
  ~/.mozilla/native-messaging-hosts/
```

Install the XPI from `/run/current-system/sw/share/thunderbird-plasma-calendar/`.

## Flatpak Thunderbird

A Flatpak Thunderbird needs a host-side native-messaging proxy because it cannot directly execute host binaries or read the host's native-messaging manifests. This project supports [`xdg-native-messaging-proxy`](https://github.com/flatpak/xdg-native-messaging-proxy).

The proxy forwards the extension's native-messaging connection to the host executable over session D-Bus. Its upstream documentation warns that this capability weakens the sandbox boundary. This project limits the registered host to the add-on's exact ID and exposes only the calendar bridge protocol.

### Proxy and manifest

Install `xdg-native-messaging-proxy` from your distribution or build it using its upstream instructions. It is D-Bus activated; it does not need to run continuously. Check availability with:

```sh
busctl --user list | grep NativeMessagingProxy
```

The proxy searches the standard Mozilla native-messaging host locations. The package installs its manifest under `share/mozilla/native-messaging-hosts/`, so link it into a searched location if necessary. For a typical system installation:

```sh
sudo mkdir -p /usr/lib/mozilla/native-messaging-hosts
sudo ln -sf /usr/share/mozilla/native-messaging-hosts/org.kde.plasma.thunderbird_calendar.json \
  /usr/lib/mozilla/native-messaging-hosts/
```

Use `/usr/lib64` where appropriate. Alternatively, place the link under `~/.mozilla/native-messaging-hosts` or `$XDG_CONFIG_HOME/mozilla/native-messaging-hosts`.

Do not place the manifest inside the Flatpak data directory. The proxy searches host locations, and the host binary must be executable on the host.

### NixOS proxy module

The flake provides the proxy package and module options:

```nix
services.flatpak.enable = true;
programs.thunderbird-plasma-calendar = {
  enable = true;
  flatpakProxy.enable = true;
  # flatpakProxy.extraHostLocations = [ "/some/other/manifest/dir" ];
};
```

The module starts `xdg-native-messaging-proxy` as a user service and configures `XNMP_HOST_LOCATIONS`. That variable replaces the proxy's default search paths; the module includes the usual user and system locations and the package's manifest directory.

Apply and check the configuration:

```sh
sudo nixos-rebuild switch
systemctl --user status xdg-native-messaging-proxy
busctl --user list | grep NativeMessagingProxy
```

### Flatpak permissions and Gecko preference

Grant the application permission to talk to the proxy (adjust the application ID for a different channel):

```sh
flatpak override --user --talk-name=org.freedesktop.NativeMessagingProxy \
  org.mozilla.thunderbird
```

In Thunderbird's `about:config`, create or set the integer preference:

```text
widget.use-xdg-desktop-portal.native-messaging-proxy = 2
```

`2` enables automatic detection. Install the XPI through **Add-ons and Themes → Extensions → gear menu → Install Add-on From File**, then fully restart Thunderbird. The proxy journal can help diagnose connection problems:

```sh
journalctl --user -u xdg-native-messaging-proxy -n 50
```

## Extension options

The main settings page controls the look-back and look-ahead window. **Show advanced settings** exposes:

- Export event descriptions (default on) and event locations (default off).
- Independently truncate descriptions (default on) and locations (default off), with separate limits from 1 to 1000 characters.
- Per-query timeout (default 60 seconds).
- **Enable debug logs** (default off).

Numeric options combine a slider with a number field. Values are saved automatically and clamped to their supported ranges. The previous shared text-truncation preference is migrated to the independent description and location preferences; previously saved values are preserved.

When debug logging is enabled, Thunderbird's Error Console (Ctrl+Shift+J) reports calendar query timings, direct-update delivery and native-host phase timings. The host omits optional timing details when debug logging is disabled. Oversized-payload errors remain visible because they require a settings change to recover.

## Testing

Run the extension-side mock tests with Node.js; they do not require Thunderbird:

```sh
node tests/experiment-api.cjs
node tests/settings.cjs
node tests/background-ack.cjs
node tests/native-message-limit.cjs
```

These cover settings migration and clamping, description/location export and truncation, stale-event preservation, observer notification coalescing, query concurrency/timeouts, direct-change delivery, request acknowledgements, debug logging and the extension-side 32 MiB payload guard.

After building the native host, run the frame-size test:

```sh
python3 tests/native-host-limit.py ./build/thunderbird-calendar-native-host
```

It sends a 32 MiB + 1 byte raw frame and verifies that the host returns a structured `NATIVE_MESSAGE_TOO_LARGE` error without modifying the calendar cache.

Test native-host message timing:

```sh
python3 tests/native-host-timing.py ./build/thunderbird-calendar-native-host
```

The event-delta integration test requires `dbus-run-session`, `dbus-monitor` and `dbus-send`:

```sh
python3 tests/native-host-event-deltas.py ./build/thunderbird-calendar-native-host
```

It runs in a private D-Bus session, waits for a monitor-readiness probe rather than relying on a startup sleep, and tests initial snapshot notification, snapshot diffing, direct add/modify/remove updates, date moves, metadata-only revisions, batching, journal append and restart replay, deterministic ordering for multiple additions, recovery from an incomplete final append followed by a new committed record (the repaired journal is expected to contain its header plus both valid records), unchanged full-snapshot detection after replay, 128-record compaction, and missing-calendar recovery. It also checks that direct-update timing fields (including journal append, fsync and compaction) are returned when debug logging is enabled.

For a live Plasma check, create an event, edit its title and time, move it to another date and delete it while the Digital Clock agenda is open. Verify that additions appear once, edits update in place, moves disappear from the old date and appear on the new date, and deletions disappear without switching months or reopening the widget. Also test multi-day all-day and timed events: each should be supplied once under its start date.

## Troubleshooting

- **No events:** enable **Enable debug logs**, check Thunderbird's Error Console, and verify that `events.json` exists under `$XDG_CACHE_HOME/plasma-thunderbird-calendar/` (normally `~/.cache/`).
- **Payload too large:** the complete native-messaging frame is limited to 32 MiB. Reduce the date window, disable description/location export or enable text truncation. Both extension and host enforce the limit.
- **No calendars in plugin settings:** start Thunderbird, wait for a snapshot, then reopen the plugin settings or use **Refresh list**.
- **Native host not found:** verify the manifest is installed in a directory searched by the client and that the executable path in the manifest exists and is executable.
- **Flatpak cannot connect:** verify the proxy is available on the session bus, the host manifest is in a proxy search directory, the Flatpak has `--talk-name=org.freedesktop.NativeMessagingProxy`, and the Gecko preference is set to `2`.
- **The add-on disappears after restart:** confirm that Thunderbird is using the same profile (`about:profiles`), install through **Install Add-on From File**, and quit Thunderbird normally rather than force-killing it immediately after installation.
- **Plasma does not list the plugin:** verify that the shared library is installed in a Qt 6 `plasmacalendarplugins` directory and restart Plasma.

## Known limitations

- This is an early prototype and has not been validated across every Plasma, Thunderbird or distribution version.
- The Experiment API relies on internal Thunderbird interfaces that can change between major releases. Thunderbird's unrestricted Experiment permission is a significant trust boundary.
- Updates to remote calendars depend on Thunderbird's own synchronisation schedule. Thunderbird must be running for new server-side changes to be observed.
- Only events within the configured date window are exported. The integration is read-only; it does not create, edit, delete or RSVP to events.
- Tasks are mapped to Plasma Todo items on a best-effort basis. Alarms, attendees, attachments and full recurrence metadata are not exposed to Plasma.
- Some calendar providers may reject broad range queries or behave differently with recurring events. Timeouts bound asynchronous stream reads, but cannot interrupt synchronous provider work before a stream is returned. Stream cancellation is best-effort.
- Plasma's `DaysModel` can perform a model reset for every `eventRemoved()` signal on affected versions. Large bursts of removals can therefore expose an upstream Plasma performance/memory issue that is also reproducible with Akonadi's PIM Events provider. This project does not patch or mitigate that Plasma behaviour.

## Debugging native-host timings

1. Rebuild and reinstall the project so Thunderbird launches the new native-host executable. Rebuilding only the XPI does not update the host.
2. Enable **Enable debug logs** in the extension settings.
3. Open Thunderbird's Error Console with Ctrl+Shift+J.
4. Create, modify or delete an event. Look for `native host direct-update phases (ms)` for direct observer updates. It reports:
   - recovery-cache setup, read and validation;
   - calendar indexing and incoming-change validation/grouping;
   - event UID indexing, event application and rebuilding the affected calendar's event array;
   - revision hashing, journal-record serialisation, append and `fsync`;
   - occasional snapshot compaction, atomic disk commit and D-Bus notification;
   - number of incoming changes, affected calendars, applied deltas and bytes written.
5. For a full refresh, look for `native host phases (ms)`. The native host also writes a `native-host message timing (ms)` line to stderr with the wait before the message, frame read, host processing, response write and total message-handling time.

For direct changes, compare the background's **direct update acknowledgement** duration with the host's `host processing` and `frame read` fields. The difference is extension/runtime transport and scheduling overhead. Direct-update diagnostics separately report journal-record serialisation, append, `fsync`, record count/size and compaction time. A normal single-event update should append a small record without rewriting `events.json`; occasional compaction writes the complete snapshot. `wait before message` is idle time between native-messaging requests and is not part of the current request's processing duration.

The host uses `poll()` to detect readable input and direct file-descriptor reads to avoid C++ stream-buffer prefetch. Timings use a monotonic clock and are included in the response only when the request has `debugLogs: true`.

## License

GPL-2.0-or-later. C++, QML and JavaScript sources carry SPDX identifiers.
