#include "ui/TrayManager.h"

#include <shellapi.h>

#include <algorithm>
#include <vector>

#include "app/Application.h"
#include "ui/MainWindow.h"
#include "util/Str.h"

namespace pt {

namespace {
enum : UINT {
    kCmdOpen = 1,
    kCmdToggleTracking,
    kCmdExit,
    kCmdShowGame = 2000,   // + index into active sessions
    kCmdStartManual = 3000,  // + index into candidate games
    kCmdStop = 4000,         // + index into active sessions
};

std::wstring truncate(std::wstring s, size_t n) {
    if (s.size() > n) s = s.substr(0, n - 1) + L"\u2026";
    return s;
}
}  // namespace

bool TrayManager::add(HWND owner, HICON icon, const std::wstring& tip) {
    owner_ = owner;
    icon_ = icon;
    tip_ = tip;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = owner;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kCallbackMsg;
    nid.hIcon = icon;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (added_) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
    return added_;
}

void TrayManager::readd() {
    if (owner_) add(owner_, icon_, tip_);
}

void TrayManager::remove() {
    if (!added_) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = owner_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

void TrayManager::setTip(const std::wstring& tip) {
    if (tip == tip_) return;
    tip_ = tip;
    if (!added_) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = owner_;
    nid.uID = 1;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayManager::notify(const std::wstring& title, const std::wstring& text) {
    if (!added_) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = owner_;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

bool TrayManager::handleCallback(Application& app, WPARAM, LPARAM lp) {
    switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            app.showWindow();
            return true;
        case WM_CONTEXTMENU:
            showMenu(app);
            return true;
    }
    return false;
}

void TrayManager::showMenu(Application& app) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdOpen, L"Open PlayTime");
    SetMenuDefaultItem(menu, kCmdOpen, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    auto& active = app.tracker.active();
    std::vector<Id> activeGames;
    for (size_t i = 0; i < active.size(); ++i) {
        auto& s = active[i];
        auto g = app.library.game(s.gameId);
        std::wstring label = (s.running ? L"\u25B6  " : L"\u23F8  ") + truncate(g ? g->name : L"?", 40) + L"  \u2014  " +
                             formatClock(app.tracker.liveSeconds(s));
        if (s.accountId) {
            if (auto a = app.accounts.account(s.accountId)) label += L"  (" + truncate(a->name, 24) + L")";
        } else {
            label += L"  (Unassigned)";
        }
        if (s.manual) label += L"  \u00B7 manual";
        AppendMenuW(menu, MF_STRING, kCmdShowGame + (UINT)i, label.c_str());
        activeGames.push_back(s.gameId);
    }
    if (active.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, app.trackingActive() ? L"No game running" : L"Tracking paused");
    }
    if (active.size() > 1)
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Simultaneous games each accumulate their own time");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, kCmdToggleTracking, app.trackingEnabled() ? L"Pause tracking" : L"Resume tracking");

    // Manual sessions: most recently played games first.
    auto summaries = app.calc.summaries(app.clock.wallUtc());
    std::sort(summaries.begin(), summaries.end(), [](auto& a, auto& b) {
        if (a.lastPlayed != b.lastPlayed) return a.lastPlayed > b.lastPlayed;
        return a.total() > b.total();
    });
    std::vector<Id> candidates;
    HMENU startMenu = CreatePopupMenu();
    for (auto& s : summaries) {
        if (candidates.size() >= 20) break;
        if (app.tracker.find(s.game.id)) continue;
        AppendMenuW(startMenu, MF_STRING, kCmdStartManual + (UINT)candidates.size(), truncate(s.game.name, 48).c_str());
        candidates.push_back(s.game.id);
    }
    UINT startFlags = MF_POPUP | ((candidates.empty() || !app.trackingActive()) ? MF_GRAYED : 0);
    AppendMenuW(menu, startFlags, (UINT_PTR)startMenu, L"Start manual session");

    // Automatic sessions end when the game exits; only manual sessions are stopped by hand.
    std::vector<Id> manual;
    for (auto& s : active)
        if (s.manual) manual.push_back(s.gameId);
    if (manual.size() == 1) {
        AppendMenuW(menu, MF_STRING, kCmdStop, L"Stop manual session");
    } else if (manual.size() > 1) {
        HMENU stopMenu = CreatePopupMenu();
        for (size_t i = 0; i < manual.size(); ++i) {
            auto g = app.library.game(manual[i]);
            AppendMenuW(stopMenu, MF_STRING, kCmdStop + (UINT)i, truncate(g ? g->name : L"?", 48).c_str());
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)stopMenu, L"Stop manual session");
    } else {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Stop manual session");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(owner_);
    UINT cmd = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, owner_, nullptr);
    PostMessageW(owner_, WM_NULL, 0, 0);
    DestroyMenu(menu);  // destroys submenus too

    if (cmd == kCmdOpen) app.showWindow();
    else if (cmd == kCmdToggleTracking) app.setTrackingEnabled(!app.trackingEnabled());
    else if (cmd == kCmdExit) app.requestExit();
    else if (cmd >= kCmdStop && cmd < kCmdStop + manual.size()) app.stopSession(manual[cmd - kCmdStop]);
    else if (cmd >= kCmdStartManual && cmd < kCmdStartManual + candidates.size())
        app.startManualSession(candidates[cmd - kCmdStartManual]);
    else if (cmd >= kCmdShowGame && cmd < kCmdShowGame + activeGames.size()) {
        app.showWindow();
        if (app.window()) app.window()->openGame(activeGames[cmd - kCmdShowGame]);
    }
}

}  // namespace pt
