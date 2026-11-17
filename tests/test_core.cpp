#include <windows.h>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "test_harness.h"

#include "core/AccountManager.h"
#include "core/BackupManager.h"
#include "core/Database.h"
#include "core/GameDetector.h"
#include "core/GameLibrary.h"
#include "core/Model.h"
#include "core/PlaytimeCalculator.h"
#include "core/SessionStore.h"
#include "core/SessionTracker.h"
#include "core/SettingsManager.h"
#include "core/StatisticsEngine.h"
#include "platform/LauncherDiscovery.h"
#include "platform/SteamImport.h"
#include "util/Json.h"
#include "util/Str.h"
#include "util/TimeUtil.h"
#include "util/Vdf.h"

namespace {

class FakeClock : public pt::Clock {
public:
    int64_t mono = 0;
    int64_t wall = 0;

    explicit FakeClock(int64_t m = 0, int64_t w = 0) : mono(m), wall(w) {}

    int64_t monoMs() override { return mono; }
    int64_t wallUtc() override { return wall; }

    void advanceMs(int64_t ms) {
        mono += ms;
        wall += ms / 1000;
    }
    void advanceSec(int64_t sec) {
        mono += sec * 1000;
        wall += sec;
    }
};

struct TempDbPath {
    std::wstring path;
    TempDbPath() {
        wchar_t tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);
        static std::atomic<int> counter{0};
        path = std::wstring(tempDir) + L"pt_test_db_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
               std::to_wstring(++counter) + L".db";
    }
    ~TempDbPath() {
        DeleteFileW(path.c_str());
        DeleteFileW((path + L"-wal").c_str());
        DeleteFileW((path + L"-shm").c_str());
    }
};

struct TempDirPath {
    std::wstring path;
    TempDirPath() {
        wchar_t tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);
        static std::atomic<int> counter{0};
        path = std::wstring(tempDir) + L"pt_test_dir_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
               std::to_wstring(++counter);
        CreateDirectoryW(path.c_str(), nullptr);
    }
    ~TempDirPath() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

}  // namespace

// Scenario 1: Baseline hour calculations
TEST(BaselineHourCalculations) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::GameLibrary lib(db, sessions);
    pt::PlaytimeCalculator calc(db);

    pt::Id rl = lib.addGame(L"Rocket League");
    CHECK(rl > 0);

    // Set 3500h historical on Unassigned
    calc.setBaseline(rl, pt::kUnassigned, 3500 * 3600);
    CHECK_EQ(calc.baseline(rl, pt::kUnassigned), 3500 * 3600);

    // Add tracked sessions totalling 2h
    sessions.addManual(rl, pt::kUnassigned, 1000, 3600);
    sessions.addManual(rl, pt::kUnassigned, 5000, 3600);
    CHECK_EQ(calc.tracked(rl, pt::kUnassigned), 7200);

    // Lifetime total = 3502h
    CHECK_EQ(calc.baseline(rl, pt::kUnassigned) + calc.tracked(rl, pt::kUnassigned), 3502 * 3600);

    // adjustBaseline +/-
    calc.adjustBaseline(rl, pt::kUnassigned, 10 * 3600);
    CHECK_EQ(calc.baseline(rl, pt::kUnassigned), 3510 * 3600);

    calc.adjustBaseline(rl, pt::kUnassigned, -5 * 3600);
    CHECK_EQ(calc.baseline(rl, pt::kUnassigned), 3505 * 3600);

    // adjust below zero throws
    CHECK_THROWS(calc.adjustBaseline(rl, pt::kUnassigned, -4000 * 3600));

    // setLifetimeTotal sets baseline = total - tracked
    calc.setLifetimeTotal(rl, pt::kUnassigned, 3600 * 3600);
    CHECK_EQ(calc.baseline(rl, pt::kUnassigned), 3598 * 3600);
    CHECK_EQ(calc.baseline(rl, pt::kUnassigned) + calc.tracked(rl, pt::kUnassigned), 3600 * 3600);

    // total < tracked throws
    CHECK_THROWS(calc.setLifetimeTotal(rl, pt::kUnassigned, 1 * 3600));

    // grandTotal sums baselines + sessions across games
    pt::Id hk = lib.addGame(L"Hollow Knight");
    calc.setBaseline(hk, pt::kUnassigned, 100 * 3600);
    sessions.addManual(hk, pt::kUnassigned, 10000, 50 * 3600);

    int64_t expectedGrand = (3598 + 2 + 100 + 50) * 3600;
    CHECK_EQ(calc.grandTotal(), expectedGrand);
}

// Scenario 2: Multi-account unified total
TEST(MultiAccountUnifiedTotal) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::GameLibrary lib(db, sessions);
    pt::AccountManager accounts(db, sessions);
    pt::PlaytimeCalculator calc(db);

    pt::Id rl = lib.addGame(L"Rocket League");

    pt::Id epicMain = accounts.addAccount(rl, L"Epic Main", L"epic");
    pt::Id alt1 = accounts.addAccount(rl, L"Epic Alt 1", L"epic");
    pt::Id alt2 = accounts.addAccount(rl, L"Epic Alt 2", L"epic");
    pt::Id steam = accounts.addAccount(rl, L"Steam", L"steam");

    calc.setBaseline(rl, epicMain, 1800 * 3600);
    calc.setBaseline(rl, alt1, 950 * 3600);
    calc.setBaseline(rl, alt2, 600 * 3600);
    calc.setBaseline(rl, steam, 900 * 3600);

    sessions.addManual(rl, epicMain, 1000, 10 * 3600);
    sessions.addManual(rl, alt1, 2000, 5 * 3600);

    // At this point Unassigned has 0 baseline and 0 tracked sessions.
    // Unassigned row must NOT appear.
    auto rows = calc.accountTotals(rl);
    CHECK_EQ(rows.size(), 4u);
    for (const auto& r : rows) {
        CHECK_NE(r.name, L"Unassigned");
    }

    int64_t sumRows = 0;
    for (const auto& r : rows) {
        sumRows += r.total();
    }
    auto summaries = calc.summaries(pt::nowUtc());
    CHECK_EQ(summaries.size(), 1u);
    CHECK_EQ(sumRows, summaries[0].total());
    CHECK_EQ(sumRows, (4250 + 15) * 3600);

    // Add 1h tracked time to Unassigned
    sessions.addManual(rl, pt::kUnassigned, 3000, 1 * 3600);

    // Now Unassigned row MUST appear
    auto rowsWithUnassigned = calc.accountTotals(rl);
    CHECK_EQ(rowsWithUnassigned.size(), 5u);

    bool foundUnassigned = false;
    int64_t sumWithUnassigned = 0;
    for (const auto& r : rowsWithUnassigned) {
        sumWithUnassigned += r.total();
        if (r.accountId == pt::kUnassigned) {
            foundUnassigned = true;
            CHECK_EQ(r.name, L"Unassigned");
            CHECK_EQ(r.tracked, 3600);
        }
    }
    CHECK(foundUnassigned);

    auto summaries2 = calc.summaries(pt::nowUtc());
    CHECK_EQ(sumWithUnassigned, summaries2[0].total());
    CHECK_EQ(sumWithUnassigned, (4250 + 16) * 3600);
}

