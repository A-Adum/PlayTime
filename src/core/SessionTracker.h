#pragma once
#include <functional>
#include <vector>

#include "core/AccountManager.h"
#include "core/Database.h"
#include "core/SessionStore.h"

namespace pt {

// Time source. Durations use a monotonic clock that does not advance during sleep/hibernation
// (QueryUnbiasedInterruptTime), so wall-clock changes and suspended time never count as play.
class Clock {
public:
    virtual ~Clock() = default;
    virtual int64_t monoMs() = 0;
    virtual int64_t wallUtc() = 0;
};

class SystemClock final : public Clock {
public:
    int64_t monoMs() override;
    int64_t wallUtc() override;
};

struct ProcessInfo {
    uint32_t pid = 0;
    std::wstring exeName;  // lowercase
    std::wstring path;     // lowercase, may be empty
    int64_t created = 0;   // process creation FILETIME
    Identity identity;
};

struct ActiveSession {
    Id sessionId = 0;
    Id gameId = 0;
    Id accountId = kUnassigned;
    bool manual = false;
    bool resumed = false;      // continued after a PlayTime restart
    int64_t startUtc = 0;
    int64_t anchorUtc = 0;     // wall time at anchorMono; end time = anchorUtc + elapsed monotonic
    int64_t anchorMono = 0;
    int64_t accumulatedMs = 0; // active time of finished segments
    int64_t segmentStart = 0;  // monotonic start of the running segment
    bool running = true;       // false while lingering after the last process exited
    int64_t lingerSince = 0;
    int64_t savedDuration = 0; // what the database currently holds
    std::vector<uint32_t> pids;
    std::wstring exeName;
};

// One active session per game no matter how many of its processes run, so time is never doubled.
// Each game accumulates its own time; simultaneous games therefore can exceed wall-clock time.
class SessionTracker {
public:
    static constexpr int64_t kCheckpointMs = 60'000;  // low-frequency persistence of running sessions
    static constexpr int64_t kGraceMs = 20'000;       // relaunch/bootstrapper window that continues a session
    static constexpr int64_t kMinSessionSec = 10;     // shorter automatic sessions are discarded as noise

    SessionTracker(SessionStore& sessions, AccountManager& accounts, Clock& clock)
        : sessions_(sessions), accounts_(accounts), clock_(clock) {}

    void setAutoCreateAccounts(bool v) { autoCreate_ = v; }

    // Closes sessions left open by a crash/power loss (keeping their last checkpoint) and remembers them
    // so a still-running game process can continue its original session. Returns the number recovered.
    int recover();
    void clearRecoveryCandidates() { candidates_.clear(); }

    void processStarted(Id gameId, const ProcessInfo& info);
    void processExited(Id gameId, uint32_t pid);
    void tick();        // finishes expired lingering sessions; checkpoints when due
    void checkpoint();  // persists running sessions now
    void stopAll();     // sleep, lock, pause, shutdown: ends every session at the current time

    bool startManual(Id gameId, Id accountId);
    void stop(Id gameId);
    void setSessionAccount(Id gameId, Id accountId);  // reassigns the running session

    const std::vector<ActiveSession>& active() const { return active_; }
    const ActiveSession* find(Id gameId) const;
    int64_t liveSeconds(const ActiveSession& s) const;
    int64_t unsavedSeconds(Id gameId = 0) const;  // live minus checkpointed, for live UI totals
    bool hasLingering() const;

    std::function<void()> onChange;

private:
    struct Candidate {
        Session session;
    };
    ActiveSession* findMut(Id gameId);
    int64_t endUtc(const ActiveSession& s, int64_t mono) const;
    void finish(size_t index);
    void notify() {
        if (onChange) onChange();
    }

    SessionStore& sessions_;
    AccountManager& accounts_;
    Clock& clock_;
    bool autoCreate_ = true;
    std::vector<ActiveSession> active_;
    std::vector<Candidate> candidates_;
    int64_t lastCheckpoint_ = 0;
};

}  // namespace pt
