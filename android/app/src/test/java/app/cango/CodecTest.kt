package app.cango

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

class CodecTest {
    @Test
    fun roundTripMatchesWireLayout() {
        val frame = CanFrame(1000, 0x7E0, 0, byteArrayOf(0x02, 0x10, 0x01))
        val raw = Codec.encodeBatch(7, listOf(frame))
        assertEquals(26, raw.size)
        assertEquals('C'.code.toByte(), raw[0])
        assertEquals('G'.code.toByte(), raw[1])
        assertEquals(1.toByte(), raw[2])
        assertEquals(1.toByte(), raw[3])
        assertEquals(7.toByte(), raw[4])
        assertEquals(0.toByte(), raw[5])
        val (seq, frames) = Codec.decodeBatch(raw)
        assertEquals(7, seq)
        assertEquals(0x7E0L, frames[0].id)
        assertEquals(1000L, frames[0].tUs)
        assertArrayEquals(byteArrayOf(0x02, 0x10, 0x01), frames[0].data)
    }
}
