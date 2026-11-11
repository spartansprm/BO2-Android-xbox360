// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.content.Intent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk=35,qualifiers="w800dp-h360dp-land")
public final class InputExtractorActivityTest {
    private Button find(View view,String label) {
        if(view instanceof Button&&label.contentEquals(((Button)view).getText()))return (Button)view;
        if(view instanceof ViewGroup) {
            ViewGroup group=(ViewGroup)view;
            for(int i=0;i<group.getChildCount();i++) {
                Button found=find(group.getChildAt(i),label);
                if(found!=null)return found;
            }
        }
        return null;
    }

    @Test public void opensWithoutNativeLibrariesAndRequiresPreparationBeforeExport() {
        try(ActivityController<InputExtractorActivity> controller=Robolectric.buildActivity(InputExtractorActivity.class).setup()) {
            InputExtractorActivity activity=controller.get();
            View root=activity.getWindow().getDecorView();
            assertFalse(find(root,"Guardar ZIP").isEnabled());
            find(root,"Preparar Campaña").performClick();
            Intent requested=Shadows.shadowOf(activity).getNextStartedActivityForResult().intent;
            assertEquals(Intent.ACTION_OPEN_DOCUMENT_TREE,requested.getAction());
            assertTrue((requested.getFlags()&Intent.FLAG_GRANT_READ_URI_PERMISSION)!=0);
            assertFalse(find(root,"Guardar ZIP").isEnabled());
        }
    }
}
