package com.hejerd.decklink

import android.content.Intent
import android.net.Uri
import android.os.PowerManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import android.Manifest
import android.content.pm.PackageManager
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.appcompat.app.AppCompatActivity

/** 상태 확인 + 권한 열기 + 보드 IP 수동 입력. 리모컨(D-pad)으로 조작 가능하게 단순 구성. */
class MainActivity : AppCompatActivity() {
    private val ui = Handler(Looper.getMainLooper())
    private lateinit var tvStatus: TextView
    private lateinit var tvNow: TextView
    private lateinit var etHost: EditText

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        tvStatus = findViewById(R.id.tvStatus)
        tvNow = findViewById(R.id.tvNow)
        etHost = findViewById(R.id.etHost)
        etHost.setText(getSharedPreferences(DeckService.PREFS, MODE_PRIVATE).getString(DeckService.KEY_HOST, ""))

        findViewById<Button>(R.id.btnPerm).setOnClickListener {
            try { startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)) }
            catch (_: Exception) {
                Toast.makeText(this, "설정 화면 없음 — adb 로 허용 필요 (README 참고)", Toast.LENGTH_LONG).show()
            }
        }
        findViewById<Button>(R.id.btnSaveHost).setOnClickListener {
            val h = etHost.text.toString().trim()
            getSharedPreferences(DeckService.PREFS, MODE_PRIVATE).edit().putString(DeckService.KEY_HOST, h).apply()
            stopService(Intent(this, DeckService::class.java))
            DeckService.start(this)
            Toast.makeText(this, "보드 주소 저장: $h", Toast.LENGTH_SHORT).show()
        }
        findViewById<Button>(R.id.btnStart).setOnClickListener { DeckService.start(this) }
        findViewById<Button>(R.id.btnBattery).setOnClickListener { requestIgnoreBattery() }

        findViewById<Button>(R.id.btnTransport).setOnClickListener {
            val p = getSharedPreferences(DeckService.PREFS, MODE_PRIVATE)
            val now = p.getString(DeckService.KEY_TRANSPORT, "ble")
            val next = if (now == "ble") "wifi" else "ble"
            p.edit().putString(DeckService.KEY_TRANSPORT, next).apply()
            stopService(Intent(this, DeckService::class.java))
            DeckService.start(this)
            Toast.makeText(this, "전송: ${next.uppercase()}", Toast.LENGTH_SHORT).show()
        }

        val server = findViewById<EditText>(R.id.etPlexServer)
        val token = findViewById<EditText>(R.id.etPlexToken)
        server.setText(PlexSettings.load(this).server)
        fun updatePlexHint() {
            token.hint = if (PlexSettings.load(this).enabled) "저장됨 · 변경할 토큰 입력" else "Plex 토큰"
        }
        fun applyPlexChange() {
            token.text.clear()
            java.io.File(filesDir, "art2").deleteRecursively()
            stopService(Intent(this, DeckService::class.java))
            DeckService.start(this)
            updatePlexHint()
        }
        updatePlexHint()
        findViewById<Button>(R.id.btnSavePlex).setOnClickListener {
            try {
                val saved = PlexSettings.load(this)
                val entered = token.text.toString().trim()
                val address = PlexSettings.normalizeServer(server.text.toString())
                require(entered.isNotEmpty() || address == saved.server) { "서버 변경 시 토큰도 입력하세요." }
                PlexSettings.save(this, address, entered.ifEmpty { saved.token })
                applyPlexChange()
                Toast.makeText(this, "Plex 설정 저장됨", Toast.LENGTH_SHORT).show()
            } catch (e: IllegalArgumentException) {
                Toast.makeText(this, "서버 주소와 토큰을 확인하세요.", Toast.LENGTH_LONG).show()
            } catch (_: Exception) {
                Toast.makeText(this, "Plex 설정을 저장하지 못했습니다.", Toast.LENGTH_LONG).show()
            }
        }
        findViewById<Button>(R.id.btnClearPlex).setOnClickListener {
            PlexSettings.clear(this)
            server.text.clear()
            applyPlexChange()
            Toast.makeText(this, "Plex 설정 삭제됨", Toast.LENGTH_SHORT).show()
        }

        requestBlePermissions()
        DeckService.start(this)
        if (!isIgnoringBattery()) requestIgnoreBattery()
    }

    private fun requestBlePermissions() {
        val need = if (android.os.Build.VERSION.SDK_INT >= 31)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        val missing = need.filter { ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) ActivityCompat.requestPermissions(this, missing.toTypedArray(), 1)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        stopService(Intent(this, DeckService::class.java)); DeckService.start(this)
    }

    private fun isIgnoringBattery(): Boolean =
        (getSystemService(POWER_SERVICE) as PowerManager).isIgnoringBatteryOptimizations(packageName)

    /** 부팅 자동 시작·백그라운드 유지에 필요. 삼성은 추가로 앱 정보 → 배터리 → '제한 없음' */
    @android.annotation.SuppressLint("BatteryLife")
    private fun requestIgnoreBattery() {
        try {
            startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
        } catch (_: Exception) {
            try { startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)) } catch (_: Exception) {}
        }
    }

    private val refresh = object : Runnable {
        override fun run() {
            val perm = Settings.Secure.getString(contentResolver, "enabled_notification_listeners")?.contains(packageName) == true
            tvStatus.text = "알림 접근: ${if (perm) "허용" else "필요"}   배터리 최적화 제외: ${if (isIgnoringBattery()) "됨" else "필요"}\n전송: ${getSharedPreferences(DeckService.PREFS, MODE_PRIVATE).getString(DeckService.KEY_TRANSPORT, "ble")?.uppercase()}   보드: ${DeckService.status}"
            tvNow.text = DeckService.nowPlayingText
            ui.postDelayed(this, 1000)
        }
    }

    override fun onResume() { super.onResume(); ui.post(refresh) }
    override fun onPause() { super.onPause(); ui.removeCallbacks(refresh) }
}
