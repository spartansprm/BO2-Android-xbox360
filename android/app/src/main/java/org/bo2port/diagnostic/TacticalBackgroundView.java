// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.Shader;
import android.os.SystemClock;
import android.view.View;
import java.util.Random;


final class TacticalBackgroundView extends View {
    private static final int EMBERS = 46;
    private final float[] x = new float[EMBERS], y = new float[EMBERS], speed = new float[EMBERS],
        size = new float[EMBERS], drift = new float[EMBERS], phase = new float[EMBERS];
    private final Random random = new Random(1983);
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint grid = new Paint();
    private Shader sky, vignette, scan;
    private float glowCenterY = 0.5f;
    private boolean animating, seeded;
    private long last;
    private final float density;

    TacticalBackgroundView(Context context) {
        super(context);
        density = context.getResources().getDisplayMetrics().density;
        grid.setColor(0x0FFFFFFF);
        grid.setStrokeWidth(Math.max(1f, density * 0.6f));
    }


    void setGlowCenter(float fraction) { glowCenterY = fraction; invalidate(); }

    void setAnimating(boolean on) {
        animating = on;
        last = SystemClock.uptimeMillis();
        if (on) postInvalidateOnAnimation();
    }

    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        sky = new LinearGradient(0, 0, 0, h, new int[] {0xFF04070B, 0xFF0B121B, 0xFF101A26}, new float[] {0f, 0.55f, 1f},
            Shader.TileMode.CLAMP);
        vignette = new RadialGradient(w / 2f, h / 2f, Math.max(w, h) * 0.75f, new int[] {0x00000000, 0x00000000, 0xC0000000},
            new float[] {0f, 0.55f, 1f}, Shader.TileMode.CLAMP);
        scan = new LinearGradient(0, 0, 0, density * 90, new int[] {0x00FF9A1F, 0x14FF9A1F, 0x00FF9A1F}, null,
            Shader.TileMode.CLAMP);
        if (!seeded && w > 0 && h > 0) {
            seeded = true;
            for (int i = 0; i < EMBERS; i++) seed(i, w, h, true);
        }
    }

    private void seed(int i, int w, int h, boolean anywhere) {
        x[i] = random.nextFloat() * w;
        y[i] = anywhere ? random.nextFloat() * h : h + random.nextFloat() * h * 0.2f;
        speed[i] = density * (10 + random.nextFloat() * 28);
        size[i] = density * (0.8f + random.nextFloat() * 2.2f);
        drift[i] = density * (random.nextFloat() * 14 - 7);
        phase[i] = random.nextFloat() * 6.2832f;
    }

    @Override protected void onDraw(Canvas canvas) {
        final int w = getWidth(), h = getHeight();
        if (w == 0 || h == 0) return;
        final long now = SystemClock.uptimeMillis();
        final float dt = Math.min(0.05f, (now - last) / 1000f);
        last = now;
        final float t = now / 1000f;

        paint.setShader(sky);
        canvas.drawRect(0, 0, w, h, paint);
        paint.setShader(null);


        final float step = density * 34;
        for (float gx = (w % step) / 2; gx < w; gx += step) canvas.drawLine(gx, 0, gx, h, grid);
        for (float gy = h; gy > 0; gy -= step * (0.55f + 0.45f * gy / h)) canvas.drawLine(0, gy, w, gy, grid);


        final float breath = 0.82f + 0.18f * (float) Math.sin(t * 2.1);
        final float cy = h * glowCenterY;
        final float radius = Math.min(w, h) * 0.62f * breath;
        paint.setShader(new RadialGradient(w / 2f, cy, radius, new int[] {0x55FF8A00, 0x22FF6A00, 0x00FF6A00},
            new float[] {0f, 0.45f, 1f}, Shader.TileMode.CLAMP));
        canvas.drawCircle(w / 2f, cy, radius, paint);
        paint.setShader(null);


        for (int i = 0; i < EMBERS; i++) {
            y[i] -= speed[i] * dt;
            x[i] += (float) Math.sin(t * 0.9 + phase[i]) * drift[i] * dt;
            if (y[i] < -size[i] * 4) seed(i, w, h, false);
            final float flicker = 0.55f + 0.45f * (float) Math.sin(t * 3.3 + phase[i] * 2);
            final float fade = Math.max(0f, Math.min(1f, y[i] / (h * 0.85f)));
            final int alpha = (int) (200 * flicker * fade);
            paint.setColor((alpha << 24) | 0xFFB347);
            canvas.drawCircle(x[i], y[i], size[i], paint);
            paint.setColor(((alpha / 5) << 24) | 0xFF7A00);
            canvas.drawCircle(x[i], y[i], size[i] * 3.2f, paint);
        }


        final float sweep = (t * 0.12f % 1f) * (h + density * 90) - density * 90;
        canvas.save();
        canvas.translate(0, sweep);
        paint.setShader(scan);
        canvas.drawRect(0, 0, w, density * 90, paint);
        paint.setShader(null);
        canvas.restore();

        paint.setShader(vignette);
        canvas.drawRect(0, 0, w, h, paint);
        paint.setShader(null);

        if (animating) postInvalidateOnAnimation();
    }
}
