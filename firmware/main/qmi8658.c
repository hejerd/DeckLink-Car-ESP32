/* QMI8658 최소 드라이버 — 가속도만 (방향 판정용) */
#include <stdio.h>
#include <string.h>
#include "qmi8658.h"
#include "i2c_bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REG_WHO_AM_I 0x00
#define REG_CTRL1    0x02
#define REG_CTRL2    0x03
#define REG_CTRL7    0x08
#define REG_AX_L     0x35

static const char *TAG = "qmi8658";
static bool ready = false;

bool qmi8658_init(void)
{
    uint8_t id = 0;
    if (i2c_read_buff(imu_dev_handle, REG_WHO_AM_I, &id, 1) != 0 || id != 0x05) {
        ESP_LOGW(TAG, "not found (id=0x%02x)", id);
        return false;
    }
    uint8_t v;
    v = 0x40;  i2c_writr_buff(imu_dev_handle, REG_CTRL1, &v, 1);   /* addr auto-increment */
    v = 0x15;  i2c_writr_buff(imu_dev_handle, REG_CTRL2, &v, 1);   /* aFS=±4g, aODR=224Hz */
    v = 0x01;  i2c_writr_buff(imu_dev_handle, REG_CTRL7, &v, 1);   /* accel enable, gyro off */
    vTaskDelay(pdMS_TO_TICKS(20));
    ready = true;
    ESP_LOGI(TAG, "init ok");
    return true;
}

bool qmi8658_read_mg(float acc[3])
{
    if (!ready) return false;
    uint8_t b[6];
    if (i2c_read_buff(imu_dev_handle, REG_AX_L, b, 6) != 0) return false;
    for (int i = 0; i < 3; i++) {
        int16_t raw = (int16_t)((b[i * 2 + 1] << 8) | b[i * 2]);
        acc[i] = raw * 1000.0f / 8192.0f;     /* ±4g → 8192 LSB/g */
    }
    return true;
}
