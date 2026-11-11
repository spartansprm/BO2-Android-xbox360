// SPDX-License-Identifier: GPL-3.0-only
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.HexFormat;
import org.bo2port.diagnostic.StfsPatchVerifier;

class test_stfs_patch_verifier {
    static final int B = 4096, BASE = 0xA000;
    static int passed;
    static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    static void be(byte[] bytes, int offset, int value) {
        ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN).putInt(offset, value);
    }
    static void le24(byte[] bytes, int offset, int value) {
        bytes[offset] = (byte) value;
        bytes[offset + 1] = (byte) (value >> 8);
        bytes[offset + 2] = (byte) (value >> 16);
    }
    static void write(RandomAccessFile file, long offset, byte[] bytes) throws IOException {
        file.seek(offset); file.write(bytes);
    }
    static long physical(int block, boolean writable) {
        int copies = writable ? 2 : 1;

        require(block < 340, "fixture only supports two L0 groups");
        int physical = block < 170 ? block + copies : block + 3 * copies;
        return BASE + (long) physical * B;
    }
    static byte[] payload(int length) {
        byte[] bytes = new byte[length];
        for (int i = 0; i < length; ++i) bytes[i] = (byte) (i * 31 + 7);
        System.arraycopy("XEX2".getBytes(StandardCharsets.US_ASCII), 0, bytes, 0, 4);
        be(bytes, 20, 1); be(bytes, 24, 0x00040006); be(bytes, 28, 128);
        be(bytes, 128, 0x457AD131); be(bytes, 132, 0x00001200);
        be(bytes, 140, 0x415608C3);
        return bytes;
    }
    static void fixture(RandomAccessFile file, boolean writable, boolean level2, int first, byte[] payload)
            throws Exception {
        int count = (payload.length + B - 1) / B;
        int total = level2 ? 28902 : Math.max(3, first + count);
        long end = level2 ? BASE + (29245L * B) : physical(first + count - 1, writable) + B;
        file.setLength(end);
        byte[] head = new byte[0x6000];
        System.arraycopy("LIVE".getBytes(StandardCharsets.US_ASCII), 0, head, 0, 4);
        be(head, 0x340, 0x971A); be(head, 0x344, 0xB0000);
        be(head, 0x354, 0x457AD131); be(head, 0x360, 0x415608C3);
        head[0x379] = 0x24; head[0x37B] = (byte) (writable ? 2 : 1);
        head[0x37C] = 1; be(head, 0x395, total);
        write(file, 0, head);
        byte[] directory = new byte[B];
        byte[] name = "default_mp.xexp".getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(name, 0, directory, 0, name.length);
        directory[0x28] = (byte) name.length;
        le24(directory, 0x2C, count); le24(directory, 0x2F, first);
        directory[0x32] = directory[0x33] = (byte) 255;
        be(directory, 0x34, payload.length);
        write(file, physical(0, writable), directory);
        byte[] hash0 = new byte[B], hash1 = new byte[B];
        be(hash0, 20, 0xFFFFFF);
        for (int i = 0; i < count; ++i) {
            int block = first + i;
            byte[] hash = block < 170 ? hash0 : hash1;
            be(hash, (block % 170) * 24 + 20, i + 1 < count ? block + 1 : 0xFFFFFF);
            int size = Math.min(B, payload.length - i * B);
            file.seek(physical(block, writable)); file.write(payload, i * B, size);
        }

        write(file, BASE + (writable ? B : 0), hash0);
        if (total > 170) {
            write(file, BASE + (writable ? 174L : 172L) * B, hash1);
            if (writable) {
                byte[] l1 = new byte[B];
                be(l1, 20, 0x40000000);
                write(file, BASE + 173L * B, l1);
            }
        }
        if (level2) {
            byte[] l2 = new byte[B];
            be(l2, 20, 0x40000000);
            write(file, BASE + 29243L * B, l2);
        }
    }
    static void expectError(RandomAccessFile file, String reason) throws Exception {
        try { StfsPatchVerifier.verify(file.getChannel()); throw new AssertionError(reason); }
        catch (IOException expected) { ++passed; }
    }
    public static void main(String[] args) throws Exception {
        Path path = Files.createTempFile("bo2-stfs-synthetic-", ".stfs");
        try (RandomAccessFile file = new RandomAccessFile(path.toFile(), "rw")) {
            for (boolean writable : new boolean[] {false, true}) {
                for (int first : new int[] {1, 169}) {
                    for (boolean level2 : new boolean[] {false, true}) {
                        if (level2 && !writable) continue;
                        file.setLength(0);
                        byte[] payload = payload(9000);
                        fixture(file, writable, level2, first, payload);
                        StfsPatchVerifier.Result result = StfsPatchVerifier.verify(file.getChannel());
                        String expected = HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(payload));
                        require(result.patchFound && result.patchBytes == payload.length, "locate patch/length");
                        require(result.patchSha256.equals(expected), "hash across data/hash table boundaries");
                        require(result.patchHeader.isBo2Candidate(), "XEXP execution info");
                        require(!result.matchesKnownTu18, "synthetic data must never claim TU18");
                        java.io.ByteArrayOutputStream copied = new java.io.ByteArrayOutputStream();
                        StfsPatchVerifier.Result extracted = StfsPatchVerifier.copyPatch(file.getChannel(), copied);
                        require(java.util.Arrays.equals(payload, copied.toByteArray()), "private stream copies exact patch bytes");
                        require(extracted.patchSha256.equals(result.patchSha256), "copy and verify hash agree");
                        require(StfsPatchVerifier.patchSize(file.getChannel()) == payload.length, "patch size before export");
                        ++passed;
                    }
                }
            }
            file.setLength(0); fixture(file, false, false, 1, payload(5000));
            file.seek(BASE + 24 + 20); file.writeInt(1);
            expectError(file, "patch cycle must fail");
            file.setLength(0); fixture(file, false, false, 1, payload(5000));
            file.seek(BASE + 20); file.writeInt(0);
            file.seek(0x37C); file.writeByte(2);
            expectError(file, "directory cycle must fail");
            file.setLength(0); fixture(file, false, false, 1, payload(5000));
            file.setLength(physical(2, false) + 903);
            expectError(file, "truncated data must fail");
            file.setLength(0); fixture(file, false, false, 1, payload(5000));
            file.seek(0x3A9); file.writeInt(1);
            expectError(file, "SVOD must not be treated as STFS");
            file.setLength(0); fixture(file, false, false, 1, payload(5000));
            file.seek(physical(0, false) + 0x28); file.writeByte(63);
            expectError(file, "invalid name length must fail");
            file.setLength(0); fixture(file, false, false, 1, payload(5000));
            file.seek(physical(0, false)); file.writeByte('x');
            require(!StfsPatchVerifier.verify(file.getChannel()).patchFound, "missing patch"); ++passed;

            file.setLength(0); byte[] campaignPayload = payload(7000); fixture(file, false, false, 1, campaignPayload);
            byte[] campaignName = new byte[0x28];
            System.arraycopy("default.xexp".getBytes(StandardCharsets.US_ASCII), 0, campaignName, 0, 12);
            file.seek(physical(0, false)); file.write(campaignName); file.writeByte(12);
            require(!StfsPatchVerifier.verify(file.getChannel()).patchFound, "campaign patch is not the MP patch");
            java.io.ByteArrayOutputStream campaignCopy = new java.io.ByteArrayOutputStream();
            StfsPatchVerifier.Result campaign = StfsPatchVerifier.copyPatch(file.getChannel(), campaignCopy, StfsPatchVerifier.CAMPAIGN_PATCH);
            require(campaign.patchFound && java.util.Arrays.equals(campaignPayload, campaignCopy.toByteArray()), "campaign patch bytes");
            require(!campaign.matchesKnownTu18, "campaign patch has no pinned hash");
            require(StfsPatchVerifier.patchSize(file.getChannel(), StfsPatchVerifier.CAMPAIGN_PATCH) == campaignPayload.length, "campaign patch size");
            ++passed;
            try { StfsPatchVerifier.patchSize(file.getChannel()); throw new AssertionError("MP patch size on a campaign-only package"); }
            catch (IOException expected) { ++passed; }
            try { StfsPatchVerifier.patchSize(file.getChannel(), "other.xexp"); throw new AssertionError("unsupported patch name"); }
            catch (IOException expected) { ++passed; }
        } finally { Files.deleteIfExists(path); }
        System.out.println("STFS verifier: " + passed + " synthetic positive/negative cases passed");
    }
}
