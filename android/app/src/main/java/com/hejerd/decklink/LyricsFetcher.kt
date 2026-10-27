package com.hejerd.decklink

import android.content.Context
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * 싱크 가사 — deck.py 의 lrclib_lrc / netease_lrc / parse_lrc / lyric_at 이식.
 * LRCLIB → NetEase 순. 한글 곡이면 한글 가사를 우선하고, 없으면 로마자라도 사용.
 * 결과는 filesDir/lrc/<key>.json 에 캐시 (가사 없음도 하루 동안 기억).
 */
object LyricsFetcher {
    const val TAG = "Lyrics"
    data class Line(val t: Double, val text: String)

    private val LRC_RE = Regex("\\[(\\d+):(\\d+(?:\\.\\d+)?)\\]")
    private val HANGUL = Regex("[가-힣]")
    private fun hasHangul(s: String?) = s != null && HANGUL.containsMatchIn(s)
    private fun norm(x: String) = x.lowercase().replace(Regex("[^0-9a-z가-힣]"), "")

    private fun variants(s: String): List<String> {
        val v = mutableListOf(s)
        Regex("^(.*?)\\s*[\\(（]([^)）]*)[\\)）]\\s*$").find(s)?.let { m ->
            val a = m.groupValues[1].trim(); val b = m.groupValues[2].trim()
            if (a.isNotEmpty()) v.add(a)
            if (b.isNotEmpty() && !Regex("prod|feat|ver|remix|inst", RegexOption.IGNORE_CASE).containsMatchIn(b)) v.add(b)
        }
        return v.filter { it.isNotEmpty() }.distinct()
    }

    // ---------- 캐시 ----------
    private fun dir(ctx: Context) = File(ctx.filesDir, "lrc2").apply { mkdirs() }
    private fun key(artist: String, title: String) = norm("${artist}_$title").take(80)

    fun fetch(ctx: Context, artist: String, title: String, durationSec: Int): List<Line> {
        val k = key(artist, title)
        if (k.isEmpty()) return emptyList()
        val f = File(dir(ctx), "$k.json")
        if (f.exists()) {
            try {
                val arr = JSONArray(f.readText())
                if (arr.length() > 0 || System.currentTimeMillis() - f.lastModified() < 24L * 3600 * 1000)
                    return (0 until arr.length()).map { i -> val o = arr.getJSONArray(i); Line(o.getDouble(0), o.getString(1)) }
            } catch (_: Exception) {}
        }
        val lrc = lookup(artist, title, durationSec)
        val lines = if (lrc != null) parse(lrc) else emptyList()
        try {
            val arr = JSONArray(); lines.forEach { arr.put(JSONArray().put(it.t).put(it.text)) }
            f.writeText(arr.toString())
        } catch (_: Exception) {}
        return lines
    }

    private fun lookup(artist: String, title: String, dur: Int): String? {
        val wantKo = hasHangul(artist) || hasHangul(title)
        var fallback: String? = null
        for ((name, fn) in listOf("lrclib" to { lrclib(artist, title, dur) }, "netease" to { netease(artist, title, dur) })) {
            val cand = try { fn() } catch (e: Exception) { Log.w(TAG, "$name: ${e.message}"); null } ?: continue
            if (wantKo && !hasHangul(cand)) { if (fallback == null) fallback = cand; continue }
            Log.i(TAG, "lyrics: $name"); return cand
        }
        if (fallback != null) Log.i(TAG, "lyrics: romanized fallback")
        return fallback
    }

    // ---------- HTTP ----------
    private fun get(u: String, headers: Map<String, String>): String? {
        var c: HttpURLConnection? = null
        return try {
            c = (URL(u).openConnection() as HttpURLConnection).apply {
                connectTimeout = 4000; readTimeout = 6000
                headers.forEach { (k, v) -> setRequestProperty(k, v) }
            }
            if (c.responseCode !in 200..299) null else c.inputStream.bufferedReader().use { it.readText() }
        } catch (e: Exception) { null } finally { c?.disconnect() }
    }
    private fun enc(s: String) = URLEncoder.encode(s, "UTF-8")

