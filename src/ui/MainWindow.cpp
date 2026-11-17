#include "ui/MainWindow.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "app/Application.h"
#include "platform/LauncherDiscovery.h"
#include "platform/SteamImport.h"
#include "platform/WinUtil.h"
#include "ui/Dialogs.h"
#include "util/Str.h"
#include "util/TimeUtil.h"

namespace pt {

namespace {
constexpr const wchar_t* kClass = L"PlayTime.Main";
constexpr UINT_PTR kLiveTimer = 1;
constexpr UINT kMsgCloseToTray = WM_APP + 50;
constexpr int kSearchId = 100;

const std::vector<std::pair<std::wstring, std::wstring>> kLaunchers = {
    {L"steam", L"Steam"}, {L"epic", L"Epic Games"}, {L"xbox", L"Xbox"},       {L"gog", L"GOG"},
    {L"ubisoft", L"Ubisoft"}, {L"battlenet", L"Battle.net"}, {L"ea", L"EA app"}, {L"", L"Other"},
};

std::vector<std::wstring> launcherLabels() {
    std::vector<std::wstring> v;
    for (auto& l : kLaunchers) v.push_back(l.second);
    return v;
}

std::wstring fmtHM(int64_t seconds) {
    return std::to_wstring(seconds / 3600) + L"h " + std::to_wstring(seconds / 60 % 60) + L"m";
}
}  // namespace

MainWindow::MainWindow(Application& app) : app_(app) {}

MainWindow::~MainWindow() {
    if (hwnd_) {
        WINDOWPLACEMENT wpl{sizeof wpl};
        if (GetWindowPlacement(hwnd_, &wpl) && app_.db.isOpen()) {
            auto& r = wpl.rcNormalPosition;
            wchar_t buf[64];
            swprintf(buf, 64, L"%ld,%ld,%ld,%ld", r.left, r.top, r.right - r.left, r.bottom - r.top);
            try {
                app_.settings.set(setting::WindowRect, buf);
            } catch (...) {
            }
        }
        HWND h = hwnd_;
        hwnd_ = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }
    if (searchFont_) DeleteObject(searchFont_);
    if (searchBrush_) DeleteObject(searchBrush_);
}

bool MainWindow::create(bool show) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = proc;
        wc.hInstance = app_.hinstance();
        wc.hIcon = app_.appIcon(false);
        wc.hIconSm = app_.appIcon(true);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }
    if (!gfx_.init()) return false;
    sort_ = app_.settings.str(setting::LibrarySort);

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1220, h = 800;
    bool placed = false;
    {
        int rx, ry, rw, rh;
        if (swscanf(app_.settings.str(setting::WindowRect).c_str(), L"%d,%d,%d,%d", &rx, &ry, &rw, &rh) == 4 &&
            rw > 200 && rh > 200) {
            RECT r{rx, ry, rx + rw, ry + rh};
            if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
                x = rx, y = ry, w = rw, h = rh;
                placed = true;
            }
        }
    }
    hwnd_ = CreateWindowExW(0, kClass, L"PlayTime", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr,
                            nullptr, app_.hinstance(), this);
    if (!hwnd_) return false;
    if (!placed) {
        // Scale the default size for the monitor DPI.
        UINT dpi = GetDpiForWindow(hwnd_);
        SetWindowPos(hwnd_, nullptr, 0, 0, MulDiv(1220, dpi, 96), MulDiv(800, dpi, 96),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    reload();
    if (show) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
    }
    updateLiveTimer();
    return true;
}

void MainWindow::bringToFront() {
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    else ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void MainWindow::refresh() {
    reload();
    updateLiveTimer();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::openGame(Id gameId) {
    if (view_ != View::Game) prevView_ = view_;
    view_ = View::Game;
    gameId_ = gameId;
    scroll_ = 0;
    layoutSearch();
    refresh();
}

void MainWindow::navigate(View v) {
    view_ = v;
    scroll_ = 0;
    layoutSearch();
    refresh();
}

bool MainWindow::decimal() const { return app_.settings.flag(setting::DecimalHours); }
std::wstring MainWindow::hours(int64_t seconds) const { return formatHours(seconds, decimal()); }

int64_t MainWindow::liveExtra(Id gameId, Id accountId) const {
    int64_t sum = 0;
    for (auto& s : app_.tracker.active()) {
        if (gameId && s.gameId != gameId) continue;
        if (accountId >= 0 && s.accountId != accountId) continue;
        sum += std::max<int64_t>(0, app_.tracker.liveSeconds(s) - s.savedDuration);
    }
    return sum;
}

void MainWindow::reload() {
    int64_t now = app_.clock.wallUtc();
    try {
        games_ = app_.calc.summaries(now);
        if (view_ == View::Dashboard || view_ == View::Stats) {
            lifetime_ = app_.calc.grandTotal();
            today_ = app_.stats.total(localDayStart(now), addLocalDays(localDayStart(now), 1));
            week_ = app_.stats.total(localWeekStart(now), addLocalDays(localWeekStart(now), 7));
            month_ = app_.stats.total(localMonthStart(now), addLocalMonths(localMonthStart(now), 1));
            heat_ = app_.stats.daily(53 * 7, now);
            allStats_ = app_.stats.sessionStats();
        }
        if (view_ == View::Stats) {
            daily30_ = app_.stats.daily(30, now);
            monthly12_ = app_.stats.monthly(12, now);
            topAccounts_ = app_.stats.topAccounts(10);
        }
        if (view_ == View::Game) {
            auto it = std::find_if(games_.begin(), games_.end(), [&](auto& g) { return g.game.id == gameId_; });
            if (it == games_.end()) {
                view_ = View::Library;
                return;
            }
            gv_.summary = *it;
            gv_.accounts = app_.calc.accountTotals(gameId_);
            gv_.sessions = app_.sessions.forGame(gameId_);
            gv_.stats = app_.stats.sessionStats(gameId_);
            gv_.chart = chartMode_ == 0   ? app_.stats.daily(30, now, gameId_)
                        : chartMode_ == 1 ? app_.stats.weekly(12, now, gameId_)
                                          : app_.stats.monthly(12, now, gameId_);
            gv_.launchers.clear();
            for (auto& r : app_.library.rules(gameId_))
                if (r.kind == RuleKind::InstallDir && !r.launcher.empty() &&
                    std::find(gv_.launchers.begin(), gv_.launchers.end(), r.launcher) == gv_.launchers.end())
                    gv_.launchers.push_back(r.launcher);
        }
    } catch (const std::exception& e) {
        showError(hwnd_, L"Could not load data: " + widen(e.what()));
    }
}

void MainWindow::run(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        showError(hwnd_, widen(e.what()));
        app_.dataChanged();
    }
}

void MainWindow::updateLiveTimer() {
    bool want = hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_) && !app_.tracker.active().empty();
    if (want && !liveTimer_) SetTimer(hwnd_, kLiveTimer, 1000, nullptr);
    else if (!want && liveTimer_) KillTimer(hwnd_, kLiveTimer);
    liveTimer_ = want;
}

void MainWindow::layoutSearch() {
    if (!search_) return;
    bool show = view_ == View::Library;
    if (show) {
        float x = kSidebarW + 28, y = 74;
        SetWindowPos(search_, nullptr, (int)(x * scale_) + 10, (int)(y * scale_) + (int)(7 * scale_),
                     (int)(260 * scale_) - 20, (int)(18 * scale_), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(search_, show ? SW_SHOWNA : SW_HIDE);
}

// ---------------------------------------------------------------- window procedure

LRESULT CALLBACK MainWindow::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self || !self->hwnd_) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->handle(msg, wp, lp);
}

