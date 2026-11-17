#pragma once
#include <string>
#include <vector>

#include "core/Database.h"
#include "core/SettingsManager.h"

namespace pt {

struct BackupInfo {
    std::wstring path;
    std::wstring fileName;
    int64_t modifiedUtc = 0;
    int64_t sizeBytes = 0;
};

// Local backups (SQLite snapshots), restore, and JSON/CSV export/import. Everything stays on this PC.
class BackupManager {
public:
    static constexpr int kKeepAutomatic = 14;
    static constexpr int64_t kAutoInterval = 24 * 3600;

    BackupManager(Database& db, std::wstring backupDir) : db_(db), dir_(std::move(backupDir)) {}

    const std::wstring& backupDir() const { return dir_; }

    // Consistent snapshot of the live database ("VACUUM INTO"). Automatic backups are pruned to
    // kKeepAutomatic; manual and pre-restore ones are kept. Returns the file path. Throws on failure.
    std::wstring createBackup(const std::wstring& tag = L"auto");
    // Creates an automatic backup if the last one is older than kAutoInterval. Returns true if made.
    bool backupIfDue(SettingsManager& settings, int64_t now);
    std::vector<BackupInfo> listBackups();  // newest first

    // Validates `file` (opens it, quick_check, schema version not newer than ours), snapshots the current
    // database as a "pre-restore" backup, then replaces the live database and reopens it (migrating if old).
    // On any failure the current database is left untouched and DbError/runtime_error is thrown.
    void restore(const std::wstring& file);

    // Full data export (all tables) as JSON; importJson replaces all data with such an export atomically.
    void exportJson(const std::wstring& path);
    void importJson(const std::wstring& path);
    // Sessions as CSV (one row per session) plus a "<name>-totals.csv" with per-account lifetime totals.
    void exportCsv(const std::wstring& sessionsPath);

    // Opens the live database. If it is corrupt/unreadable it is renamed to "*.corrupt-<time>" and the
    // newest valid backup is restored (or a fresh database created). Returns a user-facing notice, empty
    // when nothing unusual happened.
    static std::wstring openWithRecovery(Database& db, const std::wstring& dbPath, const std::wstring& backupDir);

private:
    Database& db_;
    std::wstring dir_;
};

}  // namespace pt
