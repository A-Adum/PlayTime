#include "platform/WinUtil.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdint>

namespace pt {

namespace {
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"PlayTime";
}  // namespace

std::wstring appDataDir() {
    // PLAYTIME_DATA_DIR overrides the location (portable installs, testing).
    wchar_t custom[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"PLAYTIME_DATA_DIR", custom, MAX_PATH);
    if (len && len < MAX_PATH) {
        ensureDir(custom);
        return custom;
    }
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &base))) {
        dir = base;
        CoTaskMemFree(base);
    } else {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        dir = (n && n < MAX_PATH) ? buf : L".";
    }
    dir += L"\\PlayTime";
    ensureDir(dir);
    return dir;
}

std::wstring exePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

bool ensureDir(const std::wstring& dir) {
    if (dirExists(dir)) return true;
    return SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) == ERROR_SUCCESS || dirExists(dir);
}

bool fileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::optional<std::string> readFileBytes(const std::wstring& path, size_t maxBytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    std::optional<std::string> out;
    if (GetFileSizeEx(h, &size) && (uint64_t)size.QuadPart <= maxBytes) {
        std::string data((size_t)size.QuadPart, '\0');
        DWORD read = 0;
        size_t off = 0;
        bool ok = true;
        while (off < data.size()) {
            DWORD chunk = (DWORD)std::min<size_t>(data.size() - off, 1u << 24);
            if (!ReadFile(h, data.data() + off, chunk, &read, nullptr) || read == 0) {
                ok = false;
                break;
            }
            off += read;
        }
        if (ok) out = std::move(data);
    }
    CloseHandle(h);
    return out;
}

bool writeFileAtomic(const std::wstring& path, std::string_view bytes) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t off = 0;
    bool ok = true;
    while (off < bytes.size()) {
        DWORD written = 0;
        DWORD chunk = (DWORD)std::min<size_t>(bytes.size() - off, 1u << 24);
        if (!WriteFile(h, bytes.data() + off, chunk, &written, nullptr)) {
            ok = false;
            break;
        }
        off += written;
    }
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::vector<std::wstring> listDir(const std::wstring& dir, const std::wstring& pattern, bool dirs) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\" + pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir != dirs) continue;
        if (isDir && (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)) continue;
        out.emplace_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::optional<std::wstring> regString(HKEY root, const wchar_t* subkey, const wchar_t* value, REGSAM view) {
    DWORD size = 0;
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view, nullptr, nullptr, &size) !=
            ERROR_SUCCESS ||
        size == 0)
        return std::nullopt;
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view, nullptr, s.data(), &size) !=
        ERROR_SUCCESS)
        return std::nullopt;
    s.resize(wcslen(s.c_str()));
    return s;
}

std::optional<DWORD> regDword(HKEY root, const wchar_t* subkey, const wchar_t* value, REGSAM view) {
    DWORD v = 0, size = sizeof v;
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_DWORD | view, nullptr, &v, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return v;
}

std::vector<std::wstring> regSubkeys(HKEY root, const wchar_t* subkey, REGSAM view) {
    std::vector<std::wstring> out;
    HKEY key;
    REGSAM sam = KEY_READ;
    if (view & RRF_SUBKEY_WOW6464KEY) sam |= KEY_WOW64_64KEY;
    if (view & RRF_SUBKEY_WOW6432KEY) sam |= KEY_WOW64_32KEY;
    if (RegOpenKeyExW(root, subkey, 0, sam, &key) != ERROR_SUCCESS) return out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.emplace_back(name, len);
    }
    RegCloseKey(key);
    return out;
}

bool setLaunchAtStartup(bool enable) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    LSTATUS rc;
    if (enable) {
        std::wstring cmd = L"\"" + exePath() + L"\" --tray";
        rc = RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                            (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, kRunValue);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool isLaunchAtStartup() { return regString(HKEY_CURRENT_USER, kRunKey, kRunValue).has_value(); }

std::wstring lastErrorMessage(DWORD err) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"Unknown error";
    LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s;
}

}  // namespace pt
