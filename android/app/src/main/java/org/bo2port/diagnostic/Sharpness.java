// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import java.util.Locale;


final class Sharpness {
    static final String EFFECT_PREF="present_effect";
    static final String[] EFFECTS={"bilinear","cas","fsr"};
    static final String[] EFFECT_LABELS={
        "Desactivado (bilinear, como antes)",
        "CAS: enfoque adaptativo de AMD (el más barato)",
        "FSR 1.0: escalado que respeta bordes + enfoque (recomendado)"};
    static final String EFFECT_DEFAULT="bilinear";

    static final String STRENGTH_PREF="bo2zm_sharpness_strength";
    static final int STRENGTH_DEFAULT=60;

    static final String TEXTURE_PREF="bo2zm_texture_mip_floor";
    static final String[] TEXTURES={"0","1","2","-1"};
    static final String[] TEXTURE_LABELS={
        "Completas (máxima nitidez, más ancho de banda)",
        "Mitad de resolución (equilibrio)",
        "Un cuarto (lo que hacía el preset VERY LOW)",
        "Según el preset del juego (recomendado para FPS)"};


    static final String TEXTURE_DEFAULT="0";


    static final String PRESET_PREF="bo2zm_preset";
    static final String[] PRESETS={"","low","medium","high"};
    static final String PRESET_DEFAULT="";

    static final String ANISO_PREF="bo2zm_native_anisotropic";
    static final String[] ANISOS={"0","2","4","8"};
    static final String[] ANISO_LABELS={"Desactivado (como Xbox 360)","2x","4x","8x"};
    static final String ANISO_DEFAULT="0";

    private Sharpness(){}


    static String pick(String[] known,String value,String fallback){
        for(String k:known)if(k.equals(value))return k;
        return fallback;
    }
    static String validEffect(String v){return pick(EFFECTS,v,EFFECT_DEFAULT);}
    static String validTexture(String v){return pick(TEXTURES,v,TEXTURE_DEFAULT);}
    static String validAniso(String v){return pick(ANISOS,v,ANISO_DEFAULT);}
    static String validPreset(String v){return pick(PRESETS,v,PRESET_DEFAULT);}
    static int clamp(int strength){return Math.max(0,Math.min(100,strength));}


    static String casSharpness(int strength){
        return String.format(Locale.ROOT,"%.2f",clamp(strength)/100.0);
    }

    static String fsrSharpnessReduction(int strength){
        return String.format(Locale.ROOT,"%.2f",2.0*(1.0-clamp(strength)/100.0));
    }
}
