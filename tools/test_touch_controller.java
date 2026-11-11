// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;
import java.util.*;
final class TestTouchController {
    private static int checks;
    static void check(boolean on,String message){checks++;if(!on)throw new AssertionError(message);}
    static void near(float a,float b,String message){check(Math.abs(a-b)<.002,message+": "+a+" != "+b);}
    static TouchController fresh(String profile){TouchController m=new TouchController();m.selectProfile(profile);m.layout(1920,1080,0,0,0,0);return m;}
    static void press(TouchController m,int id,String name){TouchController.Control c=m.find(name);m.down(id,c.x,c.y);}
    static void buttons(){
        for(String profile:new String[]{TouchProfiles.XBOX,TouchProfiles.CODM,TouchProfiles.CUSTOM}){
            TouchController m=fresh(profile);
            for(TouchController.Control c:m.controls){
                if(c.stick!=0)continue;m.down(0,c.x,c.y);TouchController.State s=m.state();
                check(s.buttons==c.mask,"binding "+profile+"/"+c.id);check(s.lt==(c.trigger==1?255:0),"LT "+c.id);check(s.rt==(c.trigger==2?255:0),"RT "+c.id);
                m.move(0,-100,-100);s=m.state();check(s.buttons==c.mask,"capture outside button");m.up(0);m.advanceLook(1f/60);m.advanceLook(1f/60);check(m.state().neutral(),"release "+c.id);
            }
            check(m.controls.size()==(profile.equals(TouchProfiles.XBOX)?16:18),"button count without sprint");
            for(TouchController.Control c:m.controls)check(!c.action.equals("run"),"sprint has no separate button");
        }
        TouchController m=fresh(TouchProfiles.CODM);press(m,1,"shoot");press(m,2,"hipfire");m.up(1);check(m.state().rt==255,"two independent fire buttons");m.up(2);check(m.state().rt==0,"last fire finger release");
        press(m,1,"jump");press(m,2,"jump");m.up(1);check(m.state().buttons==4096,"duplicate button fingers");m.up(2);
        m.find("jump").enabled=false;press(m,1,"jump");check(m.state().buttons==0,"disabled controls have no input");m.clear();
    }
    static void movement(){
        TouchController m=fresh(TouchProfiles.XBOX);TouchController.Control c=m.find("stick_left");press(m,0,c.id);
        m.move(0,c.x+c.radius*.12f,c.y);check(m.state().lx==0,"radial deadzone boundary");
        m.move(0,c.x+c.radius*.56f,c.y);check(Math.abs(m.state().lx-16384)<=1,"progressive half deflection");
        m.move(0,c.x+c.radius*2,c.y-c.radius*2);TouchController.State s=m.state();check(Math.abs(s.lx-23170)<=1 && Math.abs(s.ly-23170)<=1,"radial clamp preserves diagonal");
        m.move(0,c.x,c.y-c.radius*.86f);check((m.state().buttons&64)!=0,"forward auto sprint");m.move(0,c.x,c.y-c.radius*.80f);check((m.state().buttons&64)!=0,"sprint hysteresis");
        m.move(0,c.x,c.y-c.radius*.75f);check((m.state().buttons&64)==0,"exit sprint");m.move(0,c.x,c.y-c.radius*.80f);check((m.state().buttons&64)==0,"no early restart");
        m.move(0,c.x+c.radius,c.y);check((m.state().buttons&64)==0,"no sideways sprint");m.move(0,c.x,c.y+c.radius);check((m.state().buttons&64)==0,"no backward sprint");m.up(0);check(m.state().neutral(),"stick release neutral");
    }
    static final class Camera implements TouchController.Camera {
        int owner=-1;float x,y,dx,dy;ArrayList<Integer> events=new ArrayList<>();
        public void touch(int action,int pointer,float x,float y,float w,float h){events.add(action);if(action==3){owner=-1;dx=dy=0;}else if(action==0 && owner<0){owner=pointer;this.x=x;this.y=y;}else if(pointer==owner){dx+=(x-this.x)*w/h*720;dy+=(y-this.y)*720;this.x=x;this.y=y;if(action==2)owner=-1;}}
    }
    static void camera(){
        TouchController m=fresh(TouchProfiles.CODM);Camera log=new Camera();m.camera=log;TouchController.Control shoot=m.find("shoot");
        press(m,1,"shoot");check(log.owner<0,"fire button waits for drag");m.move(1,shoot.x+4,shoot.y);check(log.owner<0,"button touch slop");m.move(1,shoot.x+20,shoot.y);check(log.owner==1,"drag on fire owns camera");
        check(m.state().rt==255 && m.state().rx==0 && m.state().ry==0,"camera does not drive right stick");m.down(2,960,700);check(log.owner==2,"free finger takes camera from button");m.move(1,shoot.x+100,shoot.y);near(log.dx,0,"losing finger cannot move camera");m.move(2,980,710);check(log.dx>0 && log.dy>0,"free camera motion");
        m.down(3,1100,700);check(log.owner==2,"second free finger waits");m.move(3,1200,700);m.up(2,1000,720);check(log.owner==3,"free finger handoff");float before=log.dx;m.move(3,1210,700);near(log.dx-before,7200f/1080,"handoff without jump");
        m.clear();check(log.owner<0 && m.state().neutral(),"cancel releases camera and buttons");
        m.down(0,960,700);m.up(0,1014,700);near(log.dx,36,"UP final position retained");
        m.clear();m.down(0,960,700);m.move(0,Float.NaN,0);check(log.dx==0,"nonfinite motion ignored");m.clear();
        m.selectProfile(TouchProfiles.XBOX);m.layout(1920,1080,0,0,0,0);TouchController.Control rt=m.find("rt");press(m,0,"rt");m.move(0,rt.x-50,rt.y);check(log.owner==0 && m.state().rt==255,"Xbox fire also supports dragging the camera");m.clear();
    }
    static void layout(){
        for(int[] v:new int[][]{{800,360},{1280,720},{1920,1080},{2340,1080},{2400,1080},{2560,1440}}){
            TouchController m=fresh(TouchProfiles.XBOX);m.layout(v[0],v[1],0,0,0,0);TouchController.Control a=m.find("a");near(a.x/v[0],.88f,"Xbox A position");near(a.radius/v[1],.07f,"height-based button radius");
            TouchController.Control stick=m.find("stick_left");near(stick.x/v[0],.14f,"Xbox stick position");near(stick.radius/v[1],.18f,"Xbox stick diameter");
        }
        TouchController m=fresh(TouchProfiles.XBOX);m.layout(2400,1080,80,20,40,30);TouchController.Control c=m.find("a");near(c.x,80+.88f*2280,"safe viewport X");near(c.y,20+.7f*1030,"safe viewport Y");
        String custom=m.duplicateProfile("Copia");c=m.find("a");m.place(c,1000,500,1.5f);m.selectProfile(TouchProfiles.XBOX);near(m.find("a").normalX,.88f,"copy is independent");m.selectProfile(custom);check(m.find("a").scale==1.5f,"profile edits preserved");
        m.down(0,1200,600);m.selectProfile(TouchProfiles.CODM);check(m.state().neutral(),"profile switch cancels input");
    }
    static void ads(){
        TouchAdsFire a=new TouchAdsFire();a.configure(0,220);a.press(1000,false);check(a.triggers(1000)==1,"ADS immediate");check(a.triggers(1119)==1,"120ms ADS delay");check(a.triggers(1120)==3,"fire after ADS delay");a.release(1121);check(a.triggers(1219)==3,"minimum 100ms shot");check(a.triggers(1220)==1,"fire ends");check(a.triggers(1520)==0,"300ms ADS tail");
        a.cancel();a.press(2000,true);check(a.triggers(2000)==3,"already aiming skips delay");a.cancel();check(a.triggers(2000)==0,"cancel kills scheduled actions");
        a.configure(1,220);a.press(0,false);check(a.triggers(120)==3,"repeat first pulse");check(a.triggers(220)==1,"repeat gap");check(a.nextChange(220)==340,"repeat next edge");check(a.triggers(340)==3,"repeat second pulse");a.release(350);check(a.triggers(440)==1,"repeat ends after current pulse");a.cancel();
        a.configure(2,220);a.press(0,false);check(a.triggers(500)==1,"release mode holds aim only");a.release(500);check(a.triggers(500)==3,"release fires");check(a.triggers(600)==1,"release pulse ends");check(a.triggers(900)==0,"release ADS tail");
        for(String profile:new String[]{TouchProfiles.CODM,TouchProfiles.XBOX,TouchProfiles.CUSTOM}) {
            TouchController m=fresh(profile);m.autoAim=true;m.time(1000);
            String fire=profile.equals(TouchProfiles.XBOX)?"rt":"shoot";press(m,1,fire);
            check(m.state().lt==255 && m.state().rt==0,"principal button immediately aims: "+profile);
            m.time(1119);check(m.state().rt==0,"ADS starts before the shot");
            for(long at:new long[]{1120,1500,5000,15000}) {m.time(at);check(m.state().lt==255 && m.state().rt==255,"held fire keeps ADS and continuous trigger");}
            TouchController.Control c=m.find(fire);m.move(1,c.x-80,c.y+40);
            check(m.state().lt==255 && m.state().rt==255,"camera drag preserves ADS fire");
            m.up(1);check(m.state().rt==0 && m.state().lt==255,"release ends continuous fire");
            m.time(15300);check(m.state().lt==0,"ADS tail ends without a stuck aim");
            m.time(16000);press(m,1,fire);m.clear();check(m.nextChange()==-1 && m.state().neutral(),"menu cancels delayed ADS fire");
            m.autoAim=false;press(m,1,fire);check(m.state().lt==0 && m.state().rt==255,"option off fires without automatic ADS");m.clear();
        }
        TouchController m=fresh(TouchProfiles.CODM);m.autoAim=true;press(m,1,"hipfire");
        check(m.state().lt==0 && m.state().rt==255,"secondary hip fire remains manual");m.clear();
        m.time(1000);press(m,0,"ads");press(m,1,"shoot");check(m.state().lt==255 && m.state().rt==255,"manual ADS skips the startup delay");
        m.up(1);m.time(2000);check(m.state().lt==255 && m.state().rt==0,"releasing fire never releases the separate ADS finger");m.clear();
        m.time(3000);press(m,1,"shoot");press(m,2,"shoot");m.time(3120);m.up(1);check(m.state().lt==255 && m.state().rt==255,"second fire finger keeps the combined trigger held");m.up(2);m.clear();
        m.time(4000);press(m,0,"move");TouchController.Control stick=m.find("move");m.move(0,stick.x,stick.y-stick.radius);
        check((m.state().buttons&64)!=0,"joystick sprints before shooting");press(m,1,"shoot");
        check((m.state().buttons&64)==0 && m.state().ly>32000,"ADS stops sprint without stopping movement");
        m.time(5000);m.up(1);m.time(5300);check((m.state().buttons&64)!=0,"joystick resumes sprint after ADS finishes");m.clear();
    }
    public static void main(String[] args){buttons();movement();camera();layout();ads();System.out.println("Touch controller: "+checks+" checks passed");}
}
