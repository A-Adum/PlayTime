#pragma once
#include <vector>

#include "core/Database.h"
#include "core/SessionStore.h"

namespace pt {

struct SessionStats {
    int64_t count = 0;
    int64_t totalTracked = 0;
    int64_t longest = 0;
    Id longestGame = 0;
    int64_t longestStart = 0;
    int64_t average = 0;
    int64_t firstStart = 0;
    int64_t lastEnd = 0;
};

struct Bucket {
    int64_t start = 0;  // UTC of the local calendar boundary
    int64_t seconds = 0;
};

struct AccountShare {
    Id gameId = 0;
    Id accountId = 0;
    std::wstring gameName;
    std::wstring accountName;
    int64_t seconds = 0;  // baseline + tracked
};

// Calendar statistics computed only from dated sessions. Historical baselines have no dates, so they
// contribute to lifetime totals (PlaytimeCalculator) but never to day/week/month figures.
// A session's active time is spread over [start, start + duration) when split across calendar buckets.
class StatisticsEngine {
public:
    StatisticsEngine(Database& db, SessionStore& sessions) : db_(db), sessions_(sessions) {}

    int64_t total(int64_t from, int64_t to, Id gameId = 0);
    std::vector<Bucket> daily(int days, int64_t now, Id gameId = 0);     // oldest first, ends with today
    std::vector<Bucket> weekly(int weeks, int64_t now, Id gameId = 0);   // Monday-based weeks
    std::vector<Bucket> monthly(int months, int64_t now, Id gameId = 0);
    SessionStats sessionStats(Id gameId = 0);
    std::vector<AccountShare> topAccounts(int limit);

    // Splits sessions over arbitrary ascending boundaries (n+1 edges -> n buckets).
    static std::vector<int64_t> distribute(const std::vector<Session>& sessions, const std::vector<int64_t>& edges);

private:
    std::vector<Bucket> buckets(const std::vector<int64_t>& edges, Id gameId);
    Database& db_;
    SessionStore& sessions_;
};

}  // namespace pt
