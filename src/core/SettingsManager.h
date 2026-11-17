#pragma once
#include <map>
#include <string>

#include "core/Database.h"

namespace pt {

namespace setting {
inline constexpr const wchar_t* LaunchWithWindows = L"launch_with_windows";
inline constexpr const wchar_t* StartMinimized = L"start_minimized";
inline constexpr const wchar_t* MinimizeToTray = L"minimize_to_tray";
inline constexpr const wchar_t* CloseToTray = L"close_to_tray";
inline constexpr const wchar_t* TrackingEnabled = L"tracking_enabled";
inline constexpr const wchar_t* ScanOnStartup = L"scan_on_startup";
inline constexpr const wchar_t* PollIntervalSec = L"poll_interval_sec";
inline constexpr const wchar_t* PauseOnLock = L"pause_on_lock";
inline constexpr const wchar_t* DecimalHours = L"decimal_hours";
inline constexpr const wchar_t* AutoCreateAccounts = L"auto_create_accounts";
inline constexpr const wchar_t* LastBackupUtc = L"last_backup_utc";
inline constexpr const wchar_t* FirstRunDone = L"first_run_done";
inline constexpr const wchar_t* SteamApiKey = L"steam_api_key";
inline constexpr const wchar_t* SteamId64 = L"steam_id64";
inline constexpr const wchar_t* LibrarySort = L"library_sort";
inline constexpr const wchar_t* WindowRect = L"window_rect";
}  // namespace setting

class SettingsManager {
public:
    explicit SettingsManager(Database& db) : db_(db) {}

    void load();  // reads all rows; call after the database is (re)opened
    std::wstring str(const wchar_t* key) const;
    bool flag(const wchar_t* key) const { return str(key) == L"1"; }
    int64_t integer(const wchar_t* key) const;
    void set(const wchar_t* key, const std::wstring& value);  // persists immediately
    void setFlag(const wchar_t* key, bool v) { set(key, v ? L"1" : L"0"); }
    void setInt(const wchar_t* key, int64_t v) { set(key, std::to_wstring(v)); }

    static int clampPollInterval(int64_t sec) { return (int)(sec < 2 ? 2 : sec > 60 ? 60 : sec); }

private:
    Database& db_;
    std::map<std::wstring, std::wstring> values_;
};

}  // namespace pt
