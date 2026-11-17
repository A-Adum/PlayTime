#include "core/SessionStore.h"

#include <algorithm>
#include <map>

namespace pt {

#define SESSION_COLS \
    "id, game_id, account_id, start_utc, end_utc, duration, source, is_open, recovered, edited, exe_name, exe_path, pid, proc_created"

Session SessionStore::read(Stmt& st) {
    Session s;
    s.id = st.i64(0);
    s.gameId = st.i64(1);
    s.accountId = st.isNull(2) ? kUnassigned : st.i64(2);
    s.startUtc = st.i64(3);
    s.endUtc = st.i64(4);
    s.duration = st.i64(5);
    s.source = st.str(6) == L"manual" ? SessionSource::Manual : SessionSource::Auto;
    s.open = st.i64(7) != 0;
    s.recovered = st.i64(8) != 0;
    s.edited = st.i64(9) != 0;
    s.exeName = st.str(10);
    s.exePath = st.str(11);
    s.pid = (uint32_t)st.i64(12);
    s.procCreated = st.i64(13);
    return s;
}

Id SessionStore::insert(const Session& s) {
    auto& st = db_.prepare(
        "INSERT INTO sessions(game_id, account_id, start_utc, end_utc, duration, source, is_open, recovered, edited,"
        " exe_name, exe_path, pid, proc_created) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
    st.bind(1, s.gameId).bindAccount(2, s.accountId).bind(3, s.startUtc).bind(4, s.endUtc).bind(5, s.duration);
    st.bind(6, s.source == SessionSource::Manual ? L"manual" : L"auto");
    st.bind(7, (int64_t)s.open).bind(8, (int64_t)s.recovered).bind(9, (int64_t)s.edited);
    st.bindOptText(10, s.exeName).bindOptText(11, s.exePath);
    if (s.pid) st.bind(12, (int64_t)s.pid); else st.bindNull(12);
    if (s.procCreated) st.bind(13, s.procCreated); else st.bindNull(13);
    st.run();
    return db_.lastInsertId();
}

void SessionStore::checkpoint(Id id, int64_t endUtc, int64_t duration) {
    db_.prepare("UPDATE sessions SET end_utc = ?, duration = ? WHERE id = ?")
        .bind(1, endUtc).bind(2, duration).bind(3, id).run();
}

void SessionStore::finalize(Id id, int64_t endUtc, int64_t duration, bool recovered) {
    db_.prepare("UPDATE sessions SET end_utc = ?, duration = ?, is_open = 0, recovered = (recovered OR ?) WHERE id = ?")
        .bind(1, endUtc).bind(2, duration).bind(3, (int64_t)recovered).bind(4, id).run();
}

void SessionStore::reopen(Id id) {
    db_.prepare("UPDATE sessions SET is_open = 1 WHERE id = ?").bind(1, id).run();
}

void SessionStore::setAccount(Id id, Id accountId) {
    db_.prepare("UPDATE sessions SET account_id = ? WHERE id = ?").bindAccount(1, accountId).bind(2, id).run();
}

void SessionStore::remove(Id id) { db_.prepare("DELETE FROM sessions WHERE id = ?").bind(1, id).run(); }

Id SessionStore::addManual(Id gameId, Id accountId, int64_t startUtc, int64_t duration) {
    Session s;
    s.gameId = gameId;
    s.accountId = accountId;
    s.startUtc = startUtc;
    s.duration = std::max<int64_t>(0, duration);
    s.endUtc = startUtc + s.duration;
    s.source = SessionSource::Manual;
    return insert(s);
}

void SessionStore::edit(Id id, Id accountId, int64_t startUtc, int64_t duration) {
    duration = std::max<int64_t>(0, duration);
    db_.prepare(
           "UPDATE sessions SET account_id = ?, start_utc = ?, end_utc = ?, duration = ?, edited = 1 WHERE id = ?")
        .bindAccount(1, accountId).bind(2, startUtc).bind(3, startUtc + duration).bind(4, duration).bind(5, id).run();
}

std::optional<Session> SessionStore::get(Id id) {
    auto& st = db_.prepare("SELECT " SESSION_COLS " FROM sessions WHERE id = ?");
    st.bind(1, id);
    std::optional<Session> out;
    if (st.step()) out = read(st);
    st.reset();
    return out;
}

std::vector<Session> SessionStore::forGame(Id gameId, Id accountFilter) {
    std::vector<Session> out;
    if (accountFilter < 0) {
        auto& st = db_.prepare("SELECT " SESSION_COLS " FROM sessions WHERE game_id = ? ORDER BY start_utc DESC");
        st.bind(1, gameId);
        while (st.step()) out.push_back(read(st));
        st.reset();
    } else {
        auto& st = db_.prepare("SELECT " SESSION_COLS
                               " FROM sessions WHERE game_id = ? AND IFNULL(account_id, 0) = ? ORDER BY start_utc DESC");
        st.bind(1, gameId).bind(2, accountFilter);
        while (st.step()) out.push_back(read(st));
        st.reset();
    }
    return out;
}

std::vector<Session> SessionStore::openSessions() {
    std::vector<Session> out;
    auto& st = db_.prepare("SELECT " SESSION_COLS " FROM sessions WHERE is_open = 1");
    while (st.step()) out.push_back(read(st));
    st.reset();
    return out;
}

std::vector<Session> SessionStore::overlapping(int64_t from, int64_t to, Id gameId) {
    std::vector<Session> out;
    // start_utc < to uses the index; the duration bound is checked per row.
    auto& st = db_.prepare("SELECT " SESSION_COLS " FROM sessions WHERE start_utc < ? AND start_utc + duration > ?"
                           " AND (? = 0 OR game_id = ?) ORDER BY start_utc");
    st.bind(1, to).bind(2, from).bind(3, gameId).bind(4, gameId);
    while (st.step()) out.push_back(read(st));
    st.reset();
    return out;
}

std::vector<Session> SessionStore::all() {
    std::vector<Session> out;
    auto& st = db_.prepare("SELECT " SESSION_COLS " FROM sessions ORDER BY start_utc");
    while (st.step()) out.push_back(read(st));
    st.reset();
    return out;
}

void SessionStore::moveToAccount(std::span<const Id> ids, Id accountId) {
    Database::Tx tx(db_);
    for (Id id : ids) setAccount(id, accountId);
    tx.commit();
}

int SessionStore::coalesceOverlaps(Id gameId) {
    auto sessions = forGame(gameId);
    std::map<Id, std::vector<Session>> byAccount;
    for (auto& s : sessions)
        if (!s.open) byAccount[s.accountId].push_back(s);

    int removed = 0;
    Database::Tx tx(db_);
    for (auto& [account, list] : byAccount) {
        std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.startUtc < b.startUtc; });
        Session* cur = nullptr;
        int64_t curEnd = 0;
        bool dirty = false;
        auto flush = [&] {
            if (cur && dirty)
                db_.prepare("UPDATE sessions SET start_utc = ?, end_utc = ?, duration = ? WHERE id = ?")
                    .bind(1, cur->startUtc).bind(2, curEnd).bind(3, cur->duration).bind(4, cur->id).run();
        };
        for (auto& s : list) {
            int64_t sEnd = std::max(s.endUtc, s.startUtc + s.duration);
            if (cur && s.startUtc < curEnd) {
                // Overlap: union of wall intervals; active time counted once for the shared part.
                int64_t overlap = std::min(curEnd, sEnd) - s.startUtc;
                int64_t newEnd = std::max(curEnd, sEnd);
                int64_t combined = cur->duration + s.duration - overlap;
                combined = std::max({combined, cur->duration, s.duration});
                combined = std::min(combined, newEnd - cur->startUtc);
                cur->duration = combined;
                curEnd = newEnd;
                dirty = true;
                remove(s.id);
                ++removed;
            } else {
                flush();
                cur = &s;
                curEnd = sEnd;
                dirty = false;
            }
        }
        flush();
    }
    tx.commit();
    return removed;
}

}  // namespace pt
