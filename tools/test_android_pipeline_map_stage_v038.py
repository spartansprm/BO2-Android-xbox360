#!/usr/bin/env python3

from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
cxx = shutil.which("c++") or shutil.which("clang++")
assert cxx, "C++20 compiler required for the map pipeline phase fixture"
with tempfile.TemporaryDirectory(prefix="bo2-map-pipeline-") as tmp:
    output = Path(tmp) / "pipeline-phase-test"
    subprocess.run(
        [cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
         str(ROOT / "tools/test_android_pipeline_map_phase_v038.cpp"),
         "-o", str(output)],
        check=True, cwd=ROOT)
    subprocess.run([str(output)], check=True, cwd=ROOT)

presets = (ROOT / "app/src/bo2zm_preset.cpp").read_text(encoding="utf-8")
preset_header = (ROOT / "app/src/bo2zm_preset.h").read_text(encoding="utf-8")
draws = (ROOT / "app/src/native/render/bo2zm_native_draws.cpp").read_text(encoding="utf-8")
menus = (ROOT / "app/src/ui/menus.lua").read_text(encoding="utf-8")
native = (ROOT / "android/game/entry.cpp").read_text(encoding="utf-8")
pipeline_library = (ROOT / "android/app/src/main/java/org/bo2port/diagnostic/PipelineLibrary.java").read_text(encoding="utf-8")
assert "bo2zm_profile_map " in menus, "The loading-screen hook must identify the chosen map"
assert "MapLoadGeneration()" in preset_header
assert "g_map_load_generation.fetch_add(1, std::memory_order_release)" in presets
assert "uint64_t MapLoadGeneration()" in presets
assert "ReportMapPipelinePhases(now);" in draws
assert "map_pipeline_meter_.Observe(" in draws
assert "map_pipeline_seen_.insert(hash)" in draws
assert "++map_first_pipeline_uncached_" in draws
assert "draws_waiting_pipeline_" in draws
assert "C6 map pipelines v0.38" in draws
assert "bo2zm_map_pipeline_v038.jsonl" in draws
assert "writer_map_events_" in draws
assert "map_events = std::move(writer_map_events_)" in draws
assert "bo2zm_map_pipeline_v038.jsonl" in pipeline_library
assert '"map_pipeline_history"' in pipeline_library

import re
gradle = (ROOT / "android/app/build.gradle").read_text(encoding="utf-8")
version = re.search(r"versionName\s+'([^']+)'", gradle)
assert version and version.group(1).startswith("0.38"), "Not a v0.38-based APK"
assert f'json_quote("{version.group(1)}")' in native, "APK/native version mismatch"

print("v0.38: map-generation hook, phase meter, first-demand/compile/skips instrumentation OK")
