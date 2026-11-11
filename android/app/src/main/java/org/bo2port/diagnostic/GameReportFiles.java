// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import org.json.JSONArray;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;


final class GameReportFiles {
    private static final int CHECKPOINT_LIMIT=131072;
    static JSONObject checkpoint(File directory,String name) {
        for(String suffix:new String[]{"",".bak"}) {
            String text=ProcessResourceSampler.read(new File(directory,name+suffix),CHECKPOINT_LIMIT);
            if(text!=null)try{return new JSONObject(text);}catch(Exception ignored){}
        }
        return null;
    }
    static JSONObject session(File directory)throws Exception {
        JSONObject saved=checkpoint(directory,"game-start-checkpoint.json");
        if(saved==null)return null;
        String run=saved.optString("run_id");
        if(!run.matches("[0-9a-f-]{36}"))return null;

        File log=new File(directory,"game-sdk.log");
        if(log.isFile())try(RandomAccessFile input=new RandomAccessFile(log,"r")) {
            long size=input.length();input.seek(Math.max(0,size-65536));
            byte[] bytes=new byte[(int)Math.min(size,65536)];input.readFully(bytes);
            saved.put("sdk_log_tail",new String(bytes,StandardCharsets.UTF_8)).put("sdk_log_bytes",size);
        }catch(IOException ignored){}
        JSONObject stop=checkpoint(directory,"game-user-stop.json");
        if(stop!=null&&run.equals(stop.optString("run_id")))saved.put("user_stop_requested",stop.optBoolean("requested"));
        JSONObject fault=checkpoint(directory,"game-native-fault.json");
        if(fault!=null&&run.equals(fault.optString("run_id"))) {
            saved.put("native_memory_exception",fault);
            JSONObject modules=checkpoint(directory,"game-native-modules.json");
            if(modules!=null&&run.equals(modules.optString("run_id")))saved.put("native_modules",modules);
        }
        saved.put("performance_capture_loaded_on_export",true);
        saved.put("evidence","Saved checkpoint and log; game execution evidence retains its original version");
        return saved;
    }
    static JSONObject recovery(File directory,String version)throws Exception {
        JSONObject root=new JSONObject().put("schema",1).put("app_version",version)
            .put("recovery_without_native_initialization",true);
        JSONObject session=session(directory);
        if(session!=null)root.put("game_session",session).put("game_session_is_previous_version",!version.equals(session.optString("app_version")));
        else root.put("game_session_unavailable",true);
        JSONObject nativeCheckpoint=checkpoint(directory,"native-checkpoint.json");
        if(nativeCheckpoint!=null)root.put("previous_native_checkpoint",nativeCheckpoint);
        return root;
    }

    static void writeJson(OutputStream output,JSONObject snapshot,File directory)throws Exception {
        JSONObject root=new JSONObject(snapshot.toString());
        JSONObject session=root.optJSONObject("game_session");root.remove("game_session");
        String metadata=root.toString();
        Writer writer=new BufferedWriter(new OutputStreamWriter(output,StandardCharsets.UTF_8));
        if(session==null){writer.write(metadata);writer.flush();return;}
        session.remove("performance_capture");
        String saved=session.toString();
        writer.write(metadata,0,metadata.length()-1);if(root.length()>0)writer.write(',');
        writer.write("\"game_session\":");writer.write(saved,0,saved.length()-1);
        if(session.length()>0)writer.write(',');writer.write("\"performance_capture\":");
        ProcessResourceSampler.writeCapture(writer,directory,session.getString("run_id"));
        writer.write("}}");writer.flush();
    }

    static void writeBackup(OutputStream output,File directory)throws Exception {
        String[] names={"game-start-checkpoint.json","game-user-stop.json","game-native-fault.json",
            "game-native-modules.json","native-checkpoint.json","sdk-interrupted-checkpoint.json",
            "game-performance.previous.jsonl","game-performance.jsonl","game-sdk.log"};
        JSONArray entries=new JSONArray();
        try(ZipOutputStream zip=new ZipOutputStream(output)) {
            for(String name:names)for(String suffix:new String[]{"",".bak"}) {
                File file=new File(directory,name+suffix);if(!file.isFile())continue;
                long limit=name.endsWith(".jsonl")?ProcessResourceSampler.MAX_PART_BYTES:
                    name.endsWith(".log")?524288:CHECKPOINT_LIMIT;
                try(RandomAccessFile input=new RandomAccessFile(file,"r")) {
                    long length=input.length(),count=Math.min(length,limit),start=Math.max(0,length-count);
                    input.seek(start);zip.putNextEntry(new ZipEntry(name+suffix));byte[] buffer=new byte[16384];long remaining=count;
                    while(remaining>0){int n=input.read(buffer,0,(int)Math.min(buffer.length,remaining));if(n<0)break;zip.write(buffer,0,n);remaining-=n;}
                    zip.closeEntry();entries.put(new JSONObject().put("file",name+suffix).put("source_bytes",length)
                        .put("copied_bytes",count-remaining).put("source_offset",start));
                }
            }
            zip.putNextEntry(new ZipEntry("recovery-manifest.json"));
            zip.write(new JSONObject().put("schema",1).put("kind","bo2-report-recovery")
                .put("sources_modified",false).put("files",entries).toString(2).getBytes(StandardCharsets.UTF_8));zip.closeEntry();
        }
    }
}
