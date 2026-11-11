// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.content.Intent;
import android.content.SharedPreferences;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;


final class TouchSettings {
    static final String AUTO_AIM = "touch_auto_aim_on_fire";
    static final String LAYOUT = "touch_layout_v1";
    static final int MAX_BYTES = 1048576, MAX_CONTROLS = 64, MAX_PROFILES = 16;
    private TouchSettings() {}
    static Intent addTo(Intent intent, SharedPreferences prefs) {
        return intent.putExtra(AUTO_AIM,prefs.getBoolean(AUTO_AIM,false)).putExtra(LAYOUT,prefs.getString(LAYOUT,"{}"));
    }
    static TouchController fromIntent(Intent intent) {
        TouchController m = new TouchController();
        m.autoAim = intent.getBooleanExtra(AUTO_AIM,false); readLayout(m,intent.getStringExtra(LAYOUT)); return m;
    }
    static TouchController fromPreferences(SharedPreferences prefs) {
        return fromIntent(addTo(new Intent(),prefs));
    }
    static void setAutoAim(SharedPreferences prefs, boolean enabled) {
        TouchController model=fromPreferences(prefs);
        model.autoAim=enabled;
        if (enabled) model.fireMode=0;
        prefs.edit().putBoolean(AUTO_AIM,enabled).putString(LAYOUT,encodeLayout(model)).apply();
    }
    static boolean readLayout(TouchController model, String text) {
        if (text == null || text.length() > MAX_BYTES) return false;
        try {
            JSONObject root = new JSONObject(text);
            TouchController loaded = new TouchController();
            loaded.autoAim = model.autoAim;
            if (!root.has("schema")) migrate(loaded,root);
            else {
                if (root.getInt("schema") != 2) return false;
                JSONArray profiles = root.getJSONArray("profiles");
                if (profiles.length() < 1 || profiles.length() > MAX_PROFILES) return false;
                for (int i = 0; i < profiles.length(); ++i) {
                    TouchProfiles.Profile p = decodeProfile(profiles.getJSONObject(i));

                    for (int j = 0; j < i; ++j)
                        if (p.id.equals(profiles.getJSONObject(j).getString("id"))) throw new JSONException("Duplicate profile");
                    loaded.profiles.put(p.id,p);
                }
                if (loaded.profiles.size() > MAX_PROFILES) return false;
                String active = root.getString("active");
                if (!loaded.profiles.containsKey(active)) return false;
                loaded.selectProfile(active);
                JSONObject options = root.optJSONObject("options");
                if (options != null) {
                    loaded.aimDrag = options.optBoolean("aimDrag",true);
                    loaded.stickSprint = options.optBoolean("stickSprint",true);
                    loaded.sprintThreshold = number(options,"sprintThreshold",.85f,.5f,1);
                    loaded.autoAim = options.optBoolean("adsFire",loaded.autoAim);
                    loaded.fireMode = options.optInt("fireMode",0);
                    if (loaded.fireMode < 0 || loaded.fireMode > 2) return false;
                    loaded.fireRepeatMs = options.optLong("fireRepeatMs",220);
                    if (loaded.fireRepeatMs < 170 || loaded.fireRepeatMs > 2000) return false;
                }
            }
            boolean removedSprint=false;
            for (TouchProfiles.Profile p : loaded.profiles.values()) {
                removedSprint |= p.controls.removeIf(c -> c.action.equals("run"));
                if (root.optInt("hudRevision",0)<1) {
                    for (TouchController.Control c : p.controls) if(c.action.equals("shoot")) c.dragFire=true;
                    TouchProfiles.upgradeMobile(p);
                }
            }
            if (removedSprint) loaded.stickSprint=true;
            model.clear(); model.profiles.clear(); model.profiles.putAll(loaded.profiles);
            model.selectProfile(loaded.profile); model.aimDrag = loaded.aimDrag; model.stickSprint = loaded.stickSprint;
            model.sprintThreshold = loaded.sprintThreshold; model.autoAim = loaded.autoAim;
            model.fireMode = loaded.fireMode; model.fireRepeatMs = loaded.fireRepeatMs;
            model.placements.clear(); return true;
        } catch (JSONException | IllegalArgumentException ignored) { return false; }
    }
    private static float number(JSONObject o, String key, float fallback, float lo, float hi) throws JSONException {
        double d = o.optDouble(key,fallback);
        if (!Double.isFinite(d) || d < lo-0.000001 || d > hi+0.000001) throw new JSONException("Invalid "+key);
        return TouchController.clamp((float)d,lo,hi);
    }
    private static String shortText(String value, int max) throws JSONException {
        if (value.isEmpty() || value.length() > max || value.indexOf('\0') >= 0) throw new JSONException("Invalid text");
        return value;
    }
    private static TouchProfiles.Profile decodeProfile(JSONObject o) throws JSONException {
        String id = shortText(o.getString("id"),80);
        if (!id.equals(TouchProfiles.XBOX) && !id.equals(TouchProfiles.CODM) && !id.startsWith("custom:"))
            throw new JSONException("Invalid profile ID");
        TouchProfiles.Profile p = new TouchProfiles.Profile(id,shortText(o.getString("name"),48));
        p.base = o.optString("base",p.base);
        if (!p.base.equals(TouchProfiles.XBOX) && !p.base.equals(TouchProfiles.CODM)) throw new JSONException("Invalid base profile");
        JSONArray controls = o.getJSONArray("controls");
        if (controls.length() > MAX_CONTROLS) throw new JSONException("Too many controls");
        for (int i = 0; i < controls.length(); ++i) {
            JSONObject c = controls.getJSONObject(i);
            String cid = shortText(c.getString("id"),80), action = c.getString("action");
            if (TouchProfiles.actionIndex(action) < 0) throw new JSONException("Unsupported action");
            for (TouchController.Control previous : p.controls) if (previous.id.equals(cid)) throw new JSONException("Duplicate control");
            TouchController.Control out = TouchProfiles.control(cid,action,shortText(c.getString("label"),48),
                number(c,"x",.5f,0,1),number(c,"y",.5f,0,1),number(c,"size",.13f,.03f,1.2f));
            out.scale = number(c,"scale",1,.25f,3); out.opacity = number(c,"opacity",.75f,0,1);
            out.sensitivity = number(c,"sensitivity",1,.2f,3); out.deadzone = number(c,"deadzone",.12f,0,.6f);
            out.enabled = c.optBoolean("enabled",true); out.look = c.optBoolean("look",out.look);
            out.dragFire = c.optBoolean("dragFire",out.dragFire); p.controls.add(out);
        }
        return p;
    }
    private static void migrate(TouchController m, JSONObject old) throws JSONException {
        if (old.length() == 0) return;
        TouchProfiles.Profile p = m.profiles.get(TouchProfiles.XBOX).copy("custom:migrated","Mis controles anteriores");
        String[] before = {"fire","aim","jump","crouch","use","weapon","melee","grenade","tactical","pause","score","up","down","left","right","move"};
        String[] after = {"rt","lt","a","b","x","y","rs_click","rb","lb","start","back","dpad_up","dpad_down","dpad_left","dpad_right","stick_left"};
        float[] radius = {33,29,26,26,27,24,23,23,23,25,25,19,19,19,19,64};
        boolean found = false;
        for (int i = 0; i < before.length; ++i) {
            JSONArray pos = old.optJSONArray(before[i]);
            if (pos == null || pos.length() != 3) continue;
            float x = (float)pos.optDouble(0,Double.NaN), y = (float)pos.optDouble(1,Double.NaN), scale = (float)pos.optDouble(2,Double.NaN);
            if (!Float.isFinite(x) || !Float.isFinite(y) || !Float.isFinite(scale)) continue;
            for (TouchController.Control c : p.controls) if (c.id.equals(after[i])) {
                c.normalX = TouchController.clamp(x,0,1); c.normalY = TouchController.clamp(y,0,1);
                c.scale = TouchController.clamp(radius[i]*scale/c.baseRadius,.25f,3); found = true;
            }
        }
        if (found) { m.profiles.put(p.id,p); m.selectProfile(p.id); }
    }
    private static JSONObject encodeProfile(TouchProfiles.Profile p) throws JSONException {
        JSONArray controls = new JSONArray();
        for (TouchController.Control c : p.controls) controls.put(new JSONObject()
            .put("id",c.id).put("action",c.action).put("label",c.label).put("x",(double)c.normalX).put("y",(double)c.normalY)
            .put("size",(double)(c.baseRadius/180f)).put("scale",(double)c.scale).put("opacity",(double)c.opacity)
            .put("enabled",c.enabled).put("look",c.look).put("dragFire",c.dragFire)
            .put("deadzone",(double)c.deadzone).put("sensitivity",(double)c.sensitivity));
        return new JSONObject().put("id",p.id).put("name",p.name).put("base",p.base).put("controls",controls);
    }
    static String encodeLayout(TouchController m) {
        try {
            JSONArray profiles = new JSONArray();
            for (TouchProfiles.Profile p : m.profiles.values()) profiles.put(encodeProfile(p));
            return new JSONObject().put("schema",2).put("hudRevision",1).put("active",m.profile).put("profiles",profiles)
                .put("options",new JSONObject().put("aimDrag",m.aimDrag).put("stickSprint",m.stickSprint)
                    .put("sprintThreshold",(double)m.sprintThreshold).put("adsFire",m.autoAim)
                    .put("fireMode",m.fireMode).put("fireRepeatMs",m.fireRepeatMs)).toString();
        } catch (JSONException error) { throw new IllegalStateException("Invalid touch settings",error); }
    }
    static String exportProfile(TouchController m) {
        try { return new JSONObject().put("schema",2).put("hudRevision",1).put("active",m.profile)
            .put("profiles",new JSONArray().put(encodeProfile(m.currentProfile()))).toString(); }
        catch (JSONException error) { throw new IllegalStateException(error); }
    }
    static boolean importProfile(TouchController m, String text) {
        if (m.profiles.size() >= MAX_PROFILES || text == null || text.length() > MAX_BYTES) return false;
        TouchController imported = new TouchController();
        if (!readLayout(imported,text)) return false;
        String id = "custom:"+java.util.UUID.randomUUID();
        TouchProfiles.Profile p = imported.currentProfile().copy(id,imported.currentProfile().name);
        m.profiles.put(id,p); m.selectProfile(id); return true;
    }
}
