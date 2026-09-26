// The `time` standard library module. The build links it only for a program
// that imports 'time'. Date formatting and the scheduler timer are core, so a
// program reaches them with no import.
//
// The signatures must match timeCallType and timeDateCallType in the checker,
// and genTimeCall and genTimeDateCall in codegen.
//
// Both types of the module are an int64 count of milliseconds. A DateTime
// counts from the Unix epoch and a Duration from no fixed point. Only the
// checker tells them apart, so the arithmetic that relates the two needs no
// runtime support and is absent here.
//
// The rt_date_* functions are the calendar half, which the language writes as
// time.date.*. time.milliseconds has no entry point, because a Duration is
// already its count.

#include "runtime.h"

#include <stdio.h>
#include <string.h>

int64_t rt_time_now(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

Future *rt_time_sleep(int64_t ms) { return rt_async_timer_new(ms); }

// Reads "<count><unit>" as a signed count of milliseconds, where the unit is
// s, m, h or d. The count is decimal digits with an optional sign. The unit
// ends the string. Spaces, a second term and a fraction are not permitted.
//
// All other input stops the program. A program writes a duration as a
// literal, so a bad form is a defect in the program.
//
// "d" is 24 hours. A civil day needs a calendar and a zone, and a Duration
// has neither.
int64_t rt_time_duration(Str *s) {
    const char *p = s->data;
    const char *end = s->data + s->len;
    int neg = 0;
    if (p < end && (*p == '-' || *p == '+')) neg = (*p++ == '-');

    int64_t count = 0;
    const char *digits = p;
    for (; p < end && *p >= '0' && *p <= '9'; p++) {
        int64_t d = *p - '0';
        if (count > (INT64_MAX - d) / 10) rt_panic("duration out of range");
        count = count * 10 + d;
    }
    if (p == digits || p + 1 != end) {
        rt_panic("invalid duration (want a count and one of s, m, h, d, e.g. \"10s\")");
    }

    int64_t unit;
    switch (*p) {
    case 's': unit = 1000LL; break;
    case 'm': unit = 60LL * 1000; break;
    case 'h': unit = 60LL * 60 * 1000; break;
    case 'd': unit = 24LL * 60 * 60 * 1000; break;
    default:
        rt_panic("invalid duration unit (want s, m, h, or d)");
        return 0;
    }
    if (count > INT64_MAX / unit) rt_panic("duration out of range");
    count *= unit;
    return neg ? -count : count;
}

int64_t rt_date_year(int64_t ms) {
    struct tm t;
    rt_date_tm(ms, &t);
    return t.tm_year + 1900;
}

int64_t rt_date_month(int64_t ms) {
    struct tm t;
    rt_date_tm(ms, &t);
    return t.tm_mon + 1;
}

int64_t rt_date_day(int64_t ms) {
    struct tm t;
    rt_date_tm(ms, &t);
    return t.tm_mday;
}

int64_t rt_date_add_days(int64_t ms, int64_t days) {
    return ms + days * 86400000LL;
}

int64_t rt_date_from_text(Str *s) {
    char buf[64];
    int64_t result = 0;
    if (s->len > 0 && s->len < (int64_t)sizeof(buf)) {
        memcpy(buf, s->data, (size_t)s->len);
        buf[s->len] = 0;
        if (rt_date_parse_iso(buf, s->len, &result)) return result;
    }
    rt_panic("invalid date text (want \"YYYY-MM-DD\" or \"YYYY-MM-DDTHH:MM:SSZ\")");
    return 0;
}
