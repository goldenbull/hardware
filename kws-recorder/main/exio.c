#include "exio.h"

#include "board_pins.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "exio";

/*
 * 不用 waveshare/custom_io_expander_ch32v003 组件：它的 CMakeLists 只 REQUIRES "driver"，
 * 在 IDF 6 上找不到 driver/i2c_master.h，编不过。协议本身就是往 0x24 写"寄存器 + 1 字节"，
 * 下面的寄存器地址和上电默认值都抄自该组件 1.0.2。
 */
#define CH32V003_ADDR      0x24
#define REG_DIRECTION      0x02    /* 1 = 输出 */
#define REG_OUTPUT         0x03
#define REG_PWM            0x05    /* 背光，0~255 */

#define PIN(n)             (1U << (n))

/*
 * 方向和官方 BSP 最终的状态一致：组件复位时把 8 个脚全设成输出，BSP 再把 EXIO6
 * 改成输入。EXIO6 接的是什么原理图上没标，照 BSP 设成输入，免得和外面的信号顶牛。
 */
#define DIRECTION_VAL      (0xFF & ~PIN(6))

/* FT6336 放开复位后要两三百毫秒才会应答 I2C，ST7796 要 120ms 才能收命令，取大的 */
#define RESET_PULSE_MS     50
#define RESET_RELEASE_MS   300

static uint8_t s_output;    /* 输出寄存器的影子，芯片的输出寄存器读不回来 */

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    return i2c_bus_write_reg(CH32V003_ADDR, reg, &val, 1);
}

static esp_err_t set_pins(uint8_t mask, bool high)
{
    const uint8_t out = high ? (s_output | mask) : (s_output & ~mask);
    ESP_RETURN_ON_ERROR(write_reg(REG_OUTPUT, out), TAG, "写输出寄存器失败");
    s_output = out;
    return ESP_OK;
}

esp_err_t exio_init(void)
{
    const uint8_t resets = PIN(EXIO_TOUCH_RST) | PIN(EXIO_LCD_RST);

    /* 先把输出全拉低（屏、触摸都在复位中，功放关）再切方向，避免方向一切过去就输出不确定的电平 */
    s_output = 0;
    ESP_RETURN_ON_ERROR(write_reg(REG_OUTPUT, s_output), TAG, "CH32V003 无响应");
    ESP_RETURN_ON_ERROR(write_reg(REG_DIRECTION, DIRECTION_VAL), TAG, "写方向寄存器失败");

    /* 和官方 BSP 一样的时序：高 → 低 → 高。功放全程关着 */
    ESP_RETURN_ON_ERROR(set_pins(resets, true), TAG, "reset high failed");
    vTaskDelay(pdMS_TO_TICKS(RESET_PULSE_MS));
    ESP_RETURN_ON_ERROR(set_pins(resets, false), TAG, "reset low failed");
    vTaskDelay(pdMS_TO_TICKS(RESET_PULSE_MS));
    ESP_RETURN_ON_ERROR(set_pins(resets, true), TAG, "reset release failed");
    vTaskDelay(pdMS_TO_TICKS(RESET_RELEASE_MS));

    /* 屏初始化完之前不亮背光，免得看到上电时的花屏 */
    ESP_RETURN_ON_ERROR(exio_set_backlight(0), TAG, "backlight off failed");

    ESP_LOGI(TAG, "IO 扩展就绪：屏和触摸已复位，功放关闭");
    return ESP_OK;
}

esp_err_t exio_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return write_reg(REG_PWM, (uint8_t)(percent * 255 / 100));
}

esp_err_t exio_set_pa(bool on)
{
    return set_pins(PIN(EXIO_PA_EN), on);
}
