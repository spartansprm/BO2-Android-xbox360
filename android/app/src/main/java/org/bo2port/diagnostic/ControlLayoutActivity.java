// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;


public final class ControlLayoutActivity extends LandscapeActivity {
    private ControlLayoutEditor editor;
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        android.content.SharedPreferences prefs=GameLaunch.prefs(this);
        editor=new ControlLayoutEditor(this,prefs.getString(TouchSettings.LAYOUT,"{}"),
            prefs.getBoolean(TouchSettings.AUTO_AIM,false),state,new ControlLayoutEditor.Host(){
                public void close(){finish();}
                public void save(String layout,ControlLayoutEditor.Completion done){
                    boolean saved=prefs.edit().putString(TouchSettings.LAYOUT,layout).commit();
                    if(saved)setResult(RESULT_OK,new Intent().putExtra(TouchSettings.LAYOUT,layout));
                    done.finished(saved);
                }
                public void chooseFile(Intent intent,int request){startActivityForResult(intent,request);}
                public boolean allowFilePicker(){return true;}
            });
        setContentView(editor.view);
    }
    @Override protected void onActivityResult(int request,int result,Intent data){
        super.onActivityResult(request,result,data);if(editor!=null)editor.onActivityResult(request,result,data);
    }
    @Override public void onBackPressed(){if(editor!=null)editor.cancel();else super.onBackPressed();}
    @Override protected void onSaveInstanceState(Bundle state){if(editor!=null)editor.saveState(state);super.onSaveInstanceState(state);}
}
