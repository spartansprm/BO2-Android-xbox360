// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.os.Bundle;
import org.libsdl.app.SDLActivity;


public abstract class LandscapeSDLActivity extends SDLActivity {
    @Override protected boolean sendCommand(int command,Object data) {
        if(command==COMMAND_CHANGE_WINDOW_STYLE) {

            if(android.os.Looper.myLooper()==android.os.Looper.getMainLooper()) {
                LandscapeWindow.hideBars(this);mFullscreenModeActive=true;return true;
            }
            data=Integer.valueOf(1);
        }
        return super.sendCommand(command,data);
    }
    @Override public void setOrientationBis(int width,int height,boolean resizable,String hint) {
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }
    @Override protected void onCreate(Bundle state){LandscapeWindow.prepare(this);super.onCreate(state);LandscapeWindow.hideBars(this);}
    @Override protected void onResume(){super.onResume();LandscapeWindow.hideBars(this);}
    @Override public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);if(focused)LandscapeWindow.hideBars(this);}
}
