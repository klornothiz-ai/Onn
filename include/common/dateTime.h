#pragma once
// ProsperoLayer PS5 emulator - date/time helpers (Kyty-compatible)
#include "common/common.h"
#include <chrono>
#include <cstdint>
#include <string>

namespace Common {

class Date {
public:
        Date() = default;
        Date(int year, int month, int day): m_year(year), m_month(month), m_day(day) {}

        int Year() const { return m_year; }
        int Month() const { return m_month; }
        int Day() const { return m_day; }

        static bool IsValid(int year, int month, int day);
        static bool IsLeapYear(int year);
        static int  DaysInMonth(int month);
        int         DayOfWeek() const;

private:
        int m_year{1970};
        int m_month{1};
        int m_day{1};
};

class Time {
public:
        Time() = default;
        Time(int hour, int minute, int second, int msec = 0)
            : m_hour(hour), m_minute(minute), m_second(second), m_msec(msec) {}

        int Hour24() const { return m_hour; }
        int Minute() const { return m_minute; }
        int Second() const { return m_second; }
        int Msec() const { return m_msec; }

        std::string ToString(const std::string& format) const;

private:
        int m_hour{0};
        int m_minute{0};
        int m_second{0};
        int m_msec{0};
};

class DateTime {
public:
        DateTime() = default;
        DateTime(const Date& date, const Time& time): m_date(date), m_time(time) {}

        static DateTime FromSystem();
        static DateTime FromSystemUTC();

        const Date& GetDate() const { return m_date; }
        const Time& GetTime() const { return m_time; }

        int64_t ToUnixTime() const;
        double ToUnix() const { return static_cast<double>(ToUnixTime()); }
        static DateTime FromUnixTime(int64_t unix_time);

private:
        Date m_date;
        Time m_time;
};

} // namespace Common
