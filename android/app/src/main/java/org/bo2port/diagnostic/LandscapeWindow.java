// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.os.Build;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;


final class LandscapeWindow {
    private LandscapeWindow() {}
    static void prepare(Activity activity) {
        activity.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        Window window=activity.getWindow();
        WindowManager.LayoutParams attrs=window.getAttributes();
        attrs.layoutInDisplayCutoutMode=WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        window.setAttributes(attrs);
        hideBars(activity);
    }
    static void hideBars(Activity activity) {
        Window window=activity.getWindow();
        window.clearFlags(WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN);
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        if(Build.VERSION.SDK_INT>=30) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller=window.getInsetsController();
            if(controller!=null) {
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                controller.hide(WindowInsets.Type.systemBars());
            }
        }
    }
    static void inset(View view,int horizontal,int vertical) {
        view.setPadding(horizontal,vertical,horizontal,vertical);
        view.setOnApplyWindowInsetsListener((v,i) -> {
            int l=i.getSystemWindowInsetLeft(),t=i.getSystemWindowInsetTop(),r=i.getSystemWindowInsetRight(),b=i.getSystemWindowInsetBottom();
            if(i.getDisplayCutout()!=null) {
                l=Math.max(l,i.getDisplayCutout().getSafeInsetLeft());t=Math.max(t,i.getDisplayCutout().getSafeInsetTop());
                r=Math.max(r,i.getDisplayCutout().getSafeInsetRight());b=Math.max(b,i.getDisplayCutout().getSafeInsetBottom());
            }
            v.setPadding(horizontal+l,vertical+t,horizontal+r,vertical+b);return i;
        });
        view.requestApplyInsets();
    }
}
