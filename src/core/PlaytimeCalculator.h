#pragma once
#include <string>
#include <vector>

#include "core/Database.h"

namespace pt {

struct AccountTotals {
    Id accountId = kUnassigned;
    std::wstring name;  // "Unassigned" for kUnassigned
    std::wstring launcher;
    int64_t baseline = 0;  // historical/manual/imported seconds
    int64_t tracked = 0;   // sum of recorded sessions
    int64_t sessionCount = 0;
    int64_t lastPlayed = 0;
    std::wstring baselineSource;   // "manual", "steam", ...
    int64_t importedTotal = -1;    // launcher-reported lifetime total at last import, -1 if none
    int64_t total() const { return baseline + tracked; }
};

struct GameSummary {
    Game game;
    int64_t baseline = 0;
    int64_t tracked = 0;
    int64_t lastPlayed = 0;   // end of most recent session
    int64_t firstPlayed = 0;  // start of first session
    int64_t sessionCount = 0;
    int64_t accountCount = 0;
    int64_t recent = 0;  // tracked seconds in the last 14 days
    int64_t total() const { return baseline + tracked; }
};

struct ImportResult {
    int64_t importedTotal = 0;
    int64_t tracked = 0;
    int64_t previousBaseline = 0;
    int64_t newBaseline = 0;
    bool clamped = false;  // tracked time already exceeds the launcher's total
};

// Lifetime totals = historical baseline + tracked sessions. Baselines never create fake sessions.
class PlaytimeCalculator {
public:
    explicit PlaytimeCalculator(Database& db) : db_(db) {}

    std::vector<GameSummary> summaries(int64_t nowUtc);
    std::vector<AccountTotals> accountTotals(Id gameId);  // includes Unassigned when it has any time
    int64_t grandTotal();

    int64_t baseline(Id gameId, Id accountId);
    int64_t tracked(Id gameId, Id accountId);
    void setBaseline(Id gameId, Id accountId, int64_t seconds, const std::wstring& source = L"manual",
                     int64_t importedTotal = -1);
    // Adds (or subtracts) historical time. Fails if the baseline would go negative.
    void adjustBaseline(Id gameId, Id accountId, int64_t deltaSeconds);
    // Sets historical time so that baseline + tracked == totalSeconds. Fails if tracked already exceeds it.
    void setLifetimeTotal(Id gameId, Id accountId, int64_t totalSeconds);
    // Launcher-reported lifetime total for an account. The launcher total already includes sessions we
    // tracked on that account, so baseline = max(0, imported - tracked). Re-importing replaces, never adds.
    ImportResult reconcileImport(Id gameId, Id accountId, int64_t importedTotalSeconds, const std::wstring& source);
    // Deletes recorded sessions and historical time for one account of a game (or Unassigned).
    void resetAccount(Id gameId, Id accountId);

private:
    Database& db_;
};

}  // namespace pt
