// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.content.Context;
import android.util.AtomicFile;
import org.json.JSONException;
import org.json.JSONObject;
import org.json.JSONArray;
import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;


final class PipelineLibrary {
    private static final int FILE_MAGIC = 0x43505A42;
    private static final int LIST_MAGIC = 0x4C505A42;
    private static final int FILE_VERSION = 1;
    private static final int LIST_VERSION = 1;

    private static final int RECORD_BYTES = 424;
    private static final int MAX_RECORDS = 4096;
    private static final int LIST_HEADER = 16;
    private static final int FILE_HEADER = 24;
    private static final long MAX_CACHE_BYTES = 512L * 1024 * 1024;
    private static final String PREWARM_FOLDER = "cache/prewarm";
    private static final String BUNDLED_ASSET = "pipeline-seed/bo2-prewarm-list.bin";

    private PipelineLibrary() {}

    static File cacheFile(File filesDir) {
        return new File(filesDir, "game-user/cache/bo2zm_native_pipelines.bin");
    }

    private static File prewarmFile(File filesDir, String name) {
        return new File(new File(filesDir, "game-user/" + PREWARM_FOLDER), name);
    }

    private static int uint32(byte[] bytes, int offset) {
        return ByteBuffer.wrap(bytes, offset, 4).order(ByteOrder.LITTLE_ENDIAN).getInt();
    }

    private static long uint64(byte[] bytes, int offset) {
        return ByteBuffer.wrap(bytes, offset, 8).order(ByteOrder.LITTLE_ENDIAN).getLong();
    }

    private static void exact(InputStream input, byte[] bytes, int offset, int length) throws IOException {
        int end = offset + length;
        while (offset < end) {
            int n = input.read(bytes, offset, end - offset);
            if (n < 0) throw new IOException("Archivo truncado");
            if (n == 0) {
                int one = input.read();
                if (one < 0) throw new IOException("Archivo truncado");
                bytes[offset++] = (byte) one;
            } else {
                offset += n;
            }
        }
    }

    private static int validateList(byte[] list) throws IOException {
        if (list.length < LIST_HEADER || uint32(list, 0) != LIST_MAGIC ||
                uint32(list, 4) != LIST_VERSION || uint32(list, 8) != RECORD_BYTES) {
            throw new IOException("Lista BZPL incompatible con el renderizador actual");
        }
        int count = uint32(list, 12);
        if (count <= 0 || count > MAX_RECORDS ||
                list.length != LIST_HEADER + (long) count * RECORD_BYTES) {
            throw new IOException("Cantidad o tamaño de pipelines inválido");
        }

        for (int i = 0; i < count; ++i) {
            int record = LIST_HEADER + i * RECORD_BYTES;
            int attributes = uint32(list, record + 96);
            int bindings = uint32(list, record + 100);
            if (attributes < 0 || attributes > 16 || bindings < 0 || bindings > 16) {
                throw new IOException("Registro de pipeline inválido: " + i);
            }
        }
        return count;
    }

    static byte[] readList(InputStream input) throws IOException {
        byte[] prefix = new byte[FILE_HEADER];
        exact(input, prefix, 0, 4);
        int magic = uint32(prefix, 0);
        if (magic == FILE_MAGIC) {
            exact(input, prefix, 4, FILE_HEADER - 4);
            if (uint32(prefix, 4) != FILE_VERSION) throw new IOException("Versión BZPC incompatible");
            long listBytes = uint64(prefix, 8);
            long cacheBytes = uint64(prefix, 16);
            if (listBytes < LIST_HEADER || listBytes > LIST_HEADER + (long) MAX_RECORDS * RECORD_BYTES ||
                    cacheBytes < 0 || cacheBytes > MAX_CACHE_BYTES) {
                throw new IOException("Dimensiones BZPC inválidas");
            }
            byte[] list = new byte[(int) listBytes];
            exact(input, list, 0, list.length);
            validateList(list);


            return list;
        }
        if (magic != LIST_MAGIC) throw new IOException("Se esperaba BZPL o BZPC");
        exact(input, prefix, 4, LIST_HEADER - 4);
        int count = uint32(prefix, 12);
        int size = uint32(prefix, 8);
        if (size != RECORD_BYTES || count <= 0 || count > MAX_RECORDS) {
            throw new IOException("Cabecera BZPL incompatible");
        }
        byte[] list = new byte[LIST_HEADER + count * RECORD_BYTES];
        System.arraycopy(prefix, 0, list, 0, LIST_HEADER);
        exact(input, list, LIST_HEADER, list.length - LIST_HEADER);
        if (input.read() != -1) throw new IOException("Datos extra después de la lista BZPL");
        validateList(list);
        return list;
    }

