#include "util/Str.h"

#include <windows.h>

#include <cwctype>

namespace pt {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring toLower(std::wstring_view s) {
    std::wstring out(s);
    if (!out.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, out.data(), (int)out.size(), out.data(),
                      (int)out.size(), nullptr, nullptr, 0);
    return out;
}

std::wstring trim(std::wstring_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b])) ++b;
    while (e > b && std::iswspace(s[e - 1])) --e;
    return std::wstring(s.substr(b, e - b));
}

std::vector<std::wstring> split(std::wstring_view s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        out.emplace_back(s.substr(start, p == std::wstring_view::npos ? std::wstring_view::npos : p - start));
        if (p == std::wstring_view::npos) break;
        start = p + 1;
    }
    return out;
}

std::vector<std::wstring> splitLines(std::wstring_view s) {
    std::vector<std::wstring> out;
    for (auto& line : split(s, L'\n')) {
        auto t = trim(line);
        if (!t.empty()) out.push_back(std::move(t));
    }
    return out;
}

std::wstring join(const std::vector<std::wstring>& parts, std::wstring_view sep) {
    std::wstring out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

bool startsWith(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool globMatch(std::wstring_view p, std::wstring_view t) {
    size_t pi = 0, ti = 0, star = std::wstring_view::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == L'?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == L'*') {
            star = pi++;
            mark = ti;
        } else if (star != std::wstring_view::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == L'*') ++pi;
    return pi == p.size();
}

std::wstring normalizePath(std::wstring_view path) {
    std::wstring out = toLower(trim(path));
    for (auto& c : out)
        if (c == L'/') c = L'\\';
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}

bool pathInside(std::wstring_view path, std::wstring_view dir) {
    if (dir.empty() || path.size() < dir.size()) return false;
    if (path.compare(0, dir.size(), dir) != 0) return false;
    return path.size() == dir.size() || path[dir.size()] == L'\\' || dir.back() == L'\\';
}

std::wstring fileNameOf(std::wstring_view path) {
    size_t p = path.find_last_of(L"\\/");
    return std::wstring(p == std::wstring_view::npos ? path : path.substr(p + 1));
}

std::wstring normalizeName(std::wstring_view name) {
    std::wstring out;
    for (wchar_t c : toLower(name))
        if (std::iswalnum(c) && c < 0x2000) out += c;  // drops (R), TM, (C) and punctuation
    return out;
}

std::wstring groupThousands(int64_t v) {
    bool neg = v < 0;
    std::wstring digits = std::to_wstring(neg ? -v : v);
    std::wstring out;
    int n = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (n && n % 3 == 0) out.insert(out.begin(), L',');
        out.insert(out.begin(), *it);
        ++n;
    }
    return neg ? L"-" + out : out;
}

std::wstring formatHours(int64_t seconds, bool decimal) {
    bool neg = seconds < 0;
    if (neg) seconds = -seconds;
    std::wstring s;
    if (decimal) {
        int64_t tenths = (seconds + 180) / 360;  // round to 0.1 h
        s = groupThousands(tenths / 10) + L"." + std::to_wstring(tenths % 10) + L" h";
    } else {
        int64_t minutes = seconds / 60;
        int64_t h = minutes / 60, m = minutes % 60;
        if (h == 0) s = std::to_wstring(m) + L"m";
        else s = groupThousands(h) + L"h " + (m < 10 ? L"0" : L"") + std::to_wstring(m) + L"m";
    }
    return neg ? L"-" + s : s;
}

std::wstring formatClock(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    wchar_t buf[48];
    swprintf(buf, 48, L"%lld:%02lld:%02lld", (long long)(seconds / 3600), (long long)(seconds / 60 % 60),
             (long long)(seconds % 60));
    return buf;
}

std::wstring formatShortDuration(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    wchar_t buf[48];
    if (seconds < 60) swprintf(buf, 48, L"%llds", (long long)seconds);
    else if (seconds < 3600) swprintf(buf, 48, L"%lldm", (long long)(seconds / 60));
    else swprintf(buf, 48, L"%lldh %02lldm", (long long)(seconds / 3600), (long long)(seconds / 60 % 60));
    return buf;
}

}  // namespace pt
