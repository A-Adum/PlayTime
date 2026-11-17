#pragma once
#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace pt {

// %LOCALAPPDATA%\PlayTime, or %PLAYTIME_DATA_DIR% when set (created if missing).
std::wstring appDataDir();
std::wstring exePath();
bool ensureDir(const std::wstring& dir);
bool fileExists(const std::wstring& path);
bool dirExists(const std::wstring& path);
std::optional<std::string> readFileBytes(const std::wstring& path, size_t maxBytes = 64u << 20);
// Writes via a temp file + rename so readers never observe a partial file.
bool writeFileAtomic(const std::wstring& path, std::string_view bytes);
std::vector<std::wstring> listDir(const std::wstring& dir, const std::wstring& pattern, bool dirs);

std::optional<std::wstring> regString(HKEY root, const wchar_t* subkey, const wchar_t* value, REGSAM view = 0);
std::optional<DWORD> regDword(HKEY root, const wchar_t* subkey, const wchar_t* value, REGSAM view = 0);
std::vector<std::wstring> regSubkeys(HKEY root, const wchar_t* subkey, REGSAM view = 0);

// HKCU\...\Run entry.
bool setLaunchAtStartup(bool enable);
bool isLaunchAtStartup();

std::wstring lastErrorMessage(DWORD err = GetLastError());

}  // namespace pt
