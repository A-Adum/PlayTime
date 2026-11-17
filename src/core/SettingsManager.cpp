#include "core/SettingsManager.h"

namespace pt {

namespace {
struct Default {
    const wchar_t* key;
    const wchar_t* value;
};
const Default kDefaults[] = {
    {setting::LaunchWithWindows, L"0"}, {setting::StartMinimized, L"0"},  {setting::MinimizeToTray, L"1"},
    {setting::CloseToTray, L"1"},       {setting::TrackingEnabled, L"1"}, {setting::ScanOnStartup, L"1"},
    {setting::PollIntervalSec, L"5"},   {setting::PauseOnLock, L"0"},     {setting::DecimalHours, L"0"},
    {setting::AutoCreateAccounts, L"1"}, {setting::LibrarySort, L"playtime"},
};
}  // namespace

void SettingsManager::load() {
    values_.clear();
    for (auto& d : kDefaults) values_[d.key] = d.value;
    auto& st = db_.prepare("SELECT key, value FROM settings");
    while (st.step()) values_[st.str(0)] = st.str(1);
    st.reset();
}

std::wstring SettingsManager::str(const wchar_t* key) const {
    auto it = values_.find(key);
    return it == values_.end() ? std::wstring() : it->second;
}

int64_t SettingsManager::integer(const wchar_t* key) const {
    auto s = str(key);
    return s.empty() ? 0 : _wtoi64(s.c_str());
}

void SettingsManager::set(const wchar_t* key, const std::wstring& value) {
    auto& st = db_.prepare(
        "INSERT INTO settings(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    st.bind(1, key).bind(2, value).run();
    values_[key] = value;
}

}  // namespace pt
