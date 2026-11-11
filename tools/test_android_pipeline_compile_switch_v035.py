#!/usr/bin/env python3

from pathlib import Path
p=lambda x:Path(x).read_text(encoding="utf-8")
native=p("android/game/entry.cpp")
java=p("android/app/src/main/java/org/bo2port/diagnostic/GameActivity.java")
renderer=p("app/src/native/render/bo2zm_native_draws.cpp")
assert 'REXCVAR_DEFINE_BOOL(bo2zm_native_async_pipelines, true' in renderer
assert 'if (AsyncCompiles())' in renderer and 'VK_PIPELINE_COMPILE_REQUIRED' in renderer
assert '++draws_waiting_pipeline_' in renderer
assert 'created = CreatePipelineVulkan(key, input, vs, ps, pass_rp_, true, pipeline, n_colors);' in renderer
assert 'disable_async_pipelines_requested=false;' in native
assert 'disable_async_pipelines_requested=disable_async_pipelines==JNI_TRUE;' in native
assert 'SetFlagByName("bo2zm_native_async_pipelines"' in native
assert 'GetFlagByName("bo2zm_native_async_pipelines")' in native
assert 'pipeline_compile_strategy' in native and 'pipeline_async_enabled' in native
assert 'pipeline_sync_trial_requested' in native
assert 'jint scene_pass_mode,\n                                                         jboolean disable_async_pipelines)' in native
assert 'int scenePassMode,boolean disableAsyncPipelines);' in java
assert 'getIntent().getBooleanExtra("disable_async_pipelines",false)))' in java
assert 'state["app_version"]=json_quote(' in native

import runpy
runpy.run_path("tools/test_android_pipeline_library_v036.py", run_name="__main__")
runpy.run_path("tools/test_extract_bo2_pipeline_candidates.py", run_name="__main__")
runpy.run_path("tools/test_android_pipeline_loading_v037.py", run_name="__main__")
runpy.run_path("tools/test_android_pipeline_map_stage_v038.py", run_name="__main__")
print("v0.35/v0.36: pipeline compilation policy native/Java/intent wiring OK")
