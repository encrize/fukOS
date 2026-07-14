#include <stdint.h>
#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static void wait_for_update(void) {
    for (uint32_t i = 0; i < 1000000u; i++) {
        if (!update_in_progress()) return;
    }
}

void rtc_read(rtc_time_t *t) {
    if (!t) return;
    uint8_t sec, min, hour, day, mon, yr;
    uint8_t lsec, lmin, lhour, lday, lmon, lyr;

    wait_for_update();
    sec = cmos_read(0x00); min = cmos_read(0x02); hour = cmos_read(0x04);
    day = cmos_read(0x07); mon = cmos_read(0x08); yr  = cmos_read(0x09);

    for (uint32_t attempt = 0; attempt < 16u; attempt++) {
        lsec = sec; lmin = min; lhour = hour; lday = day; lmon = mon; lyr = yr;
        wait_for_update();
        sec = cmos_read(0x00); min = cmos_read(0x02); hour = cmos_read(0x04);
        day = cmos_read(0x07); mon = cmos_read(0x08); yr  = cmos_read(0x09);
        if (lsec == sec && lmin == min && lhour == hour &&
            lday == day && lmon == mon && lyr == yr) break;
    }

    uint8_t regB = cmos_read(0x0B);
    int is_binary = regB & 0x04;
    int is_24h    = regB & 0x02;

    if (!is_binary) {
        uint8_t pm = hour & 0x80;
        sec  = (uint8_t)((sec  & 0x0F) + ((sec  >> 4) * 10));
        min  = (uint8_t)((min  & 0x0F) + ((min  >> 4) * 10));
        hour = (uint8_t)((((hour & 0x0F) + (((hour & 0x7F) >> 4) * 10))) | pm);
        day  = (uint8_t)((day  & 0x0F) + ((day  >> 4) * 10));
        mon  = (uint8_t)((mon  & 0x0F) + ((mon  >> 4) * 10));
        yr   = (uint8_t)((yr   & 0x0F) + ((yr   >> 4) * 10));
    }

    if (!is_24h) {
        int pm = hour & 0x80;
        int h  = hour & 0x7F;
        if (h == 12) h = pm ? 12 : 0;
        else if (pm) h += 12;
        hour = (uint8_t)h;
    }

    t->second = sec;
    t->minute = min;
    t->hour   = hour;
    t->day    = day;
    t->month  = mon;
    t->year   = (uint16_t)(2000 + yr);
}
