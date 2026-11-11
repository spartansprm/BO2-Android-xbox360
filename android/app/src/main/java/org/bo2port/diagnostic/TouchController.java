// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Map;


final class TouchController {
    interface Camera {
        void touch(int action, int pointer, float x, float y, float width, float height);
    }
    static final class Placement {
        final float x, y, scale;
        Placement(float x, float y, float scale) {
            this.x = clamp(x, 0, 1); this.y = clamp(y, 0, 1); this.scale = clamp(scale, .25f, 3);
        }
    }
    static final class Control {
        final String id, label;
        final int mask, trigger, stick;
        final boolean secondary;
        final float baseRadius;
        String action;
        float normalX, normalY, x, y, radius, scale = 1, opacity = .75f, deadzone = .12f, sensitivity = 1;
        boolean enabled = true, look, dragFire;
        Control(String id, String label, float radius, int mask, int trigger, int stick, boolean secondary) {
            this.id = id; this.label = label; baseRadius = radius;
            this.mask = mask; this.trigger = trigger; this.stick = stick; this.secondary = secondary;
        }
        Control copy(String id) {
            Control c = new Control(id, label, baseRadius, mask, trigger, stick, secondary);
            c.action = action; c.normalX = normalX; c.normalY = normalY; c.scale = scale;
            c.opacity = opacity; c.deadzone = deadzone; c.sensitivity = sensitivity; c.enabled = enabled; c.look = look; c.dragFire = dragFire;
            return c;
        }
        boolean hit(float px, float py) { return hit(px, py, 1); }
        boolean hit(float px, float py, float margin) {
            float dx = px-x, dy = py-y, r = radius*margin;
            return dx*dx+dy*dy <= r*r;
        }
    }
    static final class State {
        int buttons, lt, rt, lx, ly, rx, ry;
        boolean neutral() { return (buttons | lt | rt | lx | ly | rx | ry) == 0; }
    }
    private static final class Pointer {
        final Control control;
        final float downX, downY;
        float x, y;
        boolean dragged, camera;
        Pointer(Control c, float x, float y) { control = c; downX = this.x = x; downY = this.y = y; }
    }
    final LinkedHashMap<String, TouchProfiles.Profile> profiles = new LinkedHashMap<>();
    final LinkedHashMap<String, Placement> placements = new LinkedHashMap<>();
    ArrayList<Control> controls;
    String profile = TouchProfiles.CODM;
    private final LinkedHashMap<Integer, Pointer> pointers = new LinkedHashMap<>();
    private int lookPointer = -1, lookX, lookY;
    private float pendingX, pendingY, left, top, width = 1, height = 1;
    private boolean sprint;
    boolean autoAim, editing, aimDrag = true, stickSprint = true;
    float sprintThreshold = .85f, touchSlop = 8, unit = 1;
    Camera camera;
    private final TouchAdsFire adsFire = new TouchAdsFire();
    private long now;
    int fireMode;
    long fireRepeatMs = 220;
    void time(long uptime) { now = uptime; }
    long nextChange() { return autoAim ? adsFire.nextChange(now) : -1; }
    private boolean combined(Control c) { return autoAim && c != null && c.action.equals("shoot"); }

