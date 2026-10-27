/*
 * deck_esp32 — main.cpp
 * Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B QSPI, 172x640)
 * 09_LVGL_V8_Test 예제 기반. 하드웨어 초기화 / flush / 터치 / USB 시리얼 / IMU 담당.
 * UI 는 ui.c (RP2350 LVGL_example.c 이식).
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "user_config.h"
#include "esp_lcd_axs15231b.h"
#include "i2c_bsp.h"
#include "lcd_bl_pwm_bsp.h"

#include "deck_io.h"
#include "ui.h"
#include "qmi8658.h"
#include "net.h"
#include "ble.h"

static const char *TAG = "deck";

/* 물리 패널 */
#define PHYS_W 172
#define PHYS_H 640
#define LCD_BIT_PER_PIXEL 16

/* 부팅 로그 이후 ESP_LOG 를 끌지 여부.
 * USB 드라이버 모드에서는 호스트(PC)가 포트를 안 열고 있으면 printf/ESP_LOG 가
 * 송신 버퍼가 찰 때 블로킹될 수 있어 기본적으로 끈다. 디버깅 시 0 으로. */
#define DECK_SILENCE_LOGS 1

/* 전송로 선택. 차량용은 BLE(핫스팟 5GHz 유지). Wi-Fi 는 PC 디버깅용 — 둘 다 켜면 무선 공유로 느려질 수 있음 */
#define DECK_ENABLE_BLE  1
#define DECK_ENABLE_WIFI 0

static SemaphoreHandle_t lvgl_mux = NULL;
static SemaphoreHandle_t lvgl_flush_semap = NULL;
static uint16_t *lvgl_dma_buf = NULL;     /* 내부 RAM, DMA 청크 */
static uint16_t *rotat_ptr = NULL;        /* PSRAM, 회전된 전체 프레임 (물리 172x640) */

static lv_disp_drv_t disp_drv;
static lv_disp_draw_buf_t disp_buf;
static lv_disp_t *disp = NULL;

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t []){0x00}, 0, 100},
    {0x29, (uint8_t []){0x00}, 0, 100},
};

/* ===================== deck_io 구현 ===================== */
void deck_lock(void)   { xSemaphoreTake(lvgl_mux, portMAX_DELAY); }
void deck_unlock(void) { xSemaphoreGive(lvgl_mux); }

/* 수신 공용 버퍼: USB 와 TCP 가 모두 여기로 넣고, ui.c 의 시리얼 태스크가 여기서 읽는다 */
static StreamBufferHandle_t rx_stream = NULL;

void deck_rx_push(const uint8_t *buf, int len)
{
    if (!rx_stream || len <= 0) return;
    /* 전부 들어갈 때까지 밀어 넣는다 (아트 20KB > 버퍼 크기). 수신 태스크만 잠시 대기 */
    while (len > 0) {
        size_t n = xStreamBufferSend(rx_stream, buf, len, pdMS_TO_TICKS(1000));
        if (n == 0) break;                /* 1초 넘게 안 비워지면 포기 (UI 정지 등 비정상) */
        buf += n; len -= (int)n;
    }
}

void deck_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    /* USB: 호스트가 안 읽으면 20ms 후 포기 (블로킹 방지) */
    usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(20));
    /* TCP / BLE: 클라이언트 있으면 논블로킹 송신 */
#if DECK_ENABLE_WIFI
    net_send(buf, n);
#endif
#if DECK_ENABLE_BLE
    ble_send(buf, n);
#endif
}

int deck_read(uint8_t *buf, int len, int timeout_ms)
{
    return (int)xStreamBufferReceive(rx_stream, buf, len, pdMS_TO_TICKS(timeout_ms));
}

/* USB 수신 → 공용 버퍼 */
static void usb_rx_task(void *arg)
{
    static uint8_t tmp[256];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(tmp, sizeof(tmp), pdMS_TO_TICKS(100));
        if (n > 0) deck_rx_push(tmp, n);
    }
}

void deck_set_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* lcd_bl_pwm_bsp 는 duty 가 반전 (0 = 최대 밝기, 255 = 꺼짐) */
    setUpduty((uint16_t)(255 - percent * 255 / 100));
}

bool deck_read_accel(float acc[3]) { return qmi8658_read_mg(acc); }

uint32_t deck_millis(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

/* ===================== 디스플레이 ===================== */
static bool notify_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(lvgl_flush_semap, &woken);
    return woken == pdTRUE;
}

