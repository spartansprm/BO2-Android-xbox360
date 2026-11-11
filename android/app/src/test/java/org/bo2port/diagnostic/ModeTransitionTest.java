// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import static org.junit.Assert.*;
import android.app.ActivityManager;
import android.content.Intent;
import android.os.Bundle;
import android.os.Looper;
import android.os.ResultReceiver;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import java.time.Duration;
import java.util.List;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk=35,qualifiers="w800dp-h360dp-land")
public class ModeTransitionTest {
    public static class SourceActivity extends LandscapeActivity {}
    private Intent request() {
        var app=RuntimeEnvironment.getApplication();
        return new Intent(app,RelaunchActivity.class).putExtra("game_tree","content://test/game")
            .putExtra("run_id","previous").putExtra(RelaunchActivity.EXTRA_MODE,"zm")
            .putExtra(TouchSettings.LAYOUT,TouchSettings.encodeLayout(new TouchController()));
    }
    private void processes(boolean running) {
        ActivityManager manager=RuntimeEnvironment.getApplication().getSystemService(ActivityManager.class);
        var process=new ActivityManager.RunningAppProcessInfo();process.processName=RuntimeEnvironment.getApplication().getPackageName()+":game";
        Shadows.shadowOf(manager).setProcesses(running?List.of(process):List.of());
    }
    private TouchControlsView hud(View view) {
        if(view instanceof TouchControlsView)return (TouchControlsView)view;
        if(view instanceof ViewGroup)for(int i=0;i<((ViewGroup)view).getChildCount();i++){var found=hud(((ViewGroup)view).getChildAt(i));if(found!=null)return found;}
        return null;
    }
    private void firstFrame(android.app.Activity activity) {
        View view=activity.findViewById(android.R.id.content);view.measure(View.MeasureSpec.makeMeasureSpec(800,View.MeasureSpec.EXACTLY),View.MeasureSpec.makeMeasureSpec(360,View.MeasureSpec.EXACTLY));view.layout(0,0,800,360);
        view.getViewTreeObserver().dispatchOnPreDraw();Shadows.shadowOf(Looper.getMainLooper()).idle();
    }
    @Test public void hudIsPresentAndConsumesTouchesWhileTheOldProcessEnds() {
        processes(true);boolean[] ready={false};Intent intent=request().putExtra(ModeTransition.READY,new ResultReceiver(null){
            @Override protected void onReceiveResult(int result,Bundle data){ready[0]=true;}
        });
        try(var owner=Robolectric.buildActivity(RelaunchActivity.class,intent).setup()) {
            RelaunchActivity activity=owner.get();TouchControlsView controls=hud(activity.getWindow().getDecorView());assertNotNull(controls);
            firstFrame(activity);assertTrue(ready[0]);assertNull(Shadows.shadowOf(activity).getNextStartedActivity());
            MotionEvent down=MotionEvent.obtain(0,0,MotionEvent.ACTION_DOWN,640,165,0);assertTrue(controls.dispatchTouchEvent(down));down.recycle();
            processes(false);Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(100));
            Intent game=Shadows.shadowOf(activity).getNextStartedActivity();assertNotNull(game);
            assertEquals(GameActivity.class.getName(),game.getComponent().getClassName());assertTrue(game.getBooleanExtra("relaunch",false));
            assertEquals("zm",game.getStringExtra("game_mode"));assertEquals(intent.getStringExtra(TouchSettings.LAYOUT),game.getStringExtra(TouchSettings.LAYOUT));
            assertNotEquals("previous",game.getStringExtra("run_id"));assertFalse(game.hasExtra(ModeTransition.READY));
        }
    }
    @Test public void aRepeatedPollCannotStartTheNewModeTwice() {
        processes(false);
        try(var owner=Robolectric.buildActivity(RelaunchActivity.class,request()).setup()) {
            RelaunchActivity activity=owner.get();firstFrame(activity);assertNotNull(Shadows.shadowOf(activity).getNextStartedActivity());
            firstFrame(activity);Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(1));
            assertNull(Shadows.shadowOf(activity).getNextStartedActivity());
        }
    }
    @Test public void aStuckGameReturnsToTheLandscapeLauncher() {
        processes(true);
        try(var owner=Robolectric.buildActivity(RelaunchActivity.class,request()).setup()) {
            RelaunchActivity activity=owner.get();firstFrame(activity);
            Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(15));
            Intent fallback=Shadows.shadowOf(activity).getNextStartedActivity();assertNotNull(fallback);
            assertEquals(LauncherActivity.class.getName(),fallback.getComponent().getClassName());
        }
    }
    @Test public void guestThreadWaitsForTheHandoverBeforeItCanExit()throws Exception {
        processes(true);
        try(var source=Robolectric.buildActivity(SourceActivity.class,request()).setup()) {
            var executor=java.util.concurrent.Executors.newSingleThreadExecutor();
            try {
                var result=executor.submit(()->ModeTransition.request(source.get(),"zm"));
                Intent next=null;long until=System.nanoTime()+1_000_000_000L;
                while(next==null && System.nanoTime()<until){next=Shadows.shadowOf(source.get()).getNextStartedActivity();if(next==null)Thread.sleep(2);}
                assertNotNull(next);assertFalse(result.isDone());
                try(var transition=Robolectric.buildActivity(RelaunchActivity.class,next).setup()) {
                    firstFrame(transition.get());assertTrue(result.get(1,java.util.concurrent.TimeUnit.SECONDS));assertTrue(source.get().isFinishing());
                }
            }finally{executor.shutdownNow();}
        }
    }
    @Test public void recreationWhileWaitingKeepsTheHudAndStillStartsTheSelectedMode() {
        processes(true);Bundle state=new Bundle();
        var first=Robolectric.buildActivity(RelaunchActivity.class,request()).setup();firstFrame(first.get());
        first.saveInstanceState(state).pause().stop().destroy();
        try(var restored=Robolectric.buildActivity(RelaunchActivity.class,request()).create(state).start().resume().visible()) {
            assertNotNull(hud(restored.get().getWindow().getDecorView()));assertFalse(restored.get().isFinishing());
            firstFrame(restored.get());processes(false);Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(100));
            assertNotNull(Shadows.shadowOf(restored.get()).getNextStartedActivity());
        }
    }
    @Test public void transitionStaysUntilTheDestinationHudHasItsFirstFrame() {
        processes(false);
        try(var transition=Robolectric.buildActivity(RelaunchActivity.class,request()).setup()) {
            firstFrame(transition.get());Intent game=Shadows.shadowOf(transition.get()).getNextStartedActivity();assertNotNull(game);
            assertFalse(transition.get().isFinishing());
            try(var destination=Robolectric.buildActivity(SourceActivity.class,game).setup()) {
                View view=new View(destination.get());destination.get().setContentView(view);
                ModeTransition.announceContent(destination.get().getIntent(),view);
                assertFalse(transition.get().isFinishing());firstFrame(destination.get());
                assertTrue(transition.get().isFinishing());assertFalse(destination.get().getIntent().hasExtra(ModeTransition.CONTENT_READY));
            }
        }
    }
}
