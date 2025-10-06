/*****************************************************************************
* | File      	:   LCD_1in14_test.c
* | Function    :   test Demo
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2021-03-16
* | Info        :
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
******************************************************************************/
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/adc.h"
#include "lib_lcd.h"

UWORD *Image; // cached image for LCD

void init_lcd()
{
    printf("init LCD_1in14\n");
    DEV_Module_Init();
    DEV_SET_PWM(10);
    LCD_1IN14_Init(HORIZONTAL);
    LCD_1IN14_Clear(WHITE);

    UDOUBLE Imagesize = LCD_1IN14_HEIGHT * LCD_1IN14_WIDTH * 2;
    if ((Image = (UWORD *)malloc(Imagesize)) == NULL)
    {
        printf("Failed to apply for black memory...\r\n");
        exit(0);
    }

    // Create a new image cache named IMAGE_RGB and fill it
    Paint_NewImage((UBYTE *)Image, LCD_1IN14.WIDTH, LCD_1IN14.HEIGHT, 0, WHITE);
    Paint_SetScale(65);
    Paint_Clear(WHITE);
    Paint_SetRotate(ROTATE_0);
    Paint_Clear(WHITE);

    SET_Infrared_PIN(KEY_A);
    SET_Infrared_PIN(KEY_B);

    SET_Infrared_PIN(KEY_up);
    SET_Infrared_PIN(KEY_dowm);
    SET_Infrared_PIN(KEY_left);
    SET_Infrared_PIN(KEY_right);
    SET_Infrared_PIN(KEY_ctrl);
}

/* set address */
bool reserved_addr(uint8_t addr)
{
    return (addr & 0x78) == 0 || (addr & 0x78) == 0x78;
}

void init_ADC()
{
    adc_gpio_init(26); // 26
    adc_gpio_init(29); // 29
    adc_init();
    adc_set_round_robin(0);
    adc_set_temp_sensor_enabled(1);
}

double get_cpu_temp()
{
    adc_select_input(4); // 4
    uint16_t value = adc_read();
    double temp = 27.0 - (value * 3.3 / 4095 - 0.706f) / 0.001721;
    return temp;
}

