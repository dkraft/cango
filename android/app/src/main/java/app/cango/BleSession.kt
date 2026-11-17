package app.cango

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.os.SystemClock
import java.util.UUID

/**
 * One BLE link to the board. Control writes are JSON plus a newline.
 * Data notifications are CANGO batches. Log files are not transferred here.
 *
 * Android allows one GATT operation at a time, and it drops a notification
 * that arrives while a write is still outstanding. Setup is MTU, then each
 * notification enable. Each command is one or more writes with response,
 * then a wait until the matching JSON line arrives. The board sends that
 * line in 20-byte notifications so older Android builds do not truncate it.
 */
class BleSession(
    context: Context,
    private val onStatus: (String) -> Unit,
    private val onFrames: (List<CanFrame>) -> Unit,
    private val onJson: (String) -> Unit,
) {
    private val appContext = context.applicationContext
    private val main = Handler(Looper.getMainLooper())
    private val manager = context.getSystemService(BluetoothManager::class.java)
    private val adapter = manager.adapter
    private var gatt: BluetoothGatt? = null
    private var writeChar: BluetoothGattCharacteristic? = null
    private val acc = StringBuilder()
    private val cccd = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    private var announced = false
    private var arming = false
    private var setupLeft = 0
    private var fallbackScan = false
    private var opening = false
    private val gate = Any()
    private val ops: ArrayDeque<Op> = ArrayDeque()
    private var active: Op? = null
    private var activeTries = 0
    @Volatile private var epoch = 0
    /** ATT payload. 20 until the phone finishes MTU exchange. */
    @Volatile private var payMax = 20
    private var modernNotify = false
    private var lastNotifyMs = 0L
    private var lastNotify = ByteArray(0)
    /** Reply op the phone is waiting for. No write runs while this is set. */
    private var awaitOp: String? = null
    private var awaitGen = 0

    private sealed class Op {
        data object Mtu : Op()
        class Notify(val ch: BluetoothGattCharacteristic) : Op()
        class Write(val data: ByteArray) : Op()
        class Barrier(val expect: String) : Op()
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            connect(result.device)
        }

        override fun onScanFailed(errorCode: Int) {
            post("scan failed $errorCode")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothGatt.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                gatt = g
                fallbackScan = false
                post("connected")
                main.post {
                    if (gatt !== g) return@post
                    g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                    main.postDelayed({
                        if (gatt === g) g.discoverServices()
                    }, 200)
                }
                return
            }
            if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                val retry = fallbackScan
                fallbackScan = false
                epoch++
                synchronized(gate) { opening = false }
                announced = false
                arming = false
                setupLeft = 0
                active = null
                activeTries = 0
                awaitOp = null
                awaitGen++
                ops.clear()
                writeChar = null
                payMax = 20
                modernNotify = false
                acc.setLength(0)
                lastNotifyMs = 0L
                lastNotify = ByteArray(0)
                post(if (status == BluetoothGatt.GATT_SUCCESS) "disconnected" else "disconnected $status")
                g.close()
                if (gatt === g) gatt = null
                if (retry) main.post { scan() }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            main.post {
                if (gatt !== g) return@post
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    post("discover failed $status")
                    return@post
                }
                val service = g.getService(Gatt.service) ?: run {
                    post("service missing")
                    return@post
                }
                val write = service.getCharacteristic(Gatt.ctrlWrite)
                val ctrl = service.getCharacteristic(Gatt.ctrlNotify)
                val data = service.getCharacteristic(Gatt.dataNotify)
                if (write == null || ctrl == null || data == null) {
                    post("service missing")
                    return@post
                }
                writeChar = write
                arming = true
                setupLeft = 3
                enqueue(Op.Mtu)
                enqueue(Op.Notify(ctrl))
                enqueue(Op.Notify(data))
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS && mtu >= 23) {
                payMax = (mtu - 3).coerceIn(20, 512)
            }
            val seen = epoch
            main.post {
                if (gatt !== g || seen != epoch || active !is Op.Mtu) return@post
                succeed()
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            val seen = epoch
            main.post {
                if (gatt !== g || seen != epoch || active !is Op.Notify) return@post
                if (status == BluetoothGatt.GATT_SUCCESS) succeed() else failActive()
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            val seen = epoch
            main.post {
                if (gatt !== g || seen != epoch || active !is Op.Write) return@post
                if (status == BluetoothGatt.GATT_SUCCESS) succeed() else failActive()
            }
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            modernNotify = true
            deliver(characteristic, value)
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (modernNotify) return
            val value = characteristic.value ?: return
            deliver(characteristic, value)
        }
    }

    private fun deliver(characteristic: BluetoothGattCharacteristic, value: ByteArray) {
        val now = SystemClock.elapsedRealtime()
        /* The same packet can arrive on both notify callbacks a moment apart.
         * The next 20-byte piece is a connection event later, so a short
         * window does not drop it. */
        if (now - lastNotifyMs < 8 && value.contentEquals(lastNotify)) return
        lastNotifyMs = now
        lastNotify = value.copyOf()
        val uuid = characteristic.uuid
        val copy = value.copyOf()
        main.post {
            if (gatt == null) return@post
            if (uuid == Gatt.dataNotify) {
                try {
                    val (_, frames) = Codec.decodeBatch(copy)
                    onFrames(frames)
                } catch (_err: IllegalArgumentException) {
                    post("bad batch")
                }
                return@post
            }
            acc.append(String(copy, Charsets.UTF_8))
            if (acc.length > 1500 && acc.indexOf("\n") < 0) {
                acc.setLength(0)
                return@post
            }
            var nl = acc.indexOf("\n")
            while (nl >= 0) {
                val line = acc.substring(0, nl).trimEnd('\r')
                acc.delete(0, nl + 1)
                if (line.isNotEmpty()) {
                    onJson(line)
                    val got = jsonOp(line)
                    if (awaitOp != null && got == awaitOp) {
                        awaitOp = null
                        awaitGen++
                        kick()
                    }
                }
                nl = acc.indexOf("\n")
            }
        }
    }

    @SuppressLint("MissingPermission")
    fun startScan() {
        if (gatt != null) {
            post(if (announced) "ready" else "connecting")
            return
        }
        stopScan()
        synchronized(gate) { opening = false }
        fallbackScan = false
        val radio = adapter
        if (radio == null || !radio.isEnabled) {
            post("bluetooth off")
            return
        }
        val paired = radio.bondedDevices?.firstOrNull { it.name == "CANGO" }
        if (paired != null) {
            fallbackScan = true
            connect(paired)
            return
        }
        scan()
    }

    @SuppressLint("MissingPermission")
    private fun scan() {
        val scanner = adapter?.bluetoothLeScanner ?: run {
            post("bluetooth off")
            return
        }
        stopScan()
        val filters = listOf(
            ScanFilter.Builder().setDeviceName("CANGO").build(),
            ScanFilter.Builder().setServiceUuid(ParcelUuid(Gatt.service)).build(),
        )
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanner.startScan(filters, settings, scanCallback)
        post("scanning")
    }

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        synchronized(gate) {
            if (opening) return
            opening = true
        }
        stopScan()
        main.post {
            val previous = gatt
            gatt = null
            previous?.close()
            announced = false
            arming = false
            writeChar = null
            post("connecting")
            val opened = device.connectGatt(appContext, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            gatt = opened
            if (opened == null) {
                synchronized(gate) { opening = false }
                post("connect failed")
                if (fallbackScan) {
                    fallbackScan = false
                    scan()
                }
            }
        }
    }

    @SuppressLint("MissingPermission")
    fun stopScan() {
        adapter?.bluetoothLeScanner?.stopScan(scanCallback)
    }

    fun send(json: String) {
        val payload = if (json.endsWith("\n")) json else json + "\n"
        val bytes = payload.toByteArray(Charsets.UTF_8)
        val expect = expectOp(jsonOp(payload))
        main.post {
            if (gatt == null || writeChar == null || expect.isEmpty()) return@post
            val max = payMax.coerceIn(20, 512)
            var off = 0
            while (off < bytes.size) {
                val n = minOf(max, bytes.size - off)
                ops.addLast(Op.Write(bytes.copyOfRange(off, off + n)))
                off += n
            }
            ops.addLast(Op.Barrier(expect))
            kick()
        }
    }

    private fun jsonOp(line: String): String {
        val key = "\"op\":\""
        val at = line.indexOf(key)
        if (at < 0) return ""
        val start = at + key.length
        val end = line.indexOf('"', start)
        if (end < 0) return ""
        return line.substring(start, end)
    }

    private fun expectOp(cmd: String): String {
        return if (cmd == "status" || cmd == "capture_status") "bus_status" else cmd
    }

    private fun enqueue(op: Op) {
        ops.addLast(op)
        kick()
    }

    @SuppressLint("MissingPermission")
    private fun kick() {
        if (active != null || awaitOp != null || gatt == null) return
        val op = ops.removeFirstOrNull() ?: return
        if (op is Op.Barrier) {
            awaitOp = op.expect
            val gen = ++awaitGen
            main.postDelayed({
                if (awaitGen == gen) {
                    awaitOp = null
                    kick()
                }
            }, 4000)
            return
        }
        active = op
        activeTries = 0
        begin(op)
    }

    @SuppressLint("MissingPermission")
    private fun begin(op: Op) {
        val g = gatt ?: return
        val gen = ++epoch
        val started = when (op) {
            Op.Mtu -> g.requestMtu(247)
            is Op.Notify -> enableNotify(g, op.ch)
            is Op.Write -> writeCtrl(g, op.data)
            is Op.Barrier -> false
        }
        if (!started) {
            failActive()
            return
        }
        main.postDelayed({
            if (epoch == gen) failActive()
        }, 1000)
    }

    private fun succeed() {
        epoch++
        active = null
        activeTries = 0
        if (arming) {
            setupLeft--
            if (setupLeft == 0) {
                arming = false
                finishAnnounce()
            }
        }
        kick()
    }

    private fun failActive() {
        val op = active ?: return
        epoch++
        activeTries++
        if (activeTries < 3) {
            main.postDelayed({
                if (gatt != null && active === op) begin(op)
            }, 100)
            return
        }
        active = null
        activeTries = 0
        when (op) {
            Op.Mtu -> {
                if (arming) {
                    setupLeft--
                    if (setupLeft == 0) {
                        arming = false
                        finishAnnounce()
                    }
                }
                kick()
            }
            is Op.Notify -> {
                ops.clear()
                arming = false
                setupLeft = 0
                post("notify failed")
            }
            is Op.Write -> kick()
            is Op.Barrier -> kick()
        }
    }

    @SuppressLint("MissingPermission")
    private fun enableNotify(g: BluetoothGatt, ch: BluetoothGattCharacteristic): Boolean {
        g.setCharacteristicNotification(ch, true)
        val desc = ch.getDescriptor(cccd) ?: return false
        val value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(desc, value) == BluetoothStatusCodes.SUCCESS
        } else {
            desc.value = value
            g.writeDescriptor(desc)
        }
    }

    @SuppressLint("MissingPermission")
    private fun writeCtrl(g: BluetoothGatt, data: ByteArray): Boolean {
        val ch = writeChar ?: return false
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(ch, data, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) ==
                BluetoothStatusCodes.SUCCESS
        } else {
            ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            ch.value = data
            g.writeCharacteristic(ch)
        }
    }

    private fun finishAnnounce() {
        if (announced) return
        announced = true
        post("ready")
        send("""{"op":"hello"}""")
        send("""{"op":"status"}""")
    }

    @SuppressLint("MissingPermission")
    fun close() {
        stopScan()
        synchronized(gate) { opening = false }
        epoch++
        awaitGen++
        awaitOp = null
        ops.clear()
        active = null
        gatt?.close()
        gatt = null
    }

    private fun post(text: String) {
        main.post { onStatus(text) }
    }
}