// Scenario 3: Session accumulation via SessionTracker with FakeClock
TEST(SessionAccumulation_CheckpointAndGrace) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::AccountManager accounts(db, sessions);
    pt::GameLibrary lib(db, sessions);
    pt::Id rl = lib.addGame(L"Rocket League");

    FakeClock clock(0, 1000000);
    pt::SessionTracker tracker(sessions, accounts, clock);

    pt::ProcessInfo proc{1001, L"rocketleague.exe", L"c:\\games\\rl.exe", 100, {}};
    tracker.processStarted(rl, proc);

    CHECK_EQ(tracker.active().size(), 1u);
    pt::Id sid = tracker.active()[0].sessionId;

    // Advance 10s -> tick: DB duration must remain 0
    clock.advanceSec(10);
    tracker.tick();
    CHECK_EQ(sessions.get(sid)->duration, 0);

    // Advance 20s (total 30s) -> tick: DB duration remains 0
    clock.advanceSec(20);
    tracker.tick();
    CHECK_EQ(sessions.get(sid)->duration, 0);

    // Advance 30s (total 60s) -> tick: now kCheckpointMs reached, DB duration updates to 60s
    clock.advanceSec(30);
    tracker.tick();
    CHECK_EQ(sessions.get(sid)->duration, 60);

    // Advance 10s (total 70s) -> tick: DB duration still 60s
    clock.advanceSec(10);
    tracker.tick();
    CHECK_EQ(sessions.get(sid)->duration, 60);

    // Advance 50s (total 120s) -> tick: DB duration updates to 120s
    clock.advanceSec(50);
    tracker.tick();
    CHECK_EQ(sessions.get(sid)->duration, 120);

    // Process exits
    tracker.processExited(rl, 1001);

    // Advance 5s (< kGraceMs 20s) -> session still lingering and open
    clock.advanceSec(5);
    tracker.tick();
    CHECK_EQ(sessions.get(sid)->open, true);

    // Advance 16s (total 21s > kGraceMs 20s) -> finalized
    clock.advanceSec(16);
    tracker.tick();
    auto finalSession = sessions.get(sid);
    CHECK(finalSession.has_value());
    CHECK_EQ(finalSession->open, false);
    CHECK_EQ(finalSession->duration, 120);

    // Sessions shorter than kMinSessionSec (10s) are discarded
    clock.advanceSec(10);
    pt::ProcessInfo shortProc{1002, L"rocketleague.exe", L"c:\\games\\rl.exe", 200, {}};
    tracker.processStarted(rl, shortProc);
    pt::Id shortSid = tracker.active()[0].sessionId;

    clock.advanceSec(8);  // 8s < 10s
    tracker.processExited(rl, 1002);
    clock.advanceSec(21); // Grace expired
    tracker.tick();

    CHECK_EQ(sessions.get(shortSid).has_value(), false);
}

// Scenario 4: Multiple processes of one game
TEST(MultipleProcesses_SameGame_TimeNotDoubled) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::AccountManager accounts(db, sessions);
    pt::GameLibrary lib(db, sessions);
    pt::Id rl = lib.addGame(L"Rocket League");

    FakeClock clock(0, 1000000);
    pt::SessionTracker tracker(sessions, accounts, clock);

    // Start first PID
    tracker.processStarted(rl, {2001, L"rocketleague.exe", L"", 1, {}});
    clock.advanceSec(10);

    // Start second PID for same game
    tracker.processStarted(rl, {2002, L"rocketleague.exe", L"", 2, {}});
    CHECK_EQ(tracker.active().size(), 1u);
    CHECK_EQ(tracker.active()[0].pids.size(), 2u);

    clock.advanceSec(20);

    // Exit first PID -> session continues running
    tracker.processExited(rl, 2001);
    CHECK_EQ(tracker.active()[0].running, true);

    clock.advanceSec(30);

    // Exit second PID
    tracker.processExited(rl, 2002);

    // Grace expires
    clock.advanceSec(25);
    tracker.tick();

    auto all = sessions.forGame(rl);
    CHECK_EQ(all.size(), 1u);
    // Active time: 10 + 20 + 30 = 60s, NOT doubled
    CHECK_EQ(all[0].duration, 60);
}