LRESULT MainWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            applyDarkTitleBar(hwnd_);
            COLORREF caption = RGB(0x14, 0x16, 0x19);
            DwmSetWindowAttribute(hwnd_, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof caption);
            float dpi = (float)GetDpiForWindow(hwnd_);
            scale_ = dpi / 96.f;
            gfx_.setDpi(dpi);
            search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd_,
                                      (HMENU)(INT_PTR)kSearchId, app_.hinstance(), nullptr);
            searchFont_ = CreateFontW(-MulDiv(10, (int)dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                      CLEARTYPE_QUALITY, 0, L"Segoe UI");
            searchBrush_ = CreateSolidBrush(RGB(0x22, 0x25, 0x29));
            SendMessageW(search_, WM_SETFONT, (WPARAM)searchFont_, FALSE);
            SendMessageW(search_, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search games");
            return 0;
        }
        case WM_DPICHANGED: {
            float dpi = (float)HIWORD(wp);
            scale_ = dpi / 96.f;
            gfx_.setDpi(dpi);
            if (searchFont_) DeleteObject(searchFont_);
            searchFont_ = CreateFontW(-MulDiv(10, (int)dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                      CLEARTYPE_QUALITY, 0, L"Segoe UI");
            SendMessageW(search_, WM_SETFONT, (WPARAM)searchFont_, TRUE);
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layoutSearch();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {(LONG)(940 * scale_), (LONG)(600 * scale_)};
            return 0;
        }
        case WM_SIZE: {
            if (wp == SIZE_MINIMIZED) {
                updateLiveTimer();
                if (app_.settings.flag(setting::MinimizeToTray)) PostMessageW(hwnd_, kMsgCloseToTray, 0, 0);
                return 0;
            }
            gfx_.resize(LOWORD(lp), HIWORD(lp));
            width_ = LOWORD(lp) / scale_;
            height_ = HIWORD(lp) / scale_;
            layoutSearch();
            updateLiveTimer();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            paint();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            updateHover(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_);
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            updateHover(-1, -1);
            return 0;
        case WM_LBUTTONDOWN:
            if (GetFocus() == search_) SetFocus(hwnd_);
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            float x = GET_X_LPARAM(lp) / scale_, y = GET_Y_LPARAM(lp) / scale_;
            for (int i = (int)hits_.size() - 1; i >= 0; --i) {
                if (!contains(hits_[i].r, x, y)) continue;
                auto fn = msg == WM_LBUTTONUP ? hits_[i].click : hits_[i].rclick;
                if (fn) run(fn);
                break;
            }
            return 0;
        }
        case WM_XBUTTONUP:
            if (view_ == View::Game) navigate(prevView_);
            return TRUE;
        case WM_MOUSEWHEEL:
            scrollBy(-(float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 72.f);
            return 0;
        case WM_KEYDOWN:
            switch (wp) {
                case VK_ESCAPE:
                case VK_BACK:
                    if (view_ == View::Game) navigate(prevView_);
                    return 0;
                case VK_PRIOR: scrollBy(-(height_ - bodyTop_) * 0.9f); return 0;
                case VK_NEXT: scrollBy((height_ - bodyTop_) * 0.9f); return 0;
                case VK_HOME: scrollBy(-1e9f); return 0;
                case VK_END: scrollBy(1e9f); return 0;
                case VK_UP: scrollBy(-48); return 0;
                case VK_DOWN: scrollBy(48); return 0;
                case VK_F5: refresh(); return 0;
                case 'F':
                    if (GetKeyState(VK_CONTROL) < 0) {
                        if (view_ != View::Library) navigate(View::Library);
                        SetFocus(search_);
                    }
                    return 0;
            }
            break;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && hover_ >= 0 && hover_ < (int)hits_.size() && hits_[hover_].click) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_TIMER:
            if (wp == kLiveTimer) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kSearchId && HIWORD(wp) == EN_CHANGE) {
                int n = GetWindowTextLengthW(search_);
                std::wstring s((size_t)n, L'\0');
                GetWindowTextW(search_, s.data(), n + 1);
                filter_ = toLower(trim(s));
                scroll_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(0xE8, 0xEA, 0xED));
            SetBkColor(dc, RGB(0x22, 0x25, 0x29));
            return (LRESULT)searchBrush_;
        }
        case kMsgCloseToTray:
            app_.closeWindow();
            return 0;
        case WM_CLOSE:
            if (app_.settings.flag(setting::CloseToTray)) app_.closeWindow();
            else app_.requestExit();
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void MainWindow::updateHover(float x, float y) {
    mx_ = x;
    my_ = y;
    int h = -1;
    for (int i = (int)hits_.size() - 1; i >= 0; --i)
        if (contains(hits_[i].r, x, y)) {
            h = i;
            break;
        }
    bool tip = h >= 0 && !hits_[h].tip.empty();
    if (h != hover_ || tip) {
        hover_ = h;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::scrollBy(float dy) {
    float maxScroll = std::max(0.f, contentH_ - (height_ - bodyTop_));
    float next = std::clamp(scroll_ + dy, 0.f, maxScroll);
    if (next != scroll_) {
        scroll_ = next;
        InvalidateRect(hwnd_, nullptr, FALSE);
        updateHover(mx_, my_);
    }
}

// ---------------------------------------------------------------- painting infrastructure

void MainWindow::paint() {
    if (!gfx_.begin(hwnd_)) return;
    hits_.clear();
    ty_ = 0;
    gfx_.clear(theme::bg);
    clip_ = rect(0, 0, width_, height_);

    paintSidebar(height_);

    float x = kSidebarW, w = width_ - kSidebarW;
    paintHeader(x, w);

    // Scrollable body
    clip_ = {x, bodyTop_, width_, height_};
    gfx_.pushClip(clip_);
    ty_ = -scroll_;
    gfx_.setTranslation(0, ty_);
    float pad = 28, cw = w - pad * 2;
    float bottom = bodyTop_;
    switch (view_) {
        case View::Dashboard: bottom = paintDashboard(x + pad, bodyTop_ + 4, cw); break;
        case View::Library: bottom = paintLibrary(x + pad, bodyTop_ + 4, cw); break;
        case View::Game: bottom = paintGame(x + pad, bodyTop_ + 4, cw); break;
        case View::Stats: bottom = paintStats(x + pad, bodyTop_ + 4, cw); break;
        case View::Settings: bottom = paintSettings(x + pad, bodyTop_ + 4, cw); break;
    }
    contentH_ = bottom - bodyTop_ + 28;
    gfx_.setTranslation(0, 0);
    ty_ = 0;
    gfx_.popClip();
    clip_ = rect(0, 0, width_, height_);

    // Scrollbar
    float viewH = height_ - bodyTop_;
    if (contentH_ > viewH) {
        float maxScroll = contentH_ - viewH;
        if (scroll_ > maxScroll) scroll_ = maxScroll;
        float thumbH = std::max(40.f, viewH * viewH / contentH_);
        float thumbY = bodyTop_ + (viewH - thumbH) * (scroll_ / maxScroll);
        gfx_.round(rect(width_ - 7, thumbY + 2, 4, thumbH - 4), 2, withAlpha(theme::muted, 0.35f));
    } else {
        scroll_ = 0;
    }
    tooltip();
    gfx_.end();
}

void MainWindow::addHit(const D2D1_RECT_F& r, std::function<void()> click, std::wstring tip,
                        std::function<void()> rclick) {
    D2D1_RECT_F w{r.left, r.top + ty_, r.right, r.bottom + ty_};
    // Only the visible part is clickable.
    w.left = std::max(w.left, clip_.left);
    w.top = std::max(w.top, clip_.top);
    w.right = std::min(w.right, clip_.right);
    w.bottom = std::min(w.bottom, clip_.bottom);
    if (w.right <= w.left || w.bottom <= w.top) return;
    hits_.push_back({w, std::move(click), std::move(rclick), std::move(tip)});
}

bool MainWindow::hovered(const D2D1_RECT_F& r) const {
    float y = my_ - ty_;
    return contains(r, mx_, y) && contains(clip_, mx_, my_);
}

void MainWindow::tooltip() {
    if (hover_ < 0 || hover_ >= (int)hits_.size() || hits_[hover_].tip.empty() || mx_ < 0) return;
    auto& tip = hits_[hover_].tip;
    float w = gfx_.textWidth(tip, Font::Small) + 20, h = 26;
    float x = std::min(mx_ + 14, width_ - w - 6), y = my_ + 20;
    if (y + h > height_ - 4) y = my_ - h - 8;
    gfx_.round(rect(x, y, w, h), 5, theme::raised);
    gfx_.roundStroke(rect(x, y, w, h), 5, theme::border);
    gfx_.text(tip, rect(x + 10, y, w - 20, h), Font::Small, theme::text);
}

float MainWindow::buttonWidth(const std::wstring& label, const wchar_t* icon) const {
    float w = const_cast<Gfx&>(gfx_).textWidth(label, Font::BodyBold) + 28;
    if (icon) w += label.empty() ? 6 : 24;
    return w;
}

float MainWindow::button(float x, float y, const std::wstring& label, std::function<void()> fn, Btn style,
                         const wchar_t* icon, bool enabled) {
    float w = buttonWidth(label, icon), h = 32;
    auto r = rect(x, y, w, h);
    bool hot = enabled && hovered(r);
    Color bg = theme::raised, fg = theme::text;
    switch (style) {
        case Btn::Primary: bg = hot ? rgb(0x6DA8F5) : theme::accent; fg = rgb(0x0E1420); break;
        case Btn::Secondary: bg = hot ? rgb(0x363A41) : theme::raised; break;
        case Btn::Ghost: bg = hot ? theme::raised : withAlpha(theme::raised, 0.f); break;
        case Btn::Danger: bg = hot ? rgb(0x4A2A2D) : theme::raised; fg = theme::danger; break;
    }
    if (!enabled) fg = theme::faint;
    gfx_.round(r, 6, bg);
    float tx = x + 14;
    if (icon) {
        gfx_.text(icon, rect(tx - (label.empty() ? 3 : 0), y, 18, h), Font::IconSmall, fg, Align::Center);
        tx += label.empty() ? 15 : 24;
    }
    if (!label.empty()) gfx_.text(label, rect(tx, y, w - (tx - x) - 10, h), Font::BodyBold, fg);
    if (enabled) addHit(r, std::move(fn));
    return w;
}

float MainWindow::buttonRight(float right, float y, const std::wstring& label, std::function<void()> fn, Btn style,
                              const wchar_t* icon) {
    float w = buttonWidth(label, icon);
    button(right - w, y, label, std::move(fn), style, icon);
    return right - w;
}

void MainWindow::toggle(float x, float y, bool on, std::function<void()> fn) {
    auto r = rect(x, y, 40, 22);
    bool hot = hovered(r);
    gfx_.round(r, 11, on ? (hot ? rgb(0x6DA8F5) : theme::accent) : (hot ? rgb(0x40454D) : rgb(0x383C43)));
    gfx_.circle(on ? x + 29 : x + 11, y + 11, 7.5f, on ? rgb(0x0E1420) : theme::muted);
    addHit(r, std::move(fn));
}

void MainWindow::card(const D2D1_RECT_F& r) {
    gfx_.round(r, 10, theme::card);
    gfx_.roundStroke(r, 10, withAlpha(theme::border, 0.7f));
}

float MainWindow::chip(float x, float y, const std::wstring& label, bool selected, std::function<void()> fn) {
    float w = gfx_.textWidth(label, Font::Small) + 24;
    auto r = rect(x, y, w, 28);
    bool hot = hovered(r);
    gfx_.round(r, 14, selected ? withAlpha(theme::accent, 0.22f) : (hot ? theme::raised : theme::card));
    if (!selected) gfx_.roundStroke(r, 14, theme::border);
    gfx_.text(label, r, selected ? Font::SmallBold : Font::Small, selected ? theme::accent : theme::muted,
              Align::Center);
    addHit(r, std::move(fn));
    return w;
}

void MainWindow::sectionTitle(float x, float y, const std::wstring& title) {
    gfx_.text(title, rect(x, y, 600, 24), Font::Section, theme::text);
}

void MainWindow::statTile(const D2D1_RECT_F& r, const std::wstring& label, const std::wstring& value,
                          const std::wstring& sub) {
    card(r);
    gfx_.text(label, rect(r.left + 18, r.top + 14, r.right - r.left - 36, 18), Font::Small, theme::muted);
    gfx_.text(value, rect(r.left + 18, r.top + 34, r.right - r.left - 36, 30), Font::Title, theme::text);
    if (!sub.empty())
        gfx_.text(sub, rect(r.left + 18, r.top + 66, r.right - r.left - 36, 18), Font::Small, theme::faint);
}

void MainWindow::cover(const std::wstring& path, const std::wstring& name, const D2D1_RECT_F& r, float radius) {
    if (gfx_.image(path, r, radius)) return;
    // Placeholder tile: initials on a muted tone derived from the name.
    unsigned hash = 2166136261u;
    for (wchar_t c : name) hash = (hash ^ (unsigned)c) * 16777619u;
    static const unsigned tones[] = {0x2E3A4F, 0x3A2E4F, 0x2E4F45, 0x4F3A2E, 0x2E474F, 0x4A2E3E};
    gfx_.round(r, radius, rgb(tones[hash % 6]));
    std::wstring initials;
    for (auto& word : split(name, L' '))
        if (!word.empty() && initials.size() < 2 && iswalnum(word[0])) initials += (wchar_t)towupper(word[0]);
    float h = r.bottom - r.top;
    gfx_.text(initials, r, h > 100 ? Font::Display : Font::Section, withAlpha(theme::text, 0.85f), Align::Center);
}

void MainWindow::barChart(const D2D1_RECT_F& r, const std::vector<Bucket>& data, int mode) {
    if (data.empty()) return;
    int64_t maxV = 1;
    for (auto& b : data) maxV = std::max(maxV, b.seconds);
    // Round the scale up to whole hours for readable gridlines.
    double maxH = std::max(1.0, std::ceil(maxV / 3600.0));
    float chartTop = r.top + 8, chartBottom = r.bottom - 22, left = r.left + 40, right = r.right;
    for (int i = 0; i <= 2; ++i) {
        float gy = chartBottom - (chartBottom - chartTop) * i / 2.f;
        gfx_.line(left, gy, right, gy, withAlpha(theme::border, 0.8f));
        wchar_t buf[32];
        double v = maxH * i / 2.0;
        swprintf(buf, 32, v == std::floor(v) ? L"%.0fh" : L"%.1fh", v);
        gfx_.text(buf, rect(r.left, gy - 8, 34, 16), Font::Small, theme::faint, Align::Right);
    }
    float slot = (right - left) / data.size();
    float bw = std::max(2.f, std::min(28.f, slot * 0.66f));
    int labelEvery = mode == 0 ? (slot < 22 ? 5 : 2) : (slot < 34 ? 2 : 1);
    for (size_t i = 0; i < data.size(); ++i) {
        float cx = left + slot * (i + 0.5f);
        float hgt = (float)((chartBottom - chartTop) * (data[i].seconds / 3600.0) / maxH);
        auto bar = rect(cx - bw / 2, chartBottom - hgt, bw, hgt);
        auto hitR = rect(cx - slot / 2, chartTop, slot, chartBottom - chartTop);
        bool hot = hovered(hitR);
        if (data[i].seconds > 0) gfx_.round(bar, std::min(3.f, bw / 3), hot ? rgb(0x7DB4F7) : theme::accent);
        auto p = toLocal(data[i].start);
        std::wstring label = mode == 2 ? monthShortName(p.month)
                                       : std::to_wstring(p.day) + L" " + monthShortName(p.month);
        if (((data.size() - 1 - i) % labelEvery) == 0)
            gfx_.text(mode == 2 ? label : std::to_wstring(p.day) + (mode == 1 ? L" " + monthShortName(p.month) : L""),
                      rect(cx - slot * labelEvery / 2, chartBottom + 4, slot * labelEvery, 16), Font::Small,
                      theme::faint, Align::Center);
        std::wstring tip = (mode == 1 ? L"Week of " : L"") +
                           (mode == 2 ? label + L" " + std::to_wstring(p.year) : formatDate(data[i].start)) + L"  \u00B7  " +
                           (data[i].seconds ? formatShortDuration(data[i].seconds) : L"no play");
        addHit(hitR, nullptr, tip);
    }
}

float MainWindow::heatmap(float x, float y, float w, const std::vector<Bucket>& days) {
    if (days.empty()) return 0;
    const float cell = 12, gap = 3, step = cell + gap;
    int weeksFit = std::max(4, (int)((w - 30) / step));
    // Align columns to Monday-based weeks ending with the current week.
    int lastWeekday = toLocal(days.back().start).weekday;
    int totalCells = std::min((int)days.size(), (weeksFit - 1) * 7 + lastWeekday + 1);
    size_t first = days.size() - (size_t)totalCells;
    int64_t maxV = 1;
    for (size_t i = first; i < days.size(); ++i) maxV = std::max(maxV, days[i].seconds);
    const wchar_t* dayNames[] = {L"Mon", L"", L"Wed", L"", L"Fri", L"", L""};
    for (int d = 0; d < 7; ++d)
        if (*dayNames[d]) gfx_.text(dayNames[d], rect(x, y + 16 + d * step - 2, 28, cell + 4), Font::Small, theme::faint);
    float gx = x + 30;
    int startWeekday = toLocal(days[first].start).weekday;
    int col = 0, lastMonth = -1;
    for (size_t i = first; i < days.size(); ++i) {
        int idx = (int)(i - first) + startWeekday;
        col = idx / 7;
        int row = idx % 7;
        auto p = toLocal(days[i].start);
        if (row == 0 || i == first) {
            if (p.month != lastMonth && (p.day <= 7 || i == first)) {
                gfx_.text(monthShortName(p.month), rect(gx + col * step, y, 40, 14), Font::Small, theme::faint);
                lastMonth = p.month;
            }
        }
        auto r = rect(gx + col * step, y + 16 + row * step, cell, cell);
        int64_t v = days[i].seconds;
        Color c = theme::raised;
        if (v > 0) {
            double f = (double)v / maxV;
            c = withAlpha(theme::accent, f < 0.25 ? 0.3f : f < 0.5 ? 0.5f : f < 0.75 ? 0.75f : 1.f);
        }
        gfx_.round(r, 2.5f, c);
        addHit(r, nullptr, formatDate(days[i].start) + L"  \u00B7  " + (v ? formatShortDuration(v) : L"no play"));
    }
    // Legend
    float ly = y + 16 + 7 * step + 6;
    float lx = gx + (col + 1) * step - 5 * step - 80;
    gfx_.text(L"Less", rect(lx, ly, 36, 14), Font::Small, theme::faint, Align::Right);
    const float alphas[] = {0.f, 0.3f, 0.5f, 0.75f, 1.f};
    for (int i = 0; i < 5; ++i)
        gfx_.round(rect(lx + 42 + i * step, ly + 1, cell, cell), 2.5f,
                   i == 0 ? theme::raised : withAlpha(theme::accent, alphas[i]));
    gfx_.text(L"More", rect(lx + 42 + 5 * step + 4, ly, 40, 14), Font::Small, theme::faint);
    return 16 + 7 * step + 26;
}

// ---------------------------------------------------------------- actions

void MainWindow::chooseCurrentAccount(Id gameId) {
    auto accounts = app_.accounts.accounts(gameId);
    Id current = app_.accounts.preferredAccount(gameId);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (current == 0 ? MF_CHECKED : 0), 1, L"Auto-detect from launcher (default)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < accounts.size(); ++i)
        AppendMenuW(m, MF_STRING | (accounts[i].id == current ? MF_CHECKED : 0), 100 + (UINT)i,
                    accounts[i].name.c_str());
    if (accounts.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No accounts yet - add one first");
    POINT p;
    GetCursorPos(&p);
    UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd == 1) app_.setCurrentAccount(gameId, 0);
    else if (cmd >= 100 && cmd < 100 + accounts.size()) app_.setCurrentAccount(gameId, accounts[cmd - 100].id);
}

void MainWindow::adjustHours(Id gameId, Id accountId) {
    auto game = app_.library.game(gameId);
    if (!game) return;
    auto accounts = app_.accounts.accounts(gameId);
    std::vector<Id> ids{kUnassigned};
    std::vector<std::wstring> names{L"Unassigned"};
    int sel = 0;
    for (auto& a : accounts) {
        if (a.id == accountId) sel = (int)ids.size();
        ids.push_back(a.id);
        names.push_back(a.name);
    }
    // Prefer a real account when invoked for "Unassigned" on a game that has exactly one account.
    if (accountId == kUnassigned && accounts.size() == 1 && app_.calc.baseline(gameId, kUnassigned) == 0) sel = 1;
    std::wstring summary;
    for (size_t i = 0; i < ids.size(); ++i) {
        int64_t base = app_.calc.baseline(gameId, ids[i]);
        int64_t tracked = app_.calc.tracked(gameId, ids[i]) + liveExtra(gameId, ids[i]);
        if (i > 0 || base || tracked)
            summary += names[i] + L":  historical " + fmtHM(base) + L"  \u00B7  tracked " + fmtHM(tracked) + L"\n";
    }
    Id initial = ids[(size_t)sel];
    int64_t initBase = app_.calc.baseline(gameId, initial);
    Form f(L"Adjust hours \u2014 " + game->name);
    if (!summary.empty()) f.label(summary.substr(0, summary.size() - 1));
    int acc = f.combo(L"Account", names, sel);
    int mode = f.combo(L"Action",
                       {L"Set historical (pre-tracking) time", L"Set lifetime total (historical = total - tracked)",
                        L"Add time", L"Subtract time"},
                       0);
    int hm = f.editPair(L"Time", std::to_wstring(initBase / 3600), L"hours", std::to_wstring(initBase / 60 % 60),
                        L"minutes");
    f.label(L"Historical time counts toward lifetime totals only. It is never spread over days, so daily and weekly "
            L"statistics stay accurate.");
    f.validate = [&](Form& form) -> std::wstring {
        if (!parseHoursMinutes(form.text(hm), form.text(hm + 1))) return L"Enter hours (e.g. 3500 or 12.5) and minutes.";
        return {};
    };
    if (!f.show(hwnd_)) return;
    Id target = ids[(size_t)std::max(0, f.selection(acc))];
    int64_t secs = *parseHoursMinutes(f.text(hm), f.text(hm + 1));
    switch (f.selection(mode)) {
        case 0: app_.calc.setBaseline(gameId, target, secs); break;
        case 1:
            app_.tracker.checkpoint();  // include the running session's latest time in "tracked"
            app_.calc.setLifetimeTotal(gameId, target, secs);
            break;
        case 2: app_.calc.adjustBaseline(gameId, target, secs); break;
        case 3: app_.calc.adjustBaseline(gameId, target, -secs); break;
    }
    app_.dataChanged();
}

void MainWindow::addAccount(Id gameId) {
    Form f(L"Add account");
    int name = f.edit(L"Account name", L"", 1, L"e.g. Epic Main");
    int launcher = f.combo(L"Launcher", launcherLabels(), 1);
    int hm = f.editPair(L"Existing playtime on this account (optional)", L"", L"hours", L"", L"minutes");
    f.setOkLabel(L"Add");
    f.validate = [&](Form& form) -> std::wstring {
        if (trim(form.text(name)).empty()) return L"Enter an account name.";
        if (!parseHoursMinutes(form.text(hm), form.text(hm + 1))) return L"Enter valid hours and minutes.";
        return {};
    };
    if (!f.show(hwnd_)) return;
    Id id = app_.accounts.addAccount(gameId, f.text(name), kLaunchers[(size_t)f.selection(launcher)].first);
    int64_t secs = *parseHoursMinutes(f.text(hm), f.text(hm + 1));
    if (secs > 0) app_.calc.setBaseline(gameId, id, secs);
    app_.dataChanged();
}

void MainWindow::showAccountMenu(const AccountTotals& a) {
    Id gameId = gameId_;
    auto accounts = app_.accounts.accounts(gameId);
    std::vector<Account> others;
    for (auto& o : accounts)
        if (o.id != a.accountId) others.push_back(o);
    bool unassigned = a.accountId == kUnassigned;

    enum : UINT { kRename = 1, kAdjust, kUse, kReset, kDelete, kLauncher, kMerge = 100, kAssign = 300, kSetLauncher = 500 };
    HMENU m = CreatePopupMenu();
    if (!unassigned) {
        AppendMenuW(m, MF_STRING, kRename, L"Rename\u2026");
        HMENU lm = CreatePopupMenu();
        for (size_t i = 0; i < kLaunchers.size(); ++i)
            AppendMenuW(lm, MF_STRING | (kLaunchers[i].first == a.launcher ? MF_CHECKED : 0), kSetLauncher + (UINT)i,
                        kLaunchers[i].second.c_str());
        AppendMenuW(m, MF_POPUP, (UINT_PTR)lm, L"Launcher");
    }
    AppendMenuW(m, MF_STRING, kAdjust, L"Adjust hours\u2026");
    if (!unassigned) AppendMenuW(m, MF_STRING, kUse, L"Use for current && next sessions");
    HMENU mm = CreatePopupMenu();
    for (size_t i = 0; i < others.size(); ++i) AppendMenuW(mm, MF_STRING, kMerge + (UINT)i, others[i].name.c_str());
    if (!unassigned)
        AppendMenuW(m, MF_POPUP | (others.empty() ? MF_GRAYED : 0), (UINT_PTR)mm, L"Merge into");
    else
        AppendMenuW(m, MF_POPUP | (others.empty() ? MF_GRAYED : 0), (UINT_PTR)mm, L"Assign all sessions to");
    if (!unassigned) {
        auto ids = app_.accounts.identifiers(a.accountId);
        if (!ids.empty()) {
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            for (auto& id : ids) {
                std::wstring s = L"Linked: " + AccountManager::launcherDisplayName(id.launcher) + L" " +
                                 (id.displayName.empty() ? id.externalId : id.displayName + L" (" + id.externalId + L")");
                AppendMenuW(m, MF_STRING | MF_GRAYED, 0, s.c_str());
            }
        }
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kReset, L"Reset hours\u2026");
    if (!unassigned) AppendMenuW(m, MF_STRING, kDelete, L"Delete account\u2026");
    POINT p;
    GetCursorPos(&p);
    UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, hwnd_, nullptr);
    DestroyMenu(m);
    if (!cmd) return;

    if (cmd == kRename) {
        Form f(L"Rename account");
        int n = f.edit(L"Account name", a.name);
        f.validate = [&](Form& form) { return trim(form.text(n)).empty() ? std::wstring(L"Enter a name.") : std::wstring(); };
        if (f.show(hwnd_)) app_.accounts.renameAccount(a.accountId, f.text(n));
    } else if (cmd >= kSetLauncher && cmd < kSetLauncher + kLaunchers.size()) {
        app_.accounts.setLauncher(a.accountId, kLaunchers[cmd - kSetLauncher].first);
    } else if (cmd == kAdjust) {
        adjustHours(gameId, a.accountId);
        return;
    } else if (cmd == kUse) {
        app_.setCurrentAccount(gameId, a.accountId);
        return;
    } else if (cmd >= kMerge && cmd < kMerge + others.size()) {
        auto& target = others[cmd - kMerge];
        if (unassigned) {
            int n = app_.accounts.assignUnassigned(gameId, target.id);
            // Keep the running session (if unassigned) consistent with the database.
            if (auto* s = app_.tracker.find(gameId); s && s->accountId == kUnassigned)
                app_.tracker.setSessionAccount(gameId, target.id);
            showInfo(hwnd_, L"Sessions assigned", std::to_wstring(n) + L" session(s) moved to " + target.name + L".");
        } else {
            if (!confirm(hwnd_, L"Merge accounts",
                         L"Merge \"" + a.name + L"\" into \"" + target.name +
                             L"\"?\n\nSessions, historical hours and launcher links move to \"" + target.name +
                             L"\" and \"" + a.name + L"\" is removed. Overlapping sessions are combined so time is "
                             L"counted once."))
                return;
            bool running = app_.tracker.find(gameId) && app_.tracker.find(gameId)->accountId == a.accountId;
            app_.accounts.mergeAccounts(a.accountId, target.id);
            if (running) app_.tracker.setSessionAccount(gameId, target.id);
        }
    } else if (cmd == kReset) {
        if (!confirm(hwnd_, L"Reset hours",
                     L"Delete all recorded sessions and historical time for \"" + a.name + L"\" (" + hours(a.total()) +
                         L")?\n\nThis cannot be undone (automatic backups are kept in the data folder)."))
            return;
        app_.calc.resetAccount(gameId, a.accountId);
    } else if (cmd == kDelete) {
        int r = MessageBoxW(hwnd_,
                            (L"Delete account \"" + a.name + L"\"?\n\nYes: keep its " + hours(a.total()) +
                             L" as Unassigned time of this game.\nNo: delete its sessions and hours too.")
                                .c_str(),
                            L"Delete account", MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON1);
        if (r == IDCANCEL) return;
        bool running = app_.tracker.find(gameId) && app_.tracker.find(gameId)->accountId == a.accountId;
        if (running) app_.tracker.setSessionAccount(gameId, kUnassigned);
        app_.accounts.deleteAccount(a.accountId, r == IDYES);
    }
    app_.dataChanged();
}

void MainWindow::showSessionMenu(Id sessionId) {
    auto s = app_.sessions.get(sessionId);
    if (!s) return;
    const ActiveSession* live = nullptr;
    for (auto& a : app_.tracker.active())
        if (a.sessionId == sessionId) live = &a;
    auto accounts = app_.accounts.accounts(s->gameId);
    enum : UINT { kEdit = 1, kDelete, kStop, kMove = 100 };
    HMENU m = CreatePopupMenu();
    if (live) AppendMenuW(m, MF_STRING, kStop, L"Stop session");
    AppendMenuW(m, MF_STRING | (live ? MF_GRAYED : 0), kEdit, L"Edit\u2026");
    HMENU mv = CreatePopupMenu();
    AppendMenuW(mv, MF_STRING | (s->accountId == kUnassigned ? MF_CHECKED : 0), kMove, L"Unassigned");
    for (size_t i = 0; i < accounts.size(); ++i)
        AppendMenuW(mv, MF_STRING | (accounts[i].id == s->accountId ? MF_CHECKED : 0), kMove + 1 + (UINT)i,
                    accounts[i].name.c_str());
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mv, L"Move to account");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (live ? MF_GRAYED : 0), kDelete, L"Delete\u2026");
    POINT p;
    GetCursorPos(&p);
    UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, hwnd_, nullptr);
    DestroyMenu(m);
    if (!cmd) return;
    if (cmd == kStop) {
        app_.stopSession(s->gameId);
        return;
    }
    if (cmd == kEdit) {
        editSession(sessionId);
        return;
    }
    if (cmd == kDelete) {
        if (!confirm(hwnd_, L"Delete session",
                     L"Delete the session from " + formatDateTime(s->startUtc) + L" (" + formatShortDuration(s->duration) +
                         L")?"))
            return;
        app_.sessions.remove(sessionId);
    } else if (cmd >= kMove && cmd <= kMove + accounts.size()) {
        Id target = cmd == kMove ? kUnassigned : accounts[cmd - kMove - 1].id;
        if (live) app_.tracker.setSessionAccount(s->gameId, target);
        else app_.sessions.setAccount(sessionId, target);
    }
    app_.dataChanged();
}

