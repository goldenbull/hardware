#ifndef MY_LIB_LCD
#define MY_LIB_LCD

#include <stdlib.h>
#include "DEV_Config.h"
#include "GUI_Paint.h"
#include "ImageData.h"
#include "Debug.h"
#include "LCD_1in14.h"
#include "Infrared.h"

extern UWORD *Image;

void init_lcd();
void lcd_log_info(int x, int y, char *msg);
void lcd_log_error(int x, int y, char *msg);

#endif
