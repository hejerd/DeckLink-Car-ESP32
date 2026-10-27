#ifndef UI_H
#define UI_H
#include <stdbool.h>
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif

/* lv_init + 디스플레이/입력 드라이버 등록 후, LVGL 락 잡은 상태에서 호출 */
void ui_init(lv_disp_t *disp, lv_disp_drv_t *drv);
/* 현재 방향 0:가로 1:가로180 2:세로 3:세로180 (flush/touch 변환용) */
int  ui_orient(void);
/* 화면 꺼짐 여부 / 터치로 깨우기 (touch_cb 에서 사용) */
bool ui_screen_off(void);
void ui_wake(void);
/* 시리얼 수신 루프 — 별도 태스크에서 무한 호출. 내부에서 필요 시 deck_lock 사용 */
void ui_serial_task_loop(void);

#ifdef __cplusplus
}
#endif
#endif
