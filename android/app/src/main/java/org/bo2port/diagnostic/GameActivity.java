// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.WindowManager;
import android.widget.RelativeLayout;
import org.json.JSONObject;
import org.libsdl.app.SDLActivity;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;


public final class GameActivity extends LandscapeSDLActivity {
    private String runId;
    private TouchControlsView controls;
    private GameMenuLayout gameMenu;
    private ControlLayoutEditor controlEditor;
    static volatile boolean resourceForeground;
    private GameResourceMonitor resourceMonitor;
    private native String nativePerformanceSnapshot();
    private native void nativeGpuProfileForeground(boolean foreground);
    private void profileForeground(boolean foreground){try{nativeGpuProfileForeground(foreground);}catch(LinkageError ignored){}}

    static final String CAMPAIGN_LIBRARY="bo2cp";
    private boolean campaign(){return "cp".equals(getIntent().getStringExtra("game_mode"));}
    @Override protected String[] getLibraries(){return new String[]{"c++_shared","SDL3",campaign()?CAMPAIGN_LIBRARY:"bo2game"};}

    private static volatile java.lang.ref.WeakReference<GameActivity> current=new java.lang.ref.WeakReference<>(null);


    static boolean requestRelaunch(String program){
        GameActivity activity=current.get();
        String mode="bo2cp".equals(program)?"cp":"bo2zm".equals(program)?"zm":null;
        if(activity==null||mode==null)return false;
        return ModeTransition.request(activity,mode);
    }
    private native boolean nativeConfigureGame(int header,int[] parts,int update,int[] dlc,String[] dlcNames,String game,String files,String libraries,String run,boolean detailedGpuMarks,boolean batchDraws,boolean gpuPassProfile,boolean transferDstTrial,boolean disableScenePass,boolean disableSceneTransfers,boolean disableOutputRead,int scenePassMode,boolean disableAsyncPipelines);
    private native void nativeTouchState(int buttons,int lt,int rt,int lx,int ly,int rx,int ry);
    private native boolean nativeTouchRelativeAvailable();
    private native void nativeTouchLook(int action,int pointer,float x,float y,float width,float height);
    private void sendTouch(TouchController.State s) {
        try{nativeTouchState(s.buttons,s.lt,s.rt,s.lx,s.ly,s.rx,s.ry);}catch(LinkageError ignored){}
    }
    static void writeCheckpoint(File directory,JSONObject value)throws Exception {
        android.util.AtomicFile file=new android.util.AtomicFile(new File(directory,"game-start-checkpoint.json"));
        FileOutputStream stream=null;
        try {stream=file.startWrite();stream.write(value.toString().getBytes(StandardCharsets.UTF_8));file.finishWrite(stream);}
        catch(Exception e){if(stream!=null)file.failWrite(stream);throw e;}
    }


    private static final java.util.regex.Pattern EXTRA_CVAR=
        java.util.regex.Pattern.compile("^--(bo2zm_|stfs_read_cache_|present_|android_)[A-Za-z0-9_]+=[A-Za-z0-9_.\\-]{1,32}$");

    static String switchName(String token) {
        if(token==null||!token.startsWith("--"))return "";
        int eq=token.indexOf('=');
        return eq>2?token.substring(2,eq):token.substring(2);
    }


