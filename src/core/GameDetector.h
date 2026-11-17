#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Model.h"

namespace pt {

class GameLibrary;

// Compiled detection rules. Pure matching logic, no OS calls.
//  1. Explicit executables (by full path, then by file name) always win.
//  2. Otherwise a process inside a game's install directory matches, unless its name/path matches a
//     global or per-game exclusion (launchers, updaters, crash handlers, redistributables...).
// The longest (most specific) install directory wins when directories nest.
class GameDetector {
public:
    void load(GameLibrary& library);
    void load(const std::vector<Executable>& exes, const std::vector<TrackingRule>& rules);

    // exeName and path must be normalized lowercase (see normalizePath). path may be empty when unknown.
    Id match(const std::wstring& exeName, const std::wstring& path) const;
    bool empty() const { return exes_.empty() && dirs_.empty(); }

private:
    static bool excluded(const std::vector<std::wstring>& patterns, const std::wstring& exeName,
                         const std::wstring& path);

    struct ExeRule {
        Id gameId;
        std::wstring fullPath;
    };
    std::unordered_map<std::wstring, std::vector<ExeRule>> exes_;
    std::vector<std::pair<std::wstring, Id>> dirs_;  // longest first
    std::vector<std::wstring> globalExclusions_;
    std::unordered_map<Id, std::vector<std::wstring>> gameExclusions_;
};

}  // namespace pt