// Scenario 5: Relaunch within grace continues same session
TEST(Relaunch_WithinGrace_ContinuesSession_GapExcluded) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::AccountManager accounts(db, sessions);
    pt::GameLibrary lib(db, sessions);
    pt::Id rl = lib.addGame(L"Rocket League");

    FakeClock clock(0, 1000000);
    pt::SessionTracker tracker(sessions, accounts, clock);

    tracker.processStarted(rl, {3001, L"rocketleague.exe", L"", 1, {}});
    pt::Id sid = tracker.active()[0].sessionId;

    clock.advanceSec(30);
    tracker.processExited(rl, 3001);

    // Gap of 10s (< 20s grace)
    clock.advanceSec(10);

    // Relaunch within grace
    tracker.processStarted(rl, {3002, L"rocketleague.exe", L"", 2, {}});
    CHECK_EQ(tracker.active()[0].sessionId, sid);
    CHECK_EQ(tracker.active()[0].running, true);

    clock.advanceSec(30);
    tracker.processExited(rl, 3002);

    // Grace window expires
    clock.advanceSec(25);
    tracker.tick();

    auto s = sessions.get(sid);
    CHECK(s.has_value());
    // 30s + 30s = 60s. The 10s gap was not counted!
    CHECK_EQ(s->duration, 60);

    // Relaunch after grace creates a new session
    clock.advanceSec(5);
    tracker.processStarted(rl, {3003, L"rocketleague.exe", L"", 3, {}});
    pt::Id sid2 = tracker.active()[0].sessionId;
    CHECK_NE(sid2, sid);

    tracker.stopAll();
}

// Scenario 6: Multiple games simultaneously accumulate their own time
TEST(SimultaneousGames_AccumulateIndependentTime) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::AccountManager accounts(db, sessions);
    pt::GameLibrary lib(db, sessions);
    pt::Id rl = lib.addGame(L"Rocket League");
    pt::Id hk = lib.addGame(L"Hollow Knight");

    FakeClock clock(0, 1000000);
    pt::SessionTracker tracker(sessions, accounts, clock);

    tracker.processStarted(rl, {4001, L"rl.exe", L"", 1, {}});
    clock.advanceSec(10);

    tracker.processStarted(hk, {4002, L"hk.exe", L"", 2, {}});
    CHECK_EQ(tracker.active().size(), 2u);

    clock.advanceSec(50);

    tracker.processExited(rl, 4001);
    tracker.processExited(hk, 4002);

    clock.advanceSec(25);
    tracker.tick();

    CHECK_EQ(sessions.forGame(rl)[0].duration, 60);
    CHECK_EQ(sessions.forGame(hk)[0].duration, 50);
}

// Scenario 7: Sleep/resume suspended wall time is never counted
TEST(SleepResume_SuspendedWallTimeNeverCounted) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::AccountManager accounts(db, sessions);
    pt::GameLibrary lib(db, sessions);
    pt::Id rl = lib.addGame(L"Rocket League");

    FakeClock clock(0, 1000000);
    pt::SessionTracker tracker(sessions, accounts, clock);

    tracker.processStarted(rl, {5001, L"rl.exe", L"", 1, {}});
    pt::Id s1 = tracker.active()[0].sessionId;

    clock.advanceSec(30);

    // System sleep -> stopAll() ends sessions at suspend
    tracker.stopAll();
    auto s1Obj = sessions.get(s1);
    CHECK(s1Obj.has_value());
    CHECK_EQ(s1Obj->open, false);
    CHECK_EQ(s1Obj->duration, 30);
    CHECK_EQ(s1Obj->endUtc, 1000030);

    // Suspended for 2 hours (wall jumps by 7200, mono does not advance)
    clock.wall += 7200;

    // System resume -> new session starts
    tracker.processStarted(rl, {5002, L"rl.exe", L"", 2, {}});
    pt::Id s2 = tracker.active()[0].sessionId;
    CHECK_NE(s2, s1);

    clock.advanceSec(20);
    tracker.processExited(rl, 5002);
    clock.advanceSec(25);
    tracker.tick();

    auto s2Obj = sessions.get(s2);
    CHECK(s2Obj.has_value());
    CHECK_EQ(s2Obj->duration, 20);
    CHECK_EQ(s2Obj->startUtc, 1007230);

    // Total tracked time is 50s. The 7200s sleep was not counted.
    pt::PlaytimeCalculator calc(db);
    CHECK_EQ(calc.summaries(pt::nowUtc())[0].tracked, 50);
}

// Scenario 8: System clock change mid-session
TEST(SystemClockChange_WallUtcShift_DurationEqualsMono) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::AccountManager accounts(db, sessions);
    pt::GameLibrary lib(db, sessions);
    pt::Id rl = lib.addGame(L"Rocket League");

    FakeClock clock(10000, 5000000);
    pt::SessionTracker tracker(sessions, accounts, clock);

    tracker.processStarted(rl, {6001, L"rl.exe", L"", 1, {}});
    pt::Id sid = tracker.active()[0].sessionId;

    // Advance mono 40s, but wallUtc jumps backwards by 1 hour (DST / NTP)
    clock.mono += 40000;
    clock.wall -= 3600;
    tracker.checkpoint();

    // Advance mono 30s, wallUtc jumps forwards by 2 hours
    clock.mono += 30000;
    clock.wall += 7200;

    tracker.processExited(rl, 6001);
    clock.mono += 25000;
    tracker.tick();

    auto s = sessions.get(sid);
    CHECK(s.has_value());
    // Monotonic elapsed is 40s + 30s = 70s
    CHECK_EQ(s->duration, 70);
}

