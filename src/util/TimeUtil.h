#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace pt {

// All persisted timestamps are UTC Unix seconds. Calendar bucketing uses the local time zone.
int64_t nowUtc();

int64_t localDayStart(int64_t utc);                // local midnight at or before utc
int64_t addLocalDays(int64_t dayStart, int days);  // DST-safe (uses calendar arithmetic)
int64_t localWeekStart(int64_t utc);               // Monday 00:00 local
int64_t localMonthStart(int64_t utc);
int64_t addLocalMonths(int64_t monthStart, int months);

struct LocalParts {
    int year, month, day, hour, minute, second, weekday;  // weekday: 0 = Monday
};
LocalParts toLocal(int64_t utc);
std::optional<int64_t> fromLocal(int year, int month, int day, int hour, int minute, int second = 0);

std::wstring formatDate(int64_t utc);        // "2026-11-06"
std::wstring formatDateTime(int64_t utc);    // "2026-11-06 14:05"
std::wstring formatTime(int64_t utc);        // "14:05"
std::wstring formatRelative(int64_t utc, int64_t now);  // "Today 14:05", "Yesterday", "3 days ago", date
std::wstring monthShortName(int month);      // 1..12
// Parses "YYYY-MM-DD HH:MM" (or "YYYY-MM-DD") in local time.
std::optional<int64_t> parseLocalDateTime(const std::wstring& text);

}  // namespace pt
