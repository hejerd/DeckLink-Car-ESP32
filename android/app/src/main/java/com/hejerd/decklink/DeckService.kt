package com.hejerd.decklink

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import java.io.ByteArrayOutputStream

/**
 * deck.py 의 PC 역할. 재생 중인 미디어 세션(Plexamp/YouTube Music 등)을 읽어 보드로 보내고,
 * 보드 제스처(C:PLAY/NEXT/PREV)를 미디어 컨트롤로 넘긴다.
 */
class DeckService : Service() {
    companion object {
        const val TAG = "DeckService"
        const val CH_ID = "decklink"
        const val PREFS = "decklink"
        const val KEY_HOST = "host"
        const val KEY_TRANSPORT = "transport"     // "ble" | "wifi"
        const val SERVICE_TYPE = "_deck._tcp."
        const val ART_W = 100
        const val ART_H = 100

        @Volatile var status: String = "대기"
        @Volatile var nowPlayingText: String = "-"
        var onStatus: ((String) -> Unit)? = null

        fun start(ctx: Context) {
            val i = Intent(ctx, DeckService::class.java)
            if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(i) else ctx.startService(i)
        }
    }

    private val main = Handler(Looper.getMainLooper())
    private lateinit var msm: MediaSessionManager
    private lateinit var nsd: NsdManager
    private var client: DeckLink? = null
    private var tcp: DeckClient? = null
    private var controller: MediaController? = null
    private var lastMetaKey: String? = null

    // ---------- 생명주기 ----------
    override fun onCreate() {
        super.onCreate()
        startForeground(1, buildNotification("시작 중"))
        msm = getSystemService(Context.MEDIA_SESSION_SERVICE) as MediaSessionManager
        nsd = getSystemService(Context.NSD_SERVICE) as NsdManager

        val onState: (Boolean, String) -> Unit = { ok, msg ->
            setStatus(if (ok) "보드 연결됨 ($msg)" else msg)
            if (ok) main.post { client?.sendLine("D:1"); lastLyric = null; resendAll() }
        }
        if (prefs().getString(KEY_TRANSPORT, "ble") == "wifi") {
            val t = DeckClient(::onBoardLine, onState)
            t.host = prefs().getString(KEY_HOST, "") ?: ""
            tcp = t; client = t
            startDiscovery()
        } else {
            client = BleClient(this, ::onBoardLine, onState)
        }
        client?.start()

        val cn = ComponentName(this, DeckNotifListener::class.java)
        try {
            msm.addOnActiveSessionsChangedListener({ pickController(it) }, cn, main)
            pickController(msm.getActiveSessions(cn))
        } catch (e: SecurityException) {
            setStatus("알림 접근 권한 필요")
        }
        main.post(tick)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_STICKY
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        main.removeCallbacks(tick)
        client?.stop()
        try { nsd.stopServiceDiscovery(discoveryListener) } catch (_: Exception) {}
        super.onDestroy()
    }

    private fun prefs() = getSharedPreferences(PREFS, MODE_PRIVATE)

    private fun setStatus(s: String) {
        status = s
        main.post {
            onStatus?.invoke(s)
            (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).notify(1, buildNotification(s))
        }
    }