    static String[] mergeSwitches(String[] base,java.util.List<String> extras,StringBuilder replaced) {
        java.util.ArrayList<String> all=new java.util.ArrayList<>(java.util.Arrays.asList(base));
        for(String token:extras) {
            String name=switchName(token);
            int at=-1;
            for(int i=0;i<all.size()&&!name.isEmpty();i++) {
                if(switchName(all.get(i)).equals(name)){at=i;break;}
            }
            if(at<0){all.add(token);continue;}
            if(replaced!=null)replaced.append(all.get(at)).append(" -> ").append(token).append(' ');
            all.set(at,token);
        }
        return all.toArray(new String[0]);
    }
    static String[] concat(String[]... parts) {
        java.util.ArrayList<String> all=new java.util.ArrayList<>();
        for(String[] part:parts)all.addAll(java.util.Arrays.asList(part));
        return all.toArray(new String[0]);
    }
    private String[] withExtraCvars(String[] base) {
        java.util.ArrayList<String> requested=new java.util.ArrayList<>();
        try {
            String fromIntent=getIntent().getStringExtra("extra_cvars");
            if(fromIntent!=null)for(String token:fromIntent.trim().split("\\s+"))requested.add(token);
            File external=getExternalFilesDir(null);
            File list=external==null?null:new File(external,"bo2-extra-cvars.txt");
            if(list!=null&&list.isFile()&&list.length()<4096) {
                for(String line:new String(java.nio.file.Files.readAllBytes(list.toPath()),StandardCharsets.UTF_8).split("\\R"))
                    for(String token:line.trim().split("\\s+"))requested.add(token);
            }
        }catch(Exception|LinkageError ignored) {}
        StringBuilder applied=new StringBuilder();
        java.util.ArrayList<String> accepted=new java.util.ArrayList<>();
        for(String token:requested) {
            if(token.isEmpty())continue;
            if(EXTRA_CVAR.matcher(token).matches()){accepted.add(token);applied.append(token).append(' ');}
            else android.util.Log.w("BO2","Ignored extra switch: "+token);
        }
        StringBuilder replaced=new StringBuilder();
        String[] all=mergeSwitches(base,accepted,replaced);
        if(applied.length()>0)android.util.Log.i("BO2","Extra switches applied: "+applied);
        if(replaced.length()>0)android.util.Log.i("BO2","Extra switches replacing launcher ones: "+replaced);
        return all;
    }
    @Override protected String[] getArguments() {
        runId=getIntent().getStringExtra("run_id");File zip=null;
        try {
            JSONObject progress=new JSONObject().put("app_version",BuildConfig.VERSION_NAME).put("run_id",runId)
                .put("phase","game_prepare_private_inputs").put("status","starting").put("game_entry_executed",false);
            writeCheckpoint(getFilesDir(),progress);
            Uri tree=Uri.parse(getIntent().getStringExtra("game_tree"));


            PipelineLibrary.installBundledList(this);


            GameShaderAssets.removeBundledCopy(this);
            JSONObject imported=GameShaderAssets.imported(this);
            final boolean useLibrary=imported!=null&&getIntent().getBooleanExtra("use_imported_library",true);
            JSONObject libraryInfo=imported!=null?imported:new JSONObject().put("external_library",false).put("shaders",0);
            libraryInfo.put("used_by_game",useLibrary).put("runtime_translation",true);
            android.util.Log.i("BO2",useLibrary?"Imported shader library used as an acceleration; missing shaders translated"
                :"No shader library used: every shader is translated on the device");
            android.util.AtomicFile shaderCheckpoint=new android.util.AtomicFile(new File(getFilesDir(),"game-active-shader-library.json"));
            FileOutputStream shaderStream=null;
            try{shaderStream=shaderCheckpoint.startWrite();shaderStream.write(new JSONObject().put("app_version",BuildConfig.VERSION_NAME)
                .put("run_id",runId).put("library",libraryInfo).toString().getBytes(StandardCharsets.UTF_8));shaderCheckpoint.finishWrite(shaderStream);}
            catch(Exception error){if(shaderStream!=null)shaderCheckpoint.failWrite(shaderStream);throw error;}
            File cache=new File(getCacheDir(),"game-inputs");if(!cache.isDirectory()&&!cache.mkdirs())throw new java.io.IOException("Cannot create game input staging");
            File[] old=cache.listFiles();if(old!=null)for(File item:old)RuntimeInputImporter.remove(item);
            zip=File.createTempFile("game-inputs-",".zip",getCacheDir());

            final boolean campaign=campaign();
            final InputPreparer.Program program=campaign?InputPreparer.CAMPAIGN:InputPreparer.MULTIPLAYER;
            InputPreparer.prepare(getContentResolver(),tree,zip,program);
            RuntimeInputImporter.Result inputs=RuntimeInputImporter.read(getContentResolver(),Uri.fromFile(zip),cache,program);
            try(InputPreparer.ResourceSet resources=InputPreparer.openResources(getContentResolver(),tree)) {

                final int[] dlc=campaign?new int[0]:resources.dlcDescriptors;
                final String[] dlcNames=campaign?new String[0]:resources.dlcNames;
                if(!nativeConfigureGame(resources.gameHeaderFd,resources.fragments,resources.updateFd,dlc,dlcNames,
                    inputs.directory.getAbsolutePath(),getFilesDir().getAbsolutePath(),getApplicationInfo().nativeLibraryDir,runId,
                    getIntent().getBooleanExtra("gpu_category_marks",false),getIntent().getBooleanExtra("draw_batch",true),getIntent().getBooleanExtra("gpu_pass_profile",true),getIntent().getBooleanExtra("transfer_dst_trial",false),
                    getIntent().getBooleanExtra("disable_scene_pass_dependencies",false),
                    getIntent().getBooleanExtra("disable_scene_transfer_barriers",false),
                    getIntent().getBooleanExtra("disable_scene_output_read_barrier",false),
                    getIntent().getIntExtra("scene_pass_sync_mode",0),
                    getIntent().getBooleanExtra("disable_async_pipelines",false)))
                    throw new java.io.IOException("Native launch configuration failed; see game checkpoint");
            }
            String mode=campaign?"cp":"mp".equals(getIntent().getStringExtra("game_mode"))?"mp":"zm";

            android.util.DisplayMetrics metrics=new android.util.DisplayMetrics();
            getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
            final int[] display=DisplayOptions.landscape(metrics.widthPixels,metrics.heightPixels);


            final boolean relaunch=getIntent().getBooleanExtra("relaunch",false);
            final String campaignInstalled="--bo2zm_campaign="+(BuildConfig.BO2_CAMPAIGN_AVAILABLE?"true":"false");
            String[] modeArguments=campaign?new String[]{"--bo2zm_test_mode="+mode,"--bo2zm_android_relaunch="+relaunch}
                :relaunch?new String[]{campaignInstalled,"--bo2zm_android_relaunch=true"}
                :new String[]{"--bo2zm_test_mode="+mode,campaignInstalled,"--bo2zm_android_relaunch=false"};
            String[] baseArguments=concat(new String[]{"--bo2zm_renderer=native"},modeArguments,new String[]{
                "--log_level=info","--log_file="+new File(getFilesDir(),"game-sdk.log").getAbsolutePath(),
                "--video_mode_width=1280","--video_mode_height=720",
                "--bo2zm_native_scene_sync_precise="+
                    (getIntent().getBooleanExtra("bo2zm_native_scene_sync_precise",true)?"true":"false"),
                "--bo2zm_native_scene_sync_mask="+
                    Math.max(0,Math.min(255,getIntent().getIntExtra("bo2zm_native_scene_sync_mask",95))),

                "--bo2zm_native_deferred_clears="+
                    (getIntent().getBooleanExtra("bo2zm_native_deferred_clears",true)?"true":"false"),
                "--bo2zm_android_front_copy="+
                    (getIntent().getBooleanExtra("front_no_copy",false)?"false":"true"),
                "--bo2zm_native_scene_scale="+
                    Math.max(50,Math.min(100,getIntent().getIntExtra("bo2zm_native_scene_scale",100))),

                "--bo2zm_native_scene_resolution="+
                    SceneResolution.valid(getIntent().getStringExtra(SceneResolution.PREF)),


                "--bo2zm_native_output_resolution="+
                    DisplayOptions.validOutput(getIntent().getStringExtra(DisplayOptions.OUTPUT_PREF)),
                "--bo2zm_native_screen_mode="+
                    DisplayOptions.validScreen(getIntent().getStringExtra(DisplayOptions.SCREEN_PREF)),
                "--bo2zm_native_display_width="+display[0],
                "--bo2zm_native_display_height="+display[1],


                "--present_effect="+Sharpness.validEffect(getIntent().getStringExtra(Sharpness.EFFECT_PREF)),
                "--present_cas_additional_sharpness="+
                    Sharpness.casSharpness(getIntent().getIntExtra(Sharpness.STRENGTH_PREF,Sharpness.STRENGTH_DEFAULT)),
                "--present_fsr_sharpness_reduction="+
                    Sharpness.fsrSharpnessReduction(getIntent().getIntExtra(Sharpness.STRENGTH_PREF,Sharpness.STRENGTH_DEFAULT)),
                "--bo2zm_texture_mip_floor="+Sharpness.validTexture(getIntent().getStringExtra(Sharpness.TEXTURE_PREF)),
                "--bo2zm_native_anisotropic="+Sharpness.validAniso(getIntent().getStringExtra(Sharpness.ANISO_PREF)),


                "--bo2zm_perf_v046=true",

                "--bo2zm_native_shader_library="+(useLibrary?"true":"false")});

            final String preset=Sharpness.validPreset(getIntent().getStringExtra(Sharpness.PRESET_PREF));
            if(!preset.isEmpty())baseArguments=concat(baseArguments,new String[]{"--bo2zm_preset="+preset});
            return withExtraCvars(baseArguments);
        }catch(Exception|LinkageError error) {
            try {writeCheckpoint(getFilesDir(),new JSONObject().put("app_version",BuildConfig.VERSION_NAME).put("run_id",runId)
                .put("status","error").put("phase","game_java_preparation_error").put("game_entry_executed",false)
                .put("error",error.getClass().getSimpleName()+": "+error.getMessage()));}catch(Exception ignored){}
            return new String[0];
        }finally{if(zip!=null)zip.delete();}
    }
    @Override protected void onCreate(Bundle state) {


        WindowManager.LayoutParams cutout=getWindow().getAttributes();
        cutout.layoutInDisplayCutoutMode=WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        getWindow().setAttributes(cutout);

        getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(android.graphics.Color.BLACK));
        runId=getIntent().getStringExtra("run_id");super.onCreate(state);if(mLayout==null)return;
        mLayout.setBackgroundColor(android.graphics.Color.BLACK);
        current=new java.lang.ref.WeakReference<>(this);
        if(runId!=null){resourceMonitor=new GameResourceMonitor(this,runId,this::nativePerformanceSnapshot);resourceMonitor.start();}
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        TouchController touchModel=TouchSettings.fromIntent(getIntent());
        try {
            if(nativeTouchRelativeAvailable()) touchModel.camera=this::nativeTouchLook;
        } catch(LinkageError error) { android.util.Log.w("BO2","Older native library: relative touch camera unavailable",error); }
        controls=new TouchControlsView(this,touchModel,this::sendTouch);
        gameMenu=new GameMenuLayout(this,controls,this::editControls,this::leaveGame);
        mLayout.addView(gameMenu,new RelativeLayout.LayoutParams(-1,-1));gameMenu.requestApplyInsets();


