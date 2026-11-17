#include "util/TimeUtil.h"

#include <cstdio>
#include <ctime>

namespace pt {

namespace {
std::tm localTm(int64_t utc) {
    std::tm tm{};
    __time64_t t = (__time64_t)utc;
    _localtime64_s(&tm, &t);
    return tm;
}
int64_t mk(std::tm tm) {
    tm.tm_isdst = -1;
    return (int64_t)_mktime64(&tm);
}
}  // namespace

int64_t nowUtc() { return (int64_t)_time64(nullptr); }

int64_t localDayStart(int64_t utc) {
    std::tm tm = localTm(utc);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    return mk(tm);
}

int64_t addLocalDays(int64_t dayStart, int days) {
    std::tm tm = localTm(dayStart);
    tm.tm_mday += days;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    return mk(tm);
}

int64_t localWeekStart(int64_t utc) {
    std::tm tm = localTm(utc);
    int mondayBased = (tm.tm_wday + 6) % 7;
    return addLocalDays(localDayStart(utc), -mondayBased);
}

int64_t localMonthStart(int64_t utc) {
    std::tm tm = localTm(utc);
    tm.tm_mday = 1;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    return mk(tm);
}

int64_t addLocalMonths(int64_t monthStart, int months) {
    std::tm tm = localTm(monthStart);
    tm.tm_mday = 1;
    tm.tm_mon += months;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    return mk(tm);
}

LocalParts toLocal(int64_t utc) {
    std::tm tm = localTm(utc);
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (tm.tm_wday + 6) % 7};
}

std::optional<int64_t> fromLocal(int year, int month, int day, int hour, int minute, int second) {
    if (year < 1971 || year > 3000 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59)
        return std::nullopt;
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    int64_t t = mk(tm);
    if (t < 0) return std::nullopt;
    // Reject overflowed dates such as Feb 31.
    auto back = toLocal(t);
    if (back.day != day || back.month != month) return std::nullopt;
    return t;
}

std::wstring formatDate(int64_t utc) {
    auto p = toLocal(utc);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04d-%02d-%02d", p.year, p.month, p.day);
    return buf;
}

std::wstring formatDateTime(int64_t utc) {
    auto p = toLocal(utc);
    wchar_t buf[40];
    swprintf(buf, 40, L"%04d-%02d-%02d %02d:%02d", p.year, p.month, p.day, p.hour, p.minute);
    return buf;
}

std::wstring formatTime(int64_t utc) {
    auto p = toLocal(utc);
    wchar_t buf[16];
    swprintf(buf, 16, L"%02d:%02d", p.hour, p.minute);
    return buf;
}

std::wstring formatRelative(int64_t utc, int64_t now) {
    if (utc <= 0) return L"Never";
    int64_t today = localDayStart(now);
    if (utc >= today) return L"Today " + formatTime(utc);
    if (utc >= addLocalDays(today, -1)) return L"Yesterday " + formatTime(utc);
    for (int d = 2; d < 7; ++d)
        if (utc >= addLocalDays(today, -d)) return std::to_wstring(d) + L" days ago";
    return formatDate(utc);
}

std::wstring monthShortName(int month) {
    static const wchar_t* names[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                     L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    return (month >= 1 && month <= 12) ? names[month - 1] : L"?";
}

std::optional<int64_t> parseLocalDateTime(const std::wstring& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    int n = swscanf(text.c_str(), L" %d-%d-%d %d:%d", &y, &mo, &d, &h, &mi);
    if (n != 3 && n != 5) return std::nullopt;
    return fromLocal(y, mo, d, h, mi);
}

}  // namespace pt
