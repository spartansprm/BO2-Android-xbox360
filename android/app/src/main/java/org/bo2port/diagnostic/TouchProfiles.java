// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import java.util.ArrayList;


final class TouchProfiles {
    static final String XBOX = "x360", CODM = "codm", CUSTOM = "custom:1";
    static final class Profile {
        final String id;
        String name;
        String base;
        final ArrayList<TouchController.Control> controls = new ArrayList<>();
        Profile(String id, String name) { this.id = id; this.name = name; this.base = CODM.equals(id) ? CODM : XBOX; }
        Profile copy(String id, String name) {
            Profile p = new Profile(id, name);
            p.base = base;
            for (TouchController.Control c : controls) p.controls.add(c.copy(c.id));
            return p;
        }
    }

    static final String[] ACTIONS = {"move", "shoot", "hipfire", "ads", "jump", "crouch", "reload",
        "interact", "change_weapon", "melee", "lethal", "tactical", "run", "scores", "pause",
        "dpad_up", "dpad_down", "dpad_left", "dpad_right"};
    static final String[] LABELS = {"MOVER", "DISPARAR", "DISPARO IZQ.", "APUNTAR", "SALTAR", "AGACHAR",
        "RECARGAR", "USAR", "ARMA", "GOLPE", "GRANADA", "TÁCTICA", "CORRER", "MARCADOR", "PAUSA",
        "↑", "↓", "←", "→"};

    static int actionIndex(String action) {
        for (int i = 0; i < ACTIONS.length; ++i) if (ACTIONS[i].equals(action)) return i;
        return -1;
    }
    static TouchController.Control control(String id, String action, String label, float x, float y, float size) {
        int index = actionIndex(action);
        if (index < 0) throw new IllegalArgumentException("Unknown touch action");
        int mask = 0, trigger = 0, stick = 0;
        switch (action) {
            case "move": stick = 1; break;
            case "shoot": case "hipfire": trigger = 2; break;
            case "ads": trigger = 1; break;
            case "jump": mask = 0x1000; break;
            case "crouch": mask = 0x2000; break;
            case "reload": case "interact": mask = 0x4000; break;
            case "change_weapon": mask = 0x8000; break;
            case "melee": mask = 0x80; break;
            case "lethal": mask = 0x200; break;
            case "tactical": mask = 0x100; break;
            case "run": mask = 0x40; break;
            case "scores": mask = 0x20; break;
            case "pause": mask = 0x10; break;
            case "dpad_up": mask = 1; break;
            case "dpad_down": mask = 2; break;
            case "dpad_left": mask = 4; break;
            case "dpad_right": mask = 8; break;
        }
        TouchController.Control c = new TouchController.Control(id, label == null ? LABELS[index] : label,
            size * 180f, mask, trigger, stick, false);
        c.action = action; c.normalX = x; c.normalY = y;
        c.dragFire = trigger == 2;
        c.look = action.equals("interact") || action.equals("lethal") || action.equals("tactical") || action.equals("melee");
        return c;
    }
    private static void add(Profile p, String id, String action, String label, float x, float y, float size) {
        p.controls.add(control(id, action, label, x, y, size));
    }
    static Profile defaults(String id) {
        Profile p = new Profile(id, XBOX.equals(id) ? "Xbox" : "CODM");
        if (XBOX.equals(id)) {
            add(p,"stick_left","move","LS",.14f,.70f,.36f);
            add(p,"a","jump","A",.88f,.70f,.14f);
            add(p,"b","crouch","B",.95f,.56f,.14f);
            add(p,"x","interact","X",.81f,.56f,.14f);
            add(p,"y","change_weapon","Y",.88f,.42f,.14f);
            add(p,"lt","ads","LT",.18f,.10f,.13f);
            add(p,"lb","tactical","LB",.18f,.28f,.13f);
            add(p,"rt","shoot","RT",.94f,.12f,.13f);
            add(p,"rb","lethal","RB",.94f,.28f,.13f);
            add(p,"rs_click","melee","R3",.53f,.90f,.11f);
            add(p,"dpad_up","dpad_up",null,.33f,.50f,.10f);
            add(p,"dpad_down","dpad_down",null,.33f,.70f,.10f);
            add(p,"dpad_left","dpad_left",null,.28f,.60f,.10f);
            add(p,"dpad_right","dpad_right",null,.38f,.60f,.10f);
            add(p,"back","scores","BACK",.42f,.08f,.10f);
            add(p,"start","pause","START",.58f,.08f,.10f);

            for (TouchController.Control c : p.controls) c.look = false;
        } else {
            add(p,"move","move",null,.145f,.74f,.32f);
            add(p,"shoot","shoot",null,.80f,.46f,.20f);
            add(p,"hipfire","hipfire",null,.08f,.32f,.14f);
            add(p,"ads","ads",null,.935f,.38f,.135f);
            add(p,"jump","jump",null,.935f,.62f,.14f);
            add(p,"crouch","crouch",null,.895f,.82f,.14f);
            add(p,"reload","reload",null,.77f,.85f,.13f);
            add(p,"interact","interact",null,.64f,.56f,.12f);
            add(p,"change_weapon","change_weapon",null,.57f,.89f,.13f);
            add(p,"melee","melee",null,.82f,.68f,.12f);
            add(p,"lethal","lethal",null,.69f,.72f,.115f);
            add(p,"tactical","tactical",null,.62f,.78f,.11f);

            add(p,"previous_scorestreak","dpad_up","RACHA ↑",.37f,.17f,.08f);
            add(p,"activate_scorestreak","dpad_right","USAR RACHA",.43f,.12f,.09f);
            add(p,"next_scorestreak","dpad_down","RACHA ↓",.49f,.17f,.08f);
            add(p,"alt_fire","dpad_left","ALT.",.68f,.32f,.10f);
            add(p,"scores","scores",null,.86f,.06f,.07f);
            add(p,"pause","pause",null,.22f,.07f,.08f);
        }
        return p;
    }

