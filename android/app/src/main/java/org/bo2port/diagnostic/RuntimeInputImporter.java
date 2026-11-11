// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.content.ContentResolver;
import android.net.Uri;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.HashSet;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;


final class RuntimeInputImporter {
    private static final long MAX_BYTES = 32L * 1024 * 1024;
    private static final String BASE = BuildConfig.BO2_MP_XEX_SHA256;
    private static final String PATCH = BuildConfig.BO2_MP_XEXP_SHA256;
    static final class Result {
        final File directory;
        final JSONObject metadata;
        Result(File directory, JSONObject metadata) { this.directory = directory; this.metadata = metadata; }
    }
    static Result read(ContentResolver resolver, Uri uri, File privateRoot) throws Exception {
        return read(resolver, uri, privateRoot, InputPreparer.MULTIPLAYER);
    }


    static Result read(ContentResolver resolver, Uri uri, File privateRoot, InputPreparer.Program program) throws Exception {
        final String stem = program.stem;
        final long maxBytes = program.campaign() ? 64L * 1024 * 1024 : MAX_BYTES;
        File directory = File.createTempFile("runtime-inputs-", "", privateRoot);
        if (!directory.delete() || !directory.mkdir()) throw new IOException("Cannot create private input directory");
        boolean finished = false;
        JSONObject inputs = new JSONObject(), manifest = null;
        HashSet<String> names = new HashSet<>();
        long total = 0;
        try (InputStream input = resolver.openInputStream(uri)) {
            if (input == null) throw new IOException("Cannot open selected ZIP");
            try (ZipInputStream zip = new ZipInputStream(input)) {
                byte[] buffer = new byte[128 * 1024];
                for (ZipEntry entry; (entry = zip.getNextEntry()) != null;) {
                    String name = entry.getName();
                    if (entry.isDirectory() || !names.add(name) ||
                        !(name.equals("game/" + stem + ".xex") || name.equals("game/" + stem + ".xexp") || name.equals("manifest.json")))
                        throw new IOException("Unexpected or duplicated entry in private ZIP");
                    if (name.equals("manifest.json")) {
                        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                        for (int count; (count = zip.read(buffer)) != -1;) {
                            total += count;
                            if (bytes.size() + count > 65536 || total > maxBytes) throw new IOException("Manifest or ZIP too large");
                            bytes.write(buffer, 0, count);
                        }
                        manifest = new JSONObject(bytes.toString(StandardCharsets.UTF_8.name()));
                    } else {
                        String extension = name.endsWith(".xexp") ? "xexp" : "xex";
                        MessageDigest hash = MessageDigest.getInstance("SHA-256");
                        long size = 0;
                        try (FileOutputStream output = new FileOutputStream(new File(directory, stem + "." + extension))) {
                            for (int count; (count = zip.read(buffer)) != -1;) {
                                total += count; size += count;
                                if (total > maxBytes) throw new IOException("Private ZIP expands beyond its size limit");
                                hash.update(buffer, 0, count); output.write(buffer, 0, count);
                            }
                        }
                        StringBuilder sha = new StringBuilder(64);
                        for (byte b : hash.digest()) sha.append(String.format(Locale.ROOT, "%02x", b & 255));
                        String expected = extension.equals("xex") ? BASE : PATCH;
                        if (!program.campaign() && !expected.equals(sha.toString()))
                            throw new IOException("Input hash differs from this verified private build: " + extension);
                        inputs.put(stem + "_" + extension, new JSONObject().put("bytes", size).put("sha256", sha.toString()));
                    }
                    zip.closeEntry();
                }
            }
            if (names.size() != 3 || manifest == null || manifest.optInt("schema") != 1 || !manifest.optBoolean("private_game_inputs"))
                throw new IOException("Incomplete or unknown private ZIP");
            for (String extension : new String[] {"xex", "xexp"}) {
                JSONObject actual = inputs.getJSONObject(stem + "_" + extension), declared = manifest.getJSONObject(stem + "_" + extension);
                if (actual.getLong("bytes") != declared.getLong("bytes") || !actual.getString("sha256").equals(declared.getString("sha256")))
                    throw new IOException("ZIP inputs differ from their manifest");
            }
            finished = true;
            return new Result(directory, inputs);
        } finally {
            if (!finished) remove(directory);
        }
    }
    static void remove(File directory) {
        new File(directory, "default_mp.xex").delete();
        new File(directory, "default_mp.xexp").delete();
        new File(directory, "default.xex").delete();
        new File(directory, "default.xexp").delete();
        directory.delete();
    }
}
