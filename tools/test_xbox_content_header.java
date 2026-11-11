// SPDX-License-Identifier: GPL-3.0-only
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import org.bo2port.diagnostic.XboxContentHeader;

class test_xbox_content_header {
    static void require(boolean condition, String detail) {
        if (!condition) throw new AssertionError(detail);
    }
    static void put(byte[] bytes, int offset, int value) {
        ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN).putInt(offset, value);
    }
    public static void main(String[] args) {
        for (String magic : new String[] {"LIVE", "PIRS", "CON "}) {
            byte[] bytes = new byte[0x6000];
            System.arraycopy(magic.getBytes(StandardCharsets.US_ASCII), 0, bytes, 0, 4);
            put(bytes, 0x344, 0x7000);
            put(bytes, 0x354, 0x89ABCDEF);
            put(bytes, 0x358, 0x12345678);
            put(bytes, 0x35C, 0x12340000);
            put(bytes, 0x360, 0x415608C3);
            put(bytes, 0x39D, 42);
            put(bytes, 0x3A9, 1);
            byte[] title = "BO2 synthetic header".getBytes(StandardCharsets.UTF_16BE);
            System.arraycopy(title, 0, bytes, 0x411, title.length);
            XboxContentHeader.Info info = XboxContentHeader.parse(bytes);
            require(info.kind.equals("xcontent_package") && info.metadataPresent, "package recognition");
            require(info.isBo2Candidate() && info.mediaId == 0x89ABCDEFL, "execution info/endian");
            require(info.dataFiles == 42 && info.volumeType == 1, "SVOD metadata offsets");
            require(info.version == 0x12345678 && info.baseVersion == 0x12340000, "version offsets");
            require(info.displayName.equals("BO2 synthetic header"), "UTF16 display name");
            put(bytes, 0x360, 0x12345678);
            require(!XboxContentHeader.parse(bytes).isBo2Candidate(), "different game");
        }
        byte[] xex = new byte[256];
        System.arraycopy("XEX2".getBytes(StandardCharsets.US_ASCII), 0, xex, 0, 4);
        put(xex, 20, 1);
        put(xex, 24, 0x00040006);
        put(xex, 28, 128);
        put(xex, 128 + 12, 0x415608C3);
        require(XboxContentHeader.parse(xex).isBo2Candidate(), "XEX optional execution info");
        put(xex, 28, 0xFFFFFFF0);
        require(!XboxContentHeader.parse(xex).metadataPresent, "out of range XEX offset");
        put(xex, 20, 0xFFFFFFFF);
        require(!XboxContentHeader.parse(xex).metadataPresent, "unbounded XEX count");
        put(xex, 20, 2); put(xex, 28, 128);
        put(xex, 32, 0x000005FF); put(xex, 36, 160);
        put(xex, 160, 0x4C); put(xex, 164, 0x00001200); put(xex, 168, 0x00000100);
        XboxContentHeader.Info delta = XboxContentHeader.parse(xex);
        require(delta.metadataPresent && delta.deltaMetadataPresent, "execution and delta descriptors are both parsed");
        require(delta.deltaTargetVersion == 0x00001200 && delta.deltaSourceVersion == 0x00000100, "delta version offsets");
        require(XboxContentHeader.parse(new byte[2]).kind.equals("unknown"), "truncated magic");
        require(!XboxContentHeader.parse("LIVE".getBytes(StandardCharsets.US_ASCII)).metadataPresent, "truncated package");
        require(XboxContentHeader.hex(0xFFFFFFFFL).equals("FFFFFFFF"), "unsigned hex");
        require(XboxContentHeader.contentTypeName(0x9000).equals("avatar_item"), "avatar content type");
        System.out.println("Xbox metadata reader: synthetic package/XEX, truncation and bounds checks passed");
    }
}
