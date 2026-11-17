#pragma once
#include <optional>
#include <string>
#include <vector>

#include "core/Database.h"
#include "core/SessionStore.h"

namespace pt {

class AccountManager {
public:
    AccountManager(Database& db, SessionStore& sessions) : db_(db), sessions_(sessions) {}

    std::vector<Account> accounts(Id gameId);
    std::optional<Account> account(Id id);
    Id addAccount(Id gameId, const std::wstring& name, const std::wstring& launcher = {});
    void renameAccount(Id id, const std::wstring& name);
    void setLauncher(Id id, const std::wstring& launcher);
    // keepHours: sessions and baseline move to Unassigned; otherwise they are deleted with the account.
    void deleteAccount(Id id, bool keepHours);
    // Moves sessions, baseline and identifiers of `source` into `target` (same game) and deletes `source`.
    void mergeAccounts(Id source, Id target);
    // Moves every Unassigned session of the game to `accountId`.
    int assignUnassigned(Id gameId, Id accountId);

    std::vector<AccountIdentifier> identifiers(Id accountId);
    void addIdentifier(Id accountId, const Identity& identity);
    void removeIdentifier(Id identifierId);

    // Sticky manual choice of which account the next/current sessions belong to (0 = auto-detect).
    void setPreferredAccount(Id gameId, Id accountId);
    Id preferredAccount(Id gameId);

    // Picks the account for a new session: manual choice > known identifier > (optionally) a new account
    // created from a reliable identity > Unassigned. Never guesses from unreliable signals.
    Id resolve(Id gameId, const Identity& identity, bool autoCreate);

    // Extracts an Epic identity from a launch command line (-epicuserid=..., -epicusername=...).
    // Other arguments (notably the one-time -AUTH_PASSWORD) are never retained.
    static Identity epicIdentityFromCommandLine(std::wstring_view cmdline);
    static std::wstring launcherDisplayName(const std::wstring& launcher);

private:
    Database& db_;
    SessionStore& sessions_;
};

}  // namespace pt
