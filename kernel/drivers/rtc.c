#include "kaly.h"
static uint64_t epoch;
struct rtc_date {
    uint8_t second, minute, hour, day, month, year, century, status;
};
static uint8_t cmos(unsigned index) {
    outb(0x70, index | 0x80);
    return inb(0x71);
}
static unsigned decode(unsigned value, bool binary) {
    return binary ? value : (value >> 4) * 10 + (value & 15);
}
static bool leap(unsigned year) {
    return !(year % 4) && (year % 100 || !(year % 400));
}
static void snapshot(struct rtc_date *d) {
    uint64_t limit = 10000000;
    while (cmos(10) & 128) {
        if (!--limit) {
            panic("rtc timeout");
        }
    }
    d->second = cmos(0);
    d->minute = cmos(2);
    d->hour = cmos(4);
    d->day = cmos(7);
    d->month = cmos(8);
    d->year = cmos(9);
    d->century = cmos(0x32);
    d->status = cmos(11);
}
void rtc_init(void) {
    struct rtc_date a, b;
    snapshot(&a);
    for (unsigned attempt = 0;; attempt++) {
        snapshot(&b);
        if (!memcmp(&a, &b, sizeof(a))) {
            break;
        }
        if (attempt == 100) {
            panic("rtc unstable");
        }
        a = b;
    }
    outb(0x70, 0);
    bool binary = a.status & 4;
    unsigned second = decode(a.second, binary), minute = decode(a.minute, binary);
    unsigned hour = decode(a.hour & 127, binary), day = decode(a.day, binary),
             month = decode(a.month, binary);
    unsigned year = decode(a.year, binary), century = decode(a.century, binary);
    if (!(a.status & 2)) {
        hour = hour % 12 + (a.hour & 128 ? 12 : 0);
    }
    year += century >= 19 && century <= 99 ? century * 100 : year >= 70 ? 1900 : 2000;
    if (year < 1970 || second > 59 || minute > 59 || hour > 23 || month < 1 || month > 12 ||
        day < 1 || day > 31) {
        panic("rtc date");
    }
    static const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    uint64_t total = day - 1;
    for (unsigned y = 1970; y < year; y++) {
        total += leap(y) ? 366 : 365;
    }
    for (unsigned m = 1; m < month; m++) {
        total += days[m - 1] + (m == 2 && leap(year));
    }
    epoch = total * 86400 + hour * 3600 + minute * 60 + second;
}
uint64_t kernel_time(void) {
    return epoch + ticks / 100;
}
