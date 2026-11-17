#!/usr/bin/env python3
"""Create a Thunderbird XPI without relying on external zip utilities.

Nix source archives commonly give every file an mtime of 1970-01-01.
ZIP's DOS timestamp format cannot represent dates before 1980, so build each
ZipInfo explicitly and clamp timestamps to the representable range.
"""

import argparse
import os
import stat
from datetime import datetime, timezone
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ZIP_MIN_TIMESTAMP = datetime(1980, 1, 1, 0, 0, 0, tzinfo=timezone.utc)
ZIP_MAX_TIMESTAMP = datetime(2107, 12, 31, 23, 59, 58, tzinfo=timezone.utc)


def zip_timestamp(mtime: float) -> tuple[int, int, int, int, int, int]:
    """Return an even-second UTC timestamp within ZIP's DOS date range."""
    try:
        stamp = datetime.fromtimestamp(mtime, tz=timezone.utc)
    except (OverflowError, OSError, ValueError):
        stamp = ZIP_MIN_TIMESTAMP if mtime < 0 else ZIP_MAX_TIMESTAMP

    stamp = min(max(stamp, ZIP_MIN_TIMESTAMP), ZIP_MAX_TIMESTAMP)
    # DOS timestamps have a two-second resolution.
    second = stamp.second - (stamp.second % 2)
    return (stamp.year, stamp.month, stamp.day, stamp.hour, stamp.minute, second)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    source = args.source.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    # Honour SOURCE_DATE_EPOCH for reproducible builds. Otherwise preserve the
    # source file timestamps, clamped to the ZIP format's supported range.
    source_date_epoch = os.environ.get("SOURCE_DATE_EPOCH")
    reproducible_mtime = int(source_date_epoch) if source_date_epoch is not None else None

    with ZipFile(
        args.output,
        "w",
        compression=ZIP_DEFLATED,
        compresslevel=9,
    ) as archive:
        for path in sorted(source.rglob("*")):
            if not path.is_file() or path.name.endswith(("~", ".swp")):
                continue

            file_stat = path.stat()
            mtime = reproducible_mtime if reproducible_mtime is not None else file_stat.st_mtime
            info = ZipInfo(
                path.relative_to(source).as_posix(),
                date_time=zip_timestamp(mtime),
            )
            info.compress_type = ZIP_DEFLATED
            info.create_system = 3  # Unix; retain the source file's permissions.
            info.external_attr = (stat.S_IFREG | stat.S_IMODE(file_stat.st_mode)) << 16
            info.flag_bits |= 0x800  # UTF-8 filenames.
            archive.writestr(info, path.read_bytes())


if __name__ == "__main__":
    main()
