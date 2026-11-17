#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/GameDetector.h"
#include "core/SessionTracker.h"

namespace pt {

// Detects game processes without injecting into or reading the memory of any process.
//  - Starts: a Toolhelp snapshot diff at a low, timer-driven interval (unprivileged event sources for
//    process creation either need admin rights (ETW/Win32_ProcessStartTrace) or poll internally anyway
//    (WMI __InstanceCreationEvent) while costing far more memory). Only new PIDs are inspected.
//  - Exits: event driven. Each matched process handle (SYNCHRONIZE) is waited on by the main message loop
//    via MsgWaitForMultipleObjectsEx; the snapshot diff is the fallback when a handle cannot be opened.
//  - Account identity: Epic's launcher passes -epicuserid/-epicusername on the game's command line, read with
//    the documented-by-Windows ProcessCommandLineInformation query (no memory reads). The game process and up
//    to two parents are checked. Steam games (under a steamapps folder) use Steam's ActiveUser registry value.
class ProcessMonitor {
public:
    using StartFn = std::function<void(Id gameId, const ProcessInfo&)>;
    using ExitFn = std::function<void(Id gameId, uint32_t pid)>;

    ProcessMonitor(GameDetector& detector, StartFn onStart, ExitFn onExit)
        : detector_(detector), onStart_(std::move(onStart)), onExit_(std::move(onExit)) {}
    ~ProcessMonitor() { reset(); }

    void scan();
    // Forgets all processes without reporting exits (used after the tracker stopped all sessions).
    void reset();
    // Re-evaluates processes that did not match before (after rules changed). Tracked ones are kept.
    void rulesChanged();

    // Handles for the message loop to wait on (at most `max`).
    size_t waitHandles(HANDLE* out, size_t max) const;
    void handleSignaled(HANDLE h);

private:
    struct Entry {
        std::wstring exeName;
        Id gameId = 0;
        HANDLE handle = nullptr;
    };
    void evaluate(DWORD pid, DWORD parentPid, const std::wstring& exeName, Entry& e,
                  const std::unordered_map<DWORD, DWORD>& parents);
    void exited(DWORD pid);

    GameDetector& detector_;
    StartFn onStart_;
    ExitFn onExit_;
    std::unordered_map<DWORD, Entry> seen_;
};

// Reads another process's command line via NtQueryInformationProcess(ProcessCommandLineInformation).
// Requires only PROCESS_QUERY_LIMITED_INFORMATION; empty on failure.
std::wstring processCommandLine(HANDLE process);

}  // namespace pt