namespace {
struct SessionForm {
    int account, start, hm;
};
SessionForm buildSessionForm(Form& f, const std::vector<Account>& accounts, Id accountId, int64_t start,
                             int64_t duration) {
    std::vector<std::wstring> names{L"Unassigned"};
    int sel = 0;
    for (size_t i = 0; i < accounts.size(); ++i) {
        names.push_back(accounts[i].name);
        if (accounts[i].id == accountId) sel = (int)i + 1;
    }
    SessionForm sf;
    sf.account = f.combo(L"Account", names, sel);
    sf.start = f.edit(L"Start (local time, YYYY-MM-DD HH:MM)", formatDateTime(start));
    sf.hm = f.editPair(L"Duration", std::to_wstring(duration / 3600), L"hours", std::to_wstring(duration / 60 % 60),
                       L"minutes");
    f.validate = [sf](Form& form) -> std::wstring {
        auto t = parseLocalDateTime(form.text(sf.start));
        if (!t) return L"Enter the start as YYYY-MM-DD HH:MM.";
        auto d = parseHoursMinutes(form.text(sf.hm), form.text(sf.hm + 1));
        if (!d || *d <= 0) return L"Enter a duration greater than zero.";
        if (*d > 7 * 24 * 3600) return L"A single session cannot exceed 7 days.";
        if (*t + *d > nowUtc() + 300) return L"The session cannot end in the future.";
        return {};
    };
    return sf;
}
}  // namespace

