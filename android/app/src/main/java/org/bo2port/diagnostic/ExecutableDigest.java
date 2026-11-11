// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import java.nio.ByteBuffer;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;

public final class ExecutableDigest {
    private ExecutableDigest() {}

    public static String digestMemory(ByteBuffer memory) throws NoSuchAlgorithmException {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        digest.update(memory.duplicate());
        StringBuilder result = new StringBuilder(64);
        for (byte value : digest.digest()) result.append(String.format(Locale.ROOT, "%02x", value & 255));
        return result.toString();
    }
}
