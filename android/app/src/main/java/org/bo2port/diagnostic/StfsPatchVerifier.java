// SPDX-License-Identifier: GPL-3.0-only


package org.bo2port.diagnostic;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Locale;


public final class StfsPatchVerifier {
    public static final String TU18_MP_SHA256 =
        "5d5780970f6e639b17c0570bae0e52a13bd1038e23c969428aef9d0c7d78681b";
    private static final int BLOCK = 4096, END = 0xFFFFFF, LEVEL1 = 170 * 170;
    private static final long MAX_PATCH = 32L * 1024 * 1024, MAX_IO = 48L * 1024 * 1024;

    public static final class Result {
        public boolean patchFound, matchesKnownTu18;
        public long patchBytes, bytesRead;
        public String patchSha256 = "";
        public XboxContentHeader.Info patchHeader = new XboxContentHeader.Info();
    }

    private static final class Entry {
        int start, allocated;
        long size;
    }

    private final FileChannel channel;
    private final long fileSize, base, totalBlocks, deadline;
    private final int backing, tableCount, tableStart;
    private final boolean readOnly, rootActive;
    private final HashMap<Long, byte[]> tables = new HashMap<>();
    private long bytesRead;

    private StfsPatchVerifier(FileChannel input) throws IOException {
        channel = input;
        deadline = System.nanoTime() + 60_000_000_000L;
        fileSize = channel.size();
        byte[] head = read(0, 0x3AD);
        XboxContentHeader.Info info = XboxContentHeader.parse(head);
        if (!info.kind.equals("xcontent_package") || !info.isBo2Candidate()
                || info.contentType != 0x000B0000 || info.volumeType != 0)
            throw new IOException("Requires a BO2 Installer package with a single-file STFS volume");
        if ((head[0x379] & 255) != 0x24) throw new IOException("Invalid STFS volume descriptor size");
        long headerSize = u32(head, 0x340);
        if (headerSize < 0x3AD || headerSize > fileSize) throw new IOException("Invalid STFS header size");
        base = (headerSize + BLOCK - 1) & ~(long) (BLOCK - 1);
        int flags = head[0x37B] & 255;
        readOnly = (flags & 1) != 0;
        rootActive = (flags & 2) != 0;
        backing = readOnly ? 1 : 2;
        tableCount = (head[0x37C] & 255) | ((head[0x37D] & 255) << 8);
        tableStart = u24(head, 0x37E);
        totalBlocks = u32(head, 0x395);
        if (tableCount < 1 || tableCount > 256 || totalBlocks < 1 || totalBlocks > 0xFFFFFF)
            throw new IOException("STFS table/block counts exceed supported limits");
    }


    public static final String MP_PATCH = "default_mp.xexp", CAMPAIGN_PATCH = "default.xexp";

    public static Result verify(FileChannel input) throws IOException {
        return process(input, null, MP_PATCH);
    }

    public static Result copyPatch(FileChannel input, OutputStream output) throws IOException {
        return copyPatch(input, output, MP_PATCH);
    }

    public static Result copyPatch(FileChannel input, OutputStream output, String patchName) throws IOException {
        if (output == null) throw new IOException("A destination stream is required");
        return process(input, output, patchName);
    }

    public static long patchSize(FileChannel input) throws IOException {
        return patchSize(input, MP_PATCH);
    }

    public static long patchSize(FileChannel input, String patchName) throws IOException {
        Entry target = new StfsPatchVerifier(input).findPatch(patchName);
        if (target == null) throw new IOException(patchName + " not found");
        return target.size;
    }

    private static Result process(FileChannel input, OutputStream output, String patchName) throws IOException {
        StfsPatchVerifier reader = new StfsPatchVerifier(input);
        Entry target = reader.findPatch(patchName);
        Result result = new Result();
        if (target != null) {
            result.patchFound = true;
            result.patchBytes = target.size;
            reader.hashPatch(target, result, output);


            result.matchesKnownTu18 = patchName.equals(MP_PATCH) && TU18_MP_SHA256.equals(result.patchSha256) ||
                patchName.equals(MP_PATCH) &&
                "7f4ee5e4b5ccfe71786ddc073a0363558e89634bb26c2929335aeb7f883c9499".equals(result.patchSha256);
        }
        result.bytesRead = reader.bytesRead;
        return result;
    }

    private Entry findPatch(String patchName) throws IOException {
        if (!patchName.equals(MP_PATCH) && !patchName.equals(CAMPAIGN_PATCH)) throw new IOException("Unsupported patch name");
        int block = tableStart;
        Entry target = null;
        HashSet<Integer> visited = new HashSet<>();
        for (int i = 0; i < tableCount; ++i) {
            checkBlock(block);
            if (!visited.add(block)) throw new IOException("Cycle in STFS directory chain");
            byte[] table = read(dataOffset(block), BLOCK);
            for (int row = 0; row < 64; ++row) {
                int offset = row * 64;
                if (table[offset] == 0) break;
                int flags = table[offset + 0x28] & 255, length = flags & 0x3F;
                if (length < 1 || length > 40) throw new IOException("Invalid STFS filename length");
                if ((flags & 0x80) != 0) continue;
                int parent = ((table[offset + 0x32] & 255) << 8) | (table[offset + 0x33] & 255);
                String name = new String(table, offset, length, StandardCharsets.US_ASCII);
                if (parent != 0xFFFF || !name.equalsIgnoreCase(patchName)) continue;
                if (target != null) throw new IOException("Ambiguous duplicate " + patchName);
                target = new Entry();
                target.start = u24(table, offset + 0x2F);
                target.allocated = u24(table, offset + 0x2C);
                target.size = u32(table, offset + 0x34);
                if (target.size < 24 || target.size > MAX_PATCH)
                    throw new IOException("Patch size is outside the 24-byte to 32-MiB limit");
                if ((target.size + BLOCK - 1) / BLOCK != target.allocated)
                    throw new IOException("Patch block count disagrees with its length");
            }
            if (i + 1 < tableCount) {
                block = nextBlock(block);
                if (block == END) throw new IOException("STFS directory chain ended early");
            }
        }
        return target;
    }

