#include "platform/ProcessMonitor.h"

#include <tlhelp32.h>
#include <winternl.h>

#include <unordered_set>

#include "core/AccountManager.h"
#include "platform/SteamImport.h"
#include "util/Str.h"

namespace pt {

namespace {
constexpr ULONG kProcessCommandLineInformation = 60;  // PROCESSINFOCLASS, Windows 8.1+
using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

NtQueryInformationProcessFn ntQuery() {
    static auto fn = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    return fn;
}

std::wstring imagePath(HANDLE h) {
    wchar_t buf[MAX_PATH * 2];
    DWORD len = (DWORD)std::size(buf);
    if (!QueryFullProcessImageNameW(h, 0, buf, &len)) return {};
    return std::wstring(buf, len);
}

int64_t creationTime(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(h, &c, &e, &k, &u)) return 0;
    return (int64_t)(((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime);
}

Identity epicIdentityFromAncestors(HANDLE self, DWORD pid, int64_t created,
                                   const std::unordered_map<DWORD, DWORD>& parents) {
    if (self) {
        auto id = AccountManager::epicIdentityFromCommandLine(processCommandLine(self));
        if (id.valid()) return id;
    }
    DWORD current = pid;
    int64_t childCreated = created;
    for (int depth = 0; depth < 2; ++depth) {
        auto it = parents.find(current);
        if (it == parents.end() || it->second == 0 || it->second == current) break;
        DWORD parent = it->second;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
        if (!h) break;
        int64_t parentCreated = creationTime(h);
        // A parent created after the child means the PID was reused by an unrelated process.
        bool valid = parentCreated != 0 && (childCreated == 0 || parentCreated <= childCreated);
        Identity id;
        if (valid) id = AccountManager::epicIdentityFromCommandLine(processCommandLine(h));
        CloseHandle(h);
        if (!valid) break;
        if (id.valid()) return id;
        current = parent;
        childCreated = parentCreated;
    }
    return {};
}
}  // namespace

std::wstring processCommandLine(HANDLE process) {
    auto fn = ntQuery();
    if (!fn || !process) return {};
    ULONG needed = 0;
    fn(process, kProcessCommandLineInformation, nullptr, 0, &needed);
    if (needed < sizeof(UNICODE_STRING) || needed > 1u << 20) return {};
    std::vector<BYTE> buf(needed);
    if (fn(process, kProcessCommandLineInformation, buf.data(), needed, &needed) < 0) return {};
    auto* us = reinterpret_cast<UNICODE_STRING*>(buf.data());
    if (!us->Buffer || us->Length == 0) return {};
    return std::wstring(us->Buffer, us->Length / sizeof(wchar_t));
}

void ProcessMonitor::scan() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    struct Proc {
        DWORD pid, parent;
        std::wstring name;
    };
    std::vector<Proc> procs;
    procs.reserve(seen_.size() + 32);
    std::unordered_map<DWORD, DWORD> parents;
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID <= 4) continue;  // Idle / System
        parents.emplace(pe.th32ProcessID, pe.th32ParentProcessID);
        procs.push_back({pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile});
    }
    CloseHandle(snap);

    // Exits first (covers processes whose handle could not be opened, and modal-loop periods).
    std::unordered_set<DWORD> alive;
    alive.reserve(procs.size());
    for (auto& p : procs) alive.insert(p.pid);
    std::vector<DWORD> gone;
    for (auto& [pid, e] : seen_)
        if (!alive.count(pid)) gone.push_back(pid);
    for (DWORD pid : gone) exited(pid);

    for (auto& p : procs) {
        auto it = seen_.find(p.pid);
        if (it != seen_.end()) {
            if (_wcsicmp(it->second.exeName.c_str(), p.name.c_str()) == 0) continue;
            exited(p.pid);  // PID reused by a different program
        }
        Entry& e = seen_[p.pid];
        e.exeName = toLower(p.name);
        evaluate(p.pid, p.parent, e.exeName, e, parents);
    }
}

void ProcessMonitor::evaluate(DWORD pid, DWORD, const std::wstring& exeName, Entry& e,
                              const std::unordered_map<DWORD, DWORD>& parents) {
    if (detector_.empty()) return;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    bool canWait = h != nullptr;
    if (!h) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    std::wstring path = h ? normalizePath(imagePath(h)) : std::wstring();
    Id gameId = detector_.match(exeName, path);
    if (!gameId) {
        if (h) CloseHandle(h);
        return;
    }
    ProcessInfo info;
    info.pid = pid;
    info.exeName = exeName;
    info.path = path;
    info.created = h ? creationTime(h) : 0;
    info.identity = epicIdentityFromAncestors(h, pid, info.created, parents);
    if (!info.identity.valid() && path.find(L"\\steamapps\\") != std::wstring::npos) {
        if (auto user = activeSteamUser()) info.identity = steamIdentity(*user);
    }
    e.gameId = gameId;
    if (canWait) {
        e.handle = h;
    } else if (h) {
        CloseHandle(h);  // exit detection falls back to the snapshot diff
    }
    onStart_(gameId, info);
}

void ProcessMonitor::exited(DWORD pid) {
    auto it = seen_.find(pid);
    if (it == seen_.end()) return;
    Entry e = std::move(it->second);
    seen_.erase(it);
    if (e.handle) CloseHandle(e.handle);
    if (e.gameId) onExit_(e.gameId, pid);
}

void ProcessMonitor::reset() {
    for (auto& [pid, e] : seen_)
        if (e.handle) CloseHandle(e.handle);
    seen_.clear();
}

void ProcessMonitor::rulesChanged() {
    for (auto it = seen_.begin(); it != seen_.end();) {
        if (it->second.gameId == 0) it = seen_.erase(it);
        else ++it;
    }
}

size_t ProcessMonitor::waitHandles(HANDLE* out, size_t max) const {
    size_t n = 0;
    for (auto& [pid, e] : seen_)
        if (e.handle && n < max) out[n++] = e.handle;
    return n;
}

void ProcessMonitor::handleSignaled(HANDLE h) {
    for (auto& [pid, e] : seen_)
        if (e.handle == h) {
            exited(pid);
            return;
        }
}

}  // namespace pt
