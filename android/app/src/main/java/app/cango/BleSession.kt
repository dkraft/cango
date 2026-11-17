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
    private val descQueue: ArrayDeque<BluetoothGattDescriptor> = ArrayDeque()
    private var gattBusy = false
    private var announced = false
    private var subscribed = false
    private var fallbackScan = false
    private var opening = false
    private val gate = Any()
    private val outQueue: ArrayDeque<ByteArray> = ArrayDeque()
    /** ATT payload. 20 until the phone finishes MTU exchange. A larger write is dropped. */
    private var payMax = 20
    private var writeFails = 0
    private var descFails = 0
    private var descGen = 0
    private var lastNotifyMs = 0L
    private var lastNotify = ByteArray(0)

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            // Filters are already name OR service UUID. A second name check drops
            // results whose scan record leaves deviceName null.
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
                g.discoverServices()
                post("connected")
                return
            }
            if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                val retry = fallbackScan
                fallbackScan = false
                synchronized(gate) {
                    opening = false
                    writeChar = null
                    gattBusy = false
                    outQueue.clear()
                    descQueue.clear()
                    payMax = 20
                }
                writeFails = 0
                descFails = 0
                descGen = 0
                subscribed = false
                announced = false
                acc.setLength(0)
                lastNotifyMs = 0L
                lastNotify = ByteArray(0)
                post(if (status == BluetoothGatt.GATT_SUCCESS) "disconnected" else "disconnected $status")
                g.close()
                if (gatt === g) {
                    gatt = null
                }
                if (retry) {
                    main.post { scan() }
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                post("discover failed $status")
                return
            }
            val service = g.getService(Gatt.service) ?: run {
                post("service missing")
                return
            }
            writeChar = service.getCharacteristic(Gatt.ctrlWrite)
            if (!g.requestMtu(247)) {
                subscribe(g)
            } else {
                main.postDelayed({
                    if (!subscribed && gatt === g) subscribe(g)
                }, 1500)
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS && mtu >= 23) {
                synchronized(gate) { payMax = mtu - 3 }
            }
            subscribe(g)
        }

        private fun subscribe(g: BluetoothGatt) {
            synchronized(gate) {
                if (subscribed) return
                subscribed = true
            }
            val service = g.getService(Gatt.service) ?: run {
                post("service missing")
                return
            }
            enable(g, service.getCharacteristic(Gatt.ctrlNotify))
            enable(g, service.getCharacteristic(Gatt.dataNotify))
            if (descQueue.isEmpty()) {
                finishAnnounce()
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            main.post {
                if (gatt !== g) return@post
                synchronized(gate) { gattBusy = false }
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    descFails++
                    if (descFails < 5) {
                        synchronized(gate) { descQueue.addFirst(descriptor) }
                        pump(g)
                    } else {
                        descFails = 0
                        post("notify failed")
                    }
                    return@post
                }
                descFails = 0
                if (descQueue.isNotEmpty()) {
                    pump(g)
                    return@post
                }
                finishAnnounce()
            }
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            onNotify(characteristic, value)
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            // API 33 delivers the bytes to the overload above. This value is often stale.
            if (Build.VERSION.SDK_INT >= 33) return
            onNotify(characteristic, characteristic.value ?: return)
        }

        private fun onNotify(characteristic: BluetoothGattCharacteristic, value: ByteArray) {
            val now = SystemClock.elapsedRealtime()
            if (now - lastNotifyMs < 40 && value.contentEquals(lastNotify)) return
            lastNotifyMs = now
            lastNotify = value.copyOf()
            if (characteristic.uuid == Gatt.dataNotify) {
                try {
                    val (_, frames) = Codec.decodeBatch(value)
                    main.post { onFrames(frames) }
                } catch (_err: IllegalArgumentException) {
                    post("bad batch")
                }
            } else {
                acc.append(String(value, Charsets.UTF_8))
                if (acc.length > 1500 && acc.indexOf("\n") < 0) {
                    acc.setLength(0)
                    return
                }
                var nl = acc.indexOf("\n")
                while (nl >= 0) {
                    val line = acc.substring(0, nl)
                    acc.delete(0, nl + 1)
                    main.post { onJson(line) }
                    nl = acc.indexOf("\n")
                }
            }
        }
    }

    @SuppressLint("MissingPermission")
    fun startScan() {
        // One link stops advertising, so a second scan cannot see the board.
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
            subscribed = false
            announced = false
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

    @SuppressLint("MissingPermission")
    fun send(json: String) {
        val payload = if (json.endsWith("\n")) json else json + "\n"
        val bytes = payload.toByteArray(Charsets.UTF_8)
        val g = synchronized(gate) {
            // 20 fits the default ATT payload. A larger write is what used to stall the queue.
            val max = minOf(20, payMax.coerceIn(20, 512))
            var off = 0
            while (off < bytes.size) {
                val n = minOf(max, bytes.size - off)
                outQueue.add(bytes.copyOfRange(off, off + n))
                off += n
            }
            gatt
        }
        if (g != null) onMain(g, ::pumpWrites)
    }

    @SuppressLint("MissingPermission")
    private fun pumpWrites(g: BluetoothGatt) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            main.post { if (gatt === g) pumpWrites(g) }
            return
        }
        val next: ByteArray
        val ch: BluetoothGattCharacteristic
        synchronized(gate) {
            if (gattBusy || !announced) return
            val w = writeChar ?: return
            val queued = outQueue.removeFirstOrNull() ?: return
            ch = w
            next = queued
        }
        if (!writeChunk(g, ch, next)) {
            writeFails++
            if (writeFails < 6) {
                synchronized(gate) { outQueue.addFirst(next) }
                main.postDelayed({ if (gatt === g) pumpWrites(g) }, 40)
            } else {
                writeFails = 0
            }
            return
        }
        writeFails = 0
        main.postDelayed({ if (gatt === g) pumpWrites(g) }, 25)
    }

    @SuppressLint("MissingPermission")
    private fun writeChunk(g: BluetoothGatt, ch: BluetoothGattCharacteristic, next: ByteArray): Boolean {
        ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(ch, next, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE) ==
                BluetoothStatusCodes.SUCCESS
        } else {
            ch.value = next
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

    private fun onMain(g: BluetoothGatt, block: (BluetoothGatt) -> Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            if (gatt === g) block(g)
        } else {
            main.post { if (gatt === g) block(g) }
        }
    }

    @SuppressLint("MissingPermission")
    fun close() {
        stopScan()
        synchronized(gate) { opening = false }
        gatt?.close()
        gatt = null
    }

    @SuppressLint("MissingPermission")
    private fun enable(g: BluetoothGatt, ch: BluetoothGattCharacteristic?) {
        if (ch == null) {
            return
        }
        g.setCharacteristicNotification(ch, true)
        val desc = ch.getDescriptor(cccd) ?: return
        synchronized(gate) { descQueue.add(desc) }
        pump(g)
    }

    @SuppressLint("MissingPermission")
    private fun pump(g: BluetoothGatt) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            main.post { if (gatt === g) pump(g) }
            return
        }
        val desc = synchronized(gate) {
            if (gattBusy) return
            val next = descQueue.removeFirstOrNull() ?: return
            gattBusy = true
            descGen++
            next
        }
        val gen = descGen
        // API 33 drops the deprecated write and never calls back, so Files stays empty.
        val value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        val wrote = if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(desc, value) == BluetoothStatusCodes.SUCCESS
        } else {
            desc.value = value
            g.writeDescriptor(desc)
        }
        if (wrote) {
            // The stack sometimes accepts the descriptor write and never calls back.
            main.postDelayed({
                if (gatt !== g || announced || descGen != gen) return@postDelayed
                val stuck = synchronized(gate) {
                    if (!gattBusy || descGen != gen) {
                        false
                    } else {
                        gattBusy = false
                        true
                    }
                }
                if (!stuck) return@postDelayed
                if (descQueue.isNotEmpty()) pump(g) else finishAnnounce()
            }, 600)
            return
        }
        descFails++
        synchronized(gate) {
            gattBusy = false
            if (descFails < 5) descQueue.addFirst(desc)
        }
        if (descFails < 5) {
            main.postDelayed({ if (gatt === g) pump(g) }, 40)
        } else {
            descFails = 0
            post("notify failed")
        }
    }

    private fun post(text: String) {
        main.post { onStatus(text) }
    }
}
