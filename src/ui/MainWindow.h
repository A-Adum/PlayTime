#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "core/PlaytimeCalculator.h"
#include "core/StatisticsEngine.h"
#include "ui/Gfx.h"

namespace pt {

class Application;

enum class View { Dashboard, Library, Game, Stats, Settings };

// Custom-drawn (Direct2D) main window. Repaints only on input, data changes, and - while a session is
// running and the window is visible - once per second for the live counter.
class MainWindow {
public:
    static constexpr float kSidebarW = 212.f;
    explicit MainWindow(Application& app);
    ~MainWindow();
    bool create(bool show);
    HWND hwnd() const { return hwnd_; }
    void bringToFront();
    void refresh();  // reload data from the engine and repaint
    void openGame(Id gameId);
    void navigate(View v);

private:
    enum class Btn { Secondary, Primary, Ghost, Danger };
    struct Hit {
        D2D1_RECT_F r;
        std::function<void()> click;
        std::function<void()> rclick;
        std::wstring tip;
    };

    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void paint();
    void updateHover(float x, float y);
    void scrollBy(float dy);
    void updateLiveTimer();
    void layoutSearch();
    void reload();
    void run(const std::function<void()>& fn);  // runs a UI action, reporting errors

    // Drawing helpers (local coordinates; translated by ty_ while drawing scrolled content)
    void addHit(const D2D1_RECT_F& r, std::function<void()> click, std::wstring tip = {},
                std::function<void()> rclick = {});
    bool hovered(const D2D1_RECT_F& r) const;
    float button(float x, float y, const std::wstring& label, std::function<void()> fn, Btn style = Btn::Secondary,
                 const wchar_t* icon = nullptr, bool enabled = true);  // returns width
    float buttonWidth(const std::wstring& label, const wchar_t* icon) const;
    float buttonRight(float right, float y, const std::wstring& label, std::function<void()> fn,
                      Btn style = Btn::Secondary, const wchar_t* icon = nullptr);  // returns left edge
    void toggle(float x, float y, bool on, std::function<void()> fn);
    void card(const D2D1_RECT_F& r);
    float chip(float x, float y, const std::wstring& label, bool selected, std::function<void()> fn);
    void sectionTitle(float x, float y, const std::wstring& title);
    void barChart(const D2D1_RECT_F& r, const std::vector<Bucket>& data, int mode);  // 0 day, 1 week, 2 month
    float heatmap(float x, float y, float w, const std::vector<Bucket>& days);  // returns height
    void statTile(const D2D1_RECT_F& r, const std::wstring& label, const std::wstring& value,
                  const std::wstring& sub = {});
    void cover(const std::wstring& path, const std::wstring& name, const D2D1_RECT_F& r, float radius);
    void tooltip();

    // Views
    void paintSidebar(float h);
    float paintDashboard(float x, float y, float w);
    float paintLibrary(float x, float y, float w);
    float paintGame(float x, float y, float w);
    float paintStats(float x, float y, float w);
    float paintSettings(float x, float y, float w);
    void paintHeader(float x, float w);
    float gameRow(float x, float y, float w, const GameSummary& g);
    std::vector<const GameSummary*> sortedGames(bool filter) const;

    // Actions
    void showGameMenu(Id gameId);
    void showAccountMenu(const AccountTotals& a);
    void showSessionMenu(Id sessionId);
    void chooseCurrentAccount(Id gameId);
    void adjustHours(Id gameId, Id accountId);
    void addAccount(Id gameId);
    void addSession(Id gameId);
    void editSession(Id sessionId);
    void editGame(Id gameId);
    void mergeGame(Id gameId);
    void deleteGame(Id gameId);
    void addGameFromExe();
    void addCustomGame();
    void scanLaunchers();
    void importSteamLocal();
    void importSteamWeb();
    void editGlobalExclusions();
    void backupNow();
    void restoreBackup();
    void exportJson();
    void exportCsv();
    void importJsonFile();

    // Live helpers
    int64_t liveExtra(Id gameId, Id accountId = -1) const;  // unsaved running time
    bool decimal() const;
    std::wstring hours(int64_t seconds) const;

    Application& app_;
    HWND hwnd_ = nullptr;
    HWND search_ = nullptr;
    HFONT searchFont_ = nullptr;
    HBRUSH searchBrush_ = nullptr;
    Gfx gfx_;
    float scale_ = 1.f;
    float width_ = 0, height_ = 0;  // client size in DIPs

    View view_ = View::Dashboard;
    View prevView_ = View::Library;
    Id gameId_ = 0;
    int chartMode_ = 0;
    std::wstring sort_ = L"playtime";
    std::wstring filter_;

    std::vector<Hit> hits_;
    int hover_ = -1;
    float mx_ = -1, my_ = -1;
    float ty_ = 0;
    D2D1_RECT_F clip_{};
    float scroll_ = 0, contentH_ = 0, bodyTop_ = 0;
    bool liveTimer_ = false;
    bool tracking_ = false;  // mouse leave tracking

    // Cached data
    std::vector<GameSummary> games_;
    int64_t lifetime_ = 0, today_ = 0, week_ = 0, month_ = 0;
    std::vector<Bucket> heat_, daily30_, monthly12_;
    SessionStats allStats_;
    std::vector<AccountShare> topAccounts_;
    struct GameView {
        GameSummary summary;
        std::vector<AccountTotals> accounts;
        std::vector<Session> sessions;
        SessionStats stats;
        std::vector<Bucket> chart;
        std::vector<std::wstring> launchers;
    } gv_;
};

}  // namespace pt
