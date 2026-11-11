// SPDX-License-Identifier: GPL-3.0-only

package org.bo2port.diagnostic;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Locale;


public final class SvodReader {
    public interface Fragments {
        int count();
        byte[] read(int fragment, long offset, int bytes) throws IOException;
    }
    public static final class Executable {
        public long block, bytes;
        public String name;
    }
    public static final class Result {
        public long bytes;
        public String sha256;
        public XboxContentHeader.Info header;
    }
    public static final class Address {
        public final int fragment;
        public final long offset;
        Address(int fragment, long offset) { this.fragment = fragment; this.offset = offset; }
    }
    private static final int SECTOR = 0x800;

    static final long MAX_EXECUTABLE = 48L * 1024 * 1024;
    private static final long BLOCKS_PER_FILE = 0x14388, FILE_BYTES = 0xA290000;
    private static final byte[] MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
    private final Fragments fragments;
    private final boolean enhanced;
    private final long blockOffset, deadline;
    private long ioBytes, rootBlock, rootBytes;

    public SvodReader(byte[] packageHeader, Fragments input) throws IOException {
        XboxContentHeader.Info info = XboxContentHeader.parse(packageHeader);
        if (!info.isBo2Candidate() || info.contentType != 0x7000 || info.volumeType != 1
                || info.dataFiles <= 1 || info.dataFiles != input.count())
            throw new IOException("Requires matching multipart BO2 GOD header and all data fragments");
        if ((packageHeader[0x379] & 255) != 0x24) throw new IOException("Invalid SVOD descriptor size");
        fragments = input;
        enhanced = (packageHeader[0x391] & 0x40) != 0;
        blockOffset = u24(packageHeader, 0x395) * 2L;
        deadline = System.nanoTime() + 120_000_000_000L;
        long magicOffset = enhanced ? 0x2000 : 0x12000;
        if (!Arrays.equals(read(0, magicOffset, 20), MAGIC))
            throw new IOException("SVOD media marker not found at the SDK's expected layout offset");
        byte[] root = read(0, magicOffset + 20, 16);
        rootBlock = u32(root, 0);
        rootBytes = u32(root, 4);
        if (rootBytes < 14 || rootBytes > 1024 * 1024)
            throw new IOException("SVOD root directory size outside supported limits");
    }

    public Executable findExecutable() throws IOException {
        return findExecutable("default_mp.xex");
    }


    public Executable findExecutable(String fileName) throws IOException {
        if (!fileName.equals("default_mp.xex") && !fileName.equals("default.xex"))
            throw new IOException("Unsupported executable name");
        ArrayDeque<Integer> pending = new ArrayDeque<>();
        HashSet<Integer> visited = new HashSet<>();
        pending.push(0);
        Executable result = null;
        while (!pending.isEmpty()) {
            int ordinal = pending.pop();
            if (!visited.add(ordinal) || visited.size() > 16384)
                throw new IOException("Cyclic or oversized SVOD directory tree");
            long offset = ordinal * 4L;
            if (offset > rootBytes - 14) throw new IOException("SVOD directory entry outside root extent");
            byte[] record = range(rootBlock, offset, 14);
            int left = u16(record, 0), right = u16(record, 2);
            int nameLength = record[13] & 255;
            if (nameLength == 0 || offset + 14 + nameLength > rootBytes)
                throw new IOException("Invalid SVOD filename extent");
            String name = new String(range(rootBlock, offset + 14, nameLength), StandardCharsets.US_ASCII);
            if (left != 0) pending.push(left);
            if (right != 0) pending.push(right);
            if (!name.equalsIgnoreCase(fileName)) continue;
            if ((record[12] & 0x10) != 0) throw new IOException("Executable entry is a directory");
            if (result != null) throw new IOException("Duplicate " + fileName + " entries");
            result = new Executable();
            result.name = name;
            result.block = u32(record, 4);
            result.bytes = u32(record, 8);
            if (result.bytes < 24 || result.bytes > MAX_EXECUTABLE)
                throw new IOException("Executable size exceeds the 24-byte to 48-MiB preparation limit");
        }
        if (result == null) throw new IOException(fileName + " not found in the SVOD root directory");
        return result;
    }

