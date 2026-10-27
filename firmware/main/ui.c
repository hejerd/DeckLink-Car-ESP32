/*
 * deck_esp32 — ui.c
 * RP2350 LVGL_example.c 의 UI/프로토콜 부분을 ESP-IDF 로 이식.
 * 하드웨어(디스플레이/터치/USB/IMU)는 main.cpp + deck_io.h 에 위임.
 * PC 프로토콜(deck.py)은 동일:
 *   PC→보드  S: M: A: I: L: G: D: T:      보드→PC  P: B: C: O: W: X:
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include "lvgl.h"
#include "deck_io.h"
#include "ui.h"

LV_FONT_DECLARE(font_kr_20);
LV_FONT_DECLARE(font_clock_110);
LV_FONT_DECLARE(font_wx_60);               /* 날씨 아이콘 (Material Design Icons, Apache-2.0) */           /* 시계 숫자 전용 (Montserrat SemiBold 110px, 0-9 와 :) */

static lv_disp_drv_t *disp_drv;      /* main.cpp 소유 (해상도 변경용) */
static lv_disp_t *disp;

/* ===== 방향 =====
 * 0: 가로 (기본)   1: 가로 180°
 * 2: 세로          3: 세로 180°
 * 세로 두 방향이 서로 바뀌어 나오면 PORTRAIT_SWAP 를 1로 */
#define PORTRAIT_SWAP 0
static volatile int orient = 0;
static bool ui_portrait = false;
static volatile int orient_pending = -1;

/* ===== 배경/글자색 (앨범 커버 색) ===== */
static volatile int bg_r = 255, bg_g = 255, bg_b = 255;
static volatile int fg_r = 0,   fg_g = 0,   fg_b = 0;
static bool bg_valid = false;
static volatile bool bg_updated = false;

/* ===== 앨범아트 ===== */
#define ART_W 100
#define ART_H 100
static uint8_t art_buf[ART_W * ART_H * 2];
static volatile bool art_updated = false;

static lv_obj_t *mus_img;
static lv_img_dsc_t art_dsc = {
    .header.always_zero = 0,
    .header.w = ART_W,
    .header.h = ART_H,
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .data_size = ART_W * ART_H * 2,
    .data = art_buf,
};

/* ===== 화면 상태 ===== */
#define VIEW_MUSIC   0
#define VIEW_PAGE    1
#define VIEW_SYSTEM  2
#define VIEW_COUNT   3

/* ===== 차량용: 단축키(버튼 페이지) 화면 숨김 =====
 * 1 이면 위/아래 스와이프가 음악 ↔ 시스템만 오가고, 시작 화면은 음악.
 * 나중에 버튼 페이지 다시 쓰려면 0 으로. (시스템 화면도 숨기려면 UI_HIDE_SYSTEM 1) */
#define UI_HIDE_PAGE   1
#define UI_HIDE_SYSTEM 1

static int next_view(int v, int dir)
{
    for (int i = 0; i < VIEW_COUNT; i++) {
        v = (v + dir + VIEW_COUNT) % VIEW_COUNT;
        if (UI_HIDE_PAGE   && v == VIEW_PAGE)   continue;
        if (UI_HIDE_SYSTEM && v == VIEW_SYSTEM) continue;
        return v;
    }
    return VIEW_MUSIC;
}
#define PAGE_COUNT   3

static lv_obj_t *scr_music;
static lv_obj_t *scr_page;
static lv_obj_t *scr_system;

static lv_obj_t *btns[6];
static lv_obj_t *btn_img[6];

static int cur_view = UI_HIDE_PAGE ? VIEW_MUSIC : VIEW_PAGE;
static int cur_page = 1;              /* 0,1,2 → 페이지 1,2,3 */
static bool swiped = false;
#define BL_LEVEL 90                         /* 켜졌을 때 밝기 (10~100) */
#define BL_DIM   50                         /* 음악 화면 일시정지 시 밝기 */
static volatile int bl_pending = -1;        /* 0=끄기, 1=켜기 */
static bool screen_off = false;

/* ===== 버튼 아이콘 (현재 페이지 6개, 정적 27KB) ===== */
#define ICON_W 48
#define ICON_H 48
#define ICON_BYTES (ICON_W * ICON_H * 2)
#define BTN_BG_HEX 0x2A2A2A
static uint8_t icon_buf[6 * ICON_BYTES];
static lv_img_dsc_t icon_dsc[6];
static volatile int icon_dirty = -1;

/* ===== 바이너리 수신 상태 ===== */
#define BIN_NONE 0
#define BIN_ART  1
#define BIN_ICON 2
static uint8_t *bin_dst = NULL;
static int bin_remain = 0, bin_pos = 0, bin_cap = 0;
static int bin_kind = BIN_NONE, bin_slot = 0;
static uint32_t bin_last;   /* ms */

/* ===== 음악 정보 ===== */
static char mus_title[96] = "";
static char mus_artist[64] = "";
static char lyr_prev[160] = "", lyr_cur[160] = "", lyr_next[160] = "";
static volatile int mus_pos = 0, mus_end = 0, mus_playing = 0;
static volatile bool mus_updated = false;
static volatile bool lyric_updated = false;

static lv_obj_t *mus_lbl_now;        /* "NOW PLAYING" (세로만) */
static lv_obj_t *mus_lbl_title;
static lv_obj_t *mus_lbl_artist;     /* 세로만 */
static lv_obj_t *mus_lbl_time;       /* 가로: "cur / total", 세로: cur */
static lv_obj_t *mus_lbl_dur;        /* 세로만: total */
static lv_obj_t *mus_bar;
static lv_obj_t *mus_lbl_lyric;      /* 현재 가사 */
static lv_obj_t *mus_lbl_lprev;      /* 세로만 */
static lv_obj_t *mus_lbl_lnext;      /* 세로만 */

/* ===== 곡 넘김 드래그 ===== */
#define DRAG_THRESH_L 80        /* 가로 모드: 이 이상 끌면 곡 넘김 */
#define DRAG_THRESH_P 50        /* 세로 모드 */
#define CARD_ANIM_MS  200
static lv_obj_t *mus_card;                    /* 아트+제목(+아티스트) 묶음 */
static lv_obj_t *mus_lbl_hint_l, *mus_lbl_hint_r;
static int  drag_x0, drag_y0, drag_dx;
static int  drag_axis = 0;                    /* 0 미정, 1 가로, 2 세로 */
static bool drag_was_h = false;
static bool card_out = false;                 /* 카드 빠져나가고 새 곡 대기 중 */
static int  card_out_dir = 0;                 /* -1 왼쪽으로 나감(NEXT), +1 오른쪽(PREV) */
static uint32_t card_out_t;
static char card_title[96];
static bool card_new_song = false;

