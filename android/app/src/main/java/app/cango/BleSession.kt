package app.cango

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
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
                synchronized(gate) { opening = false }
                subscribed = false
                announced = false
                writeChar = null
                gattBusy = false
                descQueue.clear()
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
            post("ready")
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            gattBusy = false
            if (descQueue.isNotEmpty()) {
                pump(g)
                return
            }
            if (!announced) {
                announced = true
                send("""{"op":"hello"}""")
                send("""{"op":"status"}""")
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            gattBusy = false
            pumpWrites(g)
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
            if (Build.VERSION.SDK_INT >= 33) return
            onNotify(characteristic, characteristic.value ?: return)
        }

        private fun onNotify(characteristic: BluetoothGattCharacteristic, value: ByteArray) {
            if (characteristic.uuid == Gatt.dataNotify) {
                try {
                    val (_, frames) = Codec.decodeBatch(value)
                    main.post { onFrames(frames) }
                } catch (_err: IllegalArgumentException) {
                    post("bad batch")
                }
            } else {
                acc.append(String(value, Charsets.UTF_8))
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
        outQueue.add(payload.toByteArray(Charsets.UTF_8))
        val g = gatt ?: return
        pumpWrites(g)
    }

    @SuppressLint("MissingPermission")
    private fun pumpWrites(g: BluetoothGatt) {
        if (gattBusy) {
            return
        }
        val ch = writeChar ?: return
        val next = outQueue.removeFirstOrNull() ?: return
        gattBusy = true
        ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        ch.value = next
        g.writeCharacteristic(ch)
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
        desc.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        descQueue.add(desc)
        pump(g)
    }

    @SuppressLint("MissingPermission")
    private fun pump(g: BluetoothGatt) {
        if (gattBusy) {
            return
        }
        val desc = descQueue.removeFirstOrNull() ?: return
        gattBusy = true
        g.writeDescriptor(desc)
    }

    private fun post(text: String) {
        main.post { onStatus(text) }
    }
}
