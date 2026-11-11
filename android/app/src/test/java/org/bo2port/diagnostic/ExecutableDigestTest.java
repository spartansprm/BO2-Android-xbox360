// SPDX-License-Identifier: GPL-3.0-only
package org.bo2port.diagnostic;

import static org.junit.Assert.assertEquals;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import org.junit.Test;

public class ExecutableDigestTest {
    @Test public void nativeMemoryHashMatchesSha256() throws Exception {
        ByteBuffer memory = ByteBuffer.allocateDirect(3);
        memory.put("abc".getBytes(StandardCharsets.US_ASCII)).flip();
        assertEquals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            ExecutableDigest.digestMemory(memory));
        assertEquals(0, memory.position());
        assertEquals(3, memory.limit());
    }

    @Test public void onlyTheSelectedReadOnlyRegionIsHashed() throws Exception {
        ByteBuffer memory = ByteBuffer.wrap("_abc_".getBytes(StandardCharsets.US_ASCII)).asReadOnlyBuffer();
        memory.position(1);memory.limit(4);
        assertEquals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            ExecutableDigest.digestMemory(memory));
        assertEquals(1, memory.position());
        assertEquals(4, memory.limit());
    }
}