/* ===== 시계 화면: 음악이 멈춰 있거나 앱 연결이 끊기면 음악 화면 대신 표시 ===== */
#define CLOCK_IDLE_MS    5000      /* 일시정지 후 이만큼 지나면 시계로 */
#define CLOCK_NOLINK_MS  8000      /* 앱이 M: 을 이만큼 안 보내면(연결 끊김) 시계로 */
#define CLOCK_PEEK_MS    15000     /* 시계 화면을 터치하면 이 시간 동안 음악 화면 보여줌 */
static lv_obj_t *scr_clock, *clk_time, *clk_date;
static bool clock_on = false;
static volatile uint32_t mus_rx_tick = 0;  /* 마지막 M: 수신 시각 */
static uint32_t idle_since = 0, clock_peek_until = 0;
static volatile bool time_valid = false;   /* 앱에서 K: 로 시간을 받았는지 */
/* 날씨: 앱이 E:도시|기온|상태 로 보냄 (15분마다 + 연결 시) */
static lv_obj_t *clk_city, *clk_temp, *clk_wx, *clk_icon;
static char wx_city[48] = "", wx_desc[48] = "", wx_icon[16] = "";
static int  wx_temp = 0;
static volatile bool wx_valid = false, wx_updated = false;
static bool art_since_out = false;            /* 카드가 나간 뒤 새 커버가 이미 도착했는지 */
#define DRAG_DEBUG 0                           /* 1: 드래그 상태를 X: 로 앱에 보고 (logcat "rx X:drag") */
#if DRAG_DEBUG
#define DRAG_LOG(...) deck_printf(__VA_ARGS__)
#else
#define DRAG_LOG(...) do {} while (0)
#endif

/* ===== 토스트 ===== */
static char toast_app[64] = "", toast_title[128] = "", toast_body[256] = "";
static volatile bool toast_updated = false;
static lv_obj_t *toast_panel, *toast_lbl1, *toast_lbl2;
static lv_timer_t *toast_timer;

/* ===== 시스템 상태 ===== */
static volatile int sys_val[7] = {0,0,0,0,0,0,0};   /* cpu, cpuT, gpu, gpuT, ram%, ramUsed*10, ramTotal*10 */
static volatile bool sys_updated = false;

static lv_obj_t *sys_big[3];
static lv_obj_t *sys_pct[3];
static lv_obj_t *sys_sub[3];
static lv_obj_t *sys_bar[3];
static lv_timer_t *sys_timer;

static const char *sys_name[3] = { "CPU", "GPU", "RAM" };
static const uint32_t sys_color[3] = { 0x2EE6E6, 0x8B5CF6, 0xF5A623 };

/* ===== 스타일 ===== */
static lv_style_t style_kr, style_small;


/* ===== 아이콘 버퍼 비우기 ===== */
static void icons_clear(void)
{
    int r = (BTN_BG_HEX >> 16) & 0xFF, g = (BTN_BG_HEX >> 8) & 0xFF, b = BTN_BG_HEX & 0xFF;
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    for (int i = 0; i < 6 * ICON_W * ICON_H; i++) {
        icon_buf[i * 2]     = (v >> 8) & 0xFF;
        icon_buf[i * 2 + 1] = v & 0xFF;
    }
}

/* ===== 페이지 적용: 버퍼 비우고 PC에 아이콘 요청 ===== */
static void apply_page(void)
{
    icons_clear();
    for (int i = 0; i < 6; i++)
        lv_obj_invalidate(btn_img[i]);
    deck_printf("P:%d\n", cur_page + 1);
}

static void bl_apply(void)
{
    static int last = -1;
    int lv;
    if (screen_off)                                   lv = 0;
    else if (clock_on)                                lv = BL_DIM;
    else if (cur_view == VIEW_MUSIC && !mus_playing)  lv = BL_DIM;
    else                                              lv = BL_LEVEL;
    if (lv != last) { deck_set_backlight(lv); last = lv; }
}

/* ===== 화면 전환 ===== */
static void show_view(int v)
{
    cur_view = v;
    clock_on = false;
    if (v == VIEW_MUSIC)       lv_scr_load(scr_music);
    else if (v == VIEW_SYSTEM) lv_scr_load(scr_system);
    else                       lv_scr_load(scr_page);
    bl_apply();
}

/* ===== 버튼 ===== */
static void btn_event_cb(lv_event_t *e)
{    
    lv_event_code_t code = lv_event_get_code(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    if (code == LV_EVENT_PRESSED) {
        swiped = false;
        return;
    }
    if (code == LV_EVENT_CLICKED) {
        if (swiped) return;
        deck_printf("B:%d:%d\n", cur_page + 1, idx + 1);
    }
}

static void card_set_x(void *obj, int32_t x) { lv_obj_set_x(obj, x); }

static void card_anim(int from, int to, int ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, mus_card);
    lv_anim_set_exec_cb(&a, card_set_x);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void card_slide_in(int from_dir)        /* +1: 오른쪽에서, -1: 왼쪽에서 */
{
    int w = lv_obj_get_width(mus_card);
    lv_anim_del(mus_card, card_set_x);
    card_anim(from_dir * w, 0, CARD_ANIM_MS + 50);
    card_out = false;
    card_new_song = false;
}

static void card_drag_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int thresh = ui_portrait ? DRAG_THRESH_P : DRAG_THRESH_L;

    if (code == LV_EVENT_PRESSED) {
        drag_x0 = p.x; drag_y0 = p.y;
        drag_axis = 0; drag_dx = 0; drag_was_h = false;
        if (!card_out) lv_anim_del(mus_card, card_set_x);
        DRAG_LOG("X:drag press x=%d y=%d out=%d cardx=%d\n", (int)p.x, (int)p.y, card_out, (int)lv_obj_get_x(mus_card));
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (card_out) return;
        int dx = p.x - drag_x0, dy = p.y - drag_y0;
        if (drag_axis == 0 && (abs(dx) > 12 || abs(dy) > 12))
            drag_axis = (abs(dx) > abs(dy)) ? 1 : 2;
        if (drag_axis != 1) return;
        drag_dx = dx;
        lv_obj_set_x(mus_card, dx);
        lv_obj_add_flag(mus_lbl_hint_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mus_lbl_hint_r, LV_OBJ_FLAG_HIDDEN);
        if (dx >  thresh / 2) lv_obj_clear_flag(mus_lbl_hint_l, LV_OBJ_FLAG_HIDDEN);
        if (dx < -thresh / 2) lv_obj_clear_flag(mus_lbl_hint_r, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_add_flag(mus_lbl_hint_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mus_lbl_hint_r, LV_OBJ_FLAG_HIDDEN);
        drag_was_h = (drag_axis == 1);
        DRAG_LOG("X:drag %s dx=%d axis=%d out=%d\n", code == LV_EVENT_RELEASED ? "rel" : "lost", drag_dx, drag_axis, card_out);
        drag_axis = 0;
        if (card_out || !drag_was_h) return;
        int w = lv_obj_get_width(mus_card);
        if (drag_dx <= -thresh) {                 /* 왼쪽으로 밀기 → 다음 곡 */
            card_out = true; card_out_dir = -1; art_since_out = false;
            card_out_t = lv_tick_get();
            strncpy(card_title, mus_title, sizeof(card_title));
            card_anim(drag_dx, -w, CARD_ANIM_MS);
            deck_printf("C:NEXT\n");
        } else if (drag_dx >= thresh) {           /* 오른쪽으로 밀기 → 이전 곡 */
            card_out = true; card_out_dir = 1; art_since_out = false;
            card_out_t = lv_tick_get();
            strncpy(card_title, mus_title, sizeof(card_title));
            card_anim(drag_dx, w, CARD_ANIM_MS);
            deck_printf("C:PREV\n");
        } else {
            card_anim(drag_dx, 0, CARD_ANIM_MS);  /* 제자리 복귀 */
        }
    }
}

/* 재생/정지: 차량 진동·노이즈로 인한 헛 터치 방지를 위해 길게 누르기(LV_INDEV_DEF_LONG_PRESS_TIME, 기본 400ms)로만.
 * 짧은 탭은 무시. 예전 탭 방식으로 되돌리려면 PLAY_ON_TAP 1 */
#define PLAY_ON_TAP 0
static void music_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (drag_was_h) { if (code == LV_EVENT_CLICKED) drag_was_h = false; return; }   /* 끈 건 클릭 아님 */
#if PLAY_ON_TAP
    if (code == LV_EVENT_CLICKED) deck_printf("C:PLAY\n");
#else
    if (code == LV_EVENT_LONG_PRESSED && drag_axis == 0) deck_printf("C:PLAY\n");
#endif
}
/* ===== 제스처 ===== */
static void gesture_cb(lv_event_t *e)
{
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (cur_view == VIEW_MUSIC && (d == LV_DIR_LEFT || d == LV_DIR_RIGHT || drag_axis == 1))
        return;                                    /* 좌우는 card_drag_cb 가 처리 */
    swiped = true;
    lv_indev_wait_release(lv_indev_get_act());

    if (d == LV_DIR_TOP) {
        show_view(next_view(cur_view, -1));
    } else if (d == LV_DIR_BOTTOM) {
        show_view(next_view(cur_view, +1));
    } else if (cur_view == VIEW_PAGE) {
        if (d == LV_DIR_LEFT) {
            cur_page = (cur_page + 1) % PAGE_COUNT;
            apply_page();
        } else if (d == LV_DIR_RIGHT) {
            cur_page = (cur_page + PAGE_COUNT - 1) % PAGE_COUNT;
            apply_page();
        }
    }
}


