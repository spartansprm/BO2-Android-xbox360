// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ResultReceiver;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.Consumer;


public final class TouchSettingsStore extends BroadcastReceiver {
    private static final String ACK = "touch_settings_ack";
    private static final ExecutorService WRITER = Executors.newSingleThreadExecutor();

    static void save(Context context, String layout, Consumer<Boolean> complete) {
        Handler main = new Handler(Looper.getMainLooper());
        boolean[] pending = {true};
        Runnable timeout = () -> { if (pending[0]) { pending[0]=false; complete.accept(false); } };
        ResultReceiver result = new ResultReceiver(main) {
            @Override protected void onReceiveResult(int code, android.os.Bundle data) {
                if (!pending[0]) return;
                pending[0]=false; main.removeCallbacks(timeout); complete.accept(code==Activity.RESULT_OK);
            }
        };
        main.postDelayed(timeout,5000);
        try {
            context.sendBroadcast(new Intent(context,TouchSettingsStore.class)
                .putExtra(TouchSettings.LAYOUT,layout).putExtra(ACK,result));
        } catch (RuntimeException failure) {
            pending[0]=false; main.removeCallbacks(timeout); complete.accept(false);
        }
    }

    @Override public void onReceive(Context context, Intent intent) {
        ResultReceiver result = Build.VERSION.SDK_INT>=33 ? intent.getParcelableExtra(ACK,ResultReceiver.class)
            : intent.getParcelableExtra(ACK);
        String layout=intent.getStringExtra(TouchSettings.LAYOUT);
        PendingResult pending=goAsync();
        WRITER.execute(() -> {
            boolean saved=false;
            try {
                TouchController validated=new TouchController();
                if (TouchSettings.readLayout(validated,layout))
                    saved=GameLaunch.prefs(context).edit().putString(TouchSettings.LAYOUT,
                        TouchSettings.encodeLayout(validated)).commit();
            } catch (RuntimeException ignored) {

            } finally {
                if (result!=null) result.send(saved?Activity.RESULT_OK:Activity.RESULT_CANCELED,null);
                pending.finish();
            }
        });
    }
}
