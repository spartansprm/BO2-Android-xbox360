// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import java.nio.charset.StandardCharsets;
import java.util.Locale;


public final class XboxContentHeader {
    public static final long BO2_TITLE_ID = 0x415608C3L;
    public static final int MAX_HEADER_BYTES = 0x6000;

    public static final class Info {
        public String kind = "unknown";
        public String signature = "";
        public boolean metadataPresent;
        public long contentType = -1, titleId = -1, mediaId = -1;
        public long version = -1, baseVersion = -1, volumeType = -1, dataFiles = -1;
        public String displayName = "";
        public boolean deltaMetadataPresent;
        public long moduleFlags = -1, deltaSourceVersion = -1, deltaTargetVersion = -1;
        public boolean isBo2Candidate() { return metadataPresent && titleId == BO2_TITLE_ID; }
    }

    private XboxContentHeader() {}

    public static boolean recognized(byte[] data) {
        if (data.length < 4) return false;
        String magic = new String(data, 0, 4, StandardCharsets.US_ASCII);
        return magic.equals("LIVE") || magic.equals("PIRS") || magic.equals("CON ")
            || magic.equals("XEX2") || (data[0] == 'M' && data[1] == 'Z');
    }

    public static Info parse(byte[] data) {
        Info info = new Info();
        if (!recognized(data)) return info;
        info.signature = new String(data, 0, 4, StandardCharsets.US_ASCII);
        if (data[0] == 'M' && data[1] == 'Z') {
            info.signature = "MZ";
            info.kind = "dos_or_pe_candidate";
            return info;
        }
        if (info.signature.equals("XEX2")) {
            info.kind = "xex2";
            if (data.length < 24) return info;
            info.moduleFlags = u32(data, 4);
            long count = u32(data, 20);
            if (count > 512 || 24 + count * 8 > data.length) return info;
            for (int i = 0; i < (int) count; ++i) {
                int entry = 24 + i * 8;
                long key = u32(data, entry);
                long offset = u32(data, entry + 4);
                if (key == 0x00040006L && offset <= data.length - 24L)
                    readExecutionInfo(info, data, (int) offset);
                if (key == 0x000005FFL && offset <= data.length - 12L && u32(data, (int) offset) >= 0x4C) {
                    info.deltaMetadataPresent = true;
                    info.deltaTargetVersion = u32(data, (int) offset + 4);
                    info.deltaSourceVersion = u32(data, (int) offset + 8);
                }
            }
            return info;
        }
        info.kind = "xcontent_package";
        if (data.length < 0x3AD) return info;
        info.contentType = u32(data, 0x344);
        readExecutionInfo(info, data, 0x354);
        info.dataFiles = u32(data, 0x39D);
        info.volumeType = u32(data, 0x3A9);
        if (data.length >= 0x511) {
            String name = new String(data, 0x411, 0x100, StandardCharsets.UTF_16BE);
            int end = name.indexOf('\0');
            info.displayName = (end >= 0 ? name.substring(0, end) : name).trim();
        }
        return info;
    }

    private static void readExecutionInfo(Info info, byte[] data, int offset) {
        info.mediaId = u32(data, offset);
        info.version = u32(data, offset + 4);
        info.baseVersion = u32(data, offset + 8);
        info.titleId = u32(data, offset + 12);
        info.metadataPresent = true;
    }

    private static long u32(byte[] data, int offset) {
        return ((long) (data[offset] & 255) << 24) | ((long) (data[offset + 1] & 255) << 16)
            | ((long) (data[offset + 2] & 255) << 8) | (data[offset + 3] & 255);
    }

    public static String hex(long value) {
        return value < 0 ? "unknown" : String.format(Locale.ROOT, "%08X", value);
    }

    public static String contentTypeName(long type) {
        if (type == 0x00007000) return "games_on_demand";
        if (type == 0x00004000) return "installed_game";
        if (type == 0x00000002) return "marketplace_content_dlc_candidate";
        if (type == 0x000B0000) return "installer_title_update_candidate";
        if (type == 0x00009000) return "avatar_item";
        return "other_or_unknown";
    }
}
