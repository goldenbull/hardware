#ifndef MY_LIB_LCD
#define MY_LIB_LCD

#include <stdlib.h>
#include "DEV_Config.h"
#include "GUI_Paint.h"
#include "ImageData.h"
#include "Debug.h"
#include "LCD_1in14.h"
#include "Infrared.h"

#define KEY_A 15
#define KEY_B 17
#define KEY_up 2
#define KEY_dowm 18
#define KEY_left 16
#define KEY_right 20
#define KEY_ctrl 3

extern UWORD *Image;

void init_lcd();
void lcd_log_info(int x, int y, char *msg);
void lcd_log_error(int x, int y, char *msg);

#endif
