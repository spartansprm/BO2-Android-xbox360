// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.View;
import android.view.WindowManager;
import android.widget.Toast;
import org.json.JSONObject;
import java.io.File;


public final class RelaunchActivity extends LandscapeActivity {
    static final String EXTRA_MODE = "relaunch_target_mode";
    private static final long WAIT_MS = 15000, POLL_MS = 100;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private long deadline;
    private boolean started;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        android.widget.FrameLayout black = new android.widget.FrameLayout(this){
            @Override public boolean onInterceptTouchEvent(android.view.MotionEvent event){return true;}
            @Override public boolean onTouchEvent(android.view.MotionEvent event){return true;}
        };
        black.setBackgroundColor(Color.BLACK);
        TouchControlsView hud=new TouchControlsView(this,TouchSettings.fromIntent(getIntent()),null);
        hud.setOnTouchListener((view,event)->true);
        hud.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        GameMenuLayout overlay=new GameMenuLayout(this,hud,()->{},()->{});
        black.addView(overlay,new android.widget.FrameLayout.LayoutParams(-1,-1));
        setContentView(black);
        if (state != null && state.getBoolean("started")) { finish(); return; }
        deadline = state==null ? SystemClock.uptimeMillis()+WAIT_MS : state.getLong("deadline",SystemClock.uptimeMillis()+WAIT_MS);
        black.getViewTreeObserver().addOnPreDrawListener(new android.view.ViewTreeObserver.OnPreDrawListener(){
            public boolean onPreDraw(){
                black.getViewTreeObserver().removeOnPreDrawListener(this);
                black.post(()->{acknowledgeReady();startWhenGameEnded();});return true;
            }
        });
    }

    private void acknowledgeReady() {
        android.os.ResultReceiver ready=android.os.Build.VERSION.SDK_INT>=33 ?
            getIntent().getParcelableExtra(ModeTransition.READY,android.os.ResultReceiver.class) : getIntent().getParcelableExtra(ModeTransition.READY);
        if(ready!=null)ready.send(RESULT_OK,null);
    }

    private boolean gameProcessRunning() {
        android.app.ActivityManager manager = getSystemService(android.app.ActivityManager.class);
        java.util.List<android.app.ActivityManager.RunningAppProcessInfo> processes =
            manager == null ? null : manager.getRunningAppProcesses();
        if (processes != null) for (android.app.ActivityManager.RunningAppProcessInfo process : processes)
            if ((getPackageName() + ":game").equals(process.processName)) return true;
        return false;
    }

    private void startWhenGameEnded() {
        if (started || isFinishing() || isDestroyed()) return;
        if (gameProcessRunning()) {
            if (SystemClock.uptimeMillis() < deadline) { handler.postDelayed(this::startWhenGameEnded, POLL_MS); return; }
            Toast.makeText(this, "El juego anterior no se cerró. Pulsa Jugar otra vez.", Toast.LENGTH_LONG).show();
            leave(new Intent(this, LauncherActivity.class));
            return;
        }
        Bundle extras = getIntent().getExtras();
        String mode = extras == null ? null : extras.getString(EXTRA_MODE);
        if (extras == null || mode == null || extras.getString("game_tree") == null) {
            leave(new Intent(this, LauncherActivity.class));
            return;
        }

        String run = java.util.UUID.randomUUID().toString();
        try {
            File oldLog = new File(getFilesDir(), "game-sdk.log");
            if (oldLog.exists() && !oldLog.delete()) throw new java.io.IOException("Cannot clear previous game log");
            GameActivity.writeCheckpoint(getFilesDir(), new JSONObject().put("app_version", BuildConfig.VERSION_NAME)
                .put("run_id", run).put("status", "starting").put("phase", "game_process_requested")
                .put("game_entry_executed", false).put("relaunch_mode", mode));
        } catch (Exception error) {
            Toast.makeText(this, "No se pudo guardar el checkpoint de arranque.", Toast.LENGTH_LONG).show();
            leave(new Intent(this, LauncherActivity.class));
            return;
        }
        Intent game = new Intent(this, GameActivity.class).putExtras(extras);
        game.removeExtra(EXTRA_MODE);
        game.removeExtra(ModeTransition.READY);
        game.putExtra("run_id", run).putExtra("game_mode", mode).putExtra("relaunch", true);
        game.putExtra(ModeTransition.CONTENT_READY,new android.os.ResultReceiver(handler){
            @Override protected void onReceiveResult(int result,Bundle data){if(result==RESULT_OK)finishTransition();}
        });
        started=true;
        startActivity(game.addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION));overridePendingTransition(0,0);

        handler.postDelayed(this::finishTransition,WAIT_MS);
    }

    private void finishTransition(){if(!isDestroyed()){finish();overridePendingTransition(0,0);}}

    private void leave(Intent next) {
        started=true;
        startActivity(next.addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION));
        overridePendingTransition(0, 0);
        finish();
        overridePendingTransition(0, 0);
    }

    @Override public void onBackPressed() {}
    @Override protected void onSaveInstanceState(Bundle state){super.onSaveInstanceState(state);state.putBoolean("started",started);state.putLong("deadline",deadline);}
    @Override protected void onDestroy() { handler.removeCallbacksAndMessages(null); super.onDestroy(); }
}
