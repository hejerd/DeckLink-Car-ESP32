package com.hejerd.decklink

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.MediaMetadata
import android.net.Uri
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * 앨범아트 확보 — deck.py 의 lookup_song 로직 이식.
 *
 *  - 제목이 "가수 - 제목" 꼴이면 쪼갠다 (Plex 차트 모음집은 제목에 가수가 들어 있음)
 *  - 앨범/아티스트가 차트·플레이리스트·모음집(COMP_RE)이면 세션이 주는 커버는
 *    모음집 공용 이미지라 쓰지 않고, iTunes → Deezer 로 곡 커버를 찾는다
 *  - 일반 앨범이면 세션 비트맵 → URI → 알림 아이콘 → iTunes → Deezer
 *  - 결과는 filesDir/art/ 에 캐시
 */
object ArtFetcher {
    const val TAG = "ArtFetcher"

    /** 패키지별 최근 미디어 알림 큰 아이콘 (DeckNotifListener 가 채움) */
    @Volatile var notifArt: Map<String, Bitmap> = emptyMap()
    /** 마지막 조회 진단 (앱 화면 표시용) */
    @Volatile var lastDebug: String = ""

    data class Result(val bitmap: Bitmap?, val source: String)
    data class Song(val artist: String, val title: String, val isComp: Boolean)

    private val COMP_RE = Regex("차트|chart|top ?100|playlist|플레이리스트|주간|월간|various|모음", RegexOption.IGNORE_CASE)

    // ---------- 제목/가수 정리 (deck.py 와 동일) ----------
    fun cleanTitle(t: String): String = t
        .replace(Regex("\\s*[\\(\\[【].*?(mv|m/v|official|audio|lyric|가사|live|ver).*?[\\)\\]】]", RegexOption.IGNORE_CASE), "")
        .replace(Regex("\\s+"), " ").trim()

    fun splitArtistTitle(title: String, artist: String): Pair<String, String> {
        val t = cleanTitle(title)
        for (sep in listOf(" - ", " – ", " — ")) {
            val i = t.indexOf(sep)
            if (i > 0) return t.substring(0, i).trim() to t.substring(i + sep.length).trim()
        }
        return artist.trim() to t
    }

