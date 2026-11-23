#!/usr/bin/env python3
"""Exercise native-host snapshot diffing and D-Bus event-delta delivery.

Usage: python3 tests/native-host-event-deltas.py /path/to/thunderbird-calendar-native-host
The test creates its own private D-Bus session with dbus-run-session.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

BUS_INTERFACE = "org.kde.ThunderbirdCalendar"
BUS_PATH = "/org/kde/ThunderbirdCalendar"


def decode_dbus_string(argument: str) -> str:
    """Decode a string argument as rendered by different dbus-monitor versions.

    Some versions print JSON string contents verbatim inside the outer D-Bus
    quotes (so the inner JSON quotes are not escaped); others render the value
    as a conventional escaped string. Accept both representations.
    """
    prefix = "string "
    if not argument.startswith(prefix):
        raise RuntimeError(f"D-Bus argument was not a string: {argument!r}")

    encoded = argument[len(prefix):].strip()
    if len(encoded) < 2 or encoded[0] != '"' or encoded[-1] != '"':
        raise RuntimeError(f"D-Bus string was not surrounded by quotes: {argument!r}")

    # First try the literal contents. This handles dbus-monitor output such as:
    # string "{"batchCount":1,"deltas":[]}"
    literal_contents = encoded[1:-1]
    try:
        json.loads(literal_contents)
        return literal_contents
    except json.JSONDecodeError:
        pass

    # Then try a fully escaped D-Bus string, e.g.:
    # string "{\"batchCount\":1,\"deltas\":[]}"
    try:
        decoded = json.loads(encoded)
        if isinstance(decoded, str):
            json.loads(decoded)  # Ensure the decoded value is the JSON payload expected here.
            return decoded
    except json.JSONDecodeError:
        pass

    raise RuntimeError(f"could not decode D-Bus JSON string argument: {argument!r}")


def test_dbus_string_decoder() -> None:
    payload = '{"batchCount":1,"deltas":[{"title":"Moved"}]}'
    raw_rendering = f'string "{payload}"'
    escaped_rendering = "string " + json.dumps(payload)
    assert json.loads(decode_dbus_string(raw_rendering)) == json.loads(payload)
    assert json.loads(decode_dbus_string(escaped_rendering)) == json.loads(payload)


def read_exact(stream, length: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < length:
        chunk = stream.read(length - len(chunks))
        if not chunk:
            raise RuntimeError("native host closed stdout before completing its response")
        chunks.extend(chunk)
    return bytes(chunks)


def send_request(host, request: dict) -> dict:
    payload = json.dumps(request, separators=(",", ":")).encode("utf-8")
    host.stdin.write(struct.pack("<I", len(payload)))
    host.stdin.write(payload)
    host.stdin.flush()
    length = struct.unpack("<I", read_exact(host.stdout, 4))[0]
    return json.loads(read_exact(host.stdout, length))


def wait_signal(lines: "queue.Queue[str]", member: str, timeout: float = 8.0) -> str | None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            line = lines.get(timeout=max(0.0, deadline - time.monotonic()))
        except queue.Empty:
            return None
        if f"member={member}" not in line:
            continue
        if member != "EventsDelta":
            return ""
        # dbus-monitor emits each signal argument on the line after its header.
        try:
            argument = lines.get(timeout=max(0.0, deadline - time.monotonic())).strip()
        except queue.Empty:
            raise RuntimeError("timed out while reading the EventsDelta signal argument")
        if not argument.startswith("string "):
            raise RuntimeError(f"EventsDelta argument was not a D-Bus string: {argument!r}")
        return decode_dbus_string(argument)
    return None


def read_lines(stream, output: "queue.Queue[str]") -> None:
    for line in iter(stream.readline, ""):
        output.put(line)


def read_binary_lines(stream, output: "queue.Queue[str]") -> None:
    for line in iter(stream.readline, b""):
        output.put(line.decode("utf-8", errors="replace"))


def queued_diagnostics(lines: "queue.Queue[str]") -> str:
    collected = []
    while True:
        try:
            collected.append(lines.get_nowait())
        except queue.Empty:
            break
    return "".join(collected).strip()




def snapshot_checkpoint(snapshot_digest: str, revision: str) -> str:
    payload = (
        b"thunderbird-plasma-calendar:snapshot-checkpoint:v1\n"
        + snapshot_digest.encode("ascii") + b"\n" + revision.encode("ascii")
    )
    return hashlib.sha256(payload).hexdigest()


def delta_revision(previous_revision: str, deltas: list[dict]) -> str:
    """Independent reference implementation of the v2 journal hash chain."""
    compact_deltas = json.dumps(
        deltas, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    payload = (
        b"thunderbird-plasma-calendar:delta-chain:v1\n"
        + previous_revision.encode("ascii")
        + b"\n"
        + compact_deltas
    )
    return hashlib.sha256(payload).hexdigest()


def make_event(uid: str, title: str, start: str = "2026-11-21T09:00:00.000Z",
               end: str = "2026-11-21T10:00:00.000Z") -> dict:
    return {
        "uid": uid,
        "title": title,
        "description": "",
        "location": "",
        "start": start,
        "end": end,
        "allDay": False,
        "kind": "event",
        "color": "#123456",
    }


def make_snapshot(events: list[dict]) -> dict:
    return {
        "schemaVersion": 1,
        "generatedAt": "2026-11-21T05:00:00.000Z",
        "rangeStart": "2026-05-01T00:00:00.000Z",
        "rangeEnd": "2027-11-01T00:00:00.000Z",
        "calendars": [{
            "id": "test-calendar",
            "name": "Test calendar",
            "color": "#123456",
            "type": "memory",
            "status": "ok",
            "events": events,
        }],
    }


def inside_test(host_path: Path) -> int:
    dbus_monitor = shutil.which("dbus-monitor")
    if not dbus_monitor:
        raise SystemExit("dbus-monitor is required for this test")

    with tempfile.TemporaryDirectory(prefix="tb-calendar-delta-test-") as cache_home:
        env = os.environ.copy()
        env["XDG_CACHE_HOME"] = cache_home
        # Keep the per-user runtime lock isolated from any real desktop session
        # while all host processes in this test still share the same lock.
        runtime_dir = Path(cache_home) / "runtime"
        runtime_dir.mkdir(mode=0o700)
        runtime_dir.chmod(0o700)
        env["XDG_RUNTIME_DIR"] = str(runtime_dir)
        monitor = subprocess.Popen(
            [dbus_monitor, "--session",
             f"type='signal',path='{BUS_PATH}',interface='{BUS_INTERFACE}'"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=env,
        )
        monitor_lines: queue.Queue[str] = queue.Queue()
        monitor_stderr_lines: queue.Queue[str] = queue.Queue()
        stdout_reader = threading.Thread(target=read_lines, args=(monitor.stdout, monitor_lines), daemon=True)
        stderr_reader = threading.Thread(target=read_lines, args=(monitor.stderr, monitor_stderr_lines), daemon=True)
        stdout_reader.start()
        stderr_reader.start()
        host = None
        host_stderr_lines: queue.Queue[str] = queue.Queue()
        host_stderr_reader = None
        try:
            # A fixed startup sleep is racy: dbus-monitor may not yet have
            # installed its AddMatch rule. Repeatedly send a harmless probe and
            # wait until this exact monitor observes one. The first observed
            # probe proves that the subscription is active before the host is
            # started and its EventsChanged signal can be emitted.
            dbus_send = shutil.which("dbus-send")
            if not dbus_send:
                raise SystemExit("dbus-send is required for the D-Bus monitor readiness handshake")
            ready = False
            deadline = time.monotonic() + 8.0
            while time.monotonic() < deadline and not ready:
                probe = subprocess.run(
                    [dbus_send, "--session", "--type=signal", BUS_PATH,
                     f"{BUS_INTERFACE}.TestMonitorReady"],
                    env=env, capture_output=True, text=True, timeout=3, check=False
                )
                if probe.returncode != 0:
                    raise AssertionError(
                        "could not send D-Bus monitor readiness probe: "
                        + (probe.stderr or probe.stdout).strip()
                    )
                ready = wait_signal(monitor_lines, "TestMonitorReady", timeout=0.2) is not None
            if not ready:
                raise AssertionError(
                    "dbus-monitor did not receive its readiness probe; stderr: "
                    + queued_diagnostics(monitor_stderr_lines)
                )

            host = subprocess.Popen(
                [str(host_path)],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=env,
            )
            host_stderr_reader = threading.Thread(
                target=read_binary_lines, args=(host.stderr, host_stderr_lines), daemon=True
            )
            host_stderr_reader.start()
            initial = make_snapshot([
                make_event("test-calendar:event-1:", "Original title"),
                make_event("test-calendar:event-3:", "Event to remove"),
            ])
            response = send_request(host, {
                "type": "snapshot", "requestId": "initial", "debugLogs": False,
                "snapshot": initial,
            })
            assert response.get("ok") is True, response
            assert response.get("requestId") == "initial", response
            if wait_signal(monitor_lines, "EventsChanged") is None:
                raise AssertionError(
                    "initial snapshot was acknowledged but the D-Bus monitor did not observe EventsChanged; "
                    "this can indicate a host notification failure or a bus mismatch. "
                    f"native-host stderr: {queued_diagnostics(host_stderr_lines) or '<empty>'}; "
                    f"dbus-monitor stderr: {queued_diagnostics(monitor_stderr_lines) or '<empty>'}"
                )
            cache_file = Path(cache_home) / "plasma-thunderbird-calendar" / "events.json"
            initial_stored = json.loads(cache_file.read_text(encoding="utf-8"))
            initial_revision = initial_stored["contentRevision"]
            assert response.get("revision") == initial_revision, response
            assert initial_stored["revisionAlgorithm"] == "delta-chain-v1", initial_stored
            assert initial_stored["snapshotDigest"] == initial_revision, initial_stored
            assert initial_stored["revisionCheckpoint"] == snapshot_checkpoint(
                initial_stored["snapshotDigest"], initial_revision
            ), initial_stored
            initial_journal = cache_file.with_name("events.journal").read_text(encoding="utf-8").splitlines()
            assert len(initial_journal) == 1, initial_journal
            assert json.loads(initial_journal[0])["schemaVersion"] == 2, initial_journal

            # The native host owns one per-user cache lock. A second Thunderbird
            # profile/process must be rejected rather than sharing the same
            # snapshot and revision journal concurrently.
            competing_host = subprocess.Popen(
                [str(host_path)],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=env,
            )
            try:
                competing_response = send_request(competing_host, {
                    "type": "snapshot", "requestId": "competing-profile", "debugLogs": False,
                    "snapshot": initial,
                })
                assert competing_response.get("ok") is False, competing_response
                assert competing_response.get("code") == "ACTIVE_PROFILE_LOCKED", competing_response
            finally:
                if competing_host.stdin and not competing_host.stdin.closed:
                    competing_host.stdin.close()
                competing_host.wait(timeout=5)
                if competing_host.stdout:
                    competing_host.stdout.close()
                if competing_host.stderr:
                    competing_host.stderr.close()
            # The original owner must remain usable after rejecting the second host.
            owner_response = send_request(host, {
                "type": "snapshot", "requestId": "lock-owner-still-active", "debugLogs": False,
                "snapshot": initial,
            })
            assert owner_response.get("ok") is True, owner_response

            updated = make_snapshot([
                make_event("test-calendar:event-1:", "Modified title"),
                make_event("test-calendar:event-2:", "New event"),
            ])
            response = send_request(host, {
                "type": "snapshot", "requestId": "delta", "debugLogs": False,
                "snapshot": updated,
            })
            assert response.get("ok") is True, response
            assert response.get("requestId") == "delta", response

            payload = wait_signal(monitor_lines, "EventsDelta")
            if payload is None:
                raise AssertionError("changed snapshot did not emit EventsDelta")
            envelope = json.loads(payload)
            assert envelope.get("schemaVersion") == 1, envelope
            deltas = envelope.get("deltas", [])
            operations = {(delta.get("operation"), delta.get("event", {}).get("uid")): delta
                          for delta in deltas}
            assert set(operations) == {
                ("modify", "test-calendar:event-1:"),
                ("add", "test-calendar:event-2:"),
                ("remove", "test-calendar:event-3:"),
            }, operations
            assert operations[("modify", "test-calendar:event-1:")]["previous"]["title"] == "Original title"
            assert operations[("modify", "test-calendar:event-1:")]["event"]["title"] == "Modified title"

            stored = json.loads(cache_file.read_text(encoding="utf-8"))
            stored_events = stored["calendars"][0]["events"]
            assert {event["uid"] for event in stored_events} == {
                "test-calendar:event-1:", "test-calendar:event-2:"
            }
            second_revision = stored["contentRevision"]
            assert envelope["previousRevision"] == initial_revision
            assert envelope["revision"] == second_revision

            # A calendar-metadata-only change advances the revision with an
            # empty batch. This keeps the plugin's next delta base in sync even
            # when no EventData needs to be changed.
            metadata_only = make_snapshot([
                make_event("test-calendar:event-1:", "Modified title"),
                make_event("test-calendar:event-2:", "New event"),
            ])
            metadata_only["calendars"][0]["name"] = "Renamed test calendar"
            response = send_request(host, {
                "type": "snapshot", "requestId": "metadata", "debugLogs": False,
                "snapshot": metadata_only,
            })
            assert response.get("ok") is True, response
            metadata_payload = wait_signal(monitor_lines, "EventsDelta")
            if metadata_payload is None:
                raise AssertionError("metadata-only update did not emit an empty EventsDelta")
            metadata_envelope = json.loads(metadata_payload)
            assert metadata_envelope["deltas"] == [], metadata_envelope
            assert metadata_envelope["previousRevision"] == second_revision
            stored = json.loads(cache_file.read_text(encoding="utf-8"))
            metadata_revision = stored["contentRevision"]
            assert metadata_envelope["revision"] == metadata_revision
            metadata_journal = cache_file.with_name("events.journal").read_text(encoding="utf-8").splitlines()
            assert len(metadata_journal) == 1, metadata_journal
            assert json.loads(metadata_journal[0])["baseRevision"] == metadata_revision

            # More than one signal's worth of changes must be split into
            # ordered batches, all sharing the same revision pair.
            many_events = [
                make_event("test-calendar:event-1:", "Modified title"),
                make_event("test-calendar:event-2:", "New event"),
            ]
            many_events.extend(
                make_event(f"test-calendar:bulk-{index}:", f"Bulk event {index}")
                for index in range(65)
            )
            bulk = make_snapshot(many_events)
            bulk["calendars"][0]["name"] = "Renamed test calendar"
            response = send_request(host, {
                "type": "snapshot", "requestId": "bulk", "debugLogs": False,
                "snapshot": bulk,
            })
            assert response.get("ok") is True, response
            batch_payloads = [wait_signal(monitor_lines, "EventsDelta"), wait_signal(monitor_lines, "EventsDelta")]
            if any(payload is None for payload in batch_payloads):
                raise AssertionError("65 additions were not emitted as two EventsDelta batches")
            batches = [json.loads(payload) for payload in batch_payloads]
            batches.sort(key=lambda item: item["batchIndex"])
            assert [item["batchIndex"] for item in batches] == [0, 1], batches
            assert all(item["batchCount"] == 2 for item in batches), batches
            assert [len(item["deltas"]) for item in batches] == [64, 1], batches
            assert all(item["previousRevision"] == metadata_revision for item in batches), batches
            assert len({item["revision"] for item in batches}) == 1, batches
            assert all(delta["operation"] == "add" for item in batches for delta in item["deltas"])
            stored = json.loads(cache_file.read_text(encoding="utf-8"))
            stored_events = stored["calendars"][0]["events"]
            assert len(stored_events) == 67, len(stored_events)
            assert stored["contentRevision"] == batches[0]["revision"]

            # Direct observer updates are written to the append-only journal,
            # not by rewriting events.json for every individual change.
            direct_changes = [
                {"operation": "modify", "calendarId": "test-calendar",
                 "event": make_event("test-calendar:event-1:", "Directly modified title",
                                     "2026-11-22T11:00:00.000Z", "2026-11-22T12:00:00.000Z")},
                {"operation": "remove", "calendarId": "test-calendar",
                 "event": make_event("test-calendar:bulk-0:", "Bulk event 0")},
                {"operation": "add", "calendarId": "test-calendar",
                 "event": make_event("test-calendar:direct-new:", "Direct observer addition")},
                # Intentionally provide additions in non-sorted order. The host
                # and readers must use deterministic UID ordering when replaying
                # the journal so the reconstructed content revision is stable.
                {"operation": "add", "calendarId": "test-calendar",
                 "event": make_event("test-calendar:direct-z:", "Direct Z addition")},
                {"operation": "add", "calendarId": "test-calendar",
                 "event": make_event("test-calendar:direct-a:", "Direct A addition")},
            ]
            response = send_request(host, {
                "type": "eventChanges", "requestId": "direct", "debugLogs": True,
                "changes": direct_changes,
            })
            assert response.get("ok") is True, response
            assert response.get("requestId") == "direct", response
            direct_timings = response.get("hostTimingsMs", {})
            required_direct_timings = (
                "messageType", "frameReadMs", "jsonParseMs", "cacheDirectoryMs",
                "existingCacheReadMs", "cacheValidationMs", "calendarIndexMs",
                "changeValidationMs", "eventIndexMs", "eventApplyMs",
                "eventArrayRebuildMs", "eventDiffMs", "contentHashMs",
                "snapshotSerializationMs", "diskCommitMs", "dbusNotifyMs",
                "journalReplayMs", "journalSerializationMs", "journalAppendMs",
                "journalFsyncMs", "journalCompactionMs", "journalRecordCount",
                "journalFileBytes", "journalWriteBytes", "journalCompacted",
                "hostProcessingMs", "directChangeCount", "affectedCalendarCount",
                "eventDeltaCount", "cacheWriteBytes", "unchanged",
            )
            missing_direct_timings = [name for name in required_direct_timings if name not in direct_timings]
            assert not missing_direct_timings, ("direct update response omitted timing fields", missing_direct_timings, response)
            assert direct_timings["messageType"] == "eventChanges", direct_timings
            assert direct_timings["directChangeCount"] == 5, direct_timings
            assert direct_timings["affectedCalendarCount"] == 1, direct_timings
            assert direct_timings["eventDeltaCount"] == 5, direct_timings
            assert direct_timings["journalRecordCount"] == 1, direct_timings
            assert direct_timings["journalWriteBytes"] > 0, direct_timings
            assert direct_timings["journalCompacted"] is False, direct_timings
            # Incremental revision calculation hashes only the previous
            # revision and this small delta batch, not the whole snapshot.
            assert direct_timings["contentHashMs"] < 100.0, direct_timings

            direct_payload = wait_signal(monitor_lines, "EventsDelta")
            if direct_payload is None:
                raise AssertionError("direct observer changes did not emit EventsDelta")
            direct_envelope = json.loads(direct_payload)
            direct_operations = {
                (delta.get("operation"), delta.get("event", {}).get("uid")): delta
                for delta in direct_envelope.get("deltas", [])
            }
            assert set(direct_operations) == {
                ("modify", "test-calendar:event-1:"),
                ("remove", "test-calendar:bulk-0:"),
                ("add", "test-calendar:direct-new:"),
                ("add", "test-calendar:direct-z:"),
                ("add", "test-calendar:direct-a:"),
            }, direct_operations
            assert direct_operations[("modify", "test-calendar:event-1:")]["event"]["title"] == "Directly modified title"
            assert direct_operations[("modify", "test-calendar:event-1:")]["previous"]["start"] == "2026-11-21T09:00:00.000Z"
            assert direct_operations[("modify", "test-calendar:event-1:")]["event"]["start"] == "2026-11-22T11:00:00.000Z"
            direct_revision = response.get("revision")
            assert direct_envelope["previousRevision"] == batches[0]["revision"]
            assert direct_envelope["revision"] == direct_revision

            # events.json remains the last compacted full snapshot. The direct
            # changes are durable in a small JSONL journal record instead.
            compacted_stored = json.loads(cache_file.read_text(encoding="utf-8"))
            assert compacted_stored["contentRevision"] == batches[0]["revision"]
            assert len(compacted_stored["calendars"][0]["events"]) == 67
            journal_file = cache_file.with_name("events.journal")
            journal_lines = journal_file.read_text(encoding="utf-8").splitlines()
            assert len(journal_lines) == 2, journal_lines
            journal_header, journal_record = map(json.loads, journal_lines)
            assert journal_header["type"] == "header", journal_header
            assert journal_header["schemaVersion"] == 2, journal_header
            assert journal_header["baseRevision"] == batches[0]["revision"], journal_header
            assert journal_record["previousRevision"] == batches[0]["revision"], journal_record
            assert journal_record["schemaVersion"] == 2, journal_record
            assert journal_record["revision"] == direct_revision, journal_record
            assert len(journal_record["deltas"]) == 5, journal_record
            assert direct_revision == delta_revision(
                batches[0]["revision"], journal_record["deltas"]
            ), (direct_revision, journal_record)

            # Restarting the host must replay the durable journal over the last
            # compacted snapshot before accepting another direct change.
            host.stdin.close()
            host.wait(timeout=5)
            if host_stderr_reader is not None:
                host_stderr_reader.join(timeout=1)
            if host.returncode != 0:
                raise AssertionError(f"native host exited with {host.returncode}: "
                                     f"{queued_diagnostics(host_stderr_lines)}")

            # Simulate a process crash while the last JSONL append is being
            # written. The incomplete, unacknowledged tail must be discarded
            # while all preceding fsync'd records remain replayable.
            with journal_file.open("ab") as journal_stream:
                journal_stream.write(b'{"type":"delta","schemaVersion":2,"torn":')
                journal_stream.flush()

            host = subprocess.Popen(
                [str(host_path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, env=env,
            )
            host_stderr_reader = threading.Thread(
                target=read_binary_lines, args=(host.stderr, host_stderr_lines), daemon=True
            )
            host_stderr_reader.start()
            recovery_change = {
                "operation": "modify", "calendarId": "test-calendar",
                "event": make_event("test-calendar:direct-new:", "Recovered then modified"),
            }
            response = send_request(host, {
                "type": "eventChanges", "requestId": "journal-replay", "debugLogs": True,
                "changes": [recovery_change],
            })
            assert response.get("ok") is True, response
            replay_payload = wait_signal(monitor_lines, "EventsDelta")
            if replay_payload is None:
                raise AssertionError("host did not emit a delta after replaying its journal")
            replay_envelope = json.loads(replay_payload)
            assert replay_envelope["previousRevision"] == direct_revision, replay_envelope
            replay_revision = response.get("revision")
            assert replay_envelope["revision"] == replay_revision, replay_envelope
            replay_timings = response.get("hostTimingsMs", {})
            assert replay_timings.get("journalRecordsReplayed") == 1, replay_timings
            repaired_journal_lines = journal_file.read_text(encoding="utf-8").splitlines()
            # The repaired journal contains its header, the original direct
            # update record replayed after restart, and the new recovery_change
            # record appended by the request above. The incomplete torn tail
            # must be absent; expecting only two lines here would incorrectly
            # reject the valid post-recovery append.
            assert len(repaired_journal_lines) == 3, repaired_journal_lines
            repaired_header, original_record, recovery_record = map(
                json.loads, repaired_journal_lines
            )
            assert repaired_header["type"] == "header", repaired_header
            assert repaired_header["baseRevision"] == batches[0]["revision"], repaired_header
            assert original_record["previousRevision"] == batches[0]["revision"], original_record
            assert original_record["revision"] == direct_revision, original_record
            assert recovery_record["previousRevision"] == direct_revision, recovery_record
            assert recovery_record["revision"] == replay_revision, recovery_record
            assert replay_revision == delta_revision(
                direct_revision, recovery_record["deltas"]
            ), (replay_revision, recovery_record)
            assert recovery_record["type"] == "delta", recovery_record
            assert recovery_record["schemaVersion"] == 2, recovery_record
            assert len(recovery_record["deltas"]) == 1, recovery_record
            assert recovery_record["deltas"][0]["operation"] == "modify", recovery_record
            assert recovery_record["deltas"][0]["event"]["uid"] == "test-calendar:direct-new:", recovery_record
            assert recovery_record["deltas"][0]["event"]["title"] == "Recovered then modified", recovery_record

            # A complete snapshot matching the recovered journal state is
            # recognised as unchanged, proving replay reconstructed the same
            # event collection rather than merely accepting the next delta.
            recovered_events = [
                make_event("test-calendar:event-1:", "Directly modified title",
                           "2026-11-22T11:00:00.000Z", "2026-11-22T12:00:00.000Z"),
                make_event("test-calendar:event-2:", "New event"),
            ]
            recovered_events.extend(
                make_event(f"test-calendar:bulk-{index}:", f"Bulk event {index}")
                for index in range(1, 65)
            )
            recovered_events.extend([
                make_event("test-calendar:direct-a:", "Direct A addition"),
                make_event("test-calendar:direct-new:", "Recovered then modified"),
                make_event("test-calendar:direct-z:", "Direct Z addition"),
            ])
            recovered_snapshot = make_snapshot(recovered_events)
            recovered_snapshot["calendars"][0]["name"] = "Renamed test calendar"
            response = send_request(host, {
                "type": "snapshot", "requestId": "verify-replay", "debugLogs": True,
                "snapshot": recovered_snapshot,
            })
            assert response.get("ok") is True, response
            assert response.get("hostTimingsMs", {}).get("unchanged") is True, response

            # Exercise journal compaction at its record threshold (128). The
            # host should atomically write one new full snapshot and reset the
            # journal to a single header line.
            compaction_response = None
            for index in range(126):
                event = make_event(
                    "test-calendar:direct-new:", f"Compaction update {index + 1}"
                )
                compaction_response = send_request(host, {
                    "type": "eventChanges", "requestId": f"compact-{index + 1}",
                    "debugLogs": index == 125,
                    "changes": [{
                        "operation": "modify", "calendarId": "test-calendar", "event": event
                    }],
                })
                assert compaction_response.get("ok") is True, compaction_response
            assert compaction_response is not None
            compact_timings = compaction_response.get("hostTimingsMs", {})
            assert compact_timings.get("journalCompacted") is True, compact_timings
            assert compact_timings.get("journalRecordCount") == 0, compact_timings
            compacted_stored = json.loads(cache_file.read_text(encoding="utf-8"))
            compacted_events = {event["uid"]: event for event in compacted_stored["calendars"][0]["events"]}
            assert compacted_events["test-calendar:direct-new:"]["title"] == "Compaction update 126"
            compacted_journal_lines = journal_file.read_text(encoding="utf-8").splitlines()
            assert len(compacted_journal_lines) == 1, compacted_journal_lines
            compacted_header = json.loads(compacted_journal_lines[0])
            assert compacted_header["schemaVersion"] == 2, compacted_header
            assert compacted_header["baseRevision"] == compacted_stored["contentRevision"], compacted_header
            assert compacted_stored["revisionAlgorithm"] == "delta-chain-v1", compacted_stored
            assert len(compacted_stored["snapshotDigest"]) == 64, compacted_stored
            assert compacted_stored["revisionCheckpoint"] == snapshot_checkpoint(
                compacted_stored["snapshotDigest"], compacted_stored["contentRevision"]
            ), compacted_stored

            # Restart after compaction and append another delta. This verifies
            # that the compacted snapshot digest anchors the v2 revision chain
            # and that a new process continues the same revision sequence.
            host.stdin.close()
            host.wait(timeout=5)
            if host_stderr_reader is not None:
                host_stderr_reader.join(timeout=1)
            host = subprocess.Popen(
                [str(host_path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, env=env,
            )
            host_stderr_reader = threading.Thread(
                target=read_binary_lines, args=(host.stderr, host_stderr_lines), daemon=True
            )
            host_stderr_reader.start()
            post_compaction_change = {
                "operation": "modify", "calendarId": "test-calendar",
                "event": make_event("test-calendar:direct-new:", "After compacted restart"),
            }
            post_compaction_response = send_request(host, {
                "type": "eventChanges", "requestId": "post-compaction-restart", "debugLogs": True,
                "changes": [post_compaction_change],
            })
            assert post_compaction_response.get("ok") is True, post_compaction_response
            post_compaction_timing = post_compaction_response.get("hostTimingsMs", {})
            assert post_compaction_timing.get("journalRecordsReplayed") == 0, post_compaction_timing
            assert post_compaction_timing.get("contentHashMs", 1e9) < 100.0, post_compaction_timing
            post_compaction_lines = journal_file.read_text(encoding="utf-8").splitlines()
            assert len(post_compaction_lines) == 2, post_compaction_lines
            assert json.loads(post_compaction_lines[0])["schemaVersion"] == 2, post_compaction_lines
            post_compaction_record = json.loads(post_compaction_lines[1])
            assert post_compaction_record["schemaVersion"] == 2, post_compaction_record
            assert post_compaction_response["revision"] == delta_revision(
                compacted_stored["contentRevision"], post_compaction_record["deltas"]
            ), post_compaction_record

            # If an event arrives for a calendar that is absent from the host's
            # recovery snapshot, the extension must rebuild a full snapshot.
            response = send_request(host, {
                "type": "eventChanges", "requestId": "missing-calendar", "debugLogs": False,
                "changes": [{
                    "operation": "add", "calendarId": "new-calendar",
                    "event": make_event("new-calendar:event:", "Needs full snapshot")
                }],
            })
            assert response.get("ok") is False, response
            assert response.get("code") == "FULL_SNAPSHOT_REQUIRED", response

            host.stdin.close()
            host.wait(timeout=5)
            if host_stderr_reader is not None:
                host_stderr_reader.join(timeout=1)
            if host.returncode != 0:
                raise AssertionError(f"native host exited with {host.returncode}: "
                                     f"{queued_diagnostics(host_stderr_lines)}")

            # Seed an old v1 snapshot and header, then verify the first direct
            # update is preceded by a safe full-snapshot migration to v2.
            legacy_cache_home = Path(cache_home) / "legacy-cache-home"
            legacy_cache_home.mkdir()
            legacy_runtime = legacy_cache_home / "runtime"
            legacy_runtime.mkdir(mode=0o700)
            legacy_cache_dir = legacy_cache_home / "plasma-thunderbird-calendar"
            legacy_cache_dir.mkdir(mode=0o700)
            legacy_snapshot = make_snapshot([make_event("legacy-calendar:event:", "Before upgrade")])
            digest_input = {
                key: value for key, value in legacy_snapshot.items()
                if key not in ("generatedAt", "rangeStart", "rangeEnd", "contentRevision",
                               "revisionAlgorithm", "snapshotDigest")
            }
            legacy_revision = hashlib.sha256(json.dumps(
                digest_input, sort_keys=True, separators=(",", ":"), ensure_ascii=False
            ).encode("utf-8")).hexdigest()
            legacy_snapshot["contentRevision"] = legacy_revision
            (legacy_cache_dir / "events.json").write_text(
                json.dumps(legacy_snapshot, sort_keys=True, separators=(",", ":")), encoding="utf-8"
            )
            (legacy_cache_dir / "events.journal").write_text(
                json.dumps({"type": "header", "schemaVersion": 1, "baseRevision": legacy_revision},
                           sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8",
            )
            legacy_env = env.copy()
            legacy_env["XDG_CACHE_HOME"] = str(legacy_cache_home)
            legacy_env["XDG_RUNTIME_DIR"] = str(legacy_runtime)
            legacy_host = subprocess.Popen(
                [str(host_path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, env=legacy_env,
            )
            legacy_stderr = queue.Queue()
            legacy_reader = threading.Thread(
                target=read_binary_lines, args=(legacy_host.stderr, legacy_stderr), daemon=True
            )
            legacy_reader.start()
            legacy_response = send_request(legacy_host, {
                "type": "eventChanges", "requestId": "legacy-v1-migration", "debugLogs": True,
                "changes": [{
                    "operation": "modify", "calendarId": "test-calendar",
                    "event": make_event("legacy-calendar:event:", "After upgrade"),
                }],
            })
            assert legacy_response.get("ok") is True, legacy_response
            legacy_journal_lines = (legacy_cache_dir / "events.journal").read_text(encoding="utf-8").splitlines()
            assert len(legacy_journal_lines) == 2, legacy_journal_lines
            migrated_header, migrated_record = map(json.loads, legacy_journal_lines)
            assert migrated_header["schemaVersion"] == 2, migrated_header
            assert migrated_header["baseRevision"] == legacy_revision, migrated_header
            assert migrated_record["schemaVersion"] == 2, migrated_record
            migrated_snapshot = json.loads((legacy_cache_dir / "events.json").read_text(encoding="utf-8"))
            assert migrated_snapshot["revisionAlgorithm"] == "delta-chain-v1", migrated_snapshot
            assert migrated_snapshot["snapshotDigest"] == legacy_revision, migrated_snapshot
            assert migrated_snapshot["revisionCheckpoint"] == snapshot_checkpoint(
                migrated_snapshot["snapshotDigest"], migrated_snapshot["contentRevision"]
            ), migrated_snapshot
            legacy_host.stdin.close()
            legacy_host.wait(timeout=5)
            if legacy_host.returncode != 0:
                raise AssertionError(f"legacy migration host exited with {legacy_host.returncode}: "
                                     f"{queued_diagnostics(legacy_stderr)}")

            # The snapshot checkpoint binds the content digest to the current
            # chain tip after compaction. Corrupting only the chain-tip metadata
            # must fail recovery rather than silently discarding acknowledged
            # journal records and continuing from an unverified revision.
            tampered_snapshot = json.loads(
                (legacy_cache_dir / "events.json").read_text(encoding="utf-8")
            )
            tampered_snapshot["contentRevision"] = "0" * 64
            (legacy_cache_dir / "events.json").write_text(
                json.dumps(tampered_snapshot, sort_keys=True, separators=(",", ":")),
                encoding="utf-8",
            )
            corrupt_host = subprocess.Popen(
                [str(host_path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, env=legacy_env,
            )
            corrupt_stderr = queue.Queue()
            corrupt_reader = threading.Thread(
                target=read_binary_lines, args=(corrupt_host.stderr, corrupt_stderr), daemon=True
            )
            corrupt_reader.start()
            try:
                corrupt_response = send_request(corrupt_host, {
                    "type": "eventChanges", "requestId": "corrupt-revision-checkpoint",
                    "debugLogs": False, "changes": [{
                        "operation": "modify", "calendarId": "test-calendar",
                        "event": make_event("legacy-calendar:event:", "Must not be applied"),
                    }],
                })
                assert corrupt_response.get("ok") is False, corrupt_response
                assert corrupt_response.get("code") == "FULL_SNAPSHOT_REQUIRED", corrupt_response
            finally:
                if corrupt_host.stdin and not corrupt_host.stdin.closed:
                    corrupt_host.stdin.close()
                corrupt_host.wait(timeout=5)
                if corrupt_host.returncode != 0:
                    raise AssertionError(
                        f"corrupt-checkpoint host exited with {corrupt_host.returncode}: "
                        f"{queued_diagnostics(corrupt_stderr)}"
                    )

            # Use a realistic large cache to ensure the direct-update hash
            # timing is independent of the full snapshot size. This is close
            # to the user's 50k-event / ~21 MiB calendar and would fail the old
            # implementation, which serialised and hashed the complete snapshot
            # for each individual event update.
            perf_cache_home = Path(cache_home) / "perf-cache-home"
            perf_cache_home.mkdir()
            perf_runtime = perf_cache_home / "runtime"
            perf_runtime.mkdir(mode=0o700)
            perf_env = env.copy()
            perf_env["XDG_CACHE_HOME"] = str(perf_cache_home)
            perf_env["XDG_RUNTIME_DIR"] = str(perf_runtime)
            perf_host = subprocess.Popen(
                [str(host_path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, env=perf_env,
            )
            perf_stderr = queue.Queue()
            perf_reader = threading.Thread(
                target=read_binary_lines, args=(perf_host.stderr, perf_stderr), daemon=True
            )
            perf_reader.start()
            try:
                perf_events = []
                perf_description = "Large-calendar hash regression fixture. " * 5
                for index in range(50000):
                    event = make_event(
                        f"test-calendar:perf-{index:05d}:",
                        f"Performance fixture event {index}",
                    )
                    event["description"] = perf_description
                    perf_events.append(event)
                large_snapshot = make_snapshot(perf_events)
                large_frame_size = len(json.dumps(large_snapshot, separators=(",", ":")).encode("utf-8"))
                assert large_frame_size < 28 * 1024 * 1024, large_frame_size
                large_snapshot_response = send_request(perf_host, {
                    "type": "snapshot", "requestId": "large-hash-fixture",
                    "debugLogs": False, "snapshot": large_snapshot,
                })
                assert large_snapshot_response.get("ok") is True, large_snapshot_response
                perf_snapshot_file = (
                    perf_cache_home / "plasma-thunderbird-calendar" / "events.json"
                )
                large_stored = json.loads(perf_snapshot_file.read_text(encoding="utf-8"))
                large_anchor = large_stored["contentRevision"]
                perf_update = make_event(
                    "test-calendar:perf-25000:", "Updated performance fixture event"
                )
                perf_update["description"] = "Updated description"
                perf_response = send_request(perf_host, {
                    "type": "eventChanges", "requestId": "large-snapshot-one-event-update",
                    "debugLogs": True, "changes": [{
                        "operation": "modify", "calendarId": "test-calendar",
                        "event": perf_update,
                    }],
                })
                assert perf_response.get("ok") is True, perf_response
                perf_timings = perf_response.get("hostTimingsMs", {})
                assert perf_timings.get("directChangeCount") == 1, perf_timings
                assert perf_timings.get("eventDeltaCount") == 1, perf_timings
                assert perf_timings.get("contentHashMs", 1e9) < 10.0, (
                    f"revision hashing scaled with a {large_frame_size / (1024 * 1024):.1f} MiB "
                    f"snapshot: {perf_timings}"
                )
                perf_journal = perf_snapshot_file.with_name("events.journal")
                perf_lines = perf_journal.read_text(encoding="utf-8").splitlines()
                assert len(perf_lines) == 2, perf_lines
                perf_record = json.loads(perf_lines[1])
                assert perf_record["revision"] == delta_revision(
                    large_anchor, perf_record["deltas"]
                ), perf_record
            finally:
                if perf_host.stdin and not perf_host.stdin.closed:
                    perf_host.stdin.close()
                perf_host.wait(timeout=10)
                if perf_host.returncode != 0:
                    raise AssertionError(
                        f"large-cache performance host exited with {perf_host.returncode}: "
                        f"{queued_diagnostics(perf_stderr)}"
                    )
            print("Native-host event-delta integration test passed:")
            print("- Initial snapshot emitted EventsChanged")
            print("- A competing native-host process was rejected by the single-profile lock")
            print("- One modify, three additions and one removal emitted through EventsDelta")
            print("- Multiple direct additions replayed in deterministic event order")
            print("- Metadata-only change emitted an empty revision-advancing batch")
            print("- 65 additions were split into ordered 64 + 1 batches")
            print("- Updated full snapshot was persisted for recovery")
            print("- Direct add/modify/remove observer changes were journaled without rewriting the full snapshot")
            print("- Journal recovery reconstructed direct changes after restarting the native host")
            print("- Incomplete journal tail was discarded before the post-recovery update was appended as a second valid record")
            print("- Matching full snapshots were recognised as unchanged after journal replay")
            print("- 128 direct update records triggered atomic snapshot compaction and journal reset")
            print("- Version-2 incremental revision chains survived compaction and process restart")
            print("- Legacy version-1 journals were migrated before the next direct update")
            print("- Corrupted post-compaction revision checkpoints requested a full snapshot")
            print("- Direct updates on a 50,000-event / ~21 MiB snapshot used bounded delta-only hash time")
            print("- Independent Python reference hashing matched revisions written by the native host")
            print("- Direct-update timings cover journal serialisation, append, fsync and compaction")
            print("- Date-moving modification preserved old and new start dates in the delta")
            print("- Both raw-quoted and escaped dbus-monitor string formats decoded correctly")
            print("- Missing calendars correctly requested a full-snapshot recovery")
            return 0
        finally:
            if host is not None and host.poll() is None:
                host.kill()
                host.wait(timeout=5)
            if monitor.poll() is None:
                monitor.terminate()
                monitor.wait(timeout=5)


def main() -> int:
    test_dbus_string_decoder()
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} /path/to/thunderbird-calendar-native-host", file=sys.stderr)
        return 2
    host_path = Path(sys.argv[1]).expanduser().resolve()
    if not host_path.is_file() or not os.access(host_path, os.X_OK):
        raise SystemExit(f"native host is not an executable file: {host_path}")
    if os.environ.get("TB_CALENDAR_TEST_PRIVATE_DBUS") == "1":
        return inside_test(host_path)
    dbus_run = shutil.which("dbus-run-session")
    if not dbus_run:
        raise SystemExit("dbus-run-session is required to create an isolated D-Bus session for this test")
    env = os.environ.copy()
    env["TB_CALENDAR_TEST_PRIVATE_DBUS"] = "1"
    return subprocess.run(
        [dbus_run, "--", sys.executable, str(Path(__file__).resolve()), str(host_path)],
        env=env,
        check=False,
    ).returncode


if __name__ == "__main__":
    raise SystemExit(main())
