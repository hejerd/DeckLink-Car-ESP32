package com.hejerd.decklink

/** 보드로의 전송로 공통 인터페이스 — TCP(DeckClient) / BLE(BleClient) */
interface DeckLink {
    val connected: Boolean
    fun start()
    fun stop()
    fun sendLine(s: String)
    fun send(b: ByteArray)
}
