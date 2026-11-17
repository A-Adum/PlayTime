#include "core/GameLibrary.h"

#include <algorithm>
#include <set>

#include "util/Str.h"
#include "util/TimeUtil.h"

namespace pt {

namespace {
Game readGame(Stmt& st) {
    Game g;
    g.id = st.i64(0);
    g.name = st.str(1);
    g.coverPath = st.str(2);
    g.createdAt = st.i64(3);
    g.preferredAccount = st.isNull(4) ? 0 : st.i64(4);
    return g;
}

TrackingRule readRule(Stmt& st) {
    TrackingRule r;
    r.id = st.i64(0);
    r.gameId = st.isNull(1) ? 0 : st.i64(1);
    r.kind = st.str(2) == L"exclude" ? RuleKind::Exclude : RuleKind::InstallDir;
    r.pattern = st.str(3);
    r.launcher = st.str(4);
    r.appId = st.str(5);
    return r;
}
}  // namespace

std::vector<Game> GameLibrary::games() {
    std::vector<Game> out;
    auto& st = db_.prepare(
        "SELECT id, name, cover_path, created_at, preferred_account_id FROM games ORDER BY name COLLATE NOCASE");
    while (st.step()) out.push_back(readGame(st));
    st.reset();
    return out;
}

std::optional<Game> GameLibrary::game(Id id) {
    auto& st = db_.prepare("SELECT id, name, cover_path, created_at, preferred_account_id FROM games WHERE id = ?");
    st.bind(1, id);
    std::optional<Game> out;
    if (st.step()) out = readGame(st);
    st.reset();
    return out;
}

Id GameLibrary::addGame(const std::wstring& name, const std::wstring& coverPath) {
    auto n = trim(name);
    if (n.empty()) throw std::invalid_argument("Game name cannot be empty");
    db_.prepare("INSERT INTO games(name, cover_path, created_at) VALUES(?, ?, ?)")
        .bind(1, n).bindOptText(2, coverPath).bind(3, nowUtc()).run();
    return db_.lastInsertId();
}

void GameLibrary::renameGame(Id id, const std::wstring& name) {
    auto n = trim(name);
    if (n.empty()) throw std::invalid_argument("Game name cannot be empty");
    db_.prepare("UPDATE games SET name = ? WHERE id = ?").bind(1, n).bind(2, id).run();
}

void GameLibrary::setCover(Id id, const std::wstring& path) {
    db_.prepare("UPDATE games SET cover_path = ? WHERE id = ?").bindOptText(1, trim(path)).bind(2, id).run();
}

void GameLibrary::deleteGame(Id id) {
    Database::Tx tx(db_);
    for (auto& r : rules(id))
        if (r.kind == RuleKind::InstallDir && !r.launcher.empty() && !r.appId.empty())
            db_.prepare("INSERT OR IGNORE INTO discovery_ignores(launcher, app_id) VALUES(?, ?)")
                .bind(1, r.launcher).bind(2, r.appId).run();
    db_.prepare("DELETE FROM games WHERE id = ?").bind(1, id).run();
    tx.commit();
}

std::vector<std::wstring> GameLibrary::aliases(Id gameId) {
    std::vector<std::wstring> out;
    auto& st = db_.prepare("SELECT alias FROM game_aliases WHERE game_id = ? ORDER BY alias");
    st.bind(1, gameId);
    while (st.step()) out.push_back(st.str(0));
    st.reset();
    return out;
}

void GameLibrary::setAliases(Id gameId, const std::vector<std::wstring>& list) {
    Database::Tx tx(db_);
    db_.prepare("DELETE FROM game_aliases WHERE game_id = ?").bind(1, gameId).run();
    for (auto& a : list) {
        auto t = trim(a);
        if (!t.empty())
            db_.prepare("INSERT OR IGNORE INTO game_aliases(game_id, alias) VALUES(?, ?)").bind(1, gameId).bind(2, t).run();
    }
    tx.commit();
}

std::vector<Executable> GameLibrary::executables(Id gameId) {
    std::vector<Executable> out;
    auto& st = db_.prepare("SELECT id, game_id, exe_name, full_path FROM game_executables WHERE game_id = ? ORDER BY id");
    st.bind(1, gameId);
    while (st.step()) out.push_back({st.i64(0), st.i64(1), st.str(2), st.str(3)});
    st.reset();
    return out;
}

std::vector<Executable> GameLibrary::allExecutables() {
    std::vector<Executable> out;
    auto& st = db_.prepare("SELECT id, game_id, exe_name, full_path FROM game_executables");
    while (st.step()) out.push_back({st.i64(0), st.i64(1), st.str(2), st.str(3)});
    st.reset();
    return out;
}

void GameLibrary::addExecutable(Id gameId, const std::wstring& nameOrPath) {
    auto t = trim(nameOrPath);
    if (t.empty()) return;
    bool isPath = t.find_first_of(L"\\/") != std::wstring::npos;
    std::wstring full = isPath ? normalizePath(t) : std::wstring();
    std::wstring name = toLower(fileNameOf(t));
    if (name.empty()) return;
    for (auto& e : executables(gameId))
        if (e.exeName == name && e.fullPath == full) return;
    db_.prepare("INSERT INTO game_executables(game_id, exe_name, full_path) VALUES(?, ?, ?)")
        .bind(1, gameId).bind(2, name).bindOptText(3, full).run();
}

void GameLibrary::setExecutables(Id gameId, const std::vector<std::wstring>& list) {
    Database::Tx tx(db_);
    db_.prepare("DELETE FROM game_executables WHERE game_id = ?").bind(1, gameId).run();
    for (auto& e : list) addExecutable(gameId, e);
    tx.commit();
}

std::vector<TrackingRule> GameLibrary::rules(Id gameId) {
    std::vector<TrackingRule> out;
    auto& st = db_.prepare("SELECT id, game_id, kind, pattern, launcher, launcher_app_id FROM tracking_rules"
                           " WHERE IFNULL(game_id, 0) = ? ORDER BY id");
    st.bind(1, gameId);
    while (st.step()) out.push_back(readRule(st));
    st.reset();
    return out;
}

std::vector<TrackingRule> GameLibrary::allRules() {
    std::vector<TrackingRule> out;
    auto& st = db_.prepare("SELECT id, game_id, kind, pattern, launcher, launcher_app_id FROM tracking_rules");
    while (st.step()) out.push_back(readRule(st));
    st.reset();
    return out;
}

Id GameLibrary::addInstallDir(Id gameId, const std::wstring& dir, const std::wstring& launcher,
                              const std::wstring& appId) {
    auto norm = normalizePath(dir);
    if (norm.size() < 4) throw std::invalid_argument("Install directory is too broad");  // refuse "c:\"
    for (auto& r : rules(gameId))
        if (r.kind == RuleKind::InstallDir && r.pattern == norm) return r.id;
    auto& st = db_.prepare(
        "INSERT INTO tracking_rules(game_id, kind, pattern, launcher, launcher_app_id) VALUES(?, 'install_dir', ?, ?, ?)");
    st.bind(1, gameId).bind(2, norm).bindOptText(3, launcher).bindOptText(4, appId).run();
    return db_.lastInsertId();
}

void GameLibrary::setInstallDirs(Id gameId, const std::vector<std::wstring>& dirs) {
    Database::Tx tx(db_);
    std::set<std::wstring> wanted;
    for (auto& d : dirs) {
        auto n = normalizePath(d);
        if (!n.empty()) wanted.insert(n);
    }
    for (auto& r : rules(gameId)) {
        if (r.kind != RuleKind::InstallDir) continue;
        if (wanted.erase(r.pattern) == 0)
            db_.prepare("DELETE FROM tracking_rules WHERE id = ?").bind(1, r.id).run();
    }
    for (auto& d : wanted) addInstallDir(gameId, d);
    tx.commit();
}

std::vector<std::wstring> GameLibrary::exclusions(Id gameId) {
    std::vector<std::wstring> out;
    for (auto& r : rules(gameId))
        if (r.kind == RuleKind::Exclude) out.push_back(r.pattern);
    return out;
}

void GameLibrary::setExclusions(Id gameId, const std::vector<std::wstring>& patterns) {
    Database::Tx tx(db_);
    db_.prepare("DELETE FROM tracking_rules WHERE IFNULL(game_id, 0) = ? AND kind = 'exclude'").bind(1, gameId).run();
    std::set<std::wstring> seen;
    for (auto& p : patterns) {
        auto t = toLower(trim(p));
        if (t.empty() || !seen.insert(t).second) continue;
        auto& st = db_.prepare("INSERT INTO tracking_rules(game_id, kind, pattern) VALUES(?, 'exclude', ?)");
        if (gameId) st.bind(1, gameId); else st.bindNull(1);
        st.bind(2, t).run();
    }
    tx.commit();
}

void GameLibrary::mergeGames(Id source, Id target) {
    if (source == target) throw std::invalid_argument("Cannot merge a game into itself");
    auto src = game(source);
    auto dst = game(target);
    if (!src || !dst) throw std::invalid_argument("Game not found");

    Database::Tx tx(db_);
    // Unassigned baselines are combined; per-account baselines follow their accounts.
    int64_t srcUnassigned = 0;
    {
        auto& st = db_.prepare("SELECT seconds FROM playtime_baselines WHERE game_id = ? AND account_id IS NULL");
        st.bind(1, source);
        if (st.step()) srcUnassigned = st.i64(0);
        st.reset();
    }
    if (srcUnassigned != 0) {
        auto& st = db_.prepare("SELECT id FROM playtime_baselines WHERE game_id = ? AND account_id IS NULL");
        st.bind(1, target);
        bool exists = st.step();
        Id baseId = exists ? st.i64(0) : 0;
        st.reset();
        if (exists)
            db_.prepare("UPDATE playtime_baselines SET seconds = seconds + ?, source = 'manual', imported_total = NULL,"
                        " updated_at = ? WHERE id = ?")
                .bind(1, srcUnassigned).bind(2, nowUtc()).bind(3, baseId).run();
        else
            db_.prepare("INSERT INTO playtime_baselines(game_id, account_id, seconds, source, updated_at)"
                        " VALUES(?, NULL, ?, 'manual', ?)")
                .bind(1, target).bind(2, srcUnassigned).bind(3, nowUtc()).run();
        db_.prepare("DELETE FROM playtime_baselines WHERE game_id = ? AND account_id IS NULL").bind(1, source).run();
    }
    db_.prepare("UPDATE playtime_baselines SET game_id = ? WHERE game_id = ?").bind(1, target).bind(2, source).run();
    db_.prepare("UPDATE accounts SET game_id = ? WHERE game_id = ?").bind(1, target).bind(2, source).run();
    db_.prepare("UPDATE sessions SET game_id = ? WHERE game_id = ?").bind(1, target).bind(2, source).run();
    db_.prepare("UPDATE OR IGNORE game_executables SET game_id = ? WHERE game_id = ?").bind(1, target).bind(2, source).run();
    db_.prepare("UPDATE tracking_rules SET game_id = ? WHERE game_id = ?").bind(1, target).bind(2, source).run();
    db_.prepare("UPDATE OR IGNORE game_aliases SET game_id = ? WHERE game_id = ?").bind(1, target).bind(2, source).run();
    if (normalizeName(src->name) != normalizeName(dst->name))
        db_.prepare("INSERT OR IGNORE INTO game_aliases(game_id, alias) VALUES(?, ?)").bind(1, target).bind(2, src->name).run();
    if (dst->coverPath.empty() && !src->coverPath.empty()) setCover(target, src->coverPath);
    db_.prepare("DELETE FROM games WHERE id = ?").bind(1, source).run();
    sessions_.coalesceOverlaps(target);
    tx.commit();
}

std::optional<Id> GameLibrary::findByName(const std::wstring& name) {
    auto key = normalizeName(name);
    if (key.empty()) return std::nullopt;
    for (auto& g : games())
        if (normalizeName(g.name) == key) return g.id;
    auto& st = db_.prepare("SELECT game_id, alias FROM game_aliases");
    std::optional<Id> out;
    while (st.step())
        if (normalizeName(st.str(1)) == key) {
            out = st.i64(0);
            break;
        }
    st.reset();
    return out;
}

std::optional<Id> GameLibrary::findByLauncherApp(const std::wstring& launcher, const std::wstring& appId) {
    auto& st = db_.prepare("SELECT game_id FROM tracking_rules WHERE launcher = ? AND launcher_app_id = ? AND game_id IS NOT NULL");
    st.bind(1, launcher).bind(2, appId);
    std::optional<Id> out;
    if (st.step()) out = st.i64(0);
    st.reset();
    return out;
}

std::vector<std::pair<std::wstring, Id>> GameLibrary::launcherApps(const std::wstring& launcher) {
    std::vector<std::pair<std::wstring, Id>> out;
    auto& st = db_.prepare(
        "SELECT launcher_app_id, game_id FROM tracking_rules WHERE launcher = ? AND game_id IS NOT NULL AND launcher_app_id IS NOT NULL");
    st.bind(1, launcher);
    while (st.step()) out.emplace_back(st.str(0), st.i64(1));
    st.reset();
    return out;
}

bool GameLibrary::isIgnored(const std::wstring& launcher, const std::wstring& appId) {
    auto& st = db_.prepare("SELECT 1 FROM discovery_ignores WHERE launcher = ? AND app_id = ?");
    st.bind(1, launcher).bind(2, appId);
    bool r = st.step();
    st.reset();
    return r;
}

DiscoveryApplyResult GameLibrary::applyDiscovery(const std::vector<DiscoveredGame>& found) {
    DiscoveryApplyResult res;
    Database::Tx tx(db_);
    for (auto& d : found) {
        if (d.name.empty() || d.installDir.empty() || isIgnored(d.launcher, d.appId)) continue;
        auto norm = normalizePath(d.installDir);
        if (norm.size() < 4) continue;
        if (auto existing = findByLauncherApp(d.launcher, d.appId)) {
            bool same = false;
            Id ruleId = 0;
            for (auto& r : rules(*existing))
                if (r.kind == RuleKind::InstallDir && r.launcher == d.launcher && r.appId == d.appId) {
                    ruleId = r.id;
                    same = r.pattern == norm;
                }
            if (!same && ruleId) {
                db_.prepare("UPDATE tracking_rules SET pattern = ? WHERE id = ?").bind(1, norm).bind(2, ruleId).run();
                ++res.updated;
            }
            if (!d.coverPath.empty()) {
                auto g = game(*existing);
                if (g && g->coverPath.empty()) setCover(*existing, d.coverPath);
            }
            continue;
        }
        if (auto byName = findByName(d.name)) {
            addInstallDir(*byName, norm, d.launcher, d.appId);
            auto g = game(*byName);
            if (g && g->coverPath.empty() && !d.coverPath.empty()) setCover(*byName, d.coverPath);
            ++res.linked;
            continue;
        }
        Id id = addGame(d.name, d.coverPath);
        addInstallDir(id, norm, d.launcher, d.appId);
        ++res.added;
        res.addedNames.push_back(d.name);
    }
    tx.commit();
    return res;
}

}  // namespace pt