void MainWindow::addSession(Id gameId) {
    auto accounts = app_.accounts.accounts(gameId);
    Form f(L"Add dated session");
    f.label(L"Record time you actually played on a specific date. It counts in daily/weekly statistics. "
            L"For undated historical time use Adjust hours instead.");
    int64_t now = nowUtc();
    auto sf = buildSessionForm(f, accounts, app_.accounts.preferredAccount(gameId), now - 3600 - now % 60, 3600);
    f.setOkLabel(L"Add");
    if (!f.show(hwnd_)) return;
    int sel = f.selection(sf.account);
    Id acc = sel <= 0 ? kUnassigned : accounts[(size_t)sel - 1].id;
    app_.sessions.addManual(gameId, acc, *parseLocalDateTime(f.text(sf.start)),
                            *parseHoursMinutes(f.text(sf.hm), f.text(sf.hm + 1)));
    app_.dataChanged();
}

void MainWindow::editSession(Id sessionId) {
    auto s = app_.sessions.get(sessionId);
    if (!s) return;
    auto accounts = app_.accounts.accounts(s->gameId);
    Form f(L"Edit session");
    if (!s->exeName.empty()) f.label(L"Detected executable: " + s->exeName);
    auto sf = buildSessionForm(f, accounts, s->accountId, s->startUtc, s->duration);
    if (!f.show(hwnd_)) return;
    int sel = f.selection(sf.account);
    Id acc = sel <= 0 ? kUnassigned : accounts[(size_t)sel - 1].id;
    app_.sessions.edit(sessionId, acc, *parseLocalDateTime(f.text(sf.start)),
                       *parseHoursMinutes(f.text(sf.hm), f.text(sf.hm + 1)));
    app_.dataChanged();
}

