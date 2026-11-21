#!/usr/bin/env python3
"""Verify native-host per-frame timing and total message-handling diagnostics.

Usage: python3 tests/native-host-timing.py /path/to/thunderbird-calendar-native-host
"""
from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} /path/to/thunderbird-calendar-native-host", file=sys.stderr)
        return 2

    host = sys.argv[1]
    request = {
        "type": "snapshot",
        "requestId": "native-host-timing-test",
        "debugLogs": True,
        "snapshot": {"schemaVersion": 1, "calendars": []},
    }
    payload = json.dumps(request, separators=(",", ":")).encode("utf-8")
    frame = struct.pack("<I", len(payload)) + payload

    with tempfile.TemporaryDirectory(prefix="tb-calendar-host-test-") as cache_home:
        env = os.environ.copy()
        env["XDG_CACHE_HOME"] = cache_home
        completed = subprocess.run(
            [host], input=frame, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env, check=False, timeout=15,
        )

    if completed.returncode != 0:
        print(completed.stderr.decode("utf-8", errors="replace"), file=sys.stderr)
        raise SystemExit(f"native host exited unexpectedly with status {completed.returncode}")
    if len(completed.stdout) < 4:
        raise SystemExit("native host did not return a framed response")

    response_length = struct.unpack("<I", completed.stdout[:4])[0]
    response_payload = completed.stdout[4:4 + response_length]
    if len(response_payload) != response_length:
        raise SystemExit("native host returned a truncated response")
    response = json.loads(response_payload)
    if response.get("ok") is not True:
        raise SystemExit(f"native host rejected timing test snapshot: {response}")
    timings = response.get("hostTimingsMs", {})
    for field in ("messageWaitMs", "frameReadMs", "hostProcessingMs"):
        if field not in timings:
            raise SystemExit(f"native host response omitted {field}: {response}")

    stderr = completed.stderr.decode("utf-8", errors="replace")
    required = (
        "native-host message timing (ms):",
        "wait before message",
        "frame read",
        "host processing",
        "response frame write",
        "total message handling",
    )
    missing = [part for part in required if part not in stderr]
    if missing:
        raise SystemExit(f"native host stderr omitted timing fields {missing}:\n{stderr}")

    print("Native host timing test passed:")
    print(stderr.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
