#pragma once

#include <stddef.h>
#include <time.h>

// Chinese lunisolar calendar, 1900-01-31 through the end of 2100. The month
// lengths and leap months are not computable from a formula, so they come from
// a packed per-year table; dates outside that range cannot be converted.

struct LunarDate
{
    int  year;  // the lunar year, which rolls over at Spring Festival
    int  month; // 1-12
    int  day;   // 1-30
    bool leap;  // true when this is the intercalary repeat of `month`
};

bool toLunar(const tm& date, LunarDate& out);

// Writes e.g. "甲子年 八月十五" (leap months get a 闰 prefix on the month).
// Returns false and writes an empty string when the date is out of range.
bool formatLunarDate(const tm& date, char* out, size_t size);
