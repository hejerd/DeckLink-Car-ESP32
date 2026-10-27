package com.hejerd.decklink

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import java.util.UUID
import java.util.concurrent.ConcurrentLinkedQueue

/**
 * 보드(ESP32 NimBLE) BLE 클라이언트. DeckClient(TCP) 와 같은 줄 프로토콜.
 *  - 서비스 UUID 로 스캔 → 연결 → MTU 517 → TX notify 구독 → RX 에 write-without-response
 *  - 쓰기는 한 번에 하나씩 (onCharacteristicWrite 후 다음), MTU-3 바이트씩 잘라 보냄
 *  - 끊기면 자동 재연결 (주소 기억 → 스캔 없이 autoConnect)
 */
@SuppressLint("MissingPermission")
class BleClient(
    private val ctx: Context,
    private val onLine: (String) -> Unit,
    private val onState: (Boolean, String) -> Unit,
) : DeckLink {
    companion object {
        const val TAG = "BleClient"
        val SVC: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        val RX: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")   // 앱→보드
        val TX: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")   // 보드→앱
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        const val PREF_ADDR = "ble_addr"
    }

    private val main = Handler(Looper.getMainLooper())
    private val adapter: BluetoothAdapter? = (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
    private var gatt: BluetoothGatt? = null
    private var rx: BluetoothGattCharacteristic? = null
    @Volatile private var ready = false
    @Volatile private var running = false
    private var mtu = 23
    private val txq = ConcurrentLinkedQueue<ByteArray>()
    @Volatile private var writing = false
    private val line = StringBuilder()
    private var scanning = false

    override val connected: Boolean get() = ready

    override fun start() {
        if (running) return
        running = true
        connectOrScan()
    }

    override fun stop() {
        running = false
        stopScan()
        try { gatt?.disconnect(); gatt?.close() } catch (_: Exception) {}
        gatt = null; ready = false
    }

    override fun sendLine(s: String) = send((s + "\n").toByteArray(Charsets.UTF_8))

    override fun send(b: ByteArray) {
        if (!ready) return
        // MTU-3 씩 조각내어 큐에. 한 메시지의 조각들 사이에 다른 스레드의 줄(M:/L:)이
        // 끼어들면 보드가 아트 바이트를 잘못 읽으므로(사선으로 밀린 이미지) 통째로 잠그고 넣는다
        val chunk = (mtu - 3).coerceAtLeast(20)
        synchronized(txq) {
            if (txq.size > 200) txq.clear()
            var off = 0
            while (off < b.size) {
                val n = minOf(chunk, b.size - off)
                txq.add(b.copyOfRange(off, off + n))
                off += n
            }
        }
        main.post { pump() }
    }

    private fun pump() {
        if (writing || !ready) return
        val g = gatt ?: return
        val c = rx ?: return
        val b = synchronized(txq) { txq.poll() } ?: return
        writing = true
        val ok = if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, b, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION") run {
                c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                c.value = b
                g.writeCharacteristic(c)
            }
        }
        if (!ok) { writing = false; Log.w(TAG, "write failed"); main.postDelayed({ pump() }, 20) }
    }

    // ---------- 연결 ----------
    private fun prefs() = ctx.getSharedPreferences(DeckService.PREFS, Context.MODE_PRIVATE)

    private fun connectOrScan() {
        if (!running) return
        val ad = adapter
        if (ad == null || !ad.isEnabled) { onState(false, "블루투스 꺼짐"); main.postDelayed({ connectOrScan() }, 3000); return }
        val addr = prefs().getString(PREF_ADDR, null)
        if (addr != null) {
            try {
                onState(false, "BLE 연결 시도 $addr")
                connect(ad.getRemoteDevice(addr), autoConnect = true)
                return
            } catch (e: Exception) { Log.w(TAG, "remote device: ${e.message}") }
        }
        startScan()
    }

    private val scanCb = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val d = result.device ?: return
            Log.i(TAG, "found ${d.address} ${result.scanRecord?.deviceName}")
            stopScan()
            prefs().edit().putString(PREF_ADDR, d.address).apply()
            connect(d, autoConnect = false)
        }
        override fun onScanFailed(errorCode: Int) { Log.w(TAG, "scan failed $errorCode"); scanning = false; main.postDelayed({ connectOrScan() }, 3000) }
    }

    private fun startScan() {
        if (scanning) return
        val scanner = adapter?.bluetoothLeScanner ?: return
        onState(false, "BLE 스캔 중")
        val filters = listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(SVC)).build())
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        try { scanner.startScan(filters, settings, scanCb); scanning = true } catch (e: Exception) { Log.w(TAG, "scan: ${e.message}") }
        main.postDelayed({ if (scanning) { stopScan(); connectOrScan() } }, 15000)   // 15초마다 재시작
    }

    private fun stopScan() {
        if (!scanning) return
        try { adapter?.bluetoothLeScanner?.stopScan(scanCb) } catch (_: Exception) {}
        scanning = false
    }

    private fun connect(dev: BluetoothDevice, autoConnect: Boolean) {
        try { gatt?.close() } catch (_: Exception) {}
        gatt = if (Build.VERSION.SDK_INT >= 23) dev.connectGatt(ctx, autoConnect, gattCb, BluetoothDevice.TRANSPORT_LE)
               else dev.connectGatt(ctx, autoConnect, gattCb)
    }

    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                Log.i(TAG, "connected, discovering")
                g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                g.requestMtu(517)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                Log.i(TAG, "disconnected status=$status")
                ready = false; writing = false; txq.clear()
                onState(false, "BLE 끊김, 재시도")
                try { g.close() } catch (_: Exception) {}
                if (gatt === g) gatt = null
                if (running) main.postDelayed({ connectOrScan() }, 1000)
            }
        }
        override fun onMtuChanged(g: BluetoothGatt, m: Int, status: Int) {
            mtu = if (status == BluetoothGatt.GATT_SUCCESS) m else 23
            Log.i(TAG, "mtu $mtu")
            g.discoverServices()
        }
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val svc = g.getService(SVC)
            rx = svc?.getCharacteristic(RX)
            val tx = svc?.getCharacteristic(TX)
            if (rx == null || tx == null) { Log.w(TAG, "service missing"); g.disconnect(); return }
            g.setCharacteristicNotification(tx, true)
            val d = tx.getDescriptor(CCCD) ?: return
            if (Build.VERSION.SDK_INT >= 33) g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            else { @Suppress("DEPRECATION") run { d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE; g.writeDescriptor(d) } }
        }
        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            ready = true
            Log.i(TAG, "ready (mtu $mtu)")
            onState(true, "BLE 연결됨")
            main.post { pump() }
        }
        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            writing = false
            main.post { pump() }
        }
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) { onRx(value) }
        @Deprecated("pre-33")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33) @Suppress("DEPRECATION") onRx(c.value ?: return)
        }
    }

    private fun onRx(b: ByteArray) {
        for (x in b) {
            val ch = x.toInt().toChar()
            if (ch == '\n' || ch == '\r') {
                if (line.isNotEmpty()) { onLine(line.toString()); line.setLength(0) }
            } else if (line.length < 500) line.append(ch)
        }
    }
}
