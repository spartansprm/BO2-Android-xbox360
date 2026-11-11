// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.zip.Deflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;


public final class InputPreparer {
    private static final long MAX_ZIP_BYTES = 32L * 1024 * 1024;
    private static final String IMAGE_SHA256 = "ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e";
    private static final class Doc {
        String id, name, mime;
        long size;
        boolean directory() { return DocumentsContract.Document.MIME_TYPE_DIR.equals(mime); }
    }
    private static final class Package {
        Doc file;
        byte[] header;
        XboxContentHeader.Info info;
    }
    private InputPreparer() {}


    public static final class Program {
        public final String stem, mode;
        final long imageBytes, maxZipBytes;
        final String imageSha256;
        private Program(String stem, String mode, long imageBytes, String imageSha256, long maxZipBytes) {
            this.stem = stem; this.mode = mode; this.imageBytes = imageBytes; this.imageSha256 = imageSha256;
            this.maxZipBytes = maxZipBytes;
        }
        public String xex() { return stem + ".xex"; }
        public String xexp() { return stem + ".xexp"; }
        public boolean campaign() { return mode.equals("cp"); }
    }
    public static final Program MULTIPLAYER = new Program("default_mp", "mp", 47054848, IMAGE_SHA256, MAX_ZIP_BYTES);


    public static final Program CAMPAIGN = new Program("default", "cp", 38731776, null, 64L * 1024 * 1024);
    public static Program program(String mode) { return "cp".equals(mode) ? CAMPAIGN : MULTIPLAYER; }

    public static JSONObject prepare(ContentResolver resolver, Uri tree, File output) throws Exception {
        return prepare(resolver, tree, output, MULTIPLAYER);
    }