void MainWindow::editGame(Id gameId) {
    auto g = app_.library.game(gameId);
    if (!g) return;
    std::vector<std::wstring> exes, dirs;
    for (auto& e : app_.library.executables(gameId)) exes.push_back(e.fullPath.empty() ? e.exeName : e.fullPath);
    for (auto& r : app_.library.rules(gameId))
        if (r.kind == RuleKind::InstallDir) dirs.push_back(r.pattern);
    Form f(L"Edit game", 560);
    int name = f.edit(L"Name", g->name);
    int coverId = f.edit(L"Cover image (optional)", g->coverPath, 1, L"PNG or JPEG file");
    f.browse(coverId, [](HWND owner) {
        return openFileDialog(owner, L"Choose cover image", L"Images (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0");
    });
    int aliases = f.edit(L"Aliases (one per line; used to match launcher entries)", join(app_.library.aliases(gameId), L"\r\n"), 2);
    int exeId = f.edit(L"Executables (one per line: file name like game.exe, or a full path)", join(exes, L"\r\n"), 3);
    f.browse(exeId, [&f, exeId](HWND owner) -> std::optional<std::wstring> {
        auto p = openFileDialog(owner, L"Choose game executable", L"Programs (*.exe)\0*.exe\0");
        if (!p) return std::nullopt;
        auto cur = trim(f.text(exeId));
        return cur.empty() ? *p : cur + L"\r\n" + *p;
    });
    int dirId = f.edit(L"Install folders (any program inside counts, except exclusions)", join(dirs, L"\r\n"), 3);
    f.browse(dirId, [&f, dirId](HWND owner) -> std::optional<std::wstring> {
        auto p = pickFolderDialog(owner, L"Choose install folder");
        if (!p) return std::nullopt;
        auto cur = trim(f.text(dirId));
        return cur.empty() ? *p : cur + L"\r\n" + *p;
    });
    int exclId = f.edit(L"Excluded helper programs for this game (patterns like *launcher*.exe)",
                        join(app_.library.exclusions(gameId), L"\r\n"), 2);
    f.validate = [&](Form& form) -> std::wstring {
        if (trim(form.text(name)).empty()) return L"Enter a name.";
        for (auto& d : splitLines(form.text(dirId)))
            if (normalizePath(d).size() < 4) return L"Install folder \"" + d + L"\" is too broad.";
        return {};
    };
    if (!f.show(hwnd_)) return;
    {
        Database::Tx tx(app_.db);
        app_.library.renameGame(gameId, f.text(name));
        app_.library.setCover(gameId, f.text(coverId));
        app_.library.setAliases(gameId, splitLines(f.text(aliases)));
        app_.library.setExecutables(gameId, splitLines(f.text(exeId)));
        app_.library.setInstallDirs(gameId, splitLines(f.text(dirId)));
        app_.library.setExclusions(gameId, splitLines(f.text(exclId)));
        tx.commit();
    }
    app_.detectionRulesChanged();
    app_.dataChanged();
}

