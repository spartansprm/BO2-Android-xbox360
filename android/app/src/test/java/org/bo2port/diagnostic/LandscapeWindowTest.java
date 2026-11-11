// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import static org.junit.Assert.*;
import android.app.Activity;
import android.content.ComponentName;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.view.ContextThemeWrapper;
import android.view.View;
import android.view.WindowManager;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.shadow.api.Shadow;
import org.robolectric.util.ReflectionHelpers.ClassParameter;

@RunWith(RobolectricTestRunner.class)
@Config(sdk=35,qualifiers="w800dp-h360dp-land")
public class LandscapeWindowTest {

    public static class SdlWindow extends LandscapeSDLActivity {
        @Override protected void onCreate(Bundle state){Shadow.directlyOn(this,Activity.class,"onCreate",ClassParameter.from(Bundle.class,state));LandscapeWindow.prepare(this);}
        @Override protected void onDestroy(){Shadow.directlyOn(this,Activity.class,"onDestroy");}
        void requestWindowed(){sendCommand(COMMAND_CHANGE_WINDOW_STYLE,0);}
    }
    private void immersive(Activity activity) {
        int flags=activity.getWindow().getAttributes().flags;
        assertTrue((flags&WindowManager.LayoutParams.FLAG_FULLSCREEN)!=0);
        assertEquals(0,flags&WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN);
        int ui=activity.getWindow().getDecorView().getSystemUiVisibility();
        assertTrue((ui&View.SYSTEM_UI_FLAG_FULLSCREEN)!=0);assertTrue((ui&View.SYSTEM_UI_FLAG_HIDE_NAVIGATION)!=0);
        assertTrue((ui&View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY)!=0);
    }
    @Test public void everyAppActivityIsLandscapeOnly()throws Exception {
        var app=RuntimeEnvironment.getApplication();
        for(Class<?> type:new Class<?>[]{LauncherActivity.class,SettingsActivity.class,GameActivity.class,RelaunchActivity.class,
                ControlLayoutActivity.class,InputExtractorActivity.class}) {
            ActivityInfo info=app.getPackageManager().getActivityInfo(new ComponentName(app,type),0);
            assertEquals(type.getName(),ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE,info.screenOrientation);
        }
    }
    @Test public void sdlInitialWindowedRequestNeverExposesSystemBars() {
        try(var owner=Robolectric.buildActivity(SdlWindow.class).create()) {
            SdlWindow activity=owner.get();activity.requestWindowed();immersive(activity);
            activity.setOrientationBis(360,800,true,"Portrait PortraitUpsideDown");
            assertEquals(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE,activity.getRequestedOrientation());
        }
    }
    @Test public void menuRestoresImmersiveFlagsAfterAnExternalWindow() {
        try(var owner=Robolectric.buildActivity(SettingsActivity.class).setup()) {
            SettingsActivity activity=owner.get();immersive(activity);
            activity.getWindow().addFlags(WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN);
            activity.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
            activity.getWindow().getDecorView().setSystemUiVisibility(0);
            activity.onWindowFocusChanged(true);immersive(activity);
        }
    }
    @Test public void gameStartingWindowCannotReplaceTheHandoverHudWithABlankPreview() {
        var theme=new ContextThemeWrapper(RuntimeEnvironment.getApplication(),R.style.Theme_BO2_Black);
        var values=theme.obtainStyledAttributes(new int[]{android.R.attr.windowFullscreen,android.R.attr.windowDisablePreview});
        assertTrue(values.getBoolean(0,false));assertTrue(values.getBoolean(1,false));values.recycle();
    }
}
