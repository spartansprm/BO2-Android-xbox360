package org.bo2port.diagnostic;
import java.io.*;
import java.nio.file.*;
import org.json.JSONObject;

final class BuildConfig {static final String VERSION_NAME="host-test";}
final class TestShaderLibraryInstaller {
    public static void main(String[] args)throws Exception {
        File source=new File(args[0]),root=new File(args[1]);root.mkdirs();
        File old=new File(root,"game-shaders"),keep=new File(old,"keep.txt");old.mkdirs();Files.writeString(keep.toPath(),"previous active library");
        boolean reject=args[2].equals("reject");
        try{
            JSONObject result=ShaderLibraryInstaller.install(name->new FileInputStream(new File(source,name)),root,(done,total)->{
                if(done<0||done>total)throw new AssertionError("Bad progress");
            });
            if(reject)throw new AssertionError("Accepted invalid library");
            if(!result.getBoolean("external_library")||keep.exists())throw new AssertionError("Not committed");
            for(int i=0;i<result.getJSONArray("files").length();i++){
                JSONObject row=result.getJSONArray("files").getJSONObject(i);
                if(!ShaderLibraryInstaller.matches(new File(old,row.getString("file")),row))throw new AssertionError("Wrong installed bytes");
            }
            JSONObject prepared=GameShaderAssets.imported(new android.content.Context(root));
            if(prepared==null||!prepared.getBoolean("external_library")||prepared.getInt("shaders")!=result.getInt("shaders"))
                throw new AssertionError("Launch did not select the installed library");
            if(GameShaderAssets.removeBundledCopy(new android.content.Context(root))||!new File(old,"bo2zm_shaders.spvpack").isFile())
                throw new AssertionError("An imported library was removed as a bundled copy");
            File installedPack=new File(old,"bo2zm_shaders.spvpack");
            try(RandomAccessFile changed=new RandomAccessFile(installedPack,"rw")){
                changed.seek(23);int previous=changed.read();changed.seek(23);changed.write(previous^1);
                try{GameShaderAssets.imported(new android.content.Context(root));throw new AssertionError("Corrupt installed file accepted");}
                catch(IOException expected){}
                changed.seek(23);changed.write(previous);
            }
            Files.move(old.toPath(),new File(root,"game-shaders.previous").toPath(),StandardCopyOption.ATOMIC_MOVE);
            ShaderLibraryInstaller.recover(root);
            if(!new File(old,"manifest.json").isFile())throw new AssertionError("Interrupted publication did not recover");
        }catch(Exception error){
            if(!reject)throw error;
            if(!keep.isFile()||!Files.readString(keep.toPath()).equals("previous active library"))throw new AssertionError("Failure changed active library",error);
        }
        if(new File(root,"game-shaders.importing").exists())throw new AssertionError("Staging left after failure");

        File bundled=new File(root,"bundled-check/game-shaders");bundled.mkdirs();
        Files.writeString(new File(bundled,"manifest.json").toPath(),"{\"schema\":1,\"shaders\":626,\"files\":[]}");
        Files.writeString(new File(bundled,"bo2zm_shaders.spvpack").toPath(),"pack");
        Files.writeString(new File(bundled,"bo2zm_shaders.spvidx").toPath(),"index");
        if(!GameShaderAssets.removeBundledCopy(new android.content.Context(new File(root,"bundled-check")))||
           new File(bundled,"bo2zm_shaders.spvpack").exists()||new File(bundled,"manifest.json").exists())
            throw new AssertionError("Bundled copy not removed");
        if(GameShaderAssets.imported(new android.content.Context(new File(root,"bundled-check")))!=null)
            throw new AssertionError("Nothing imported must give no library");
        System.out.println(reject?"rejected_preserving_previous":"installed_exact_bytes_and_recovered");
    }
}
