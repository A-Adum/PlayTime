#include "app/Application.h"

#include <commctrl.h>
#include <psapi.h>
#include <wtsapi32.h>

#include "platform/LauncherDiscovery.h"
#include "platform/WinUtil.h"
#include "ui/Dialogs.h"
#include "ui/MainWindow.h"
#include "util/Str.h"
#include "util/TimeUtil.h"

namespace pt {

Application* Application::instance_ = nullptr;

namespace {
constexpr UINT_PTR kPollTimer = 1;
constexpr UINT kMsgDestroyWindow = WM_APP + 11;
}  // namespace

Application::Application() { instance_ = this; }

Application::~Application() {
    window_.reset();
    instance_ = nullptr;
}

bool Application::trackingEnabled() const { return settings.flag(setting::TrackingEnabled); }

int Application::run(HINSTANCE inst, bool startInTray) {
    hinst_ = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    enableDarkMenus();
    iconLarge_ = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                   GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    iconSmall_ = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

    std::wstring error;
    if (!initEngine(error)) {
        MessageBoxW(nullptr, error.c_str(), L"PlayTime", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    if (!startInTray && !settings.flag(setting::StartMinimized)) showWindow();
    messageLoop();

    shutdownTracking();
    window_.reset();
    tray.remove();
    if (powerNotify_) UnregisterSuspendResumeNotification(powerNotify_);
    WTSUnRegisterSessionNotification(engine_);
    DestroyWindow(engine_);
    db.close();
    CoUninitialize();
    return 0;
}

bool Application::initEngine(std::wstring& error) {
    dataDir_ = appDataDir();
    backups = std::make_unique<BackupManager>(db, dataDir_ + L"\\backups");
    std::wstring notice;
    try {
        notice = BackupManager::openWithRecovery(db, dataDir_ + L"\\playtime.db", backups->backupDir());
        settings.load();
    } catch (const std::exception& e) {
        error = L"PlayTime could not open its database in\n" + dataDir_ + L"\n\n" + widen(e.what());
        return false;
    }
    tracker.setAutoCreateAccounts(settings.flag(setting::AutoCreateAccounts));
    tracker.onChange = [this] { onTrackerChanged(); };
    try {
        tracker.recover();
    } catch (const std::exception&) {
    }

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = engineProc;
    wc.hInstance = hinst_;
    wc.lpszClassName = kEngineClass;
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so that it receives power and session broadcasts.
    engine_ = CreateWindowExW(WS_EX_TOOLWINDOW, kEngineClass, L"PlayTime engine", WS_POPUP, 0, 0, 0, 0, nullptr,
                              nullptr, hinst_, this);
    if (!engine_) {
        error = L"Could not create the engine window: " + lastErrorMessage();
        return false;
    }
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(engine_, taskbarCreatedMsg_, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(engine_, kMsgShowWindow, MSGFLT_ALLOW, nullptr);
    powerNotify_ = RegisterSuspendResumeNotification(engine_, DEVICE_NOTIFY_WINDOW_HANDLE);
    WTSRegisterSessionNotification(engine_, NOTIFY_FOR_THIS_SESSION);
    tray.add(engine_, iconSmall_, L"PlayTime");

    try {
        if (!settings.flag(setting::FirstRunDone) || settings.flag(setting::ScanOnStartup)) {
            runDiscovery();
            settings.setFlag(setting::FirstRunDone, true);
        }
    } catch (const std::exception&) {
        // Discovery is best-effort; manual adding always works.
    }
    detector.load(library);
    if (trackingActive()) startPolling();
    tracker.clearRecoveryCandidates();
    try {
        backups->backupIfDue(settings, nowUtc());
    } catch (const std::exception&) {
    }
    // Keep the Run entry pointing at this executable if it moved.
    if (isLaunchAtStartup()) pt::setLaunchAtStartup(true);
    updateTrayTip();
    if (!notice.empty()) MessageBoxW(nullptr, notice.c_str(), L"PlayTime", MB_OK | MB_ICONWARNING);
    return true;
}

void Application::messageLoop() {
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    for (;;) {
        DWORD n = trackingActive() ? (DWORD)monitor.waitHandles(handles, MAXIMUM_WAIT_OBJECTS - 1) : 0;
        DWORD r = MsgWaitForMultipleObjectsEx(n, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r < WAIT_OBJECT_0 + n) {
            // A game process exited: event driven, no polling involved.
            monitor.handleSignaled(handles[r - WAIT_OBJECT_0]);
            continue;
        }
        if (r == WAIT_FAILED) {
            // Should not happen (handles are owned by the monitor); resynchronise via a snapshot.
            monitor.reset();
            pollNow();
            MsgWaitForMultipleObjectsEx(0, nullptr, 1000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

void Application::startPolling() {
    UINT ms = (UINT)SettingsManager::clampPollInterval(settings.integer(setting::PollIntervalSec)) * 1000;
    // Coalescable: lets Windows batch our wake-ups with others for lower power use.
    SetCoalescableTimer(engine_, kPollTimer, ms, nullptr, std::min<ULONG>(ms / 10, 1000));
    polling_ = true;
    pollNow();
}

void Application::stopPolling() {
    if (polling_) KillTimer(engine_, kPollTimer);
    polling_ = false;
}

void Application::applyPollInterval() {
    if (polling_) startPolling();
}

void Application::pollNow() {
    if (!trackingActive()) return;
    try {
        monitor.scan();
        tracker.tick();
    } catch (const std::exception&) {
        // A transient database error must not stop tracking; the next poll retries.
    }
}

void Application::pauseForSystem() {
    try {
        tracker.stopAll();
    } catch (const std::exception&) {
    }
    monitor.reset();
    stopPolling();
    dataChanged();
}

void Application::resumeFromSystem() {
    monitor.reset();
    if (trackingActive()) startPolling();
    dataChanged();
}

void Application::shutdownTracking() {
    if (finalized_) return;
    finalized_ = true;
    try {
        tracker.stopAll();
    } catch (const std::exception&) {
    }
    monitor.reset();
    stopPolling();
}

void Application::setTrackingEnabled(bool enabled) {
    settings.setFlag(setting::TrackingEnabled, enabled);
    if (!enabled) {
        tracker.stopAll();
        monitor.reset();
        stopPolling();
    } else if (trackingActive()) {
        monitor.reset();
        startPolling();
    }
    dataChanged();
}

void Application::startManualSession(Id gameId) {
    if (!trackingActive()) throw std::runtime_error("Tracking is paused. Resume tracking to start a session.");
    if (!tracker.startManual(gameId, 0)) throw std::runtime_error("This game already has a running session.");
}

void Application::stopSession(Id gameId) { tracker.stop(gameId); }

void Application::setCurrentAccount(Id gameId, Id accountId) {
    accounts.setPreferredAccount(gameId, accountId);
    if (accountId && tracker.find(gameId)) tracker.setSessionAccount(gameId, accountId);
    dataChanged();
}

void Application::detectionRulesChanged() {
    detector.load(library);
    monitor.rulesChanged();
    pollNow();
    dataChanged();
}

void Application::libraryStructureChanging() {
    tracker.stopAll();
    monitor.reset();
}

void Application::libraryStructureChanged() {
    detector.load(library);
    monitor.reset();
    pollNow();
    dataChanged();
}

void Application::dataChanged() {
    updateTrayTip();
    if (window_) window_->refresh();
}

void Application::onTrackerChanged() {
    updateTrayTip();
    if (window_) window_->refresh();
}

void Application::updateTrayTip() {
    std::wstring tip = L"PlayTime";
    if (!trackingActive()) tip += lockPaused_ ? L" \u2014 paused while locked" : L" \u2014 tracking paused";
    auto& active = tracker.active();
    for (auto& s : active) {
        auto g = library.game(s.gameId);
        if (!g) continue;
        tip += L"\n" + g->name + L" since " + formatTime(s.startUtc);
    }
    if (active.empty() && trackingActive()) tip += L" \u2014 no game running";
    tray.setTip(tip);
}

DiscoveryApplyResult Application::runDiscovery() {
    auto found = discoverInstalledGames();
    auto result = library.applyDiscovery(found);
    detector.load(library);
    monitor.rulesChanged();
    if (result.added || result.linked || result.updated) {
        pollNow();
        dataChanged();
    }
    return result;
}

void Application::reloadAfterDatabaseSwap() {
    settings.load();
    tracker.setAutoCreateAccounts(settings.flag(setting::AutoCreateAccounts));
    try {
        tracker.recover();
        tracker.clearRecoveryCandidates();
    } catch (const std::exception&) {
    }
    detector.load(library);
    monitor.reset();
    if (trackingActive()) startPolling();
    else stopPolling();
    dataChanged();
}

void Application::restoreBackup(const std::wstring& file) {
    libraryStructureChanging();
    try {
        backups->restore(file);
    } catch (...) {
        if (!db.isOpen()) BackupManager::openWithRecovery(db, dataDir_ + L"\\playtime.db", backups->backupDir());
        reloadAfterDatabaseSwap();
        throw;
    }
    reloadAfterDatabaseSwap();
}

void Application::importJson(const std::wstring& file) {
    libraryStructureChanging();
    try {
        backups->importJson(file);
    } catch (...) {
        reloadAfterDatabaseSwap();
        throw;
    }
    reloadAfterDatabaseSwap();
}

void Application::setLaunchAtStartup(bool enable) {
    if (!pt::setLaunchAtStartup(enable)) throw std::runtime_error("Could not update the Windows startup entry.");
    settings.setFlag(setting::LaunchWithWindows, enable);
    dataChanged();
}

void Application::showWindow() {
    if (window_) {
        window_->bringToFront();
        return;
    }
    window_ = std::make_unique<MainWindow>(*this);
    if (!window_->create(true)) {
        window_.reset();
        MessageBoxW(nullptr, L"PlayTime could not create its window (Direct2D unavailable).", L"PlayTime",
                    MB_OK | MB_ICONERROR);
    }
}

void Application::closeWindow() {
    if (!window_) return;
    ShowWindow(window_->hwnd(), SW_HIDE);
    // Destroy outside the window's own message handler.
    PostMessageW(engine_, kMsgDestroyWindow, 0, 0);
}

void Application::requestExit() {
    exiting_ = true;
    shutdownTracking();
    if (window_) ShowWindow(window_->hwnd(), SW_HIDE);  // destroyed after the message loop ends
    PostQuitMessage(0);
}

// ---------------------------------------------------------------- engine window

LRESULT CALLBACK Application::engineProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app || !app->engine_) return DefWindowProcW(hwnd, msg, wp, lp);
    return app->handleEngine(msg, wp, lp);
}

LRESULT Application::handleEngine(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_) {
        tray.readd();
        return 0;
    }
    switch (msg) {
        case WM_TIMER:
            if (wp == kPollTimer) pollNow();
            return 0;
        case TrayManager::kCallbackMsg:
            try {
                tray.handleCallback(*this, wp, lp);
            } catch (const std::exception& e) {
                showError(window_ ? window_->hwnd() : nullptr, widen(e.what()));
            }
            return 0;
        case kMsgShowWindow:
            showWindow();
            return 0;
        case kMsgDestroyWindow:
            if (window_ && !IsWindowVisible(window_->hwnd())) {
                window_.reset();
                // Release the UI's memory (Direct2D, images, fonts) back to the OS while we sit in the tray.
                CoFreeUnusedLibrariesEx(0, 0);
                HeapCompact(GetProcessHeap(), 0);
                SetProcessWorkingSetSizeEx(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1, 0);
            }
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMSUSPEND) {
                // Sleep/hibernate: end sessions now so suspended time is never counted.
                suspended_ = true;
                pauseForSystem();
            } else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                if (suspended_) {
                    suspended_ = false;
                    resumeFromSystem();
                }
            }
            return TRUE;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK && settings.flag(setting::PauseOnLock) && !lockPaused_) {
                lockPaused_ = true;
                pauseForSystem();
            } else if (wp == WTS_SESSION_UNLOCK && lockPaused_) {
                lockPaused_ = false;
                resumeFromSystem();
            }
            return 0;
        case WM_QUERYENDSESSION:
            try {
                tracker.checkpoint();
            } catch (const std::exception&) {
            }
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                // Windows is shutting down or signing out: sessions end now.
                shutdownTracking();
                db.close();
            }
            return 0;
    }
    return DefWindowProcW(engine_, msg, wp, lp);
}

}  // namespace pt
