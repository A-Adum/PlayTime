#include "core/SessionTracker.h"

#include <windows.h>

#include <algorithm>

#include "util/TimeUtil.h"

namespace pt {

int64_t SystemClock::monoMs() {
    ULONGLONG t = 0;
    QueryUnbiasedInterruptTime(&t);  // 100ns units, excludes sleep and hibernation
    return (int64_t)(t / 10000);
}

int64_t SystemClock::wallUtc() { return nowUtc(); }

int SessionTracker::recover() {
    candidates_.clear();
    auto open = sessions_.openSessions();
    for (auto& s : open) {
        sessions_.finalize(s.id, std::max(s.endUtc, s.startUtc + s.duration), s.duration, /*recovered=*/true);
        s.open = false;
        s.recovered = true;
        candidates_.push_back({s});
    }
    return (int)open.size();
}

const ActiveSession* SessionTracker::find(Id gameId) const {
    for (auto& s : active_)
        if (s.gameId == gameId) return &s;
    return nullptr;
}

ActiveSession* SessionTracker::findMut(Id gameId) {
    for (auto& s : active_)
        if (s.gameId == gameId) return &s;
    return nullptr;
}

int64_t SessionTracker::liveSeconds(const ActiveSession& s) const {
    int64_t ms = s.accumulatedMs;
    if (s.running) ms += std::max<int64_t>(0, clock_.monoMs() - s.segmentStart);
    return ms / 1000;
}

int64_t SessionTracker::unsavedSeconds(Id gameId) const {
    int64_t sum = 0;
    for (auto& s : active_)
        if (!gameId || s.gameId == gameId) sum += std::max<int64_t>(0, liveSeconds(s) - s.savedDuration);
    return sum;
}

bool SessionTracker::hasLingering() const {
    for (auto& s : active_)
        if (!s.running) return true;
    return false;
}

int64_t SessionTracker::endUtc(const ActiveSession& s, int64_t mono) const {
    return s.anchorUtc + std::max<int64_t>(0, mono - s.anchorMono) / 1000;
}

void SessionTracker::processStarted(Id gameId, const ProcessInfo& info) {
    int64_t now = clock_.monoMs();
    if (auto* s = findMut(gameId)) {
        if (std::find(s->pids.begin(), s->pids.end(), info.pid) == s->pids.end()) s->pids.push_back(info.pid);
        if (!s->running) {  // relaunch within the grace window continues the same session
            s->running = true;
            s->segmentStart = now;
            notify();
        }
        return;
    }

    ActiveSession a;
    a.gameId = gameId;
    a.pids.push_back(info.pid);
    a.exeName = info.exeName;
    a.segmentStart = now;
    a.anchorMono = now;
    a.anchorUtc = clock_.wallUtc();

    // Same process still alive after a PlayTime crash/restart: continue its session.
    for (size_t i = 0; i < candidates_.size(); ++i) {
        auto& c = candidates_[i].session;
        if (c.gameId == gameId && c.pid == info.pid && c.procCreated != 0 && c.procCreated == info.created) {
            a.sessionId = c.id;
            a.accountId = c.accountId;
            a.manual = c.source == SessionSource::Manual;
            a.startUtc = c.startUtc;
            a.accumulatedMs = c.duration * 1000;
            a.savedDuration = c.duration;
            a.resumed = true;
            sessions_.reopen(c.id);
            candidates_.erase(candidates_.begin() + (ptrdiff_t)i);
            active_.push_back(std::move(a));
            notify();
            return;
        }
    }

    a.startUtc = a.anchorUtc;
    a.accountId = accounts_.resolve(gameId, info.identity, autoCreate_);
    Session row;
    row.gameId = gameId;
    row.accountId = a.accountId;
    row.startUtc = a.startUtc;
    row.endUtc = a.startUtc;
    row.duration = 0;
    row.source = SessionSource::Auto;
    row.open = true;
    row.exeName = info.exeName;
    row.exePath = info.path;
    row.pid = info.pid;
    row.procCreated = info.created;
    a.sessionId = sessions_.insert(row);
    active_.push_back(std::move(a));
    if (active_.size() == 1) lastCheckpoint_ = now;
    notify();
}

void SessionTracker::processExited(Id gameId, uint32_t pid) {
    auto* s = findMut(gameId);
    if (!s) return;
    s->pids.erase(std::remove(s->pids.begin(), s->pids.end(), pid), s->pids.end());
    if (!s->pids.empty() || s->manual || !s->running) return;
    int64_t now = clock_.monoMs();
    s->accumulatedMs += std::max<int64_t>(0, now - s->segmentStart);
    s->running = false;
    s->lingerSince = now;
    // Persist the exit moment right away; the row is finalized when the grace window expires.
    int64_t dur = s->accumulatedMs / 1000;
    sessions_.checkpoint(s->sessionId, endUtc(*s, now), dur);
    s->savedDuration = dur;
    notify();
}

void SessionTracker::finish(size_t index) {
    ActiveSession s = std::move(active_[index]);
    active_.erase(active_.begin() + (ptrdiff_t)index);
    int64_t now = clock_.monoMs();
    int64_t endMono = s.running ? now : s.lingerSince;
    if (s.running) s.accumulatedMs += std::max<int64_t>(0, now - s.segmentStart);
    int64_t dur = s.accumulatedMs / 1000;
    if (!s.manual && !s.resumed && dur < kMinSessionSec) sessions_.remove(s.sessionId);
    else sessions_.finalize(s.sessionId, std::max(endUtc(s, endMono), s.startUtc + dur), dur);
}

void SessionTracker::tick() {
    int64_t now = clock_.monoMs();
    bool changed = false;
    for (size_t i = active_.size(); i-- > 0;) {
        if (!active_[i].running && now - active_[i].lingerSince >= kGraceMs) {
            finish(i);
            changed = true;
        }
    }
    if (!active_.empty() && now - lastCheckpoint_ >= kCheckpointMs) checkpoint();
    if (changed) notify();
}

void SessionTracker::checkpoint() {
    int64_t now = clock_.monoMs();
    lastCheckpoint_ = now;
    bool any = false;
    for (auto& s : active_)
        if (s.running) any = true;
    if (!any) return;
    for (auto& s : active_) {
        if (!s.running) continue;
        int64_t dur = liveSeconds(s);
        if (dur == s.savedDuration) continue;
        sessions_.checkpoint(s.sessionId, endUtc(s, now), dur);
        s.savedDuration = dur;
    }
}

void SessionTracker::stopAll() {
    if (active_.empty()) return;
    while (!active_.empty()) finish(active_.size() - 1);
    notify();
}

bool SessionTracker::startManual(Id gameId, Id accountId) {
    if (find(gameId)) return false;
    int64_t now = clock_.monoMs();
    ActiveSession a;
    a.gameId = gameId;
    a.manual = true;
    a.segmentStart = now;
    a.anchorMono = now;
    a.anchorUtc = a.startUtc = clock_.wallUtc();
    a.accountId = accountId ? accountId : accounts_.resolve(gameId, {}, false);
    Session row;
    row.gameId = gameId;
    row.accountId = a.accountId;
    row.startUtc = row.endUtc = a.startUtc;
    row.source = SessionSource::Manual;
    row.open = true;
    a.sessionId = sessions_.insert(row);
    active_.push_back(std::move(a));
    if (active_.size() == 1) lastCheckpoint_ = now;
    notify();
    return true;
}

void SessionTracker::stop(Id gameId) {
    for (size_t i = 0; i < active_.size(); ++i)
        if (active_[i].gameId == gameId) {
            finish(i);
            notify();
            return;
        }
}

void SessionTracker::setSessionAccount(Id gameId, Id accountId) {
    if (auto* s = findMut(gameId)) {
        s->accountId = accountId;
        sessions_.setAccount(s->sessionId, accountId);
        notify();
    }
}

}  // namespace pt
