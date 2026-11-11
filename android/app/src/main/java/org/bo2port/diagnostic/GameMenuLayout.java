// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.content.Context;
import android.graphics.Rect;
import android.os.Build;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.Collections;


final class GameMenuLayout extends FrameLayout {
    private final TouchControlsView controls;
    private final EdgeMenuGesture gesture;
    private final Rect edge = new Rect();
    private final View scrim;
    private final LinearLayout drawer;
    private final TextView handle;
    private final Button edit;
    private int leftInset, topInset, rightInset, bottomInset;
    private boolean open, captured, pointerStream;
    private View editor;

    GameMenuLayout(Context context, TouchControlsView controls, Runnable editControls, Runnable leaveGame) {
        super(context);
        this.controls = controls;
        gesture = new EdgeMenuGesture(dp(48), ViewConfiguration.get(context).getScaledTouchSlop());
        addView(controls, new LayoutParams(-1, -1));

        handle = new TextView(context) {

            @Override public boolean onTouchEvent(MotionEvent event) { return false; }
            @Override public boolean performClick() {
                super.performClick(); return GameMenuLayout.this.performClick();
            }
        };
        handle.setText("›"); handle.setTextColor(LauncherActivity.ACCENT); handle.setTextSize(24);
        handle.setGravity(Gravity.CENTER);
        handle.setBackground(LauncherActivity.rounded(0x88212B36, 0x88FF8A00, dp(8)));
        handle.setContentDescription("Abrir menú de partida: Controles y Salir");
        handle.setClickable(true); handle.setFocusable(true);
        addView(handle, new LayoutParams(dp(24), dp(80)));

        scrim = new View(context); scrim.setBackgroundColor(0x99000000);
        scrim.setOnClickListener(v -> closeMenu());
        scrim.setVisibility(GONE); addView(scrim, new LayoutParams(-1, -1));

        drawer = new LinearLayout(context); drawer.setOrientation(LinearLayout.VERTICAL);
        drawer.setGravity(Gravity.TOP); drawer.setBackgroundColor(LauncherActivity.BACKGROUND);
        drawer.setClickable(true); drawer.setVisibility(GONE);
        TextView title = new TextView(context); title.setText("Menú"); title.setTextSize(24);
        title.setTextColor(0xFFFFFFFF); title.setPadding(0, 0, 0, dp(20)); drawer.addView(title);
        edit = button("Controles", () -> { closeMenu(); editControls.run(); });
        button("Salir", () -> { closeMenu(); leaveGame.run(); });
        TextView hint = new TextView(context); hint.setText("Atrás o toca fuera para volver a la partida");
        hint.setTextColor(LauncherActivity.MUTED); hint.setTextSize(13); hint.setPadding(0, dp(20), 0, 0);
        drawer.addView(hint); addView(drawer, new LayoutParams(dp(280), -1, Gravity.LEFT));
        updateBounds();
    }

    private int dp(int value) { return Math.round(getResources().getDisplayMetrics().density*value); }

