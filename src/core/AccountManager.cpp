#include "core/AccountManager.h"

#include <cwctype>

#include "util/Str.h"
#include "util/TimeUtil.h"

namespace pt {

namespace {
Account readAccount(Stmt& st) {
    return {st.i64(0), st.i64(1), st.str(2), st.str(3), st.i64(4)};
}

// Moves (adds) the baseline of (game, from) onto (game, to). Both may be kUnassigned.
void moveBaseline(Database& db, Id gameId, Id from, Id to) {
    auto& sel = db.prepare("SELECT id, seconds FROM playtime_baselines WHERE game_id = ? AND IFNULL(account_id, 0) = ?");
    sel.bind(1, gameId).bind(2, from);
    if (!sel.step()) {
        sel.reset();
        return;
    }
    Id srcId = sel.i64(0);
    int64_t seconds = sel.i64(1);
    sel.reset();
    auto& sel2 = db.prepare("SELECT id FROM playtime_baselines WHERE game_id = ? AND IFNULL(account_id, 0) = ?");
    sel2.bind(1, gameId).bind(2, to);
    bool exists = sel2.step();
    Id dstId = exists ? sel2.i64(0) : 0;
    sel2.reset();
    if (exists) {
        db.prepare("UPDATE playtime_baselines SET seconds = seconds + ?, source = 'manual', imported_total = NULL,"
                   " updated_at = ? WHERE id = ?")
            .bind(1, seconds).bind(2, nowUtc()).bind(3, dstId).run();
        db.prepare("DELETE FROM playtime_baselines WHERE id = ?").bind(1, srcId).run();
    } else {
        db.prepare("UPDATE playtime_baselines SET account_id = ?, updated_at = ? WHERE id = ?")
            .bindAccount(1, to).bind(2, nowUtc()).bind(3, srcId).run();
    }
}

std::wstring argValue(std::wstring_view cmd, std::wstring_view key) {
    std::wstring lower = toLower(cmd);
    size_t p = lower.find(key);
    while (p != std::wstring::npos) {
        // Must start an argument (preceded by whitespace or a quote).
        if (p == 0 || std::iswspace(lower[p - 1]) || lower[p - 1] == L'"') break;
        p = lower.find(key, p + 1);
    }
    if (p == std::wstring::npos) return {};
    size_t i = p + key.size();
    std::wstring out;
    if (i < cmd.size() && cmd[i] == L'"') {
        ++i;
        while (i < cmd.size() && cmd[i] != L'"') out += cmd[i++];
    } else {
        while (i < cmd.size() && !std::iswspace(cmd[i]) && cmd[i] != L'"') out += cmd[i++];
    }
    return trim(out);
}
}  // namespace

std::vector<Account> AccountManager::accounts(Id gameId) {
    std::vector<Account> out;
    auto& st = db_.prepare("SELECT id, game_id, name, launcher, created_at FROM accounts WHERE game_id = ? ORDER BY id");
    st.bind(1, gameId);
    while (st.step()) out.push_back(readAccount(st));
    st.reset();
    return out;
}

std::optional<Account> AccountManager::account(Id id) {
    auto& st = db_.prepare("SELECT id, game_id, name, launcher, created_at FROM accounts WHERE id = ?");
    st.bind(1, id);
    std::optional<Account> out;
    if (st.step()) out = readAccount(st);
    st.reset();
    return out;
}

Id AccountManager::addAccount(Id gameId, const std::wstring& name, const std::wstring& launcher) {
    auto n = trim(name);
    if (n.empty()) throw std::invalid_argument("Account name cannot be empty");
    db_.prepare("INSERT INTO accounts(game_id, name, launcher, created_at) VALUES(?, ?, ?, ?)")
        .bind(1, gameId).bind(2, n).bind(3, launcher).bind(4, nowUtc()).run();
    return db_.lastInsertId();
}

void AccountManager::renameAccount(Id id, const std::wstring& name) {
    auto n = trim(name);
    if (n.empty()) throw std::invalid_argument("Account name cannot be empty");
    db_.prepare("UPDATE accounts SET name = ? WHERE id = ?").bind(1, n).bind(2, id).run();
}

void AccountManager::setLauncher(Id id, const std::wstring& launcher) {
    db_.prepare("UPDATE accounts SET launcher = ? WHERE id = ?").bind(1, launcher).bind(2, id).run();
}

void AccountManager::deleteAccount(Id id, bool keepHours) {
    auto acc = account(id);
    if (!acc) return;
    Database::Tx tx(db_);
    if (keepHours) {
        moveBaseline(db_, acc->gameId, id, kUnassigned);
        db_.prepare("UPDATE sessions SET account_id = NULL WHERE account_id = ?").bind(1, id).run();
    } else {
        db_.prepare("DELETE FROM sessions WHERE account_id = ?").bind(1, id).run();
    }
    db_.prepare("DELETE FROM accounts WHERE id = ?").bind(1, id).run();  // cascades baseline + identifiers
    tx.commit();
}

void AccountManager::mergeAccounts(Id source, Id target) {
    if (source == target) throw std::invalid_argument("Cannot merge an account into itself");
    auto src = account(source);
    auto dst = account(target);
    if (!src || !dst) throw std::invalid_argument("Account not found");
    if (src->gameId != dst->gameId) throw std::invalid_argument("Accounts belong to different games");
    Database::Tx tx(db_);
    moveBaseline(db_, src->gameId, source, target);
    db_.prepare("UPDATE sessions SET account_id = ? WHERE account_id = ?").bind(1, target).bind(2, source).run();
    db_.prepare("UPDATE account_identifiers SET account_id = ? WHERE account_id = ?").bind(1, target).bind(2, source).run();
    db_.prepare("UPDATE games SET preferred_account_id = ? WHERE preferred_account_id = ?").bind(1, target).bind(2, source).run();
    db_.prepare("DELETE FROM accounts WHERE id = ?").bind(1, source).run();
    sessions_.coalesceOverlaps(src->gameId);
    tx.commit();
}

int AccountManager::assignUnassigned(Id gameId, Id accountId) {
    db_.prepare("UPDATE sessions SET account_id = ? WHERE game_id = ? AND account_id IS NULL")
        .bindAccount(1, accountId).bind(2, gameId).run();
    return db_.changes();
}

std::vector<AccountIdentifier> AccountManager::identifiers(Id accountId) {
    std::vector<AccountIdentifier> out;
    auto& st = db_.prepare("SELECT id, account_id, launcher, external_id, display_name FROM account_identifiers"
                           " WHERE account_id = ? ORDER BY id");
    st.bind(1, accountId);
    while (st.step()) out.push_back({st.i64(0), st.i64(1), st.str(2), st.str(3), st.str(4)});
    st.reset();
    return out;
}

void AccountManager::addIdentifier(Id accountId, const Identity& identity) {
    if (!identity.valid()) return;
    for (auto& i : identifiers(accountId))
        if (i.launcher == identity.launcher && i.externalId == identity.externalId) return;
    db_.prepare("INSERT INTO account_identifiers(account_id, launcher, external_id, display_name) VALUES(?, ?, ?, ?)")
        .bind(1, accountId).bind(2, identity.launcher).bind(3, identity.externalId).bind(4, identity.displayName).run();
}

void AccountManager::removeIdentifier(Id identifierId) {
    db_.prepare("DELETE FROM account_identifiers WHERE id = ?").bind(1, identifierId).run();
}

void AccountManager::setPreferredAccount(Id gameId, Id accountId) {
    auto& st = db_.prepare("UPDATE games SET preferred_account_id = ? WHERE id = ?");
    st.bindAccount(1, accountId).bind(2, gameId).run();
}

Id AccountManager::preferredAccount(Id gameId) {
    auto& st = db_.prepare("SELECT preferred_account_id FROM games WHERE id = ?");
    st.bind(1, gameId);
    Id out = 0;
    if (st.step() && !st.isNull(0)) out = st.i64(0);
    st.reset();
    return out;
}

Id AccountManager::resolve(Id gameId, const Identity& identity, bool autoCreate) {
    if (Id pref = preferredAccount(gameId)) return pref;
    if (!identity.valid()) return kUnassigned;
    {
        auto& st = db_.prepare(
            "SELECT a.id FROM account_identifiers i JOIN accounts a ON a.id = i.account_id"
            " WHERE a.game_id = ? AND i.launcher = ? AND i.external_id = ? LIMIT 1");
        st.bind(1, gameId).bind(2, identity.launcher).bind(3, identity.externalId);
        Id found = st.step() ? st.i64(0) : 0;
        st.reset();
        if (found) return found;
    }
    if (!autoCreate) return kUnassigned;
    std::wstring name = launcherDisplayName(identity.launcher) + L" \u00B7 " +
                        (identity.displayName.empty() ? identity.externalId : identity.displayName);
    Database::Tx tx(db_);
    Id id = addAccount(gameId, name, identity.launcher);
    addIdentifier(id, identity);
    tx.commit();
    return id;
}

Identity AccountManager::epicIdentityFromCommandLine(std::wstring_view cmdline) {
    Identity id;
    std::wstring uid = toLower(argValue(cmdline, L"-epicuserid="));
    if (uid.size() < 16 || uid.size() > 64) return id;
    for (wchar_t c : uid)
        if (!std::iswxdigit(c)) return id;
    id.launcher = L"epic";
    id.externalId = uid;
    id.displayName = argValue(cmdline, L"-epicusername=");
    return id;
}

std::wstring AccountManager::launcherDisplayName(const std::wstring& launcher) {
    if (launcher == L"steam") return L"Steam";
    if (launcher == L"epic") return L"Epic";
    if (launcher == L"xbox") return L"Xbox";
    if (launcher == L"gog") return L"GOG";
    if (launcher == L"ubisoft") return L"Ubisoft";
    if (launcher.empty()) return L"Other";
    return launcher;
}

}  // namespace pt