int LCD_1in14_test(void)
{
    init_ADC();

    DEV_Delay_ms(100);
    printf("LCD_1in14_test Demo\r\n");
    if (DEV_Module_Init() != 0)
    {
        return -1;
    }
    DEV_SET_PWM(50);

    /* LCD Init */
    printf("1.14inch LCD demo...\r\n");
    LCD_1IN14_Init(HORIZONTAL);
    LCD_1IN14_Clear(WHITE);

    // LCD_SetBacklight(1023);
    UDOUBLE Imagesize = LCD_1IN14_HEIGHT * LCD_1IN14_WIDTH * 2;
    UWORD *Image;
    if ((Image = (UWORD *)malloc(Imagesize)) == NULL)
    {
        printf("Failed to apply for black memory...\r\n");
        exit(0);
    }

    // /*1.Create a new image cache named IMAGE_RGB and fill it with white*/
    Paint_NewImage((UBYTE *)Image, LCD_1IN14.WIDTH, LCD_1IN14.HEIGHT, 0, WHITE);
    Paint_SetScale(65);
    Paint_Clear(WHITE);
    Paint_SetRotate(ROTATE_0);
    Paint_Clear(WHITE);

    // /* GUI */
    printf("drawing...\r\n");
    // /*2.Drawing on the image*/
#if 1
    Paint_DrawPoint(2, 1, BLACK, DOT_PIXEL_1X1, DOT_FILL_RIGHTUP); // 240 240
    Paint_DrawPoint(2, 6, BLACK, DOT_PIXEL_2X2, DOT_FILL_RIGHTUP);
    Paint_DrawPoint(2, 11, BLACK, DOT_PIXEL_3X3, DOT_FILL_RIGHTUP);
    Paint_DrawPoint(2, 16, BLACK, DOT_PIXEL_4X4, DOT_FILL_RIGHTUP);
    Paint_DrawPoint(2, 21, BLACK, DOT_PIXEL_5X5, DOT_FILL_RIGHTUP);
    Paint_DrawLine(10, 5, 40, 35, MAGENTA, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
    Paint_DrawLine(10, 35, 40, 5, MAGENTA, DOT_PIXEL_2X2, LINE_STYLE_SOLID);

    Paint_DrawLine(80, 20, 110, 20, CYAN, DOT_PIXEL_1X1, LINE_STYLE_DOTTED);
    Paint_DrawLine(95, 5, 95, 35, CYAN, DOT_PIXEL_1X1, LINE_STYLE_DOTTED);

    Paint_DrawRectangle(10, 5, 40, 35, RED, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
    Paint_DrawRectangle(45, 5, 75, 35, BLUE, DOT_PIXEL_2X2, DRAW_FILL_FULL);

    Paint_DrawCircle(95, 20, 15, GREEN, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    Paint_DrawCircle(130, 20, 15, GREEN, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    Paint_DrawNum(50, 40, 9.87654321, &Font20, 3, WHITE, BLACK);
    Paint_DrawString_EN(1, 40, "ABC", &Font20, 0x000f, 0xfff0);
    Paint_DrawString_CN(1, 60, "»¶Ó­Ê¹ÓÃ", &Font24CN, WHITE, BLUE);
    Paint_DrawString_EN(1, 100, "Pico-LCD-1.14", &Font16, RED, WHITE);

    // /*3.Refresh the picture in RAM to LCD*/
    LCD_1IN14_Display(Image);
    DEV_Delay_ms(1000);
    DEV_SET_PWM(10);
#endif

#if 1

    Paint_DrawImage(gImage_1inch14_1, 0, 0, 240, 135);
    LCD_1IN14_Display(Image);
    DEV_Delay_ms(1000);
#endif

    LCD_1IN14_Display(Image);
    DEV_Delay_ms(1000);

    // start LCD
    // uint8_t keyA = 15;
    // uint8_t keyB = 17;

    // uint8_t up = 2;
    // uint8_t dowm = 18;
    // uint8_t left = 16;
    // uint8_t right = 20;
    // uint8_t ctrl = 3;

    // SET_Infrared_PIN(keyA);
    // SET_Infrared_PIN(keyB);

    // SET_Infrared_PIN(up);
    // SET_Infrared_PIN(dowm);
    // SET_Infrared_PIN(left);
    // SET_Infrared_PIN(right);
    // SET_Infrared_PIN(ctrl);

    Paint_Clear(WHITE);
    Paint_DrawRectangle(208, 12, 229, 32, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
    Paint_DrawRectangle(208, 103, 229, 123, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
    Paint_DrawRectangle(37, 35, 58, 55, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
    Paint_DrawRectangle(37, 85, 58, 105, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
    Paint_DrawRectangle(12, 60, 33, 80, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
    Paint_DrawRectangle(62, 60, 83, 80, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
    Paint_DrawRectangle(37, 60, 58, 80, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
    LCD_1IN14_Display(Image);

    int counter = 0;
    int cur_brightness = 100;
    double cur_temperature = 20;
    while (1)
    {
        if (counter % 10 == 0)
        {
            cur_temperature = get_cpu_temp();
        }

        Paint_Clear(WHITE);
        if (DEV_Digital_Read(KEY_A) == 0)
        {
            Paint_DrawRectangle(208, 12, 228, 32, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(208, 12, 228, 32, Image);
            printf("gpio =%d\r\n", KEY_A);

            cur_brightness += 10;
            if (cur_brightness > 100)
                cur_brightness = 100;
            DEV_SET_PWM(cur_brightness);
            // sleep_ms(500);
        }
        else
        {
            Paint_DrawRectangle(208, 12, 228, 32, WHITE, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(208, 12, 228, 32, Image);
        }

        if (DEV_Digital_Read(KEY_B) == 0)
        {
            Paint_DrawRectangle(208, 103, 228, 123, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(208, 103, 228, 123, Image);
            printf("gpio =%d\r\n", KEY_B);

            cur_brightness -= 10;
            if (cur_brightness < 0)
                cur_brightness = 0;
            DEV_SET_PWM(cur_brightness);
            // sleep_ms(500);
        }
        else
        {
            Paint_DrawRectangle(208, 103, 228, 123, WHITE, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(208, 103, 228, 123, Image);
        }

        if (DEV_Digital_Read(KEY_up) == 0)
        {
            Paint_DrawRectangle(37, 35, 57, 55, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(37, 35, 57, 55, Image);
            printf("gpio =%d\r\n", KEY_up);
        }
        else
        {
            Paint_DrawRectangle(37, 35, 57, 55, WHITE, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(37, 35, 57, 55, Image);
        }

        if (DEV_Digital_Read(KEY_dowm) == 0)
        {
            Paint_DrawRectangle(37, 85, 57, 105, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(37, 85, 57, 105, Image);
            printf("gpio =%d\r\n", KEY_dowm);
        }
        else
        {
            Paint_DrawRectangle(37, 85, 57, 105, WHITE, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(37, 85, 57, 105, Image);
        }

        if (DEV_Digital_Read(KEY_left) == 0)
        {
            Paint_DrawRectangle(12, 60, 32, 80, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(12, 60, 32, 80, Image);
            printf("gpio =%d\r\n", KEY_left);
        }
        else
        {
            Paint_DrawRectangle(12, 60, 32, 80, WHITE, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(12, 60, 32, 80, Image);
        }

        if (DEV_Digital_Read(KEY_right) == 0)
        {
            Paint_DrawRectangle(62, 60, 82, 80, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(62, 60, 82, 80, Image);
            printf("gpio =%d\r\n", KEY_right);
        }
        else
        {
            Paint_DrawRectangle(62, 60, 82, 80, WHITE, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(62, 60, 82, 80, Image);
        }

        if (DEV_Digital_Read(KEY_ctrl) == 0)
        {
            Paint_DrawRectangle(37, 60, 57, 80, 0xF800, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(37, 60, 57, 80, Image);
            printf("gpio =%d\r\n", KEY_ctrl);
        }
        else
        {
            Paint_DrawRectangle(37, 60, 57, 80, WHITE, DOT_PIXEL_2X2, DRAW_FILL_FULL);
            LCD_1IN14_DisplayWindows(37, 60, 57, 80, Image);
        }

        counter++;
        Paint_DrawNum(80, 20, counter, &Font20, 0, BLUE, YELLOW);
        Paint_DrawNum(80, 50, cur_brightness, &Font20, 0, BLUE, YELLOW);
        Paint_DrawNum(80, 80, cur_temperature, &Font20, 2, RED, YELLOW);
        LCD_1IN14_Display(Image);

        // cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, counter % 2);
    }

    /* Module Exit */
    free(Image);
    Image = NULL;

    DEV_Module_Exit();
    return 0;
}

void lcd_log_info(int x, int y, char *msg)
{
    Paint_DrawString_EN(x, y, msg, &Font16, GREEN, BLACK);
    LCD_1IN14_Display(Image);
}

void lcd_log_error(int x, int y, char *msg)
{
    Paint_DrawString_EN(x, y, msg, &Font16, RED, BLACK);
    LCD_1IN14_Display(Image);
}