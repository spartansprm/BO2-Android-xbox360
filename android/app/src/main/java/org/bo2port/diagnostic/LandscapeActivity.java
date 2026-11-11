// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.app.Activity;
import android.os.Bundle;


public abstract class LandscapeActivity extends Activity {
    @Override protected void onCreate(Bundle state){LandscapeWindow.prepare(this);super.onCreate(state);}
    @Override protected void onResume(){super.onResume();LandscapeWindow.hideBars(this);}
    @Override public void onWindowFocusChanged(boolean focused){super.onWindowFocusChanged(focused);if(focused)LandscapeWindow.hideBars(this);}
}