void MainWindow::mergeGame(Id gameId) {
    auto g = app_.library.game(gameId);
    if (!g) return;
    auto all = app_.library.games();
    std::vector<Game> targets;
    for (auto& o : all)
        if (o.id != gameId) targets.push_back(o);
    if (targets.empty()) {
        showInfo(hwnd_, L"Merge", L"There is no other game to merge into.");
        return;
    }
    std::vector<std::wstring> names;
    for (auto& t : targets) names.push_back(t.name);
    Form f(L"Merge game");
    f.label(L"Merge \"" + g->name + L"\" into another entry, e.g. the Epic and Steam copies of the same game. "
            L"Accounts, sessions, historical hours, executables and folders move to the selected game; overlapping "
            L"sessions are combined so time is counted once.");
    int sel = f.combo(L"Merge into", names, 0);
    f.setOkLabel(L"Merge");
    if (!f.show(hwnd_)) return;
    Id target = targets[(size_t)f.selection(sel)].id;
    app_.libraryStructureChanging();
    try {
        app_.library.mergeGames(gameId, target);
    } catch (...) {
        app_.libraryStructureChanged();
        throw;
    }
    app_.libraryStructureChanged();
    openGame(target);
}

void MainWindow::deleteGame(Id gameId) {
    auto it = std::find_if(games_.begin(), games_.end(), [&](auto& g) { return g.game.id == gameId; });
    if (it == games_.end()) return;
    if (!confirm(hwnd_, L"Remove game",
                 L"Remove \"" + it->game.name + L"\" and all of its playtime (" + hours(it->total() + liveExtra(gameId)) +
                     L", " + std::to_wstring(it->sessionCount) +
                     L" sessions, all accounts)?\n\nUninstalling a game from your PC never requires this - PlayTime "
                     L"keeps its hours. Launcher scans will not re-add a removed game."))
        return;
    app_.libraryStructureChanging();
    try {
        app_.library.deleteGame(gameId);
    } catch (...) {
        app_.libraryStructureChanged();
        throw;
    }
    app_.libraryStructureChanged();
    if (view_ == View::Game && gameId_ == gameId) navigate(prevView_);
}