    fun song(m: MediaMetadata): Song {
        val rawTitle = m.getString(MediaMetadata.METADATA_KEY_TITLE) ?: ""
        val rawArtist = m.getString(MediaMetadata.METADATA_KEY_ARTIST) ?: ""
        val album = m.getString(MediaMetadata.METADATA_KEY_ALBUM) ?: ""
        val albumArtist = m.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST) ?: ""
        var (a, t) = splitArtistTitle(rawTitle, rawArtist.ifEmpty { albumArtist })
        val isComp = COMP_RE.containsMatchIn("$album $albumArtist $rawArtist") || rawTitle.contains(" - ")
        if (COMP_RE.containsMatchIn(a)) a = ""          // "멜론 주간 차트" 같은 모음집 이름은 가수가 아님
        return Song(a, t, isComp)
    }

    private fun norm(x: String) = x.lowercase().replace(Regex("[^0-9a-z가-힣]"), "")

    /** 가수 이름 비교 — 정규화 후 포함 관계, 괄호 변형(아이오아이 (I.O.I)), 쉼표/&/feat 로 나뉜 복수 가수 각각 */
    fun artistMatch(want: String, got: String): Boolean {
        val w = norm(want); val g = norm(got)
        if (w.isEmpty() || g.isEmpty()) return false
        if (w in g || g in w) return true
        val parts = { s: String -> s.split(Regex("[,&/]|feat\\.?|ft\\.?|\\(|\\)|x\\s", RegexOption.IGNORE_CASE)).map { norm(it) }.filter { it.length >= 2 } }
        val wp = parts(want); val gp = parts(got)
        return wp.any { a -> gp.any { b -> a in b || b in a } }
    }

    /** '뛰어(JUMP)' → ['뛰어(JUMP)', '뛰어', 'JUMP'] */
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
    private fun cacheKey(s: Song): String? {
        val k = norm("${s.artist}_${s.title}")
        return if (k.isEmpty()) null else k.take(80)
    }
    private fun cacheDir(ctx: Context) = File(ctx.filesDir, "art2").apply { mkdirs() }
    private fun cacheLoad(ctx: Context, key: String): Bitmap? =
        File(cacheDir(ctx), "$key.png").takeIf { it.exists() }?.let { BitmapFactory.decodeFile(it.path) }
    private fun cacheSave(ctx: Context, key: String, b: Bitmap) {
        try {
            val s = if (b.width > 300) Bitmap.createScaledBitmap(b, 300, 300 * b.height / b.width, true) else b
            FileOutputStream(File(cacheDir(ctx), "$key.png")).use { s.compress(Bitmap.CompressFormat.PNG, 90, it) }
        } catch (e: Exception) { Log.w(TAG, "cache save: ${e.message}") }
    }
    private fun cacheNone(ctx: Context, key: String) { try { File(cacheDir(ctx), "$key.none").writeText("") } catch (_: Exception) {} }
    private fun cacheIsNone(ctx: Context, key: String): Boolean {
        val f = File(cacheDir(ctx), "$key.none")
        return f.exists() && System.currentTimeMillis() - f.lastModified() < 24L * 3600 * 1000
    }

    // ---------- 진입점 ----------
    fun fetch(ctx: Context, m: MediaMetadata?, pkg: String?): Result {
        if (m == null) return Result(null, "메타 없음")
        val s = song(m)
        val key = cacheKey(s)
        if (key != null) cacheLoad(ctx, key)?.let { return Result(it, "캐시") }
        val r = fetchUncached(ctx, m, pkg, s, key)
        if (key != null) { if (r.bitmap != null) cacheSave(ctx, key, r.bitmap) else cacheNone(ctx, key) }
        return r
    }

    private fun fetchUncached(ctx: Context, m: MediaMetadata, pkg: String?, s: Song, key: String?): Result {
        val sessionArt = { sessionBitmap(m) ?: sessionUri(ctx, m) ?: pkg?.let { notifArt[it] } }
        val skipOnline = key != null && cacheIsNone(ctx, key)

        // Plex 라이브러리에서 정식 앨범 트랙 검색 (deck.py plex_find_track)
        val track = if (skipOnline) null else try { PlexFetcher.findTrack(ctx, s.artist, s.title) } catch (e: Exception) { null }
        val plexGood = track?.artistMatch == true && !COMP_RE.containsMatchIn(track.album)
        val tryPlex: () -> Pair<Bitmap, String>? = { if (track == null || COMP_RE.containsMatchIn(track.album)) null else PlexFetcher.art(ctx, track)?.let { it to "Plex" } }
        val tryItunes: () -> Pair<Bitmap, String>? = { if (skipOnline) null else itunes(s.artist, s.title) }
        val tryDeezer: () -> Pair<Bitmap, String>? = { if (skipOnline) null else deezer(s.artist, s.title) }
        val trySession: () -> Pair<Bitmap, String>? = { sessionArt()?.let { it to (if (s.isComp) "세션(모음집)" else "세션") } }

        // deck.py 순서: 제목+가수 다 맞는 Plex 트랙이 있으면 Plex 우선, 아니면 iTunes → Deezer → Plex.
        // 일반 앨범(모음집 아님)은 세션 커버가 곧 정답이라 세션 먼저.
        val order: List<() -> Pair<Bitmap, String>?> = when {
            !s.isComp -> listOf(trySession, tryPlex, tryItunes, tryDeezer)
            plexGood  -> listOf(tryPlex, tryItunes, tryDeezer, trySession)
            else      -> listOf(tryItunes, tryDeezer, tryPlex, trySession)
        }
        for (fn in order) {
            val r = try { fn() } catch (e: Exception) { Log.w(TAG, "art: ${e.message}"); null }
            if (r != null && r.first.width > 8) return Result(r.first, r.second)
        }
        return Result(null, "없음 ${lastDebug}")
    }

    private fun sessionBitmap(m: MediaMetadata): Bitmap? {
        for (k in listOf(MediaMetadata.METADATA_KEY_ALBUM_ART, MediaMetadata.METADATA_KEY_ART, MediaMetadata.METADATA_KEY_DISPLAY_ICON)) {
            val b = try { m.getBitmap(k) } catch (_: Exception) { null }
            if (b != null && b.width > 8) return b
        }
        return null
    }

    private fun sessionUri(ctx: Context, m: MediaMetadata): Bitmap? {
        for (k in listOf(MediaMetadata.METADATA_KEY_ALBUM_ART_URI, MediaMetadata.METADATA_KEY_ART_URI, MediaMetadata.METADATA_KEY_DISPLAY_ICON_URI)) {
            val u = m.getString(k) ?: continue
            loadUri(ctx, u)?.let { return it }
        }
        return null
    }

    private fun loadUri(ctx: Context, u: String): Bitmap? = try {
        val uri = Uri.parse(u)
        when (uri.scheme) {
            "http", "https" -> download(u)
            "content", "file", "android.resource" -> ctx.contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it) }
            else -> null
        }
    } catch (e: Exception) { Log.w(TAG, "Artwork URI request failed"); null }

    private fun download(u: String): Bitmap? {
        var c: HttpURLConnection? = null
        return try {
            c = (URL(u).openConnection() as HttpURLConnection).apply {
                connectTimeout = 4000; readTimeout = 6000
                setRequestProperty("User-Agent", "DeckLink/1.0")
            }
            if (c.responseCode !in 200..299) null else c.inputStream.use { BitmapFactory.decodeStream(it) }
        } catch (e: Exception) { Log.w(TAG, "dl ${e.message}"); null } finally { c?.disconnect() }
    }

    private fun httpJson(u: String): JSONObject? {
        var c: HttpURLConnection? = null
        return try {
            c = (URL(u).openConnection() as HttpURLConnection).apply { connectTimeout = 4000; readTimeout = 6000 }
            JSONObject(c.inputStream.bufferedReader().use { it.readText() })
        } catch (e: Exception) { Log.w(TAG, "json ${e.message}"); null } finally { c?.disconnect() }
    }

    /** deck.py itunes_art: 가수/제목 변형 × (가수+제목@us, 제목@us, 가수+제목@kr), 결과를 제목·가수로 검증 */
    private fun itunes(artist: String, title: String): Pair<Bitmap, String>? {
        val nt = norm(title); val na = norm(artist)
        val tried = HashSet<String>()
        for (a in variants(artist)) for (t in variants(title)) {
            for ((term, country) in listOf("$a $t" to "us", t to "us", "$a $t" to "kr")) {
                if (!tried.add("$term@$country")) continue
                val url = "https://itunes.apple.com/search?term=${URLEncoder.encode(term, "UTF-8")}&country=$country&media=music&entity=song&limit=10"
                val res = httpJson(url)?.optJSONArray("results") ?: continue
                Log.i(TAG, "itunes q=($term @$country) -> ${res.length()}")
                for (i in 0 until res.length()) {
                    val r = res.getJSONObject(i)
                    val rt = norm(r.optString("trackName")); val ra = norm(r.optString("artistName"))
                    val tv = norm(t); val av = norm(a)
                    val titleOk = tv in rt || rt in tv || nt in rt
                    val ra0 = r.optString("artistName")
                    val artistOk = artistMatch(a, ra0) || artistMatch(artist, ra0)
                    // 가수 일치 우선. 표기가 달라 못 맞추는 경우(한로로/HANRORO)는
                    // 가수+제목으로 검색한 1순위 결과가 제목 정확 일치일 때만 허용 (deck.py 규칙)
                    val ok = if (na.isEmpty()) titleOk
                             else (titleOk && artistOk) || (i == 0 && term != t && tv == rt)
                    if (i == 0) lastDebug = "iT[$country] ${ra0} / ${r.optString("trackName")} ${if (ok) "OK" else "X"}"
                    if (ok) {
                        val art = r.optString("artworkUrl100")
                        if (art.isNotEmpty()) download(art.replace("100x100", "300x300"))?.let { return it to "iTunes" }
                    }
                }
            }
        }
        return null
    }

    /** deck.py deezer_art */
    private fun deezer(artist: String, title: String): Pair<Bitmap, String>? {
        val nt = norm(title)
        for (a in variants(artist)) for (t in variants(title)) {
            val q = URLEncoder.encode("artist:\"$a\" track:\"$t\"", "UTF-8")
            val res = httpJson("https://api.deezer.com/search?q=$q&limit=5")?.optJSONArray("data") ?: continue
            for (i in 0 until res.length()) {
                val r = res.getJSONObject(i)
                val rt = norm(r.optString("title"))
                val ra = r.optJSONObject("artist")?.optString("name") ?: ""
                val artistOk = norm(artist).isEmpty() || artistMatch(a, ra) || artistMatch(artist, ra)
                if ((norm(t) in rt || rt in norm(t) || nt in rt) && artistOk) {
                    val al = r.optJSONObject("album")
                    val cover = al?.optString("cover_big").orEmpty().ifEmpty { al?.optString("cover_medium").orEmpty() }
                    if (cover.isNotEmpty()) download(cover)?.let { return it to "Deezer" }
                }
            }
        }
        return null
    }
}
