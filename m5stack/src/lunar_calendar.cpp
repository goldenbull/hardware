#include "lunar_calendar.h"

#include <stdint.h>
#include <stdio.h>

namespace
{
constexpr int kFirstYear = 1900;
constexpr int kLastYear  = 2100;

// One entry per year from 1900 to 2100. Bits 15..4 are months 1..12, set when
// the month is long (30 days); bits 3..0 hold the leap month number, 0 for none;
// bit 16 is set when that leap month is long.
const uint32_t kLunarYears[] = {
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2, // 1900
    0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977, // 1910
    0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970, // 1920
    0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950, // 1930
    0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557, // 1940
    0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5b0, 0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0, // 1950
    0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, 0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0, // 1960
    0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540, 0x0b6a0, 0x195a6, // 1970
    0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570, // 1980
    0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58, 0x055c0, 0x0ab60, 0x096d5, 0x092e0, // 1990
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5, // 2000
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930, // 2010
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530, // 2020
    0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45, // 2030
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0, // 2040
    0x14b63, 0x09370, 0x049f8, 0x04970, 0x064b0, 0x168a6, 0x0ea50, 0x06b20, 0x1a6c4, 0x0aae0, // 2050
    0x0a2e0, 0x0d2e3, 0x0c960, 0x0d557, 0x0d4a0, 0x0da50, 0x05d55, 0x056a0, 0x0a6d0, 0x055d4, // 2060
    0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0, 0x0b6a6, 0x0ad50, 0x055a0, 0x0aba4, 0x0a5b0, 0x052b0, // 2070
    0x0b273, 0x06930, 0x07337, 0x06aa0, 0x0ad50, 0x14b55, 0x04b60, 0x0a570, 0x054e4, 0x0d160, // 2080
    0x0e968, 0x0d520, 0x0daa0, 0x16aa6, 0x056d0, 0x04ae0, 0x0a9d4, 0x0a2d0, 0x0d150, 0x0f252, // 2090
    0x0d520,                                                                                  // 2100
};

const char* const kHeavenlyStems[]   = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
const char* const kEarthlyBranches[] = {"子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"};
const char* const kMonthNames[]
    = {"正月", "二月", "三月", "四月", "五月", "六月", "七月", "八月", "九月", "十月", "冬月", "腊月"};
const char* const kDayTens[]  = {"初", "十", "廿", "三"};
const char* const kDayUnits[] = {"十", "一", "二", "三", "四", "五", "六", "七", "八", "九"};

uint32_t yearInfo(int year)
{
    return kLunarYears[year - kFirstYear];
}

int leapMonth(int year)
{
    return static_cast<int>(yearInfo(year) & 0xF);
}

int leapMonthDays(int year)
{
    if (leapMonth(year) == 0)
        return 0;
    return (yearInfo(year) & 0x10000) ? 30 : 29;
}

int monthDays(int year, int month)
{
    return (yearInfo(year) & (0x10000u >> month)) ? 30 : 29;
}

int yearDays(int year)
{
    int sum = 348; // twelve 29-day months, before the long-month bits are added
    for (uint32_t bit = 0x8000; bit > 0x8; bit >>= 1)
        sum += (yearInfo(year) & bit) ? 1 : 0;
    return sum + leapMonthDays(year);
}

// Days since 1970-01-01, by Howard Hinnant's civil-date algorithm. Used only to
// difference two dates, so the epoch itself does not matter.
long daysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + doe - 719468L;
}

const char* dayName(int day, char* buffer, size_t size)
{
    if (day == 10 || day == 20 || day == 30)
    {
        // "初十", "二十" and "三十" break the tens-plus-units pattern.
        static const char* const kRoundDays[] = {"初十", "二十", "三十"};
        snprintf(buffer, size, "%s", kRoundDays[day / 10 - 1]);
    }
    else
    {
        snprintf(buffer, size, "%s%s", kDayTens[day / 10], kDayUnits[day % 10]);
    }
    return buffer;
}
} // namespace

bool toLunar(const tm& date, LunarDate& out)
{
    const int year  = date.tm_year + 1900;
    const int month = date.tm_mon + 1;
    const int day   = date.tm_mday;
    if (year < kFirstYear || year > kLastYear)
        return false;

    // 1900-01-31 is the first day of the first lunar year the table covers.
    long offset = daysFromCivil(year, month, day) - daysFromCivil(kFirstYear, 1, 31);
    if (offset < 0)
        return false;

    int lunar_year = kFirstYear;
    int span       = 0;
    while (lunar_year <= kLastYear && offset > 0)
    {
        span = yearDays(lunar_year);
        offset -= span;
        ++lunar_year;
    }
    if (offset < 0)
    {
        offset += span;
        --lunar_year;
    }
    if (lunar_year > kLastYear)
        return false;

    const int leap    = leapMonth(lunar_year);
    bool      in_leap = false;
    int       month_index;
    span = 0;
    for (month_index = 1; month_index < 13 && offset > 0; ++month_index)
    {
        // The leap month is walked as a second pass over the same index.
        if (leap > 0 && month_index == leap + 1 && !in_leap)
        {
            --month_index;
            in_leap = true;
            span    = leapMonthDays(lunar_year);
        }
        else
        {
            span = monthDays(lunar_year, month_index);
        }
        if (in_leap && month_index == leap + 1)
            in_leap = false;
        offset -= span;
    }
    // Landing exactly on a boundary means the first day of the next month, which
    // is the leap month when one follows.
    if (offset == 0 && leap > 0 && month_index == leap + 1)
    {
        if (in_leap)
        {
            in_leap = false;
        }
        else
        {
            in_leap = true;
            --month_index;
        }
    }
    if (offset < 0)
    {
        offset += span;
        --month_index;
    }

    out.year  = lunar_year;
    out.month = month_index;
    out.day   = static_cast<int>(offset) + 1;
    out.leap  = in_leap;
    return true;
}

bool formatLunarDate(const tm& date, char* out, size_t size)
{
    LunarDate lunar{};
    if (!toLunar(date, lunar))
    {
        if (size > 0)
            out[0] = '\0';
        return false;
    }

    // 1864 was a 甲子 year, so the sexagenary cycle indexes straight off it.
    const int cycle = lunar.year - 1864;

    char day_name[8];
    snprintf(out,
             size,
             "%s%s年 %s%s%s",
             kHeavenlyStems[cycle % 10],
             kEarthlyBranches[cycle % 12],
             lunar.leap ? "闰" : "",
             kMonthNames[lunar.month - 1],
             dayName(lunar.day, day_name, sizeof(day_name)));
    return true;
}