    TouchController() {
        profiles.put(TouchProfiles.XBOX, TouchProfiles.defaults(TouchProfiles.XBOX));
        profiles.put(TouchProfiles.CODM, TouchProfiles.defaults(TouchProfiles.CODM));
        profiles.put(TouchProfiles.CUSTOM, profiles.get(TouchProfiles.CODM).copy(TouchProfiles.CUSTOM, "Personalizado"));
        controls = profiles.get(profile).controls;
    }
    static float clamp(float v, float lo, float hi) { return Float.isFinite(v) ? Math.max(lo, Math.min(hi, v)) : lo; }
    TouchProfiles.Profile currentProfile() { return profiles.get(profile); }
    void selectProfile(String id) {
        if (!profiles.containsKey(id)) return;
        clear(); profile = id; controls = profiles.get(id).controls; geometry();
    }
    String duplicateProfile(String name) {
        String id = "custom:"+java.util.UUID.randomUUID();
        profiles.put(id, currentProfile().copy(id, name)); selectProfile(id); return id;
    }
    void resetProfile() {
        clear();
        TouchProfiles.Profile p = TouchProfiles.defaults(currentProfile().base);
        TouchProfiles.Profile old = currentProfile();
        old.controls.clear(); old.controls.addAll(p.controls); geometry();
    }
    Control addControl(String action) {
        if (action.equals("run")) throw new IllegalArgumentException("Sprint belongs to the movement stick");
        String id = action; int n = 2;
        while (find(id) != null) id = action+"#"+n++;
        Control c = TouchProfiles.control(id, action, null, .5f, .5f, action.equals("move") ? .36f : .13f);
        controls.add(c); geometry(); return c;
    }
    Control find(String id) { for (Control c : controls) if (c.id.equals(id)) return c; return null; }
    void remove(Control c) { clear(); controls.remove(c); }
    void layout(float w, float h, float left, float top, float right, float bottom) {
        clear(); this.left = left; this.top = top;
        width = Math.max(1, w-left-right); height = Math.max(1, h-top-bottom);
        unit = height/360f; geometry();
    }
    private void geometry() {
        for (Control c : controls) {
            c.radius = c.baseRadius*unit*c.scale;
            c.x = left+c.normalX*width; c.y = top+c.normalY*height;
        }
    }
    void place(Control c, float x, float y, float scale) {
        c.normalX = clamp((x-left)/width, 0, 1); c.normalY = clamp((y-top)/height, 0, 1);
        c.scale = clamp(scale, .25f, 3); geometry();
    }
    boolean visible(Control c) { return c.enabled || editing; }
    Control hit(float x, float y) {

        for (int kind = 0; kind < 2; ++kind)
            for (int i = controls.size()-1; i >= 0; --i) {
                Control c = controls.get(i);
                if ((c.stick != 0 ? 1 : 0) == kind && visible(c) && c.hit(x, y, kind == 1 ? 1.15f : 1)) return c;
            }
        return null;
    }
    private void cameraEvent(int action, int id, float x, float y) {
        if (camera != null) camera.touch(action, id, (x-left)/width, (y-top)/height, width, height);
    }
    private void acquire(int id, Pointer p, float x, float y) {
        lookPointer = id; p.camera = true; cameraEvent(0, id, x, y);
    }
    private boolean turnsCamera(Control c) {
        return c != null && c.stick == 0 && (c.look || c.dragFire ||
            (aimDrag && c.action.equals("ads")));
    }
    void down(int id, float x, float y) {
        if (id < 0 || pointers.containsKey(id) || pointers.size() >= 16 || !Float.isFinite(x) || !Float.isFinite(y) ||
            x < left || x > left+width || y < top || y > top+height) return;
        Control c = hit(x,y);
        if (c != null && c.stick != 0) for (Pointer p : pointers.values()) if (p.control == c) return;
        boolean otherCombined = false, aiming = false;
        for (Pointer held : pointers.values()) {
            otherCombined |= combined(held.control);
            aiming |= held.control != null && held.control.trigger == 1;
        }
        Pointer p = new Pointer(c,x,y); pointers.put(id,p);
        if (combined(c) && !otherCombined) { adsFire.configure(fireMode,fireRepeatMs); adsFire.press(now,aiming); }
        if (c == null) {
            Pointer owner = pointers.get(lookPointer);
            if (owner != null && owner.control != null) {
                cameraEvent(3, lookPointer, owner.x, owner.y); owner.camera = false; lookPointer = -1;
                pendingX = pendingY = 0;
            }
            if (lookPointer < 0) acquire(id,p,x,y);
        }
    }
    void move(int id, float x, float y) {
        Pointer p = pointers.get(id);
        if (p == null || !Float.isFinite(x) || !Float.isFinite(y)) return;
        if (!p.dragged && Math.hypot(x-p.downX,y-p.downY) > touchSlop) p.dragged = true;
        if (p.dragged && lookPointer < 0 && turnsCamera(p.control)) acquire(id,p,p.x,p.y);
        if (id == lookPointer) {
            if (camera == null) { pendingX += x-p.x; pendingY += p.y-y; }
            cameraEvent(1,id,x,y);
        }
        p.x = x; p.y = y;
    }
    void up(int id, float x, float y) {
        Pointer p = pointers.get(id);
        if (p == null) return;

        if (id == lookPointer) move(id,x,y);
        up(id);
    }
    void up(int id) {
        Pointer p = pointers.remove(id);
        if (p == null) return;
        if (combined(p.control)) {
            boolean another = false; for (Pointer held : pointers.values()) another |= combined(held.control);
            if (!another) adsFire.release(now);
        }
        if (lookPointer != id) return;
        cameraEvent(2,id,p.x,p.y); lookPointer = -1;
        for (Map.Entry<Integer,Pointer> e : pointers.entrySet()) if (e.getValue().control == null) {
            Pointer next = e.getValue(); acquire(e.getKey(),next,next.x,next.y); break;
        }
    }
    void clear() {
        cameraEvent(3,lookPointer,0,0); pointers.clear(); lookPointer = -1;
        pendingX = pendingY = 0; lookX = lookY = 0; sprint = false; adsFire.cancel();
    }
    boolean hasLookPointer() { return lookPointer != -1; }
    boolean hasPendingLook() { return pendingX != 0 || pendingY != 0; }

