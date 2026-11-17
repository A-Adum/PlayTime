#pragma once
#include <windows.h>

#include <memory>
#include <string>

#include "core/AccountManager.h"
#include "core/BackupManager.h"
#include "core/Database.h"
#include "core/GameDetector.h"
#include "core/GameLibrary.h"
#include "core/PlaytimeCalculator.h"
#include "core/SessionStore.h"
#include "core/SessionTracker.h"
#include "core/SettingsManager.h"
#include "core/StatisticsEngine.h"
#include "platform/ProcessMonitor.h"
#include "ui/TrayManager.h"

namespace pt {

class MainWindow;

// Owns the tracking engine (which runs on the main thread, driven by a hidden window's timer, power and
// session notifications, and process-exit handles) and the on-demand UI. Closing the window to the tray
// destroys it and releases all rendering resources; tracking continues.
class Application {
public:
    static constexpr UINT kMsgShowWindow = WM_APP + 10;  // posted by a second instance
    static constexpr const wchar_t* kEngineClass = L"PlayTime.Engine";

    Application();
    ~Application();
    static Application& instance() { return *instance_; }

    int run(HINSTANCE inst, bool startInTray);
    HINSTANCE hinstance() const { return hinst_; }
    HWND engineWindow() const { return engine_; }
    HICON appIcon(bool small) const { return small ? iconSmall_ : iconLarge_; }
    const std::wstring& dataDir() const { return dataDir_; }

    // Window management
    void showWindow();
    void closeWindow();  // destroys the UI; tracking keeps running
    bool windowOpen() const { return window_ != nullptr; }
    MainWindow* window() const { return window_.get(); }
    void requestExit();

    // Tracking control
    bool trackingEnabled() const;
    bool trackingActive() const { return trackingEnabled() && !lockPaused_; }
    bool lockPaused() const { return lockPaused_; }
    void setTrackingEnabled(bool enabled);
    void applyPollInterval();
    void startManualSession(Id gameId);
    void stopSession(Id gameId);
    // Sticky account choice for a game; also reassigns its running session.
    void setCurrentAccount(Id gameId, Id accountId);

    // Change notifications from the UI
    void detectionRulesChanged();  // executables / dirs / exclusions edited
    void libraryStructureChanging();  // call before deleting/merging games: ends sessions safely
    void libraryStructureChanged();   // and after
    void dataChanged();               // any data edit: refresh UI and tray

    DiscoveryApplyResult runDiscovery();
    void restoreBackup(const std::wstring& file);
    void importJson(const std::wstring& file);
    void setLaunchAtStartup(bool enable);

    // Engine state shared with the UI (single-threaded).
    Database db;
    SettingsManager settings{db};
    SessionStore sessions{db};
    GameLibrary library{db, sessions};
    AccountManager accounts{db, sessions};
    PlaytimeCalculator calc{db};
    StatisticsEngine stats{db, sessions};
    std::unique_ptr<BackupManager> backups;
    GameDetector detector;
    SystemClock clock;
    SessionTracker tracker{sessions, accounts, clock};
    ProcessMonitor monitor{detector, [this](Id g, const ProcessInfo& p) { tracker.processStarted(g, p); },
                           [this](Id g, uint32_t pid) { tracker.processExited(g, pid); }};
    TrayManager tray;

private:
    static LRESULT CALLBACK engineProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleEngine(UINT msg, WPARAM wp, LPARAM lp);
    bool initEngine(std::wstring& error);
    void messageLoop();
    void startPolling();
    void stopPolling();
    void pollNow();
    void pauseForSystem();   // sleep / lock
    void resumeFromSystem();
    void shutdownTracking();  // finalize everything (exit, logoff, shutdown)
    void onTrackerChanged();
    void updateTrayTip();
    void reloadAfterDatabaseSwap();

    static Application* instance_;
    HINSTANCE hinst_ = nullptr;
    HWND engine_ = nullptr;
    HICON iconLarge_ = nullptr, iconSmall_ = nullptr;
    std::wstring dataDir_;
    std::unique_ptr<MainWindow> window_;
    bool polling_ = false;
    bool lockPaused_ = false;
    bool suspended_ = false;
    bool exiting_ = false;
    bool finalized_ = false;
    HPOWERNOTIFY powerNotify_ = nullptr;
    UINT taskbarCreatedMsg_ = 0;
};

}  // namespace pt
