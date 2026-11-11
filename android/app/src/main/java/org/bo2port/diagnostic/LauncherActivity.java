// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.animation.AnimatorSet;
import android.animation.ObjectAnimator;
import android.animation.PropertyValuesHolder;
import android.animation.ValueAnimator;
import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.animation.DecelerateInterpolator;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import java.util.ArrayList;


public final class LauncherActivity extends LandscapeActivity {
    private static final int PICK_GAME = 1;
    static final int ACCENT = 0xFFFF8A00, ACCENT_LIGHT = 0xFFFFB300, ACCENT_DEEP = 0xFFFF5E00;
    static final int BACKGROUND = 0xFF05080C, CARD = 0xD9121A24, CARD_STROKE = 0xFF243140, MUTED = 0xFF8E9AA7;
    private TacticalBackgroundView backdrop;
    private TextView main, hint;
    private View halo;
    private AnimatorSet pulse;
    private ObjectAnimator haloAnimator, floatAnimator;
    private ImageView emblem;
    private final ArrayList<View> entrance = new ArrayList<>();
    private boolean entered;

    static GradientDrawable rounded(int fill, int stroke, float radius) {
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(fill);
        shape.setCornerRadius(radius);
        if (stroke != 0) shape.setStroke(3, stroke);
        return shape;
    }
    static GradientDrawable accentPill(float radius) {
        GradientDrawable shape = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
            new int[] {ACCENT_LIGHT, ACCENT, ACCENT_DEEP});
        shape.setCornerRadius(radius);
        return shape;
    }
    private int dp(int value) { return Math.round(getResources().getDisplayMetrics().density * value); }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(BACKGROUND);
        getWindow().setNavigationBarColor(BACKGROUND);

        if (!GameLaunch.prefs(this).getBoolean("launcher_v051", false))
            GameLaunch.prefs(this).edit().putString(Sharpness.EFFECT_PREF, Sharpness.EFFECT_DEFAULT).putBoolean("launcher_v051", true).apply();

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(BACKGROUND);
        backdrop = new TacticalBackgroundView(this);
        backdrop.setGlowCenter(0.56f);
        root.addView(backdrop, new FrameLayout.LayoutParams(-1, -1));
        LinearLayout landscape=new LinearLayout(this);landscape.setOrientation(LinearLayout.HORIZONTAL);
        FrameLayout brand=new FrameLayout(this);
        LinearLayout actions=new LinearLayout(this);actions.setOrientation(LinearLayout.VERTICAL);
        actions.setGravity(Gravity.CENTER_VERTICAL);actions.setPadding(dp(12),0,dp(12),0);
        landscape.addView(brand,new LinearLayout.LayoutParams(0,-1,.44f));
        landscape.addView(actions,new LinearLayout.LayoutParams(0,-1,.56f));
        root.addView(landscape,new FrameLayout.LayoutParams(-1,-1));


        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.VERTICAL);
        header.setGravity(Gravity.CENTER_HORIZONTAL);
        emblem = new ImageView(this);
        emblem.setImageResource(R.drawable.emblem_reticle);
        header.addView(emblem, new LinearLayout.LayoutParams(dp(80), dp(80)));
        TextView title = new TextView(this);
        title.setText("BLACK OPS II");
        title.setTextColor(Color.WHITE);
        title.setTextSize(30);title.setMaxLines(1);
        title.setAutoSizeTextTypeUniformWithConfiguration(18,30,1,android.util.TypedValue.COMPLEX_UNIT_SP);
        title.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        title.setLetterSpacing(0.14f);
        title.setShadowLayer(dp(10), 0, 0, 0xAAFF8A00);
        title.setGravity(Gravity.CENTER);
        title.setPadding(0, dp(14), 0, dp(8));

        header.addView(title);
        View line = new View(this);
        line.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
            new int[] {0x00FF8A00, ACCENT, 0x00FF8A00}));
        header.addView(line, new LinearLayout.LayoutParams(dp(150), dp(2)));
        TextView modes = new TextView(this);
        modes.setText(BuildConfig.BO2_CAMPAIGN_AVAILABLE ? "CAMPAÑA  ·  MULTIJUGADOR  ·  ZOMBIES" : "MULTIJUGADOR  ·  ZOMBIES");
        modes.setTextColor(MUTED);
        modes.setTextSize(11);
        modes.setLetterSpacing(0.22f);
        modes.setGravity(Gravity.CENTER);
        modes.setPadding(0, dp(10), 0, 0);
        header.addView(modes);
        FrameLayout.LayoutParams top = new FrameLayout.LayoutParams(-1, -2, Gravity.CENTER);
        brand.addView(header, top);
        entrance.add(header);


        FrameLayout centre = new FrameLayout(this);
        halo = new View(this);
        GradientDrawable ring = new GradientDrawable();
        ring.setShape(GradientDrawable.RECTANGLE);
        ring.setCornerRadius(dp(40));
        ring.setColor(0x00000000);
        ring.setStroke(dp(2), ACCENT);
        halo.setBackground(ring);
        halo.setAlpha(0f);
        FrameLayout.LayoutParams haloLayout=new FrameLayout.LayoutParams(-1,dp(64),Gravity.CENTER);
        haloLayout.leftMargin=dp(20);haloLayout.rightMargin=dp(20);centre.addView(halo,haloLayout);
        main = new TextView(this);
        main.setGravity(Gravity.CENTER);
        main.setTextSize(21);
        main.setLetterSpacing(0.12f);
        main.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        main.setTextColor(0xFF140A00);
        main.setBackground(accentPill(dp(38)));
        main.setElevation(dp(14));
        main.setOnClickListener(v -> onMain());
        main.setOnTouchListener((v, event) -> {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) v.animate().scaleX(0.95f).scaleY(0.95f).setDuration(90).start();
            if (event.getActionMasked() == MotionEvent.ACTION_UP || event.getActionMasked() == MotionEvent.ACTION_CANCEL)
                v.animate().scaleX(1f).scaleY(1f).setDuration(140).start();
            return false;
        });
        FrameLayout.LayoutParams mainLayout=new FrameLayout.LayoutParams(-1,dp(64),Gravity.CENTER);
        mainLayout.leftMargin=dp(20);mainLayout.rightMargin=dp(20);centre.addView(main,mainLayout);
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setGravity(Gravity.CENTER_HORIZONTAL);
        column.addView(centre, new LinearLayout.LayoutParams(-1,dp(108)));
        hint = new TextView(this);
        hint.setTextColor(MUTED);
        hint.setTextSize(13);
        hint.setGravity(Gravity.CENTER);
        hint.setPadding(dp(24), 0, dp(24), 0);
        column.addView(hint, new LinearLayout.LayoutParams(-1, -2));
        actions.addView(column,new LinearLayout.LayoutParams(-1,-2));
        entrance.add(column);


        LinearLayout footer = new LinearLayout(this);
        footer.setOrientation(LinearLayout.VERTICAL);
        footer.setGravity(Gravity.CENTER_HORIZONTAL);
        footer.setPadding(dp(20),0,dp(20),0);
        TextView settings = new TextView(this);
        settings.setText("⚙   AJUSTES");
        settings.setGravity(Gravity.CENTER);
        settings.setTextSize(15);
        settings.setLetterSpacing(0.16f);
        settings.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        settings.setTextColor(Color.WHITE);
        settings.setBackground(rounded(CARD, 0x99FF8A00, dp(26)));
        settings.setOnClickListener(v -> {
            startActivity(new Intent(this, SettingsActivity.class));
            overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out);
        });
        footer.addView(settings, new LinearLayout.LayoutParams(-1,dp(48)));
        TextView version = new TextView(this);
        version.setText("v" + BuildConfig.VERSION_NAME);
        version.setTextColor(0x668E9AA7);
        version.setTextSize(11);
        version.setPadding(0, dp(14), 0, 0);
        footer.addView(version);
        LinearLayout.LayoutParams bottom=new LinearLayout.LayoutParams(-1,-2);bottom.topMargin=dp(20);
        actions.addView(footer,bottom);
        entrance.add(footer);

        setContentView(root);
        LandscapeWindow.inset(root,dp(24),dp(12));
        for (View view : entrance) { view.setAlpha(0f); view.setTranslationY(dp(26)); }
    }

    @Override protected void onResume() {
        super.onResume();
        backdrop.setAnimating(true);
        if (!entered) {
            entered = true;
            long delay = 80;
            for (View view : entrance) {
                view.animate().alpha(1f).translationY(0).setStartDelay(delay).setDuration(520)
                    .setInterpolator(new DecelerateInterpolator(1.6f)).start();
                delay += 120;
            }
        }
        floatAnimator = ObjectAnimator.ofFloat(emblem, View.TRANSLATION_Y, 0f, -dp(6));
        floatAnimator.setDuration(2400);
        floatAnimator.setRepeatMode(ValueAnimator.REVERSE);
        floatAnimator.setRepeatCount(ValueAnimator.INFINITE);
        floatAnimator.start();
        refresh();
    }
    @Override protected void onPause() {
        backdrop.setAnimating(false);
        stopPulse();
        if (floatAnimator != null) { floatAnimator.cancel(); floatAnimator = null; }
        super.onPause();
    }

    private void refresh() {
        boolean ready = GameLaunch.savedGameTree(this) != null;
        main.setText(ready ? "▶   JUGAR" : "＋   AGREGAR JUEGO");
        hint.setText(ready ? "Toca para entrar al menú del juego" : "Elige la carpeta que contiene 00007000 y 000B0000");
        if (ready) startPulse(); else stopPulse();
    }
    private void startPulse() {
        if (pulse != null) return;
        ObjectAnimator breathe = ObjectAnimator.ofPropertyValuesHolder(main,
            PropertyValuesHolder.ofFloat(View.SCALE_X, 1f, 1.05f), PropertyValuesHolder.ofFloat(View.SCALE_Y, 1f, 1.05f),
            PropertyValuesHolder.ofFloat(View.ALPHA, 1f, 0.72f));
        breathe.setDuration(900);
        breathe.setRepeatMode(ValueAnimator.REVERSE);
        breathe.setRepeatCount(ValueAnimator.INFINITE);
        haloAnimator = ObjectAnimator.ofPropertyValuesHolder(halo,
            PropertyValuesHolder.ofFloat(View.SCALE_X, 1f, 1.28f), PropertyValuesHolder.ofFloat(View.SCALE_Y, 1f, 1.7f),
            PropertyValuesHolder.ofFloat(View.ALPHA, 0.75f, 0f));
        haloAnimator.setDuration(1800);
        haloAnimator.setRepeatCount(ValueAnimator.INFINITE);
        pulse = new AnimatorSet();
        pulse.playTogether(breathe, haloAnimator);
        pulse.start();
    }
    private void stopPulse() {
        if (pulse != null) { pulse.cancel(); pulse = null; }
        main.setAlpha(1f); main.setScaleX(1f); main.setScaleY(1f);
        halo.setAlpha(0f); halo.setScaleX(1f); halo.setScaleY(1f);
    }

    private void onMain() {
        if (!BuildConfig.BO2_GAME_APP_AVAILABLE) {
            Toast.makeText(this, "Este APK se compiló sin el juego.", Toast.LENGTH_LONG).show();
            return;
        }
        Uri tree = GameLaunch.savedGameTree(this);
        if (tree == null) {
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            pick.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            startActivityForResult(pick, PICK_GAME);
            return;
        }

        GameLaunch.start(this, tree, BuildConfig.BO2_CAMPAIGN_AVAILABLE ? "cp" : "zm", false);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_GAME || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri tree = data.getData();
        if (!GameLaunch.looksLikeGameFolder(this, tree)) {
            Toast.makeText(this, "Esa carpeta no tiene 00007000 y 000B0000. Elige la carpeta que contiene las dos.",
                Toast.LENGTH_LONG).show();
            return;
        }
        if (!GameLaunch.rememberGameTree(this, tree)) {
            Toast.makeText(this, "Android no permitió recordar la carpeta. Vuelve a elegirla.", Toast.LENGTH_LONG).show();
            return;
        }
        refresh();
        Toast.makeText(this, "Juego agregado.", Toast.LENGTH_SHORT).show();
    }
}
