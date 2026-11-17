#include <windows.h>

#include <shellapi.h>

#include "app/Application.h"

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    bool tray = false;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i)
            if (_wcsicmp(argv[i], L"--tray") == 0 || _wcsicmp(argv[i], L"--minimized") == 0) tray = true;
        LocalFree(argv);
    }

    // Single instance per user session: a second launch just opens the existing window.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\PlayTime.SingleInstance.v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!tray) {
            if (HWND existing = FindWindowW(pt::Application::kEngineClass, nullptr)) {
                DWORD pid = 0;
                GetWindowThreadProcessId(existing, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(existing, pt::Application::kMsgShowWindow, 0, 0);
            }
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    // Background tracking should never compete with games for CPU or I/O.
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetProcessShutdownParameters(0x3FF, 0);  // be notified early at shutdown so sessions are closed cleanly

    int rc;
    {
        pt::Application app;
        rc = app.run(inst, tray);
    }
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return rc;
}
