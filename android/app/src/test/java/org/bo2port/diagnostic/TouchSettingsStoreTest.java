// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import static org.junit.Assert.*;
import android.app.Application;
import android.os.Looper;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;


@RunWith(RobolectricTestRunner.class)
@Config(sdk=35)
public class TouchSettingsStoreTest {
    private boolean save(Application app,String layout)throws Exception{
        AtomicReference<Boolean> result=new AtomicReference<>();TouchSettingsStore.save(app,layout,result::set);
        long until=System.nanoTime()+5_000_000_000L;
        while(result.get()==null && System.nanoTime()<until){Shadows.shadowOf(Looper.getMainLooper()).idle();Thread.sleep(5);}
        assertNotNull("The main-process receiver must acknowledge the save",result.get());return result.get();
    }
    @Test public void receiverPersistsProfileAndPreservesOtherPreferences()throws Exception{
        Application app=RuntimeEnvironment.getApplication();
        GameLaunch.prefs(app).edit().putString("unrelated_setting","preserved").commit();
        TouchController model=new TouchController();model.selectProfile(TouchProfiles.CODM);
        assertTrue(save(app,TouchSettings.encodeLayout(model)));
        TouchController restored=new TouchController();assertTrue(TouchSettings.readLayout(restored,GameLaunch.prefs(app).getString(TouchSettings.LAYOUT,null)));
        assertEquals(TouchProfiles.CODM,restored.profile);assertEquals("preserved",GameLaunch.prefs(app).getString("unrelated_setting",null));
    }
    @Test public void invalidProfileIsRejectedWithoutChangingSavedControls()throws Exception{
        Application app=RuntimeEnvironment.getApplication();String before=TouchSettings.encodeLayout(new TouchController());
        GameLaunch.prefs(app).edit().putString(TouchSettings.LAYOUT,before).commit();
        assertFalse(save(app,"invalid JSON"));assertEquals(before,GameLaunch.prefs(app).getString(TouchSettings.LAYOUT,null));
    }
}
