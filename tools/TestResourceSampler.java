package org.bo2port.diagnostic;
import java.io.*;
import java.nio.file.*;
import org.json.*;
public class TestResourceSampler {
 static int checks;
 static void check(boolean b,String message){checks++;if(!b)throw new AssertionError(message);}
 static void write(File f,String value)throws Exception{f.getParentFile().mkdirs();Files.writeString(f.toPath(),value);}
 static String stat(long tid,String name,long ticks,long start,int cpu){String[] f=new String[37];java.util.Arrays.fill(f,"0");f[0]="R";f[11]=""+ticks;f[19]=""+start;f[36]=""+cpu;return tid+" ("+name+") "+String.join(" ",f);}
 public static void main(String[] args)throws Exception {
  File root=Files.createTempDirectory("bo2-resource-test").toFile(),proc=new File(root,"proc"),cpu=new File(root,"cpu"),capture=new File(root,"capture");capture.mkdirs();
  write(new File(proc,"self/stat"),stat(100,"game",100,900,4));
  write(new File(proc,"self/task/7/stat"),stat(7,"render (worker)",100,1000,6));
  write(new File(proc,"self/task/7/schedstat"),"100 50000000 1");
  write(new File(proc,"self/status"),"VmRSS:\t2048 kB\nVmSize:\t5000000 kB\nCpus_allowed_list:\t0-7\n");
  write(new File(cpu,"cpu6/cpufreq/scaling_cur_freq"),"2200000\n");
  ProcessResourceSampler s=new ProcessResourceSampler(proc,cpu,100,8);
  JSONObject first=s.sample(1000);check(first.isNull("process_cpu_percent_one_core"),"first sample has no CPU baseline");
  check(first.getJSONArray("threads").getJSONObject(0).getString("name").equals("render (worker)"),"stat parses nested parentheses");
  check(first.getLong("rss_kib")==2048,"RSS parsed in KiB");check(first.getLong("virtual_size_kib")==5000000,"virtual reservation separate from RSS");
  check(first.getJSONArray("cpu_frequency_snapshots").getJSONObject(0).isNull("frequency_khz"),"denied/missing frequency is null");
  check(first.getJSONArray("cpu_frequency_snapshots").getJSONObject(6).getLong("frequency_khz")==2200000,"frequency unit");
  write(new File(proc,"self/stat"),stat(100,"game",600,900,4));write(new File(proc,"self/task/7/stat"),stat(7,"render (worker)",600,1000,6));
  write(new File(proc,"self/task/7/schedstat"),"200 250000000 2");
  JSONObject next=s.sample(6000),thread=next.getJSONArray("threads").getJSONObject(0);
  check(next.getDouble("process_cpu_percent_one_core")==100.0,"one saturated core is 100%");
  check(next.getDouble("process_cpu_percent_reported_capacity")==12.5,"8-core normalization is explicit");
  check(thread.getDouble("cpu_percent_one_core")==100.0,"per-thread CPU delta");
  check(thread.getDouble("runqueue_wait_ms")==200,"runqueue wait delta, not sleeping time");
  check(thread.getInt("last_cpu")==6,"processor is a snapshot");
  write(new File(proc,"self/task/7/stat"),stat(7,"reused tid",1,2000,0));
  check(s.sample(11000).getJSONArray("threads").getJSONObject(0).isNull("cpu_percent_one_core"),"TID reuse cannot inherit CPU counters");
  check(ProcessResourceSampler.parseStat("malformed",10)==null,"malformed stat rejected");
  check(ProcessResourceSampler.parseStat(stat(1,"negative",-1,5,0),10)==null,"negative ticks rejected");
  check(ProcessResourceSampler.kib("VmRSS: 99 bytes","VmRSS")==JSONObject.NULL,"unknown units rejected");
  ProcessResourceSampler noHz=new ProcessResourceSampler(proc,cpu,-1,8);noHz.sample(0);
  check(noHz.sample(1000).isNull("process_cpu_percent_one_core"),"no fabricated clock frequency");
  JSONObject missing=new ProcessResourceSampler(new File(root,"absent"),cpu,100,8).sample(1000);
  check(!missing.getBoolean("threads_available")&&missing.isNull("rss_kib"),"unavailable proc is explicit");
  for(int i=20;i<90;i++)write(new File(proc,"self/task/"+i+"/stat"),stat(i,"worker",1,2000,i%8));
  JSONObject many=s.sample(16000);check(many.getBoolean("threads_truncated"),"thread cap identified");check(many.getJSONArray("threads").length()==64,"thread export bounded");
  for(int i=0;i<241;i++)ProcessResourceSampler.append(capture,new JSONObject().put("run_id","current").put("seq",i));
  ProcessResourceSampler.append(capture,new JSONObject().put("run_id","previous"));
  try(FileOutputStream f=new FileOutputStream(new File(capture,"game-performance.jsonl"),true)){f.write("{partial".getBytes());}
  JSONObject exported=ProcessResourceSampler.capture(capture,"current");
  check(exported.getJSONArray("samples").length()==240,"bounded capture");
  check(exported.getJSONArray("samples").getJSONObject(0).getInt("seq")==1,"keeps latest records");
  check(exported.getInt("other_sessions_skipped")==1,"cross-session samples excluded");
  check(exported.getInt("malformed_lines_skipped")==1,"partial final record tolerated");
  ProcessResourceSampler.append(capture,new JSONObject().put("run_id","after-crash"));
  check(ProcessResourceSampler.capture(capture,"after-crash").getJSONArray("samples").length()==1,"append recovers after partial record");
  check(!exported.getBoolean("physical_display_fps_measured"),"does not claim display FPS");
  File rotation=new File(root,"rotation");rotation.mkdirs();
  try(RandomAccessFile f=new RandomAccessFile(new File(rotation,"game-performance.jsonl"),"rw")){f.setLength(ProcessResourceSampler.MAX_PART_BYTES-2);}
  ProcessResourceSampler.append(rotation,new JSONObject().put("run_id","a"));
  check(new File(rotation,"game-performance.previous.jsonl").length()==ProcessResourceSampler.MAX_PART_BYTES-2,"rotation retains prior part");
  check(new File(rotation,"game-performance.jsonl").length()<100,"rotation starts bounded fresh file");
  long before=new File(rotation,"game-performance.jsonl").length();boolean rejected=false;
  try{ProcessResourceSampler.append(rotation,new JSONObject().put("run_id","a").put("large","x".repeat(300000)));}catch(IOException expected){rejected=true;}
  check(rejected&&new File(rotation,"game-performance.jsonl").length()==before,"oversized sample preserves capture");

  JSONObject live=new ProcessResourceSampler(new File("/proc"),new File("/sys/devices/system/cpu"),100,Runtime.getRuntime().availableProcessors()).sample(1);
  check(live.getBoolean("threads_available")&&live.getInt("threads_observed")>0,"live own-process read");
  System.out.println(checks+" checks passed: CPU normalization, TID reuse, unavailable metrics, capture bounds, rotation, session isolation and live proc smoke");
 }
}
