/* BLE(NimBLE) GATT 서버 — deck.py 프로토콜을 블루투스로 주고받는다
 *   서비스 UUID : 6e400001-b5a3-f393-e0a9-e50e24dcca9e  (Nordic UART 호환 형식)
 *   RX  (앱→보드): 6e400002-...  write / write-without-response
 *   TX  (보드→앱): 6e400003-...  notify
 * 받은 바이트는 deck_rx_push() 로, 보낼 바이트는 ble_send() 로. */
#ifndef BLE_H
#define BLE_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DECK_BLE_NAME "Deck"

void ble_start(void);
void ble_send(const char *buf, int len);   /* 연결 없으면 버림, 논블로킹 */
bool ble_connected(void);

#ifdef __cplusplus
}
#endif
#endif
