// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;
import org.json.*;
final class TestTouchSettings {
    static int checks;
    static void check(boolean yes,String text){checks++;if(!yes)throw new AssertionError(text);}
    public static void main(String[] args)throws Exception {
        TouchController m=new TouchController();m.layout(1920,1080,0,0,0,0);
        check(m.profile.equals(TouchProfiles.CODM),"mobile is the initial profile");check(m.profiles.size()==3,"three initial profiles");
        m.selectProfile(TouchProfiles.CODM);m.find("shoot").opacity=.37f;m.find("jump").enabled=false;
        m.find("move").deadzone=.23f;m.find("move").sensitivity=1.35f;m.aimDrag=false;m.stickSprint=false;m.sprintThreshold=.91f;m.autoAim=true;m.fireMode=2;m.fireRepeatMs=450;
        String custom=m.duplicateProfile("Mi HUD");TouchController.Control copy=m.addControl("shoot");copy.look=true;m.place(copy,900,500,1.4f);
        m.resetProfile();check(m.controls.size()==18,"custom reset retains CODM base without sprint");copy=m.addControl("shoot");copy.look=true;m.place(copy,900,500,1.4f);m.find("shoot").opacity=.37f;m.find("jump").enabled=false;m.find("move").deadzone=.23f;m.find("move").sensitivity=1.35f;
        String encoded=TouchSettings.encodeLayout(m);TouchController n=new TouchController();check(TouchSettings.readLayout(n,encoded),"v2 round trip");
        check(n.profile.equals(custom),"active custom profile retained");check(n.profiles.size()==4,"custom profiles retained");check(n.controls.size()==19,"duplicate actions retained");
        check(!n.find("jump").enabled,"hidden setting");check(Math.abs(n.find("shoot").opacity-.37f)<1e-6,"opacity setting");check(n.find("move").deadzone==.23f,"deadzone setting");check(n.find("move").sensitivity==1.35f,"sensitivity setting");
        check(!n.aimDrag && !n.stickSprint && n.autoAim && n.fireMode==2 && n.fireRepeatMs==450,"response options retained");
        n.selectProfile(TouchProfiles.XBOX);check(n.find("a").enabled,"editing a copy does not edit Xbox");
        TouchController imported=new TouchController();check(TouchSettings.importProfile(imported,TouchSettings.exportProfile(m)),"profile import");check(imported.profile.startsWith("custom:"),"import cannot replace built-in profile");check(imported.controls.size()==19,"imported duplicate actions");
        String good=TouchSettings.encodeLayout(imported);
        check(!TouchSettings.readLayout(imported,"{"),"malformed JSON rejected");check(good.equals(TouchSettings.encodeLayout(imported)),"invalid read is atomic");
        JSONObject bad=new JSONObject(encoded);bad.put("schema",99);check(!TouchSettings.readLayout(imported,bad.toString()),"future schema rejected");
        bad=new JSONObject(encoded);bad.getJSONArray("profiles").getJSONObject(0).getJSONArray("controls").getJSONObject(0).put("x",2);check(!TouchSettings.readLayout(imported,bad.toString()),"invalid coordinates rejected");
        bad=new JSONObject(encoded);JSONArray p=bad.getJSONArray("profiles");p.put(p.getJSONObject(0));check(!TouchSettings.readLayout(imported,bad.toString()),"duplicate profile rejected");
        bad=new JSONObject(encoded);bad.getJSONArray("profiles").getJSONObject(0).getJSONArray("controls").getJSONObject(0).put("action","execute arbitrary command");check(!TouchSettings.readLayout(imported,bad.toString()),"unknown action rejected");
        check(!TouchSettings.readLayout(imported," ".repeat(TouchSettings.MAX_BYTES+1)),"oversized profile rejected");
        TouchController legacy=new TouchController();check(TouchSettings.readLayout(legacy,"{\"fire\":[0.8,0.3,1.2],\"move\":[0.2,0.7,1.1],\"other\":null}"),"v1 migration");check(legacy.profile.equals("custom:migrated"),"v1 saved as separate profile");check(legacy.find("rt").normalX==.8f,"v1 fire position preserved");check(legacy.find("stick_left").normalX==.2f,"v1 joystick preserved");
        TouchController restored=new TouchController();check(TouchSettings.readLayout(restored,TouchSettings.encodeLayout(legacy)),"migrated format round trip");check(restored.profile.equals("custom:migrated"),"migrated profile stays selected");
        TouchController defaults=new TouchController();check(TouchSettings.readLayout(defaults,"{}"),"empty previous layout uses defaults");check(defaults.profile.equals(TouchProfiles.CODM),"empty migration defaults mobile");
        JSONObject old=new JSONObject(encoded);old.remove("hudRevision");
        JSONObject xbox=old.getJSONArray("profiles").getJSONObject(0);JSONArray buttons=xbox.getJSONArray("controls");
        JSONObject run=new JSONObject(buttons.getJSONObject(0).toString()).put("id","ls_click").put("action","run").put("label","L3");buttons.put(run);
        for(int i=0;i<buttons.length();i++)if(buttons.getJSONObject(i).getString("id").equals("rt"))buttons.getJSONObject(i).put("dragFire",false);
        TouchController upgraded=new TouchController();check(TouchSettings.readLayout(upgraded,old.toString()),"old sprint button migrates");
        check(upgraded.profile.equals(custom),"migration preserves active custom profile");
        check(upgraded.find("shoot").normalX==m.find("shoot").normalX && upgraded.find("shoot").scale==m.find("shoot").scale,"migration preserves edited fire geometry");
        check(upgraded.stickSprint,"removing the old run button leaves sprint on the joystick");
        for(TouchProfiles.Profile profile:upgraded.profiles.values())for(TouchController.Control c:profile.controls)check(!c.action.equals("run"),"no saved run buttons remain");
        upgraded.selectProfile(TouchProfiles.XBOX);check(upgraded.find("rt").dragFire,"old Xbox RT gains drag aiming");
        check(!java.util.Arrays.asList(TouchProfiles.editableActions()).contains("run"),"editor cannot re-add a run button");
        String snapshot=java.nio.file.Files.readString(java.nio.file.Path.of(System.getProperty("bo2.tests.root","."),
            "android/app/src/test/resources/touch-layout-v0521.json"));
        TouchController stock=new TouchController();check(TouchSettings.readLayout(stock,snapshot),"real 0.52.1 snapshot loads");
        check(stock.profile.equals(TouchProfiles.CODM),"old active mobile profile stays selected");
        check(stock.find("move").normalX==.145f && stock.find("move").normalY==.74f,"stock mobile geometry upgrades");
        check(Math.abs(stock.find("move").baseRadius/180f-.32f)<.00001,"stock joystick becomes more compact");
        check(stock.find("shoot").opacity==.52f,"stock opacity survives the layout refresh");
        stock.selectProfile(TouchProfiles.CUSTOM);
        check(stock.find("rt").normalX==.84f && stock.find("rt").scale==1.3f,"actual previous custom placement stays intact");
        for(TouchProfiles.Profile profile:stock.profiles.values())for(TouchController.Control c:profile.controls)check(!c.action.equals("run"),"0.52.1 run buttons removed everywhere");
        System.out.println("Touch settings: "+checks+" checks passed");
    }
}