        ModeTransition.announceContent(getIntent(),mLayout);
    }
    private void editControls() {
        if(gameMenu==null||gameMenu.isEditing())return;
        controls.releaseAll();
        controlEditor=new ControlLayoutEditor(this,getIntent().getStringExtra(TouchSettings.LAYOUT),
            getIntent().getBooleanExtra(TouchSettings.AUTO_AIM,false),null,new ControlLayoutEditor.Host(){
                public void close(){gameMenu.hideEditor();controlEditor=null;}
                public void save(String layout,ControlLayoutEditor.Completion done){
                    TouchSettingsStore.save(GameActivity.this,layout,saved -> {
                        if(saved&&!isDestroyed()){
                            getIntent().putExtra(TouchSettings.LAYOUT,layout);
                            controls.applySavedLayout(layout);
                        }
                        done.finished(saved);
                    });
                }
                public void chooseFile(Intent intent,int request){throw new IllegalStateException("Use the launcher for file selection");}
                public boolean allowFilePicker(){return false;}
            });
        gameMenu.showEditor(controlEditor.view);
    }
    private void leaveGame() {
        profileForeground(false);
        if(controls!=null)controls.releaseAll();
        try {
            android.util.AtomicFile mark=new android.util.AtomicFile(new File(getFilesDir(),"game-user-stop.json"));
            FileOutputStream output=mark.startWrite();output.write(new JSONObject().put("run_id",runId).put("requested",true).toString().getBytes(StandardCharsets.UTF_8));mark.finishWrite(output);
        }catch(Exception ignored){}

        Thread exit=new Thread(()->{try{Thread.sleep(300);}catch(InterruptedException ignored){}android.os.Process.killProcess(android.os.Process.myPid());},"leave-game-process");
        exit.setDaemon(true);exit.start();

        startActivity(new Intent(this,LauncherActivity.class).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
    }
    @Override public void onBackPressed(){
        if(controlEditor!=null){controlEditor.cancel();return;}
        if(gameMenu==null||!gameMenu.closeMenu())leaveGame();
    }
    @Override protected void onResume(){super.onResume();resourceForeground=true;profileForeground(true);}
    @Override protected void onPause(){resourceForeground=false;profileForeground(false);if(gameMenu!=null)gameMenu.closeMenu();if(controls!=null)controls.releaseAll();else sendTouch(new TouchController.State());super.onPause();}
    @Override public void onWindowFocusChanged(boolean focused){profileForeground(focused&&resourceForeground);if(!focused){if(controls!=null)controls.releaseAll();}super.onWindowFocusChanged(focused);}
    @Override protected void onDestroy(){
        profileForeground(false);
        if(resourceMonitor!=null)resourceMonitor.close();


        android.os.Process.killProcess(android.os.Process.myPid());super.onDestroy();
    }
}