/* LVGL 버퍼(가로 640x172 또는 세로 172x640) → 물리 172x640 으로 4방향 회전 복사 후 청크 DMA */
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    uint16_t *src = (uint16_t *)color_map;
    uint16_t *dst = rotat_ptr;

    switch (ui_orient()) {
    case 0:   /* 가로: LVGL 640x172 → 물리 172x640 */
        for (int py = 0; py < PHYS_H; py++)
            for (int y = 0; y < PHYS_W; y++)
                dst[py * PHYS_W + y] = src[(PHYS_W - 1 - y) * PHYS_H + py];
        break;
    case 1:   /* 가로 180° */
        for (int py = 0; py < PHYS_H; py++)
            for (int y = 0; y < PHYS_W; y++)
                dst[py * PHYS_W + y] = src[y * PHYS_H + (PHYS_H - 1 - py)];
        break;
    case 2:   /* 세로: 물리 그대로 */
        memcpy(dst, src, PHYS_W * PHYS_H * 2);
        break;
    default:  /* 세로 180° */
        for (int py = 0; py < PHYS_H; py++)
            for (int y = 0; y < PHYS_W; y++)
                dst[py * PHYS_W + y] = src[(PHYS_H - 1 - py) * PHYS_W + (PHYS_W - 1 - y)];
        break;
    }

    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    const int flush_count = (PHYS_W * PHYS_H * 2) / LVGL_DMA_BUFF_LEN;   /* 640/64 = 10 */
    const int rows = PHYS_H / flush_count;                                /* 64 */
    const int dmalen = LVGL_DMA_BUFF_LEN / 2;                             /* 픽셀 수 */
    uint16_t *map = dst;
    int y1 = 0, y2 = rows;

    xSemaphoreGive(lvgl_flush_semap);
    for (int i = 0; i < flush_count; i++) {
        xSemaphoreTake(lvgl_flush_semap, portMAX_DELAY);
        memcpy(lvgl_dma_buf, map, LVGL_DMA_BUFF_LEN);
        esp_lcd_panel_draw_bitmap(panel, 0, y1, PHYS_W, y2, lvgl_dma_buf);
        y1 += rows; y2 += rows;
        map += dmalen;
    }
    xSemaphoreTake(lvgl_flush_semap, portMAX_DELAY);
    lv_disp_flush_ready(drv);
}

/* ===================== 터치 ===================== */
/* AXS15231B 터치: raw pointX 0..640, pointY 0..172 (예제 기준)
 * 물리 좌표 ts_x(0..171), ts_y(0..639) 로 바꾼 뒤 방향별 변환 (RP2350 Touch.c 와 동일 규약) */
#define TOUCH_RELEASE_MISS 3   /* 연속 3회(약 90ms) 안 눌려야 손 뗌으로 처리 */
static void touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    static uint16_t last_x = 0, last_y = 0;
    static bool     was_pressed = false;
    static uint8_t  miss = 0;              /* 눌린 상태에서 연속으로 '안 눌림/읽기 실패'가 나온 횟수 */
    uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e, 0x0, 0x0, 0x0};
    uint8_t buff[32] = {0};

    bool ok = (i2c_master_touch_write_read(disp_touch_dev_handle, cmd, 11, buff, 32) == ESP_OK);
    bool pressed = ok && (buff[1] > 0 && buff[1] < 5);
    if (!pressed) {
        /* 드래그 중 한두 번 끊겨 읽히는 것을 손 뗌으로 보지 않음 → 슬라이드가 중간에 리셋되지 않게 */
        if (was_pressed && ++miss < TOUCH_RELEASE_MISS) {
            data->state = LV_INDEV_STATE_PR;
        } else {
            was_pressed = false; miss = 0;
            data->state = LV_INDEV_STATE_REL;
        }
        data->point.x = last_x; data->point.y = last_y;
        return;
    }
    miss = 0;

    if (ui_screen_off()) {                 /* 꺼져 있으면 깨우기만, 입력은 무시 */
        ui_wake();
        data->state = LV_INDEV_STATE_REL;
        data->point.x = last_x; data->point.y = last_y;
        return;
    }

    uint16_t pX = (((uint16_t)buff[2] & 0x0f) << 8) | buff[3];
    uint16_t pY = (((uint16_t)buff[4] & 0x0f) << 8) | buff[5];
    if (pX > PHYS_H) pX = PHYS_H;
    if (pY > PHYS_W - 1) pY = PHYS_W - 1;
    int ts_x = pY;                 /* 0..171 */
    int ts_y = PHYS_H - pX;        /* 0..640 */
    if (ts_y > PHYS_H - 1) ts_y = PHYS_H - 1;

    int x, y;
    switch (ui_orient()) {
    case 0:  x = ts_y;              y = PHYS_W - 1 - ts_x; break;
    case 1:  x = PHYS_H - 1 - ts_y; y = ts_x;              break;
    case 2:  x = ts_x;              y = ts_y;              break;
    default: x = PHYS_W - 1 - ts_x; y = PHYS_H - 1 - ts_y; break;
    }
    last_x = x; last_y = y;
    was_pressed = true;
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
}

