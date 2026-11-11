// SPDX-License-Identifier: GPL-3.0-only
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HexFormat;
import org.bo2port.diagnostic.SvodReader;

class test_svod_reader {
    static int passed;
    static final long BPF = 82824, FULL = 0xA290000;
    static void require(boolean value, String text) { if (!value) throw new AssertionError(text); }
    static void le(byte[] data, int at, int value) { ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).putInt(at, value); }
    static void be(byte[] data, int at, int value) { ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN).putInt(at, value); }
    static final class Segment {
        int file; long offset; byte[] data;
        Segment(int f, long o, byte[] d) { file=f; offset=o; data=Arrays.copyOf(d,d.length); }
    }
    static final class Sparse implements SvodReader.Fragments {
        ArrayList<Segment> parts = new ArrayList<>();
        void write(int file, long offset, byte[] data) { parts.add(new Segment(file,offset,data)); }
        public int count() { return 2; }
        public byte[] read(int file, long offset, int bytes) throws IOException {
            if(file<0 || file>=2 || offset<0 || offset+bytes>FULL) throw new IOException("fixture out of range");
            byte[] out=new byte[bytes];
            for(Segment p:parts) if(p.file==file) {
                long begin=Math.max(offset,p.offset),end=Math.min(offset+bytes,p.offset+p.data.length);
                if(end>begin) System.arraycopy(p.data,(int)(begin-p.offset),out,(int)(begin-offset),(int)(end-begin));
            }
            return out;
        }
    }
    static long[] location(long guest, int start, boolean enhanced) {
        long real=guest-start*2L+(enhanced?2:0), within=real%BPF;
        return new long[]{real/BPF,0x2000+within*2048+(within/408)*4096};
    }
    static void virtual(Sparse input,long block,int start,boolean enhanced,byte[] data) {
        for(int at=0;at<data.length;at+=2048) {
            long[] p=location(block+at/2048,start,enhanced);
            input.write((int)p[0],p[1],Arrays.copyOfRange(data,at,Math.min(data.length,at+2048)));
        }
    }
    static byte[] header(boolean enhanced,int start) {
        byte[] h=new byte[0x6000];
        System.arraycopy("LIVE".getBytes(StandardCharsets.US_ASCII),0,h,0,4);
        be(h,0x344,0x7000);be(h,0x360,0x415608C3);be(h,0x39D,2);be(h,0x3A9,1);
        h[0x379]=0x24;h[0x391]=(byte)(enhanced?0x40:0);
        h[0x395]=(byte)start;h[0x396]=(byte)(start>>8);h[0x397]=(byte)(start>>16);
        return h;
    }
    static byte[] payload() {
        byte[] p=new byte[9000];
        for(int i=0;i<p.length;++i)p[i]=(byte)(i*17+9);
        System.arraycopy("XEX2".getBytes(StandardCharsets.US_ASCII),0,p,0,4);
        be(p,20,1);be(p,24,0x00040006);be(p,28,128);be(p,140,0x415608C3);
        return p;
    }
    static Sparse fixture(boolean enhanced,int start,long root,long executable,byte[] payload) {
        return fixture(enhanced,start,root,executable,payload,"default_mp.xex");
    }
    static Sparse fixture(boolean enhanced,int start,long root,long executable,byte[] payload,String fileName) {
        Sparse input=new Sparse();
        byte[] media=new byte[36];
        System.arraycopy("MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII),0,media,0,20);
        le(media,20,(int)root);le(media,24,4096);
        input.write(0,enhanced?0x2000:0x12000,media);
        byte[] dir=new byte[4096];
        dir[2]=(byte)508;dir[3]=(byte)(508>>8);
        dir[13]=5;System.arraycopy("other".getBytes(StandardCharsets.US_ASCII),0,dir,14,5);
        int at=508*4;
        le(dir,at+4,(int)executable);le(dir,at+8,payload.length);
        byte[] name=fileName.getBytes(StandardCharsets.US_ASCII);
        dir[at+13]=(byte)name.length;System.arraycopy(name,0,dir,at+14,name.length);
        virtual(input,root,start,enhanced,dir);
        virtual(input,executable,start,enhanced,payload);
        return input;
    }
    static void expectError(Throwing test) throws Exception {
        try{test.run();throw new AssertionError("Expected bounded read failure");}
        catch(IOException expected){++passed;}
    }
    interface Throwing { void run() throws Exception; }
    public static void main(String[] args) throws Exception {
        for(boolean enhanced:new boolean[]{true,false})for(int start:new int[]{0,5}) {
            long root=2L*start, exe=404+2L*start;
            byte[] payload=payload();
            Sparse input=fixture(enhanced,start,root,exe,payload);
            SvodReader reader=new SvodReader(header(enhanced,start),input);
            ByteArrayOutputStream out=new ByteArrayOutputStream();
            SvodReader.Result result=reader.copyExecutable(reader.findExecutable(),out);
            require(Arrays.equals(payload,out.toByteArray()),"skip L0 hash pages without changing payload");
            require(result.sha256.equals(HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(payload))),"hash exact copied bytes");
            ++passed;
        }
        byte[] h=header(true,0),payload=payload();
        Sparse input=fixture(true,0,405,BPF-4,payload);
        SvodReader reader=new SvodReader(h,input);
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        reader.copyExecutable(reader.findExecutable(),out);
        require(Arrays.equals(payload,out.toByteArray()),"directory/name/hash/fragment boundary reads");++passed;
        require(reader.address(BPF-2).fragment==1&&reader.address(BPF-2).offset==8192,"next fragment begins after two hash pages");++passed;
        expectError(()->reader.address(BPF*2));
        Sparse bad=fixture(true,0,0,404,payload);
        bad.write(0,0x2000,new byte[20]);
        expectError(()->new SvodReader(h,bad));
        Sparse cyclic=fixture(true,0,0,404,payload);
        byte[] loop=new byte[2];loop[0]=(byte)508;loop[1]=(byte)(508>>8);
        long[] pos=location(0,0,true);cyclic.write(0,pos[1]+508*4+2,loop);
        expectError(()->new SvodReader(h,cyclic).findExecutable());
        byte[] wrong=Arrays.copyOf(h,h.length);be(wrong,0x360,0x12345678);
        expectError(()->new SvodReader(wrong,input));
        byte[] notXex=new byte[9000];
        Sparse invalid=fixture(true,0,0,404,notXex);
        expectError(()->{SvodReader r=new SvodReader(h,invalid);r.copyExecutable(r.findExecutable(),new ByteArrayOutputStream());});

        Sparse campaign=fixture(true,0,0,404,payload,"default.xex");
        SvodReader campaignReader=new SvodReader(h,campaign);
        ByteArrayOutputStream campaignOut=new ByteArrayOutputStream();
        SvodReader.Executable campaignExe=campaignReader.findExecutable("default.xex");
        require(campaignExe.name.equals("default.xex"),"campaign executable name");
        campaignReader.copyExecutable(campaignExe,campaignOut);
        require(Arrays.equals(payload,campaignOut.toByteArray()),"campaign executable bytes");++passed;
        expectError(()->campaignReader.findExecutable());
        expectError(()->reader.findExecutable("default.xex"));
        expectError(()->reader.findExecutable("other"));
        System.out.println("SVOD reader: "+passed+" synthetic layout, boundary and negative cases passed");
    }
}