    private static void writeAtomic(File target, byte[] bytes) throws IOException {
        File folder = target.getParentFile();
        if (!folder.isDirectory() && !folder.mkdirs()) throw new IOException("No se pudo crear cache/prewarm");
        AtomicFile file = new AtomicFile(target);
        FileOutputStream output = null;
        try {
            output = file.startWrite();
            output.write(bytes);
            file.finishWrite(output);
        } catch (IOException error) {
            if (output != null) file.failWrite(output);
            throw error;
        }
    }

    static int importList(File filesDir, InputStream input) throws IOException {
        byte[] list = readList(input);
        int count = validateList(list);
        writeAtomic(prewarmFile(filesDir, "bo2-imported.bin"), list);
        return count;
    }


    static int installBundledList(Context context) throws IOException {
        File target = prewarmFile(context.getFilesDir(), "bo2-bundled.bin");
        try (InputStream input = context.getAssets().open(BUNDLED_ASSET)) {
            byte[] list = readList(input);
            int count = validateList(list);
            writeAtomic(target, list);
            return count;
        } catch (FileNotFoundException absent) {


            if (target.isFile() && !target.delete())
                throw new IOException("No se pudo retirar la lista de la APK anterior", absent);
            return 0;
        }
    }

    static int verifyCache(File cache) throws IOException {
        if (!cache.isFile() || cache.length() < FILE_HEADER || cache.length() > MAX_CACHE_BYTES) {
            throw new IOException("No existe una caché BZPC exportable");
        }
        try (FileInputStream input = new FileInputStream(cache)) {
            byte[] header = new byte[FILE_HEADER];
            exact(input, header, 0, header.length);
            if (uint32(header, 0) != FILE_MAGIC || uint32(header, 4) != FILE_VERSION) {
                throw new IOException("Cabecera de caché BZPC incompatible");
            }
            long listBytes = uint64(header, 8);
            long driverBytes = uint64(header, 16);
            if (listBytes < LIST_HEADER || listBytes > LIST_HEADER + (long) MAX_RECORDS * RECORD_BYTES ||
                    driverBytes < 0 || FILE_HEADER + listBytes + driverBytes != cache.length()) {
                throw new IOException("Caché BZPC incompleta o con tamaños inválidos");
            }
            byte[] list = new byte[(int) listBytes];
            exact(input, list, 0, list.length);
            return validateList(list);
        }
    }

    static int exportCache(File filesDir, OutputStream destination) throws IOException {
        File source = cacheFile(filesDir);
        int count = verifyCache(source);
        try (FileInputStream input = new FileInputStream(source)) {
            byte[] block = new byte[128 * 1024];
            for (int n; (n = input.read(block)) >= 0;) {
                if (n > 0) destination.write(block, 0, n);
            }
            destination.flush();
        }
        return count;
    }


    private static JSONArray mapPipelineHistory(File filesDir) throws IOException {
        File file = new File(filesDir, "game-user/cache/bo2zm_map_pipeline_v038.jsonl");
        JSONArray events = new JSONArray();
        if (!file.isFile()) return events;
        if (file.length() > 128 * 1024)
            throw new IOException("Historial de pipelines demasiado grande");
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            int rows = 0;
            while ((line = reader.readLine()) != null && rows < 512) {
                if (line.length() > 2048) continue;
                try {
                    JSONObject event = new JSONObject(line);
                    if (event.optInt("schema") == 1) {
                        events.put(event);
                        ++rows;
                    }
                } catch (JSONException ignored) {

                }
            }
        }
        return events;
    }

    static JSONObject inspect(File filesDir) throws JSONException {
        JSONObject result = new JSONObject();
        File cache = cacheFile(filesDir);
        result.put("cache_present", cache.isFile());
        result.put("cache_bytes", cache.isFile() ? cache.length() : 0);
        if (cache.isFile()) {
            try { result.put("cached_pipelines", verifyCache(cache)); }
            catch (IOException error) { result.put("cache_error", error.getMessage()); }
        }
        for (String name : new String[] {"bo2-imported.bin", "bo2-bundled.bin"}) {
            File file = prewarmFile(filesDir, name);
            String key = name.startsWith("bo2-imported") ? "imported" : "bundled";
            result.put(key + "_list_present", file.isFile());
            if (file.isFile()) {
                try (FileInputStream input = new FileInputStream(file)) {
                    result.put(key + "_list_pipelines", validateList(readList(input)));
                } catch (IOException error) {
                    result.put(key + "_list_error", error.getMessage());
                }
            }
        }
        try {
            result.put("map_pipeline_history", mapPipelineHistory(filesDir));
        } catch (IOException error) {
            result.put("map_pipeline_history_error", error.getMessage());
        }
        return result;
    }
}