/* ===== 바이너리 수신 ===== */
static void bin_start(uint8_t *dst, int cap, int n, int kind, int slot)
{
    bin_dst = dst; bin_cap = cap; bin_remain = n; bin_pos = 0;
    bin_kind = kind; bin_slot = slot;
    bin_last = deck_millis();
}

static void copy_field(char *dst, size_t cap, const char *src)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}


/* ===== 한 줄 명령 처리 (LVGL 락 잡은 상태에서 호출) ===== */
static void handle_line(char *buf)
{
    if (buf[0] == 'S' && buf[1] == ':') {
        int v[7];
        if (sscanf(buf+2, "%d,%d,%d,%d,%d,%d,%d",
                   &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6]) == 7) {
            for (int i = 0; i < 7; i++) sys_val[i] = v[i];
            sys_updated = true;
        }
    }
    else if (buf[0] == 'M' && buf[1] == ':') {
        char *p = buf + 2;
        char *b1 = strchr(p, '|');
        if (b1) {
            *b1 = 0;
            copy_field(mus_title, sizeof(mus_title), p);
            char *p2 = b1 + 1;
            char *b2 = strchr(p2, '|');
            if (b2) {
                *b2 = 0;
                copy_field(mus_artist, sizeof(mus_artist), p2);
                int a2,b3,c4;
                if (sscanf(b2+1, "%d|%d|%d", &a2,&b3,&c4) == 3) {
                    mus_pos = a2; mus_end = b3; mus_playing = c4;
                    mus_rx_tick = lv_tick_get();
                    mus_updated = true;
                }
            }
        }
    }
    else if (buf[0] == 'A' && buf[1] == ':') {
        int n = atoi(buf + 2);
        if (n > 0 && n <= (int)sizeof(art_buf))
            bin_start(art_buf, sizeof(art_buf), n, BIN_ART, 0);
    }
    else if (buf[0] == 'I' && buf[1] == ':') {
        int p, sl, n;
        if (sscanf(buf + 2, "%d:%d:%d", &p, &sl, &n) == 3 &&
            p == cur_page + 1 && sl >= 1 && sl <= 6 &&
            n > 0 && n <= ICON_BYTES) {
            bin_start(icon_buf + (sl - 1) * ICON_BYTES, ICON_BYTES, n, BIN_ICON, sl - 1);
        }
    }
    else if (buf[0] == 'L' && buf[1] == ':') {
        /* L:이전|현재|다음   (구형 L:현재 도 허용) */
        char *p = buf + 2;
        char *b1 = strchr(p, '|');
        char *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
        if (b1 && b2) {
            *b1 = 0; *b2 = 0;
            copy_field(lyr_prev, sizeof(lyr_prev), p);
            copy_field(lyr_cur,  sizeof(lyr_cur),  b1 + 1);
            copy_field(lyr_next, sizeof(lyr_next), b2 + 1);
        } else {
            lyr_prev[0] = 0; lyr_next[0] = 0;
            copy_field(lyr_cur, sizeof(lyr_cur), p);
        }
        lyric_updated = true;
    }
    else if (buf[0] == 'G' && buf[1] == ':') {
        int r, g, b, r2, g2, b2;
        if (sscanf(buf + 2, "%d,%d,%d,%d,%d,%d", &r, &g, &b, &r2, &g2, &b2) == 6) {
            bg_r = r;  bg_g = g;  bg_b = b;
            fg_r = r2; fg_g = g2; fg_b = b2;
            bg_valid = true;
            bg_updated = true;
        }
    }
    else if (buf[0] == 'E' && buf[1] == ':') {
        char *p = buf + 2;
        char *b1 = strchr(p, '|');
        char *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
        if (b1 && b2) {
            *b1 = 0; *b2 = 0;
            copy_field(wx_city, sizeof(wx_city), p);
            wx_temp = atoi(b1 + 1);
            char *b3 = strchr(b2 + 1, '|');
            if (b3) { *b3 = 0; copy_field(wx_icon, sizeof(wx_icon), b3 + 1); } else wx_icon[0] = 0;
            copy_field(wx_desc, sizeof(wx_desc), b2 + 1);
            wx_valid = true;
            wx_updated = true;
        }
    }
    else if (buf[0] == 'K' && buf[1] == ':') {
        /* K:<유닉스 시각(초)> — 앱이 연결 시·1분마다 보냄. 끊겨도 보드 타이머로 계속 흐름 */
        long long t = atoll(buf + 2);
        if (t > 1700000000LL) {
            struct timeval tv = { .tv_sec = (time_t)t, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            time_valid = true;
        }
    }
    else if (buf[0] == 'D' && buf[1] == ':') {
        bl_pending = (buf[2] == '0') ? 0 : 1;
    }
    else if (buf[0] == 'T' && buf[1] == ':') {
        char *p = buf + 2;
        char *b1 = strchr(p, '|');
        char *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
        if (b1 && b2) {
            *b1 = 0; *b2 = 0;
            copy_field(toast_app,   sizeof(toast_app),   p);
            copy_field(toast_title, sizeof(toast_title), b1 + 1);
            copy_field(toast_body,  sizeof(toast_body),  b2 + 1);
            toast_updated = true;
        }
    }
}

/* ===== 시리얼 수신 루프 (별도 태스크, LVGL 락 없이 읽고 줄 완성 시에만 락) =====
 * 텍스트 줄은 line 버퍼에 모으고, A:/I: 뒤의 바이너리는 art_buf/icon_buf 에 직접 기록. */
