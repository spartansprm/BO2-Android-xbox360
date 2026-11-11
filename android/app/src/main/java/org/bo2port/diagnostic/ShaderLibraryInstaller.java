// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import org.json.JSONArray;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.HashSet;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;


final class ShaderLibraryInstaller {
    static final String MANIFEST="biblioteca-shaders.json";
    static final long PACK_MAX=2L*1024*1024*1024, INDEX_MAX=8L*1024*1024;
    static final long CHUNK_MAX=96L*1024*1024, ZIP_MAX=100L*1024*1024;
    interface Source {InputStream open(String name)throws Exception;}
    interface Progress {void update(long done,long total)throws Exception;}
    private ShaderLibraryInstaller(){}

    static JSONObject readJson(InputStream input,int maximum)throws Exception {
        try(InputStream stream=input;ByteArrayOutputStream data=new ByteArrayOutputStream()) {
            byte[] block=new byte[8192];
            for(int n;(n=stream.read(block))!=-1;){if(data.size()+n>maximum)throw new IOException("Manifiesto demasiado grande");data.write(block,0,n);}
            return new JSONObject(data.toString(StandardCharsets.UTF_8.name()));
        }
    }
    static String hex(byte[] bytes){StringBuilder out=new StringBuilder();for(byte b:bytes)out.append(String.format(java.util.Locale.ROOT,"%02x",b&255));return out.toString();}
    static String hash(JSONObject row)throws Exception {
        String value=row.getString("sha256");if(!value.matches("[0-9a-f]{64}"))throw new IOException("Hash de biblioteca inválido");return value;
    }
    static void remove(File path)throws IOException {
        File[] children=path.listFiles();if(children!=null)for(File child:children)remove(child);
        if(path.exists()&&!path.delete())throw new IOException("No se pudo limpiar "+path.getName());
    }
    static JSONArray validateFiles(JSONObject manifest)throws Exception {
        JSONArray files=manifest.getJSONArray("files");
        if(manifest.getInt("schema")!=1||!manifest.getBoolean("private_game_resources")||
           !"ubo-only".equals(manifest.getString("profile"))||manifest.getInt("shaders")<1||
           manifest.getInt("shaders")>65536||files.length()!=2)throw new IOException("Biblioteca Android incompatible");
        HashSet<String> names=new HashSet<>();
        for(int i=0;i<files.length();i++){
            JSONObject row=files.getJSONObject(i);String name=row.getString("file");
            long maximum=name.equals("bo2zm_shaders.spvpack")?PACK_MAX:INDEX_MAX;
            if(!(name.equals("bo2zm_shaders.spvpack")||name.equals("bo2zm_shaders.spvidx"))||!names.add(name)||
               row.getLong("bytes")<24||row.getLong("bytes")>maximum)throw new IOException("Archivo de biblioteca inválido");
            hash(row);
        }
        return files;
    }
    static boolean matches(File file,JSONObject row)throws Exception {
        if(!file.isFile()||file.length()!=row.getLong("bytes"))return false;
        MessageDigest digest=MessageDigest.getInstance("SHA-256");byte[] block=new byte[128*1024];
        try(InputStream stream=new FileInputStream(file)){for(int n;(n=stream.read(block))!=-1;)digest.update(block,0,n);}
        return hex(digest.digest()).equals(hash(row));
    }
    static void recover(File root)throws Exception {
        File active=new File(root,"game-shaders"),previous=new File(root,"game-shaders.previous");
        if(!active.exists()&&previous.exists())Files.move(previous.toPath(),active.toPath(),StandardCopyOption.ATOMIC_MOVE);
    }
    static void validatePair(File directory,int shaders)throws Exception {
        File pack=new File(directory,"bo2zm_shaders.spvpack"),index=new File(directory,"bo2zm_shaders.spvidx");
        byte[] header=new byte[24],lookup=new byte[80];
        try(DataInputStream input=new DataInputStream(new FileInputStream(pack))){input.readFully(header);}
        try(DataInputStream input=new DataInputStream(new FileInputStream(index))){input.readFully(lookup);}
        java.nio.ByteBuffer p=java.nio.ByteBuffer.wrap(header).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        java.nio.ByteBuffer i=java.nio.ByteBuffer.wrap(lookup).order(java.nio.ByteOrder.LITTLE_ENDIAN);
        if(!java.util.Arrays.equals(java.util.Arrays.copyOf(header,8),new byte[]{'B','Z','M','S','P','V',0,0})||p.getInt(8)!=1||p.getInt(12)!=shaders||
           !java.util.Arrays.equals(java.util.Arrays.copyOf(lookup,8),new byte[]{'B','Z','M','S','I','D','X',0})||i.getInt(8)!=1||i.getInt(12)!=shaders||
           i.getLong(16)!=pack.length()||i.getLong(24)!=p.getLong(16)||i.getInt(32)<0||index.length()!=80L+72L*shaders+2L*i.getInt(32))
            throw new IOException("El índice no corresponde a la biblioteca");
    }
    static final class CheckedStream extends FilterInputStream {
        long count;final MessageDigest digest=MessageDigest.getInstance("SHA-256");
        CheckedStream(InputStream stream)throws Exception{super(stream);}
        @Override public int read()throws IOException{int b=in.read();if(b!=-1){count++;digest.update((byte)b);check();}return b;}
        @Override public int read(byte[] b,int off,int len)throws IOException{int n=in.read(b,off,len);if(n>0){count+=n;digest.update(b,off,n);check();}return n;}
        void check()throws IOException{if(count>ZIP_MAX)throw new IOException("Lote de biblioteca demasiado grande");}
    }
    static JSONObject install(Source source,File root,Progress progress)throws Exception {
        JSONObject manifest=readJson(source.open(MANIFEST),256*1024);JSONArray files=validateFiles(manifest);
        if(!"bo2_android_compiled_shaders".equals(manifest.getString("type"))||!"ready".equals(manifest.getString("status")))
            throw new IOException("Biblioteca incompleta");
        long total=0;HashSet<String> chunksSeen=new HashSet<>();
        for(int i=0;i<files.length();i++){
            JSONObject row=files.getJSONObject(i);total+=row.getLong("bytes");JSONArray chunks=row.getJSONArray("chunks");long offset=0;
            if(chunks.length()<1||chunks.length()>64)throw new IOException("Número de lotes inválido");
            for(int j=0;j<chunks.length();j++){
                JSONObject part=chunks.getJSONObject(j);String name=part.getString("zip");long bytes=part.getLong("bytes");
                if(!name.matches("bo2-biblioteca-[0-9]{4}\\.zip")||!chunksSeen.add(name)||part.getLong("offset")!=offset||bytes<1||bytes>CHUNK_MAX||
                   part.getLong("zip_bytes")<1||part.getLong("zip_bytes")>ZIP_MAX||!part.getString("zip_sha256").matches("[0-9a-f]{64}"))
                    throw new IOException("Secuencia de biblioteca inválida");
                hash(part);offset+=bytes;
            }
            if(offset!=row.getLong("bytes"))throw new IOException("Biblioteca truncada en el manifiesto");
        }
        recover(root);File staging=new File(root,"game-shaders.importing");
        remove(staging);if(!staging.mkdirs())throw new IOException("No se pudo preparar la importación");
        if(staging.getUsableSpace()<total+32L*1024*1024){remove(staging);throw new IOException("Espacio insuficiente para importar la biblioteca");}
        long done=0;byte[] block=new byte[128*1024];
        try{
            for(int i=0;i<files.length();i++){
                JSONObject row=files.getJSONObject(i);MessageDigest whole=MessageDigest.getInstance("SHA-256");
                try(FileOutputStream output=new FileOutputStream(new File(staging,row.getString("file")))){
                    JSONArray chunks=row.getJSONArray("chunks");
                    for(int j=0;j<chunks.length();j++){
                        JSONObject part=chunks.getJSONObject(j);MessageDigest chunk=MessageDigest.getInstance("SHA-256");long written=0;
                        try(CheckedStream raw=new CheckedStream(source.open(part.getString("zip")));ZipInputStream zip=new ZipInputStream(raw)){
                            ZipEntry entry=zip.getNextEntry();if(entry==null||!entry.getName().equals("payload.bin")||entry.isDirectory())throw new IOException("Contenido de lote inválido");
                            for(int n;(n=zip.read(block))!=-1;){
                                written+=n;if(written>part.getLong("bytes"))throw new IOException("Lote excede su tamaño");
                                output.write(block,0,n);whole.update(block,0,n);chunk.update(block,0,n);done+=n;progress.update(done,total);
                            }
                            if(zip.getNextEntry()!=null)throw new IOException("Archivos adicionales en el lote");
                            while(raw.read(block)!=-1){}
                            if(raw.count!=part.getLong("zip_bytes")||!hex(raw.digest.digest()).equals(part.getString("zip_sha256")))throw new IOException("Lote dañado: "+part.getString("zip"));
                        }
                        if(written!=part.getLong("bytes")||!hex(chunk.digest()).equals(hash(part)))throw new IOException("Fragmento de biblioteca dañado");
                    }
                    output.getFD().sync();
                }
                if(!hex(whole.digest()).equals(hash(row)))throw new IOException("La biblioteca no supera la verificación SHA-256");
            }
            validatePair(staging,manifest.getInt("shaders"));
            manifest.put("external_library",true).put("installed_by_app_version",BuildConfig.VERSION_NAME);
            try(FileOutputStream output=new FileOutputStream(new File(staging,"manifest.json"))){output.write(manifest.toString().getBytes(StandardCharsets.UTF_8));output.getFD().sync();}
            File active=new File(root,"game-shaders"),previous=new File(root,"game-shaders.previous");remove(previous);
            if(active.exists())Files.move(active.toPath(),previous.toPath(),StandardCopyOption.ATOMIC_MOVE);
            try{Files.move(staging.toPath(),active.toPath(),StandardCopyOption.ATOMIC_MOVE);}
            catch(Exception error){recover(root);throw error;}
            try{remove(previous);}catch(IOException ignored){                                                                         }
            return manifest;
        }finally{remove(staging);}
    }
}
