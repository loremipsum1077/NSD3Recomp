// nn::time: the wall clock and calendar conversions.
#include "nn/nn_types.h"

#include <time.h>

namespace nn { namespace time {

struct PosixTime { int64_t value; };

struct CalendarTime {
    int16_t year;
    int8_t month;
    int8_t day;
    int8_t hour;
    int8_t minute;
    int8_t second;
};

enum DayOfWeek { DayOfWeek_Sunday = 0 };

Result Initialize()
{
    return ResultSuccess();
}

Result Finalize()
{
    return ResultSuccess();
}

DayOfWeek GetDayOfWeek(int year, int month, int day)
{
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = 12;
    time_t when = timegm(&t);
    gmtime_r(&when, &t);
    return static_cast<DayOfWeek>(t.tm_wday);
}

PosixTime ToPosixTimeFromUtc(const CalendarTime & calendar)
{
    struct tm t = {};
    t.tm_year = calendar.year - 1900;
    t.tm_mon = calendar.month - 1;
    t.tm_mday = calendar.day;
    t.tm_hour = calendar.hour;
    t.tm_min = calendar.minute;
    t.tm_sec = calendar.second;
    return PosixTime{static_cast<int64_t>(timegm(&t))};
}

CalendarTime ToCalendarTimeInUtc(const PosixTime & posix)
{
    time_t when = static_cast<time_t>(posix.value);
    struct tm t;
    gmtime_r(&when, &t);
    CalendarTime calendar;
    calendar.year = static_cast<int16_t>(t.tm_year + 1900);
    calendar.month = static_cast<int8_t>(t.tm_mon + 1);
    calendar.day = static_cast<int8_t>(t.tm_mday);
    calendar.hour = static_cast<int8_t>(t.tm_hour);
    calendar.minute = static_cast<int8_t>(t.tm_min);
    calendar.second = static_cast<int8_t>(t.tm_sec);
    return calendar;
}

class StandardUserSystemClock {
public:
    static Result GetCurrentTime(PosixTime * out);
};

Result StandardUserSystemClock::GetCurrentTime(PosixTime * out)
{
    out->value = static_cast<int64_t>(::time(nullptr));
    return ResultSuccess();
}

}}  // namespace nn::time
