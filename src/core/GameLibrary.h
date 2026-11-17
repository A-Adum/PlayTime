#pragma once
#include <optional>
#include <string>
#include <vector>

#include "core/Database.h"
#include "core/SessionStore.h"

namespace pt {

struct DiscoveredGame;

struct DiscoveryApplyResult {
    int added = 0;    // new games created
    int linked = 0;   // new install paths attached to existing games (e.g. Steam + Epic copy)
    int updated = 0;  // known install paths that moved
    std::vector<std::wstring> addedNames;
};

// Games, their detection rules (executables, install dirs, exclusions) and aliases.
class GameLibrary {
public:
    GameLibrary(Database& db, SessionStore& sessions) : db_(db), sessions_(sessions) {}

    std::vector<Game> games();
    std::optional<Game> game(Id id);
    Id addGame(const std::wstring& name, const std::wstring& coverPath = {});
    void renameGame(Id id, const std::wstring& name);
    void setCover(Id id, const std::wstring& path);
    // Removes the game and all its data; discovery will not re-add its launcher entries.
    void deleteGame(Id id);

    std::vector<std::wstring> aliases(Id gameId);
    void setAliases(Id gameId, const std::vector<std::wstring>& aliases);

    std::vector<Executable> executables(Id gameId);
    std::vector<Executable> allExecutables();
    // Accepts a file name ("game.exe") or a full path (constrains matching to that exact file).
    void addExecutable(Id gameId, const std::wstring& nameOrPath);
    void setExecutables(Id gameId, const std::vector<std::wstring>& namesOrPaths);

    std::vector<TrackingRule> rules(Id gameId);  // gameId 0 = global rules
    std::vector<TrackingRule> allRules();
    Id addInstallDir(Id gameId, const std::wstring& dir, const std::wstring& launcher = {},
                     const std::wstring& appId = {});
    // Replaces install dirs (keeping launcher metadata for dirs that remain) and per-game exclusions.
    void setInstallDirs(Id gameId, const std::vector<std::wstring>& dirs);
    void setExclusions(Id gameId, const std::vector<std::wstring>& patterns);  // gameId 0 = global
    std::vector<std::wstring> exclusions(Id gameId);

    // Moves accounts, sessions, baselines, rules and aliases from `source` into `target`, then deletes
    // `source`. Overlapping sessions are coalesced so shared time is not double-counted.
    void mergeGames(Id source, Id target);

    std::optional<Id> findByName(const std::wstring& name);  // normalized match on names and aliases
    std::optional<Id> findByLauncherApp(const std::wstring& launcher, const std::wstring& appId);
    std::vector<std::pair<std::wstring, Id>> launcherApps(const std::wstring& launcher);  // appId -> game
    bool isIgnored(const std::wstring& launcher, const std::wstring& appId);

    DiscoveryApplyResult applyDiscovery(const std::vector<DiscoveredGame>& found);

private:
    Database& db_;
    SessionStore& sessions_;
};

}  // namespace pt
