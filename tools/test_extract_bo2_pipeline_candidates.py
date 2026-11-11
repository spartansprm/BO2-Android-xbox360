#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only

import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("bo2_candidates", ROOT / "tools/extract_bo2_pipeline_candidates.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)

hash64 = mod.Hash()

def index_bytes(entries):
    body = bytearray()
    for data, stage in entries:
        row = bytearray(72)
        struct.pack_into("<Q", row, 0, hash64(data))
        struct.pack_into("<I", row, 36, 32)
        row[68] = 1 if stage == "vs" else 0
        body += row
    header = bytearray(80)
    header[:8] = b"BZMSIDX\0"
    struct.pack_into("<II", header, 8, 1, len(entries))
    struct.pack_into("<Q", header, 72, hash64(body))
    return header + body

def fake_zone():
    data = bytearray(2048)
    struct.pack_into(">I", data, 0, len(data) - 40)
    return data

with tempfile.TemporaryDirectory() as td:
    path = Path(td)
    index_file = path / "test.spvidx"
    index_file.write_bytes(index_bytes([(b"vs-a","vs"),(b"ps-a","ps"),(b"ps-b","ps"),(b"vs-b","vs")]))
    indexed = mod.load_index(index_file, hash64)
    assert len(indexed) == 4
    assert indexed[hash64(b"vs-a")] == (1,"vs",32)
    broken = bytearray(index_file.read_bytes())
    broken[-1] ^= 0x7f
    index_file.write_bytes(broken)
    try:
        mod.load_index(index_file, hash64)
        raise AssertionError("corrupted shader index accepted")
    except ValueError as error:
        assert "checksum" in str(error)
    index_file.write_bytes(index_bytes([(b"vs-a","vs"),(b"ps-a","ps"),(b"ps-b","ps"),(b"vs-b","vs")]))

    records = [
        {"record":100, "kind":"vs", "blob":b"vs-a", "ok":True,
         "name":"pimp_shader_shadow_abc12345.hlsl*cached"},
        {"record":200, "kind":"ps", "blob":b"ps-a", "ok":True,
         "name":"pimp_shader_shadow_fedcba12.hlsl"},
        {"record":250, "kind":"ps", "blob":b"ps-b", "ok":True,
         "name":"pimp_shader_ui_deadbeef.hlsl"},
        {"record":300, "kind":"vs", "blob":b"vs-b", "ok":True,
         "name":"pimp_shader_ui_123abcde.hlsl"},
        {"record":500, "kind":"ps", "blob":b"ps-b", "ok":True,
         "name":"pimp_shader_ui_deadbeef.hlsl"},
    ]
    with patch.object(mod,"find_shaders",return_value=records):
        pairs,stats=mod.find_pairs(fake_zone(), indexed, hash64)
        assert {(p["vs"],p["ps"]) for p in pairs} == {(1,2),(4,3)}, pairs
        assert stats["candidate_pairs"] == 2
        assert mod.find_pairs(fake_zone(),indexed,hash64,distance=70)[0] == []

    assert mod.family("pimp_shader_shadow_12345678.hlsl*cached") == "shadow"
    assert mod.family("pimp_shader_shadow_12345678.hlsl") == "shadow"
    assert mod.family("../../evil.hlsl") is None

    known = path / "known.bin"
    record = bytearray(424)
    struct.pack_into("<II",record,0,1,2)
    known.write_bytes(struct.pack("<IIII",0x4C505A42,1,424,1)+record)
    assert mod.known_pairs(known) == {(1,2)}
    catalog = mod.Catalog(index_file,hash64,{(1,2)})
    with patch.object(mod,"find_shaders",return_value=records):
        catalog.add("test-a.zone",fake_zone())
        catalog.add("test-b.zone",fake_zone())
    result=catalog.report()
    assert result["status"] == "candidate_pairs_only"
    assert result["zones"] == 2
    assert result["candidate_pairs"] == 2
    assert result["already_observed_pairs"] == 1
    assert result["unverified_pairs"] == 1
    assert "Vulkan pipelines" in result["safety"]
    out = path / "report.json"
    mod.atomic_json(out,result)
    assert len(json.loads(out.read_text())["pairs"]) == 2
    assert not out.with_suffix(".json.partial").exists()

print("BO2 offline shader pair miner: verified index, bounded records, known cache and safety OK")
