// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;
import android.content.Context;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;


final class GameShaderAssets {
    private GameShaderAssets() {}


    static JSONObject imported(Context context)throws Exception {
        ShaderLibraryInstaller.recover(context.getFilesDir());
        File directory=new File(context.getFilesDir(),"game-shaders");
        File installed=new File(directory,"manifest.json");
        if(!installed.isFile())return null;
        JSONObject external=ShaderLibraryInstaller.readJson(new java.io.FileInputStream(installed),256*1024);
        if(!external.optBoolean("external_library"))return null;
        JSONArray entries=ShaderLibraryInstaller.validateFiles(external);
        for(int i=0;i<entries.length();i++) {
            JSONObject row=entries.getJSONObject(i);
            if(!ShaderLibraryInstaller.matches(new File(directory,row.getString("file")),row))
                throw new java.io.IOException("Biblioteca importada dañada. Vuelve a importarla o desactiva su uso.");
        }
        ShaderLibraryInstaller.validatePair(directory,external.getInt("shaders"));
        return external;
    }


    static boolean removeBundledCopy(Context context) {
        File directory=new File(context.getFilesDir(),"game-shaders");
        if(!directory.isDirectory())return false;
        File manifest=new File(directory,"manifest.json");
        try{
            if(manifest.isFile()&&ShaderLibraryInstaller.readJson(new java.io.FileInputStream(manifest),256*1024).optBoolean("external_library"))
                return false;
        }catch(Exception unreadable){return false;}
        boolean removed=false;
        for(String name:new String[]{"bo2zm_shaders.spvpack","bo2zm_shaders.spvidx","manifest.json",
                "bo2zm_shaders.spvpack.part","bo2zm_shaders.spvidx.part","shader-build-results.json"}){
            File file=new File(directory,name);
            if(file.exists()&&file.delete())removed=true;
        }
        return removed;
    }
}
