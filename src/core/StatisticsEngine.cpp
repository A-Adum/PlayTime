#include "core/StatisticsEngine.h"

#include <algorithm>

#include "util/TimeUtil.h"

namespace pt {

std::vector<int64_t> StatisticsEngine::distribute(const std::vector<Session>& sessions,
                                                  const std::vector<int64_t>& edges) {
    std::vector<int64_t> out(edges.size() > 1 ? edges.size() - 1 : 0, 0);
    if (out.empty()) return out;
    for (auto& s : sessions) {
        int64_t a = s.startUtc, b = s.startUtc + s.duration;
        if (b <= edges.front() || a >= edges.back()) continue;
        // First bucket whose end is after a.
        size_t i = (size_t)(std::upper_bound(edges.begin(), edges.end(), a) - edges.begin());
        i = i == 0 ? 0 : i - 1;
        for (; i < out.size() && edges[i] < b; ++i) {
            int64_t lo = std::max(a, edges[i]), hi = std::min(b, edges[i + 1]);
            if (hi > lo) out[i] += hi - lo;
        }
    }
    return out;
}

std::vector<Bucket> StatisticsEngine::buckets(const std::vector<int64_t>& edges, Id gameId) {
    auto sessions = sessions_.overlapping(edges.front(), edges.back(), gameId);
    auto sums = distribute(sessions, edges);
    std::vector<Bucket> out(sums.size());
    for (size_t i = 0; i < sums.size(); ++i) out[i] = {edges[i], sums[i]};
    return out;
}

int64_t StatisticsEngine::total(int64_t from, int64_t to, Id gameId) {
    auto sums = distribute(sessions_.overlapping(from, to, gameId), {from, to});
    return sums.empty() ? 0 : sums[0];
}

std::vector<Bucket> StatisticsEngine::daily(int days, int64_t now, Id gameId) {
    int64_t today = localDayStart(now);
    std::vector<int64_t> edges;
    for (int d = days - 1; d >= -1; --d) edges.push_back(addLocalDays(today, -d));
    return buckets(edges, gameId);
}

std::vector<Bucket> StatisticsEngine::weekly(int weeks, int64_t now, Id gameId) {
    int64_t week = localWeekStart(now);
    std::vector<int64_t> edges;
    for (int w = weeks - 1; w >= -1; --w) edges.push_back(addLocalDays(week, -7 * w));
    return buckets(edges, gameId);
}

std::vector<Bucket> StatisticsEngine::monthly(int months, int64_t now, Id gameId) {
    int64_t month = localMonthStart(now);
    std::vector<int64_t> edges;
    for (int m = months - 1; m >= -1; --m) edges.push_back(addLocalMonths(month, -m));
    return buckets(edges, gameId);
}

SessionStats StatisticsEngine::sessionStats(Id gameId) {
    SessionStats s;
    auto& st = db_.prepare(
        "SELECT COUNT(*), IFNULL(SUM(duration), 0), IFNULL(MIN(start_utc), 0), IFNULL(MAX(end_utc), 0)"
        " FROM sessions WHERE (? = 0 OR game_id = ?)");
    st.bind(1, gameId).bind(2, gameId);
    if (st.step()) {
        s.count = st.i64(0);
        s.totalTracked = st.i64(1);
        s.firstStart = st.i64(2);
        s.lastEnd = st.i64(3);
    }
    st.reset();
    s.average = s.count ? s.totalTracked / s.count : 0;
    auto& lg = db_.prepare(
        "SELECT duration, game_id, start_utc FROM sessions WHERE (? = 0 OR game_id = ?) ORDER BY duration DESC LIMIT 1");
    lg.bind(1, gameId).bind(2, gameId);
    if (lg.step()) {
        s.longest = lg.i64(0);
        s.longestGame = lg.i64(1);
        s.longestStart = lg.i64(2);
    }
    lg.reset();
    return s;
}

std::vector<AccountShare> StatisticsEngine::topAccounts(int limit) {
    std::vector<AccountShare> out;
    auto& st = db_.prepare(
        "SELECT g.id, a.id, g.name, a.name,"
        " IFNULL((SELECT seconds FROM playtime_baselines b WHERE b.account_id = a.id), 0) +"
        " IFNULL((SELECT SUM(duration) FROM sessions s WHERE s.account_id = a.id), 0) AS total"
        " FROM accounts a JOIN games g ON g.id = a.game_id ORDER BY total DESC LIMIT ?");
    st.bind(1, (int64_t)limit);
    while (st.step()) {
        AccountShare a{st.i64(0), st.i64(1), st.str(2), st.str(3), st.i64(4)};
        if (a.seconds > 0) out.push_back(std::move(a));
    }
    st.reset();
    return out;
}

}  // namespace pt