    private fun lrclib(artist: String, title: String, dur: Int): String? {
        val hdr = mapOf("User-Agent" to "DeckLink/1.0")
        val nt = norm(title)
        val wantKo = hasHangul(artist) || hasHangul(title)
        for (t in variants(title)) {
            // 1) 정확 조회
            get("https://lrclib.net/api/get?artist_name=${enc(artist)}&track_name=${enc(t)}&duration=$dur", hdr)?.let { body ->
                try {
                    val d = JSONObject(body)
                    val s = d.optString("syncedLyrics")
                    if (s.isNotEmpty() && (!wantKo || hasHangul(s))) return s
                } catch (_: Exception) {}
            }
            // 2) 가수+제목 검색 → 3) 제목만 검색 — 어느 쪽이든 가수가 일치해야 채택
            val na = norm(artist)
            for (q in listOf("track_name=${enc(t)}&artist_name=${enc(artist)}", "track_name=${enc(t)}")) {
                val res = try { JSONArray(get("https://lrclib.net/api/search?$q", hdr) ?: continue) } catch (_: Exception) { continue }
                val ntv = norm(t)
                for (i in 0 until res.length()) {
                    val d = res.getJSONObject(i); val s = d.optString("syncedLyrics")
                    val rn = norm(d.optString("trackName"))
                    val titleOk = ntv in rn || nt in rn
                    val artistOk = na.isEmpty() || ArtFetcher.artistMatch(artist, d.optString("artistName"))
                    val durOk = dur == 0 || Math.abs(d.optInt("duration") - dur) <= 5
                    if (s.isNotEmpty() && titleOk && artistOk && durOk && (!wantKo || hasHangul(s))) return s
                }
            }
        }
        return null
    }

    private fun netease(artist: String, title: String, dur: Int): String? {
        val hdr = mapOf("User-Agent" to "Mozilla/5.0", "Referer" to "https://music.163.com/")
        for (t in variants(title)) {
            val body = get("https://music.163.com/api/search/get?s=${enc("$artist $t")}&type=1&limit=8", hdr) ?: continue
            val songs = try { JSONObject(body).optJSONObject("result")?.optJSONArray("songs") } catch (_: Exception) { null } ?: continue
            val ntv = norm(t)
            var best: JSONObject? = null
            val na = norm(artist)
            for (i in 0 until songs.length()) {
                val s = songs.getJSONObject(i)
                val name = norm(s.optString("name")); val d = s.optLong("duration") / 1000.0
                // 가수 검증: artists[].name 중 하나라도 일치
                val ars = s.optJSONArray("artists")
                var artistOk = na.isEmpty()
                if (ars != null) for (j in 0 until ars.length()) if (ArtFetcher.artistMatch(artist, ars.getJSONObject(j).optString("name"))) artistOk = true
                if ((ntv in name || name in ntv) && artistOk) {
                    if (dur == 0 || Math.abs(d - dur) <= 5) { best = s; break }
                    if (best == null) best = s
                }
            }
            val id = best?.optLong("id") ?: continue
            val lb = get("https://music.163.com/api/song/lyric?id=$id&lv=1&kv=0&tv=0", hdr) ?: continue
            val lrc = try { JSONObject(lb).optJSONObject("lrc")?.optString("lyric") } catch (_: Exception) { null } ?: continue
            if (lrc.isNotEmpty() && LRC_RE.containsMatchIn(lrc)) return lrc
        }
        return null
    }

    // ---------- LRC ----------
    fun parse(text: String): List<Line> {
        val out = ArrayList<Line>()
        for (line in text.lines()) {
            val stamps = LRC_RE.findAll(line).toList()
            if (stamps.isEmpty()) continue
            val lyric = LRC_RE.replace(line, "").trim()
            for (m in stamps) out.add(Line(m.groupValues[1].toInt() * 60 + m.groupValues[2].toDouble(), lyric))
        }
        return out.sortedBy { it.t }
    }

    /** (이전, 현재, 다음) */
    fun at(lines: List<Line>, t: Double): Triple<String, String, String> {
        if (lines.isEmpty()) return Triple("", "", "")
        var idx = -1
        for (i in lines.indices) { if (lines[i].t <= t) idx = i else break }
        if (idx < 0) return Triple("", "", lines[0].text)
        val prev = if (idx > 0) lines[idx - 1].text else ""
        val next = if (idx + 1 < lines.size) lines[idx + 1].text else ""
        return Triple(prev, lines[idx].text, next)
    }
}
