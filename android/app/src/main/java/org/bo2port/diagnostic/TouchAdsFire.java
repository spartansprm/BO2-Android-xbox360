// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import java.util.ArrayList;


final class TouchAdsFire {
    private static final long FOREVER = Long.MAX_VALUE;
    private static final class Shot {
        long start, end;
        Shot(long start,long end) { this.start=start; this.end=end; }
    }
    private final ArrayList<Shot> shots = new ArrayList<>();
    private long aimStart = -1, aimEnd = -1, interval = 220;
    private boolean held;
    private int mode;
    void configure(int mode,long interval) { this.mode = mode==1 || mode==2 ? mode : 0; this.interval=Math.max(170,interval); }
    private void advance(long now) {
        if (held && mode==1 && !shots.isEmpty()) {
            Shot last=shots.get(shots.size()-1);
            if (last.end<=now) {
                long at=Math.max(last.start+interval,last.end+70); shots.add(new Shot(at,at+100));
            }
        }
    }
    int triggers(long now) {
        advance(now);
        if (aimStart<0 || now<aimStart) return 0;
        int value=now<aimEnd ? 1 : 0;
        for(Shot s:shots) if(s.start<=now && now<s.end) value|=2;
        return value;
    }
    void press(long now,boolean alreadyAiming) {
        while(shots.size()>1 && shots.get(0).end<=now) shots.remove(0);
        boolean aimed=alreadyAiming || (triggers(now)&1)!=0;
        long start=now+(aimed?0:120);
        if(!shots.isEmpty()) {
            Shot previous=shots.get(shots.size()-1);
            if(previous.end==FOREVER) previous.end=Math.max(now,previous.start+100);
            start=Math.max(start,previous.end+70);
            int waiting=0;for(Shot s:shots) if(s.end>now)waiting++;
            if(waiting>=5){held=true;aimEnd=FOREVER;return;}
        }
        if(aimStart<0 || (triggers(now)&1)==0) aimStart=now;
        aimEnd=FOREVER;held=true;
        if(mode!=2) shots.add(new Shot(start,mode==1?start+100:FOREVER));
    }
    void release(long now) {
        if(!held)return;
        held=false;
        if(mode==2) {
            long at=Math.max(now,aimStart+120); shots.add(new Shot(at,at+100));aimEnd=at+400;return;
        }
        if(mode==1) for(int i=shots.size()-1;i>=0;--i) if(shots.get(i).start>now)shots.remove(i);
        long until=now;
        if(!shots.isEmpty()) {
            Shot last=shots.get(shots.size()-1);
            if(last.end==FOREVER)last.end=Math.max(now,last.start+100);
            until=Math.max(until,last.end);
        }
        aimEnd=until+300;
    }
    long nextChange(long now) {
        advance(now);long next=FOREVER;
        for(Shot s:shots){if(s.start>now)next=Math.min(next,s.start);if(s.end>now)next=Math.min(next,s.end);}
        if(aimEnd>now)next=Math.min(next,aimEnd);
        return next==FOREVER?-1:next;
    }
    void cancel() { held=false;shots.clear();aimStart=aimEnd=-1; }
}
