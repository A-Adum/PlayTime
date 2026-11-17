#pragma once
#include <windows.h>

#include <string>

namespace pt {

class Application;

// Notification-area icon and its context menu.
class TrayManager {
public:
    static constexpr UINT kCallbackMsg = WM_APP + 1;

    bool add(HWND owner, HICON icon, const std::wstring& tip);
    void readd();  // Explorer restarted (TaskbarCreated)
    void remove();
    void setTip(const std::wstring& tip);
    void notify(const std::wstring& title, const std::wstring& text);

    // Handles kCallbackMsg. Returns true if handled.
    bool handleCallback(Application& app, WPARAM wp, LPARAM lp);

private:
    void showMenu(Application& app);

    HWND owner_ = nullptr;
    HICON icon_ = nullptr;
    std::wstring tip_;
    bool added_ = false;
};

}  // namespace pt
