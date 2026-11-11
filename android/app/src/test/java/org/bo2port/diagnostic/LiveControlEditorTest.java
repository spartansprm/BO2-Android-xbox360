// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import static org.junit.Assert.*;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.os.Bundle;
import android.os.Looper;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.FrameLayout;
import android.widget.Spinner;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.shadows.ShadowAlertDialog;


@RunWith(RobolectricTestRunner.class)
@Config(sdk=35,qualifiers="w800dp-h360dp-land")
public class LiveControlEditorTest {
    public static class RecordingActivity extends Activity {
        int pauses, stops;
        @Override protected void onPause(){pauses++;super.onPause();}
        @Override protected void onStop(){stops++;super.onStop();}
    }
    private ActivityController<RecordingActivity> controller;
    private RecordingActivity activity;
    private FrameLayout root;
    private SurfaceView surface;
    private SurfaceHolder holder;
    private TouchController play;
    private TouchControlsView controls;
    private GameMenuLayout menu;
    private ControlLayoutEditor editor;
    private String saved;
    private int closes, filePickers, destroyed;
    private boolean acceptSave=true;
    private boolean delaySave;
    private ControlLayoutEditor.Completion pendingSave;

    @Before public void setUp(){
        controller=Robolectric.buildActivity(RecordingActivity.class).create();activity=controller.get();
        activity.setTheme(android.R.style.Theme_Material_NoActionBar);
        root=new FrameLayout(activity);surface=new SurfaceView(activity);holder=surface.getHolder();
        holder.addCallback(new SurfaceHolder.Callback(){
            public void surfaceCreated(SurfaceHolder h){}
            public void surfaceChanged(SurfaceHolder h,int f,int w,int hgt){}
            public void surfaceDestroyed(SurfaceHolder h){destroyed++;}
        });
        root.addView(surface,new FrameLayout.LayoutParams(-1,-1));
        play=new TouchController();play.selectProfile(TouchProfiles.XBOX);controls=new TouchControlsView(activity,play,null);
        menu=new GameMenuLayout(activity,controls,() -> {},() -> fail("Must not leave the game"));
        root.addView(menu,new FrameLayout.LayoutParams(-1,-1));activity.setContentView(root);
        controller.start().resume().visible();layout();
    }
    @After public void tearDown(){controller.pause().stop().destroy();}
    private void layout(){
        root.measure(View.MeasureSpec.makeMeasureSpec(1920,View.MeasureSpec.EXACTLY),View.MeasureSpec.makeMeasureSpec(1080,View.MeasureSpec.EXACTLY));
        root.layout(0,0,1920,1080);Shadows.shadowOf(Looper.getMainLooper()).idle();
    }
    private void open(){
        editor=new ControlLayoutEditor(activity,TouchSettings.encodeLayout(play),false,null,new ControlLayoutEditor.Host(){
            public void close(){menu.hideEditor();closes++;}
            public void save(String value,ControlLayoutEditor.Completion done){
                if(delaySave){pendingSave=done;return;}
                if(acceptSave){saved=value;controls.applySavedLayout(value);}done.finished(acceptSave);
            }
            public void chooseFile(Intent intent,int request){filePickers++;}
            public boolean allowFilePicker(){return false;}
        });
        menu.showEditor(editor.view);layout();
    }
    private static <T extends View> T find(View view,Class<T> type,String text){
        if(type.isInstance(view) && (text==null || view instanceof android.widget.TextView && text.contentEquals(((android.widget.TextView)view).getText())))return type.cast(view);
        if(view instanceof ViewGroup){ViewGroup group=(ViewGroup)view;for(int i=0;i<group.getChildCount();i++){T found=find(group.getChildAt(i),type,text);if(found!=null)return found;}}
        return null;
    }
    private void click(String text){Button button=find(editor.view,Button.class,text);assertNotNull(text,button);button.performClick();Shadows.shadowOf(Looper.getMainLooper()).idle();}
    private void intact(){
        assertSame(root,surface.getParent());assertSame(holder,surface.getHolder());assertEquals(View.VISIBLE,surface.getVisibility());
        assertEquals(0,destroyed);assertEquals(0,activity.pauses);assertEquals(0,activity.stops);
        assertNull(Shadows.shadowOf(activity).getNextStartedActivity());
    }
    @Test public void saveCancelAndBackKeepTheSameGameSurface(){
        for(int i=0;i<3;i++){
            open();assertTrue(menu.isEditing());assertEquals(View.INVISIBLE,controls.getVisibility());intact();
            if(i==0)click("Guardar");else if(i==1)click("Cancelar");else editor.cancel();
            assertFalse(menu.isEditing());assertEquals(View.VISIBLE,controls.getVisibility());intact();
        }
        assertNotNull(saved);assertEquals(3,closes);
    }
    @Test public void savingAChangedProfileAppliesItWithoutReplacingTheSurface(){
        open();Spinner profile=find(editor.view,Spinner.class,null);assertNotNull(profile);
        profile.setSelection(1);Shadows.shadowOf(Looper.getMainLooper()).idle();click("Guardar");
        assertEquals(TouchProfiles.CODM,play.profile);intact();
    }
    @Test public void canceledChangesLeaveThePlayingLayoutUntouched(){
        String before=TouchSettings.encodeLayout(play);open();
        find(editor.view,Spinner.class,null).setSelection(1);Shadows.shadowOf(Looper.getMainLooper()).idle();
        click("Cancelar");assertNull(saved);assertEquals(before,TouchSettings.encodeLayout(play));intact();
    }
    @Test public void failedSaveLeavesEditorOpenAndAllowsRetry(){
        open();acceptSave=false;click("Guardar");assertTrue(menu.isEditing());assertEquals(0,closes);assertNull(saved);
        assertTrue(find(editor.view,Button.class,"Guardar").isEnabled());intact();
        acceptSave=true;click("Guardar");assertFalse(menu.isEditing());intact();
    }
    @Test public void responseDialogDoesNotStartAnotherActivity(){
        open();click("Respuesta");AlertDialog dialog=ShadowAlertDialog.getLatestAlertDialog();assertTrue(dialog.isShowing());
        CheckBox option=find(dialog.getWindow().getDecorView(),CheckBox.class,"Arrastrar cámara desde Apuntar");assertNotNull(option);option.performClick();
        dialog.getButton(DialogInterface.BUTTON_POSITIVE).performClick();click("Guardar");assertFalse(play.aimDrag);intact();
    }
    @Test public void liveFileImportStaysInTheGameWindow(){
        open();click("Opciones");AlertDialog dialog=ShadowAlertDialog.getLatestAlertDialog();
        Shadows.shadowOf(dialog).clickOnItem(2);Shadows.shadowOf(Looper.getMainLooper()).idle();
        assertEquals(0,filePickers);assertTrue(menu.isEditing());intact();
    }
    @Test public void pendingSaveWaitsForAcknowledgementBeforeClosing(){
        open();delaySave=true;click("Guardar");assertNotNull(pendingSave);
        editor.cancel();assertTrue(menu.isEditing());assertEquals(0,closes);
        assertFalse(find(editor.view,Button.class,"Cancelar").isEnabled());intact();
        pendingSave.finished(true);Shadows.shadowOf(Looper.getMainLooper()).idle();assertFalse(menu.isEditing());intact();
    }
    @Test public void editorBackgroundConsumesTouchesAboveTheGameSurface(){
        int[] touches={0};surface.setOnTouchListener((v,event) -> {touches[0]++;return true;});open();
        for(int action:new int[]{MotionEvent.ACTION_DOWN,MotionEvent.ACTION_UP}){
            MotionEvent event=MotionEvent.obtain(1,2,action,1,root.getHeight()-2,0);assertTrue(root.dispatchTouchEvent(event));event.recycle();
        }
        assertEquals(0,touches[0]);intact();
    }
}
