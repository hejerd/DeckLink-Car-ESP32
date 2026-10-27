package com.hejerd.decklink

import android.util.Log
import java.io.BufferedOutputStream
import java.io.InputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * 보드(ESP32) TCP 클라이언트. deck.py 와 같은 줄 프로토콜.
 *  보내기: 텍스트 줄(M:/A:/G:/L:/D:/T:) + A: 뒤 바이너리
 *  받기  : C:PLAY / C:NEXT / C:PREV / W: / P:n / O:n / X:...
 */
class DeckClient(
    private val onLine: (String) -> Unit,
    private val onState: (Boolean, String) -> Unit,   // connected?, 설명
) : DeckLink {
    companion object { const val TAG = "DeckClient" }

    @Volatile var host: String = ""
    @Volatile var port: Int = 5555
    @Volatile private var running = false
    private var socket: Socket? = null
    private val txq = LinkedBlockingQueue<ByteArray>()
    private var rxThread: Thread? = null
    private var txThread: Thread? = null

    override val connected: Boolean get() = socket?.isConnected == true && socket?.isClosed == false

    override fun start() {
        if (running) return
        running = true
        rxThread = Thread(::rxLoop, "deck-rx").apply { isDaemon = true; start() }
        txThread = Thread(::txLoop, "deck-tx").apply { isDaemon = true; start() }
    }

    override fun stop() {
        running = false
        closeSocket()
        rxThread?.interrupt(); txThread?.interrupt()
    }

    /** 텍스트 한 줄 (개행 자동) */
    override fun sendLine(s: String) = send((s + "\n").toByteArray(Charsets.UTF_8))

    /** 바이너리 그대로 (A: 뒤 아트 등) */
    override fun send(b: ByteArray) {
        if (!connected) return
        if (txq.size > 64) txq.clear()          // 밀리면 오래된 것 버림
        txq.offer(b)
    }

    private fun closeSocket() {
        try { socket?.close() } catch (_: Exception) {}
        socket = null
    }

    private fun rxLoop() {
        while (running) {
            val h = host
            if (h.isEmpty()) { onState(false, "보드 주소 없음"); sleep(1000); continue }
            try {
                onState(false, "연결 시도 $h:$port")
                val s = Socket()
                s.tcpNoDelay = true
                s.keepAlive = true
                s.connect(InetSocketAddress(h, port), 3000)
                s.soTimeout = 0
                socket = s
                txq.clear()
                onState(true, "연결됨 $h:$port")
                readLines(s.getInputStream())
            } catch (e: Exception) {
                Log.w(TAG, "conn: ${e.message}")
            }
            closeSocket()
            onState(false, "끊김, 재시도")
            sleep(2000)
        }
    }

    private fun readLines(inp: InputStream) {
        val buf = ByteArray(512)
        val line = StringBuilder()
        while (running) {
            val n = inp.read(buf)
            if (n < 0) return
            for (i in 0 until n) {
                val c = buf[i].toInt().toChar()
                if (c == '\n' || c == '\r') {
                    if (line.isNotEmpty()) { onLine(line.toString()); line.setLength(0) }
                } else if (line.length < 500) line.append(c)
            }
        }
    }

    private fun txLoop() {
        while (running) {
            val b = try { txq.poll(500, TimeUnit.MILLISECONDS) } catch (_: InterruptedException) { null } ?: continue
            val s = socket ?: continue
            try {
                val out = s.getOutputStream()
                out.write(b); out.flush()
            } catch (e: Exception) {
                Log.w(TAG, "tx: ${e.message}")
                closeSocket()
            }
        }
    }

    private fun sleep(ms: Long) { try { Thread.sleep(ms) } catch (_: InterruptedException) {} }
}