    private Button button(String title, Runnable action) {
        Button b = new Button(getContext()); b.setText(title); b.setAllCaps(false); b.setTextSize(18);
        b.setTextColor(0xFFFFFFFF);
        b.setBackground(LauncherActivity.rounded(LauncherActivity.CARD, LauncherActivity.CARD_STROKE, dp(12)));
        b.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, dp(56)); p.bottomMargin = dp(12);
        drawer.addView(b, p); return b;
    }

    boolean isOpen() { return open; }

    boolean isEditing() { return editor!=null; }

    void showEditor(View view) {
        if (editor!=null) return;
        closeMenu(); gesture.cancel(); captured=false; pointerStream=false; controls.releaseAll();
        editor=view; controls.setVisibility(INVISIBLE); handle.setVisibility(GONE);
        addView(editor,new LayoutParams(-1,-1)); editor.requestApplyInsets(); editor.requestFocus();
    }

    void hideEditor() {
        if (editor==null) return;
        removeView(editor); editor=null; controls.releaseAll();
        controls.setVisibility(VISIBLE); handle.setVisibility(VISIBLE);
    }

    private void openMenu() {
        if (open || editor!=null) return;
        open = true; controls.releaseAll();


        if (pointerStream) captured = true;
        handle.setVisibility(GONE); scrim.setVisibility(VISIBLE); drawer.setVisibility(VISIBLE);

        drawer.setTranslationX(-Math.max(drawer.getWidth(), drawer.getLayoutParams().width));
        drawer.animate().translationX(0).setDuration(160).start();
        edit.requestFocus();
        announceForAccessibility("Menú de partida. Controles. Salir.");
    }

    @Override public boolean dispatchTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) pointerStream = true;
        try { return super.dispatchTouchEvent(event); }
        finally {
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) pointerStream = false;
        }
    }

    boolean closeMenu() {
        gesture.cancel();
        if (!open) return false;
        open = false; drawer.animate().cancel(); drawer.setTranslationX(0);
        drawer.setVisibility(GONE); scrim.setVisibility(GONE); handle.setVisibility(VISIBLE);
        controls.releaseAll(); return true;
    }

    @Override public boolean onInterceptTouchEvent(MotionEvent event) {
        if (editor!=null) return false;

        if (captured) return true;
        if (open) return false;
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
            float x = event.getX(), y = event.getY();
            captured = gesture.begin(event.getPointerId(0), x, y, event.getEventTime(),
                edge.contains((int)x, (int)y) && !controls.hitsControlAt(x, y));
            return captured;
        }
        return false;
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        if (!captured) return super.onTouchEvent(event);
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_MOVE || action == MotionEvent.ACTION_UP) {
            if (gesture.move(event.getPointerId(0), event.getX(), event.getY(), event.getEventTime(),
                    event.getPointerCount())) performClick();
        } else if (action == MotionEvent.ACTION_POINTER_DOWN) gesture.cancel();
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            gesture.cancel(); captured = false;
        }
        return true;
    }

    @Override public boolean performClick() { super.performClick(); openMenu(); return true; }

    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        gesture.cancel(); captured = false; controls.releaseAll(); updateBounds();
    }

    @Override public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        leftInset = insets.getSystemWindowInsetLeft(); topInset = insets.getSystemWindowInsetTop();
        rightInset = insets.getSystemWindowInsetRight(); bottomInset = insets.getSystemWindowInsetBottom();
        if (insets.getDisplayCutout() != null) {
            leftInset = Math.max(leftInset, insets.getDisplayCutout().getSafeInsetLeft());
            topInset = Math.max(topInset, insets.getDisplayCutout().getSafeInsetTop());
            rightInset = Math.max(rightInset, insets.getDisplayCutout().getSafeInsetRight());
            bottomInset = Math.max(bottomInset, insets.getDisplayCutout().getSafeInsetBottom());
        }
        gesture.cancel(); captured = false; updateBounds();
        return super.onApplyWindowInsets(insets);
    }

    private void updateBounds() {
        int safeH = Math.max(0, getHeight()-topInset-bottomInset);
        int middle = topInset+safeH/2, half = Math.min(dp(56), safeH/2);
        edge.set(leftInset, middle-half, Math.min(getWidth()-rightInset, leftInset+dp(24)), middle+half);
        LayoutParams h = (LayoutParams)handle.getLayoutParams();
        h.leftMargin = leftInset; h.topMargin = Math.max(topInset, middle-dp(40)); handle.setLayoutParams(h);
        LayoutParams d = (LayoutParams)drawer.getLayoutParams();
        d.width = Math.max(0, Math.min(dp(280)+leftInset, getWidth()-rightInset)); drawer.setLayoutParams(d);
        drawer.setPadding(leftInset+dp(20), topInset+dp(20), dp(20), bottomInset+dp(20));

        if (Build.VERSION.SDK_INT >= 29)
            setSystemGestureExclusionRects(edge.isEmpty() ? Collections.emptyList() : Collections.singletonList(new Rect(edge)));
    }

    @Override public void onWindowFocusChanged(boolean focused) {
        if (!focused) { gesture.cancel(); captured = false; pointerStream = false; closeMenu(); }
        super.onWindowFocusChanged(focused);
    }

    @Override protected void onDetachedFromWindow() {
        gesture.cancel(); captured = false; pointerStream = false; closeMenu(); super.onDetachedFromWindow();
    }
}