void ui_serial_task_loop(void)
{
    static char line[512];
    static int len = 0;
    static uint8_t rx[256];

    if (bin_remain > 0) {
        int want = bin_remain < (int)sizeof(rx) ? bin_remain : (int)sizeof(rx);
        int n = deck_read(rx, want, 20);
        if (n <= 0) {
            if (deck_millis() - bin_last > 2000) {        /* 2초 정지 → 포기 */
                bin_remain = 0; bin_kind = BIN_NONE; len = 0;
            }
            return;
        }
        bin_last = deck_millis();
        for (int i = 0; i < n; i++) {
            if (bin_pos < bin_cap) bin_dst[bin_pos++] = rx[i];
        }
        bin_remain -= n;
        if (bin_remain == 0) {
            if (bin_kind == BIN_ART)  art_updated = true;
            if (bin_kind == BIN_ICON) icon_dirty = bin_slot;
            bin_kind = BIN_NONE;
        }
        return;
    }

    /* 텍스트 모드: 한 바이트씩 (A:/I: 다음 바이트부터 바이너리로 전환해야 하므로) */
    uint8_t ch;
    int n = deck_read(&ch, 1, 50);
    if (n <= 0) return;

    if (ch == '\n' || ch == '\r') {
        if (len > 0) {
            line[len] = 0;
            deck_lock();
            handle_line(line);
            deck_unlock();
            len = 0;
        }
    } else if (len < 510) {
        line[len++] = (char)ch;
    }
}

