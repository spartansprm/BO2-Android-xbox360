// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.animation.DecelerateInterpolator;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.Switch;
import org.json.JSONObject;


public final class SettingsActivity extends LandscapeActivity {
    private static final int PICK_LIBRARY = 1;
    private SharedPreferences prefs;
    private LinearLayout panel;
    private TacticalBackgroundView backdrop;
    private TextView libraryStatus, libraryButton;
    private Switch aimOnFire;
    private volatile boolean installing;
    private FrameLayout pageSlot;
    private final java.util.ArrayList<ScrollView> pages=new java.util.ArrayList<>();
    private final java.util.ArrayList<TextView> tabs=new java.util.ArrayList<>();
    private int selectedPage;


    private static final String[] RESOLUTION_SCENE = {"880x720", "1280x720", "1920x1080"};
    private static final String[] RESOLUTION_OUTPUT = {"1280x720", "1280x720", "1920x1080"};
    private static final String[] RESOLUTION_LABELS = {"Original (como Xbox 360)", "HD · 720p", "Full HD · 1080p"};

    private static final String[] SCREEN_VALUES = {"ultrawide", "original", "stretch"};
    private static final String[] SCREEN_LABELS = {"Pantalla completa", "Original", "Pantalla completa estirada"};

    private static final String[] TEXTURE_VALUES = {"0", "1", "-1"};
    private static final String[] TEXTURE_LABELS = {"Máxima (recomendada)", "Media", "La del preset del juego (baja por defecto)"};

    private static final String[] PRESET_VALUES = {"", "low", "medium", "high"};
    private static final String[] PRESET_LABELS = {"La del menú del juego (muy baja por defecto)", "Baja", "Media", "Alta (como Xbox 360)"};

    private static final String[] ANISO_VALUES = {"0", "4", "8"};
    private static final String[] ANISO_LABELS = {"El del juego (2×–4×)", "4×", "8×"};

