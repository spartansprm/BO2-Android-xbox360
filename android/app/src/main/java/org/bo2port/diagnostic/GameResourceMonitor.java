// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.app.ActivityManager;
import android.content.*;
import android.os.*;
import android.system.Os;
import android.system.OsConstants;
import org.json.JSONObject;
import java.io.File;
import java.util.concurrent.*;


final class GameResourceMonitor implements AutoCloseable {
    interface NativeSample {String get();}
    private final Context context;
    private final String run;
    private final NativeSample nativeSample;
    private final ProcessResourceSampler sampler;
    private final ScheduledExecutorService worker=Executors.newSingleThreadScheduledExecutor(r->{Thread t=new Thread(r,"bo2-resource-monitor");t.setDaemon(true);return t;});
    private JSONObject memory= new JSONObject();
    private long memoryAt=-1,lastCaptureMs=0,swapAt=-1,swaps=-1;
    private File gpuNode;
    GameResourceMonitor(Context context,String run,NativeSample nativeSample) {
        this.context=context.getApplicationContext();this.run=run;this.nativeSample=nativeSample;
        long ticks=-1;try{ticks=Os.sysconf(OsConstants._SC_CLK_TCK);}catch(Exception ignored){}
        sampler=new ProcessResourceSampler(new File("/proc"),new File("/sys/devices/system/cpu"),ticks,Runtime.getRuntime().availableProcessors());
        File[] nodes=new File("/sys/class/devfreq").listFiles();
        if(nodes!=null)for(File node:nodes)if(node.getName().toLowerCase(java.util.Locale.ROOT).matches(".*(mali|gpu).*")){gpuNode=node;break;}
    }
    void start(){worker.scheduleWithFixedDelay(this::sample,0,5,TimeUnit.SECONDS);}
    private void sample() {
        long before=SystemClock.elapsedRealtime();
        try {
            JSONObject result=sampler.sample(before).put("run_id",run).put("app_version",BuildConfig.VERSION_NAME)
                .put("foreground",GameActivity.resourceForeground).put("previous_capture_wall_ms",lastCaptureMs);
            if(memoryAt<0||before-memoryAt>=30000) {
                memory=new JSONObject();
                try {
                    Debug.MemoryInfo info=new Debug.MemoryInfo();Debug.getMemoryInfo(info);
                    memory.put("pss_kib",info.getTotalPss()).put("private_dirty_kib",info.getTotalPrivateDirty())
                        .put("graphics_summary_kib",ProcessResourceSampler.nullable(info.getMemoryStat("summary.graphics")));
                    ActivityManager manager=context.getSystemService(ActivityManager.class);
                    if(manager!=null){ActivityManager.MemoryInfo system=new ActivityManager.MemoryInfo();manager.getMemoryInfo(system);
                        memory.put("system_available_bytes",system.availMem).put("system_total_bytes",system.totalMem).put("system_low_memory",system.lowMemory);}
                }catch(Exception error){memory.put("unavailable_reason",error.getClass().getSimpleName());}
                memoryAt=before;
            }
            result.put("memory_details",new JSONObject(memory.toString()).put("age_ms",before-memoryAt)
                .put("native_heap_allocated_bytes",Debug.getNativeHeapAllocatedSize())
                .put("java_heap_used_bytes",Runtime.getRuntime().totalMemory()-Runtime.getRuntime().freeMemory()));
            PowerManager power=context.getSystemService(PowerManager.class);
            result.put("thermal_status",Build.VERSION.SDK_INT>=29&&power!=null?power.getCurrentThermalStatus():JSONObject.NULL);
            Intent battery=context.registerReceiver(null,new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            result.put("battery_temperature_c",battery!=null&&battery.hasExtra(BatteryManager.EXTRA_TEMPERATURE)?
                battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE,0)/10.0:JSONObject.NULL);
            result.put("temperature_scope","Battery temperature; thermal_status is Android's severity, not CPU temperature");
            result.put("gpu_frequency_hz",gpuNode==null?JSONObject.NULL:ProcessResourceSampler.number(new File(gpuNode,"cur_freq")));
            result.put("gpu_load_raw",gpuNode==null?JSONObject.NULL:ProcessResourceSampler.nullable(ProcessResourceSampler.read(new File(gpuNode,"load"),128)));
            result.put("gpu_sysfs_node",gpuNode==null?JSONObject.NULL:gpuNode.getAbsolutePath());
            result.put("gpu_utilization_percent",JSONObject.NULL);
            try {
                JSONObject renderer=new JSONObject(nativeSample.get());result.put("renderer",renderer);
                long count=renderer.optLong("guest_swaps_total",-1);
                if(count>=0&&swaps>=0&&count>=swaps&&before>swapAt)
                    result.put("guest_swap_rate_not_display_fps",(count-swaps)*1000.0/(before-swapAt));
                swaps=count;swapAt=before;
            }catch(Exception|LinkageError error){result.put("renderer_unavailable_reason",error.getClass().getSimpleName());}
            String checkpoint=ProcessResourceSampler.read(new File(context.getFilesDir(),"game-start-checkpoint.json"),131072);
            if(checkpoint!=null)try {JSONObject state=new JSONObject(checkpoint);
                if(run.equals(state.optString("run_id")))result.put("game_phase",state.optString("phase"))
                    .put("map",state.optString("loaded_map")).put("graphics_options",state.optString("graphics_options"));
            }catch(Exception ignored){}
            result.put("sample_collection_wall_ms",SystemClock.elapsedRealtime()-before);
            ProcessResourceSampler.append(context.getFilesDir(),result);
        }catch(Exception error){
            try{ProcessResourceSampler.append(context.getFilesDir(),new JSONObject().put("run_id",run)
                .put("app_version",BuildConfig.VERSION_NAME).put("elapsed_ms",before)
                .put("sample_error_class",error.getClass().getSimpleName()));}catch(Exception ignored){}
        }
        lastCaptureMs=SystemClock.elapsedRealtime()-before;
    }
    @Override public void close(){worker.shutdownNow();}
}