// Scenario 9: Unexpected shutdown recovery
TEST(UnexpectedShutdownRecovery) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::AccountManager accounts(db, sessions);
    pt::GameLibrary lib(db, sessions);
    pt::Id rl = lib.addGame(L"Rocket League");

    FakeClock clock(0, 1000000);

    // 1. Tracker with checkpointed open session, destroyed without stopAll()
    {
        pt::SessionTracker tracker1(sessions, accounts, clock);
        pt::ProcessInfo proc{7001, L"rl.exe", L"", 99999, {}};
        tracker1.processStarted(rl, proc);
        clock.advanceSec(65);
        tracker1.tick();  // Checkpointed at 65s
        clock.advanceSec(20);  // 85s live, but uncheckpointed
        // tracker1 destroyed simulating crash
    }

    pt::Id sid = sessions.all()[0].id;
    CHECK_EQ(sessions.get(sid)->open, true);
    CHECK_EQ(sessions.get(sid)->duration, 65);

    // 2. New tracker recover() closes session as recovered with checkpoint duration
    {
        pt::SessionTracker tracker2(sessions, accounts, clock);
        int rec = tracker2.recover();
        CHECK_EQ(rec, 1);

        auto s = sessions.get(sid);
        CHECK_EQ(s->open, false);
        CHECK_EQ(s->recovered, true);
        CHECK_EQ(s->duration, 65);

        // Same pid + created process reported -> session resumed (same id, duration continues)
        pt::ProcessInfo procSame{7001, L"rl.exe", L"", 99999, {}};
        tracker2.processStarted(rl, procSame);
        CHECK_EQ(tracker2.active().size(), 1u);
        CHECK_EQ(tracker2.active()[0].sessionId, sid);
        CHECK_EQ(tracker2.active()[0].resumed, true);

        clock.advanceSec(35);
        tracker2.processExited(rl, 7001);
        clock.advanceSec(25);
        tracker2.tick();

        s = sessions.get(sid);
        CHECK_EQ(s->open, false);
        CHECK_EQ(s->duration, 100);  // 65s + 35s = 100s
    }

    // 3. Different process after recovery creates a new session
    pt::Id sidA = 0;
    {
        pt::SessionTracker tracker3(sessions, accounts, clock);
        pt::ProcessInfo procA{8001, L"rl.exe", L"", 1234, {}};
        tracker3.processStarted(rl, procA);
        sidA = tracker3.active()[0].sessionId;
        clock.advanceSec(65);
        tracker3.tick();
        // crash
    }
    {
        pt::SessionTracker tracker4(sessions, accounts, clock);
        tracker4.recover();

        // Process with different pid/created reported
        pt::ProcessInfo procB{8002, L"rl.exe", L"", 5678, {}};
        tracker4.processStarted(rl, procB);
        pt::Id sidB = tracker4.active()[0].sessionId;
        CHECK_NE(sidB, sidA);

        auto oldSession = sessions.get(sidA);
        CHECK_EQ(oldSession->open, false);
        CHECK_EQ(oldSession->duration, 65);

        tracker4.stopAll();
    }
}

