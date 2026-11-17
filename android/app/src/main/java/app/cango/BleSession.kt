package app.cango

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
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
 * Android allows one GATT operation at a time. Starting a second one while
 * the first is still pending drops the link. Setup waits briefly, discovers
 * services, then enables each notification. A timeout closes the link and
 * connects once more. It does not stack another write. MTU is not requested
 * on this path: a stuck requestMtu blocks every later callback. Commands
 * wait until notifications are on, then one write-with-response at a time,
 * then the matching JSON line. The board sends that line in 20-byte
 * notifications.
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
    private var setupTries = 0
    private var reconnectDev: BluetoothDevice? = null
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
    /** Commands asked for before notifications are enabled. */
    private val parked = ArrayDeque<String>()

    private sealed class Op {
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
                /* Discovery on the callback thread races the board's connection
                 * update and the phone then drops the link. */
                main.postDelayed({
                    if (gatt === g) g.discoverServices()
                }, 500)
                return
            }
            if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                val retry = fallbackScan
                val again = reconnectDev
                fallbackScan = false
                reconnectDev = null
                epoch++
                synchronized(gate) { opening = false }
                announced = false
                arming = false
                setupLeft = 0
                active = null
                activeTries = 0
                awaitOp = null
                awaitGen++
                parked.clear()
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
                if (again != null) {
                    main.postDelayed({ connect(again) }, 400)
                } else if (retry) {
                    main.post { scan() }
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            gatt = g
            main.post {
                if (gatt !== g) return@post
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    post("discover failed $status")
                    g.disconnect()
                    return@post
                }
                val service = g.getService(Gatt.service) ?: run {
                    post("service missing")
                    g.disconnect()
                    return@post
                }
                val write = service.chr(Gatt.ctrlWrite, Gatt.ctrlWriteLegacy)
                val ctrl = service.chr(Gatt.ctrlNotify, Gatt.ctrlNotifyLegacy)
                val data = service.chr(Gatt.dataNotify, Gatt.dataNotifyLegacy)
                if (write == null || ctrl == null || data == null) {
                    post("service missing")
                    g.disconnect()
                    return@post
                }
                writeChar = write
                arming = true
                setupLeft = 2
                enqueue(Op.Notify(ctrl))
                enqueue(Op.Notify(data))
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            if (gatt !== g) return
            if (status == BluetoothGatt.GATT_SUCCESS && mtu >= 23) {
                payMax = (mtu - 3).coerceIn(20, 512)
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            val seen = epoch
            main.post {
                if (gatt !== g || seen != epoch || active !is Op.Notify) return@post
                if (status == BluetoothGatt.GATT_SUCCESS) succeed() else failStart()
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            val seen = epoch
            main.post {
                if (gatt !== g || seen != epoch || active !is Op.Write) return@post
                if (status == BluetoothGatt.GATT_SUCCESS) succeed() else failStart()
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

    private fun BluetoothGattService.chr(primary: UUID, legacy: UUID): BluetoothGattCharacteristic? {
        return getCharacteristic(primary) ?: getCharacteristic(legacy)
    }

    @SuppressLint("MissingPermission")
    fun startScan() {
        setupTries = 0
        reconnectDev = null
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
            if (gatt == null) gatt = opened
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
        main.post {
            if (gatt == null) return@post
            if (!announced || writeChar == null) {
                if (parked.size >= 16) parked.removeFirst()
                parked.addLast(payload)
                return@post
            }
            enqueueCommand(payload)
        }
    }

    private fun enqueueCommand(payload: String) {
        val expect = expectOp(jsonOp(payload))
        if (expect.isEmpty() || gatt == null || writeChar == null) return
        val bytes = payload.toByteArray(Charsets.UTF_8)
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
            is Op.Notify -> enableNotify(g, op.ch)
            is Op.Write -> writeCtrl(g, op.data)
            is Op.Barrier -> false
        }
        if (!started) {
            failStart()
            return
        }
        main.postDelayed({
            if (epoch == gen) failPending()
        }, 2500)
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

    /** The stack rejected the operation, so nothing is still pending. */
    private fun failStart() {
        val op = active ?: return
        epoch++
        activeTries++
        if (activeTries < 3) {
            main.postDelayed({
                if (gatt != null && active === op) begin(op)
            }, 200)
            return
        }
        dropLink(op is Op.Notify)
    }

    /** The operation may still be pending. Another one would drop the link. */
    @SuppressLint("MissingPermission")
    private fun failPending() {
        val op = active ?: return
        epoch++
        dropLink(op is Op.Notify)
    }

    @SuppressLint("MissingPermission")
    private fun dropLink(notify: Boolean) {
        active = null
        activeTries = 0
        ops.clear()
        awaitOp = null
        awaitGen++
        arming = false
        setupLeft = 0
        if (setupTries < 1) {
            setupTries++
            reconnectDev = gatt?.device
        } else if (notify) {
            post("notify failed")
        }
        gatt?.disconnect()
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
        setupTries = 0
        post("ready")
        val held = parked.toList()
        parked.clear()
        for (payload in held) enqueueCommand(payload)
        enqueueCommand("{\"op\":\"hello\"}\n")
        enqueueCommand("{\"op\":\"status\"}\n")
        enqueueCommand("{\"op\":\"list_dir\",\"path\":\"\",\"skip\":0}\n")
    }

    @SuppressLint("MissingPermission")
    fun close() {
        stopScan()
        synchronized(gate) { opening = false }
        epoch++
        awaitGen++
        awaitOp = null
        parked.clear()
        ops.clear()
        active = null
        gatt?.close()
        gatt = null
    }

    private fun post(text: String) {
        main.post { onStatus(text) }
    }
}
