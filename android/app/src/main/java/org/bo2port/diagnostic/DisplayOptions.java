// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;


final class DisplayOptions {
    static final String OUTPUT_PREF="bo2zm_native_output_resolution";
    static final String[] OUTPUTS={"1280x720","1920x1080","auto"};
    static final String[] OUTPUT_LABELS={"Original: 1280 × 720","Full HD: 1920 × 1080","Automática (según la pantalla)"};
    static final String SCREEN_PREF="bo2zm_native_screen_mode";
    static final String[] SCREENS={"original","stretch","ultrawide"};


    static final String[] SCREEN_LABELS={
        "Original (16:9, con barras negras)",
        "Estirada (estira la imagen 16:9: DEFORMA las proporciones)",
        "Pantalla completa real (toda la pantalla, sin estirar ni deformar)"};
    private DisplayOptions(){}
    static String validOutput(String value){return pick(OUTPUTS,value);}
    static String validScreen(String value){return pick(SCREENS,value);}
    private static String pick(String[] known,String value){
        for(String k:known)if(k.equals(value))return k;
        return known[0];
    }

    static int[] landscape(int a,int b){
        if(a<=0||b<=0)return new int[]{0,0};
        return new int[]{Math.max(a,b),Math.min(a,b)};
    }
}
