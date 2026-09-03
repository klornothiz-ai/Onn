// ProsperoLayer PS5 emulator - date/time helpers implementation
#include "common/dateTime.h"
#include <cstring>
#include <ctime>

namespace Common {

bool Date::IsValid(int year, int month, int day) {
        if (month < 1 || month > 12) {
                return false;
        }
        if (day < 1 || day > DaysInMonth(month)) {
                return false;
        }
        return year >= 1900 && year <= 9999;
}

bool Date::IsLeapYear(int year) {
        return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int Date::DaysInMonth(int month) {
        static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (month < 1 || month > 12) {
                return 0;
        }
        return days[month - 1];
}

int Date::DayOfWeek() const {
        // Zeller's congruence; returns 0=Sunday .. 6=Saturday.
        int year  = m_year;
        int month = m_month;
        if (month < 3) {
                month += 12;
                year--;
        }
        const int h = (m_day + (13 * (month + 1)) / 5 + year + year / 4 - year / 100 + year / 400) % 7;
        return (h + 6) % 7; // normalize: 0=Sunday
}

std::string Time::ToString(const std::string& format) const {
        char buf[64] = {};
        std::snprintf(buf, sizeof(buf), "%s", format.c_str());
        // Minimal substitution: %H %M %S %f
        std::string out = format;
        auto repl = [&out](const char* pat, const std::string& val) {
                std::string::size_type pos = 0;
                while ((pos = out.find(pat, pos)) != std::string::npos) {
                        out.replace(pos, std::strlen(pat), val);
                        pos += val.size();
                }
        };
        repl("%H", std::to_string(m_hour));
        repl("%M", std::to_string(m_minute));
        repl("%S", std::to_string(m_second));
        repl("%f", std::to_string(m_msec));
        return out;
}

DateTime DateTime::FromSystem() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_local{};
        localtime_r(&t, &tm_local);
        Date d(tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday);
        Time ti(tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
        return DateTime(d, ti);
}

DateTime DateTime::FromSystemUTC() {
        return FromUnixTime(static_cast<int64_t>(std::chrono::system_clock::now().time_since_epoch().count() / 1000000000));
}

int64_t DateTime::ToUnixTime() const {
        // Days from 1970-01-01 via civil algorithm.
        int y = m_date.Year();
        int m = m_date.Month();
        int d = m_date.Day();
        y -= m <= 2;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
        return days * 86400 + m_time.Hour24() * 3600 + m_time.Minute() * 60 + m_time.Second();
}

DateTime DateTime::FromUnixTime(int64_t unix_time) {
        std::time_t t = static_cast<std::time_t>(unix_time);
        std::tm tm_utc{};
        gmtime_r(&t, &tm_utc);
        Date d(tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday);
        Time ti(tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
        return DateTime(d, ti);
}

} // namespace Common
