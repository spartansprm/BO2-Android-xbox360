// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import static org.junit.Assert.*;
import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.time.Duration;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

@RunWith(RobolectricTestRunner.class)
@Config(sdk=35,qualifiers="w800dp-h360dp-land")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class LandscapeMenusTest {
    private TextView text(View view,String wanted) {
        if(view instanceof TextView && ((TextView)view).getText().toString().contains(wanted))return (TextView)view;
        if(view instanceof ViewGroup)for(int i=0;i<((ViewGroup)view).getChildCount();i++){
            TextView result=text(((ViewGroup)view).getChildAt(i),wanted);if(result!=null)return result;
        }
        return null;
    }
    private View layout(Activity activity,int width,int height) {
        View root=activity.getWindow().getDecorView();
        root.measure(View.MeasureSpec.makeMeasureSpec(width,View.MeasureSpec.EXACTLY),View.MeasureSpec.makeMeasureSpec(height,View.MeasureSpec.EXACTLY));
        root.layout(0,0,width,height);return root;
    }
    private void export(Activity activity,int width,int height,String name)throws Exception {
        String directory=System.getProperty("bo2.landscape.preview.dir");if(directory==null)return;
        View root=layout(activity,width,height);Bitmap bitmap=Bitmap.createBitmap(width,height,Bitmap.Config.ARGB_8888);
        root.draw(new Canvas(bitmap));File folder=new File(directory);assertTrue(folder.isDirectory()||folder.mkdirs());
        try(FileOutputStream out=new FileOutputStream(new File(folder,name+".png"))){assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG,100,out));}bitmap.recycle();
    }
    private void launcher(int width,int height)throws Exception {
        try(var owner=Robolectric.buildActivity(LauncherActivity.class).setup()) {
            Shadows.shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(2));Activity activity=owner.get();View root=layout(activity,width,height);
            TextView play=text(root,"AGREGAR JUEGO"),settings=text(root,"AJUSTES");assertNotNull(play);assertNotNull(settings);
            TextView title=text(root,"BLACK OPS II");assertNotNull(title);assertFalse(title.isLongClickable());
            assertFalse(title.performLongClick());assertNull(Shadows.shadowOf(activity).getNextStartedActivity());
            Rect a=new Rect(),b=new Rect();assertTrue(play.getGlobalVisibleRect(a));assertTrue(settings.getGlobalVisibleRect(b));
            assertFalse(Rect.intersects(a,b));assertTrue(a.height()>=48);assertTrue(b.bottom<=height);
            export(activity,width,height,"launcher-"+width+"x"+height);
        }
    }
    private void settings(int width,int height)throws Exception {
        try(var owner=Robolectric.buildActivity(SettingsActivity.class).setup()) {
            Activity activity=owner.get();View root=layout(activity,width,height);
            for(String name:new String[]{"Controles","Imagen","Biblioteca"}) {
                TextView tab=text(root,name);assertNotNull(tab);tab.performClick();Shadows.shadowOf(Looper.getMainLooper()).idle();layout(activity,width,height);
                assertTrue(tab.isSelected());Rect visible=new Rect();assertTrue(tab.getGlobalVisibleRect(visible));
                assertTrue(visible.left<width/2);assertTrue(visible.bottom<=height);
                export(activity,width,height,"settings-"+name+"-"+width+"x"+height);
            }
            text(root,"Controles").performClick();layout(activity,width,height);
            TextView edit=text(root,"EDITAR CONTROLES");assertNotNull(edit);
            for(android.view.ViewParent p=edit.getParent();p!=null;p=p.getParent())if(p instanceof ScrollView){((ScrollView)p).fullScroll(View.FOCUS_DOWN);break;}
            Shadows.shadowOf(Looper.getMainLooper()).idle();Rect visible=new Rect();assertTrue(edit.getGlobalVisibleRect(visible));
            assertTrue(visible.bottom<=height);
        }
    }
    @Test public void landscapeLauncherHasDistinctReachableActions()throws Exception{launcher(800,360);}
    @Test public void landscapeSettingsCategoriesAreReachable()throws Exception{settings(800,360);}
    @Test @Config(qualifiers="w640dp-h320dp-land") public void compactLauncherStillFits()throws Exception{launcher(640,320);}
    @Test @Config(qualifiers="w640dp-h320dp-land") public void compactSettingsStillScrollsToEveryAction()throws Exception{settings(640,320);}
    @Test public void editorRemainsAvailableInLandscape()throws Exception {
        try(var editor=Robolectric.buildActivity(ControlLayoutActivity.class).setup()) {
            layout(editor.get(),800,360);assertNotNull(text(editor.get().getWindow().getDecorView(),"Guardar"));export(editor.get(),800,360,"controls-editor");
        }
    }
    @Test public void modeHandoverDrawsTheSameControlsWithoutAcceptingGameplayInput()throws Exception {
        var app=org.robolectric.RuntimeEnvironment.getApplication();
        var manager=app.getSystemService(android.app.ActivityManager.class);
        var process=new android.app.ActivityManager.RunningAppProcessInfo();process.processName=app.getPackageName()+":game";
        Shadows.shadowOf(manager).setProcesses(java.util.List.of(process));
        android.content.Intent intent=new android.content.Intent(app,RelaunchActivity.class).putExtra("game_tree","content://test/game")
            .putExtra(RelaunchActivity.EXTRA_MODE,"zm").putExtra(TouchSettings.LAYOUT,TouchSettings.encodeLayout(new TouchController()));
        try(var transition=Robolectric.buildActivity(RelaunchActivity.class,intent).setup()){export(transition.get(),800,360,"mode-handover");}
    }
}
