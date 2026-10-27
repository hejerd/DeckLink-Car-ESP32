package com.hejerd.decklink

import android.app.Notification
import android.graphics.Bitmap
import android.graphics.drawable.BitmapDrawable
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification

/**
 * MediaSessionManager.getActiveSessions() 를 쓰려면 알림 접근 권한을 가진
 * NotificationListenerService 가 있어야 한다.
 * 덤으로 미디어 알림의 큰 아이콘(앨범아트)을 패키지별로 보관해 ArtFetcher 의 대체 경로로 쓴다.
 */
class DeckNotifListener : NotificationListenerService() {
    override fun onListenerConnected() {
        super.onListenerConnected()
        DeckService.start(this)
        try { activeNotifications?.forEach { grab(it) } } catch (_: Exception) {}
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) { grab(sbn) }

    private fun grab(sbn: StatusBarNotification) {
        val n = sbn.notification ?: return
        // 미디어 알림만 (세션 토큰이 있거나 MediaStyle)
        val isMedia = n.extras?.containsKey(Notification.EXTRA_MEDIA_SESSION) == true ||
                n.extras?.getString(Notification.EXTRA_TEMPLATE)?.contains("Media") == true
        if (!isMedia) return
        val bmp: Bitmap? = try {
            val icon = n.getLargeIcon()
            (icon?.loadDrawable(this) as? BitmapDrawable)?.bitmap
                ?: @Suppress("DEPRECATION") n.largeIcon
                ?: (n.extras?.getParcelable<Bitmap>(Notification.EXTRA_LARGE_ICON))
        } catch (_: Exception) { null }
        if (bmp != null) ArtFetcher.notifArt = ArtFetcher.notifArt + (sbn.packageName to bmp)
    }
}
