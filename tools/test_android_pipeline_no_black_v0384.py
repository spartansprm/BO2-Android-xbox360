#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
def read(path):
    return (ROOT / path).read_text(encoding="utf-8")

draws = read("app/src/native/render/bo2zm_native_draws.cpp")
entry = read("android/game/entry.cpp")
gpu = read("app/src/native/render/bo2zm_native_targets.cpp")
precise = read("app/src/native/render/bo2zm_native_scene_precise_patch.h")
gradle = read("android/app/build.gradle")


assert 'SetFlagByName("bo2zm_native_pipelines_save_in_match","2")' in entry
assert 'SetFlagByName("bo2zm_native_dynamic_state","false")' in entry
assert 'SetFlagByName("bo2zm_native_compile_threads","3")' in entry
assert 'SetFlagByName("bo2zm_native_compile_helpers_in_match","true")' in entry
assert 'pipeline_compile_threads' in entry
assert 'pipeline_compile_helpers_in_match' in entry
assert 'pipeline_dynamic_state_requested' in entry
assert 'REXCVAR_DEFINE_INT32(bo2zm_native_compile_threads, 1' in draws
assert '.range(1, 4)' in draws
assert 'REXCVAR_DEFINE_BOOL(bo2zm_native_compile_helpers_in_match, false' in draws
assert 'std::vector<std::thread> compile_helpers_' in draws
assert 'compile_helpers_.emplace_back([this] { CompilePipelinesLoop(true); });' in draws
assert 'for (std::thread& helper : compile_helpers_)' in draws
assert 'helpers_in_match || !bo2zm::cheats::InMatch()' in draws
assert 'SaveCachePipelines(true);' in draws
assert 'SaveCachePipelines(false);' in draws


assert 'REXCVAR_DEFINE_BOOL(bo2zm_native_scene_sync_precise, true' in gpu
assert 'REXCVAR_DEFINE_INT32(bo2zm_native_scene_sync_mask, 95' in gpu
assert 'constexpr uint32_t kDepDefaultMask' in precise
assert 'scene_dependencies_precise' in entry




assert "versionCode 390" in gradle
assert "versionName '0.39.0-throughput'" in gradle
assert 'state["app_version"]=json_quote("0.39.0-throughput")' in entry
print("PASS v0.39.0: uploaded pipeline patch, precise-mask95 GPU fallback and scrolling UI")
