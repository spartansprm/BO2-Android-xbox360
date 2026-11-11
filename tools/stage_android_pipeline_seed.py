#!/usr/bin/env python3


import argparse
from pathlib import Path
import struct

from pipelines_list import read_list

MAX_RECORDS = 4096
RECORD_BYTES = 424
MAGIC = 0x4C505A42
VERSION = 1


def stage(source: Path, output: Path) -> int:
    size, records = read_list(str(source))
    if size != RECORD_BYTES or not 0 < len(records) <= MAX_RECORDS:
        raise ValueError("Incompatible pipeline list (expected BZPL v1 / 424-byte records / 1..4096 entries)")
    for i, record in enumerate(records):
        if len(record) != RECORD_BYTES:
            raise ValueError(f"Truncated record {i}")
        attributes, bindings = struct.unpack_from("<II", record, 96)
        if attributes > 16 or bindings > 16:
            raise ValueError(f"Invalid vertex input at record {i}")
    destination = output / "pipeline-seed" / "bo2-prewarm-list.bin"
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(".tmp")
    with temporary.open("wb") as stream:
        stream.write(struct.pack("<IIII", MAGIC, VERSION, size, len(records)))
        for record in records:
            stream.write(record)
    temporary.replace(destination)
    return len(records)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(f"Staged {stage(args.input, args.output)} pipelines into private APK assets")
