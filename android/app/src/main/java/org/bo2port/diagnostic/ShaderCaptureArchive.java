// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.Comparator;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;


final class ShaderCaptureArchive {
    static final long MAX_BYTES=32L*1024*1024,MAX_LOG=8L*1024*1024;
    static boolean compatibleSession(JSONObject session){
        if(session==null||!session.optBoolean("game_entry_executed"))return false;
        String version=session.optString("app_version");
        return (BuildConfig.VERSION_NAME.equals(version)||"0.19-cache-diagnostics".equals(version))&&
            "ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e".equals(session.optString("loaded_image_sha256"));
    }
    static File prepare(File files,File cache,JSONObject session)throws Exception {
        String run=session.optString("run_id");
        if(!compatibleSession(session)||!run.matches("[0-9a-f-]{36}"))throw new IOException("No completed game attempt to export");
        File root=new File(files,"shader-capture/"+run).getCanonicalFile();
        File[] containers=root.listFiles((dir,name)->name.matches("[vp]_[0-9a-f]{32}\\.bin"));
        if(containers==null||containers.length==0||containers.length>8192)throw new IOException("No missing shader containers captured");
        Arrays.sort(containers,Comparator.comparing(File::getName));
        File target=new File(cache,"bo2-shaders-capturados-privados.zip"),part=new File(target.getPath()+".part");
        JSONArray entries=new JSONArray();long total=0;
        try {
            try(ZipOutputStream zip=new ZipOutputStream(new FileOutputStream(part))) {
                zip.setLevel(6);
                for(File item:containers) {
                    if(!item.isFile()||Files.isSymbolicLink(item.toPath())||!item.getCanonicalFile().getParentFile().equals(root)||
                        item.length()<24||item.length()>65536||item.length()>MAX_BYTES-total)throw new IOException("Invalid capture file/budget");
                    byte[] data=Files.readAllBytes(item.toPath());total+=data.length;
                    String name="containers/"+item.getName();
                    entries.put(new JSONObject().put("file",name).put("bytes",data.length).put("sha256",sha(data)));
                    zip.putNextEntry(new ZipEntry(name));zip.write(data);zip.closeEntry();
                }

                File log=new File(files,"game-sdk.log");
                if(log.isFile()) {
                    if(log.length()>MAX_LOG)throw new IOException("Game log exceeds the 8-MiB export limit");
                    zip.putNextEntry(new ZipEntry("game-sdk.log"));
                    try(FileInputStream input=new FileInputStream(log)){copy(input,zip);}zip.closeEntry();
                }
                JSONObject manifest=new JSONObject().put("schema",1).put("kind","bo2-runtime-shader-capture")
                    .put("private_game_resources",true).put("run_id",run).put("app_version",session.getString("app_version"))
                    .put("loaded_image_sha256",session.getString("loaded_image_sha256"))
                    .put("capture",session.optJSONObject("shader_capture")).put("containers",entries);
                byte[] metadata=manifest.toString(2).getBytes(StandardCharsets.UTF_8);
                if(metadata.length>4*1024*1024)throw new IOException("Capture manifest too large");
                zip.putNextEntry(new ZipEntry("capture_manifest.json"));zip.write(metadata);zip.closeEntry();
                zip.putNextEntry(new ZipEntry("game-session.json"));zip.write(session.toString(2).getBytes(StandardCharsets.UTF_8));zip.closeEntry();
            }
            if(part.length()>48L*1024*1024)throw new IOException("Capture ZIP exceeds 48 MiB");
            Files.move(part.toPath(),target.toPath(),StandardCopyOption.REPLACE_EXISTING,StandardCopyOption.ATOMIC_MOVE);
            return target;
        }finally{part.delete();}
    }
    private static void copy(java.io.InputStream input,java.io.OutputStream output)throws IOException {
        byte[] buffer=new byte[65536];for(int count;(count=input.read(buffer))!=-1;)output.write(buffer,0,count);
    }
    private static String sha(byte[] data)throws Exception {
        byte[] hash=java.security.MessageDigest.getInstance("SHA-256").digest(data);
        StringBuilder result=new StringBuilder();for(byte b:hash)result.append(String.format(java.util.Locale.ROOT,"%02x",b&255));return result.toString();
    }
}
