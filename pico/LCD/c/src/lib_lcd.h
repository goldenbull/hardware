#ifndef MY_LIB_LCD
#define MY_LIB_LCD

#include <stdlib.h>
#include "DEV_Config.h"
#include "GUI_Paint.h"
#include "ImageData.h"
#include "Debug.h"
#include "LCD_1in14.h"
#include "Infrared.h"

extern UWORD *Image; // global image for LCD
void init_lcd();

#endif
