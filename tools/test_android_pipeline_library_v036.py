#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from stage_android_pipeline_seed import stage
from pipelines_list import read_list, write_list, main as list_main

RECORD = 424


def record(seed):
    item = bytearray(RECORD)
    struct.pack_into("<II", item, 0, seed, seed + 1)
    struct.pack_into("<II", item, 96, 2, 1)
    return bytes(item)


with tempfile.TemporaryDirectory() as temp:
    tmp = Path(temp)
    records = [record(1), record(2)]
    raw = tmp / "raw.bin"
    write_list(str(raw), RECORD, records)
    out = tmp / "apk-assets"
    assert stage(raw, out) == 2
    packaged = out / "pipeline-seed" / "bo2-prewarm-list.bin"
    assert read_list(str(packaged)) == (RECORD, records)


    cache = tmp / "export.bin"
    opaque = bytes(range(64))
    source = raw.read_bytes()
    cache.write_bytes(struct.pack("<IIQQ", 0x43505A42, 1, len(source), len(opaque)) + source + opaque)
    assert stage(cache, out) == 2
    assert packaged.read_bytes() == source
    assert opaque not in packaged.read_bytes()

    other = tmp / "other.bin"
    write_list(str(other), RECORD, [record(2), record(3)])
    combined = tmp / "merged.bin"
    list_main(["pipelines_list.py", "merge", str(combined), str(raw), str(other)])
    assert read_list(str(combined))[1] == [record(1), record(2), record(3)]

    invalid = tmp / "invalid.bin"
    bad = bytearray(record(4))
    struct.pack_into("<I", bad, 96, 17)
    write_list(str(invalid), RECORD, [bytes(bad)])
    try:
        stage(invalid, out)
        raise AssertionError("invalid input accepted")
    except ValueError:
        pass

    wrong = tmp / "wrong.bin"
    write_list(str(wrong), 420, [bytes(420)])
    try:
        stage(wrong, out)
        raise AssertionError("wrong record size accepted")
    except ValueError:
        pass

    assert stage(raw, out) == 2

def source(path):
    return (ROOT / path).read_text(encoding="utf-8")

java = source("android/app/src/main/java/org/bo2port/diagnostic/PipelineLibrary.java")
game = source("android/app/src/main/java/org/bo2port/diagnostic/GameActivity.java")
gradle = source("android/app/build.gradle")
native = source("android/game/entry.cpp")
renderer = source("app/src/native/render/bo2zm_native_draws.cpp")
assert 'game-user/cache/bo2zm_native_pipelines.bin' in java
assert 'game-user/" + PREWARM_FOLDER' in java
assert 'MAX_RECORDS = 4096' in java
assert 'RECORD_BYTES = 424' in java
assert 'new AtomicFile(target)' in java
assert 'bo2-imported.bin' in java and 'bo2-bundled.bin' in java
assert 'PipelineLibrary.installBundledList(this)' in game
assert 'LoadExtraListsPipelines()' in renderer


import re
code = re.search(r"versionCode\s+(\d+)", gradle)
version = re.search(r"versionName\s+'([^']+)'", gradle)
assert code and int(code.group(1)) >= 36
assert version and f'json_quote("{version.group(1)}")' in native
assert 'bo2PipelineSeed' in gradle
print("v0.36 pipeline seed fixtures, merging, input validation and Android wiring OK")