    public Result copyExecutable(Executable entry, OutputStream output) throws IOException {
        MessageDigest digest;
        try { digest = MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException error) { throw new IOException(error); }
        ByteArrayOutputStream prefix = new ByteArrayOutputStream(XboxContentHeader.MAX_HEADER_BYTES);
        long block = entry.block, remaining = entry.bytes;
        while (remaining > 0) {
            Address first = address(block);
            int bytes = (int) Math.min(SECTOR, remaining);
            long sectors = 1;

            while (bytes < remaining && bytes + SECTOR <= 128 * 1024) {
                Address next = address(block + sectors);
                if (next.fragment != first.fragment || next.offset != first.offset + sectors * SECTOR) break;
                bytes += (int) Math.min(SECTOR, remaining - bytes);
                ++sectors;
            }
            byte[] data = read(first.fragment, first.offset, bytes);
            digest.update(data);
            output.write(data);
            int keep = Math.min(data.length, XboxContentHeader.MAX_HEADER_BYTES - prefix.size());
            if (keep > 0) prefix.write(data, 0, keep);
            remaining -= bytes;
            block += sectors;
        }
        Result result = new Result();
        result.bytes = entry.bytes;
        result.header = XboxContentHeader.parse(prefix.toByteArray());
        if (!result.header.kind.equals("xex2") || (result.header.metadataPresent && !result.header.isBo2Candidate()))
            throw new IOException("Extracted " + entry.name + " is not a BO2 XEX2 candidate");
        StringBuilder hex = new StringBuilder(64);
        for (byte value : digest.digest()) hex.append(String.format(Locale.ROOT, "%02x", value & 255));
        result.sha256 = hex.toString();
        return result;
    }

    public Address address(long block) throws IOException {
        long actual = block - blockOffset + (enhanced ? 2 : 0);
        if (actual < 0) throw new IOException("SVOD block precedes the declared data start");
        long inFile = actual % BLOCKS_PER_FILE, file = actual / BLOCKS_PER_FILE;
        long l0 = inFile / 0x198 + 1, l1 = l0 / 0xA1C4 + 1;
        long offset = inFile * SECTOR + (l0 + l1) * 0x1000;
        if (offset >= FILE_BYTES) { ++file; offset = offset % FILE_BYTES + 0x2000; }
        if (file >= fragments.count()) throw new IOException("SVOD block references a missing fragment");
        return new Address((int) file, offset);
    }

    private byte[] range(long block, long offset, int count) throws IOException {
        ByteArrayOutputStream result = new ByteArrayOutputStream(count);
        while (count > 0) {
            Address address = address(block + offset / SECTOR);
            int within = (int) (offset % SECTOR), take = Math.min(count, SECTOR - within);
            result.write(read(address.fragment, address.offset + within, take), 0, take);
            offset += take; count -= take;
        }
        return result.toByteArray();
    }

    private byte[] read(int fragment, long offset, int count) throws IOException {
        if (ioBytes + count > 64L * 1024 * 1024 || System.nanoTime() > deadline)
            throw new IOException("SVOD preparation read/time budget exceeded");
        byte[] data = fragments.read(fragment, offset, count);
        if (data.length != count) throw new IOException("Truncated SVOD read");
        ioBytes += count;
        return data;
    }
    private static int u16(byte[] data, int at) { return (data[at] & 255) | ((data[at + 1] & 255) << 8); }
    private static int u24(byte[] data, int at) { return u16(data, at) | ((data[at + 2] & 255) << 16); }
    private static long u32(byte[] data, int at) {
        return (data[at] & 255L) | ((data[at + 1] & 255L) << 8)
            | ((data[at + 2] & 255L) << 16) | ((data[at + 3] & 255L) << 24);
    }
}
