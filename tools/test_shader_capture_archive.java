// SPDX-License-Identifier: GPL-3.0-only

package org.bo2port.diagnostic;
import java.nio.file.*;
import java.util.zip.ZipFile;
import org.json.JSONObject;
final class TestShaderCaptureArchive {
    private static void require(boolean value){if(!value)throw new AssertionError();}
    public static void main(String[] args)throws Exception {
        Path root=Path.of(args[0]),files=root.resolve("files"),cache=root.resolve("cache");
        String run="1f599c1e-c0b1-41d2-946d-edf338465449";
        Path capture=files.resolve("shader-capture/"+run);Files.createDirectories(capture);Files.createDirectories(cache);
        try(var paths=Files.list(Path.of(args[1]))) {
            for(Path file:paths.toList())Files.copy(file,capture.resolve(file.getFileName()));
        }
        byte[] log="authored SDK diagnostic log\n".getBytes(java.nio.charset.StandardCharsets.UTF_8);Files.write(files.resolve("game-sdk.log"),log);
        JSONObject session=new JSONObject().put("run_id",run).put("app_version",BuildConfig.VERSION_NAME)
            .put("game_entry_executed",true).put("loaded_image_sha256","ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e");
        Path archive=ShaderCaptureArchive.prepare(files.toFile(),cache.toFile(),session).toPath();byte[] prior=Files.readAllBytes(archive);
        try(ZipFile zip=new ZipFile(archive.toFile())) {
            require(java.util.Arrays.equals(zip.getInputStream(zip.getEntry("game-sdk.log")).readAllBytes(),log));
            JSONObject manifest=new JSONObject(new String(zip.getInputStream(zip.getEntry("capture_manifest.json")).readAllBytes(),java.nio.charset.StandardCharsets.UTF_8));
            require(manifest.getJSONArray("containers").length()==2&&manifest.getString("run_id").equals(run));
        }
        Path invalid=capture.resolve("p_"+"f".repeat(32)+".bin");Files.write(invalid,new byte[65537]);
        try{ShaderCaptureArchive.prepare(files.toFile(),cache.toFile(),session);throw new AssertionError();}catch(java.io.IOException expected){}
        require(java.util.Arrays.equals(prior,Files.readAllBytes(archive)));require(!Files.exists(Path.of(archive+".part")));Files.delete(invalid);
        Files.createSymbolicLink(invalid,Path.of(args[1]).toAbsolutePath().resolve(Files.list(Path.of(args[1])).findFirst().orElseThrow().getFileName()));
        try{ShaderCaptureArchive.prepare(files.toFile(),cache.toFile(),session);throw new AssertionError();}catch(java.io.IOException expected){}
        require(java.util.Arrays.equals(prior,Files.readAllBytes(archive)));Files.delete(invalid);
        session.put("app_version","0.19-cache-diagnostics");
        require(ShaderCaptureArchive.compatibleSession(session));
        var previous=ShaderCaptureArchive.prepare(files.toFile(),cache.toFile(),session);
        try(ZipFile zip=new ZipFile(previous)) {
            JSONObject manifest=new JSONObject(new String(zip.getInputStream(zip.getEntry("capture_manifest.json")).readAllBytes(),java.nio.charset.StandardCharsets.UTF_8));
            require(manifest.getString("app_version").equals("0.19-cache-diagnostics"));
        }
        session.put("loaded_image_sha256","0".repeat(64));require(!ShaderCaptureArchive.compatibleSession(session));
        try{ShaderCaptureArchive.prepare(files.toFile(),cache.toFile(),session);throw new AssertionError();}catch(java.io.IOException expected){}
        session.put("loaded_image_sha256","ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e");
        session.put("app_version","old-version");
        try{ShaderCaptureArchive.prepare(files.toFile(),cache.toFile(),session);throw new AssertionError();}catch(java.io.IOException expected){}
        session.put("app_version",BuildConfig.VERSION_NAME);
        Path many=Files.createTempDirectory(root,"expanded-");
        try {
            Path manyFiles=many.resolve("files"),manyCache=many.resolve("cache"),manyCapture=manyFiles.resolve("shader-capture/"+run);
            Files.createDirectories(manyCapture);Files.createDirectories(manyCache);
            byte[] authored=new byte[48];authored[0]=0x10;authored[1]=0x2A;authored[2]=0x11;authored[3]=1;authored[7]=24;authored[11]=24;
            for(int i=0;i<8192;++i)Files.write(manyCapture.resolve(String.format(java.util.Locale.ROOT,"v_%032x.bin",i)),authored);
            FileCheck.check(manyFiles,manyCache,session,8192);
        }finally{
            try(var paths=Files.walk(many)){for(Path path:paths.sorted(java.util.Comparator.reverseOrder()).toList())Files.delete(path);}
        }
        System.out.println("Java capture export: native-authored bytes, metadata, complete log, size/symlink rejection and prior archive preservation passed");
        System.out.println(archive);
    }
    private static final class FileCheck {
        static void check(Path files,Path cache,JSONObject session,int count)throws Exception {
            var archive=ShaderCaptureArchive.prepare(files.toFile(),cache.toFile(),session);
            try(ZipFile zip=new ZipFile(archive)) {
                var manifest=zip.getEntry("capture_manifest.json");require(manifest.getSize()>1024*1024&&manifest.getSize()<4*1024*1024);
                JSONObject metadata=new JSONObject(new String(zip.getInputStream(manifest).readAllBytes(),java.nio.charset.StandardCharsets.UTF_8));
                require(metadata.getJSONArray("containers").length()==count);
            }
            System.out.println("Expanded capture export passed: "+count+" entries, including manifest larger than the previous limit");
        }
    }
}