    public static JSONObject prepare(ContentResolver resolver, Uri tree, File output, Program program) throws Exception {
        Selection selected = select(resolver, tree);
        Package game = selected.game, update = selected.update;
        ArrayList<Doc> fragments = selected.fragments;
        JSONObject manifest = new JSONObject();
        manifest.put("schema", 1);
        manifest.put("private_game_inputs", true);
        manifest.put("scope", "Executable inputs for local compilation");
        manifest.put("program", program.stem);
        manifest.put("expected_patched_image_sha256", program.imageSha256 == null ? JSONObject.NULL : program.imageSha256);
        manifest.put("expected_patched_image_bytes", program.imageBytes);
        manifest.put("patched_image_verified", false);
        manifest.put("god_header_file", game.file.name);
        manifest.put("update_package_file", update.file.name);
        manifest.put("game_package", metadata(game.info));
        manifest.put("update_package", metadata(update.info));
        JSONArray fragmentList = new JSONArray();
        for (Doc doc : fragments) fragmentList.put(new JSONObject().put("name", doc.name).put("bytes", doc.size));
        manifest.put("fragments", fragmentList);
        try (FragmentSource source = new FragmentSource(resolver, tree, fragments);
             ParcelFileDescriptor.AutoCloseInputStream patch = open(resolver, uri(tree, update.file.id))) {
            SvodReader svod = new SvodReader(game.header, source);
            SvodReader.Executable exe = svod.findExecutable(program.xex());
            long patchBytes = StfsPatchVerifier.patchSize(patch.getChannel(), program.xexp());
            if (exe.bytes + patchBytes > program.maxZipBytes - 65536)
                throw new IOException("The two build inputs exceed the private ZIP limit");
            try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(output))) {
                zip.setLevel(Deflater.BEST_SPEED);
                zip.putNextEntry(new ZipEntry("game/" + program.xex()));
                SvodReader.Result base = svod.copyExecutable(exe, zip);
                zip.closeEntry();
                zip.putNextEntry(new ZipEntry("game/" + program.xexp()));
                StfsPatchVerifier.Result delta = StfsPatchVerifier.copyPatch(patch.getChannel(), zip, program.xexp());
                if (!delta.patchFound) throw new IOException("The update contains no " + program.xexp());
                zip.closeEntry();
                manifest.put(program.stem + "_xex", new JSONObject().put("bytes", base.bytes).put("sha256", base.sha256)
                    .put("metadata", metadata(base.header)));
                manifest.put(program.stem + "_xexp", new JSONObject().put("bytes", delta.patchBytes).put("sha256", delta.patchSha256)
                    .put("matches_known_tu18_patch", delta.matchesKnownTu18).put("metadata", metadata(delta.patchHeader)));
                zip.putNextEntry(new ZipEntry("manifest.json"));
                zip.write(manifest.toString(2).getBytes(StandardCharsets.UTF_8));
                zip.closeEntry();
            }
        }
        if (output.length() > program.maxZipBytes) throw new IOException("Prepared ZIP exceeds its size limit");
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (FileInputStream input = new FileInputStream(output)) {
            byte[] buffer = new byte[128 * 1024];
            for (int count; (count = input.read(buffer)) != -1;) digest.update(buffer, 0, count);
        }
        StringBuilder sha = new StringBuilder(64);
        for (byte b : digest.digest()) sha.append(String.format(Locale.ROOT, "%02x", b & 255));
        return new JSONObject().put("status", "ready").put("archive_bytes", output.length())
            .put("archive_sha256", sha.toString()).put("manifest", manifest);
    }

    private static final class Selection {
        Package game, update;
        ArrayList<Doc> fragments;
    }
    private static Selection select(ContentResolver resolver, Uri tree) throws Exception {
        ArrayList<Doc> root = list(resolver, tree, DocumentsContract.getTreeDocumentId(tree));
        Doc gameDir = namedDirectory(root, "00007000"), updateDir = namedDirectory(root, "000B0000");
        if (gameDir == null || updateDir == null)
            throw new IOException("Select the parent folder containing 00007000 and 000B0000");
        ArrayList<Doc> gameFiles = list(resolver, tree, gameDir.id);
        Package game = choosePackage(resolver, tree, gameFiles, 0x7000, -1);
        Package update = choosePackage(resolver, tree, list(resolver, tree, updateDir.id), 0xB0000, game.info.mediaId);
        Doc data = namedDirectory(gameFiles, game.file.name + ".data");
        if (data == null) throw new IOException("The GOD header's matching .data directory is missing");
        ArrayList<Doc> fragments = new ArrayList<>();
        for (Doc doc : list(resolver, tree, data.id))
            if (!doc.directory() && doc.name.matches("(?i)Data[0-9A-F]{4,}")) fragments.add(doc);
        fragments.sort(Comparator.comparing(doc -> doc.name.toUpperCase(Locale.ROOT)));
        if (fragments.size() != game.info.dataFiles) throw new IOException("Fragment count differs from the GOD header");
        boolean decimal = true, hexadecimal = true;
        HashSet<String> names = new HashSet<>();
        for (int i = 0; i < fragments.size(); ++i) {
            String name = fragments.get(i).name;
            if (!names.add(name.toUpperCase(Locale.ROOT))) throw new IOException("Duplicate fragment name");
            decimal &= name.equalsIgnoreCase(String.format(Locale.ROOT, "Data%04d", i));
            hexadecimal &= name.equalsIgnoreCase(String.format(Locale.ROOT, "Data%04X", i));
        }
        if (!decimal && !hexadecimal) throw new IOException("Fragment names are not a contiguous standard sequence");
        Selection result = new Selection(); result.game=game; result.update=update; result.fragments=fragments;
        return result;
    }


    public static final class ResourceSet implements AutoCloseable {
        public int gameHeaderFd, updateFd;
        public int[] fragments;
        public int[] dlcDescriptors=new int[0];
        public String[] dlcNames=new String[0];
        public JSONObject metadata;
        private final ArrayList<ParcelFileDescriptor> handles = new ArrayList<>();
        private int add(ContentResolver resolver, Uri uri) throws IOException {
            ParcelFileDescriptor descriptor = resolver.openFileDescriptor(uri, "r");
            if (descriptor == null) throw new IOException("Cannot open read-only resource descriptor");
            handles.add(descriptor); return descriptor.getFd();
        }
        public void close() throws IOException {
            IOException failure=null;
            for (ParcelFileDescriptor handle:handles) try { handle.close(); } catch(IOException error) {failure=error;}
            handles.clear(); if(failure!=null) throw failure;
        }
    }
    public static ResourceSet openResources(ContentResolver resolver, Uri tree) throws Exception {
        Selection selected=select(resolver,tree); ResourceSet result=new ResourceSet();
        try {
            result.gameHeaderFd=result.add(resolver,uri(tree,selected.game.file.id));
            result.updateFd=result.add(resolver,uri(tree,selected.update.file.id));
            result.fragments=new int[selected.fragments.size()];
            for(int i=0;i<result.fragments.length;++i)
                result.fragments[i]=result.add(resolver,uri(tree,selected.fragments.get(i).id));
            ArrayList<Integer> dlcFds=new ArrayList<>();ArrayList<String> dlcNames=new ArrayList<>();
            Doc dlcDir=namedDirectory(list(resolver,tree,DocumentsContract.getTreeDocumentId(tree)),"00000002");
            if(dlcDir!=null)for(Doc doc:list(resolver,tree,dlcDir.id)) {
                if(doc.directory()||doc.size>=0&&doc.size<0x3AD)continue;
                XboxContentHeader.Info info;
                try(ParcelFileDescriptor.AutoCloseInputStream stream=open(resolver,uri(tree,doc.id))) {
                    info=XboxContentHeader.parse(read(stream.getChannel(),0,(int)Math.min(XboxContentHeader.MAX_HEADER_BYTES,stream.getChannel().size())));
                }
                if(!info.isBo2Candidate()||info.contentType!=2||info.volumeType!=0)continue;
                if(dlcFds.size()>=32)throw new IOException("Too many BO2 DLC packages (maximum 32)");
                dlcFds.add(result.add(resolver,uri(tree,doc.id)));dlcNames.add(doc.name);
            }
            result.dlcDescriptors=new int[dlcFds.size()];for(int i=0;i<dlcFds.size();++i)result.dlcDescriptors[i]=dlcFds.get(i);
            result.dlcNames=dlcNames.toArray(new String[0]);
            result.metadata=new JSONObject().put("game",metadata(selected.game.info))
                .put("update",metadata(selected.update.info)).put("fragments",result.fragments.length).put("dlc_packages",result.dlcDescriptors.length);
            return result;
        } catch(Exception error) {try {result.close();} catch(IOException ignored) {} throw error;}
    }

    private static JSONObject metadata(XboxContentHeader.Info info) throws Exception {
        return new JSONObject().put("kind", info.kind).put("title_id", XboxContentHeader.hex(info.titleId))
            .put("media_id", XboxContentHeader.hex(info.mediaId)).put("version_raw", XboxContentHeader.hex(info.version))
            .put("base_version_raw", XboxContentHeader.hex(info.baseVersion)).put("module_flags", XboxContentHeader.hex(info.moduleFlags))
            .put("delta_metadata_present", info.deltaMetadataPresent)
            .put("delta_source_version_raw", XboxContentHeader.hex(info.deltaSourceVersion))
            .put("delta_target_version_raw", XboxContentHeader.hex(info.deltaTargetVersion));
    }

    private static Package choosePackage(ContentResolver resolver, Uri tree, ArrayList<Doc> docs,
            long type, long media) throws Exception {
        Package selected = null;
        for (Doc doc : docs) {
            if (doc.directory() || (doc.size >= 0 && doc.size < 0x3AD)) continue;
            byte[] prefix;
            try (ParcelFileDescriptor.AutoCloseInputStream input = open(resolver, uri(tree, doc.id))) {
                int length = (int) Math.min(XboxContentHeader.MAX_HEADER_BYTES, input.getChannel().size());
                prefix = read(input.getChannel(), 0, length);
            }
            XboxContentHeader.Info info = XboxContentHeader.parse(prefix);
            if (!info.isBo2Candidate() || info.contentType != type || (media >= 0 && info.mediaId != media)) continue;
            if (selected != null) throw new IOException("Multiple matching packages: preparation needs one base game and one update");
            selected = new Package(); selected.file = doc; selected.header = prefix; selected.info = info;
        }
        if (selected == null) throw new IOException("No matching BO2 package found in the selected folders");
        return selected;
    }

    private static Doc namedDirectory(ArrayList<Doc> docs, String name) {
        for (Doc doc : docs) if (doc.directory() && doc.name.equalsIgnoreCase(name)) return doc;
        return null;
    }
    private static Uri uri(Uri tree, String id) { return DocumentsContract.buildDocumentUriUsingTree(tree, id); }
    private static ArrayList<Doc> list(ContentResolver resolver, Uri tree, String id) throws Exception {
        String[] fields = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        ArrayList<Doc> out = new ArrayList<>();
        try (Cursor cursor = resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, id), fields, null, null, null)) {
            if (cursor == null) throw new IOException("Cannot list selected directory");
            while (cursor.moveToNext()) {
                if (out.size() >= 512) throw new IOException("Too many directory entries for input preparation");
                Doc doc = new Doc(); doc.id = cursor.getString(0); doc.name = cursor.getString(1);
                doc.mime = cursor.getString(2); doc.size = cursor.isNull(3) ? -1 : cursor.getLong(3);
                if (doc.name == null) throw new IOException("Document provider returned an unnamed entry");
                out.add(doc);
            }
        }
        return out;
    }
    private static ParcelFileDescriptor.AutoCloseInputStream open(ContentResolver resolver, Uri uri) throws IOException {
        ParcelFileDescriptor descriptor = resolver.openFileDescriptor(uri, "r");
        if (descriptor == null) throw new IOException("No read-only descriptor");
        return new ParcelFileDescriptor.AutoCloseInputStream(descriptor);
    }
    private static byte[] read(FileChannel channel, long offset, int count) throws IOException {
        if (offset < 0 || count < 0 || offset > channel.size() - count) throw new IOException("Truncated fragment or package");
        ByteBuffer buffer = ByteBuffer.allocate(count);
        while (buffer.hasRemaining()) if (channel.read(buffer, offset + buffer.position()) <= 0)
            throw new IOException("Input ended or made no progress");
        return buffer.array();
    }

    private static final class FragmentSource implements SvodReader.Fragments, AutoCloseable {
        final ContentResolver resolver;
        final Uri tree;
        final ArrayList<Doc> docs;
        final LinkedHashMap<Integer, ParcelFileDescriptor.AutoCloseInputStream> open = new LinkedHashMap<>(4, 0.75f, true);
        FragmentSource(ContentResolver resolver, Uri tree, ArrayList<Doc> docs) { this.resolver=resolver; this.tree=tree; this.docs=docs; }
        public int count() { return docs.size(); }
        public byte[] read(int index, long offset, int count) throws IOException {
            if (index < 0 || index >= docs.size()) throw new IOException("Missing SVOD fragment");
            ParcelFileDescriptor.AutoCloseInputStream stream = open.get(index);
            if (stream == null) {
                if (open.size() >= 4) {
                    Iterator<Map.Entry<Integer, ParcelFileDescriptor.AutoCloseInputStream>> oldest = open.entrySet().iterator();
                    Map.Entry<Integer, ParcelFileDescriptor.AutoCloseInputStream> item = oldest.next();
                    item.getValue().close(); oldest.remove();
                }
                stream = InputPreparer.open(resolver, uri(tree, docs.get(index).id)); open.put(index, stream);
            }
            return InputPreparer.read(stream.getChannel(), offset, count);
        }
        public void close() throws IOException {
            IOException failure = null;
            for (ParcelFileDescriptor.AutoCloseInputStream stream : open.values())
                try { stream.close(); } catch (IOException error) { failure=error; }
            open.clear();
            if (failure != null) throw failure;
        }
    }
}
