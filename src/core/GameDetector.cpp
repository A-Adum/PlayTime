#include "core/GameDetector.h"

#include <algorithm>

#include "core/GameLibrary.h"
#include "util/Str.h"

namespace pt {

void GameDetector::load(GameLibrary& library) { load(library.allExecutables(), library.allRules()); }

void GameDetector::load(const std::vector<Executable>& exes, const std::vector<TrackingRule>& rules) {
    exes_.clear();
    dirs_.clear();
    globalExclusions_.clear();
    gameExclusions_.clear();
    for (auto& e : exes) exes_[toLower(e.exeName)].push_back({e.gameId, normalizePath(e.fullPath)});
    for (auto& r : rules) {
        if (r.kind == RuleKind::InstallDir) {
            if (r.gameId) dirs_.emplace_back(normalizePath(r.pattern), r.gameId);
        } else if (r.gameId == 0) {
            globalExclusions_.push_back(toLower(r.pattern));
        } else {
            gameExclusions_[r.gameId].push_back(toLower(r.pattern));
        }
    }
    std::sort(dirs_.begin(), dirs_.end(), [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
}

bool GameDetector::excluded(const std::vector<std::wstring>& patterns, const std::wstring& exeName,
                            const std::wstring& path) {
    for (auto& p : patterns) {
        bool pathPattern = p.find(L'\\') != std::wstring::npos;
        if (pathPattern ? (!path.empty() && globMatch(p, path)) : globMatch(p, exeName)) return true;
    }
    return false;
}

Id GameDetector::match(const std::wstring& exeName, const std::wstring& path) const {
    if (auto it = exes_.find(exeName); it != exes_.end()) {
        Id nameOnly = 0;
        for (auto& r : it->second) {
            if (!r.fullPath.empty()) {
                if (!path.empty() && r.fullPath == path) return r.gameId;
            } else if (!nameOnly) {
                nameOnly = r.gameId;
            }
        }
        if (nameOnly) return nameOnly;
    }
    if (path.empty()) return 0;
    for (auto& [dir, gameId] : dirs_) {
        if (!pathInside(path, dir)) continue;
        if (excluded(globalExclusions_, exeName, path)) return 0;
        if (auto g = gameExclusions_.find(gameId); g != gameExclusions_.end() && excluded(g->second, exeName, path))
            return 0;
        return gameId;
    }
    return 0;
}

}  // namespace pt
