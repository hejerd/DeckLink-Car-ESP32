package com.hejerd.decklink

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** 부팅/앱 업데이트 시 서비스 자동 시작 (차에서 스틱이 켜지면 바로 연결) */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_BOOT_COMPLETED,
            Intent.ACTION_MY_PACKAGE_REPLACED,
            "android.intent.action.QUICKBOOT_POWERON",
            "com.htc.intent.action.QUICKBOOT_POWERON" -> DeckService.start(context)
        }
    }
}
