#!/usr/bin/env python3
"""Integration test for the native host's independent 32 MiB frame guard.

Usage: python3 tests/native-host-limit.py /path/to/thunderbird-calendar-native-host
"""
from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

MAX_BYTES = 32 * 1024 * 1024
TEST_BYTES = MAX_BYTES + 1


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} /path/to/thunderbird-calendar-native-host", file=sys.stderr)
        return 2

    host = sys.argv[1]
    frame = struct.pack("<I", TEST_BYTES) + (b"x" * TEST_BYTES)
    with tempfile.TemporaryDirectory(prefix="tb-calendar-limit-test-") as cache_home:
        env = os.environ.copy()
        env["XDG_CACHE_HOME"] = cache_home
        runtime_dir = Path(cache_home) / "runtime"
        runtime_dir.mkdir(mode=0o700)
        runtime_dir.chmod(0o700)
        env["XDG_RUNTIME_DIR"] = str(runtime_dir)
        completed = subprocess.run(
            [host], input=frame, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env, check=False
        )
    if completed.returncode != 0:
        print(completed.stderr.decode("utf-8", errors="replace"), file=sys.stderr)
        raise SystemExit(f"native host exited unexpectedly with status {completed.returncode}")
    if len(completed.stdout) < 4:
        raise SystemExit("native host did not return a framed error response")

    response_length = struct.unpack("<I", completed.stdout[:4])[0]
    response_payload = completed.stdout[4:4 + response_length]
    if len(response_payload) != response_length:
        raise SystemExit("native host returned a truncated response")
    response = json.loads(response_payload)

    assert response.get("ok") is False, response
    assert response.get("code") == "NATIVE_MESSAGE_TOO_LARGE", response
    assert response.get("payloadBytes") == TEST_BYTES, response
    assert response.get("maxBytes") == MAX_BYTES, response
    assert "too large" in response.get("error", "").lower(), response
    assert "32.00 MiB" in response.get("error", ""), response
    print("Native host limit test passed:")
    print(response["error"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