// Scenario 10: Account detection
TEST(AccountDetection_EpicCommandLine_And_Resolve) {
    std::wstring cmd =
        L"\"C:\\Program Files\\Epic Games\\rocketleague\\Binaries\\Win64\\RocketLeague.exe\" "
        L"-AUTH_LOGIN=unused -AUTH_PASSWORD=abc123 -AUTH_TYPE=exchangecode -epicapp=Sugar -epicenv=Prod "
        L"-EpicPortal -epicusername=\"Main Account\" -epicuserid=0123456789abcdef0123456789abcdef -epiclocale=en";

    pt::Identity id = pt::AccountManager::epicIdentityFromCommandLine(cmd);
    CHECK_EQ(id.launcher, L"epic");
    CHECK_EQ(id.externalId, L"0123456789abcdef0123456789abcdef");
    CHECK_EQ(id.displayName, L"Main Account");
    CHECK(id.valid());

    // Malformed Epic ids
    CHECK(!pt::AccountManager::epicIdentityFromCommandLine(L"-epicuserid=12345").valid());
    CHECK(!pt::AccountManager::epicIdentityFromCommandLine(L"-epicuserid=0123456789abcdefz").valid());
    std::wstring longId(70, L'a');
    CHECK(!pt::AccountManager::epicIdentityFromCommandLine(L"-epicuserid=" + longId).valid());

    // Resolve logic
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::AccountManager accounts(db, sessions);
    pt::GameLibrary lib(db, sessions);
    pt::Id rl = lib.addGame(L"Rocket League");

    // Invalid identity -> Unassigned
    pt::Identity invalidId;
    CHECK_EQ(accounts.resolve(rl, invalidId, true), pt::kUnassigned);

    // Unknown valid identity with autoCreate=false -> Unassigned
    pt::Identity unknownId{L"epic", L"0123456789abcdef0123456789abcdef", L"Main Account"};
    CHECK_EQ(accounts.resolve(rl, unknownId, /*autoCreate=*/false), pt::kUnassigned);

    // Unknown valid identity with autoCreate=true -> creates account
    pt::Id autoAcc = accounts.resolve(rl, unknownId, /*autoCreate=*/true);
    CHECK_NE(autoAcc, pt::kUnassigned);
    auto accObj = accounts.account(autoAcc);
    CHECK(accObj.has_value());
    CHECK_EQ(accObj->name, L"Epic \u00B7 Main Account");

    // Known identifier maps to its account
    CHECK_EQ(accounts.resolve(rl, unknownId, false), autoAcc);

    // Preferred account wins over known identifier
    pt::Id prefAcc = accounts.addAccount(rl, L"Preferred", L"epic");
    accounts.setPreferredAccount(rl, prefAcc);
    CHECK_EQ(accounts.resolve(rl, unknownId, true), prefAcc);
    accounts.setPreferredAccount(rl, 0);

    // Tracker with identities for two different epic ids assigns sessions to two different accounts
    FakeClock clock(0, 1000000);
    pt::SessionTracker tracker(sessions, accounts, clock);

    pt::Identity epic1{L"epic", L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", L"User1"};
    pt::Identity epic2{L"epic", L"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", L"User2"};

    tracker.processStarted(rl, {9001, L"rl.exe", L"", 1, epic1});
    pt::Id s1 = tracker.active()[0].sessionId;
    clock.advanceSec(15);
    tracker.processExited(rl, 9001);
    clock.advanceSec(25);
    tracker.tick();

    tracker.processStarted(rl, {9002, L"rl.exe", L"", 2, epic2});
    pt::Id s2 = tracker.active()[0].sessionId;
    clock.advanceSec(15);
    tracker.processExited(rl, 9002);
    clock.advanceSec(25);
    tracker.tick();

    CHECK_NE(sessions.get(s1)->accountId, sessions.get(s2)->accountId);
    CHECK_NE(sessions.get(s1)->accountId, pt::kUnassigned);
    CHECK_NE(sessions.get(s2)->accountId, pt::kUnassigned);
}

// Scenario 11: Account merging
TEST(AccountMerging_And_DeleteAccount) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::AccountManager accounts(db, sessions);
    pt::GameLibrary lib(db, sessions);
    pt::PlaytimeCalculator calc(db);
    pt::Id rl = lib.addGame(L"Rocket League");

    pt::Id alt1 = accounts.addAccount(rl, L"Epic Alt 1", L"epic");
    pt::Id alt2 = accounts.addAccount(rl, L"Epic Alt 2", L"epic");

    calc.setBaseline(rl, alt1, 950 * 3600);
    calc.setBaseline(rl, alt2, 600 * 3600);

    accounts.addIdentifier(alt1, {L"epic", L"id_alt1", L"Alt 1"});
    accounts.addIdentifier(alt2, {L"epic", L"id_alt2", L"Alt 2"});

    // Overlapping sessions: [1000, 1100) and [1050, 1150)
    sessions.addManual(rl, alt1, 1000, 100);
    sessions.addManual(rl, alt2, 1050, 100);

    // Merging across games throws
    pt::Id otherGame = lib.addGame(L"Other Game");
    pt::Id otherAcc = accounts.addAccount(otherGame, L"Other");
    CHECK_THROWS(accounts.mergeAccounts(otherAcc, alt1));

    // Merge Alt 2 into Alt 1
    accounts.mergeAccounts(alt2, alt1);

    // Alt 2 deleted
    CHECK_EQ(accounts.account(alt2).has_value(), false);

    // Baselines summed
    CHECK_EQ(calc.baseline(rl, alt1), 1550 * 3600);

    // Identifiers moved
    auto idents = accounts.identifiers(alt1);
    CHECK_EQ(idents.size(), 2u);

    // Sessions coalesced into [1000, 1150) duration 150s
    auto alt1Sessions = sessions.forGame(rl, alt1);
    CHECK_EQ(alt1Sessions.size(), 1u);
    CHECK_EQ(alt1Sessions[0].duration, 150);
    CHECK_EQ(alt1Sessions[0].startUtc, 1000);
    CHECK_EQ(alt1Sessions[0].endUtc, 1150);

    // Total game hours = 1550h + 150s
    CHECK_EQ(calc.summaries(pt::nowUtc())[0].total(), 1550 * 3600 + 150);

    // deleteAccount keepHours=true moves hours to Unassigned
    pt::Id tempAcc = accounts.addAccount(rl, L"Temp", L"epic");
    calc.setBaseline(rl, tempAcc, 100 * 3600);
    sessions.addManual(rl, tempAcc, 5000, 3600);

    int64_t totalBeforeKeep = calc.summaries(pt::nowUtc())[0].total();
    accounts.deleteAccount(tempAcc, /*keepHours=*/true);
    CHECK_EQ(accounts.account(tempAcc).has_value(), false);
    CHECK_EQ(calc.summaries(pt::nowUtc())[0].total(), totalBeforeKeep);
    CHECK_EQ(calc.baseline(rl, pt::kUnassigned), 100 * 3600);

    // deleteAccount keepHours=false removes them
    pt::Id tempAcc2 = accounts.addAccount(rl, L"Temp2", L"epic");
    calc.setBaseline(rl, tempAcc2, 50 * 3600);
    sessions.addManual(rl, tempAcc2, 10000, 3600);

    int64_t totalBeforeRemove = calc.summaries(pt::nowUtc())[0].total();
    accounts.deleteAccount(tempAcc2, /*keepHours=*/false);
    CHECK_EQ(accounts.account(tempAcc2).has_value(), false);
    CHECK_EQ(calc.summaries(pt::nowUtc())[0].total(), totalBeforeRemove - 51 * 3600);
}

// Scenario 12: Game merging
TEST(GameMerging_CombinesData_And_CoalescesSessions) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::GameLibrary lib(db, sessions);
    pt::PlaytimeCalculator calc(db);

    pt::Id gSteam = lib.addGame(L"Rocket League (Steam)");
    calc.setBaseline(gSteam, pt::kUnassigned, 100 * 3600);
    lib.addExecutable(gSteam, L"rocketleague.exe");
    lib.setAliases(gSteam, {L"RL Steam"});
    sessions.addManual(gSteam, pt::kUnassigned, 1000, 200);  // [1000, 1200)

    pt::Id gEpic = lib.addGame(L"Rocket League");
    calc.setBaseline(gEpic, pt::kUnassigned, 200 * 3600);
    sessions.addManual(gEpic, pt::kUnassigned, 1100, 200);  // [1100, 1300), overlaps by 100s

    lib.mergeGames(gSteam, gEpic);

    // Source game deleted
    CHECK_EQ(lib.game(gSteam).has_value(), false);

    // Unassigned baselines summed
    CHECK_EQ(calc.baseline(gEpic, pt::kUnassigned), 300 * 3600);

    // Source name added as alias to target
    auto aliases = lib.aliases(gEpic);
    bool hasSourceAlias = false;
    for (const auto& a : aliases) {
        if (a == L"Rocket League (Steam)") hasSourceAlias = true;
    }
    CHECK(hasSourceAlias);

    // Executable moved
    auto exes = lib.executables(gEpic);
    CHECK_EQ(exes.size(), 1u);
    CHECK_EQ(exes[0].exeName, L"rocketleague.exe");

    // Overlapping unassigned duplicate sessions coalesced
    auto unassignedSessions = sessions.forGame(gEpic, pt::kUnassigned);
    CHECK_EQ(unassignedSessions.size(), 1u);
    CHECK_EQ(unassignedSessions[0].startUtc, 1000);
    CHECK_EQ(unassignedSessions[0].endUtc, 1300);
    CHECK_EQ(unassignedSessions[0].duration, 300);

    // Total equals union (300h baseline + 300s session)
    CHECK_EQ(calc.summaries(pt::nowUtc())[0].total(), 300 * 3600 + 300);
}