void MainWindow::showGameMenu(Id gameId) {
    auto it = std::find_if(games_.begin(), games_.end(), [&](auto& g) { return g.game.id == gameId; });
    if (it == games_.end()) return;
    bool active = app_.tracker.find(gameId) != nullptr;
    enum : UINT { kOpen = 1, kStart, kStop, kAdjust, kEdit, kMerge, kRemove, kAccount };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kOpen, L"Open details");
    if (active) AppendMenuW(m, MF_STRING, kStop, L"Stop session");
    else AppendMenuW(m, MF_STRING | (app_.trackingActive() ? 0 : MF_GRAYED), kStart, L"Start manual session");
    AppendMenuW(m, MF_STRING, kAccount, L"Current account\u2026");
    AppendMenuW(m, MF_STRING, kAdjust, L"Adjust unassigned hours\u2026");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kEdit, L"Edit game && detection\u2026");
    AppendMenuW(m, MF_STRING, kMerge, L"Merge into\u2026");
    AppendMenuW(m, MF_STRING, kRemove, L"Remove\u2026");
    POINT p;
    GetCursorPos(&p);
    UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, hwnd_, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case kOpen: openGame(gameId); break;
        case kStart: app_.startManualSession(gameId); break;
        case kStop: app_.stopSession(gameId); break;
        case kAccount: chooseCurrentAccount(gameId); break;
        case kAdjust: adjustHours(gameId, kUnassigned); break;
        case kEdit: editGame(gameId); break;
        case kMerge: mergeGame(gameId); break;
        case kRemove: deleteGame(gameId); break;
    }
}

void MainWindow::addGameFromExe() {
    auto path = openFileDialog(hwnd_, L"Choose the game's executable", L"Programs (*.exe)\0*.exe\0");
    if (!path) return;
    std::wstring file = fileNameOf(*path);
    std::wstring stem = file.substr(0, file.find_last_of(L'.'));
    // Default name: the folder name usually reads better than the exe name.
    std::wstring folder = path->substr(0, path->find_last_of(L"\\/"));
    std::wstring folderName = fileNameOf(folder);
    std::wstring lower = toLower(folderName);
    std::wstring suggestion = (lower == L"bin" || lower == L"win64" || lower == L"x64" || lower == L"binaries" ||
                               lower.empty()) ? stem : folderName;
    Form f(L"Add game");
    f.label(L"Executable: " + *path);
    int name = f.edit(L"Game name", suggestion);
    int hm = f.editPair(L"Existing playtime (optional, goes to Unassigned)", L"", L"hours", L"", L"minutes");
    f.setOkLabel(L"Add");
    f.validate = [&](Form& form) -> std::wstring {
        if (trim(form.text(name)).empty()) return L"Enter a name.";
        if (!parseHoursMinutes(form.text(hm), form.text(hm + 1))) return L"Enter valid hours and minutes.";
        return {};
    };
    if (!f.show(hwnd_)) return;
    Id id;
    if (auto existing = app_.library.findByName(f.text(name));
        existing && confirm(hwnd_, L"Game exists", L"A game named like \"" + f.text(name) +
                                                       L"\" already exists. Add this executable to it instead?")) {
        id = *existing;
    } else {
        id = app_.library.addGame(f.text(name));
    }
    app_.library.addExecutable(id, *path);
    int64_t secs = *parseHoursMinutes(f.text(hm), f.text(hm + 1));
    if (secs > 0) app_.calc.adjustBaseline(id, kUnassigned, secs);
    app_.detectionRulesChanged();
    openGame(id);
}

void MainWindow::addCustomGame() {
    Form f(L"Add custom game");
    f.label(L"A game without an executable: track it with manual sessions or historical hours. You can add "
            L"executables or folders later under Edit.");
    int name = f.edit(L"Game name", L"");
    int hm = f.editPair(L"Existing playtime (optional, goes to Unassigned)", L"", L"hours", L"", L"minutes");
    f.setOkLabel(L"Add");
    f.validate = [&](Form& form) -> std::wstring {
        if (trim(form.text(name)).empty()) return L"Enter a name.";
        if (app_.library.findByName(form.text(name))) return L"A game with this name already exists.";
        if (!parseHoursMinutes(form.text(hm), form.text(hm + 1))) return L"Enter valid hours and minutes.";
        return {};
    };
    if (!f.show(hwnd_)) return;
    Id id = app_.library.addGame(f.text(name));
    int64_t secs = *parseHoursMinutes(f.text(hm), f.text(hm + 1));
    if (secs > 0) app_.calc.setBaseline(id, kUnassigned, secs);
    app_.dataChanged();
    openGame(id);
}

void MainWindow::scanLaunchers() {
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    auto r = app_.runDiscovery();
    SetCursor(old);
    std::wstring msg = std::to_wstring(r.added) + L" new game(s) added, " + std::to_wstring(r.linked) +
                       L" install(s) linked to existing games, " + std::to_wstring(r.updated) + L" moved install(s) updated.";
    if (!r.addedNames.empty()) {
        msg += L"\n\nAdded:\n";
        for (size_t i = 0; i < r.addedNames.size() && i < 25; ++i) msg += L"  \u2022 " + r.addedNames[i] + L"\n";
        if (r.addedNames.size() > 25) msg += L"  \u2026 and " + std::to_wstring(r.addedNames.size() - 25) + L" more\n";
    }
    showInfo(hwnd_, L"Installed games", msg);
}

