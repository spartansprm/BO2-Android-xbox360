// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import static org.junit.Assert.*;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Shader;
import java.io.File;
import java.io.FileOutputStream;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;


@RunWith(RobolectricTestRunner.class)
@Config(sdk=35)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class TouchHudRenderingTest {
    private Bitmap render(TouchController model,int width,int height,boolean background) {
        TouchControlsView view=new TouchControlsView(RuntimeEnvironment.getApplication(),model,null);view.layout(0,0,width,height);
        return snapshot(view,width,height,background);
    }
    private Bitmap snapshot(TouchControlsView view,int width,int height,boolean background) {
        Bitmap image=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);Canvas canvas=new Canvas(image);
        if(background){
            Paint p=new Paint();p.setShader(new LinearGradient(0,0,width,height,0xFF778681,0xFF394348,Shader.TileMode.CLAMP));canvas.drawRect(0,0,width,height,p);
        }
        view.draw(canvas);return image;
    }
    private void export(Bitmap bitmap,String name)throws Exception {
        String directory=System.getProperty("bo2.preview.dir");if(directory==null)return;
        File folder=new File(directory);assertTrue(folder.isDirectory()||folder.mkdirs());
        try(FileOutputStream out=new FileOutputStream(new File(folder,name))){assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG,100,out));}
    }
    @Test public void opacityAppliesToTheWholeJoystickIncludingItsThumb() {
        TouchController model=new TouchController();for(TouchController.Control c:model.controls)c.opacity=0;
        Bitmap transparent=render(model,1280,720,false);
        int[] pixels=new int[1280*720];transparent.getPixels(pixels,0,1280,0,0,1280,720);
        for(int pixel:pixels)assertEquals(0,Color.alpha(pixel));transparent.recycle();
    }
    @Test public void productionHudRendersEveryProfileAndPressedFeedback()throws Exception {
        for(String profile:new String[]{TouchProfiles.CODM,TouchProfiles.XBOX,TouchProfiles.CUSTOM}) {
            TouchController model=new TouchController();model.selectProfile(profile);model.autoAim=true;
            TouchControlsView view=new TouchControlsView(RuntimeEnvironment.getApplication(),model,null);view.layout(0,0,1280,720);
            Bitmap normal=snapshot(view,1280,720,true);export(normal,"hud-"+profile.replace(':','-')+".png");
            TouchController.Control fire=model.find(profile.equals(TouchProfiles.XBOX)?"rt":"shoot");

            int x=Math.round(fire.x),y=Math.round(fire.y+fire.radius*.8f),before=normal.getPixel(x,y);
            model.time(0);model.down(1,fire.x,fire.y);model.time(1000);
            Bitmap pressed=snapshot(view,1280,720,true);
            assertNotEquals(before,pressed.getPixel(x,y));export(pressed,"hud-"+profile.replace(':','-')+"-pressed.png");
            normal.recycle();pressed.recycle();view.releaseAll();
        }
        TouchController wide=new TouchController();Bitmap image=render(wide,1600,720,true);export(image,"hud-mobile-wide.png");image.recycle();
    }
}
