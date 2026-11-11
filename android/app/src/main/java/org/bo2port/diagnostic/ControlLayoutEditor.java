// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.content.res.Resources;
import android.widget.*;
import java.nio.charset.StandardCharsets;


final class ControlLayoutEditor {
    interface Completion { void finished(boolean saved); }
    interface Host {
        void close();
        void save(String layout, Completion completion);
        void chooseFile(Intent intent, int request);
        boolean allowFilePicker();
    }
    private final Activity activity;
    private final Host host;
    final View view;
    private boolean saving;
    private Button saveButton, cancelButton;
    private Resources getResources() { return activity.getResources(); }
    private static final int IMPORT = 5201, EXPORT = 5202;
    private TouchController model;
    private TouchControlsView preview;
    private Spinner profiles, picker;
    private TextView sizeLabel;
    private SeekBar size;
    private boolean selecting, selectingProfile;
    private String pendingExport;
    private int dp(int n) { return Math.round(getResources().getDisplayMetrics().density*n); }
    ControlLayoutEditor(Activity activity, String layout, boolean autoAim, Bundle state, Host host) {
        this.activity = activity; this.host = host;
        model = new TouchController(); model.editing = true; model.autoAim = autoAim;
        TouchSettings.readLayout(model,state == null ? layout : state.getString("draft",layout));
        pendingExport = state == null ? null : state.getString("export"); model.layout(800,360,0,0,0,0);
        LinearLayout root = new LinearLayout(activity); root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(LauncherActivity.BACKGROUND);
        LinearLayout actions = row(); profiles = new Spinner(activity);
        actions.addView(profiles,new LinearLayout.LayoutParams(dp(170),dp(44)));
        button(actions,"Copiar",this::copyProfile);
        button(actions,"Opciones",this::profileOptions);
        cancelButton=button(actions,"Cancelar",this::cancel);
        saveButton=button(actions,"Guardar",this::save);
        root.addView(scroll(actions));
        LinearLayout tools = row(); picker = new Spinner(activity);
        tools.addView(picker,new LinearLayout.LayoutParams(dp(190),dp(42)));
        button(tools,"Propiedades",this::properties);
        button(tools,"Añadir",this::addControl);
        button(tools,"Eliminar",() -> {
            TouchController.Control c = preview.selected(); if (c == null) return;
            model.remove(c); refreshControls(); preview.refreshLayout();
        });
        button(tools,"Respuesta",this::responseOptions);
        root.addView(scroll(tools));
        LinearLayout sizing = row(); sizeLabel = label("Selecciona un control");
        sizing.addView(sizeLabel,new LinearLayout.LayoutParams(dp(185),dp(36)));
        size = new SeekBar(activity); size.setMax(275); size.setContentDescription("Tamaño del control, de 25 a 300 por ciento");
        sizing.addView(size,new LinearLayout.LayoutParams(0,dp(36),1)); root.addView(sizing);
        FrameLayout area = new FrameLayout(activity); root.addView(area,new LinearLayout.LayoutParams(-1,0,1));
        preview = new TouchControlsView(activity,model,null); preview.setBackgroundColor(0xFF192733);
        area.addView(preview,new FrameLayout.LayoutParams(1,1,Gravity.CENTER));
        area.addOnLayoutChangeListener((v,l,t,r,b,ol,ot,or,ob) -> {
            int w = root.getWidth()-root.getPaddingLeft()-root.getPaddingRight();
            int h = root.getHeight()-root.getPaddingTop()-root.getPaddingBottom();
            if (w <= 0 || h <= 0 || area.getWidth() <= 0 || area.getHeight() <= 0) return;
            float scale = Math.min(area.getWidth()/(float)w,area.getHeight()/(float)h);
            int pw = Math.max(1,Math.round(w*scale)), ph = Math.max(1,Math.round(h*scale));
            if (preview.getLayoutParams().width != pw || preview.getLayoutParams().height != ph)
                preview.setLayoutParams(new FrameLayout.LayoutParams(pw,ph,Gravity.CENTER));
        });
        preview.setSelectionListener(c -> {
            selecting = true; picker.setSelection(c == null ? 0 : model.controls.indexOf(c)+1);
            size.setEnabled(c != null); size.setProgress(c == null ? 75 : Math.round(c.scale*100)-25);
            sizeLabel.setText(c == null ? "Selecciona un control" : "Tamaño · "+Math.round(c.scale*100)+" %"); selecting = false;
        });
        picker.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            public void onItemSelected(AdapterView<?> p,View v,int at,long id) {
                if (!selecting && at <= model.controls.size()) {
                    String wanted = at == 0 ? null : model.controls.get(at-1).id;
                    TouchController.Control current = preview.selected();
                    if (!(current == null ? wanted == null : current.id.equals(wanted))) preview.selectControl(wanted);
                }
            }
            public void onNothingSelected(AdapterView<?> p) {}
        });
        profiles.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            public void onItemSelected(AdapterView<?> p,View v,int at,long id) {
                if (selectingProfile || at >= model.profiles.size()) return;
                String next = model.profiles.keySet().toArray(new String[0])[at];
                if (!next.equals(model.profile)) { model.selectProfile(next); refreshControls(); preview.refreshLayout(); }
            }
            public void onNothingSelected(AdapterView<?> p) {}
        });
        size.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar b,int n,boolean user) {
                if (user) { preview.resizeSelected((25+n)/100f); sizeLabel.setText("Tamaño · "+(25+n)+" %"); }
            }
            public void onStartTrackingTouch(SeekBar b) {}
            public void onStopTrackingTouch(SeekBar b) {}
        });
        refreshProfiles(); refreshControls(); preview.selectControl(null);
        root.setClickable(true); root.setFocusableInTouchMode(true); view=root;
        root.setOnApplyWindowInsetsListener((v,i) -> {
            int l=i.getSystemWindowInsetLeft(),t=i.getSystemWindowInsetTop(),r=i.getSystemWindowInsetRight(),b=i.getSystemWindowInsetBottom();
            if (i.getDisplayCutout()!=null) { l=Math.max(l,i.getDisplayCutout().getSafeInsetLeft()); t=Math.max(t,i.getDisplayCutout().getSafeInsetTop());
                r=Math.max(r,i.getDisplayCutout().getSafeInsetRight()); b=Math.max(b,i.getDisplayCutout().getSafeInsetBottom()); }
            root.setPadding(l,t,r,b); return i;
        }); root.requestApplyInsets();
    }
    void cancel() { if (!saving) host.close(); }
    private void save() {
        if (saving) return;
        String layout = TouchSettings.encodeLayout(model);
        saving=true; saveButton.setEnabled(false); cancelButton.setEnabled(false);
        host.save(layout, saved -> activity.runOnUiThread(() -> {
            if (activity.isDestroyed()) return;
            saving=false; saveButton.setEnabled(true); cancelButton.setEnabled(true);
            if (saved) host.close(); else toast("No se pudieron guardar los controles. Inténtalo otra vez.");
        }));
    }
    private LinearLayout row() { LinearLayout r = new LinearLayout(activity); r.setGravity(Gravity.CENTER_VERTICAL); return r; }
    private View scroll(View row) { HorizontalScrollView s = new HorizontalScrollView(activity); s.setFillViewport(true); s.addView(row); return s; }
    private TextView label(String text) { TextView v = new TextView(activity); v.setText(text); v.setTextColor(0xFFFFFFFF); v.setTextSize(13); v.setPadding(dp(10),0,dp(8),0); v.setGravity(Gravity.CENTER_VERTICAL); return v; }
    private Button button(LinearLayout row,String title,Runnable action) { Button b = new Button(activity); b.setText(title); b.setTextSize(12); b.setAllCaps(false); b.setOnClickListener(v -> { if (!saving) action.run(); }); row.addView(b,new LinearLayout.LayoutParams(-2,dp(44))); return b; }
    private void adapter(Spinner spinner,String[] names) { ArrayAdapter<String> a = new ArrayAdapter<>(activity,android.R.layout.simple_spinner_item,names); a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item); spinner.setAdapter(a); }
    private void refreshProfiles() {
        selectingProfile = true; String[] names = new String[model.profiles.size()]; int n=0,selected=0;
        for (TouchProfiles.Profile p : model.profiles.values()) { names[n]=p.name; if (p.id.equals(model.profile)) selected=n; n++; }
        adapter(profiles,names); profiles.setSelection(selected); selectingProfile=false;
    }
    private void refreshControls() {
        selecting=true; String[] names = new String[model.controls.size()+1]; names[0]="Selecciona un control";
        for (int i=0;i<model.controls.size();++i) { TouchController.Control c=model.controls.get(i); names[i+1]=c.label+(c.enabled?"":" (oculto)"); }
        adapter(picker,names); selecting=false;
    }
    private void nameDialog(String title,String initial,java.util.function.Consumer<String> action) {
        EditText name = new EditText(activity); name.setSingleLine(true); name.setText(initial); name.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(48)});
        new AlertDialog.Builder(activity).setTitle(title).setView(name).setNegativeButton("Cancelar",null).setPositiveButton("Aceptar",(d,w) -> {
            String value=name.getText().toString().trim(); if (!value.isEmpty()) action.accept(value);
        }).show();
    }
    private void copyProfile() {
        if (model.profiles.size()>=TouchSettings.MAX_PROFILES) { toast("Máximo 16 perfiles."); return; }
        nameDialog("Copiar distribución", "Mis controles",name -> { model.duplicateProfile(name); refreshProfiles(); refreshControls(); preview.refreshLayout(); });
    }
    private void profileOptions() {
        new AlertDialog.Builder(activity).setTitle(model.currentProfile().name).setItems(new String[]{"Cambiar nombre","Restablecer distribución","Importar perfil","Exportar perfil","Eliminar perfil"},(d,which) -> {
            if (which==0) nameDialog("Nombre del perfil",model.currentProfile().name,n -> { model.currentProfile().name=n; refreshProfiles(); });
            if (which==1) new AlertDialog.Builder(activity).setMessage("¿Restablecer esta distribución?").setNegativeButton("Cancelar",null).setPositiveButton("Restablecer",(x,w) -> { preview.resetLayout(); refreshControls(); }).show();
            if (which==2 && canChooseFile()) host.chooseFile(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*").addCategory(Intent.CATEGORY_OPENABLE),IMPORT);
            if (which==3 && canChooseFile()) { pendingExport=TouchSettings.exportProfile(model); host.chooseFile(new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("application/json").addCategory(Intent.CATEGORY_OPENABLE).putExtra(Intent.EXTRA_TITLE,"bo2-controles.json"),EXPORT); }
            if (which==4) {
                if (!model.profile.startsWith("custom:")) { toast("Puedes restablecer este perfil o crear una copia."); return; }
                String old=model.profile; model.selectProfile(TouchProfiles.XBOX); model.profiles.remove(old); refreshProfiles(); refreshControls(); preview.refreshLayout();
            }
        }).show();
    }
    private void addControl() {
        if (model.controls.size()>=TouchSettings.MAX_CONTROLS) { toast("Máximo 64 controles por perfil."); return; }
        new AlertDialog.Builder(activity).setTitle("Añadir acción").setItems(TouchProfiles.editableLabels(),(d,n) -> {
            TouchController.Control c=model.addControl(TouchProfiles.editableActions()[n]); refreshControls(); preview.refreshLayout(); preview.selectControl(c.id);
        }).show();
    }
    private CheckBox check(LinearLayout body,String title,boolean value,java.util.function.Consumer<Boolean> change) {
        CheckBox c=new CheckBox(activity); c.setText(title); c.setChecked(value); c.setOnCheckedChangeListener((v,on) -> { change.accept(on); preview.invalidate(); }); body.addView(c); return c;
    }
    private void slider(LinearLayout body,String title,float initial,float min,float max,float step,java.util.function.Consumer<Float> change) {
        TextView text=label(title+" · "+Math.round(initial*100)+" %"); body.addView(text);
        SeekBar bar=new SeekBar(activity); bar.setMax(Math.round((max-min)/step)); bar.setProgress(Math.round((initial-min)/step));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar b,int n,boolean user) { if (user) { float value=min+n*step; change.accept(value); text.setText(title+" · "+Math.round(value*100)+" %"); preview.invalidate(); } }
            public void onStartTrackingTouch(SeekBar b) {} public void onStopTrackingTouch(SeekBar b) {}
        }); body.addView(bar);
    }
    private LinearLayout column() { LinearLayout b=new LinearLayout(activity); b.setOrientation(LinearLayout.VERTICAL); b.setPadding(dp(12),dp(6),dp(12),dp(6)); return b; }
    private ScrollView panel(View content) { ScrollView scroll=new ScrollView(activity); scroll.addView(content); return scroll; }
    private void properties() {
        TouchController.Control c=preview.selected(); if(c==null){toast("Selecciona un control.");return;}
        LinearLayout b=column(); check(b,"Visible durante la partida",c.enabled,on -> c.enabled=on);
        slider(b,"Opacidad",c.opacity,0,1,.01f,v -> c.opacity=v);
        if(c.stick!=0){slider(b,"Zona muerta",c.deadzone,0,.6f,.01f,v -> c.deadzone=v);slider(b,"Sensibilidad",c.sensitivity,.2f,3,.05f,v -> c.sensitivity=v);}
        else if(c.dragFire) b.addView(label("Este botón permite apuntar mientras disparas."));
        else if(c.trigger==1) b.addView(label("El arrastre desde Apuntar se ajusta en Respuesta."));
        else check(b,"Permitir arrastrar la cámara",c.look,on -> c.look=on);
        new AlertDialog.Builder(activity).setTitle(c.label).setView(panel(b)).setPositiveButton("Listo",(d,w) -> refreshControls()).show();
    }
    private void responseOptions() {
        LinearLayout b=column(); check(b,"Arrastrar cámara desde Apuntar",model.aimDrag,on -> model.aimDrag=on);
        check(b,"Correr al empujar el joystick hacia delante",model.stickSprint,on -> model.stickSprint=on);
        slider(b,"Umbral de carrera",model.sprintThreshold,.5f,1,.01f,v -> model.sprintThreshold=v);
        CheckBox adsFire=check(b,"Apuntar al disparar (botón principal)",model.autoAim,on -> model.autoAim=on);
        b.addView(label("Mantén Disparar para conservar la mira y el disparo. El disparo izquierdo es de cadera."));
        Spinner mode=new Spinner(activity); adapter(mode,new String[]{"Disparo continuo","Disparo por pulsos","Disparar al soltar"});
        mode.setSelection(model.fireMode); b.addView(mode);
        adsFire.setOnCheckedChangeListener((button,on) -> {
            model.autoAim=on;
            if(on){ model.fireMode=0; mode.setSelection(0); }
        });
        mode.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener(){
            public void onItemSelected(AdapterView<?> p,View v,int at,long id){model.fireMode=at;}
            public void onNothingSelected(AdapterView<?> p){}
        });
        new AlertDialog.Builder(activity).setTitle("Respuesta táctil").setView(panel(b)).setPositiveButton("Listo",null).show();
    }
    private boolean canChooseFile() {
        if (host.allowFilePicker()) return true;
        toast("Importa o exporta perfiles desde Ajustes en el menú del port."); return false;
    }
    void onActivityResult(int request,int result,Intent data) {
        if(result!=Activity.RESULT_OK || data==null || data.getData()==null)return;
        android.net.Uri uri=data.getData(); String exported=pendingExport;
        new Thread(() -> {
            try {
                if(request==IMPORT){
                    String text;
                    try(java.io.InputStream in=activity.getContentResolver().openInputStream(uri);java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream()){
                        if(in==null)throw new java.io.IOException(); byte[] buf=new byte[4096];int n;
                        while((n=in.read(buf))!=-1){if(out.size()+n>TouchSettings.MAX_BYTES)throw new java.io.IOException("too large");out.write(buf,0,n);}text=out.toString("UTF-8");
                    }
                    activity.runOnUiThread(() -> { if(activity.isFinishing()||activity.isDestroyed())return; if(TouchSettings.importProfile(model,text)){refreshProfiles();refreshControls();preview.refreshLayout();toast("Perfil importado. Pulsa Guardar para conservarlo.");}else toast("Perfil incompatible o dañado."); });
                }else if(request==EXPORT && exported!=null){
                    try(java.io.OutputStream out=activity.getContentResolver().openOutputStream(uri,"wt")){if(out==null)throw new java.io.IOException();out.write(exported.getBytes(StandardCharsets.UTF_8));}
                    activity.runOnUiThread(() -> toast("Perfil exportado."));
                }
            }catch(Exception error){activity.runOnUiThread(() -> toast("No se pudo abrir o guardar el perfil."));}
        },"bo2-touch-profile").start();
    }
    private void toast(String message){if(!activity.isDestroyed())Toast.makeText(activity,message,Toast.LENGTH_SHORT).show();}
    void saveState(Bundle state){state.putString("draft",TouchSettings.encodeLayout(model));state.putString("export",pendingExport);}
}