// Scenario 13: Duplicate prevention in discovery
TEST(Discovery_DuplicatePrevention_And_Ignores) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::GameLibrary lib(db, sessions);

    pt::DiscoveredGame steamRL{
        L"steam", L"252950", L"Rocket League", L"C:\\Games\\Steam\\steamapps\\common\\rocketleague", L""};
    pt::DiscoveredGame epicRL{
        L"epic", L"Sugar", L"Rocket League\u00AE", L"C:\\Games\\Epic\\rocketleague", L""};

    auto res1 = lib.applyDiscovery({steamRL, epicRL});
    CHECK_EQ(res1.added, 1);
    CHECK_EQ(res1.linked, 1);
    CHECK_EQ(lib.games().size(), 1u);

    // Re-running discovery adds nothing
    auto res2 = lib.applyDiscovery({steamRL, epicRL});
    CHECK_EQ(res2.added, 0);
    CHECK_EQ(res2.linked, 0);
    CHECK_EQ(res2.updated, 0);
    CHECK_EQ(lib.games().size(), 1u);

    // Delete game -> deleted game is not re-added
    pt::Id gid = lib.games()[0].id;
    lib.deleteGame(gid);
    CHECK_EQ(lib.games().size(), 0u);

    auto res3 = lib.applyDiscovery({steamRL, epicRL});
    CHECK_EQ(res3.added, 0);
    CHECK_EQ(res3.linked, 0);
    CHECK_EQ(res3.updated, 0);
    CHECK_EQ(lib.games().size(), 0u);
}

// Scenario 14: Imported playtime reconciliation
TEST(SteamImport_Reconciliation_And_AccountCreation) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::GameLibrary lib(db, sessions);
    pt::AccountManager accounts(db, sessions);
    pt::PlaytimeCalculator calc(db);

    pt::Id rl = lib.addGame(L"Rocket League");
    pt::Id steamAcc = accounts.addAccount(rl, L"Steam · Player", L"steam");

    // 10h already tracked on that account
    sessions.addManual(rl, steamAcc, 1000, 10 * 3600);

    // reconcileImport with Steam total 900h -> baseline 890h, total 900h
    auto r1 = calc.reconcileImport(rl, steamAcc, 900 * 3600, L"steam");
    CHECK_EQ(r1.newBaseline, 890 * 3600);
    CHECK_EQ(r1.clamped, false);
    CHECK_EQ(calc.baseline(rl, steamAcc) + calc.tracked(rl, steamAcc), 900 * 3600);

    // Track 2 more hours -> 12h tracked
    sessions.addManual(rl, steamAcc, 5000, 2 * 3600);
    CHECK_EQ(calc.tracked(rl, steamAcc), 12 * 3600);

    // Re-import 905h -> baseline 893h (replaced, not added)
    auto r2 = calc.reconcileImport(rl, steamAcc, 905 * 3600, L"steam");
    CHECK_EQ(r2.newBaseline, 893 * 3600);
    CHECK_EQ(calc.baseline(rl, steamAcc) + calc.tracked(rl, steamAcc), 905 * 3600);

    // Import lower than tracked -> clamped baseline 0
    auto r3 = calc.reconcileImport(rl, steamAcc, 10 * 3600, L"steam");
    CHECK_EQ(r3.clamped, true);
    CHECK_EQ(r3.newBaseline, 0);
    CHECK_EQ(calc.baseline(rl, steamAcc) + calc.tracked(rl, steamAcc), 12 * 3600);

    // applySteamImport maps appid->game via discovered steam rule
    lib.addInstallDir(rl, L"C:\\Steam\\steamapps\\common\\rocketleague", L"steam", L"252950");

    // Preferred account set on game
    pt::Id prefAcc = accounts.addAccount(rl, L"My Preferred Account");
    accounts.setPreferredAccount(rl, prefAcc);

    pt::SteamUser user{777888, 76561198000777888ULL, L"RocketPersona"};
    std::map<std::wstring, int64_t> secondsByApp = {{L"252950", 900 * 3600}};

    auto lines = pt::applySteamImport(lib, accounts, calc, user, secondsByApp);
    CHECK_EQ(lines.size(), 1u);
    CHECK_EQ(lines[0].gameId, rl);
    CHECK_EQ(lines[0].accountName, L"Steam \u00B7 RocketPersona");
    // Verify no preferred account override: lines[0].accountId != prefAcc
    CHECK_NE(lines[0].accountId, prefAcc);
    CHECK_EQ(calc.baseline(rl, lines[0].accountId), 900 * 3600);

    // Re-running applySteamImport uses existing account
    secondsByApp[L"252950"] = 910 * 3600;
    auto lines2 = pt::applySteamImport(lib, accounts, calc, user, secondsByApp);
    CHECK_EQ(lines2.size(), 1u);
    CHECK_EQ(lines2[0].accountId, lines[0].accountId);
    CHECK_EQ(calc.baseline(rl, lines[0].accountId), 910 * 3600);
}

