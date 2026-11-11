// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;


final class SceneResolution {
    static final String PREF="bo2zm_native_scene_resolution";
    static final String[] VALUES={"880x720","1280x720","1920x1080"};
    static final String[] LABELS={"Original: 880 × 720","HD: 1280 × 720","Full HD: 1920 × 1080"};
    private SceneResolution(){}

    static String valid(String value){
        for(String known:VALUES)if(known.equals(value))return known;
        return VALUES[0];
    }
}
