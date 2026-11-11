// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;
import org.json.*;

public final class TestGameReportRecovery {
    static int checks;
    static final String RUN="12345678-abcd-abcd-abcd-123456789012";
    static void check(boolean condition,String message){checks++;if(!condition)throw new AssertionError(message);}
    static void write(File root,String name,String value)throws Exception{Files.writeString(new File(root,name).toPath(),value);}
    static Map<String,String> hashes(File root)throws Exception {
        Map<String,String> result=new TreeMap<>();
        for(File f:Objects.requireNonNull(root.listFiles()))if(f.isFile())result.put(f.getName(),Base64.getEncoder().encodeToString(MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(f.toPath()))));
        return result;
    }
    public static void main(String[] args)throws Exception {
        File files=Files.createTempDirectory("bo2-report-recovery-").toFile();
        JSONObject saved=new JSONObject().put("run_id",RUN).put("app_version","0.23-resource-profile")
            .put("game_entry_executed",true).put("status","running").put("loaded_map","zm_transit");
        write(files,"game-start-checkpoint.json",saved.toString());
        write(files,"game-user-stop.json",new JSONObject().put("run_id",RUN).put("requested",true).toString());
        write(files,"game-native-fault.json",new JSONObject().put("run_id","different").put("fault","stale").toString());
        write(files,"game-sdk.log","log start\n"+"x".repeat(70000)+"\nlast recorded line");
        JSONArray threads=new JSONArray();
        for(int i=0;i<64;i++)threads.put(new JSONObject().put("tid",1000+i).put("name","worker-"+i)
            .put("start_time_ticks",42).put("cpu_percent_one_core",i/2.0).put("last_cpu",i%8).put("runqueue_wait_ms",i));
        for(int i=0;i<245;i++)ProcessResourceSampler.append(files,new JSONObject().put("run_id",RUN)
            .put("seq",i).put("threads",threads).put("padding","y".repeat(12288)));
        ProcessResourceSampler.append(files,new JSONObject().put("run_id","old-session").put("seq",999));
        try(FileOutputStream out=new FileOutputStream(new File(files,"game-performance.jsonl"),true)) {
            out.write((new JSONObject().put("run_id",RUN).toString()+" trailing-garbage\n{partial").getBytes(StandardCharsets.UTF_8));
        }
        check(new File(files,"game-performance.previous.jsonl").isFile(),"fixture exercises both capture parts");
        Map<String,String> before=hashes(files);
        JSONObject report=GameReportFiles.recovery(files,"0.24-report-recovery");
        JSONObject session=report.getJSONObject("game_session");
        check(session.getString("app_version").equals("0.23-resource-profile"),"old capture retains its version");
        check(report.getBoolean("game_session_is_previous_version"),"old session is explicitly labelled");
        check(session.getBoolean("user_stop_requested"),"user exit is retained");
        check(!session.has("native_memory_exception"),"fault from a different run is excluded");
        check(!session.has("performance_capture"),"screen metadata never materializes samples");
        check(session.getString("sdk_log_tail").endsWith("last recorded line"),"log tail includes final record");
        check(session.getString("sdk_log_tail").length()<=65536,"log preview is bounded");
        ByteArrayOutputStream bytes=new ByteArrayOutputStream();GameReportFiles.writeJson(bytes,report,files);
        JSONObject exported=new JSONObject(bytes.toString(StandardCharsets.UTF_8));
        JSONObject capture=exported.getJSONObject("game_session").getJSONObject("performance_capture");
        check(capture.getJSONArray("samples").length()==240,"latest 240 complete records exported");
        check(capture.getJSONArray("samples").getJSONObject(0).getInt("seq")==5,"oldest excess records omitted");
        check(capture.getJSONArray("samples").getJSONObject(239).getInt("seq")==244,"last complete sample survives interrupted exit");
        check(capture.getInt("other_sessions_skipped")==1,"old sessions excluded");
        check(capture.getInt("malformed_lines_skipped")==2,"trailing garbage and partial line excluded");
        check(!report.getJSONObject("game_session").has("performance_capture"),"export does not mutate UI snapshot");
        check(hashes(files).equals(before),"JSON recovery preserves all input evidence byte for byte");
        bytes.reset();GameReportFiles.writeBackup(bytes,files);
        Map<String,byte[]> recovered=new HashMap<>();
        try(ZipInputStream zip=new ZipInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            ZipEntry entry;while((entry=zip.getNextEntry())!=null)recovered.put(entry.getName(),zip.readAllBytes());
        }
        check(Arrays.equals(recovered.get("game-performance.jsonl"),Files.readAllBytes(new File(files,"game-performance.jsonl").toPath())),"raw backup retains partial line too");
        check(recovered.containsKey("game-performance.previous.jsonl"),"raw backup includes both parts");
        check(!new JSONObject(new String(recovered.get("recovery-manifest.json"),StandardCharsets.UTF_8)).getBoolean("sources_modified"),"backup declares source preservation");
        check(hashes(files).equals(before),"ZIP recovery preserves original files");
        write(files,"game-start-checkpoint.json.bak",saved.toString());write(files,"game-start-checkpoint.json","{partial");
        check(GameReportFiles.session(files).getString("run_id").equals(RUN),"valid backup checkpoint recovers interrupted metadata");
        write(files,"game-start-checkpoint.json.bak","{also partial");
        JSONObject orphan=GameReportFiles.recovery(files,"0.24-report-recovery");
        check(orphan.getBoolean("game_session_unavailable"),"missing session identity remains explicit");
        bytes.reset();GameReportFiles.writeJson(bytes,orphan,files);
        check(new JSONObject(bytes.toString(StandardCharsets.UTF_8)).getBoolean("game_session_unavailable"),"metadata-only recovery remains valid JSON");
        bytes.reset();GameReportFiles.writeBackup(bytes,files);
        check(bytes.size()>0,"raw recovery works with both checkpoints corrupt");
        System.out.println(checks+" recovery checks passed: legacy session, 245x64-thread fixture, interrupted records, streamed JSON, immutable inputs and raw ZIP fallback");
    }
}