    private fun buildNotification(text: String): Notification {
        val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= 26 && nm.getNotificationChannel(CH_ID) == null)
            nm.createNotificationChannel(NotificationChannel(CH_ID, "DeckLink", NotificationManager.IMPORTANCE_LOW))
        val b = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(this, CH_ID) else Notification.Builder(this)
        return b.setContentTitle("DeckLink").setContentText(text)
            .setSmallIcon(android.R.drawable.ic_media_play).setOngoing(true).build()
    }

    // ---------- 보드 찾기 (mDNS) ----------
    private val discoveryListener = object : NsdManager.DiscoveryListener {
        override fun onStartDiscoveryFailed(t: String?, e: Int) { Log.w(TAG, "nsd start fail $e") }
        override fun onStopDiscoveryFailed(t: String?, e: Int) {}
        override fun onDiscoveryStarted(t: String?) {}
        override fun onDiscoveryStopped(t: String?) {}
        override fun onServiceLost(s: NsdServiceInfo?) {}
        override fun onServiceFound(s: NsdServiceInfo) {
            if (!s.serviceType.startsWith("_deck.")) return
            @Suppress("DEPRECATION")
            nsd.resolveService(s, object : NsdManager.ResolveListener {
                override fun onResolveFailed(i: NsdServiceInfo?, e: Int) {}
                override fun onServiceResolved(i: NsdServiceInfo) {
                    val host = i.host?.hostAddress ?: return
                    Log.i(TAG, "deck found $host:${i.port}")
                    prefs().edit().putString(KEY_HOST, host).apply()
                    tcp?.host = host
                    tcp?.port = i.port
                }
            })
        }
    }

    private fun startDiscovery() {
        try { nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discoveryListener) }
        catch (e: Exception) { Log.w(TAG, "nsd: ${e.message}") }
    }

    // ---------- 미디어 세션 ----------
    private val ctrlCallback = object : MediaController.Callback() {
        override fun onMetadataChanged(m: MediaMetadata?) { sendMeta(force = false) }
        override fun onPlaybackStateChanged(s: PlaybackState?) { sendNow() }
        override fun onSessionDestroyed() { pickController(null) }
    }

    /** 재생 중인 세션 우선, 없으면 첫 세션 */
    private fun pickController(list: List<MediaController>?) {
        val l = list ?: try { msm.getActiveSessions(ComponentName(this, DeckNotifListener::class.java)) } catch (_: Exception) { emptyList() }
        val playing = l.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING }
        val next = playing ?: l.firstOrNull()
        if (next?.sessionToken == controller?.sessionToken && next != null) return
        controller?.unregisterCallback(ctrlCallback)
        controller = next
        next?.registerCallback(ctrlCallback, main)
        Log.i(TAG, "controller = ${next?.packageName}")
        lastMetaKey = null; lyricsKey = null
        resendAll()
    }

    private val tick = object : Runnable {
        override fun run() {
            // 재생 중일 때만 1초마다 진행 시간 갱신; 세션이 바뀌었는지도 확인
            if (controller == null || controller?.playbackState?.state != PlaybackState.STATE_PLAYING) pickController(null)
            sendNow()
            // 아트를 못 구했는데 그 사이 미디어 알림 아이콘이 들어왔으면 다시 시도
            val pkg = controller?.packageName
            if (artStatus == "아트: 없음" && pkg != null && ArtFetcher.notifArt.containsKey(pkg)) { artStatus = "아트: 재시도"; sendMeta(force = true) }
            sendLyric()
            main.postDelayed(this, 500)
        }
    }

    private fun resendAll() { sendMeta(force = true); sendNow() }

    /** 재생 위치(초, 실수) — position 은 lastPositionUpdateTime 기준이라 보정 */
    private fun positionSec(): Double {
        val st = controller?.playbackState ?: return 0.0
        var p = st.position.toDouble()
        if (st.state == PlaybackState.STATE_PLAYING)
            p += (android.os.SystemClock.elapsedRealtime() - st.lastPositionUpdateTime) * st.playbackSpeed
        return p / 1000.0
    }

    // ---------- 가사 ----------
    @Volatile private var lyrics: List<LyricsFetcher.Line> = emptyList()
    private var lyricsKey: String? = null
    private var lastLyric: Triple<String, String, String>? = null
    @Volatile var lyricStatus: String = "-"

    private fun loadLyrics(song: ArtFetcher.Song, dur: Int) {
        val k = "${song.artist}|${song.title}"
        if (k == lyricsKey) return
        lyricsKey = k
        lyrics = emptyList(); lastLyric = null
        client?.sendLine("L:||")
        lyricStatus = "가사: 조회"
        Thread {
            val l = LyricsFetcher.fetch(this, song.artist, song.title, dur)
            if (lyricsKey == k) { lyrics = l; lyricStatus = if (l.isEmpty()) "가사: 없음" else "가사: ${l.size}줄" }
        }.apply { isDaemon = true }.start()
    }

    private fun sendLyric() {
        val l = lyrics
        if (l.isEmpty()) return
        val ly = LyricsFetcher.at(l, positionSec())
        if (ly == lastLyric) return
        lastLyric = ly
        val c = { x: String -> x.replace('|', ' ').replace("\n", " ") }
        client?.sendLine("L:${c(ly.first)}|${c(ly.second)}|${c(ly.third)}")
    }

    /** M:제목|아티스트|pos|dur|playing */
    private fun sendNow() {
        val c = controller
        val m = c?.metadata
        val st = c?.playbackState
        // "가수 - 제목" 꼴(Plex 차트 모음집)은 쪼개서 보냄
        val song = m?.let { ArtFetcher.song(it) }
        val title = song?.title ?: ""
        val artist = song?.artist ?: ""
        val dur = ((m?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L) / 1000).toInt()
        var pos = positionSec().toInt()
        val playing = st?.state == PlaybackState.STATE_PLAYING
        if (dur > 0 && pos > dur) pos = dur
        if (song != null && title.isNotEmpty()) loadLyrics(song, dur)
        nowPlayingText = (if (title.isEmpty()) "-" else "$artist - $title  ${pos}/${dur}s ${if (playing) "▶" else "❚❚"}") + "   [$artStatus, $lyricStatus]   보드 명령: $lastCmd"
        client?.sendLine("M:${clean(title)}|${clean(artist)}|$pos|$dur|${if (playing) 1 else 0}")
    }

    /** 앨범아트(A: + RGB565 BE 바이너리) + 배경/글자색(G:) — 곡이 바뀔 때만. 네트워크 조회가 있어 백그라운드 스레드 */
    @Volatile var artStatus: String = "-"
    private var artSeq = 0
    private fun sendMeta(force: Boolean) {
        val m = controller?.metadata ?: return
        val pkg = controller?.packageName
        val key = (m.getString(MediaMetadata.METADATA_KEY_TITLE) ?: "") + "|" + (m.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: "")
        if (!force && key == lastMetaKey) return
        lastMetaKey = key
        val seq = ++artSeq
        Thread {
            val r = ArtFetcher.fetch(this, m, pkg)
            if (seq != artSeq) return@Thread                  // 그 사이 곡이 또 바뀜
            artStatus = "아트: ${r.source}"
            val bmp = r.bitmap
            if (bmp == null) { Log.i(TAG, "no art for $key"); return@Thread }
            sendArtBitmap(bmp)
        }.apply { isDaemon = true }.start()
    }

    private fun sendArtBitmap(bmp: Bitmap) {
        val small = Bitmap.createScaledBitmap(bmp, ART_W, ART_H, true)
        val bytes = ByteArrayOutputStream(ART_W * ART_H * 2)
        var rs = 0L; var gs = 0L; var bs = 0L
        val px = IntArray(ART_W * ART_H)
        small.getPixels(px, 0, ART_W, 0, 0, ART_W, ART_H)
        for (p in px) {
            val r = (p shr 16) and 0xFF; val g = (p shr 8) and 0xFF; val b = p and 0xFF
            rs += r; gs += g; bs += b
            val v = ((r and 0xF8) shl 8) or ((g and 0xFC) shl 3) or (b shr 3)
            bytes.write((v shr 8) and 0xFF); bytes.write(v and 0xFF)     // LV_COLOR_16_SWAP: 상위 바이트 먼저
        }
        val n = px.size
        val ar = (rs / n).toInt(); val ag = (gs / n).toInt(); val ab = (bs / n).toInt()
        // 배경 = 평균색을 어둡게, 글자 = 밝기에 따라 흰/검
        val bgR = ar * 6 / 10; val bgG = ag * 6 / 10; val bgB = ab * 6 / 10
        val lum = (bgR * 299 + bgG * 587 + bgB * 114) / 1000
        val fg = if (lum > 140) 0 else 255
        client?.sendLine("G:$bgR,$bgG,$bgB,$fg,$fg,$fg")
        // 헤더와 바이너리를 한 덩어리로 — 사이에 M: 줄이 끼어들면 보드가 이미지를 깨뜨림
        val data = bytes.toByteArray()
        val header = "A:${data.size}\n".toByteArray(Charsets.UTF_8)
        client?.send(header + data)
        Log.i(TAG, "art sent ${data.size}")
    }

    private fun clean(s: String) = s.replace('|', '/').replace("\n", " ").take(90)

    // ---------- 보드 → 앱 ----------
    @Volatile var lastCmd: String = "-"
    private var lastPlayCmdAt = 0L
    private fun onBoardLine(line: String) {
        Log.d(TAG, "rx $line")
        val tc = controller?.transportControls
        lastCmd = "$line ${java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.KOREA).format(java.util.Date())}"
        when {
            line == "C:PLAY" -> {
                val now = System.currentTimeMillis()
                if (now - lastPlayCmdAt < 1500) { lastCmd += " (무시)"; return }   // 연속 토글 방지
                lastPlayCmdAt = now
                if (controller?.playbackState?.state == PlaybackState.STATE_PLAYING) tc?.pause() else tc?.play()
            }
            line == "C:NEXT" -> tc?.skipToNext()
            line == "C:PREV" -> tc?.skipToPrevious()
            line.startsWith("W:") -> client?.sendLine("D:1")
            line.startsWith("P:") -> { /* 버튼 페이지: 차량용은 아직 미사용 */ }
        }
    }
}
