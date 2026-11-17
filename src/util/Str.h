#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pt {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

std::wstring toLower(std::wstring_view s);
std::wstring trim(std::wstring_view s);
std::vector<std::wstring> split(std::wstring_view s, wchar_t sep);
std::vector<std::wstring> splitLines(std::wstring_view s);  // trims, drops empty lines
std::wstring join(const std::vector<std::wstring>& parts, std::wstring_view sep);
bool startsWith(std::wstring_view s, std::wstring_view prefix);
bool endsWith(std::wstring_view s, std::wstring_view suffix);

// Case-insensitive glob with '*' and '?'. Both inputs should already be lowercase.
bool globMatch(std::wstring_view pattern, std::wstring_view text);

// Lowercase, '/' -> '\', no trailing separator. Used for path-prefix comparisons.
std::wstring normalizePath(std::wstring_view path);
// True if `path` equals `dir` or lies inside it (both normalized).
bool pathInside(std::wstring_view path, std::wstring_view dir);
std::wstring fileNameOf(std::wstring_view path);

// Lowercase alphanumerics only; strips trademark symbols/punctuation. "Rocket League®" -> "rocketleague".
std::wstring normalizeName(std::wstring_view name);

// "1,234"
std::wstring groupThousands(int64_t v);
// Playtime formatting. decimal: "4,250.5 h"; otherwise "4,250h 30m". Under one hour shows minutes.
std::wstring formatHours(int64_t seconds, bool decimal);
// Clock style "1:02:03" for live counters.
std::wstring formatClock(int64_t seconds);
// Short "2h 05m" / "45m" / "30s".
std::wstring formatShortDuration(int64_t seconds);

}  // namespace pt