// Scenario 15: GameDetector
TEST(GameDetector_RulesExclusionsAndPaths) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::GameLibrary lib(db, sessions);

    pt::Id g1 = lib.addGame(L"Base Game");
    pt::Id g2 = lib.addGame(L"Nested Game");
    pt::Id g3 = lib.addGame(L"Constrained Game");

    lib.addInstallDir(g1, L"c:\\games");
    lib.addInstallDir(g2, L"c:\\games\\nested");

    // Global exclusions block install-dir matches
    lib.setExclusions(0, {L"*launcher*", L"*crash*"});

    // Per-game exclusion
    lib.setExclusions(g2, {L"*report*.exe"});

    // Explicit executable always matches
    lib.addExecutable(g1, L"launcher.exe");

    // Full-path constraint
    lib.addExecutable(g3, L"c:\\games\\special\\run.exe");

    pt::GameDetector detector;
    detector.load(lib);

    // Longest dir wins for nested dirs
    CHECK_EQ(detector.match(L"app.exe", L"c:\\games\\nested\\bin\\app.exe"), g2);
    CHECK_EQ(detector.match(L"app.exe", L"c:\\games\\other\\app.exe"), g1);

    // Global exclusions block install-dir matches
    CHECK_EQ(detector.match(L"crashpad_handler.exe", L"c:\\games\\nested\\crashpad_handler.exe"), 0);

    // Explicit executables always match
    CHECK_EQ(detector.match(L"launcher.exe", L"c:\\games\\other\\launcher.exe"), g1);

    // Per-game exclusion blocks for g2 but not g1
    CHECK_EQ(detector.match(L"report.exe", L"c:\\games\\nested\\report.exe"), 0);
    CHECK_EQ(detector.match(L"report.exe", L"c:\\games\\other\\report.exe"), g1);

    // Full-path constraint
    CHECK_EQ(detector.match(L"run.exe", L"c:\\games\\special\\run.exe"), g3);
    CHECK_EQ(detector.match(L"run.exe", L"c:\\games\\different\\run.exe"), g1);  // fallback to dir match for g1
}

// Scenario 16: Statistics accuracy
TEST(StatisticsEngine_LocalMidnightSplit_Buckets_And_Stats) {
    pt::Database db;
    db.openMemory();
    pt::SessionStore sessions(db);
    pt::GameLibrary lib(db, sessions);
    pt::PlaytimeCalculator calc(db);
    pt::Id rl = lib.addGame(L"Rocket League");

    // Session spanning local midnight: 23:00 to 01:00 (2h = 7200s)
    // Session spanning local midnight within the same Monday-based week:
    // Tuesday May 12, 2026 23:00 to Wednesday May 13, 2026 01:00 (2h = 7200s)
    auto optStart = pt::fromLocal(2026, 5, 12, 23, 0, 0);
    CHECK(optStart.has_value());
    int64_t start = *optStart;

    sessions.addManual(rl, pt::kUnassigned, start, 7200);

    pt::StatisticsEngine stats(db, sessions);
    int64_t now = *pt::fromLocal(2026, 5, 13, 12, 0, 0);

    // Daily buckets (2 days: May 12, May 13)
    auto daily = stats.daily(2, now, rl);
    CHECK_EQ(daily.size(), 2u);
    CHECK_EQ(daily[0].seconds, 3600);
    CHECK_EQ(daily[1].seconds, 3600);

    // Baselines never appear in daily/weekly/monthly
    calc.setBaseline(rl, pt::kUnassigned, 1000 * 3600);
    daily = stats.daily(2, now, rl);
    CHECK_EQ(daily[0].seconds, 3600);
    CHECK_EQ(daily[1].seconds, 3600);

    // total(from, to) partial overlap: 23:30 to 00:30 (1 hour = 3600s)
    int64_t from = *pt::fromLocal(2026, 5, 12, 23, 30, 0);
    int64_t to = *pt::fromLocal(2026, 5, 13, 0, 30, 0);
    CHECK_EQ(stats.total(from, to, rl), 3600);

    // Weekly and monthly buckets
    auto weekly = stats.weekly(1, now, rl);
    CHECK_EQ(weekly[0].seconds, 7200);

    auto monthly = stats.monthly(1, now, rl);
    CHECK_EQ(monthly[0].seconds, 7200);

    // SessionStats: longest, average, count
    sessions.addManual(rl, pt::kUnassigned, *pt::fromLocal(2026, 5, 11, 10, 0, 0), 1800);
    auto sstats = stats.sessionStats(rl);
    CHECK_EQ(sstats.count, 2);
    CHECK_EQ(sstats.totalTracked, 9000);
    CHECK_EQ(sstats.longest, 7200);
    CHECK_EQ(sstats.average, 4500);
}

