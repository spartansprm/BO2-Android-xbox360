// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import java.util.HashMap;


final class TouchHudRenderer {
    private static final int WHITE=0xFFF3F5EF, ACCENT=0xFFFFC466;
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF arc=new RectF();
    private final HashMap<String,Glyph> glyphs=new HashMap<>();
    private int opacity;

    TouchHudRenderer() {
        paint.setStrokeCap(Paint.Cap.ROUND); paint.setStrokeJoin(Paint.Join.ROUND);
        paint.setTypeface(Typeface.create("sans-serif-medium",Typeface.NORMAL));
        for(String action:TouchProfiles.ACTIONS) glyphs.put(action,glyph(action));
    }
    private void ink(int color,Paint.Style style,float width) {
        paint.setColor(color); paint.setAlpha(Math.round((color>>>24)*opacity/255f));
        paint.setStyle(style); paint.setStrokeWidth(width);
    }
    private void ring(Canvas canvas,float x,float y,float radius,int color,float width) {
        ink(color,Paint.Style.STROKE,width); canvas.drawCircle(x,y,radius,paint);
    }
    private void arc(Canvas canvas,float x,float y,float r,float start,float sweep,int color,float width) {
        arc.set(x-r,y-r,x+r,y+r); ink(color,Paint.Style.STROKE,width); canvas.drawArc(arc,start,sweep,false,paint);
    }
    void draw(Canvas canvas,TouchController model,TouchController.Control c,boolean selected,TouchController.State state) {
        opacity=Math.round(255*(model.editing&&!c.enabled?.25f:c.opacity));
        boolean held=model.pressed(c), active=held || c.trigger==1&&state.lt!=0;
        float r=c.radius, u=model.unit;
        int accent=selected||active?ACCENT:WHITE;

        ink(selected?0x705C401C:active?0x9C43351F:0x68202629,Paint.Style.FILL,0);
        canvas.drawCircle(c.x,c.y,r,paint);
        ring(canvas,c.x,c.y,r,active||selected?0xEBFFC466:0x8CF3F5EF,(active||selected?1.8f:1.1f)*u);
        ring(canvas,c.x,c.y,r*.88f,active?0x4DFFC466:0x20FFFFFF,.65f*u);
        arc(canvas,c.x,c.y,r-2*u,210,75,active?0xA0FFC466:0x50FFFFFF,.8f*u);
        if(c.stick!=0) {
            stick(canvas,model,c,state,u);
        } else {
            if(c.trigger==2) {
                arc(canvas,c.x,c.y,r+2.8f*u,20,42,active?ACCENT:0xA6F3F5EF,1.5f*u);
                arc(canvas,c.x,c.y,r+2.8f*u,200,42,active?ACCENT:0xA6F3F5EF,1.5f*u);
            }
            Glyph icon=glyphs.get(c.action);
            if(icon!=null) {
                canvas.save(); canvas.translate(c.x,c.y); float scale=r*.060f; canvas.scale(scale,scale);
                ink(accent,Paint.Style.FILL,0); canvas.drawPath(icon.fill,paint);
                ink(accent,Paint.Style.STROKE,1.65f); canvas.drawPath(icon.stroke,paint);
                canvas.restore();
            }
            if(c.action.equals("shoot") && model.autoAim) {

                float x=c.x+r*.69f,y=c.y-r*.69f,badge=r*.25f;
                ink(0xE31B2326,Paint.Style.FILL,0); canvas.drawCircle(x,y,badge,paint);
                ring(canvas,x,y,badge,ACCENT,.9f*u);
                ink(ACCENT,Paint.Style.STROKE,.9f*u);
                canvas.drawLine(x-badge*.5f,y,x+badge*.5f,y,paint);
                canvas.drawLine(x,y-badge*.5f,x,y+badge*.5f,paint);
            }
        }
        if(selected) {
            ink(ACCENT,Paint.Style.FILL,0); canvas.drawCircle(c.x+r*.707f,c.y+r*.707f,3*u,paint);
        }
        if(model.editing) {

            ink(WHITE,Paint.Style.FILL,0); paint.setTextSize(8*u); paint.setTextAlign(Paint.Align.CENTER);
            String name=TouchProfiles.LABELS[TouchProfiles.actionIndex(c.action)];
            canvas.drawText(name,c.x,c.y+r+11*u,paint);
        }
    }
    private void stick(Canvas canvas,TouchController model,TouchController.Control c,TouchController.State state,float u) {
        boolean running=(state.buttons&0x40)!=0;
        int color=running?ACCENT:0x9AF3F5EF;
        arc(canvas,c.x,c.y,c.radius*.77f,228,84,running?0xB8FFC466:0x38FFFFFF,2.6f*u);
        ink(color,Paint.Style.STROKE,1.5f*u);
        float y=c.y-c.radius*.69f, d=c.radius*.105f;
        for(int n=0;n<2;n++) {
            canvas.drawLine(c.x-d,y+d,c.x,y,paint); canvas.drawLine(c.x,y,c.x+d,y+d,paint); y+=d*.85f;
        }
        float[] thumb=model.thumb(c);
        ink(running?0x5AFFC466:0x30F3F5EF,Paint.Style.FILL,0); canvas.drawCircle(thumb[0],thumb[1],c.radius*.37f,paint);
        ring(canvas,thumb[0],thumb[1],c.radius*.37f,running?0xD9FFC466:0xB3F3F5EF,1.3f*u);
        ring(canvas,thumb[0],thumb[1],c.radius*.29f,0x26FFFFFF,.7f*u);
        ink(running?ACCENT:0xB3F3F5EF,Paint.Style.FILL,0); canvas.drawCircle(thumb[0],thumb[1],1.5f*u,paint);
    }
    private static final class Glyph {
        final Path fill=new Path(),stroke=new Path();
        void line(float...p) { stroke.moveTo(p[0],p[1]); for(int i=2;i<p.length;i+=2)stroke.lineTo(p[i],p[i+1]); }
        void shape(float...p) { fill.moveTo(p[0],p[1]); for(int i=2;i<p.length;i+=2)fill.lineTo(p[i],p[i+1]); fill.close(); }
        void head(float x,float y) { fill.addCircle(x,y,2.15f,Path.Direction.CW); }
    }
    private static Glyph glyph(String action) {
        Glyph g=new Glyph();
        switch(action) {
            case "shoot": case "hipfire":
                g.shape(-2.8f,8,-2.8f,-3,-1.5f,-7,0,-10,1.5f,-7,2.8f,-3,2.8f,8);
                g.line(-4,10,4,10); g.line(-4,5,4,5);
                android.graphics.Matrix tilt=new android.graphics.Matrix(); tilt.setRotate(38); g.fill.transform(tilt);g.stroke.transform(tilt);
                break;
            case "ads":
                g.stroke.addCircle(0,0,7,Path.Direction.CW);g.line(-11,0,-4,0);g.line(4,0,11,0);g.line(0,-11,0,-4);g.line(0,4,0,11);
                g.fill.addCircle(0,0,1.1f,Path.Direction.CW);break;
            case "jump":
                g.head(1,-8);g.shape(-1,-5,3,-4,3,1,0,3,-3,0);
                g.line(-1,-3,-5,-1,-8,-6);g.line(3,-3,7,-5,8,-9);
                g.line(0,2,-5,5,-7,10);g.line(1,2,6,5,8,3);break;
            case "crouch":
                g.head(-2,-7);g.shape(-4,-4,0,-4,2,1,-2,2,-5,-1);
                g.line(-3,2,3,4,3,9,8,9);g.line(-2,2,-7,6,-10,6);
                g.line(0,-3,3,-1,8,-1);g.shape(1,-3,10,-3,10,-1,6,-1,5,1,4,1,4,-1,1,-1);break;
            case "reload":
                g.stroke.addArc(new RectF(-10,-10,10,10),-65,260);g.shape(-11,-4,-6,-3,-9,1);
                g.shape(-3,-5,3,-5,4,6,-2,7);g.line(-1,-2,1,-2);g.line(-.5f,1,1.5f,1);g.line(0,4,2,4);break;
            case "interact":
                g.line(-6,9,-9,1,-9,-2,-7,-3,-4,0,-4,-7,-2,-9,0,-7,0,-2,0,-9,2,-10,4,-8,4,-2,4,-6,6,-7,8,-5,8,3,5,9,-6,9);break;
            case "change_weapon":
                g.shape(-11,-2,-7,-2,-7,-4,4,-4,5,-6,7,-6,7,-4,11,-4,11,-2,5,-2,4,0,-3,0,-4,4,-6,4,-6,0,-9,0,-11,2);
                g.shape(-1,0,3,0,2,6,-1,6);g.line(-8,8,8,8);g.line(5,5,8,8,5,11);break;
            case "melee":
                g.shape(-9,9,-5,3,-3,3,3,-6,9,-10,8,-3,1,3,1,5,-5,11);
                g.line(-5,0,3,6);break;
            case "lethal":
                g.fill.addOval(new RectF(-6,-3,6,10),Path.Direction.CW);
                g.line(-3,-4,-3,-7,2,-7,4,-2);g.stroke.addCircle(5,-6,2.5f,Path.Direction.CW);
                g.line(-3,0,3,0);g.line(-4,4,4,4);break;
            case "tactical":
                g.line(-5,-5,-3,-8,2,-8,4,-4);g.line(1,-9,7,-9,7,-5);
                g.shape(-4,-4,4,-4,4,9,-4,9);g.line(-6,1,6,1);g.line(-6,5,6,5);break;
            case "pause":
                g.fill.addRoundRect(new RectF(-7,-9,-3,9),1,1,Path.Direction.CW);
                g.fill.addRoundRect(new RectF(3,-9,7,9),1,1,Path.Direction.CW);break;
            case "scores":
                g.stroke.addRoundRect(new RectF(-10,-8,10,8),2,2,Path.Direction.CW);
                for(int y=-4;y<=4;y+=4){g.line(-6,y,-4,y);g.line(0,y,6,y);}break;
            case "dpad_up": case "dpad_down": case "dpad_left": case "dpad_right":
                if(action.equals("dpad_left")) {
                    g.line(-9,-4,7,-4,4,-7);g.line(7,4,-9,4,-6,7);
                } else if(action.equals("dpad_right")) {
                    g.shape(-4,-8,8,0,-4,8);g.line(-9,-8,-9,8);
                } else {
                    g.line(-7,3,0,-4,7,3);g.line(-7,9,0,2,7,9);
                    if(action.equals("dpad_down")){android.graphics.Matrix flip=new android.graphics.Matrix();flip.setRotate(180);g.stroke.transform(flip);}
                }
                break;
            default: break;
        }
        return g;
    }
}