/* ===================== 태스크 ===================== */
static void lvgl_tick_cb(void *arg) { lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS); }

static void lvgl_task(void *arg)
{
    for (;;) {
        deck_lock();
        uint32_t d = lv_timer_handler();
        deck_unlock();
        if (d > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) d = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        if (d < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) d = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(d));
    }
}

static void serial_task(void *arg)
{
    for (;;) ui_serial_task_loop();
}

/* ===================== app_main ===================== */
extern "C" void app_main(void)
{
    /* USB 시리얼 (프로토콜 채널) */
    usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_cfg.rx_buffer_size = 8192;
    usb_cfg.tx_buffer_size = 2048;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));
    ESP_LOGI(TAG, "usb serial ready");
#if DECK_SILENCE_LOGS
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_log_level_set("net", ESP_LOG_INFO);      /* Wi-Fi/BLE 연결 로그만 남김 */
    esp_log_level_set("ble", ESP_LOG_INFO);
#endif
    rx_stream = xStreamBufferCreate(32768, 1);   /* 아트 20KB 가 통째로 들어가게 */
    assert(rx_stream);
    xTaskCreatePinnedToCore(usb_rx_task, "usb_rx", 3072, NULL, 3, NULL, 1);

    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

    rotat_ptr = (uint16_t *)heap_caps_malloc(PHYS_W * PHYS_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    assert(rotat_ptr);
    lvgl_flush_semap = xSemaphoreCreateBinary();
    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);

    i2c_master_Init();
    qmi8658_init();

    /* LCD reset GPIO */
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_conf.mode = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask = ((uint64_t)1 << EXAMPLE_PIN_NUM_LCD_RST);
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    /* QSPI */
    spi_bus_config_t buscfg = {};
    buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
    buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
    buscfg.sclk_io_num  = EXAMPLE_PIN_NUM_LCD_PCLK;
    buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
    buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
    buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;
    io_config.dc_gpio_num = -1;
    io_config.spi_mode = 3;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.on_color_trans_done = notify_flush_ready;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &panel_io));

    axs15231b_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
    panel_config.vendor_config = &vendor_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &panel));

    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    /* LVGL */
    lv_init();
    lvgl_dma_buf = (uint16_t *)heap_caps_malloc(LVGL_DMA_BUFF_LEN, MALLOC_CAP_DMA);
    assert(lvgl_dma_buf);
    lv_color_t *buffer_1 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
    lv_color_t *buffer_2 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
    assert(buffer_1 && buffer_2);
    lv_disp_draw_buf_init(&disp_buf, buffer_1, buffer_2, PHYS_W * PHYS_H);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = PHYS_H;      /* 시작은 가로 640x172 */
    disp_drv.ver_res = PHYS_W;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.full_refresh = 1;      /* 반드시 1 */
    disp_drv.user_data = panel;
    disp = lv_disp_drv_register(&disp_drv);

    esp_timer_create_args_t tick_args = {};
    tick_args.callback = &lvgl_tick_cb;
    tick_args.name = "lvgl_tick";
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_cb;
    lv_indev_drv_register(&indev_drv);

    /* UI 생성 */
    deck_lock();
    ui_init(disp, &disp_drv);
    deck_unlock();

    xTaskCreatePinnedToCore(lvgl_task,   "LVGL",   8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(serial_task, "serial", 4096, NULL, 3, NULL, 1);

    /* 전송로 시작 (화면 뜬 뒤) */
#if DECK_ENABLE_BLE
    ble_start();
#endif
#if DECK_ENABLE_WIFI
    net_start();
#endif
}
