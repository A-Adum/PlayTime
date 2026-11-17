#include "core/PlaytimeCalculator.h"

#include <algorithm>

#include "util/TimeUtil.h"

namespace pt {

std::vector<GameSummary> PlaytimeCalculator::summaries(int64_t now) {
    std::vector<GameSummary> out;
    auto& st = db_.prepare(
        "SELECT g.id, g.name, g.cover_path, g.created_at, g.preferred_account_id,"
        " (SELECT IFNULL(SUM(seconds), 0) FROM playtime_baselines b WHERE b.game_id = g.id),"
        " IFNULL(s.tracked, 0), IFNULL(s.last, 0), IFNULL(s.first, 0), IFNULL(s.cnt, 0),"
        " (SELECT COUNT(*) FROM accounts a WHERE a.game_id = g.id),"
        " (SELECT IFNULL(SUM(duration), 0) FROM sessions r WHERE r.game_id = g.id AND r.start_utc >= ?)"
        " FROM games g LEFT JOIN (SELECT game_id, SUM(duration) AS tracked, MAX(end_utc) AS last,"
        " MIN(start_utc) AS first, COUNT(*) AS cnt FROM sessions GROUP BY game_id) s ON s.game_id = g.id");
    st.bind(1, now - 14 * 86400);
    while (st.step()) {
        GameSummary s;
        s.game.id = st.i64(0);
        s.game.name = st.str(1);
        s.game.coverPath = st.str(2);
        s.game.createdAt = st.i64(3);
        s.game.preferredAccount = st.isNull(4) ? 0 : st.i64(4);
        s.baseline = st.i64(5);
        s.tracked = st.i64(6);
        s.lastPlayed = st.i64(7);
        s.firstPlayed = st.i64(8);
        s.sessionCount = st.i64(9);
        s.accountCount = st.i64(10);
        s.recent = st.i64(11);
        out.push_back(std::move(s));
    }
    st.reset();
    return out;
}

std::vector<AccountTotals> PlaytimeCalculator::accountTotals(Id gameId) {
    std::vector<AccountTotals> out;
    auto& st = db_.prepare(
        "SELECT a.id, a.name, a.launcher, IFNULL(b.seconds, 0), IFNULL(b.source, ''), IFNULL(b.imported_total, -1),"
        " (SELECT IFNULL(SUM(duration), 0) FROM sessions s WHERE s.account_id = a.id),"
        " (SELECT COUNT(*) FROM sessions s WHERE s.account_id = a.id),"
        " (SELECT IFNULL(MAX(end_utc), 0) FROM sessions s WHERE s.account_id = a.id)"
        " FROM accounts a LEFT JOIN playtime_baselines b ON b.account_id = a.id"
        " WHERE a.game_id = ? ORDER BY a.id");
    st.bind(1, gameId);
    while (st.step()) {
        AccountTotals t;
        t.accountId = st.i64(0);
        t.name = st.str(1);
        t.launcher = st.str(2);
        t.baseline = st.i64(3);
        t.baselineSource = st.str(4);
        t.importedTotal = st.i64(5);
        t.tracked = st.i64(6);
        t.sessionCount = st.i64(7);
        t.lastPlayed = st.i64(8);
        out.push_back(std::move(t));
    }
    st.reset();

    AccountTotals un;
    un.name = L"Unassigned";
    {
        auto& b = db_.prepare("SELECT seconds, source, IFNULL(imported_total, -1) FROM playtime_baselines"
                              " WHERE game_id = ? AND account_id IS NULL");
        b.bind(1, gameId);
        if (b.step()) {
            un.baseline = b.i64(0);
            un.baselineSource = b.str(1);
            un.importedTotal = b.i64(2);
        }
        b.reset();
    }
    {
        auto& s = db_.prepare("SELECT IFNULL(SUM(duration), 0), COUNT(*), IFNULL(MAX(end_utc), 0) FROM sessions"
                              " WHERE game_id = ? AND account_id IS NULL");
        s.bind(1, gameId);
        if (s.step()) {
            un.tracked = s.i64(0);
            un.sessionCount = s.i64(1);
            un.lastPlayed = s.i64(2);
        }
        s.reset();
    }
    if (un.baseline != 0 || un.sessionCount > 0) out.push_back(std::move(un));
    return out;
}

int64_t PlaytimeCalculator::grandTotal() {
    auto& st = db_.prepare("SELECT (SELECT IFNULL(SUM(seconds), 0) FROM playtime_baselines) +"
                           " (SELECT IFNULL(SUM(duration), 0) FROM sessions)");
    int64_t v = st.step() ? st.i64(0) : 0;
    st.reset();
    return v;
}

int64_t PlaytimeCalculator::baseline(Id gameId, Id accountId) {
    auto& st = db_.prepare("SELECT seconds FROM playtime_baselines WHERE game_id = ? AND IFNULL(account_id, 0) = ?");
    st.bind(1, gameId).bind(2, accountId);
    int64_t v = st.step() ? st.i64(0) : 0;
    st.reset();
    return v;
}

int64_t PlaytimeCalculator::tracked(Id gameId, Id accountId) {
    auto& st = db_.prepare(
        "SELECT IFNULL(SUM(duration), 0) FROM sessions WHERE game_id = ? AND IFNULL(account_id, 0) = ?");
    st.bind(1, gameId).bind(2, accountId);
    int64_t v = st.step() ? st.i64(0) : 0;
    st.reset();
    return v;
}

void PlaytimeCalculator::setBaseline(Id gameId, Id accountId, int64_t seconds, const std::wstring& source,
                                     int64_t importedTotal) {
    if (seconds < 0) throw std::invalid_argument("Historical time cannot be negative");
    Database::Tx tx(db_);
    db_.prepare("DELETE FROM playtime_baselines WHERE game_id = ? AND IFNULL(account_id, 0) = ?")
        .bind(1, gameId).bind(2, accountId).run();
    if (seconds > 0 || importedTotal >= 0) {
        auto& st = db_.prepare("INSERT INTO playtime_baselines(game_id, account_id, seconds, source, imported_total,"
                               " updated_at) VALUES(?, ?, ?, ?, ?, ?)");
        st.bind(1, gameId).bindAccount(2, accountId).bind(3, seconds).bind(4, source);
        if (importedTotal >= 0) st.bind(5, importedTotal); else st.bindNull(5);
        st.bind(6, nowUtc()).run();
    }
    tx.commit();
}

void PlaytimeCalculator::adjustBaseline(Id gameId, Id accountId, int64_t delta) {
    int64_t next = baseline(gameId, accountId) + delta;
    if (next < 0)
        throw std::invalid_argument(
            "That would make historical time negative. Edit or delete recorded sessions to reduce tracked time.");
    setBaseline(gameId, accountId, next);
}

void PlaytimeCalculator::setLifetimeTotal(Id gameId, Id accountId, int64_t totalSeconds) {
    int64_t t = tracked(gameId, accountId);
    if (totalSeconds < t)
        throw std::invalid_argument(
            "The total is lower than the time already tracked. Edit or delete recorded sessions instead.");
    setBaseline(gameId, accountId, totalSeconds - t);
}

ImportResult PlaytimeCalculator::reconcileImport(Id gameId, Id accountId, int64_t importedTotal,
                                                 const std::wstring& source) {
    ImportResult r;
    r.importedTotal = std::max<int64_t>(0, importedTotal);
    r.tracked = tracked(gameId, accountId);
    r.previousBaseline = baseline(gameId, accountId);
    r.newBaseline = std::max<int64_t>(0, r.importedTotal - r.tracked);
    r.clamped = r.importedTotal < r.tracked;
    setBaseline(gameId, accountId, r.newBaseline, source, r.importedTotal);
    return r;
}

void PlaytimeCalculator::resetAccount(Id gameId, Id accountId) {
    Database::Tx tx(db_);
    db_.prepare("DELETE FROM sessions WHERE game_id = ? AND IFNULL(account_id, 0) = ? AND is_open = 0")
        .bind(1, gameId).bind(2, accountId).run();
    db_.prepare("DELETE FROM playtime_baselines WHERE game_id = ? AND IFNULL(account_id, 0) = ?")
        .bind(1, gameId).bind(2, accountId).run();
    tx.commit();
}

}  // namespace pt