    void advanceLook(float seconds) {
        float x = pendingX, y = pendingY; pendingX = pendingY = 0; lookX = lookY = 0;
        if (camera != null) return;
        double length = Math.hypot(x,y);
        if (length < .01 || !Double.isFinite(length)) return;
        double speed = Math.min(1,length/unit/clamp(seconds,.001f,.05f)/900);
        double magnitude = (.25+.75*speed)*32767;
        lookX = (int)Math.round(x/length*magnitude); lookY = (int)Math.round(y/length*magnitude);
    }
    boolean pressed(Control c) { for (Pointer p : pointers.values()) if (p.control == c) return true; return false; }
    float[] thumb(Control c) {
        for (Pointer p : pointers.values()) if (p.control == c) {
            float x = p.x-c.x, y = p.y-c.y, d = (float)Math.hypot(x,y), f = d > c.radius ? c.radius/d : 1;
            return new float[]{c.x+x*f,c.y+y*f};
        }
        return new float[]{c.x,c.y};
    }
    static int[] stickAxes(float dx, float dy, float deadzone, float sensitivity) {
        float length = (float)Math.sqrt(dx*dx+dy*dy);
        if (length <= deadzone || length == 0 || !Float.isFinite(length)) return new int[]{0,0};
        float amount = Math.min(1, (length-deadzone)/(1-deadzone)*sensitivity);
        return new int[]{Math.round(dx/length*amount*32767), Math.round(-dy/length*amount*32767)};
    }
    State state() {
        State s = new State();
        if (editing) return s;
        boolean stickHeld = false;
        for (Pointer p : pointers.values()) {
            Control c = p.control;
            if (c == null || !c.enabled) continue;
            s.buttons |= c.mask;
            if (c.trigger == 1) s.lt = 255;
            if (c.trigger == 2 && !combined(c)) s.rt = 255;
            if (c.stick != 0 && c.radius > 0) {
                stickHeld = true;
                float x = (p.x-c.x)/c.radius, y = (p.y-c.y)/c.radius;
                int[] axes = stickAxes(x,y,c.deadzone,c.sensitivity); s.lx = axes[0]; s.ly = axes[1];
                double length = Math.hypot(x,y);
                sprint = stickSprint && length > 0 && -y/length >= .819 &&
                    Math.min(1,length) >= sprintThreshold*(sprint ? .9f : 1);
                if (sprint) s.buttons |= 0x40;
            }
        }
        if (!stickHeld) sprint = false;
        if (autoAim) { int timed = adsFire.triggers(now); if ((timed&1)!=0) s.lt=255; if ((timed&2)!=0) s.rt=255; }

        if (s.lt!=0 || s.rt!=0) { s.buttons &= ~0x40; sprint=false; }
        s.rx = lookX; s.ry = lookY;
        return s;
    }
}
