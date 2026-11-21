# Thunderbird calendars in KDE Plasma's Digital Clock

<img width="2085" height="1011" alt="Screenshot_20261118_000610" src="https://github.com/user-attachments/assets/3ae24623-3175-4b41-9679-12a62800785b" />

> **Disclaimer:** This project is vibe coded with substantial AI assistance. It is an early prototype; review the source, permissions and behaviour carefully before relying on it.

A read-only integration that shows calendars configured in Mozilla Thunderbird in
the Plasma 6 Digital Clock, as a `CalendarEvents` provider. It sits beside KDE's
Akonadi/PIM Events provider; it does not modify Plasma and does not need Akonadi.

**Status: early prototype (0.4.4).** The extension and native-host protocol have
automated mock/integration tests. The incremental CalendarEvents plugin still
needs verification in a complete Plasma desktop session. See [Known limitations](#known-limitations).

## How it works

```text
Thunderbird MailExtension
  ├─ Experiment API reads cal.manager / calICalendar for initial/recovery snapshots
  ├─ calendar observers serialize the actual added/modified/deleted items
  └─ runtime.connectNative()  ── Mozilla native-messaging framing (JSON)
           ▼
Native messaging host (C++ / Qt)
  ├─ validates and atomically persists the full recovery snapshot
  ├─ applies compact item-level changes directly to its cached snapshot
  └─ emits revisioned, batched event-level deltas over the session D-Bus
           ▼
Plasma CalendarEvents plugin (C++ / Qt)
  ├─ reads the full snapshot only for initial/date-range loading
  └─ maps add / modify / remove deltas to CalendarEvents signals
           ▼
Plasma DaysModel (single owner of displayed event data)
```

- The host is not a daemon. Thunderbird starts it when the add-on connects and it
  exits when Thunderbird closes. Plasma reads the last cache written, so synced
  events stay visible while Thunderbird is closed.
- The Experiment API observes Thunderbird's calendar manager and calendar-item
  callbacks. Ordinary item additions, modifications and deletions are serialized
  directly from the observer arguments; they do not trigger another `getItems()`
  query. The background script coalesces changes by calendar ID and stable event
  UID for 120 ms, then sends the compact batch to the native host. Thunderbird
  batch callbacks are coalesced before they leave the Experiment API. Calendar
  metadata changes, provider errors, missed/unsupported observer states, settings
  changes and recovery still request a full snapshot. A 15-minute safety poll
  remains. The exported window defaults to 90 days back and 360 days ahead and can
  be changed in the add-on's options (see [Options](#options)).
- The Experiment API queries each calendar in 30-day chunks, because a provider
  expands recurring events inside a single `getItems()` call that cannot be
  interrupted. Up to three calendars are queried concurrently. Each chunk has a
  configurable timeout (60 seconds by default); a timeout or provider error is
  isolated to that calendar, while the other calendars continue. Item processing
  yields to Thunderbird's event loop after roughly 10 ms of synchronous work.
  Items returned more than once across chunks are exported once. A change that
  arrives during a refresh triggers one follow-up refresh.
- Each calendar's exported events are cached in the Experiment API for full
  snapshot queries. A direct item notification invalidates that calendar's cache
  entry for the next safety refresh, but the immediate update does not reread the
  calendar. If a full read fails, the last successful event list for the same date
  range and export options is retained and marked stale rather than replaced with
  an empty list. A disabled or deleted calendar is handled separately. The exported
  window runs from local midnight to local midnight, so it stays the same for a
  whole day; it moves, and every calendar is read again, once a day. The 15-minute
  safety-net poll rereads every calendar in case a notification was missed. A
  change to the range or export options also invalidates the affected cache entries.
  The cache holds the exported events in memory, about the size of one snapshot.
- The native host keeps the full snapshot as the recovery source. Ordinary
  observer changes arrive as `eventChanges` messages containing only the affected
  items. The host applies these to the affected calendar entries in its current
  snapshot, atomically persists that recovery snapshot, and emits revisioned,
  batched `EventsDelta` signals. It does not need to diff all 50,000 events to
  discover a one-event change. The Plasma plugin translates additions into
  `dataReady()`, in-place edits into `eventModified()`, and deletions into
  `eventRemoved()`. After inserting delta additions with `dataReady()`, it emits
  `eventModified()` for those newly inserted items as an agenda-cache invalidation:
  Plasma's current `DaysModel` otherwise only marks the agenda cache dirty for
  additions on today's date. This follow-up tells the agenda to refresh the exact
  event date, including when an event is added or moved to another future day.
  The Plasma-facing UID includes the event's local start date. Moving an event to
  another start date therefore removes the old date identity and inserts a new one.
  This is important because some Plasma versions defer removals; the deferred
  old-UID removal must not remove the newly inserted event. The plugin retains
  only its requested date range and selected calendar IDs; it does
  not maintain a second full event collection. Each D-Bus delta batch carries
  snapshot revisions so a range load that races with an update can ignore a delta
  already represented in its read. A complete `EventsChanged` load is reserved
  for first snapshot creation/recovery and normal date-range loads.
- Multi-day events are inserted once, keyed by their start date, as required by
  the `CalendarEventsPlugin` API. `EventData::isAllDay` is set only for single-day
  all-day events; multi-day events leave it false as required by the `EventData`
  documentation.
  Consequently, a multi-day event appears once in the agenda, on its start date,
  rather than being duplicated on every day it spans.
- The host skips the cache write and D-Bus signal when a snapshot differs from
  the cached one only in timestamps, so Plasma does not re-parse the file
  needlessly. When event content does change, only the event-level delta is sent
  over D-Bus (the full updated snapshot is still atomically persisted for recovery).
- If the host keeps failing, reconnect attempts back off exponentially from 15
  seconds up to 15 minutes.
- The host accepts only two narrowly defined message types: `snapshot` for
  initial/recovery synchronisation and `eventChanges` for item-level add/modify/
  remove updates. It accepts no paths, commands or arbitrary method names. Every
  message has a request ID and is acknowledged only after the host has accepted
  the update (and, when necessary, atomically written the recovery snapshot). If
  an item update refers to a calendar absent from the cached snapshot, the host
  returns `FULL_SNAPSHOT_REQUIRED` and the extension rebuilds the full snapshot.
  The background script waits for acknowledgements and retries failed deliveries
  with bounded backoff. The host manifest allows only this add-on's exact ID.
- The XPI builder clamps ZIP timestamps to the supported range (Nix sources use
  the 1970 epoch) and honours `SOURCE_DATE_EPOCH`.

Repository layout:

| Path | Purpose |
| --- | --- |
| `extension/` | MV2 MailExtension, options page and a narrow Experiment API (kept in a private closure) |
| `src/nativehost.cpp` | native-messaging host, snapshot recovery writer, direct event updater, D-Bus delta notifier |
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

The main options page provides the number of days to look back and ahead. Use the
**Show advanced settings** button for the remaining controls:

- export event descriptions (default on) and locations (default off);
- independently truncate long descriptions (default on) and locations (default off), with
  separate maximum lengths from 1 to 1000 characters (defaults 300 and 200).
  Either kind of text can be exported without truncation while the other remains
  truncated;
- calendar query timeout (5 to 300 seconds per 30-day query, default 60);
- **Enable debug logs** (default off). When enabled, refresh timings, calendar
  provider/query diagnostics and native-host phase timings are written to
  Thunderbird's Error Console (Ctrl+Shift+J). Native-host timings include the
  wait between messages (reported separately), current-frame reading, JSON
  parsing, snapshot validation, content hashing, cache-directory setup,
  previous-cache loading, snapshot serialisation, disk commit and D-Bus
  notification. The host also reports host processing, response-frame writing
  and total message-handling time to stderr. The total starts when the first
  bytes of a request become readable and ends after its response has been
  written, so it can be compared directly with the extension's native-host
  acknowledgement duration. When debug logging is disabled, diagnostic timings
  are omitted. A payload-size rejection is always reported as an actionable
  error because it requires a settings change to recover.

Numeric settings have both a slider and a number field. Values are saved
automatically and clamped to their supported ranges. The older `logTiming`
preference is migrated to `enableDebugLogs` the next time settings are saved.

## Troubleshooting

- **No events:** enable **Enable debug logs** in the add-on's advanced settings,
  then check Thunderbird's Error Console for `Thunderbird Plasma Calendar`
  messages. Also check that `$XDG_CACHE_HOME/plasma-thunderbird-calendar/events.json`
  (normally under `~/.cache`) exists.
- **"snapshot too large":** the complete native-messaging JSON envelope is
  limited to 32 MiB. The extension checks its UTF-8 size before sending, and the
  host independently checks the incoming frame. The error reports actual and
  maximum sizes. Reduce the look-back/look-ahead range, turn off description or
  location export, or enable text truncation. Size-limit errors are shown even
  when **Enable debug logs** is off.
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
  with recurring events. Query timeouts bound asynchronous stream reads; they
  cannot interrupt synchronous work performed internally by a provider before it
  returns a stream. Stream cancellation is best-effort because provider support
  varies.

## Development

On NixOS: `nix develop`. Elsewhere install the dependencies above, then:

```sh
cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX="$PWD/_install"
cmake --build build --parallel
cmake --install build
```

The host speaks Mozilla's length-prefixed JSON protocol on stdin/stdout, so do not
run it from a terminal to inspect output.

### Extension mock tests

Node.js is sufficient to run the extension-side mock tests without launching
Thunderbird:

```sh
node tests/experiment-api.cjs
node tests/settings.cjs
node tests/background-ack.cjs
node tests/native-message-limit.cjs
```

The first four tests cover independent description/location truncation,
stale-event preservation, debug-log gating, batch notification coalescing,
query concurrency/timeouts,
settings migration, value clamping, request-ID acknowledgement and the extension's
pre-send 32 MiB guard. They do not replace testing against real Thunderbird
calendar providers or a complete Plasma session.

After building the native host, test its independent frame-size guard with a
single 32 MiB + 1 byte test frame:

```sh
python3 tests/native-host-limit.py ./build/thunderbird-calendar-native-host
```

For a Nix build, pass the installed host executable from the build result instead
of the local `./build` path. The test sends an intentionally oversized raw frame
and verifies that the host drains it, returns a structured
`NATIVE_MESSAGE_TOO_LARGE` response containing the received and maximum byte
counts, and exits normally. It does not modify the calendar cache.

After building the host, also verify the timing fields and total message duration:

```sh
python3 tests/native-host-timing.py ./build/thunderbird-calendar-native-host
```

This sends a minimal valid snapshot with debug logging enabled, checks that the
response contains the wait, frame-read and host-processing measurements, and
checks stderr for response-frame writing and total message-handling duration. It
uses a temporary `XDG_CACHE_HOME` so it does not overwrite the user's real cache.


The event-delta integration test requires `dbus-run-session`, `dbus-monitor` and
`dbus-send` in addition to the built host:

```sh
python3 tests/native-host-event-deltas.py ./build/thunderbird-calendar-native-host
```

It starts the host and monitor in a private D-Bus session. The test no longer
uses a fixed startup sleep: it repeatedly emits a harmless readiness probe until
this exact `dbus-monitor` process receives it, proving its match rule is active
before the host's first `EventsChanged` signal. Its string decoder accepts both
raw-quoted and escaped JSON renderings used by different `dbus-monitor` versions.
It checks snapshot-based recovery and diffing, then sends an `eventChanges`
message containing a date-moving modification and verifies direct add/modify/
remove updates, old/new event timestamps, response revision, persistence of the
recovery snapshot and `FULL_SNAPSHOT_REQUIRED` for a calendar missing from the
host cache.

For a live Plasma check, create a one-day event, modify its title and time, move
it to another date, and delete it. Existing agenda entries should remain single;
additions should appear once, edits should update in place, date moves should
disappear from the old date and appear on the new date, and deletions should
disappear without switching months or reopening the widget. Also test both a
multi-day all-day event and a timed event spanning midnight: each should be
supplied once under its start date, not once for every day it spans. A single-day
all-day event should continue to display as an all-day entry.


### Checking native-host phase timings

1. Rebuild and reinstall the project so Thunderbird launches the updated native
   host executable. Rebuilding only the XPI is not sufficient for host timings.
2. In Thunderbird, open the add-on's options and enable **Enable debug logs**.
3. Open the Error Console with Ctrl+Shift+J and trigger a calendar change, or
   wait for the next refresh.
4. Find the `Thunderbird Plasma Calendar: native host phases (ms)` line. It reports
   the wait between requests separately from receiving the current frame, followed
   by JSON parsing, validation, content hashing, cache preparation, serialisation,
   disk commit and D-Bus notification.
5. In the same Error Console, find the native-host stderr line beginning
   `native-host message timing (ms)`. It reports current-frame reading, host
   processing, response-frame writing and **total message handling**. Compare that
   total with `native host acknowledgement` in the extension's refresh line. They
   should be close; their difference is mainly extension/runtime-to-host transport
   and scheduling overhead. The `wait before message` value is deliberately excluded
   from both current-frame reading and total message handling: it measures the idle
   interval between native-messaging requests, which can include the time Thunderbird
   spends querying calendars before it sends the next snapshot.

The host uses `poll()` to detect readable input before starting the frame timer, and
reads directly from the file descriptor so C++ stream-buffer prefetch cannot hide a
subsequent frame from `poll()`. Total message handling starts when the first bytes
become readable and ends after the response frame is written. Timing diagnostics are
emitted only when the request carries `debugLogs: true`; actionable payload-size
rejections remain visible regardless of that preference.

## License

GPL-2.0-or-later. C++, QML and JavaScript sources carry SPDX identifiers.
