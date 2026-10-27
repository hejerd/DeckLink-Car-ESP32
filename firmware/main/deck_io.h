/* main.cpp 가 제공하고 ui.c 가 사용하는 하드웨어 인터페이스 */
#ifndef DECK_IO_H
#define DECK_IO_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* PC 로 한 줄 전송 (printf 형식). 호스트가 안 읽고 있으면 버린다 — 블로킹 없음 */
void deck_printf(const char *fmt, ...);
/* USB 에서 최대 len 바이트 읽기. timeout_ms 동안 데이터 없으면 0 */
int  deck_read(uint8_t *buf, int len, int timeout_ms);
/* LVGL 뮤텍스 */
void deck_lock(void);
void deck_unlock(void);
/* 백라이트 0~100 */
void deck_set_backlight(int percent);
/* 가속도 mg. 실패 시 false */
bool deck_read_accel(float acc[3]);
/* ms 틱 */
uint32_t deck_millis(void);
/* 수신 공용 버퍼에 바이트 넣기 (USB/TCP 수신 태스크, 로컬 명령 주입용) */
void deck_rx_push(const uint8_t *buf, int len);

#ifdef __cplusplus
}
#endif
#endif
