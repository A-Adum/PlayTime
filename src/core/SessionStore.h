#pragma once
#include <optional>
#include <span>
#include <vector>

#include "core/Database.h"

namespace pt {

// Persistence for sessions: inserts/checkpoints from the tracker, and user edits.
class SessionStore {
public:
    explicit SessionStore(Database& db) : db_(db) {}

    Id insert(const Session& s);
    void checkpoint(Id id, int64_t endUtc, int64_t duration);
    void finalize(Id id, int64_t endUtc, int64_t duration, bool recovered = false);
    void reopen(Id id);  // resume a recovered session after a PlayTime restart
    void setAccount(Id id, Id accountId);
    void remove(Id id);

    // Dated manual record supplied by the user ("I played 3h on Saturday").
    Id addManual(Id gameId, Id accountId, int64_t startUtc, int64_t duration);
    // User edit of times/account. Marks the session as edited. Duration is clamped to the wall interval.
    void edit(Id id, Id accountId, int64_t startUtc, int64_t duration);

    std::optional<Session> get(Id id);
    std::vector<Session> forGame(Id gameId, Id accountFilter = -1);  // newest first; -1 = all accounts
    std::vector<Session> openSessions();
    // Sessions whose active interval [start, start+duration) overlaps [from, to). gameId 0 = all games.
    std::vector<Session> overlapping(int64_t from, int64_t to, Id gameId = 0);
    std::vector<Session> all();

    void moveToAccount(std::span<const Id> ids, Id accountId);

    // Merges overlapping closed sessions of the same (game, account) so shared time is counted once.
    // Returns number of sessions removed.
    int coalesceOverlaps(Id gameId);

private:
    Session read(Stmt& st);
    Database& db_;
};

}  // namespace pt
