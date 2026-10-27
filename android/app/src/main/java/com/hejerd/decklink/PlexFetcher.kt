package com.hejerd.decklink

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.util.Log
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * Plex 라이브러리에서 곡을 찾아 정식 앨범 커버를 가져온다 — deck.py plex_find_track / plex_art 이식.
 * 차트 모음집 폴더의 곡은 커버가 공용이라, 같은 곡이 정식 앨범에도 있으면 그쪽 커버를 쓴다.
 */
object PlexFetcher {
    const val TAG = "Plex"

    data class Track(val thumb: String?, val parentThumb: String?, val artistMatch: Boolean, val album: String)

    private val COMP_RE = Regex("차트|chart|top ?100|playlist|플레이리스트|주간|월간|various|모음", RegexOption.IGNORE_CASE)
    private fun norm(x: String) = x.lowercase().replace(Regex("[^0-9a-z가-힣]"), "")

    private fun httpJson(u: String, token: String): JSONObject? {
        var c: HttpURLConnection? = null
        return try {
            c = (URL(u).openConnection() as HttpURLConnection).apply {
                connectTimeout = 4000; readTimeout = 6000
                setRequestProperty("Accept", "application/json")
                setRequestProperty("X-Plex-Token", token)
                instanceFollowRedirects = false
            }
            if (c.responseCode !in 200..299) null else JSONObject(c.inputStream.bufferedReader().use { it.readText() })
        } catch (e: Exception) { Log.w(TAG, "Plex request failed"); null } finally { c?.disconnect() }
    }

    /** 제목으로 검색 → 제목/가수 점수 매겨 최선 트랙. 점수 ≥5 면 가수까지 맞음 */
    fun findTrack(ctx: Context, artist: String, title: String): Track? {
        val config = PlexSettings.load(ctx)
        if (!config.enabled || title.isBlank()) return null
        val url = "${config.server}/hubs/search?query=${URLEncoder.encode(title, "UTF-8")}&limit=20"
        val hubs = httpJson(url, config.token)?.optJSONObject("MediaContainer")?.optJSONArray("Hub") ?: return null
        val na = norm(artist); val nt = norm(title)
        var bestScore = 0; var best: JSONObject? = null
        for (i in 0 until hubs.length()) {
            val h = hubs.getJSONObject(i)
            if (h.optString("type") != "track") continue
            val md = h.optJSONArray("Metadata") ?: continue
            for (j in 0 until md.length()) {
                val m = md.getJSONObject(j)
                val ma = norm(m.optString("grandparentTitle") + m.optString("originalTitle"))
                val mt = norm(m.optString("title"))
                val album = m.optString("parentTitle") + m.optString("grandparentTitle")
                var score = 0
                if (nt.isNotEmpty() && (nt in mt || mt in nt)) score += 2
                if (na.isNotEmpty() && (na in ma || ma in na)) score += 3        // 진짜 아티스트 앨범이면 크게 가산
                if (COMP_RE.containsMatchIn(album)) score -= 2                     // 차트/모음집 폴더는 감점
                if (best == null || score > bestScore) { bestScore = score; best = m }
            }
        }
        val m = best ?: return null
        if (bestScore <= 0) return null
        return Track(m.optString("thumb").ifEmpty { null }, m.optString("parentThumb").ifEmpty { null },
                     bestScore >= 5, m.optString("parentTitle"))
    }

    fun art(ctx: Context, t: Track): Bitmap? {
        val config = PlexSettings.load(ctx)
        if (!config.enabled) return null
        for (thumb in listOf(t.thumb, t.parentThumb)) {
            if (thumb.isNullOrEmpty() || !thumb.startsWith("/") || thumb.startsWith("//")) continue
            var c: HttpURLConnection? = null
            try {
                c = (URL("${config.server}$thumb").openConnection() as HttpURLConnection).apply { connectTimeout = 4000; readTimeout = 8000
                    instanceFollowRedirects = false
                    setRequestProperty("X-Plex-Token", config.token) }
                if (c.responseCode in 200..299) c.inputStream.use { BitmapFactory.decodeStream(it) }?.let { return it }
            } catch (e: Exception) { Log.w(TAG, "Plex artwork request failed") } finally { c?.disconnect() }
        }
        return null
    }
}
