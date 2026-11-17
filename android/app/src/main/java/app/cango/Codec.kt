package app.cango

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

/** GATT ids. BLE_UUID128_INIT is little-endian: its first byte is the UUID tail. */
object Gatt {
    val service: UUID = UUID.fromString("8a3c0001-1f2e-4b5a-9c0d-11c0a1000001")
    val ctrlWrite: UUID = UUID.fromString("8a3c0002-1f2e-4b5a-9c0d-11c0a1000001")
    val ctrlNotify: UUID = UUID.fromString("8a3c0003-1f2e-4b5a-9c0d-11c0a1000001")
    val dataNotify: UUID = UUID.fromString("8a3c0004-1f2e-4b5a-9c0d-11c0a1000001")
    /** Tail byte was duplicated from the characteristic number on older firmware. */
    val ctrlWriteLegacy: UUID = UUID.fromString("8a3c0002-1f2e-4b5a-9c0d-11c0a1000002")
    val ctrlNotifyLegacy: UUID = UUID.fromString("8a3c0003-1f2e-4b5a-9c0d-11c0a1000003")
    val dataNotifyLegacy: UUID = UUID.fromString("8a3c0004-1f2e-4b5a-9c0d-11c0a1000004")
}

data class CanFrame(
    val tUs: Long,
    val id: Long,
    val flags: Int,
    val data: ByteArray,
)

object Codec {
    const val frameBytes = 20
    const val flagExt = 0x01
    const val flagRtr = 0x02
    const val flagTx = 0x04

    fun encodeBatch(seq: Int, frames: List<CanFrame>): ByteArray {
        val out = ByteArray(6 + frames.size * frameBytes)
        out[0] = 'C'.code.toByte()
        out[1] = 'G'.code.toByte()
        out[2] = 1
        out[3] = frames.size.toByte()
        out[4] = (seq and 0xff).toByte()
        out[5] = ((seq shr 8) and 0xff).toByte()
        var off = 6
        for (frame in frames) {
            putFrame(out, off, frame)
            off += frameBytes
        }
        return out
    }

    fun decodeBatch(raw: ByteArray): Pair<Int, List<CanFrame>> {
        if (raw.size < 6 || raw[0] != 'C'.code.toByte() || raw[1] != 'G'.code.toByte() || raw[2] != 1.toByte()) {
            throw IllegalArgumentException("batch header")
        }
        val count = raw[3].toInt() and 0xff
        if (raw.size < 6 + count * frameBytes) {
            throw IllegalArgumentException("batch short")
        }
        val seq = (raw[4].toInt() and 0xff) or ((raw[5].toInt() and 0xff) shl 8)
        val frames = ArrayList<CanFrame>(count)
        var off = 6
        for (i in 0 until count) {
            frames.add(getFrame(raw, off))
            off += frameBytes
        }
        return seq to frames
    }

    private fun putFrame(out: ByteArray, off: Int, frame: CanFrame) {
        val buf = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN)
        val n = frame.data.size.coerceAtMost(8)
        buf.putInt(off, frame.tUs.toInt())
        buf.putInt(off + 4, frame.id.toInt())
        out[off + 8] = frame.flags.toByte()
        out[off + 9] = n.toByte()
        frame.data.copyInto(out, off + 10, 0, n)
    }

    private fun getFrame(raw: ByteArray, off: Int): CanFrame {
        val buf = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN)
        val tUs = buf.getInt(off).toLong() and 0xffffffffL
        val id = buf.getInt(off + 4).toLong() and 0xffffffffL
        val flags = raw[off + 8].toInt() and 0xff
        val n = (raw[off + 9].toInt() and 0xff).coerceAtMost(8)
        return CanFrame(tUs, id, flags, raw.copyOfRange(off + 10, off + 10 + n))
    }
}
