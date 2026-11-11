// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import org.json.JSONArray;
import org.json.JSONObject;
import org.json.JSONTokener;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;


final class ProcessResourceSampler {
    static final int MAX_THREADS=64, MAX_SAMPLES=240;
    static final long MAX_PART_BYTES=4L*1024*1024;
    private final File proc,cpu;
    private final long hz;
    private final int cores;
    private Stat previousProcess;
    private final Map<Long,Stat> previousThreads=new HashMap<>();
    static final class Stat {
        long tid,start,ticks,timeMs,queueNs=-1;
        int lastCpu;
        String name,state;
    }
    ProcessResourceSampler(File proc,File cpu,long hz,int cores) {
        this.proc=proc;this.cpu=cpu;this.hz=hz;this.cores=cores;
    }
    static String read(File path,int limit) {
        try(InputStream in=new FileInputStream(path)) {
            byte[] bytes=new byte[limit+1];int length=0,n;
            while(length<bytes.length&&(n=in.read(bytes,length,bytes.length-length))>0)length+=n;
            return length>limit?null:new String(bytes,0,length,StandardCharsets.UTF_8).trim();
        }catch(IOException|SecurityException error){return null;}
    }
    static Stat parseStat(String text,long timeMs) {
        if(text==null)return null;
        try {
            int first=text.indexOf('('),last=text.lastIndexOf(')');
            if(first<1||last<=first)return null;
            String[] f=text.substring(last+1).trim().split("\\s+");
            if(f.length<37)return null;
            Stat s=new Stat();s.tid=Long.parseLong(text.substring(0,first).trim());
            s.name=text.substring(first+1,last);s.state=f[0];
            long user=Long.parseLong(f[11]),system=Long.parseLong(f[12]);
            s.ticks=Math.addExact(user,system);s.start=Long.parseLong(f[19]);
            s.lastCpu=Integer.parseInt(f[36]);s.timeMs=timeMs;
            if(user<0||system<0||s.start<0)return null;
            return s;
        }catch(RuntimeException error){return null;}
    }
    private Object percent(Stat now,Stat old) {
        if(now==null||old==null||hz<=0||now.start!=old.start||now.timeMs<=old.timeMs||now.ticks<old.ticks)return JSONObject.NULL;
        return (now.ticks-old.ticks)*100000.0/hz/(now.timeMs-old.timeMs);
    }
    static String field(String status,String key) {
        if(status!=null)for(String line:status.split("\n"))if(line.startsWith(key+":"))return line.substring(key.length()+1).trim();
        return null;
    }
    static Object kib(String status,String key) {
        try {String value=field(status,key);if(value==null)return JSONObject.NULL;
            String[] p=value.split("\\s+");if(p.length!=2||!p[1].equals("kB"))return JSONObject.NULL;
            long n=Long.parseLong(p[0]);return n>=0?n:JSONObject.NULL;
        }catch(RuntimeException error){return JSONObject.NULL;}
    }
    static Object number(File path) {
        try{String s=read(path,128);if(s==null)return JSONObject.NULL;long n=Long.parseLong(s);return n>=0?n:JSONObject.NULL;}
        catch(RuntimeException error){return JSONObject.NULL;}
    }
    JSONObject sample(long nowMs)throws Exception {
        JSONObject out=new JSONObject().put("elapsed_ms",nowMs).put("cpu_percent_basis","100 = one full logical CPU")
            .put("clock_ticks_per_second",hz>0?hz:JSONObject.NULL).put("logical_cpus_reported",cores);
        Stat process=parseStat(read(new File(proc,"self/stat"),8192),nowMs);
        Object usage=percent(process,previousProcess);
        out.put("process_cpu_percent_one_core",usage);
        out.put("process_cpu_interval_ms",usage instanceof Number?nowMs-previousProcess.timeMs:JSONObject.NULL);
        out.put("process_cpu_percent_reported_capacity",usage instanceof Number&&cores>0?((Number)usage).doubleValue()/cores:JSONObject.NULL);
        if(process!=null)previousProcess=process;
        String status=read(new File(proc,"self/status"),32768);
        out.put("rss_kib",kib(status,"VmRSS")).put("virtual_size_kib",kib(status,"VmSize"))
            .put("cpus_allowed_list",nullable(field(status,"Cpus_allowed_list")));
        File[] tasks=new File(proc,"self/task").listFiles();
        List<JSONObject> rows=new ArrayList<>();Map<Long,Stat> current=new HashMap<>();
        if(tasks!=null)for(File task:tasks) {
            Stat s=parseStat(read(new File(task,"stat"),8192),nowMs);if(s==null)continue;
            Stat old=previousThreads.get(s.tid);
            JSONObject row=new JSONObject().put("tid",s.tid).put("start_time_ticks",s.start).put("name",s.name).put("sampled_state",s.state)
                .put("last_cpu",s.lastCpu).put("cpu_percent_one_core",percent(s,old));
            row.put("cpu_interval_ms",row.isNull("cpu_percent_one_core")?JSONObject.NULL:nowMs-old.timeMs);
            String sched=read(new File(task,"schedstat"),256);
            try{if(sched!=null)s.queueNs=Long.parseLong(sched.split("\\s+")[1]);}catch(RuntimeException ignored){}
            row.put("runqueue_wait_ms",old!=null&&s.start==old.start&&old.queueNs>=0&&s.queueNs>=old.queueNs?
                (s.queueNs-old.queueNs)/1e6:JSONObject.NULL);
            current.put(s.tid,s);rows.add(row);
        }
        if(tasks!=null){previousThreads.clear();previousThreads.putAll(current);}
        rows.sort((a,b)->{int c=Double.compare(b.optDouble("cpu_percent_one_core",-1),a.optDouble("cpu_percent_one_core",-1));
            return c!=0?c:Long.compare(a.optLong("tid"),b.optLong("tid"));});
        JSONArray threads=new JSONArray();for(int i=0;i<Math.min(MAX_THREADS,rows.size());i++)threads.put(rows.get(i));
        out.put("threads_available",tasks!=null).put("threads_observed",rows.size()).put("threads_truncated",rows.size()>MAX_THREADS).put("threads",threads);
        JSONArray frequencies=new JSONArray();
        for(int i=0;i<Math.min(cores,64);i++)frequencies.put(new JSONObject().put("cpu",i)
            .put("frequency_khz",number(new File(cpu,"cpu"+i+"/cpufreq/scaling_cur_freq")))
            .put("maximum_khz",number(new File(cpu,"cpu"+i+"/cpufreq/cpuinfo_max_freq"))));
        return out.put("cpu_frequency_snapshots",frequencies);
    }
    static Object nullable(Object value){return value==null?JSONObject.NULL:value;}
    static void append(File directory,JSONObject sample)throws Exception {
        byte[] bytes=(sample.toString()+"\n").getBytes(StandardCharsets.UTF_8);
        if(bytes.length>262144)throw new IOException("Performance sample exceeds limit");
        File file=new File(directory,"game-performance.jsonl"),old=new File(directory,"game-performance.previous.jsonl");
        if(file.length()+bytes.length+1>MAX_PART_BYTES) {
            if(old.exists()&&!old.delete())throw new IOException("Cannot retire performance part");
            if(file.exists()&&!file.renameTo(old))throw new IOException("Cannot rotate performance capture");
        }
        boolean separate=false;
        if(file.length()>0)try(RandomAccessFile input=new RandomAccessFile(file,"r")) {
            input.seek(input.length()-1);separate=input.read()!='\n';
        }
        try(FileOutputStream stream=new FileOutputStream(file,true)){if(separate)stream.write('\n');stream.write(bytes);}
    }
    static JSONObject capture(File directory,String run)throws Exception {
        Capture capture=collect(directory,run);JSONArray samples=new JSONArray();
        for(String line:capture.lines)samples.put(new JSONObject(line));
        return capture.metadata(run).put("samples",samples);
    }
    private static final class Capture {
        final ArrayDeque<String> lines=new ArrayDeque<>();
        int malformed,otherRuns,omitted;
        JSONObject metadata(String run)throws Exception {
            return new JSONObject().put("schema",1).put("run_id",run).put("sample_period_seconds",5)
                .put("malformed_lines_skipped",malformed).put("other_sessions_skipped",otherRuns)
                .put("older_samples_or_oversized_parts_omitted",omitted).put("physical_display_fps_measured",false)
                .put("notes","Thread state and CPU frequency are snapshots; wall-time counters may overlap; missing counters are null, not zero.");
        }
    }
    private static Capture collect(File directory,String run)throws Exception {
        Capture result=new Capture();
        for(String name:new String[]{"game-performance.previous.jsonl","game-performance.jsonl"}) {
            File file=new File(directory,name);if(!file.isFile())continue;
            if(file.length()>MAX_PART_BYTES){result.omitted++;continue;}
            try(BufferedReader in=new BufferedReader(new InputStreamReader(new FileInputStream(file),StandardCharsets.UTF_8))) {
                String line;while((line=in.readLine())!=null)try {
                    if(line.getBytes(StandardCharsets.UTF_8).length>262144){result.omitted++;continue;}
                    JSONTokener input=new JSONTokener(line);JSONObject item=new JSONObject(input);
                    if(input.nextClean()!=0){result.malformed++;continue;}
                    if(!run.equals(item.optString("run_id"))){result.otherRuns++;continue;}
                    result.lines.add(item.toString());if(result.lines.size()>MAX_SAMPLES){result.lines.removeFirst();result.omitted++;}
                }catch(Exception error){result.malformed++;}
            }
        }
        return result;
    }

    static void writeCapture(Writer writer,File directory,String run)throws Exception {
        Capture capture=collect(directory,run);String metadata=capture.metadata(run).toString();
        writer.write(metadata,0,metadata.length()-1);writer.write(",\"samples\":[");boolean comma=false;
        for(String line:capture.lines){if(comma)writer.write(',');writer.write(line);comma=true;}
        writer.write("]}");
    }
}