    private int dp(int value) { return Math.round(getResources().getDisplayMetrics().density * value); }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = GameLaunch.prefs(this);
        getWindow().setStatusBarColor(LauncherActivity.BACKGROUND);
        getWindow().setNavigationBarColor(LauncherActivity.BACKGROUND);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(LauncherActivity.BACKGROUND);
        backdrop = new TacticalBackgroundView(this);
        backdrop.setGlowCenter(0.08f);
        root.addView(backdrop, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout horizontal=new LinearLayout(this);horizontal.setOrientation(LinearLayout.HORIZONTAL);
        LinearLayout rail=new LinearLayout(this);rail.setOrientation(LinearLayout.VERTICAL);rail.setPadding(0,0,dp(12),0);
        ScrollView railScroll=new ScrollView(this);railScroll.setFillViewport(true);railScroll.addView(rail);
        pageSlot=new FrameLayout(this);
        int railWidth=dp(Math.max(156,Math.min(210,getResources().getConfiguration().screenWidthDp/4)));
        horizontal.addView(railScroll,new LinearLayout.LayoutParams(railWidth,-1));
        horizontal.addView(pageSlot,new LinearLayout.LayoutParams(0,-1,1));
        root.addView(horizontal,new FrameLayout.LayoutParams(-1,-1));


        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        TextView back = new TextView(this);
        back.setText("‹");
        back.setTextColor(LauncherActivity.ACCENT);
        back.setTextSize(36);
        back.setPadding(0, 0, dp(14), dp(4));
        back.setOnClickListener(v -> finish());
        back.setContentDescription("Volver al menú");bar.addView(back,new LinearLayout.LayoutParams(dp(44),dp(48)));
        TextView title = new TextView(this);
        title.setText("AJUSTES");
        title.setTextColor(Color.WHITE);
        title.setTextSize(20);
        title.setLetterSpacing(0.14f);
        title.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        title.setShadowLayer(dp(8), 0, 0, 0x88FF8A00);
        bar.addView(title);
        rail.addView(bar);
        View line = new View(this);
        line.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
            new int[] {LauncherActivity.ACCENT, 0x00FF8A00}));
        LinearLayout.LayoutParams lineLayout = new LinearLayout.LayoutParams(-1,dp(2));
        lineLayout.topMargin = dp(4);
        rail.addView(line, lineLayout);
        TextView note = new TextView(this);
        note.setText("Se aplican la próxima vez que pulses Jugar.");
        note.setTextColor(LauncherActivity.MUTED);
        note.setTextSize(13);
        note.setPadding(0, dp(10), 0, dp(4));
        rail.addView(note);
        String[] categories={"Controles","Imagen","Biblioteca"};
        for(int i=0;i<categories.length;i++) {
            final int index=i;TextView tab=new TextView(this);tab.setText(categories[i]);tab.setTextSize(15);
            tab.setGravity(Gravity.CENTER_VERTICAL);tab.setPadding(dp(14),0,dp(10),0);tab.setOnClickListener(v->showPage(index));
            LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,dp(48));lp.topMargin=dp(10);rail.addView(tab,lp);tabs.add(tab);
        }
        panel=newPage();

        LinearLayout controlsCard = card("Configuración de controles", "Distribución móvil CODM, Xbox y perfiles personalizados. " +
            "Corre empujando el joystick hacia delante y arrastra desde Disparar para mover la cámara. Durante la partida, desliza el indicador " +
            "del borde izquierdo hacia la derecha para abrir Controles o Salir.");
        aimOnFire=new Switch(this); aimOnFire.setText("Apuntar al disparar");
        aimOnFire.setTextColor(Color.WHITE); aimOnFire.setTextSize(16); aimOnFire.setPadding(0,dp(12),0,dp(8));
        controlsCard.addView(aimOnFire,new LinearLayout.LayoutParams(-1,dp(60)));
        TextView firingHelp=new TextView(this); firingHelp.setTextColor(LauncherActivity.MUTED); firingHelp.setTextSize(13);
        firingHelp.setText("El botón principal levanta la mira y mantiene el disparo mientras lo pulsas. Puedes arrastrar para apuntar. El disparo izquierdo es de cadera.");
        controlsCard.addView(firingHelp); refreshFiringOption();
        TextView editControls = new TextView(this);
        editControls.setText("EDITAR CONTROLES"); editControls.setTextColor(Color.WHITE);
        editControls.setTextSize(15); editControls.setGravity(Gravity.CENTER);
        editControls.setBackground(LauncherActivity.rounded(0x33FF8A00, LauncherActivity.ACCENT, dp(12)));
        editControls.setOnClickListener(v -> startActivity(new Intent(this, ControlLayoutActivity.class)));
        LinearLayout.LayoutParams editLayout = new LinearLayout.LayoutParams(-1, dp(48));
        editLayout.topMargin = dp(10); controlsCard.addView(editControls, editLayout);

        panel=newPage();
        int scene = indexOf(RESOLUTION_SCENE, SceneResolution.valid(prefs.getString(SceneResolution.PREF, RESOLUTION_SCENE[0])));
        choiceCard("Resolución", "Más resolución, imagen más nítida y más trabajo para el teléfono.",
            RESOLUTION_LABELS, scene, i -> prefs.edit().putString(SceneResolution.PREF, RESOLUTION_SCENE[i])
                .putString(DisplayOptions.OUTPUT_PREF, RESOLUTION_OUTPUT[i]).apply());
        int screen = indexOf(SCREEN_VALUES, DisplayOptions.validScreen(prefs.getString(DisplayOptions.SCREEN_PREF, "original")));
        choiceCard("Tipo de pantalla", "Pantalla completa: toda la pantalla sin deformar (en Campaña, de momento 16:9). " +
                "Original: 16:9 con barras. Estirada: toda la pantalla, pero deforma la imagen.",
            SCREEN_LABELS, screen, i -> prefs.edit().putString(DisplayOptions.SCREEN_PREF, SCREEN_VALUES[i]).apply());
        int texture = indexOf(TEXTURE_VALUES, Sharpness.validTexture(prefs.getString(Sharpness.TEXTURE_PREF, Sharpness.TEXTURE_DEFAULT)));
        choiceCard("Nitidez", "Detalle de las texturas: paredes, suelos y armas. Por debajo de Máxima se ven borrosas " +
                "aunque subas la resolución.",
            TEXTURE_LABELS, texture, i -> prefs.edit().putString(Sharpness.TEXTURE_PREF, TEXTURE_VALUES[i]).apply());
        int preset = indexOf(PRESET_VALUES, Sharpness.validPreset(prefs.getString(Sharpness.PRESET_PREF, Sharpness.PRESET_DEFAULT)));
        choiceCard("Calidad del escenario", "Detalle de los modelos a distancia, distancia de dibujado, sombras, luces y " +
                "efectos. Más calidad, menos FPS.",
            PRESET_LABELS, preset, i -> prefs.edit().putString(Sharpness.PRESET_PREF, PRESET_VALUES[i]).apply());
        int aniso = indexOf(ANISO_VALUES, Sharpness.validAniso(prefs.getString(Sharpness.ANISO_PREF, Sharpness.ANISO_DEFAULT)));
        choiceCard("Filtrado anisotrópico", "Mantiene nítidos los suelos y paredes vistos en ángulo.",
            ANISO_LABELS, aniso, i -> prefs.edit().putString(Sharpness.ANISO_PREF, ANISO_VALUES[i]).apply());

        panel=newPage();
        LinearLayout library = card("Biblioteca de shaders",
            "Shaders ya preparados: el juego no tiene que traducirlos la primera vez (evita tirones iniciales). " +
            "La Campaña también usa los que comparte con Zombies y Multijugador.");
        libraryButton = new TextView(this);
        libraryButton.setText("＋   AGREGAR BIBLIOTECA");
        libraryButton.setGravity(Gravity.CENTER);
        libraryButton.setTextColor(0xFF140A00);
        libraryButton.setTextSize(15);
        libraryButton.setLetterSpacing(0.1f);
        libraryButton.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        libraryButton.setBackground(LauncherActivity.accentPill(dp(24)));
        libraryButton.setElevation(dp(6));
        libraryButton.setOnClickListener(v -> pickLibrary());
        LinearLayout.LayoutParams buttonLayout = new LinearLayout.LayoutParams(-1, dp(48));
        buttonLayout.topMargin = dp(12);
        library.addView(libraryButton, buttonLayout);
        libraryStatus = new TextView(this);
        libraryStatus.setTextColor(LauncherActivity.MUTED);
        libraryStatus.setTextSize(13);
        libraryStatus.setPadding(0, dp(10), 0, 0);
        library.addView(libraryStatus);
        showLibrary();

        setContentView(root);
        LandscapeWindow.inset(root,dp(16),dp(12));
        showPage(state==null?0:state.getInt("page",0));
    }

    private LinearLayout newPage() {
        LinearLayout column=new LinearLayout(this);column.setOrientation(LinearLayout.VERTICAL);column.setPadding(dp(8),0,dp(8),dp(20));
        ScrollView scroll=new ScrollView(this);scroll.setFillViewport(true);scroll.addView(column);scroll.setVisibility(View.GONE);
        pageSlot.addView(scroll,new FrameLayout.LayoutParams(-1,-1));pages.add(scroll);return column;
    }
    private void showPage(int index) {
        selectedPage=Math.max(0,Math.min(pages.size()-1,index));
        for(int i=0;i<pages.size();i++) {
            boolean on=i==selectedPage;pages.get(i).setVisibility(on?View.VISIBLE:View.GONE);tabs.get(i).setSelected(on);
            tabs.get(i).setTextColor(on?Color.WHITE:LauncherActivity.MUTED);
            tabs.get(i).setBackground(LauncherActivity.rounded(on?0x33FF8A00:0x14FFFFFF,on?LauncherActivity.ACCENT:0x22FFFFFF,dp(12)));
        }
    }
    @Override protected void onSaveInstanceState(Bundle state){super.onSaveInstanceState(state);state.putInt("page",selectedPage);}

    private void refreshFiringOption() {
        aimOnFire.setOnCheckedChangeListener(null);
        aimOnFire.setChecked(TouchSettings.fromPreferences(prefs).autoAim);
        aimOnFire.setOnCheckedChangeListener((button,on) -> TouchSettings.setAutoAim(prefs,on));
    }
    @Override protected void onResume() { super.onResume(); backdrop.setAnimating(true); refreshFiringOption(); }
    @Override protected void onPause() { backdrop.setAnimating(false); super.onPause(); }
    @Override public void finish() { super.finish(); overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out); }

    private interface Choice { void chosen(int index); }

    private static int indexOf(String[] values, String value) {
        for (int i = 0; i < values.length; i++) if (values[i].equals(value)) return i;
        return -1;
    }


    private LinearLayout card(String name, String help) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setBackground(LauncherActivity.rounded(LauncherActivity.CARD, LauncherActivity.CARD_STROKE, dp(18)));
        box.setPadding(dp(18), dp(16), dp(18), dp(18));
        LinearLayout heading = new LinearLayout(this);
        heading.setOrientation(LinearLayout.HORIZONTAL);
        heading.setGravity(Gravity.CENTER_VERTICAL);
        View mark = new View(this);
        mark.setBackground(LauncherActivity.accentPill(dp(2)));
        heading.addView(mark, new LinearLayout.LayoutParams(dp(4), dp(18)));
        TextView label = new TextView(this);
        label.setText(name.toUpperCase(java.util.Locale.ROOT));
        label.setTextColor(Color.WHITE);
        label.setTextSize(15);
        label.setLetterSpacing(0.12f);
        label.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        label.setPadding(dp(10), 0, 0, 0);
        heading.addView(label);
        box.addView(heading);
        TextView description = new TextView(this);
        description.setText(help);
        description.setTextColor(LauncherActivity.MUTED);
        description.setTextSize(13);
        description.setPadding(0, dp(6), 0, 0);
        box.addView(description);
        LinearLayout.LayoutParams layout = new LinearLayout.LayoutParams(-1, -2);
        layout.topMargin = dp(14);
        panel.addView(box, layout);
        return box;
    }

    private void choiceCard(String name, String help, String[] labels, int selected, Choice choice) {
        LinearLayout box = card(name, help);
        final TextView[] rows = new TextView[labels.length];
        for (int i = 0; i < labels.length; i++) {
            TextView row = new TextView(this);
            row.setText(labels[i]);
            row.setTextSize(15);
            row.setPadding(dp(16), 0, dp(16), 0);
            row.setGravity(Gravity.CENTER_VERTICAL);
            rows[i] = row;
            final int index = i;
            row.setOnClickListener(v -> {
                for (int k = 0; k < rows.length; k++) styleRow(rows[k], k == index);
                v.animate().scaleX(0.97f).scaleY(0.97f).setDuration(70)
                    .withEndAction(() -> v.animate().scaleX(1f).scaleY(1f).setDuration(120).start()).start();
                choice.chosen(index);
            });
            styleRow(row, i == selected);
            LinearLayout.LayoutParams layout = new LinearLayout.LayoutParams(-1, dp(46));
            layout.topMargin = dp(8);
            box.addView(row, layout);
        }
    }
    private void styleRow(TextView row, boolean on) {
        row.setBackground(LauncherActivity.rounded(on ? 0x33FF8A00 : 0x14FFFFFF, on ? LauncherActivity.ACCENT : 0x22FFFFFF, dp(12)));
        row.setTextColor(on ? Color.WHITE : 0xFFC7D0D9);
        row.setTypeface(on ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
        String text = row.getText().toString().replaceFirst("^● {2}", "");
        row.setText(on ? "●  " + text : text);
    }

    private void showLibrary() {
        JSONObject library;
        try { library = GameShaderAssets.imported(this); } catch (Exception error) { library = null; }
        libraryStatus.setText(library != null ? "✓  Instalada: " + library.optInt("shaders") + " shaders."
            : "No hay biblioteca instalada: el juego prepara los shaders la primera vez que los usa.");
        libraryStatus.setTextColor(library != null ? 0xFF7CDB8A : LauncherActivity.MUTED);
    }
    private void pickLibrary() {
        if (installing) return;
        if (GameLaunch.gameProcessRunning(this)) {
            Toast.makeText(this, "Cierra primero el juego.", Toast.LENGTH_LONG).show();
            return;
        }
        Toast.makeText(this, "Elige la carpeta con biblioteca-shaders.json y sus ZIP.", Toast.LENGTH_LONG).show();
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(pick, PICK_LIBRARY);
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_LIBRARY || result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri folder = data.getData();
        installing = true;
        libraryButton.setEnabled(false);
        libraryButton.setAlpha(0.5f);
        libraryStatus.setTextColor(LauncherActivity.MUTED);
        libraryStatus.setText("Verificando e instalando…");
        Thread worker = new Thread(() -> {
            String message;
            try {
                final long[] last = {0};
                JSONObject library = ShaderLibraryInstaller.install(new ShaderLibrarySource(getContentResolver(), folder), getFilesDir(),
                    (done, total) -> {
                        long now = System.nanoTime();
                        if (now - last[0] < 1_000_000_000L) return;
                        last[0] = now;
                        final long percent = total > 0 ? done * 100 / total : 0;
                        runOnUiThread(() -> libraryStatus.setText("Instalando… " + percent + " %"));
                    });
                prefs.edit().putBoolean("use_imported_library", true).apply();
                message = "Biblioteca instalada: " + library.optInt("shaders") + " shaders.";
            } catch (Exception error) {
                message = "No se pudo instalar: " + error.getMessage();
            }
            final String finished = message;
            runOnUiThread(() -> {
                installing = false;
                if (isFinishing() || isDestroyed()) return;
                libraryButton.setEnabled(true);
                libraryButton.setAlpha(1f);
                showLibrary();
                Toast.makeText(this, finished, Toast.LENGTH_LONG).show();
            });
        }, "install-shader-library");
        worker.setDaemon(true);
        worker.start();
    }
}
