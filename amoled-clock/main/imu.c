#include "imu.h"

#include <math.h>

#include "board_pins.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "imu";

/* 寄存器地址取自官方 qmi8658c 驱动的 Qmi8658Register 枚举 */
#define QMI8658_REG_WHOAMI 0x00
#define QMI8658_REG_CTRL1  0x02
#define QMI8658_REG_CTRL2  0x03
#define QMI8658_REG_CTRL7  0x08
#define QMI8658_REG_AX_L   0x35

#define QMI8658_WHOAMI_VALUE 0x05

/* CTRL1: bit6 ADDR_AI 地址自增，bit5 BE 保持 0 走小端，这样 (H<<8)|L 就是对的 */
#define QMI8658_CTRL1_VALUE 0x40
/* CTRL2: 量程 ±2g（高 3 位 000）+ ODR 低功耗 21Hz（0x0D）。测重力方向够用了 */
#define QMI8658_CTRL2_VALUE 0x0D
/* CTRL7: 只使能加速度计，陀螺仪不开 */
#define QMI8658_CTRL7_VALUE 0x01
/* ±2g 时 1g 对应的原始值 */
#define QMI8658_LSB_PER_G 16384.0f

#define IMU_POLL_MS 250

/*
 * 判定阈值。屏幕平放在桌面时重力几乎全在 Z 轴上，面内分量接近 0，
 * 这时方向是没法判断的，保持原样即可——所以要有个死区。
 */
#define TILT_THRESHOLD_G 0.35f
/* 连续这么多次读数一致才真的翻转，避免拿在手里晃的时候来回跳 */
#define STABLE_COUNT 4

static imu_orientation_cb_t s_on_change;
static bool                 s_flipped;

static esp_err_t imu_write_reg(uint8_t reg, uint8_t value)
{
    return i2c_bus_write_reg(I2C_ADDR_QMI8658, reg, &value, 1);
}

esp_err_t imu_read_accel(float *ax, float *ay, float *az)
{
    uint8_t buf[6];
    ESP_RETURN_ON_ERROR(i2c_bus_read_reg(I2C_ADDR_QMI8658, QMI8658_REG_AX_L, buf, sizeof(buf)),
                        TAG, "读加速度失败");

    const int16_t raw_x = (int16_t)((uint16_t)buf[1] << 8 | buf[0]);
    const int16_t raw_y = (int16_t)((uint16_t)buf[3] << 8 | buf[2]);
    const int16_t raw_z = (int16_t)((uint16_t)buf[5] << 8 | buf[4]);

    *ax = raw_x / QMI8658_LSB_PER_G;
    *ay = raw_y / QMI8658_LSB_PER_G;
    *az = raw_z / QMI8658_LSB_PER_G;
    return ESP_OK;
}

/*
 * 判断"屏幕上下"用的是 X 轴——实测结果，IMU 的 X 轴对着屏幕短边（竖直方向），
 * 官方资料里没有贴装方向的说明，是上机试出来的。
 *
 * 方向要是反了，把 IMU_UP_SIGN 改成 -1 即可。
 * 开机日志和每次方向切换都会打印三个轴的读数，照着对很快。
 */
#define IMU_UP_SIGN (+1.0f)

static float imu_screen_up_axis(float ax, float ay, float az)
{
    (void)ay;
    (void)az;
    return IMU_UP_SIGN * ax;
}

static void imu_task(void *arg)
{
    int stable = 0;
    bool candidate = s_flipped;

    while (1) {
        float ax, ay, az;
        if (imu_read_accel(&ax, &ay, &az) == ESP_OK) {
            const float up = imu_screen_up_axis(ax, ay, az);

            /* 死区内（平放/立起来侧着）说明判断不了，保持现状 */
            if (fabsf(up) >= TILT_THRESHOLD_G) {
                const bool want = (up < 0.0f);
                if (want != candidate) {
                    candidate = want;
                    stable = 0;
                } else if (want != s_flipped && ++stable >= STABLE_COUNT) {
                    s_flipped = want;
                    stable = 0;
                    ESP_LOGI(TAG, "方向切换：%s（ax=%.2f ay=%.2f az=%.2f）",
                             want ? "翻转 180°" : "正常", ax, ay, az);
                    if (s_on_change) {
                        s_on_change(want);
                    }
                }
            } else {
                stable = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(IMU_POLL_MS));
    }
}

esp_err_t imu_init(imu_orientation_cb_t on_change)
{
    s_on_change = on_change;

    uint8_t who = 0;
    ESP_RETURN_ON_ERROR(i2c_bus_read_reg(I2C_ADDR_QMI8658, QMI8658_REG_WHOAMI, &who, 1),
                        TAG, "QMI8658 无响应");
    ESP_RETURN_ON_FALSE(who == QMI8658_WHOAMI_VALUE, ESP_ERR_NOT_FOUND, TAG,
                        "WhoAmI = 0x%02X，期望 0x%02X", who, QMI8658_WHOAMI_VALUE);

    ESP_RETURN_ON_ERROR(imu_write_reg(QMI8658_REG_CTRL1, QMI8658_CTRL1_VALUE), TAG, "写 CTRL1 失败");
    ESP_RETURN_ON_ERROR(imu_write_reg(QMI8658_REG_CTRL2, QMI8658_CTRL2_VALUE), TAG, "写 CTRL2 失败");
    ESP_RETURN_ON_ERROR(imu_write_reg(QMI8658_REG_CTRL7, QMI8658_CTRL7_VALUE), TAG, "写 CTRL7 失败");
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 打一条原始读数，方便对照着确认 IMU 的贴装方向 */
    float ax = 0, ay = 0, az = 0;
    if (imu_read_accel(&ax, &ay, &az) == ESP_OK) {
        ESP_LOGI(TAG, "QMI8658 在线，当前加速度 ax=%.2f ay=%.2f az=%.2f (g)", ax, ay, az);
    }

    ESP_RETURN_ON_FALSE(xTaskCreate(imu_task, "imu", 3072, NULL, 2, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "创建 IMU 任务失败");

    ESP_LOGI(TAG, "自动翻转已启用，每 %dms 采样一次", IMU_POLL_MS);
    return ESP_OK;
}