    private void hashPatch(Entry entry, Result result, OutputStream output) throws IOException {
        final MessageDigest digest;
        try { digest = MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException error) { throw new IOException("SHA-256 unavailable", error); }
        ByteArrayOutputStream prefix = new ByteArrayOutputStream(XboxContentHeader.MAX_HEADER_BYTES);
        HashSet<Integer> visited = new HashSet<>();
        int block = entry.start;
        long remaining = entry.size;
        while (remaining > 0) {
            checkBlock(block);
            if (!visited.add(block)) throw new IOException("Cycle in STFS patch chain");
            int count = (int) Math.min(BLOCK, remaining);
            byte[] data = read(dataOffset(block), count);
            digest.update(data);
            if (output != null) output.write(data);
            int keep = Math.min(data.length, XboxContentHeader.MAX_HEADER_BYTES - prefix.size());
            if (keep > 0) prefix.write(data, 0, keep);
            remaining -= count;
            if (remaining > 0) {
                block = nextBlock(block);
                if (block == END) throw new IOException("STFS patch chain ended early");
            }
        }
        StringBuilder hex = new StringBuilder(64);
        for (byte value : digest.digest()) hex.append(String.format(Locale.ROOT, "%02x", value & 255));
        result.patchSha256 = hex.toString();
        result.patchHeader = XboxContentHeader.parse(prefix.toByteArray());
    }

    private void checkBlock(int block) throws IOException {
        if (block < 0 || block >= totalBlocks || block == END)
            throw new IOException("STFS block is outside the declared volume");
    }

    private long dataOffset(int index) throws IOException {
        checkBlock(index);
        long physical = index, group = 170;
        for (int level = 0; level < 3; ++level) {
            physical += ((index + group) / group) * backing;
            if (index < group) break;
            group *= 170;
        }
        return base + physical * BLOCK;
    }

    private long hashOffset(int index, int level) {
        long step0 = 170 + backing, step1 = LEVEL1 + 171L * backing, block;
        if (level == 0) {
            if (index < 170) block = 0;
            else {
                block = (index / 170L) * step0 + (index / (long) LEVEL1 + 1) * backing;
                if (index >= LEVEL1) block += backing;
            }
        } else if (level == 1) {
            block = index < LEVEL1 ? step0 : (index / (long) LEVEL1) * step1 + backing;
        } else block = step1;
        return base + block * BLOCK;
    }

    private byte[] hashTable(long offset) throws IOException {
        byte[] table = tables.get(offset);
        if (table == null) {
            table = read(offset, BLOCK);
            if (tables.size() >= 128) tables.clear();
            tables.put(offset, table);
        }
        return table;
    }

    private int nextBlock(int block) throws IOException {
        checkBlock(block);
        int secondary = !readOnly && rootActive ? BLOCK : 0;
        if (!readOnly && totalBlocks > 170) {
            if (totalBlocks > LEVEL1) {
                byte[] upper = hashTable(hashOffset(block, 2) + secondary);
                int record = (block / LEVEL1) % 170;
                secondary = (upper[record * 24 + 20] & 0x40) != 0 ? BLOCK : 0;
            }
            byte[] upper = hashTable(hashOffset(block, 1) + secondary);
            int record = (block / 170) % 170;
            secondary = (upper[record * 24 + 20] & 0x40) != 0 ? BLOCK : 0;
        }
        byte[] table = hashTable(hashOffset(block, 0) + secondary);
        return (int) (u32(table, (block % 170) * 24 + 20) & 0xFFFFFF);
    }

    private byte[] read(long offset, int count) throws IOException {
        if (offset < 0 || count < 0 || offset > fileSize - count) throw new IOException("Truncated STFS data");
        if (bytesRead + count > MAX_IO || System.nanoTime() > deadline)
            throw new IOException("STFS verification read/time budget exceeded");
        ByteBuffer buffer = ByteBuffer.allocate(count);
        while (buffer.hasRemaining()) {
            int read = channel.read(buffer, offset + buffer.position());
            if (read <= 0) throw new IOException("STFS input ended or made no progress");
        }
        bytesRead += count;
        return buffer.array();
    }

    private static int u24(byte[] data, int offset) {
        return (data[offset] & 255) | ((data[offset + 1] & 255) << 8) | ((data[offset + 2] & 255) << 16);
    }

    private static long u32(byte[] data, int offset) {
        return ((long) (data[offset] & 255) << 24) | ((long) (data[offset + 1] & 255) << 16)
            | ((long) (data[offset + 2] & 255) << 8) | (data[offset + 3] & 255);
    }
}