/* ===== 토스트 ===== */
static void toast_hide_cb(lv_timer_t *t)
{
    if (toast_panel) lv_obj_add_flag(toast_panel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(toast_timer);
}

static void toast_click_cb(lv_event_t *e)
{
    toast_hide_cb(NULL);
}

static void toast_show(void)
{
    char line1[200];
    if (toast_app[0]) snprintf(line1, sizeof(line1), "%s  ·  %s", toast_app, toast_title);
    else              snprintf(line1, sizeof(line1), "%s", toast_title);
    lv_label_set_text(toast_lbl1, line1);
    lv_label_set_text(toast_lbl2, toast_body);
    lv_obj_clear_flag(toast_panel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(toast_timer);
    lv_timer_resume(toast_timer);
}

/* ===== 현재 데이터로 화면 채우기 (UI 재생성 후에도 사용) ===== */
static void refresh_lyrics(void)
{
    lv_label_set_text(mus_lbl_lyric, lyr_cur[0] ? lyr_cur : " ");
    if (mus_lbl_lprev) lv_label_set_text(mus_lbl_lprev, lyr_prev);
    if (mus_lbl_lnext) lv_label_set_text(mus_lbl_lnext, lyr_next);

    if (ui_portrait && mus_lbl_lprev && mus_lbl_lnext) {
        lv_obj_update_layout(mus_lbl_lyric);
        lv_obj_align_to(mus_lbl_lprev, mus_lbl_lyric, LV_ALIGN_OUT_TOP_MID,    0, -3);
        lv_obj_align_to(mus_lbl_lnext, mus_lbl_lyric, LV_ALIGN_OUT_BOTTOM_MID, 0,  24);
    }
}
static void refresh_music(void)
{
    char s[32];
    if (!ui_portrait) {
        char tt[170];
        if (mus_artist[0]) snprintf(tt, sizeof(tt), "%s - %s", mus_artist, mus_title);
        else               snprintf(tt, sizeof(tt), "%s", mus_title[0] ? mus_title : "-");
        lv_label_set_text(mus_lbl_title, tt);
        snprintf(s, sizeof(s), "%d:%02d / %d:%02d", mus_pos/60, mus_pos%60, mus_end/60, mus_end%60);
        lv_label_set_text(mus_lbl_time, s);
    } else {
        lv_label_set_text(mus_lbl_title, mus_title[0] ? mus_title : "-");
        lv_label_set_text(mus_lbl_artist, mus_artist);
        snprintf(s, sizeof(s), "%d:%02d", mus_pos/60, mus_pos%60);
        lv_label_set_text(mus_lbl_time, s);
        snprintf(s, sizeof(s), "%d:%02d", mus_end/60, mus_end%60);
        lv_label_set_text(mus_lbl_dur, s);
    }
    int p = (mus_end > 0) ? (mus_pos * 1000 / mus_end) : 0;
    lv_bar_set_value(mus_bar, p, LV_ANIM_OFF);
    refresh_lyrics();
}

static void refresh_colors(void)
{
    if (!bg_valid) return;
    lv_color_t bg = lv_color_make(bg_r, bg_g, bg_b);
    lv_color_t fg = lv_color_make(fg_r, fg_g, fg_b);
    lv_color_t dim = lv_color_mix(fg, bg, LV_OPA_50);
    lv_obj_set_style_bg_color(scr_music, bg, 0);
    lv_obj_set_style_text_color(mus_lbl_title, fg, 0);
    lv_obj_set_style_text_color(mus_lbl_lyric, fg, 0);
    lv_obj_set_style_text_color(mus_lbl_time,  fg, 0);
    lv_obj_set_style_bg_color(mus_bar, fg, LV_PART_INDICATOR);
    if (mus_lbl_now)    lv_obj_set_style_text_color(mus_lbl_now,    dim, 0);
    if (mus_lbl_artist) lv_obj_set_style_text_color(mus_lbl_artist, dim, 0);
    if (mus_lbl_dur)    lv_obj_set_style_text_color(mus_lbl_dur,    fg,  0);
    if (mus_lbl_lprev)  lv_obj_set_style_text_color(mus_lbl_lprev,  dim, 0);
    if (mus_lbl_lnext)  lv_obj_set_style_text_color(mus_lbl_lnext,  dim, 0);
}

static void refresh_system(void)
{
    int load[3] = { sys_val[0], sys_val[2], sys_val[4] };
    char s[48];
    for (int i = 0; i < 3; i++) {
        int pct = load[i] > 100 ? 100 : load[i];
        snprintf(s, sizeof(s), "%d", pct);
        lv_label_set_text(sys_big[i], s);
        lv_obj_align_to(sys_pct[i], sys_big[i], LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -8);
        lv_bar_set_value(sys_bar[i], pct, LV_ANIM_OFF);
    }
    snprintf(s, sizeof(s), "%d°C", sys_val[1]);
    lv_label_set_text(sys_sub[0], s);
    snprintf(s, sizeof(s), "%d°C", sys_val[3]);
    lv_label_set_text(sys_sub[1], s);
    snprintf(s, sizeof(s), "%d.%d / %d GB", sys_val[5]/10, sys_val[5]%10, sys_val[6]/10);
    lv_label_set_text(sys_sub[2], s);
}

/* ===== UI 생성 (가로 / 세로) ===== */
/* ===== 날씨 표시 ===== */
static const char *wx_glyph(const char *key)
{
    static const struct { const char *k, *g; } T[] = {
        { "sun", "\xF3\xB0\x96\x99" },      { "moon", "\xF3\xB0\x96\x94" },
        { "pcloud", "\xF3\xB0\x96\x95" },   { "pcloud_n", "\xF3\xB0\xBC\xB1" },
        { "cloud", "\xF3\xB0\x96\x90" },    { "fog", "\xF3\xB0\x96\x91" },
        { "rain", "\xF3\xB0\x96\x97" },     { "pour", "\xF3\xB0\x96\x96" },
        { "snow", "\xF3\xB0\x96\x98" },     { "snow_h", "\xF3\xB0\xBC\xB6" },
        { "thunder", "\xF3\xB0\x96\x93" },  { "thunder_r", "\xF3\xB0\x99\xBE" },
        { "hail", "\xF3\xB0\x96\x92" },     { "sleet", "\xF3\xB0\x99\xBF" },
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (strcmp(key, T[i].k) == 0) return T[i].g;
    return "";
}

static void wx_refresh(void)
{
    if (!clk_city) return;
    if (!wx_valid) {
        lv_label_set_text(clk_city, "");
        lv_label_set_text(clk_temp, "");
        lv_label_set_text(clk_wx, "");
        lv_label_set_text(clk_icon, "");
        return;
    }
    lv_label_set_text(clk_icon, wx_glyph(wx_icon));
    char t[16];
    snprintf(t, sizeof(t), "%d\xC2\xB0", wx_temp);            /* 18° (montserrat 에 ° 있음) */
    lv_label_set_text(clk_city, wx_city);
    lv_label_set_text(clk_temp, t);
    lv_label_set_text(clk_wx, wx_desc);
    if (!ui_portrait) lv_obj_set_pos(clk_temp, wx_icon[0] ? 498 : 426, 54);   /* 아이콘 없으면 왼쪽으로 */
}

/* ===== 시계 ===== */
static void clock_press_cb(lv_event_t *e)
{
    (void)e;
    clock_peek_until = lv_tick_get() + CLOCK_PEEK_MS;   /* 잠깐 음악 화면 보기 */
}

static void clock_refresh(bool force)
{
    static int last_min = -1;
    char tbuf[16], dbuf[64];
    if (!time_valid) {
        if (force || last_min != -2) {
            lv_label_set_text(clk_time, ui_portrait ? "--\n--" : "--:--");
            lv_label_set_text(clk_date, "시간 동기화 대기");
            last_min = -2;
        }
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int m = tm.tm_hour * 60 + tm.tm_min;
    if (!force && m == last_min) return;
    last_min = m;
    static const char *wd[] = { "일", "월", "화", "수", "목", "금", "토" };
    snprintf(tbuf, sizeof(tbuf), ui_portrait ? "%02d\n%02d" : "%02d:%02d", tm.tm_hour, tm.tm_min);
    snprintf(dbuf, sizeof(dbuf), "%d월 %d일 %s요일", tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
    lv_label_set_text(clk_time, tbuf);
    lv_label_set_text(clk_date, dbuf);
}

/* 음악 화면에서만: 일시정지 CLOCK_IDLE_MS 지나거나 앱 연결이 끊기면 시계, 재생되면 음악 화면 */
static void clock_update(void)
{
    static int last_playing = -1;
    uint32_t now = lv_tick_get();
    if (mus_playing != last_playing) { last_playing = mus_playing; if (!mus_playing) idle_since = now; }
    bool link_lost = lv_tick_elaps(mus_rx_tick) > CLOCK_NOLINK_MS;
    bool idle      = !mus_playing && lv_tick_elaps(idle_since) > CLOCK_IDLE_MS;
    bool peek      = (int32_t)(clock_peek_until - now) > 0;
    bool want = (cur_view == VIEW_MUSIC) && !card_out && drag_axis == 0 && (idle || link_lost) && !peek;
    if (want && !clock_on) {
        clock_refresh(true);
        lv_scr_load(scr_clock);
        clock_on = true;
        bl_apply();
    } else if (!want && clock_on) {
        lv_scr_load(scr_music);
        clock_on = false;
        bl_apply();
    } else if (clock_on) {
        clock_refresh(false);
    }
}

static void build_ui(bool portrait)
{
    /* --- 페이지 화면 --- */
    scr_page = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr_page, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_set_style_bg_color(scr_page, lv_color_hex(0x101010), 0);

    for (int i = 0; i < 6; i++) {
        btns[i] = lv_btn_create(scr_page);
        if (!portrait) {
            lv_obj_set_size(btns[i], 98, 156);
            lv_obj_align(btns[i], LV_ALIGN_BOTTOM_LEFT, 8 + i * 104, -6);
        } else {
            int col = i % 2, row = i / 2;
            lv_obj_set_size(btns[i], 76, 196);
            lv_obj_set_pos(btns[i], 8 + col * 80, 8 + row * 208);
        }
        lv_obj_set_style_radius(btns[i], 12, 0);
        lv_obj_set_style_bg_color(btns[i], lv_color_hex(BTN_BG_HEX), 0);
        lv_obj_set_style_bg_color(btns[i], lv_color_hex(0x444444), LV_STATE_PRESSED);
        lv_obj_set_style_shadow_width(btns[i], 0, 0);
        lv_obj_add_flag(btns[i], LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(btns[i], btn_event_cb, LV_EVENT_ALL, (void *)(intptr_t)i);

        btn_img[i] = lv_img_create(btns[i]);
        lv_img_set_src(btn_img[i], &icon_dsc[i]);
        lv_obj_center(btn_img[i]);
    }

    /* --- 음악 화면 --- */
    scr_music = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr_music, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(scr_music, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_music, music_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr_music, music_click_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(scr_music, card_drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr_music, card_drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr_music, card_drag_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(scr_music, card_drag_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_set_style_pad_all(scr_music, 0, 0);
    lv_obj_clear_flag(scr_music, LV_OBJ_FLAG_SCROLLABLE);

    mus_lbl_now = NULL; mus_lbl_artist = NULL; mus_lbl_dur = NULL;
    mus_lbl_lprev = NULL; mus_lbl_lnext = NULL;

    /* 드래그로 움직이는 카드 (투명, 화면 크기) */
    mus_card = lv_obj_create(scr_music);
    lv_obj_remove_style_all(mus_card);
    lv_obj_set_size(mus_card, portrait ? 172 : 640, portrait ? 640 : 172);
    lv_obj_set_pos(mus_card, 0, 0);
    lv_obj_clear_flag(mus_card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    card_out = false; drag_axis = 0; drag_was_h = false;

    /* 넘김 힌트 */
    mus_lbl_hint_l = lv_label_create(scr_music);
    lv_label_set_text(mus_lbl_hint_l, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(mus_lbl_hint_l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(mus_lbl_hint_l, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(mus_lbl_hint_l, LV_ALIGN_LEFT_MID, 12, portrait ? -214 : -20);
    lv_obj_add_flag(mus_lbl_hint_l, LV_OBJ_FLAG_HIDDEN);
    mus_lbl_hint_r = lv_label_create(scr_music);
    lv_label_set_text(mus_lbl_hint_r, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(mus_lbl_hint_r, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(mus_lbl_hint_r, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(mus_lbl_hint_r, LV_ALIGN_RIGHT_MID, -12, portrait ? -214 : -20);
    lv_obj_add_flag(mus_lbl_hint_r, LV_OBJ_FLAG_HIDDEN);

    mus_img = lv_img_create(mus_card);
    lv_img_set_src(mus_img, &art_dsc);

    mus_lbl_title = lv_label_create(mus_card);
    lv_obj_add_style(mus_lbl_title, &style_kr, 0);

    mus_bar = lv_bar_create(mus_card);
    lv_bar_set_range(mus_bar, 0, 1000);

    mus_lbl_time = lv_label_create(mus_card);
    lv_obj_add_style(mus_lbl_time, &style_small, 0);

    mus_lbl_lyric = lv_label_create(mus_card);
    lv_label_set_long_mode(mus_lbl_lyric, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(mus_lbl_lyric, &style_kr, 0);

    if (!portrait) {
        mus_lbl_now = lv_label_create(scr_music);
        lv_label_set_text(mus_lbl_now, "NOW PLAYING");
        lv_obj_add_style(mus_lbl_now, &style_small, 0);
        lv_obj_set_style_text_color(mus_lbl_now, lv_color_hex(0x888888), 0);
        lv_obj_align(mus_lbl_now, LV_ALIGN_TOP_LEFT, 20, 10);

        lv_obj_set_pos(mus_img, 20, 36);
        lv_label_set_long_mode(mus_lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(mus_lbl_title, 470);
        lv_obj_set_pos(mus_lbl_title, 140, 40);
        lv_obj_set_size(mus_bar, 370, 8);
        lv_obj_set_pos(mus_bar, 140, 82);
        lv_obj_set_pos(mus_lbl_time, 525, 76);
        lv_obj_set_width(mus_lbl_lyric, 480);
        lv_obj_set_pos(mus_lbl_lyric, 140, 108);
    } else {
        /* NOW PLAYING */
        mus_lbl_now = lv_label_create(scr_music);
        lv_label_set_text(mus_lbl_now, "NOW PLAYING");
        lv_obj_add_style(mus_lbl_now, &style_small, 0);
        lv_obj_set_style_text_color(mus_lbl_now, lv_color_hex(0x888888), 0);
        lv_obj_align(mus_lbl_now, LV_ALIGN_TOP_MID, 0, 12);

        /* 앨범아트 100 → 1.4배 = 140 */
        lv_img_set_zoom(mus_img, 358);
        lv_obj_align(mus_img, LV_ALIGN_TOP_MID, 0, 56);   /* 그려지는 영역 ≈ y 36~176 */

        /* 제목 / 아티스트 */
        lv_label_set_long_mode(mus_lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(mus_lbl_title, 156);
        lv_obj_set_style_text_align(mus_lbl_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(mus_lbl_title, 8, 196);

        mus_lbl_artist = lv_label_create(mus_card);
        lv_label_set_long_mode(mus_lbl_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(mus_lbl_artist, 156);
        lv_obj_add_style(mus_lbl_artist, &style_kr, 0);
        lv_obj_set_style_text_align(mus_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(mus_lbl_artist, lv_color_hex(0x888888), 0);
        lv_obj_set_pos(mus_lbl_artist, 8, 226);

        /* 재생바 + 시간 */
        lv_obj_set_size(mus_bar, 152, 6);
        lv_obj_set_pos(mus_bar, 10, 300);
        lv_obj_set_pos(mus_lbl_time, 10, 312);
        mus_lbl_dur = lv_label_create(mus_card);
        lv_obj_add_style(mus_lbl_dur, &style_small, 0);
        lv_obj_set_width(mus_lbl_dur, 60);
        lv_obj_set_style_text_align(mus_lbl_dur, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(mus_lbl_dur, 102, 312);

        /* 가사 3줄: 이전(1줄) / 현재(최대 3줄 고정 높이) / 다음(1줄) */
        mus_lbl_lprev = lv_label_create(mus_card);
        lv_label_set_long_mode(mus_lbl_lprev, LV_LABEL_LONG_DOT);
        lv_obj_set_width(mus_lbl_lprev, 156);
        lv_obj_add_style(mus_lbl_lprev, &style_kr, 0);
        lv_obj_set_style_text_align(mus_lbl_lprev, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(mus_lbl_lprev, lv_color_hex(0x777777), 0);
        lv_obj_set_pos(mus_lbl_lprev, 8, 380);

        lv_label_set_long_mode(mus_lbl_lyric, LV_LABEL_LONG_WRAP);
        lv_obj_set_size(mus_lbl_lyric, 156, 78);           /* 3줄 고정 */
        lv_obj_set_style_text_align(mus_lbl_lyric, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(mus_lbl_lyric, 8, 396);

        mus_lbl_lnext = lv_label_create(mus_card);
        lv_label_set_long_mode(mus_lbl_lnext, LV_LABEL_LONG_DOT);
        lv_obj_set_width(mus_lbl_lnext, 156);
        lv_obj_add_style(mus_lbl_lnext, &style_kr, 0);
        lv_obj_set_style_text_align(mus_lbl_lnext, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(mus_lbl_lnext, lv_color_hex(0x777777), 0);
        lv_obj_set_pos(mus_lbl_lnext, 8, 500);
    }

    /* --- 시스템 화면 --- */
    scr_system = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr_system, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_set_style_pad_all(scr_system, 0, 0);
    lv_obj_set_style_bg_color(scr_system, lv_color_hex(0x101010), 0);
    lv_obj_clear_flag(scr_system, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++) {
        int x, y0, barw;
        if (!portrait) { x = 40 + i * 213; y0 = 0;           barw = 150; }
        else           { x = 16;           y0 = 4 + i * 208; barw = 140; }
        lv_color_t col = lv_color_hex(sys_color[i]);

        lv_obj_t *name = lv_label_create(scr_system);
        lv_label_set_text(name, sys_name[i]);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(name, col, 0);
        lv_obj_set_pos(name, x, y0 + 16);

        sys_big[i] = lv_label_create(scr_system);
        lv_label_set_text(sys_big[i], "0");
        lv_obj_set_style_text_font(sys_big[i], &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(sys_big[i], lv_color_white(), 0);
        lv_obj_set_pos(sys_big[i], x, y0 + 44);

        sys_pct[i] = lv_label_create(scr_system);
        lv_label_set_text(sys_pct[i], "%");
        lv_obj_set_style_text_font(sys_pct[i], &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(sys_pct[i], lv_color_white(), 0);
        lv_obj_align_to(sys_pct[i], sys_big[i], LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -8);

        sys_sub[i] = lv_label_create(scr_system);
        lv_label_set_text(sys_sub[i], "-");
        lv_obj_set_style_text_font(sys_sub[i], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(sys_sub[i], lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_pos(sys_sub[i], x, y0 + 104);

        sys_bar[i] = lv_bar_create(scr_system);
        lv_obj_set_size(sys_bar[i], barw, 8);
        lv_obj_set_pos(sys_bar[i], x, y0 + 136);
        lv_bar_set_range(sys_bar[i], 0, 100);
        lv_obj_set_style_bg_color(sys_bar[i], lv_color_hex(0x3A3A3A), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sys_bar[i], col, LV_PART_INDICATOR);
        lv_obj_set_style_radius(sys_bar[i], 4, LV_PART_MAIN);
        lv_obj_set_style_radius(sys_bar[i], 4, LV_PART_INDICATOR);

        if (i > 0) {
            lv_obj_t *ln = lv_obj_create(scr_system);
            if (!portrait) { lv_obj_set_size(ln, 1, 130); lv_obj_set_pos(ln, i * 213, 20); }
            else           { lv_obj_set_size(ln, 140, 1); lv_obj_set_pos(ln, 16, y0 - 8); }
            lv_obj_set_style_bg_color(ln, lv_color_hex(0x3A3A3A), 0);
            lv_obj_set_style_border_width(ln, 0, 0);
            lv_obj_set_style_radius(ln, 0, 0);
        }
    }

    /* --- 시계 화면 --- */
    scr_clock = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr_clock, 0, 0);
    lv_obj_set_style_bg_color(scr_clock, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(scr_clock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr_clock, clock_press_cb, LV_EVENT_PRESSED, NULL);
    clk_time = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(clk_time, &font_clock_110, 0);
    lv_obj_set_style_text_color(clk_time, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(clk_time, LV_TEXT_ALIGN_CENTER, 0);
    clk_date = lv_label_create(scr_clock);
    lv_obj_add_style(clk_date, &style_kr, 0);
    lv_obj_set_style_text_color(clk_date, lv_color_hex(0x9A9A9A), 0);
    lv_obj_set_style_text_align(clk_date, LV_TEXT_ALIGN_CENTER, 0);
    clk_city = lv_label_create(scr_clock);
    lv_obj_add_style(clk_city, &style_kr, 0);
    lv_obj_set_style_text_color(clk_city, lv_color_hex(0x9A9A9A), 0);
    clk_temp = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(clk_temp, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(clk_temp, lv_color_hex(0xFFFFFF), 0);
    clk_wx = lv_label_create(scr_clock);
    lv_obj_add_style(clk_wx, &style_kr, 0);
    lv_obj_set_style_text_color(clk_wx, lv_color_hex(0xCCCCCC), 0);
    clk_icon = lv_label_create(scr_clock);
    lv_obj_set_style_text_font(clk_icon, &font_wx_60, 0);
    lv_obj_set_style_text_color(clk_icon, lv_color_hex(0xFFD36B), 0);
    if (!portrait) {
        /* 왼쪽: 시각 + 날짜 / 오른쪽: 도시 · 기온 · 날씨 */
        lv_obj_set_width(clk_time, 380);
        lv_obj_align(clk_time, LV_ALIGN_LEFT_MID, 0, -14);
        lv_obj_set_width(clk_date, 380);
        lv_obj_align(clk_date, LV_ALIGN_BOTTOM_LEFT, 0, -10);
        lv_obj_t *ln = lv_obj_create(scr_clock);
        lv_obj_set_size(ln, 1, 120);
        lv_obj_align(ln, LV_ALIGN_LEFT_MID, 400, 0);
        lv_obj_set_style_bg_color(ln, lv_color_hex(0x333333), 0);
        lv_obj_set_style_border_width(ln, 0, 0);
        lv_obj_set_style_radius(ln, 0, 0);
        lv_obj_set_pos(clk_city, 428, 20);
        lv_obj_set_pos(clk_icon, 424, 48);          /* 아이콘 옆에 기온 */
        lv_obj_set_pos(clk_temp, 498, 54);
        lv_obj_set_pos(clk_wx,   428, 118);
        lv_obj_set_width(clk_city, 200); lv_label_set_long_mode(clk_city, LV_LABEL_LONG_DOT);
        lv_obj_set_width(clk_wx, 200);   lv_label_set_long_mode(clk_wx, LV_LABEL_LONG_DOT);
    } else {
        lv_obj_set_style_text_line_space(clk_time, -10, 0);
        lv_obj_align(clk_time, LV_ALIGN_TOP_MID, 0, 40);
        lv_obj_set_width(clk_date, 164);
        lv_label_set_long_mode(clk_date, LV_LABEL_LONG_WRAP);
        lv_obj_align(clk_date, LV_ALIGN_TOP_MID, 0, 230);
        lv_obj_set_width(clk_city, 164); lv_obj_set_style_text_align(clk_city, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(clk_city, LV_LABEL_LONG_DOT);
        lv_obj_align(clk_city, LV_ALIGN_TOP_MID, 0, 380);
        lv_obj_align(clk_icon, LV_ALIGN_TOP_MID, 0, 408);
        lv_obj_align(clk_temp, LV_ALIGN_TOP_MID, 0, 474);
        lv_obj_set_width(clk_wx, 164); lv_obj_set_style_text_align(clk_wx, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(clk_wx, LV_LABEL_LONG_WRAP);
        lv_obj_align(clk_wx, LV_ALIGN_TOP_MID, 0, 536);
    }
    lv_label_set_text(clk_time, "--:--");
    lv_label_set_text(clk_date, "");
    wx_refresh();

    /* --- 토스트 배너 (모든 화면 위) --- */
    toast_panel = lv_obj_create(lv_layer_top());
    if (!portrait) lv_obj_set_size(toast_panel, 620, 70);
    else           lv_obj_set_size(toast_panel, 160, 110);
    lv_obj_align(toast_panel, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_bg_color(toast_panel, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(toast_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_color(toast_panel, lv_color_hex(0x4A90D9), 0);
    lv_obj_set_style_border_width(toast_panel, 2, 0);
    lv_obj_set_style_radius(toast_panel, 10, 0);
    lv_obj_set_style_pad_all(toast_panel, 8, 0);
    lv_obj_clear_flag(toast_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(toast_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(toast_panel, toast_click_cb, LV_EVENT_CLICKED, NULL);

    toast_lbl1 = lv_label_create(toast_panel);
    lv_obj_add_style(toast_lbl1, &style_kr, 0);
    lv_obj_set_style_text_color(toast_lbl1, lv_color_hex(0x8EC5FF), 0);
    toast_lbl2 = lv_label_create(toast_panel);
    lv_obj_add_style(toast_lbl2, &style_kr, 0);
    lv_obj_set_style_text_color(toast_lbl2, lv_color_white(), 0);
    if (!portrait) {
        lv_label_set_long_mode(toast_lbl1, LV_LABEL_LONG_DOT);
        lv_label_set_long_mode(toast_lbl2, LV_LABEL_LONG_DOT);
        lv_obj_set_width(toast_lbl1, 600); lv_obj_set_pos(toast_lbl1, 0, 0);
        lv_obj_set_width(toast_lbl2, 600); lv_obj_set_pos(toast_lbl2, 0, 28);
    } else {
        lv_label_set_long_mode(toast_lbl1, LV_LABEL_LONG_DOT);
        lv_label_set_long_mode(toast_lbl2, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(toast_lbl1, 140); lv_obj_set_pos(toast_lbl1, 0, 0);
        lv_obj_set_size(toast_lbl2, 140, 60); lv_obj_set_pos(toast_lbl2, 0, 28);
    }
    lv_obj_add_flag(toast_panel, LV_OBJ_FLAG_HIDDEN);
}

/* ===== 방향 적용: 필요하면 해상도 바꾸고 UI 재생성 ===== */
/* ===== 방향 적용 ===== */
static void apply_orientation(int new_orient)
{
    bool portrait = (new_orient >= 2);
    deck_printf("O:%d\n", new_orient);              /* deck.py 콘솔에서 확인용 */

    if (portrait == ui_portrait) {             /* 180° 만 바뀜 */
        orient = new_orient;
        lv_obj_invalidate(lv_scr_act());
        return;
    }

    /* 1) 임시 빈 화면으로 넘기고 옛 위젯 전부 삭제 (메모리 확보) */
    lv_obj_t *tmp = lv_obj_create(NULL);
    lv_scr_load(tmp);
    lv_obj_del(scr_page);
    lv_obj_del(scr_music);
    lv_obj_del(scr_system);
    lv_obj_del(scr_clock);
    clk_city = clk_temp = clk_wx = clk_icon = NULL;
    lv_obj_del(toast_panel);
    toast_panel = NULL;
    lv_timer_pause(toast_timer);

    /* 2) 해상도 교체 */
    ui_portrait = portrait;
    orient = new_orient;
    disp_drv->hor_res = portrait ? 172 : 640;
    disp_drv->ver_res = portrait ? 640 : 172;
    lv_disp_drv_update(disp, disp_drv);

    /* 3) 새 방향으로 생성하고 데이터 다시 채움 */
    build_ui(portrait);
    refresh_music();
    refresh_colors();
    refresh_system();
    show_view(cur_view);
    lv_obj_del(tmp);
}

/* ===== IMU: 중력 방향으로 4방향 판정 (mg 단위) =====
 * ESP32 보드는 IMU 실장 방향이 RP2350 보드와 다를 수 있음.
 * 방향이 엉뚱하면 IMU_DEBUG 로 acc 값을 보고 아래 축/부호를 바꿀 것. */
#define IMU_DEBUG 0
/* 차량 진동·과속방지턱·급제동으로 방향이 오락가락하지 않도록:
 *  1) 가속도를 저역통과(지수평균)로 걸러 순간 충격 제거
 *  2) 한 축이 확실히 우세할 때만 방향 후보로 인정 (임계값 + 다른 축과의 차이)
 *  3) 같은 후보가 ORIENT_HOLD_MS 동안 계속 유지돼야 실제로 회전
 * 차에 고정해서 회전이 필요 없으면 ORIENT_LOCK 을 0~3 으로 지정하면 IMU 를 무시 */
#define ORIENT_HOLD_MS   5000
#define ORIENT_LOCK      (-1)       /* -1: 자동 회전, 0/1: 가로, 2/3: 세로 고정 */
#define ORIENT_THRESH    700.0f     /* mg — 1g 의 70% 이상 기울어야 후보 */
#define ORIENT_MARGIN    300.0f     /* mg — 다른 축보다 이만큼 커야 후보 */
#define IMU_ALPHA        0.3f       /* 지수평균 계수 (500ms 주기 기준 약 1.5초 시정수) */
static void imu_cb(lv_timer_t *t)
{
#if ORIENT_LOCK >= 0
    if (orient != ORIENT_LOCK && orient_pending < 0) orient_pending = ORIENT_LOCK;
    return;
#else
    static float f[3];
    static bool f_init = false;
    static int cand = -1;
    static uint32_t cand_since = 0;
    float acc[3];
    if (!deck_read_accel(acc)) return;
#if IMU_DEBUG
    deck_printf("acc:%d,%d,%d\n", (int)acc[0], (int)acc[1], (int)acc[2]);
#endif
    if (!f_init) { f[0] = acc[0]; f[1] = acc[1]; f[2] = acc[2]; f_init = true; }
    for (int i = 0; i < 3; i++) f[i] += IMU_ALPHA * (acc[i] - f[i]);

    float ax = f[0] < 0 ? -f[0] : f[0];
    float ay = f[1] < 0 ? -f[1] : f[1];
    int o = -1;
    if (ay > ORIENT_THRESH && ay > ax + ORIENT_MARGIN)      o = (f[1] < 0) ? 0 : 1;
    else if (ax > ORIENT_THRESH && ax > ay + ORIENT_MARGIN) o = (f[0] > 0) ? (PORTRAIT_SWAP ? 3 : 2) : (PORTRAIT_SWAP ? 2 : 3);

    uint32_t now = lv_tick_get();
    if (o < 0 || o == orient) { cand = -1; return; }          /* 애매하거나 현재와 같으면 대기 취소 */
    if (o != cand) { cand = o; cand_since = now; return; }    /* 새 후보 → 시간 재기 시작 */
    if (lv_tick_elaps(cand_since) >= ORIENT_HOLD_MS && orient_pending < 0) {
        orient_pending = o;
        cand = -1;
    }
#endif
}


static void sys_update_cb(lv_timer_t *t)
{
    if (wx_updated) { wx_updated = false; wx_refresh(); }
    clock_update();
    if (orient_pending >= 0) {
        int o = orient_pending;
        orient_pending = -1;
        apply_orientation(o);
    }

    if (bl_pending >= 0) {
        int on = bl_pending;
        bl_pending = -1;
        screen_off = !on;
        bl_apply();
    }

    if (mus_updated) {
        mus_updated = false;
        refresh_music();
        bl_apply();
        if (card_out && (strcmp(mus_title, card_title) != 0 ||
                         (card_out_dir > 0 && mus_pos < 3))) {
            if (!art_since_out) lv_obj_add_flag(mus_img, LV_OBJ_FLAG_HIDDEN);   /* 옛 커버 감추고 (새 커버가 먼저 왔으면 그대로) */
            deck_printf("X:new song, in from %s\n", card_out_dir < 0 ? "right" : "left");
            card_slide_in(-card_out_dir);                    /* 바로 들어옴 */
        }
    }
    if (card_out && lv_tick_elaps(card_out_t) > 5000) {
        deck_printf("X:timeout, back from %s\n", card_out_dir < 0 ? "left" : "right");
        card_slide_in(card_out_dir);
    }

    if (lyric_updated) {
        lyric_updated = false;
        refresh_lyrics();
    }

    if (art_updated) {
        art_updated = false;
        art_since_out = true;
        lv_obj_clear_flag(mus_img, LV_OBJ_FLAG_HIDDEN);     /* 새 커버 도착 → 표시 */
        lv_obj_invalidate(mus_img);
    }

    if (icon_dirty >= 0) {
        int sl = icon_dirty;
        icon_dirty = -1;
        lv_obj_invalidate(btn_img[sl]);
    }

    if (toast_updated) {
        toast_updated = false;
        toast_show();
    }

    if (bg_updated) {
        bg_updated = false;
        refresh_colors();
    }

    if (sys_updated) {
        sys_updated = false;
        refresh_system();
    }

}



/* ===== 초기화 ===== */
void ui_init(lv_disp_t *d, lv_disp_drv_t *drv)
{
    disp = d;
    disp_drv = drv;
    setenv("TZ", "KST-9", 1);                 /* 한국 표준시 (UTC+9) */
    tzset();

    lv_style_init(&style_kr);
    lv_style_set_text_font(&style_kr, &font_kr_20);

    lv_style_init(&style_small);
    lv_style_set_text_font(&style_small, &lv_font_montserrat_16);

    icons_clear();
    for (int i = 0; i < 6; i++) {
        icon_dsc[i].header.always_zero = 0;
        icon_dsc[i].header.w = ICON_W;
        icon_dsc[i].header.h = ICON_H;
        icon_dsc[i].header.cf = LV_IMG_CF_TRUE_COLOR;
        icon_dsc[i].data_size = ICON_BYTES;
        icon_dsc[i].data = icon_buf + i * ICON_BYTES;
    }
    memset(art_buf, 0x20, sizeof(art_buf));

    toast_timer = lv_timer_create(toast_hide_cb, 10000, NULL);
    lv_timer_pause(toast_timer);

    build_ui(false);
    refresh_music();
    refresh_system();

    sys_timer = lv_timer_create(sys_update_cb, 300, NULL);
    lv_timer_create(imu_cb, 500, NULL);

#if UI_HIDE_PAGE
    show_view(VIEW_MUSIC);           /* 차량용: 음악 화면으로 시작, 아이콘 요청(P:) 안 함 */
#else
    apply_page();
    lv_scr_load(scr_page);
#endif
    bl_apply();
}

/* ===== main.cpp 용 접근자 ===== */
int  ui_orient(void)     { return orient; }
bool ui_screen_off(void) { return screen_off; }
void ui_wake(void)
{
    if (screen_off && bl_pending < 0) {
        bl_pending = 1;
        deck_printf("W:\n");            /* PC 에 알림 → 유휴 타이머 리셋 */
    }
}
