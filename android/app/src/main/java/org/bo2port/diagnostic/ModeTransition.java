// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.app.Activity;
import android.content.Intent;
import android.os.Looper;
import android.os.ResultReceiver;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;


final class ModeTransition {
    static final String READY="mode_transition_ready";
    static final String CONTENT_READY="mode_content_ready";
    static void announceContent(Intent intent,android.view.View view) {
        ResultReceiver receiver=android.os.Build.VERSION.SDK_INT>=33 ? intent.getParcelableExtra(CONTENT_READY,ResultReceiver.class) : intent.getParcelableExtra(CONTENT_READY);
        if(receiver==null)return;
        view.getViewTreeObserver().addOnPreDrawListener(new android.view.ViewTreeObserver.OnPreDrawListener(){
            public boolean onPreDraw(){
                view.getViewTreeObserver().removeOnPreDrawListener(this);
                view.post(()->{intent.removeExtra(CONTENT_READY);receiver.send(Activity.RESULT_OK,null);});return true;
            }
        });
    }
    static boolean request(Activity activity,String mode) {
        CountDownLatch ready=new CountDownLatch(1);
        ResultReceiver receiver=new ResultReceiver(null) {
            @Override protected void onReceiveResult(int code,android.os.Bundle data){ready.countDown();}
        };
        Intent next=new Intent(activity,RelaunchActivity.class);
        if(activity.getIntent().getExtras()!=null)next.putExtras(activity.getIntent().getExtras());
        next.putExtra(RelaunchActivity.EXTRA_MODE,mode).putExtra(READY,receiver).addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION);
        try {
            activity.startActivity(next);activity.overridePendingTransition(0,0);

            if(Looper.myLooper()!=Looper.getMainLooper())try{ready.await(1500,TimeUnit.MILLISECONDS);}catch(InterruptedException e){Thread.currentThread().interrupt();}


            activity.finish();
            return true;
        }catch(RuntimeException error){android.util.Log.e("BO2","Relaunch request failed",error);return false;}
    }
}
