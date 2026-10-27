package com.hejerd.decklink

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.AtomicFile
import android.util.Base64
import org.json.JSONObject
import java.io.File
import java.net.URI
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** Credentials stay on this device: Keystore encryption, excluded from backup. */
object PlexSettings {
    data class Config(val server: String = "", val token: String = "") {
        val enabled: Boolean get() = server.isNotBlank() && token.isNotBlank()
    }
    private const val ALIAS = "decklink.plex.v1"
    private fun file(ctx: Context) = AtomicFile(File(ctx.noBackupFilesDir, "plex.enc"))
    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build())
        }.generateKey()
    }
    fun normalizeServer(value: String): String {
        val uri = URI(value.trim())
        require(uri.scheme in listOf("http", "https") && !uri.host.isNullOrBlank() &&
            uri.rawUserInfo == null && uri.rawQuery == null && uri.rawFragment == null &&
            (uri.path.isNullOrEmpty() || uri.path == "/") &&
            (uri.port == -1 || uri.port in 1..65535)) { "Plex 서버 주소를 확인하세요 (https://서버:포트)." }
        return uri.toString().trimEnd('/')
    }
    @Synchronized fun load(ctx: Context): Config = try {
        val payload = file(ctx).openRead().use { JSONObject(it.bufferedReader().readText()) }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, Base64.decode(payload.getString("iv"), Base64.NO_WRAP)))
        val plain = JSONObject(String(cipher.doFinal(Base64.decode(payload.getString("data"), Base64.NO_WRAP)), Charsets.UTF_8))
        Config(normalizeServer(plain.getString("server")), plain.getString("token"))
    } catch (_: Exception) { Config() }

    @Synchronized fun save(ctx: Context, server: String, token: String) {
        val normalized = normalizeServer(server)
        require(token.isNotBlank() && token.none { it.isWhitespace() || it.isISOControl() }) { "Plex 토큰을 확인하세요." }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key())
        val plain = JSONObject().put("server", normalized).put("token", token).toString().toByteArray(Charsets.UTF_8)
        val payload = JSONObject().put("iv", Base64.encodeToString(cipher.iv, Base64.NO_WRAP))
            .put("data", Base64.encodeToString(cipher.doFinal(plain), Base64.NO_WRAP)).toString()
        val target = file(ctx)
        val stream = target.startWrite()
        try { stream.write(payload.toByteArray(Charsets.UTF_8)); target.finishWrite(stream) }
        catch (e: Exception) { target.failWrite(stream); throw e }
    }
    @Synchronized fun clear(ctx: Context) { file(ctx).delete() }
}
