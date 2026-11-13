package app.cango

import android.annotation.SuppressLint
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
    private val outQueue: ArrayDeque<ByteArray> = ArrayDeque()

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            stopScan()
            result.device.connectGatt(context, false, gattCallback)
            post("connecting")
        }

        override fun onScanFailed(errorCode: Int) {
            post("scan failed $errorCode")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothGatt.STATE_CONNECTED) {
                gatt = g
                g.discoverServices()
                post("connected")
            } else if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                post("disconnected")
                g.close()
                if (gatt === g) {
                    gatt = null
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val service = g.getService(Gatt.service) ?: run {
                post("service missing")
                return
            }
            writeChar = service.getCharacteristic(Gatt.ctrlWrite)
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

        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            val value = characteristic.value ?: return
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
        val scanner = adapter?.bluetoothLeScanner ?: run {
            post("bluetooth off")
            return
        }
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(Gatt.service)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanner.startScan(listOf(filter), settings, scanCallback)
        post("scanning")
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