namespace {
std::wstring importSummary(const std::vector<SteamImportLine>& lines, bool decimal) {
    if (lines.empty()) return L"No matching games with Steam playtime were found in your library.";
    std::wstring msg;
    size_t shown = 0;
    for (auto& l : lines) {
        if (shown++ >= 20) {
            msg += L"\u2026 and " + std::to_wstring(lines.size() - 20) + L" more\n";
            break;
        }
        msg += l.gameName + L" \u00B7 " + l.accountName + L": " + formatHours(l.result.importedTotal, decimal) +
               L" on Steam (historical " + formatHours(l.result.newBaseline, decimal) + L" + tracked " +
               formatHours(l.result.tracked, decimal) + L")" + (l.result.clamped ? L" \u2014 tracked exceeds Steam's total" : L"") + L"\n";
    }
    return msg;
}
}  // namespace

void MainWindow::importSteamLocal() {
    auto root = steamRoot();
    if (root.empty()) {
        showError(hwnd_, L"Steam does not appear to be installed for this Windows user.");
        return;
    }
    auto users = steamUsers(root);
    std::wstring preview;
    std::vector<std::pair<SteamUser, std::map<std::wstring, int64_t>>> data;
    std::map<std::wstring, Id> linked;
    for (auto& [app, game] : app_.library.launcherApps(L"steam")) linked[app] = game;
    for (auto& u : users) {
        auto pt = steamLocalPlaytime(root, u.accountId);
        int matches = 0;
        for (auto& [app, secs] : pt)
            if (secs > 0 && linked.count(app)) ++matches;
        preview += L"  \u2022 " + (u.persona.empty() ? std::to_wstring(u.accountId) : u.persona) + L": " +
                   std::to_wstring(matches) + L" game(s) in your library\n";
        if (matches) data.emplace_back(u, std::move(pt));
    }
    if (data.empty()) {
        showInfo(hwnd_, L"Steam import",
                 L"No Steam playtime for games in your library was found in Steam's local data.\n\nRun \"Scan for "
                 L"installed games\" first, or try the Steam Web API import.");
        return;
    }
    if (!confirm(hwnd_, L"Import Steam playtime",
                 L"Steam keeps each signed-in user's lifetime playtime locally. Import it for:\n\n" + preview +
                     L"\nEach Steam user becomes (or updates) an account under the game. The Steam total already "
                     L"includes time PlayTime tracked on that account, so historical time is set to the difference - "
                     L"re-importing never double counts."))
        return;
    std::vector<SteamImportLine> all;
    for (auto& [user, pt] : data) {
        auto lines = applySteamImport(app_.library, app_.accounts, app_.calc, user, pt);
        all.insert(all.end(), lines.begin(), lines.end());
    }
    app_.dataChanged();
    showInfo(hwnd_, L"Steam import complete", importSummary(all, decimal()));
}

void MainWindow::importSteamWeb() {
    Form f(L"Steam Web API import", 520);
    f.label(L"Uses Valve's official GetOwnedGames API with your own key (steamcommunity.com/dev/apikey). Your "
            L"profile's game details must be visible to the key's owner. Nothing else is sent anywhere.");
    int key = f.edit(L"Web API key", app_.settings.str(setting::SteamApiKey), 1, L"32 hexadecimal characters");
    int sid = f.edit(L"SteamID64", app_.settings.str(setting::SteamId64), 1, L"7656119...");
    int remember = f.check(L"Remember the key on this PC", !app_.settings.str(setting::SteamApiKey).empty());
    f.setOkLabel(L"Import");
    if (!f.show(hwnd_)) return;
    std::wstring k = trim(f.text(key)), id = trim(f.text(sid));
    app_.settings.set(setting::SteamApiKey, f.checked(remember) ? k : L"");
    app_.settings.set(setting::SteamId64, id);
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    auto res = steamWebApiOwnedGames(k, id);
    SetCursor(old);
    if (!res.ok) {
        showError(hwnd_, L"Steam Web API: " + res.error);
        return;
    }
    SteamUser user;
    user.steamId64 = _wcstoui64(id.c_str(), nullptr, 10);
    user.accountId = (uint32_t)(user.steamId64 - kSteamId64Base);
    if (auto root = steamRoot(); !root.empty())
        for (auto& u : steamUsers(root))
            if (u.accountId == user.accountId) user.persona = u.persona;
    std::map<std::wstring, int64_t> secs;
    std::map<std::wstring, std::wstring> names;
    for (auto& [app, g] : res.games) {
        secs[app] = g.seconds;
        names[app] = g.name;
    }
    auto lines = applySteamImport(app_.library, app_.accounts, app_.calc, user, secs, names);
    app_.dataChanged();
    showInfo(hwnd_, L"Steam import complete", importSummary(lines, decimal()));
}

void MainWindow::editGlobalExclusions() {
    Form f(L"Detection exclusions", 520);
    f.label(L"Programs inside a game's install folder that match these patterns are never counted as the game "
            L"(launchers, updaters, crash reporters, redistributables). One pattern per line; * and ? wildcards; a "
            L"pattern containing \\ matches the full path. Executables listed explicitly on a game always count.");
    int ex = f.edit(L"Patterns", join(app_.library.exclusions(0), L"\r\n"), 12);
    if (!f.show(hwnd_)) return;
    app_.library.setExclusions(0, splitLines(f.text(ex)));
    app_.detectionRulesChanged();
    app_.dataChanged();
}

void MainWindow::backupNow() {
    auto path = app_.backups->createBackup(L"manual");
    showInfo(hwnd_, L"Backup created", L"Saved to:\n" + path);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::restoreBackup() {
    auto file = openFileDialog(hwnd_, L"Restore PlayTime backup", L"PlayTime backups (*.db)\0*.db\0",
                               app_.backups->backupDir());
    if (!file) return;
    if (!confirm(hwnd_, L"Restore backup",
                 L"Replace all current data with this backup?\n\n" + *file +
                     L"\n\nA snapshot of the current data is saved first (\"pre-restore\")."))
        return;
    app_.restoreBackup(*file);
    showInfo(hwnd_, L"Restore complete", L"The backup was restored.");
}

void MainWindow::exportJson() {
    auto file = saveFileDialog(hwnd_, L"Export PlayTime data", L"JSON (*.json)\0*.json\0",
                               L"playtime-export-" + formatDate(nowUtc()) + L".json", L"json");
    if (!file) return;
    app_.tracker.checkpoint();
    app_.backups->exportJson(*file);
    showInfo(hwnd_, L"Export complete", L"Saved to:\n" + *file);
}

void MainWindow::exportCsv() {
    auto file = saveFileDialog(hwnd_, L"Export sessions as CSV", L"CSV (*.csv)\0*.csv\0",
                               L"playtime-sessions-" + formatDate(nowUtc()) + L".csv", L"csv");
    if (!file) return;
    app_.tracker.checkpoint();
    app_.backups->exportCsv(*file);
    showInfo(hwnd_, L"Export complete", L"Saved sessions and per-account totals next to:\n" + *file);
}

void MainWindow::importJsonFile() {
    auto file = openFileDialog(hwnd_, L"Import PlayTime JSON export", L"JSON (*.json)\0*.json\0");
    if (!file) return;
    if (!confirm(hwnd_, L"Import data",
                 L"Replace ALL current data with the contents of this export?\n\n" + *file +
                     L"\n\nA snapshot of the current data is saved first (\"pre-import\")."))
        return;
    app_.importJson(*file);
    showInfo(hwnd_, L"Import complete", L"The export was imported.");
}

}  // namespace pt
