// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class InputExtractorActivity extends LandscapeActivity {
    private static final int PICK_GAME=1, SAVE_ZIP=2;
    private final ExecutorService worker=Executors.newSingleThreadExecutor();
    private final ArrayList<Button> buttons=new ArrayList<>();
    private InputPreparer.Program selected=InputPreparer.MULTIPLAYER;
    private InputPreparer.Program preparedProgram=InputPreparer.MULTIPLAYER;
    private File prepared;
    private TextView status;
    private Button save;
    private boolean busy;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout panel=new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        int padding=Math.round(20*getResources().getDisplayMetrics().density);
        panel.setPadding(padding,padding,padding,padding);
        panel.setBackgroundColor(0xFF101820);
        TextView title=new TextView(this);
        title.setText("BO2 · Ejecutables para compilar");title.setTextSize(24);title.setTextColor(Color.WHITE);
        panel.addView(title);
        status=new TextView(this);
        status.setText("Selecciona la carpeta que contiene 00007000 y 000B0000 de tu copia del juego y TU18. Prepara y guarda un ZIP para cada programa.");
        status.setTextColor(0xFFE0E6EC);status.setTextSize(16);panel.addView(status);
        Button mp=button("Preparar Zombies / Multijugador",panel);
        mp.setOnClickListener(view->pick(InputPreparer.MULTIPLAYER));
        Button cp=button("Preparar Campaña",panel);
        cp.setOnClickListener(view->pick(InputPreparer.CAMPAIGN));
        save=button("Guardar ZIP",panel);save.setEnabled(false);
        save.setOnClickListener(view->{
            Intent intent=new Intent(Intent.ACTION_CREATE_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);intent.setType("application/zip");
            intent.putExtra(Intent.EXTRA_TITLE,preparedProgram.campaign()?"bo2-inputs-campaign.zip":"bo2-inputs-mp.zip");
            startActivityForResult(intent,SAVE_ZIP);
        });
        ScrollView scroll=new ScrollView(this);scroll.addView(panel);setContentView(scroll);
    }

    private Button button(String label,LinearLayout parent) {
        Button result=new Button(this);result.setText(label);parent.addView(result);buttons.add(result);return result;
    }

    private void pick(InputPreparer.Program program) {
        if(busy)return;
        selected=program;
        Intent intent=new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(intent,PICK_GAME);
    }

    private void working(boolean active,String message) {
        busy=active;status.setText(message);
        for(Button button:buttons)button.setEnabled(!active);
        save.setEnabled(!active&&prepared!=null&&prepared.isFile());
    }

    @Override protected void onActivityResult(int request,int result,Intent data) {
        super.onActivityResult(request,result,data);
        if(result!=RESULT_OK||data==null||data.getData()==null)return;
        Uri uri=data.getData();
        if(request==PICK_GAME) {
            if(prepared!=null){prepared.delete();prepared=null;}
            working(true,"Preparando ejecutables…");
            InputPreparer.Program program=selected;
            worker.execute(()->{
                File output=null;
                try {
                    output=File.createTempFile("bo2-inputs-",".zip",getCacheDir());
                    InputPreparer.prepare(getContentResolver(),uri,output,program);
                    File ready=output;
                    runOnUiThread(()->{
                        if(isFinishing()||isDestroyed()){ready.delete();return;}
                        prepared=ready;preparedProgram=program;
                        working(false,"Archivos preparados. Pulsa Guardar ZIP y después prepara el otro programa.");
                    });
                } catch(Exception error) {
                    if(output!=null)output.delete();
                    runOnUiThread(()->{if(!isDestroyed())working(false,"No se pudieron preparar los archivos: "+error.getMessage());});
                }
            });
        } else if(request==SAVE_ZIP&&prepared!=null) {
            File source=prepared;working(true,"Guardando ZIP…");
            worker.execute(()->{
                try(FileInputStream input=new FileInputStream(source);OutputStream output=getContentResolver().openOutputStream(uri,"w")) {
                    if(output==null)throw new java.io.IOException("No se pudo abrir el destino");
                    byte[] buffer=new byte[128*1024];
                    for(int count;(count=input.read(buffer))!=-1;)output.write(buffer,0,count);
                    runOnUiThread(()->{if(!isDestroyed())working(false,"ZIP guardado. Transfiérelo al equipo donde vas a compilar.");});
                } catch(Exception error) {
                    runOnUiThread(()->{if(!isDestroyed())working(false,"No se pudo guardar el ZIP: "+error.getMessage());});
                }
            });
        }
    }

    @Override protected void onDestroy() {
        worker.shutdown();
        if(prepared!=null)prepared.delete();
        super.onDestroy();
    }
}
