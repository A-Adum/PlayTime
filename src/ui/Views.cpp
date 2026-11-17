// Painting of the individual views. Layout is computed every frame in DIPs; frames are only produced on
// input, data changes, or the 1 Hz live counter while a session is running and the window is visible.
#include <windows.h>

#include <shellapi.h>

#include <algorithm>

#include "app/Application.h"
#include "platform/WinUtil.h"
#include "ui/Dialogs.h"
#include "ui/MainWindow.h"
#include "util/Str.h"
#include "util/TimeUtil.h"

namespace pt {

namespace {
constexpr const wchar_t* kIcoHome = L"\uE80F";
constexpr const wchar_t* kIcoLibrary = L"\uE8F1";
constexpr const wchar_t* kIcoStats = L"\uE9D2";
constexpr const wchar_t* kIcoSettings = L"\uE713";
constexpr const wchar_t* kIcoBack = L"\uE72B";
constexpr const wchar_t* kIcoAdd = L"\uE710";
constexpr const wchar_t* kIcoPlay = L"\uE768";
constexpr const wchar_t* kIcoStop = L"\uE71A";
constexpr const wchar_t* kIcoMore = L"\uE712";
constexpr const wchar_t* kIcoEdit = L"\uE70F";
constexpr const wchar_t* kIcoDelete = L"\uE74D";
constexpr const wchar_t* kIcoRefresh = L"\uE72C";
constexpr const wchar_t* kIcoFolder = L"\uE8B7";
constexpr const wchar_t* kIcoPeople = L"\uE716";
constexpr const wchar_t* kIcoGame = L"\uE7FC";
constexpr const wchar_t* kIcoSearch = L"\uE721";
constexpr const wchar_t* kIcoDown = L"\uE70D";
constexpr const wchar_t* kIcoMerge = L"\uE8AB";

std::wstring plural(int64_t n, const wchar_t* one, const wchar_t* many) {
    return groupThousands(n) + L" " + (n == 1 ? one : many);
}

std::wstring launcherName(const std::wstring& l) {
    if (l.empty()) return L"\u2014";
    if (l == L"epic") return L"Epic Games";
    if (l == L"battlenet") return L"Battle.net";
    if (l == L"ea") return L"EA app";
    return AccountManager::launcherDisplayName(l);
}

void openFolder(const std::wstring& dir) { ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
}  // namespace

// ---------------------------------------------------------------- chrome

void MainWindow::paintSidebar(float h) {
    gfx_.fill(rect(0, 0, kSidebarW, h), theme::sidebar);
    gfx_.round(rect(20, 22, 32, 32), 9, theme::accent);
    gfx_.text(kIcoPlay, rect(20, 22, 32, 32), Font::Icon, rgb(0x0E1420), Align::Center);
    gfx_.text(L"PlayTime", rect(62, 22, 140, 32), Font::Title, theme::text);

    struct Nav {
        View v;
        const wchar_t* icon;
        const wchar_t* label;
    };
    const Nav items[] = {{View::Dashboard, kIcoHome, L"Dashboard"},
                         {View::Library, kIcoLibrary, L"Library"},
                         {View::Stats, kIcoStats, L"Statistics"},
                         {View::Settings, kIcoSettings, L"Settings"}};
    float y = 84;
    View current = view_ == View::Game ? prevView_ : view_;
    for (auto& it : items) {
        auto r = rect(12, y, kSidebarW - 24, 40);
        bool sel = it.v == current;
        bool hot = hovered(r);
        if (sel || hot) gfx_.round(r, 8, sel ? theme::card : withAlpha(theme::card, 0.6f));
        if (sel) gfx_.round(rect(12, y + 10, 3, 20), 1.5f, theme::accent);
        Color fg = sel ? theme::text : theme::muted;
        gfx_.text(it.icon, rect(28, y, 20, 40), Font::Icon, sel ? theme::accent : fg, Align::Center);
        gfx_.text(it.label, rect(60, y, 140, 40), sel ? Font::BodyBold : Font::Body, fg);
        View v = it.v;
        addHit(r, [this, v] { navigate(v); });
        y += 44;
    }

    // Live sessions + tracking status at the bottom.
    float by = h - 66;
    auto& active = app_.tracker.active();
    size_t shown = std::min<size_t>(active.size(), 3);
    float ny = by - 12 - shown * 54.f;
    for (size_t i = 0; i < shown; ++i) {
        auto& s = active[i];
        auto it = std::find_if(games_.begin(), games_.end(), [&](auto& g) { return g.game.id == s.gameId; });
        auto r = rect(12, ny, kSidebarW - 24, 48);
        gfx_.round(r, 8, hovered(r) ? theme::cardHover : theme::card);
        gfx_.circle(28, ny + 17, 4, s.running ? theme::success : theme::warning);
        gfx_.text(it != games_.end() ? it->game.name : L"\u2026", rect(40, ny + 6, kSidebarW - 64, 20), Font::SmallBold,
                  theme::text);
        gfx_.text(formatClock(app_.tracker.liveSeconds(s)) + (s.manual ? L"  manual" : L""),
                  rect(40, ny + 24, kSidebarW - 64, 18), Font::Small, s.running ? theme::success : theme::warning);
        Id gid = s.gameId;
        addHit(r, [this, gid] { openGame(gid); });
        ny += 54;
    }

    gfx_.line(16, by, kSidebarW - 16, by, theme::border);
    bool enabled = app_.trackingEnabled();
    bool activeTracking = app_.trackingActive();
    gfx_.circle(26, by + 33, 4, activeTracking ? theme::success : theme::warning);
    std::wstring status = activeTracking ? L"Tracking" : app_.lockPaused() ? L"Paused (locked)" : L"Tracking paused";
    gfx_.text(status, rect(38, by + 22, 110, 22), Font::Small, theme::muted);
    toggle(kSidebarW - 56, by + 22, enabled, [this, enabled] { app_.setTrackingEnabled(!enabled); });
}

void MainWindow::paintHeader(float x, float w) {
    float pad = 28;
    bodyTop_ = view_ == View::Library ? 120.f : 76.f;
    gfx_.fill(rect(x, 0, w, bodyTop_), theme::bg);
    float right = x + w - pad;
    float ty = 22;
    switch (view_) {
        case View::Dashboard: {
            gfx_.text(L"Dashboard", rect(x + pad, ty, 400, 34), Font::Title, theme::text);
            auto p = toLocal(nowUtc());
            static const wchar_t* days[] = {L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday", L"Sunday"};
            std::wstring date = std::wstring(days[p.weekday]) + L", " + std::to_wstring(p.day) + L" " +
                                monthShortName(p.month) + L" " + std::to_wstring(p.year);
            gfx_.text(date, rect(right - 300, ty, 300, 34), Font::Body, theme::muted, Align::Right);
            break;
        }
        case View::Library: {
            gfx_.text(L"Library", rect(x + pad, ty, 200, 34), Font::Title, theme::text);
            gfx_.text(plural((int64_t)games_.size(), L"game", L"games"), rect(x + pad + 92, ty + 4, 200, 30), Font::Body,
                      theme::muted);
            float bx = buttonRight(right, ty, L"Scan launchers", [this] { scanLaunchers(); }, Btn::Primary, kIcoRefresh);
            bx = buttonRight(bx - 8, ty, L"Add executable", [this] { addGameFromExe(); }, Btn::Secondary, kIcoAdd);
            buttonRight(bx - 8, ty, L"Add custom game", [this] { addCustomGame(); }, Btn::Secondary, kIcoGame);
            // Search box (native edit sits inside) + sort chips
            float sy = 74;
            auto sr = rect(x + pad, sy, 260, 32);
            gfx_.round(sr, 6, theme::card);
            gfx_.roundStroke(sr, 6, GetFocus() == search_ ? theme::accent : theme::border);
            gfx_.text(kIcoSearch, rect(sr.right - 30, sy, 22, 32), Font::IconSmall, theme::faint, Align::Center);
            float cx = sr.right + 18;
            gfx_.text(L"Sort", rect(cx, sy, 40, 32), Font::Small, theme::faint);
            cx += 38;
            const std::pair<const wchar_t*, const wchar_t*> sorts[] = {
                {L"playtime", L"Most played"}, {L"last", L"Last played"}, {L"name", L"Name"}, {L"recent", L"Recent activity"}};
            for (auto& [key, label] : sorts) {
                std::wstring k = key;
                cx += chip(cx, sy + 2, label, sort_ == k, [this, k] {
                          sort_ = k;
                          app_.settings.set(setting::LibrarySort, k);
                          InvalidateRect(hwnd_, nullptr, FALSE);
                      }) + 8;
            }
            break;
        }
        case View::Game: {
            button(x + pad - 6, ty, L"", [this] { navigate(prevView_); }, Btn::Ghost, kIcoBack);
            gfx_.text(gv_.summary.game.name, rect(x + pad + 40, ty, w - pad * 2 - 420, 34), Font::Title, theme::text);
            Id gid = gameId_;
            float bx = buttonRight(right, ty, L"Remove", [this, gid] { deleteGame(gid); }, Btn::Danger, kIcoDelete);
            bx = buttonRight(bx - 8, ty, L"Merge", [this, gid] { mergeGame(gid); }, Btn::Secondary, kIcoMerge);
            buttonRight(bx - 8, ty, L"Edit", [this, gid] { editGame(gid); }, Btn::Secondary, kIcoEdit);
            break;
        }
        case View::Stats:
            gfx_.text(L"Statistics", rect(x + pad, ty, 400, 34), Font::Title, theme::text);
            break;
        case View::Settings:
            gfx_.text(L"Settings", rect(x + pad, ty, 400, 34), Font::Title, theme::text);
            break;
    }
}

std::vector<const GameSummary*> MainWindow::sortedGames(bool filter) const {
    std::vector<const GameSummary*> out;
    for (auto& g : games_)
        if (!filter || filter_.empty() || toLower(g.game.name).find(filter_) != std::wstring::npos) out.push_back(&g);
    auto total = [this](const GameSummary* g) { return g->total() + liveExtra(g->game.id); };
    auto last = [this](const GameSummary* g) { return app_.tracker.find(g->game.id) ? INT64_MAX : g->lastPlayed; };
    if (sort_ == L"name") {
        std::sort(out.begin(), out.end(), [](auto a, auto b) { return _wcsicmp(a->game.name.c_str(), b->game.name.c_str()) < 0; });
    } else if (sort_ == L"last") {
        std::stable_sort(out.begin(), out.end(), [&](auto a, auto b) { return last(a) > last(b); });
    } else if (sort_ == L"recent") {
        std::stable_sort(out.begin(), out.end(), [&](auto a, auto b) {
            int64_t ra = a->recent + liveExtra(a->game.id), rb = b->recent + liveExtra(b->game.id);
            return ra != rb ? ra > rb : last(a) > last(b);
        });
    } else {
        std::stable_sort(out.begin(), out.end(), [&](auto a, auto b) { return total(a) > total(b); });
    }
    return out;
}

float MainWindow::gameRow(float x, float y, float w, const GameSummary& g) {
    const float h = 78;
    float visTop = bodyTop_ + scroll_, visBottom = height_ + scroll_;
    if (y + h < visTop || y > visBottom) return y + h + 8;  // off-screen: skip drawing
    auto r = rect(x, y, w, h);
    bool hot = hovered(r);
    gfx_.round(r, 10, hot ? theme::cardHover : theme::card);
    cover(g.game.coverPath, g.game.name, rect(x + 10, y + 6, 47, 66), 6);
    const ActiveSession* live = app_.tracker.find(g.game.id);
    float nameW = w - 74 - 210;
    gfx_.text(g.game.name, rect(x + 72, y + 12, nameW, 22), Font::Section, theme::text);
    std::wstring sub = (g.lastPlayed ? L"Last played " + formatRelative(g.lastPlayed, nowUtc()) : L"Not played yet");
    sub += L"   \u00B7   " + plural(g.accountCount, L"account", L"accounts");
    if (g.sessionCount) sub += L"   \u00B7   " + plural(g.sessionCount, L"session", L"sessions");
    gfx_.text(sub, rect(x + 72, y + 36, nameW, 18), Font::Small, theme::muted);
    if (live) {
        gfx_.circle(x + 76, y + 63, 3.5f, live->running ? theme::success : theme::warning);
        gfx_.text((live->running ? L"Playing  " : L"Closing  ") + formatClock(app_.tracker.liveSeconds(*live)),
                  rect(x + 84, y + 54, 200, 18), Font::SmallBold, live->running ? theme::success : theme::warning);
    }
    int64_t extra = liveExtra(g.game.id);
    gfx_.text(hours(g.total() + extra), rect(x + w - 220, y + 14, 200, 28), Font::Title, theme::text, Align::Right);
    std::wstring detail = L"tracked " + hours(g.tracked + extra);
    if (g.baseline) detail = L"historical " + hours(g.baseline) + L"  \u00B7  " + detail;
    gfx_.text(detail, rect(x + w - 360, y + 44, 340, 18), Font::Small, theme::faint, Align::Right);
    Id id = g.game.id;
    addHit(r, [this, id] { openGame(id); }, {}, [this, id] { showGameMenu(id); });
    return y + h + 8;
}

// ---------------------------------------------------------------- dashboard

float MainWindow::paintDashboard(float x, float y, float w) {
    int64_t extra = liveExtra(0);
    float gap = 14, tw = (w - gap * 3) / 4;
    int64_t played = 0;
    for (auto& g : games_)
        if (g.total() > 0) ++played;
    statTile(rect(x, y, tw, 92), L"Lifetime", hours(lifetime_ + extra), plural(played, L"game played", L"games played"));
    statTile(rect(x + (tw + gap), y, tw, 92), L"Today", hours(today_ + extra));
    statTile(rect(x + (tw + gap) * 2, y, tw, 92), L"This week", hours(week_ + extra));
    statTile(rect(x + (tw + gap) * 3, y, tw, 92), L"This month", hours(month_ + extra));
    y += 92 + 18;

    if (games_.empty()) {
        auto r = rect(x, y, w, 150);
        card(r);
        gfx_.text(L"Welcome to PlayTime", rect(x + 24, y + 20, w - 48, 26), Font::Section, theme::text);
        gfx_.text(L"Add your games once, enter the hours you already have, and PlayTime tracks everything from now on - "
                  L"no launcher needs to stay open.",
                  rect(x + 24, y + 50, w - 48, 40), Font::Body, theme::muted, Align::Left, true);
        float bx = x + 24;
        bx += button(bx, y + 98, L"Scan installed games", [this] { scanLaunchers(); }, Btn::Primary, kIcoRefresh) + 8;
        bx += button(bx, y + 98, L"Add executable", [this] { addGameFromExe(); }, Btn::Secondary, kIcoAdd) + 8;
        button(bx, y + 98, L"Add custom game", [this] { addCustomGame(); }, Btn::Secondary, kIcoGame);
        return y + 150;
    }

    // Now playing
    auto& active = app_.tracker.active();
    if (active.empty()) {
        auto r = rect(x, y, w, 64);
        card(r);
        gfx_.text(kIcoGame, rect(x + 18, y, 24, 64), Font::Icon, theme::faint, Align::Center);
        gfx_.text(app_.trackingActive() ? L"No game running" : L"Tracking is paused",
                  rect(x + 54, y + 12, w - 80, 22), Font::BodyBold, theme::text);
        gfx_.text(app_.trackingActive() ? L"Games are detected automatically when they start."
                                        : L"Resume tracking from the sidebar or the tray icon.",
                  rect(x + 54, y + 32, w - 80, 20), Font::Small, theme::muted);
        y += 64 + 18;
    } else {
        float h = 18 + active.size() * 62.f + (active.size() > 1 ? 26 : 0);
        card(rect(x, y, w, h));
        float ry = y + 10;
        for (auto& s : active) {
            auto it = std::find_if(games_.begin(), games_.end(), [&](auto& g) { return g.game.id == s.gameId; });
            if (it == games_.end()) continue;
            cover(it->game.coverPath, it->game.name, rect(x + 14, ry + 4, 38, 52), 5);
            gfx_.circle(x + 68, ry + 19, 4, s.running ? theme::success : theme::warning);
            gfx_.text(it->game.name, rect(x + 80, ry + 8, w - 520, 22), Font::Section, theme::text);
            std::wstring acc = L"Unassigned";
            if (s.accountId)
                if (auto a = app_.accounts.account(s.accountId)) acc = a->name;
            std::wstring how = s.manual ? L"manual session" : s.resumed ? L"resumed after restart" : L"detected " + s.exeName;
            if (!s.running) how = L"game closed \u2014 waiting briefly for a relaunch";
            gfx_.text(acc + L"   \u00B7   " + how, rect(x + 68, ry + 32, w - 500, 18), Font::Small, theme::muted);
            gfx_.text(formatClock(app_.tracker.liveSeconds(s)), rect(x + w - 420, ry + 10, 160, 34), Font::Title,
                      s.running ? theme::success : theme::warning, Align::Right);
            Id gid = s.gameId;
            float bx = x + w - 244;
            bx += button(bx, ry + 12, L"Account", [this, gid] { chooseCurrentAccount(gid); }, Btn::Secondary, kIcoPeople) + 8;
            if (s.manual) button(bx, ry + 12, L"Stop", [this, gid] { app_.stopSession(gid); }, Btn::Secondary, kIcoStop);
            else button(bx, ry + 12, L"Open", [this, gid] { openGame(gid); }, Btn::Ghost);
            ry += 62;
        }
        if (active.size() > 1)
            gfx_.text(L"Each running game accumulates its own time, so combined game-hours can exceed real elapsed time.",
                      rect(x + 18, ry, w - 36, 18), Font::Small, theme::faint);
        y += h + 18;
    }

    // Activity heatmap + most played
    float rightW = std::min(360.f, w * 0.36f), leftW = w - rightW - gap;
    float cardH = 196;
    card(rect(x, y, leftW, cardH));
    sectionTitle(x + 18, y + 14, L"Activity");
    gfx_.text(L"last " + std::to_wstring(std::min<int>(53, (int)((leftW - 66) / 15))) + L" weeks",
              rect(x + leftW - 200, y + 14, 182, 24), Font::Small, theme::faint, Align::Right);
    heatmap(x + 18, y + 46, leftW - 36, heat_);

    card(rect(x + leftW + gap, y, rightW, cardH));
    sectionTitle(x + leftW + gap + 18, y + 14, L"Most played");
    auto top = games_;
    std::sort(top.begin(), top.end(), [&](auto& a, auto& b) { return a.total() + liveExtra(a.game.id) > b.total() + liveExtra(b.game.id); });
    int64_t maxT = top.empty() ? 1 : std::max<int64_t>(1, top[0].total() + liveExtra(top[0].game.id));
    float my = y + 48, mx = x + leftW + gap + 18, mw = rightW - 36;
    for (size_t i = 0; i < top.size() && i < 5; ++i) {
        int64_t t = top[i].total() + liveExtra(top[i].game.id);
        if (t <= 0) break;
        auto row = rect(mx - 6, my - 2, mw + 12, 28);
        if (hovered(row)) gfx_.round(row, 6, theme::cardHover);
        gfx_.text(top[i].game.name, rect(mx, my, mw - 100, 16), Font::Small, theme::text);
        gfx_.text(hours(t), rect(mx + mw - 100, my, 100, 16), Font::SmallBold, theme::muted, Align::Right);
        gfx_.round(rect(mx, my + 18, mw, 4), 2, theme::raised);
        gfx_.round(rect(mx, my + 18, mw * (float)t / maxT, 4), 2, i == 0 ? theme::accent : theme::accentDim);
        Id gid = top[i].game.id;
        addHit(row, [this, gid] { openGame(gid); });
        my += 28;
    }
    y += cardH + 18;

    // Recently played
    sectionTitle(x, y, L"Recently played");
    float ly = y + 34;
    auto recent = games_;
    std::sort(recent.begin(), recent.end(), [](auto& a, auto& b) { return a.lastPlayed > b.lastPlayed; });
    int n = 0;
    for (auto& g : recent) {
        if (!g.lastPlayed || n++ >= 6) break;
        ly = gameRow(x, ly, w, g);
    }
    if (n == 0) {
        gfx_.text(L"Nothing tracked yet. Launch a game from your library and it will appear here.",
                  rect(x, ly, w, 20), Font::Body, theme::muted);
        ly += 28;
    }
    return ly;
}

// ---------------------------------------------------------------- library

float MainWindow::paintLibrary(float x, float y, float w) {
    auto list = sortedGames(true);
    if (list.empty()) {
        gfx_.text(games_.empty() ? L"Your library is empty. Scan launchers or add a game."
                                 : L"No games match your search.",
                  rect(x, y + 8, w, 24), Font::Body, theme::muted);
        return y + 40;
    }
    for (auto* g : list) y = gameRow(x, y, w, *g);
    return y;
}

// ---------------------------------------------------------------- game details

float MainWindow::paintGame(float x, float y, float w) {
    auto& g = gv_.summary;
    int64_t extra = liveExtra(gameId_);
    const ActiveSession* live = app_.tracker.find(gameId_);
    float gap = 14;

    // Hero
    float heroH = 214;
    card(rect(x, y, w, heroH));
    cover(g.game.coverPath, g.game.name, rect(x + 18, y + 18, 120, 178), 8);
    float hx = x + 162;
    gfx_.text(L"Lifetime playtime", rect(hx, y + 18, 300, 18), Font::Small, theme::muted);
    gfx_.text(hours(g.total() + extra), rect(hx, y + 36, w - 200, 42), Font::Display, theme::text);
    struct Mini {
        const wchar_t* label;
        std::wstring value;
    };
    Mini minis[] = {{L"Historical", hours(g.baseline)},
                    {L"Tracked", hours(g.tracked + extra)},
                    {L"Accounts", std::to_wstring(g.accountCount)},
                    {L"Sessions", groupThousands(g.sessionCount)}};
    float mx = hx;
    for (auto& m : minis) {
        gfx_.text(m.label, rect(mx, y + 88, 150, 16), Font::Small, theme::faint);
        gfx_.text(m.value, rect(mx, y + 104, 150, 22), Font::Section, theme::text);
        mx += 150;
    }
    std::wstring status;
    if (live) {
        std::wstring acc = L"Unassigned";
        if (live->accountId)
            if (auto a = app_.accounts.account(live->accountId)) acc = a->name;
        status = (live->running ? L"Playing now  \u00B7  " : L"Closing  \u00B7  ") + formatClock(app_.tracker.liveSeconds(*live)) +
                 L"  \u00B7  " + acc;
        gfx_.circle(hx + 4, y + 145, 4, live->running ? theme::success : theme::warning);
        gfx_.text(status, rect(hx + 14, y + 136, w - 200, 18), Font::SmallBold, live->running ? theme::success : theme::warning);
    } else {
        status = g.lastPlayed ? L"Last played " + formatRelative(g.lastPlayed, nowUtc()) : L"Not played yet";
        float lx = hx + gfx_.textWidth(status, Font::Small) + 14;
        gfx_.text(status, rect(hx, y + 136, 400, 18), Font::Small, theme::muted);
        for (auto& l : gv_.launchers) {
            auto name = launcherName(l);
            float tw = gfx_.textWidth(name, Font::Small) + 16;
            gfx_.round(rect(lx, y + 136, tw, 18), 9, theme::raised);
            gfx_.text(name, rect(lx, y + 136, tw, 18), Font::Small, theme::muted, Align::Center);
            lx += tw + 6;
        }
    }
    Id gid = gameId_;
    float by = y + heroH - 50, bx = hx;
    bx += button(bx, by, L"Adjust hours", [this, gid] { adjustHours(gid, kUnassigned); }, Btn::Primary, kIcoEdit) + 8;
    bx += button(bx, by, L"Add account", [this, gid] { addAccount(gid); }, Btn::Secondary, kIcoPeople) + 8;
    bx += button(bx, by, L"Add session", [this, gid] { addSession(gid); }, Btn::Secondary, kIcoAdd) + 8;
    if (live && live->manual)
        bx += button(bx, by, L"Stop session", [this, gid] { app_.stopSession(gid); }, Btn::Secondary, kIcoStop) + 8;
    else if (!live)
        bx += button(bx, by, L"Start session", [this, gid] { app_.startManualSession(gid); }, Btn::Secondary, kIcoPlay,
                     app_.trackingActive()) + 8;
    Id pref = g.game.preferredAccount;
    std::wstring prefName = L"Auto-detect";
    for (auto& a : gv_.accounts)
        if (a.accountId == pref && pref) prefName = a.name;
    button(bx, by, L"Current account: " + prefName, [this, gid] { chooseCurrentAccount(gid); }, Btn::Ghost, kIcoDown);
    y += heroH + 18;

    // Accounts table
    size_t rows = gv_.accounts.size();
    float tableH = 52 + 30 + std::max<size_t>(rows, 1) * 40.f + (rows ? 44 : 0);
    card(rect(x, y, w, tableH));
    sectionTitle(x + 18, y + 14, L"Accounts");
    gfx_.text(L"Right-click or \u22EF for rename, merge, adjust and delete", rect(x + 120, y + 16, w - 300, 20),
              Font::Small, theme::faint);
    buttonRight(x + w - 14, y + 10, L"Add account", [this, gid] { addAccount(gid); }, Btn::Ghost, kIcoAdd);
    float colTotal = x + w - 64, colTracked = colTotal - 130, colHist = colTracked - 120, colLaunch = colHist - 120;
    float ty = y + 52;
    auto header = [&](const wchar_t* t, float right, float wdt) {
        gfx_.text(t, rect(right - wdt, ty, wdt, 22), Font::Small, theme::faint, Align::Right);
    };
    gfx_.text(L"Account", rect(x + 18, ty, 200, 22), Font::Small, theme::faint);
    gfx_.text(L"Launcher", rect(colLaunch - 110, ty, 110, 22), Font::Small, theme::faint);
    header(L"Historical", colHist, 110);
    header(L"Tracked", colTracked, 110);
    header(L"Total", colTotal, 120);
    ty += 30;
    int64_t sumTotal = 0, sumHist = 0, sumTracked = 0;
    int64_t gameTotal = std::max<int64_t>(1, g.total() + extra);
    if (rows == 0) {
        gfx_.text(L"No accounts yet. Add one per launcher login (e.g. \"Epic Main\", \"Steam\"); detected Epic/Steam "
                  L"logins are added automatically.",
                  rect(x + 18, ty, w - 36, 40), Font::Small, theme::muted);
    }
    for (auto& a : gv_.accounts) {
        int64_t ax = liveExtra(gameId_, a.accountId);
        int64_t tracked = a.tracked + ax, total = a.total() + ax;
        sumTotal += total;
        sumHist += a.baseline;
        sumTracked += tracked;
        auto rr = rect(x + 8, ty, w - 16, 40);
        if (hovered(rr)) gfx_.round(rr, 6, theme::cardHover);
        bool un = a.accountId == kUnassigned;
        bool isPref = !un && a.accountId == pref, isLive = live && live->accountId == a.accountId;
        float nameRight = colLaunch - 110 - 24;  // name, tags and share bar stay left of the launcher column
        float tagsW = (isPref ? 64.f : 0.f) + (isLive ? 60.f : 0.f);
        float nameW = std::max(40.f, nameRight - (x + 18) - tagsW);
        Font nameFont = un ? Font::Body : Font::BodyBold;
        gfx_.text(a.name, rect(x + 18, ty, nameW, 36), nameFont, un ? theme::muted : theme::text);
        float nw = std::min(nameW, gfx_.textWidth(a.name, nameFont));
        float tagX = x + 18 + nw + 8;
        if (isPref) {
            gfx_.round(rect(tagX, ty + 9, 58, 18), 9, withAlpha(theme::accent, 0.2f));
            gfx_.text(L"current", rect(tagX, ty + 9, 58, 18), Font::Small, theme::accent, Align::Center);
            tagX += 64;
        }
        if (isLive) {
            gfx_.round(rect(tagX, ty + 9, 54, 18), 9, withAlpha(theme::success, 0.2f));
            gfx_.text(L"live", rect(tagX, ty + 9, 54, 18), Font::Small, theme::success, Align::Center);
        }
        if (!a.baselineSource.empty() && a.baselineSource != L"manual" && a.importedTotal >= 0) {
            // Imported totals are shown on hover over the historical cell.
            addHit(rect(colHist - 110, ty, 110, 40), nullptr,
                   L"Imported from " + launcherName(a.baselineSource) + L": " + hours(a.importedTotal) +
                       L" lifetime, minus " + hours(a.tracked) + L" tracked by PlayTime");
        }
        // Share of the game's lifetime total
        float barW = nameRight - (x + 18);
        gfx_.round(rect(x + 18, ty + 32, barW, 3), 1.5f, theme::raised);
        gfx_.round(rect(x + 18, ty + 32, barW * std::clamp((float)total / gameTotal, 0.f, 1.f), 3), 1.5f,
                   un ? theme::faint : theme::accent);
        gfx_.text(un ? L"\u2014" : launcherName(a.launcher), rect(colLaunch - 110, ty, 110, 40), Font::Small, theme::muted);
        gfx_.text(hours(a.baseline), rect(colHist - 110, ty, 110, 40), Font::Body, theme::muted, Align::Right);
        gfx_.text(hours(tracked), rect(colTracked - 110, ty, 110, 40), Font::Body, theme::muted, Align::Right);
        gfx_.text(hours(total), rect(colTotal - 120, ty, 120, 40), Font::BodyBold, theme::text, Align::Right);
        auto more = rect(x + w - 48, ty + 6, 32, 28);
        gfx_.round(more, 6, hovered(more) ? theme::raised : withAlpha(theme::raised, 0.f));
        gfx_.text(kIcoMore, more, Font::IconSmall, theme::muted, Align::Center);
        AccountTotals copy = a;
        addHit(rr, nullptr, {}, [this, copy] { showAccountMenu(copy); });
        addHit(more, [this, copy] { showAccountMenu(copy); });
        ty += 40;
    }
    if (rows) {
        gfx_.line(x + 18, ty + 4, x + w - 18, ty + 4, theme::border);
        gfx_.text(L"Total", rect(x + 18, ty + 6, 200, 36), Font::BodyBold, theme::text);
        gfx_.text(hours(sumHist), rect(colHist - 110, ty + 6, 110, 36), Font::BodyBold, theme::muted, Align::Right);
        gfx_.text(hours(sumTracked), rect(colTracked - 110, ty + 6, 110, 36), Font::BodyBold, theme::muted, Align::Right);
        gfx_.text(hours(sumTotal), rect(colTotal - 120, ty + 6, 120, 36), Font::Section, theme::accent, Align::Right);
    }
    y += tableH + 18;

    // Session statistics
    float tw = (w - gap * 4) / 5;
    auto& st = gv_.stats;
    statTile(rect(x, y, tw, 84), L"Longest session", st.longest ? formatShortDuration(st.longest) : L"\u2014",
             st.longest ? formatDate(st.longestStart) : L"");
    statTile(rect(x + (tw + gap), y, tw, 84), L"Average session", st.count ? formatShortDuration(st.average) : L"\u2014");
    statTile(rect(x + (tw + gap) * 2, y, tw, 84), L"Sessions", groupThousands(st.count));
    statTile(rect(x + (tw + gap) * 3, y, tw, 84), L"First tracked", st.firstStart ? formatDate(st.firstStart) : L"\u2014");
    statTile(rect(x + (tw + gap) * 4, y, tw, 84), L"Last played",
             live ? std::wstring(L"Now") : (st.lastEnd ? formatDate(st.lastEnd) : std::wstring(L"\u2014")));
    y += 84 + 18;

    // Chart
    float chartH = 262;
    card(rect(x, y, w, chartH));
    sectionTitle(x + 18, y + 14, L"Playtime");
    const wchar_t* modes[] = {L"Daily", L"Weekly", L"Monthly"};
    float cx = x + w - 18;
    for (int i = 2; i >= 0; --i) {
        float cw = gfx_.textWidth(modes[i], Font::Small) + 24;
        cx -= cw;
        chip(cx, y + 12, modes[i], chartMode_ == i, [this, i] {
            chartMode_ = i;
            refresh();
        });
        cx -= 6;
    }
    barChart(rect(x + 14, y + 52, w - 32, 176), gv_.chart, chartMode_);
    if (g.baseline > 0)
        gfx_.text(L"Historical hours have no dates, so they are counted in lifetime totals but not in this chart.",
                  rect(x + 18, y + chartH - 28, w - 36, 18), Font::Small, theme::faint);
    y += chartH + 18;

    // Sessions
    size_t n = gv_.sessions.size();
    float listH = 52 + 26 + std::max<size_t>(n, 1) * 38.f + 12;
    card(rect(x, y, w, listH));
    sectionTitle(x + 18, y + 14, L"Sessions");
    gfx_.text(groupThousands((int64_t)n), rect(x + 100, y + 16, 100, 20), Font::Small, theme::faint);
    buttonRight(x + w - 14, y + 10, L"Add session", [this, gid] { addSession(gid); }, Btn::Ghost, kIcoAdd);
    float sy = y + 52;
    float cDate = x + 18, cDur = x + 300, cAcc = x + 420, cFlags = x + w - 300;
    gfx_.text(L"When", rect(cDate, sy, 200, 20), Font::Small, theme::faint);
    gfx_.text(L"Duration", rect(cDur, sy, 100, 20), Font::Small, theme::faint);
    gfx_.text(L"Account", rect(cAcc, sy, 200, 20), Font::Small, theme::faint);
    gfx_.text(L"Source", rect(cFlags, sy, 200, 20), Font::Small, theme::faint);
    sy += 26;
    if (n == 0) gfx_.text(L"No sessions recorded yet.", rect(cDate, sy, 400, 38), Font::Body, theme::muted);
    std::unordered_map<Id, std::wstring> accNames;
    for (auto& a : gv_.accounts) accNames[a.accountId] = a.name;
    float visTop = bodyTop_ + scroll_, visBottom = height_ + scroll_;
    for (auto& s : gv_.sessions) {
        if (sy + 38 < visTop || sy > visBottom) {
            sy += 38;
            continue;
        }
        bool isLive = live && live->sessionId == s.id;
        auto rr = rect(x + 8, sy, w - 16, 38);
        if (hovered(rr)) gfx_.round(rr, 6, theme::cardHover);
        int64_t dur = isLive ? app_.tracker.liveSeconds(*live) : s.duration;
        std::wstring when = formatDateTime(s.startUtc) + L" \u2013 " +
                            (isLive ? std::wstring(L"now") : formatTime(std::max(s.endUtc, s.startUtc + s.duration)));
        gfx_.text(when, rect(cDate, sy, cDur - cDate - 10, 38), Font::Body, theme::text);
        gfx_.text(formatShortDuration(dur), rect(cDur, sy, 110, 38), Font::BodyBold, isLive ? theme::success : theme::text);
        auto an = accNames.find(s.accountId);
        gfx_.text(an != accNames.end() ? an->second : L"Unassigned", rect(cAcc, sy, cFlags - cAcc - 10, 38), Font::Body,
                  s.accountId ? theme::muted : theme::faint);
        std::wstring flags = isLive ? L"\u25CF Live" : s.source == SessionSource::Manual ? L"Manual" : L"Automatic";
        if (s.recovered) flags += L" \u00B7 recovered";
        if (s.edited) flags += L" \u00B7 edited";
        gfx_.text(flags, rect(cFlags, sy, 240, 38), Font::Small, isLive ? theme::success : theme::muted);
        auto more = rect(x + w - 48, sy + 5, 32, 28);
        gfx_.round(more, 6, hovered(more) ? theme::raised : withAlpha(theme::raised, 0.f));
        gfx_.text(kIcoMore, more, Font::IconSmall, theme::muted, Align::Center);
        Id sid = s.id;
        std::wstring tip = s.exeName.empty() ? std::wstring() : s.exeName;
        addHit(rr, nullptr, tip, [this, sid] { showSessionMenu(sid); });
        addHit(more, [this, sid] { showSessionMenu(sid); });
        sy += 38;
    }
    return y + listH;
}

// ---------------------------------------------------------------- statistics

float MainWindow::paintStats(float x, float y, float w) {
    int64_t extra = liveExtra(0);
    float gap = 14, tw = (w - gap * 3) / 4;
    statTile(rect(x, y, tw, 92), L"Lifetime", hours(lifetime_ + extra), L"incl. historical hours");
    statTile(rect(x + (tw + gap), y, tw, 92), L"Today", hours(today_ + extra));
    statTile(rect(x + (tw + gap) * 2, y, tw, 92), L"This week", hours(week_ + extra), L"since Monday");
    statTile(rect(x + (tw + gap) * 3, y, tw, 92), L"This month", hours(month_ + extra));
    y += 92 + gap;
    std::wstring longestGame;
    for (auto& g : games_)
        if (g.game.id == allStats_.longestGame) longestGame = g.game.name;
    int64_t played = 0;
    for (auto& g : games_)
        if (g.sessionCount) ++played;
    statTile(rect(x, y, tw, 92), L"Longest session", allStats_.longest ? formatShortDuration(allStats_.longest) : L"\u2014",
             longestGame.empty() ? L"" : longestGame + L", " + formatDate(allStats_.longestStart));
    statTile(rect(x + (tw + gap), y, tw, 92), L"Average session",
             allStats_.count ? formatShortDuration(allStats_.average) : L"\u2014");
    statTile(rect(x + (tw + gap) * 2, y, tw, 92), L"Sessions", groupThousands(allStats_.count),
             allStats_.firstStart ? L"since " + formatDate(allStats_.firstStart) : L"");
    statTile(rect(x + (tw + gap) * 3, y, tw, 92), L"Games tracked", groupThousands(played));
    y += 92 + 18;

    card(rect(x, y, w, 196));
    sectionTitle(x + 18, y + 14, L"Activity");
    heatmap(x + 18, y + 46, w - 36, heat_);
    y += 196 + 18;

    card(rect(x, y, w, 250));
    sectionTitle(x + 18, y + 14, L"Last 30 days");
    barChart(rect(x + 14, y + 50, w - 32, 184), daily30_, 0);
    y += 250 + 18;

    card(rect(x, y, w, 250));
    sectionTitle(x + 18, y + 14, L"Last 12 months");
    barChart(rect(x + 14, y + 50, w - 32, 184), monthly12_, 2);
    y += 250 + 18;

    // Most played games and top accounts
    float half = (w - gap) / 2;
    auto top = games_;
    std::sort(top.begin(), top.end(), [&](auto& a, auto& b) { return a.total() + liveExtra(a.game.id) > b.total() + liveExtra(b.game.id); });
    size_t topN = 0;
    while (topN < top.size() && topN < 10 && top[topN].total() > 0) ++topN;
    size_t accN = topAccounts_.size();
    float listH = 52 + std::max<size_t>(std::max(topN, accN), 1) * 34.f + 10;
    card(rect(x, y, half, listH));
    sectionTitle(x + 18, y + 14, L"Most played games");
    int64_t maxT = topN ? std::max<int64_t>(1, top[0].total() + liveExtra(top[0].game.id)) : 1;
    float ly = y + 52;
    for (size_t i = 0; i < topN; ++i) {
        int64_t t = top[i].total() + liveExtra(top[i].game.id);
        auto row = rect(x + 10, ly - 3, half - 20, 32);
        if (hovered(row)) gfx_.round(row, 6, theme::cardHover);
        gfx_.text(std::to_wstring(i + 1), rect(x + 18, ly, 20, 18), Font::Small, theme::faint);
        gfx_.text(top[i].game.name, rect(x + 40, ly, half - 170, 18), Font::Small, theme::text);
        gfx_.text(hours(t), rect(x + half - 130, ly, 112, 18), Font::SmallBold, theme::muted, Align::Right);
        gfx_.round(rect(x + 40, ly + 20, half - 58, 4), 2, theme::raised);
        gfx_.round(rect(x + 40, ly + 20, (half - 58) * (float)t / maxT, 4), 2, theme::accent);
        Id gid = top[i].game.id;
        addHit(row, [this, gid] { openGame(gid); });
        ly += 34;
    }
    if (!topN) gfx_.text(L"No playtime yet.", rect(x + 18, ly, half - 36, 20), Font::Small, theme::muted);

    float ax = x + half + gap;
    card(rect(ax, y, half, listH));
    sectionTitle(ax + 18, y + 14, L"Top accounts");
    ly = y + 52;
    int64_t maxA = accN ? std::max<int64_t>(1, topAccounts_[0].seconds) : 1;
    for (auto& a : topAccounts_) {
        auto row = rect(ax + 10, ly - 3, half - 20, 32);
        if (hovered(row)) gfx_.round(row, 6, theme::cardHover);
        gfx_.text(a.accountName + L"  \u00B7  " + a.gameName, rect(ax + 18, ly, half - 160, 18), Font::Small, theme::text);
        gfx_.text(hours(a.seconds), rect(ax + half - 130, ly, 112, 18), Font::SmallBold, theme::muted, Align::Right);
        gfx_.round(rect(ax + 18, ly + 20, half - 36, 4), 2, theme::raised);
        gfx_.round(rect(ax + 18, ly + 20, (half - 36) * (float)a.seconds / maxA, 4), 2, theme::accentDim);
        Id gid = a.gameId;
        addHit(row, [this, gid] { openGame(gid); });
        ly += 34;
    }
    if (!accN) gfx_.text(L"No accounts yet.", rect(ax + 18, ly, half - 36, 20), Font::Small, theme::muted);
    y += listH + 16;

    gfx_.text(L"Lifetime totals include historical hours you entered or imported; they have no dates, so day, week and "
              L"month figures count dated sessions only. Simultaneously running games each accumulate their own time, "
              L"so combined game-hours can exceed real elapsed time.",
              rect(x, y, w, 40), Font::Small, theme::faint, Align::Left, true);
    return y + 40;
}

// ---------------------------------------------------------------- settings

float MainWindow::paintSettings(float x, float y, float w) {
    auto& s = app_.settings;
    const float rowH = 58;
    auto cardWithRows = [&](const wchar_t* title, int rows, float extraH) {
        float h = 50 + rows * rowH + extraH;
        card(rect(x, y, w, h));
        sectionTitle(x + 20, y + 16, title);
        return y + 50;
    };
    auto rowText = [&](float ry, const std::wstring& title, const std::wstring& desc) {
        gfx_.text(title, rect(x + 20, ry + 8, w - 260, 20), Font::Body, theme::text);
        gfx_.text(desc, rect(x + 20, ry + 28, w - 260, 18), Font::Small, theme::muted);
    };
    auto flagRow = [&](float ry, const std::wstring& title, const std::wstring& desc, const wchar_t* key,
                       std::function<void(bool)> after = {}) {
        rowText(ry, title, desc);
        bool on = s.flag(key);
        toggle(x + w - 64, ry + 16, on, [this, key, on, after] {
            app_.settings.setFlag(key, !on);
            if (after) after(!on);
            app_.dataChanged();
        });
    };

    // General
    float ry = cardWithRows(L"General", 5, 6);
    rowText(ry, L"Launch with Windows", L"Starts quietly in the tray when you sign in.");
    {
        bool on = isLaunchAtStartup();
        toggle(x + w - 64, ry + 16, on, [this, on] { app_.setLaunchAtStartup(!on); });
    }
    ry += rowH;
    flagRow(ry, L"Start minimized", L"Open to the tray instead of showing this window when PlayTime starts.",
            setting::StartMinimized);
    ry += rowH;
    flagRow(ry, L"Minimize to tray", L"Minimizing hides the window and frees its memory; tracking continues.",
            setting::MinimizeToTray);
    ry += rowH;
    flagRow(ry, L"Close to tray", L"Closing the window keeps PlayTime tracking in the background. Exit from the tray menu.",
            setting::CloseToTray);
    ry += rowH;
    flagRow(ry, L"Show hours as decimals", L"\"4,250.5 h\" instead of \"4,250h 30m\".", setting::DecimalHours);
    y = ry + rowH + 6 + 18;

    // Tracking
    ry = cardWithRows(L"Tracking", 6, 60);
    rowText(ry, L"Tracking enabled", L"Detect running games and record sessions automatically.");
    {
        bool on = app_.trackingEnabled();
        toggle(x + w - 64, ry + 16, on, [this, on] { app_.setTrackingEnabled(!on); });
    }
    ry += rowH;
    flagRow(ry, L"Pause while Windows is locked", L"Stops counting when you lock the PC and resumes on unlock.",
            setting::PauseOnLock);
    ry += rowH;
    flagRow(ry, L"Create accounts for detected launcher logins",
            L"Epic logins (from the launch command line) and Steam logins become accounts automatically.",
            setting::AutoCreateAccounts, [this](bool on) { app_.tracker.setAutoCreateAccounts(on); });
    ry += rowH;
    flagRow(ry, L"Check for installed games at startup", L"Adds newly installed Steam, Epic, GOG, Ubisoft and Xbox games.",
            setting::ScanOnStartup);
    ry += rowH;
    rowText(ry, L"Process check interval",
            L"How often new processes are checked. Game exits are detected instantly regardless.");
    {
        static const int steps[] = {2, 3, 5, 10, 15, 30, 60};
        int cur = SettingsManager::clampPollInterval(s.integer(setting::PollIntervalSec));
        int idx = 0;
        while (idx < 6 && steps[idx] < cur) ++idx;
        float cx = x + w - 150;
        button(cx, ry + 12, L"", [this, idx] {
            app_.settings.setInt(setting::PollIntervalSec, steps[std::max(0, idx - 1)]);
            app_.applyPollInterval();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }, Btn::Secondary, L"\uE738", idx > 0);
        gfx_.text(std::to_wstring(cur) + L" s", rect(cx + 42, ry + 12, 52, 32), Font::BodyBold, theme::text, Align::Center);
        button(cx + 100, ry + 12, L"", [this, idx] {
            app_.settings.setInt(setting::PollIntervalSec, steps[std::min(6, idx + 1)]);
            app_.applyPollInterval();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }, Btn::Secondary, kIcoAdd, idx < 6);
    }
    ry += rowH;
    rowText(ry, L"Helper process exclusions", L"Launchers, updaters and crash reporters inside game folders are ignored.");
    buttonRight(x + w - 20, ry + 12, L"Edit exclusions", [this] { editGlobalExclusions(); });
    ry += rowH;
    float bx = x + 20;
    bx += button(bx, ry + 8, L"Scan for installed games now", [this] { scanLaunchers(); }, Btn::Secondary, kIcoRefresh) + 8;
    y = ry + 60 + 18;

    // Launcher import
    ry = cardWithRows(L"Import existing playtime", 0, 170);
    gfx_.text(L"Steam stores each signed-in user's lifetime playtime on this PC; importing it needs no password or "
              L"network. The Web API option uses Valve's official API with your own key. Imported totals are reconciled "
              L"against time PlayTime already tracked on that account, so re-importing never double counts.",
              rect(x + 20, ry, w - 40, 56), Font::Small, theme::muted, Align::Left, true);
    bx = x + 20;
    bx += button(bx, ry + 62, L"Import from Steam on this PC", [this] { importSteamLocal(); }, Btn::Primary) + 8;
    button(bx, ry + 62, L"Import via Steam Web API", [this] { importSteamWeb(); });
    gfx_.text(L"Epic Games offers no supported way to read playtime, so enter Epic hours with Adjust hours on each "
              L"account. Epic accounts are still told apart automatically while you play: the Epic launcher passes the "
              L"signed-in account id to the game. Games started directly (without a launcher) are recorded as "
              L"Unassigned unless you pick a current account.",
              rect(x + 20, ry + 104, w - 40, 56), Font::Small, theme::faint, Align::Left, true);
    y = ry + 170 + 18;

    // Data
    ry = cardWithRows(L"Data & backups", 0, 150);
    std::wstring dbPath = app_.db.path();
    int64_t last = s.integer(setting::LastBackupUtc);
    gfx_.text(L"Database: " + dbPath, rect(x + 20, ry, w - 40, 20), Font::Small, theme::muted);
    gfx_.text(L"Automatic daily backups (last 14 kept). Last backup: " +
                  (last ? formatDateTime(last) : std::wstring(L"never")),
              rect(x + 20, ry + 20, w - 40, 20), Font::Small, theme::muted);
    bx = x + 20;
    float by = ry + 52;
    bx += button(bx, by, L"Back up now", [this] { backupNow(); }, Btn::Secondary) + 8;
    bx += button(bx, by, L"Restore backup", [this] { restoreBackup(); }, Btn::Secondary) + 8;
    bx += button(bx, by, L"Open backups folder", [this] { openFolder(app_.backups->backupDir()); }, Btn::Ghost, kIcoFolder) + 8;
    button(bx, by, L"Open data folder", [this] { openFolder(app_.dataDir()); }, Btn::Ghost, kIcoFolder);
    bx = x + 20;
    by += 44;
    bx += button(bx, by, L"Export JSON", [this] { exportJson(); }) + 8;
    bx += button(bx, by, L"Export CSV", [this] { exportCsv(); }) + 8;
    button(bx, by, L"Import JSON", [this] { importJsonFile(); });
    y = ry + 150 + 18;

    // About
    ry = cardWithRows(L"About", 0, 44);
    gfx_.text(std::wstring(L"PlayTime ") + widen(PLAYTIME_VERSION) +
                  L"  \u00B7  Fully offline. No accounts, telemetry or analytics; your data never leaves this PC.",
              rect(x + 20, ry, w - 40, 20), Font::Small, theme::muted);
    y = ry + 44;
    return y;
}

}  // namespace pt
