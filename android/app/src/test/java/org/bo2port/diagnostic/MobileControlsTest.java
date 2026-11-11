// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import static org.junit.Assert.*;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Looper;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Switch;
import java.time.Duration;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk=35)
public class MobileControlsTest {
    private static <T extends View> T find(View v,Class<T> type) {
        if(type.isInstance(v))return type.cast(v);
        if(v instanceof ViewGroup)for(int i=0;i<((ViewGroup)v).getChildCount();i++){
            T child=find(((ViewGroup)v).getChildAt(i),type);if(child!=null)return child;
        }
        return null;
    }
    @Test public void settingsTogglePersistsThroughGameSnapshotAndPreservesCustomControls() {
        SharedPreferences prefs=GameLaunch.prefs(RuntimeEnvironment.getApplication());
        TouchController initial=new TouchController();initial.duplicateProfile("Mis controles");
        initial.layout(1280,720,0,0,0,0);initial.place(initial.find("shoot"),903,412,1.2f);initial.fireMode=2;
        prefs.edit().clear().putString(TouchSettings.LAYOUT,TouchSettings.encodeLayout(initial)).putString("other","kept").commit();
        try(var owner=Robolectric.buildActivity(SettingsActivity.class).setup()) {
            Switch option=find(owner.get().getWindow().getDecorView(),Switch.class);assertNotNull(option);assertFalse(option.isChecked());
            option.performClick();
            TouchController game=TouchSettings.fromIntent(TouchSettings.addTo(new Intent(),prefs));
            assertTrue(game.autoAim);assertEquals(0,game.fireMode);assertEquals(initial.profile,game.profile);
            assertEquals(initial.find("shoot").normalX,game.find("shoot").normalX,0);
            assertEquals(1.2f,game.find("shoot").scale,0);assertEquals("kept",prefs.getString("other",null));
            option.performClick();assertFalse(TouchSettings.fromPreferences(prefs).autoAim);
        }
    }
    @Test public void settingsRefreshesAfterTheSeparateEditorSavesTheSameOption() {
        SharedPreferences prefs=GameLaunch.prefs(RuntimeEnvironment.getApplication());prefs.edit().clear().commit();
        try(var owner=Robolectric.buildActivity(SettingsActivity.class).setup()) {
            Switch option=find(owner.get().getWindow().getDecorView(),Switch.class);assertFalse(option.isChecked());
            owner.pause();TouchController edited=new TouchController();edited.autoAim=true;
            prefs.edit().putString(TouchSettings.LAYOUT,TouchSettings.encodeLayout(edited)).commit();
            owner.resume();assertTrue(option.isChecked());
        }
    }
    private void touch(TouchControlsView view,int action,float x,float y) {
        long now=SystemClock.uptimeMillis();MotionEvent event=MotionEvent.obtain(0,now,action,x,y,0);
        assertTrue(view.onTouchEvent(event));event.recycle();
    }
    @Test public void androidTimerStartsFireWithoutMovingTheFingerAndCancelStopsEverything() {
        TouchController model=new TouchController();model.autoAim=true;
        int[] cameraMoves={0};model.camera=(action,id,x,y,w,h) -> {if(action==1)cameraMoves[0]++;};
        AtomicReference<TouchController.State> last=new AtomicReference<>();
        TouchControlsView view=new TouchControlsView(RuntimeEnvironment.getApplication(),model,last::set);
        view.layout(0,0,1280,720);TouchController.Control fire=model.find("shoot");
        touch(view,MotionEvent.ACTION_DOWN,fire.x,fire.y);assertEquals(255,last.get().lt);assertEquals(0,last.get().rt);
        Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(120));assertEquals(255,last.get().rt);
        Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(5));assertEquals(255,last.get().rt);assertEquals(255,last.get().lt);
        touch(view,MotionEvent.ACTION_MOVE,fire.x+100,fire.y+40);assertTrue(cameraMoves[0]>0);
        assertEquals(255,last.get().rt);assertEquals(255,last.get().lt);
        touch(view,MotionEvent.ACTION_CANCEL,fire.x+100,fire.y+40);assertTrue(last.get().neutral());
        Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(1));assertTrue(last.get().neutral());
    }
    @Test public void normalReleaseEndsContinuousFireAndTheAimTail() {
        TouchController model=new TouchController();model.autoAim=true;
        AtomicReference<TouchController.State> last=new AtomicReference<>();
        TouchControlsView view=new TouchControlsView(RuntimeEnvironment.getApplication(),model,last::set);view.layout(0,0,1280,720);
        TouchController.Control fire=model.find("shoot");touch(view,MotionEvent.ACTION_DOWN,fire.x,fire.y);
        Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(1));assertEquals(255,last.get().rt);
        touch(view,MotionEvent.ACTION_UP,fire.x,fire.y);assertEquals(0,last.get().rt);
        Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(300));assertEquals(0,last.get().lt);
        view.releaseAll();
    }
}