    static String[] editableActions() {
        ArrayList<String> out = new ArrayList<>();
        for (String action : ACTIONS) if (!action.equals("run")) out.add(action);
        return out.toArray(new String[0]);
    }
    static String[] editableLabels() {
        ArrayList<String> out = new ArrayList<>();
        for (int i=0;i<ACTIONS.length;i++) if (!ACTIONS[i].equals("run")) out.add(LABELS[i]);
        return out.toArray(new String[0]);
    }

    static void upgradeMobile(Profile p) {
        if (!p.id.equals(CODM) || p.controls.size()!=18) return;
        String[] ids={"move","shoot","hipfire","ads","jump","crouch","reload","interact","change_weapon","melee","lethal","tactical",
            "previous_scorestreak","activate_scorestreak","next_scorestreak","alt_fire","scores","pause"};
        float[] xs={.15f,.80f,.07f,.94f,.93f,.86f,.74f,.62f,.62f,.80f,.66f,.54f,.37f,.43f,.49f,.70f,.86f,.22f};
        float[] ys={.68f,.42f,.42f,.40f,.62f,.80f,.80f,.62f,.88f,.22f,.12f,.12f,.17f,.12f,.17f,.32f,.06f,.07f};
        float[] sizes={.40f,.20f,.15f,.15f,.14f,.14f,.13f,.13f,.13f,.13f,.12f,.12f,.08f,.09f,.08f,.10f,.07f,.08f};
        for (int i=0;i<ids.length;i++) {
            TouchController.Control c=p.controls.get(i);
            if (!ids[i].equals(c.id) || Math.abs(c.normalX-xs[i])>.00001f || Math.abs(c.normalY-ys[i])>.00001f ||
                Math.abs(c.baseRadius/180f-sizes[i])>.00001f || c.scale!=1) return;
        }
        Profile mobile=defaults(CODM);
        for (int i=0;i<mobile.controls.size();i++) {
            TouchController.Control old=p.controls.get(i), next=mobile.controls.get(i);
            next.enabled=old.enabled; next.opacity=old.opacity; next.deadzone=old.deadzone; next.sensitivity=old.sensitivity;
            next.look=old.look; next.dragFire=old.dragFire;
        }
        p.controls.clear(); p.controls.addAll(mobile.controls);
    }
    private TouchProfiles() {}
}
