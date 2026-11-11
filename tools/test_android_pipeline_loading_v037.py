#!/usr/bin/env python3

from pathlib import Path
import ctypes
import ctypes.util
import struct

ROOT = Path(__file__).resolve().parents[1]
draws = (ROOT / "app/src/native/render/bo2zm_native_draws.cpp").read_text(encoding="utf-8")
precompile = (ROOT / "app/src/native/render/bo2zm_native_precompile.cpp").read_text(encoding="utf-8")
native = (ROOT / "android/game/entry.cpp").read_text(encoding="utf-8")


assert "const bool returned_to_lobby = pipeline_was_in_match_ && !in_match;" in draws
assert "pipeline_was_in_match_ = in_match;" in draws
assert "returned_to_lobby && (pipelines_no_save_ || pending_list)" in draws
assert "SaveCachePipelines(false);" in draws
assert "void StartWriterCache()" in draws
assert "writer_warning_.notify_one();" in draws
assert "bool pipeline_was_in_match_ = false;" in draws


assert "constexpr uint32_t kStampVersion = 2;" in precompile
assert "XXH3_64bits_reset(&hash)" in precompile
assert "XXH3_64bits_update(&hash, chunk.data(), size_t(got))" in precompile
assert "XXH3_64bits_digest(&hash)" in precompile
assert "const bool lists = !ExtraLists(&extra_lists_readable).empty();" in precompile
assert "extra_lists_readable;" in precompile
assert "REXCVAR_DEFINE_BOOL(bo2zm_native_precompile, true" in precompile
assert 'SetFlagByName("bo2zm_native_precompile","true")' in native
assert 'flag("pipeline_precompile_before_game",true);' in native
assert 'SetFlagByName("bo2zm_native_async_pipelines"' in native


library = ctypes.util.find_library("xxhash")
assert library, "host libxxhash is required by the shader-index tests too"
xxh = ctypes.CDLL(library)
xxh.XXH3_64bits.argtypes = (ctypes.c_void_p, ctypes.c_size_t)
xxh.XXH3_64bits.restype = ctypes.c_uint64
def digest(data):
    return xxh.XXH3_64bits(ctypes.c_char_p(data), len(data))

def list_bytes(vs, ps):
    record = bytearray(424)
    struct.pack_into("<II", record, 0, vs, ps)
    return struct.pack("<IIII", 0x4C505A42, 1, 424, 1) + record

first, second = list_bytes(3, 9), list_bytes(9, 3)
assert len(first) == len(second)
assert digest(first) != digest(second)
assert digest(first) == digest(first)

print("v0.37 pipeline return-to-lobby save and content-hashed precompile readiness OK")
