// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;


final class EdgeMenuGesture {
    private final float distance, slop;
    private int pointer = -1;
    private float startX, startY;
    private long started;

    EdgeMenuGesture(float distance, float slop) {
        this.distance = distance;
        this.slop = slop;
    }

    boolean begin(int id, float x, float y, long time, boolean available) {
        cancel();
        if (!available || id < 0 || !Float.isFinite(x) || !Float.isFinite(y)) return false;
        pointer = id; startX = x; startY = y; started = time;
        return true;
    }

    boolean move(int id, float x, float y, long time, int fingers) {
        if (pointer < 0) return false;
        if (id != pointer || fingers != 1 || time < started || time-started > 1000 ||
                !Float.isFinite(x) || !Float.isFinite(y)) { cancel(); return false; }
        float dx = x-startX, dy = Math.abs(y-startY);
        if (dx < -slop || (dy > slop && dy > Math.abs(dx)*.5f)) { cancel(); return false; }
        if (dx >= distance && dy <= dx*.5f) { cancel(); return true; }
        return false;
    }

    void cancel() { pointer = -1; }
}
