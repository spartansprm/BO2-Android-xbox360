// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.Choreographer;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.ViewConfiguration;


final class TouchControlsView extends View {
    interface Sink { void update(TouchController.State state); }
    interface Selection { void changed(TouchController.Control control); }
    private final TouchController model;
    private final Sink sink;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final TouchHudRenderer hud = new TouchHudRenderer();
    private int left, top, right, bottom, editPointer = -1;
    private String selectedId;
    private float dragX, dragY;
    private Selection selection;
    private boolean framePending;
    private long lastFrame;
    private final Choreographer.FrameCallback cameraFrame = this::cameraFrame;
    private final android.os.Handler timer = new android.os.Handler(android.os.Looper.getMainLooper());
    private final Runnable timedInput = this::publishTimedInput;
    private void publishTimedInput() { model.time(android.os.SystemClock.uptimeMillis()); publish(); }

    TouchControlsView(Context context, TouchController model, Sink sink) {
        super(context); this.model = model; this.sink = sink;
        model.touchSlop = ViewConfiguration.get(context).getScaledTouchSlop();
        setBackgroundColor(Color.TRANSPARENT); setFocusable(false); setClickable(true);
        setContentDescription(model.editing ? "Arrastra un control y ajusta su tamaño" :
            "Controles táctiles. Desliza en una zona libre o sobre disparar para mover la cámara");
    }

    void setSelectionListener(Selection listener) { selection = listener; }
    boolean hitsControlAt(float x, float y) { return model.hit(x, y) != null; }
    void applySavedLayout(String encoded) {
        TouchSettings.readLayout(model, encoded);
        layoutControls();
    }
    void selectControl(String id) {
        selectedId = id;
        if (selection != null) selection.changed(selected());
        invalidate();
    }
    TouchController.Control selected() {
        for (TouchController.Control c : model.controls) if (c.id.equals(selectedId)) return c;
        return null;
    }
    void resizeSelected(float scale) {
        TouchController.Control c = selected();
        if (c != null) { model.place(c, c.x, c.y, scale); invalidate(); }
    }
    void resetLayout() { model.resetProfile(); selectedId = null; layoutControls(); }
    void refreshLayout() { selectedId = null; layoutControls(); }

    private void layoutControls() {
        releaseAll(); model.layout(getWidth(), getHeight(), left, top, right, bottom);
        if (selection != null) selection.changed(selected());
        invalidate();
    }
    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh); layoutControls();
    }
    @Override public WindowInsets onApplyWindowInsets(WindowInsets insets) {

        if (model.editing) return insets;
        left = insets.getSystemWindowInsetLeft(); right = insets.getSystemWindowInsetRight();
        top = insets.getSystemWindowInsetTop(); bottom = insets.getSystemWindowInsetBottom();
        if (insets.getDisplayCutout() != null) {
            left = Math.max(left, insets.getDisplayCutout().getSafeInsetLeft());
            right = Math.max(right, insets.getDisplayCutout().getSafeInsetRight());
            top = Math.max(top, insets.getDisplayCutout().getSafeInsetTop());
            bottom = Math.max(bottom, insets.getDisplayCutout().getSafeInsetBottom());
        }
        layoutControls(); return insets;
    }

    void releaseAll() {
        Choreographer.getInstance().removeFrameCallback(cameraFrame);
        framePending = false; lastFrame = 0; editPointer = -1;
        timer.removeCallbacks(timedInput);
        model.clear(); publish();
    }
    @Override protected void onDetachedFromWindow() { releaseAll(); super.onDetachedFromWindow(); }
    @Override protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        if (visibility != VISIBLE && model != null) releaseAll();
    }
    private void publish() {
        if (sink != null) sink.update(model.state()); invalidate();
        timer.removeCallbacks(timedInput);
        if (!model.editing) { long at = model.nextChange(); if (at >= 0) timer.postAtTime(timedInput,at); }
    }
    private void scheduleCamera() {
        if (model.camera == null && !framePending && (model.hasLookPointer() || model.hasPendingLook() ||
                model.state().rx != 0 || model.state().ry != 0)) {
            framePending = true; Choreographer.getInstance().postFrameCallback(cameraFrame);
        }
    }
    private void cameraFrame(long time) {
        framePending = false;
        float seconds = lastFrame == 0 ? 1f/60 : (time-lastFrame)/1_000_000_000f;
        lastFrame = time;
        model.advanceLook(seconds); publish();
        if (model.hasLookPointer() || model.state().rx != 0 || model.state().ry != 0) scheduleCamera(); else lastFrame = 0;
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        if (model.editing) return editTouch(event);
        model.time(android.os.SystemClock.uptimeMillis());
        int action = event.getActionMasked(), index = event.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN) {
            releaseAll(); model.down(event.getPointerId(index), event.getX(index), event.getY(index));
        } else if (action == MotionEvent.ACTION_POINTER_DOWN) {
            model.down(event.getPointerId(index), event.getX(index), event.getY(index));
        } else if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < event.getPointerCount(); ++i)
                model.move(event.getPointerId(i), event.getX(i), event.getY(i));
        } else if (action == MotionEvent.ACTION_POINTER_UP) {
            model.up(event.getPointerId(index), event.getX(index), event.getY(index));
        } else if (action == MotionEvent.ACTION_UP) {
            model.up(event.getPointerId(index), event.getX(index), event.getY(index)); performClick();
        } else if (action == MotionEvent.ACTION_CANCEL) releaseAll();
        publish(); scheduleCamera(); return true;
    }

    private boolean editTouch(MotionEvent event) {
        int action = event.getActionMasked(), index = event.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN) {
            TouchController.Control c = selected();
            if (c == null || !c.hit(event.getX(), event.getY())) c = model.hit(event.getX(), event.getY());
            selectedId = c == null ? null : c.id;
            editPointer = c == null ? -1 : event.getPointerId(0);
            if (c != null) { dragX = c.x-event.getX(); dragY = c.y-event.getY(); }
            if (selection != null) selection.changed(c);
        } else if (action == MotionEvent.ACTION_MOVE && editPointer != -1) {
            int i = event.findPointerIndex(editPointer);
            TouchController.Control c = selected();
            if (i >= 0 && c != null) model.place(c, event.getX(i)+dragX, event.getY(i)+dragY, c.scale);
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL ||
                (action == MotionEvent.ACTION_POINTER_UP && event.getPointerId(index) == editPointer)) {
            editPointer = -1;
            if (action == MotionEvent.ACTION_UP) performClick();
        }
        invalidate(); return true;
    }
    @Override public boolean performClick() { super.performClick(); return true; }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (model.editing) {
            paint.setColor(0x667E94A5); paint.setStrokeWidth(model.unit);
            canvas.drawLine(getWidth()/2f, 0, getWidth()/2f, getHeight(), paint);
            canvas.drawLine(0, getHeight()/2f, getWidth(), getHeight()/2f, paint);
        }
        TouchController.State state=model.state();

        for (int layer = 0; layer < 2; ++layer) for (int n = 0; n < model.controls.size(); ++n) {
            TouchController.Control c = model.controls.get(n);
            if (!model.visible(c) || (c.stick != 0 ? 0 : 1) != layer) continue;
            boolean selected = model.editing && c.id.equals(selectedId);
            hud.draw(canvas,model,c,selected,state);
        }
    }
}