// Scenario 17: Database persistence, Backup, Restore, JSON round trip
TEST(Database_Persistence_Backup_Restore_JsonRoundTrip) {
    TempDbPath tempDb;
    TempDirPath tempDir;

    // 1. Temp-file DB persistence
    {
        pt::Database db;
        db.open(tempDb.path);
        CHECK_EQ(db.userVersion(), pt::Database::kSchemaVersion);

        pt::SessionStore sessions(db);
        pt::GameLibrary lib(db, sessions);
        pt::AccountManager accounts(db, sessions);
        pt::PlaytimeCalculator calc(db);
        pt::SettingsManager settings(db);

        settings.load();
        settings.set(pt::setting::LaunchWithWindows, L"1");
        settings.setInt(pt::setting::PollIntervalSec, 5);

        pt::Id gid = lib.addGame(L"Rocket League");
        pt::Id aid = accounts.addAccount(gid, L"Epic Main", L"epic");
        calc.setBaseline(gid, aid, 1800 * 3600);
        sessions.addManual(gid, aid, 1000000, 7200);

        db.close();
    }

    // Reopen -> identical totals
    int64_t expectedTotal = (1800 * 3600) + 7200;
    std::wstring backupFile;
    std::wstring jsonPath = tempDir.path + L"\\export.json";
    {
        pt::Database db;
        db.open(tempDb.path);
        pt::PlaytimeCalculator calc(db);
        CHECK_EQ(calc.summaries(pt::nowUtc())[0].total(), expectedTotal);

        pt::SettingsManager settings(db);
        settings.load();
        CHECK_EQ(settings.str(pt::setting::LaunchWithWindows), L"1");
        CHECK_EQ(settings.integer(pt::setting::PollIntervalSec), 5);

        // BackupManager createBackup + exportJson
        pt::BackupManager backup(db, tempDir.path);
        backupFile = backup.createBackup(L"test");
        CHECK(GetFileAttributesW(backupFile.c_str()) != INVALID_FILE_ATTRIBUTES);

        backup.exportJson(jsonPath);
        CHECK(GetFileAttributesW(jsonPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    }

    // importJson round trip into fresh temp DB yields identical summaries
    {
        TempDbPath freshDb;
        pt::Database dbFresh;
        dbFresh.open(freshDb.path);

        pt::BackupManager backupFresh(dbFresh, tempDir.path);
        backupFresh.importJson(jsonPath);

        pt::PlaytimeCalculator calcFresh(dbFresh);
        auto summaries = calcFresh.summaries(pt::nowUtc());
        CHECK_EQ(summaries.size(), 1u);
        CHECK_EQ(summaries[0].total(), expectedTotal);
    }

    // restore() restores old state
    {
        pt::Database db;
        db.open(tempDb.path);
        pt::SessionStore sessions(db);
        pt::GameLibrary lib(db, sessions);
        pt::PlaytimeCalculator calc(db);

        // Alter state
        pt::Id gid = lib.games()[0].id;
        calc.adjustBaseline(gid, pt::kUnassigned, 500 * 3600);
        CHECK_NE(calc.summaries(pt::nowUtc())[0].total(), expectedTotal);

        // Restore
        pt::BackupManager backup(db, tempDir.path);
        backup.restore(backupFile);

        CHECK_EQ(calc.summaries(pt::nowUtc())[0].total(), expectedTotal);
    }
}

// Scenario 18: Utilities with real edge cases
TEST(Utilities_Vdf_Json_EpicManifest_FormatHours) {
    // 1. parseVdf on localconfig-like snippet with nested sections and case-insensitive lookup
    std::string_view vdfSnippet =
        "\"UserLocalConfigStore\"\n"
        "{\n"
        "    \"Software\"\n"
        "    {\n"
        "        \"valve\"\n"
        "        {\n"
        "            \"Steam\"\n"
        "            {\n"
        "                \"apps\"\n"
        "                {\n"
        "                    \"252950\"\n"
        "                    {\n"
        "                        \"Playtime\" \"12345\"\n"
        "                        \"LastPlayed\" \"1600000000\"\n"
        "                    }\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n";

    pt::VdfNode root = pt::parseVdf(vdfSnippet);
    const pt::VdfNode* appNode = root.path({"userlocalconfigstore", "software", "VALVE", "steam", "APPS", "252950"});
    CHECK(appNode != nullptr);
    CHECK_EQ(appNode->get("playtime"), "12345");
    CHECK_EQ(appNode->get("LASTPLAYED"), "1600000000");
    CHECK_EQ(appNode->get("missing", "default"), "default");

    // 2. Json parse/dump round trip with unicode escapes
    std::string jsonText =
        "{\"game\":\"Rocket League\\u00AE\",\"symbol\":\"\\u2122 \\u00A9\",\"chinese\":\"\\u4e16\\u754c\"}";
    pt::Json parsed = pt::Json::parse(jsonText);
    std::string dumped = parsed.dump();
    pt::Json reParsed = pt::Json::parse(dumped);

    CHECK_EQ(parsed["game"].asString(), reParsed["game"].asString());
    CHECK_EQ(parsed["symbol"].asString(), reParsed["symbol"].asString());
    CHECK_EQ(parsed["chinese"].asString(), reParsed["chinese"].asString());

    // 3. parseEpicManifest rejects incomplete installs and DLC
    std::string incompleteJson =
        "{\"AppName\":\"Sugar\",\"DisplayName\":\"RL\",\"InstallLocation\":\"C:/RL\",\"AppCategories\":[\"games\"],"
        "\"bIsIncompleteInstall\":true}";
    CHECK(!pt::parseEpicManifest(incompleteJson).has_value());

    std::string dlcJson =
        "{\"AppName\":\"SugarDLC\",\"MainGameAppName\":\"Sugar\",\"DisplayName\":\"DLC\",\"InstallLocation\":\"C:/RL\","
        "\"AppCategories\":[\"games\"]}";
    CHECK(!pt::parseEpicManifest(dlcJson).has_value());

    std::string nonGameJson =
        "{\"AppName\":\"Sugar\",\"DisplayName\":\"RL\",\"InstallLocation\":\"C:/RL\",\"AppCategories\":[\"addons\"]}";
    CHECK(!pt::parseEpicManifest(nonGameJson).has_value());

    std::string validJson =
        "{\"AppName\":\"Sugar\",\"DisplayName\":\"Rocket League\\u00AE\",\"InstallLocation\":\"C:/Games/RocketLeague\","
        "\"AppCategories\":[\"games\"]}";
    auto validEpic = pt::parseEpicManifest(validJson);
    CHECK(validEpic.has_value());
    CHECK_EQ(validEpic->appId, L"Sugar");
    CHECK_EQ(validEpic->name, L"Rocket League");

    // 4. cleanGameName strips ®
    CHECK_EQ(pt::cleanGameName(L"Rocket League\u00AE"), L"Rocket League");
    CHECK_EQ(pt::cleanGameName(L"Game\u2122 Name\u00A9"), L"Game Name");
    CHECK_EQ(pt::cleanGameName(L"  Trim   Spaces  \u00AE "), L"Trim Spaces");

    // 5. formatHours decimal and hm
    CHECK_EQ(pt::formatHours(3600, true), L"1.0 h");
    CHECK_EQ(pt::formatHours(3600, false), L"1h 00m");
    CHECK_EQ(pt::formatHours(2700, false), L"45m");
    CHECK_EQ(pt::formatHours(15301800, true), L"4,250.5 h");
    CHECK_EQ(pt::formatHours(15301800, false), L"4,250h 30m");
}
