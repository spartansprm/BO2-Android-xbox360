// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.widget.Toast;
import org.json.JSONObject;
import java.io.File;


final class GameLaunch {
    static final String PREFS = "bo2-performance";
    static final String GAME_TREE_PREF = "game_tree";
    private GameLaunch() {}

    static SharedPreferences prefs(Context context) { return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE); }


    static Uri savedGameTree(Context context) {
        String value = prefs(context).getString(GAME_TREE_PREF, null);
        if (value == null) return null;
        Uri tree = Uri.parse(value);
        for (android.content.UriPermission permission : context.getContentResolver().getPersistedUriPermissions())
            if (permission.isReadPermission() && tree.equals(permission.getUri())) return tree;
        prefs(context).edit().remove(GAME_TREE_PREF).apply();
        return null;
    }


    static boolean rememberGameTree(Context context, Uri tree) {
        try {
            context.getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException error) {
            return false;
        }
        prefs(context).edit().putString(GAME_TREE_PREF, tree.toString()).apply();
        return true;
    }


    static boolean looksLikeGameFolder(Context context, Uri tree) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        boolean game = false, update = false;
        try (android.database.Cursor cursor = context.getContentResolver().query(children,
                new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE},
                null, null, null)) {
            while (cursor != null && cursor.moveToNext()) {
                String name = cursor.getString(0);
                boolean directory = DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(1));
                if (directory && "00007000".equals(name)) game = true;
                if (directory && "000B0000".equals(name)) update = true;
            }
        } catch (RuntimeException error) {
            return false;
        }
        return game && update;
    }

    static boolean gameProcessRunning(Context context) {
        android.app.ActivityManager manager = context.getSystemService(android.app.ActivityManager.class);
        java.util.List<android.app.ActivityManager.RunningAppProcessInfo> processes =
            manager == null ? null : manager.getRunningAppProcesses();
        if (processes != null) for (android.app.ActivityManager.RunningAppProcessInfo process : processes)
            if ((context.getPackageName() + ":game").equals(process.processName)) return true;
        return false;
    }


    static boolean start(Activity activity, Uri tree, String mode, boolean relaunch) {
        if (!BuildConfig.BO2_GAME_APP_AVAILABLE) return false;
        if (gameProcessRunning(activity)) {
            Toast.makeText(activity, "El juego aún se está cerrando. Espera un momento.", Toast.LENGTH_LONG).show();
            return false;
        }
        String run = java.util.UUID.randomUUID().toString();
        try {
            File oldLog = new File(activity.getFilesDir(), "game-sdk.log");
            if (oldLog.exists() && !oldLog.delete()) throw new java.io.IOException("Cannot clear previous game log");
            GameActivity.writeCheckpoint(activity.getFilesDir(), new JSONObject().put("app_version", BuildConfig.VERSION_NAME)
                .put("run_id", run).put("status", "starting").put("phase", "game_process_requested").put("game_entry_executed", false));
        } catch (Exception error) {
            Toast.makeText(activity, "No se pudo guardar el checkpoint de arranque.", Toast.LENGTH_LONG).show();
            return false;
        }
        activity.startActivity(intent(activity, tree, mode, relaunch, run));
        return true;
    }

    static Intent intent(Context context, Uri tree, String mode, boolean relaunch, String run) {
        SharedPreferences p = prefs(context);
        return TouchSettings.addTo(new Intent(context, GameActivity.class), p)
            .putExtra("game_tree", tree.toString()).putExtra("run_id", run)
            .putExtra("game_mode", mode).putExtra("relaunch", relaunch)
            .putExtra("gpu_category_marks", false)
            .putExtra("draw_batch", true)
            .putExtra("gpu_pass_profile", false)
            .putExtra("transfer_dst_trial", false)
            .putExtra("disable_scene_pass_dependencies", false)
            .putExtra("scene_pass_sync_mode", 0)
            .putExtra("disable_scene_transfer_barriers", false)
            .putExtra("disable_scene_output_read_barrier", false)
            .putExtra("disable_async_pipelines", false)
            .putExtra("bo2zm_native_scene_sync_precise", true)
            .putExtra("bo2zm_native_scene_sync_mask", 95)
            .putExtra("bo2zm_native_deferred_clears", true)
            .putExtra("front_no_copy", false)
            .putExtra("bo2zm_native_scene_scale", 100)
            .putExtra("use_imported_library", p.getBoolean("use_imported_library", true))
            .putExtra(SceneResolution.PREF, SceneResolution.valid(p.getString(SceneResolution.PREF, SceneResolution.VALUES[0])))
            .putExtra(DisplayOptions.OUTPUT_PREF, DisplayOptions.validOutput(p.getString(DisplayOptions.OUTPUT_PREF, DisplayOptions.OUTPUTS[0])))
            .putExtra(DisplayOptions.SCREEN_PREF, DisplayOptions.validScreen(p.getString(DisplayOptions.SCREEN_PREF, DisplayOptions.SCREENS[0])))
            .putExtra(Sharpness.EFFECT_PREF, Sharpness.validEffect(p.getString(Sharpness.EFFECT_PREF, Sharpness.EFFECT_DEFAULT)))
            .putExtra(Sharpness.STRENGTH_PREF, Sharpness.clamp(p.getInt(Sharpness.STRENGTH_PREF, Sharpness.STRENGTH_DEFAULT)))
            .putExtra(Sharpness.TEXTURE_PREF, Sharpness.validTexture(p.getString(Sharpness.TEXTURE_PREF, Sharpness.TEXTURE_DEFAULT)))
            .putExtra(Sharpness.ANISO_PREF, Sharpness.validAniso(p.getString(Sharpness.ANISO_PREF, Sharpness.ANISO_DEFAULT)))
            .putExtra(Sharpness.PRESET_PREF, Sharpness.validPreset(p.getString(Sharpness.PRESET_PREF, Sharpness.PRESET_DEFAULT)));
    }
}
