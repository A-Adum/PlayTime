#include "core/BackupManager.h"

#include <sqlite3.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "core/Database.h"
#include "core/PlaytimeCalculator.h"
#include "core/SettingsManager.h"
#include "platform/WinUtil.h"
#include "util/Json.h"
#include "util/Str.h"
#include "util/TimeUtil.h"

namespace pt {

namespace {

const char* kExportTables[] = {
    "games",
    "game_aliases",
    "accounts",
    "account_identifiers",
    "game_executables",
    "tracking_rules",
    "playtime_baselines",
    "sessions",
    "discovery_ignores",
    "settings"
};

const char* kDeleteOrder[] = {
    "sessions",
    "playtime_baselines",
    "account_identifiers",
    "game_executables",
    "tracking_rules",
    "game_aliases",
    "accounts",
    "games",
    "discovery_ignores"
};

const char* kInsertOrder[] = {
    "games",
    "accounts",
    "game_aliases",
    "account_identifiers",
    "game_executables",
    "tracking_rules",
    "playtime_baselines",
    "sessions",
    "discovery_ignores",
    "settings"
};

std::wstring joinPath(const std::wstring& dir, const std::wstring& file) {
    if (dir.empty()) return file;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + file;
    return dir + L'\\' + file;
}

std::string escapeCsv(std::string_view s) {
    bool needsQuotes = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\r' || c == '\n') {
            needsQuotes = true;
            break;
        }
    }
    if (!needsQuotes) return std::string(s);
    std::string out;
    out.reserve(s.size() + 8);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') {
            out.push_back('"');
            out.push_back('"');
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::vector<BackupInfo> findBackups(const std::wstring& dir, const std::wstring& pattern = L"playtime-*.db") {
    std::vector<BackupInfo> out;
    std::wstring searchPattern = joinPath(dir, pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(searchPattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        BackupInfo info;
        info.fileName = fd.cFileName;
        info.path = joinPath(dir, fd.cFileName);
        uint64_t ft = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (ft >= 116444736000000000ULL) {
            info.modifiedUtc = static_cast<int64_t>((ft - 116444736000000000ULL) / 10000000ULL);
        } else {
            info.modifiedUtc = 0;
        }
        info.sizeBytes = ((int64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        out.push_back(std::move(info));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) {
        if (a.modifiedUtc != b.modifiedUtc) return a.modifiedUtc > b.modifiedUtc;
        return a.fileName > b.fileName;
    });

    return out;
}

bool validateBackup(const std::wstring& file) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(narrow(file).c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return false;
    }

    bool ok = true;

    // 1. PRAGMA quick_check must be "ok"
    {
        sqlite3_stmt* st = nullptr;
        rc = sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &st, nullptr);
        if (rc == SQLITE_OK && st) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                const char* res = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                if (!res || std::strcmp(res, "ok") != 0) {
                    ok = false;
                }
            } else {
                ok = false;
            }
            sqlite3_finalize(st);
        } else {
            ok = false;
        }
    }

    // 2. PRAGMA user_version must be between 1 and Database::kSchemaVersion
    if (ok) {
        sqlite3_stmt* st = nullptr;
        rc = sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr);
        if (rc == SQLITE_OK && st) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                int ver = sqlite3_column_int(st, 0);
                if (ver < 1 || ver > Database::kSchemaVersion) {
                    ok = false;
                }
            } else {
                ok = false;
            }
            sqlite3_finalize(st);
        } else {
            ok = false;
        }
    }

    // 3. Tables 'sessions' and 'games' must exist
    if (ok) {
        sqlite3_stmt* st = nullptr;
        rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ('games', 'sessions')",
                                -1, &st, nullptr);
        if (rc == SQLITE_OK && st) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                int cnt = sqlite3_column_int(st, 0);
                if (cnt != 2) {
                    ok = false;
                }
            } else {
                ok = false;
            }
            sqlite3_finalize(st);
        } else {
            ok = false;
        }
    }

    sqlite3_close(db);
    return ok;
}

std::unordered_set<std::string> getTableColumns(sqlite3* db, const std::string& tableName) {
    std::unordered_set<std::string> cols;
    std::string sql = "PRAGMA table_info(" + tableName + ")";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (name) {
                cols.insert(name);
            }
        }
        sqlite3_finalize(stmt);
    }
    return cols;
}

}  // namespace

std::wstring BackupManager::createBackup(const std::wstring& tag) {
    if (!ensureDir(dir_)) {
        throw DbError("failed to create backup directory: " + narrow(dir_), SQLITE_CANTOPEN);
    }
    if (!db_.handle()) {
        throw DbError("database is not open", SQLITE_MISUSE);
    }

    LocalParts lp = toLocal(nowUtc());
    wchar_t timeBuf[64];
    swprintf(timeBuf, 64, L"%04d%02d%02d-%02d%02d%02d",
             lp.year, lp.month, lp.day, lp.hour, lp.minute, lp.second);

    std::wstring baseName = L"playtime-" + tag + L"-" + timeBuf;
    std::wstring fileName = baseName + L".db";
    std::wstring fullPath = joinPath(dir_, fileName);

    if (fileExists(fullPath)) {
        int counter = 1;
        while (fileExists(joinPath(dir_, baseName + L"-" + std::to_wstring(counter) + L".db"))) {
            counter++;
        }
        fileName = baseName + L"-" + std::to_wstring(counter) + L".db";
        fullPath = joinPath(dir_, fileName);
    }

    std::string utf8Path = narrow(fullPath);
    std::string escapedPath;
    escapedPath.reserve(utf8Path.size() + 4);
    for (char c : utf8Path) {
        if (c == '\'') escapedPath += "''";
        else escapedPath += c;
    }

    std::string sql = "VACUUM INTO '" + escapedPath + "'";
    char* err = nullptr;
    int rc = sqlite3_exec(db_.handle(), sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : sqlite3_errstr(rc);
        sqlite3_free(err);
        throw DbError("createBackup failed: " + msg, rc);
    }

    // Prune: keep only the newest kKeepAutomatic files matching playtime-auto-*.db
    auto autoBackups = findBackups(dir_, L"playtime-auto-*.db");
    if (autoBackups.size() > static_cast<size_t>(kKeepAutomatic)) {
        for (size_t i = kKeepAutomatic; i < autoBackups.size(); ++i) {
            DeleteFileW(autoBackups[i].path.c_str());
        }
    }

    return fullPath;
}

bool BackupManager::backupIfDue(SettingsManager& settings, int64_t now) {
    if (now - settings.integer(setting::LastBackupUtc) >= kAutoInterval) {
        createBackup(L"auto");
        settings.setInt(setting::LastBackupUtc, now);
        return true;
    }
    return false;
}

std::vector<BackupInfo> BackupManager::listBackups() {
    return findBackups(dir_, L"playtime-*.db");
}

void BackupManager::restore(const std::wstring& file) {
    // 1. Validate file
    if (!validateBackup(file)) {
        throw DbError("backup validation failed: corrupt or incompatible file", SQLITE_CORRUPT);
    }

    // 2. Snapshot current database as pre-restore backup
    createBackup(L"pre-restore");

    // 3. Remember livePath, close db, copy file over livePath via temp file
    std::wstring livePath = db_.path();
    if (livePath.empty()) {
        throw DbError("cannot restore: database has no file path (in-memory)", SQLITE_MISUSE);
    }

    db_.close();

    std::wstring tempPath = livePath + L".restore.tmp";
    DeleteFileW(tempPath.c_str());

    bool copyOk = (CopyFileW(file.c_str(), tempPath.c_str(), FALSE) != 0);
    if (!copyOk) {
        try {
            db_.open(livePath);
        } catch (...) {
        }
        throw DbError("failed to copy backup to temporary file", SQLITE_IOERR);
    }

    DeleteFileW((livePath + L"-wal").c_str());
    DeleteFileW((livePath + L"-shm").c_str());

    bool moveOk = (MoveFileExW(tempPath.c_str(), livePath.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0);
    if (!moveOk) {
        DeleteFileW(tempPath.c_str());
        try {
            db_.open(livePath);
        } catch (...) {
        }
        throw DbError("failed to replace database with backup", SQLITE_IOERR);
    }

    // 4. Reopen livePath (migrates)
    // Note: Callers must reload SettingsManager, GameLibrary, etc. after restore() completes.
    try {
        db_.open(livePath);
    } catch (...) {
        try {
            if (!db_.isOpen() && fileExists(livePath)) {
                db_.open(livePath);
            }
        } catch (...) {
        }
        throw;
    }
}

void BackupManager::exportJson(const std::wstring& path) {
    if (!db_.handle()) {
        throw DbError("database is not open", SQLITE_MISUSE);
    }

    Json root = Json::object();
    root.set("format", "playtime-export");
    root.set("version", static_cast<int64_t>(1));
    root.set("schema", static_cast<int64_t>(Database::kSchemaVersion));
    root.set("exported_at", nowUtc());

    Json tablesObj = Json::object();

    for (const char* tableName : kExportTables) {
        std::string sql = std::string("SELECT * FROM ") + tableName;
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_.handle(), sql.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::string msg = sqlite3_errmsg(db_.handle());
            throw DbError("failed to query table " + std::string(tableName) + ": " + msg, rc);
        }

        int colCount = sqlite3_column_count(stmt);
        std::vector<std::string> colNames;
        colNames.reserve(colCount);
        for (int i = 0; i < colCount; ++i) {
            const char* n = sqlite3_column_name(stmt, i);
            colNames.push_back(n ? n : "");
        }

        Json rowsArr = Json::array();
        bool isSettings = (std::strcmp(tableName, "settings") == 0);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Json rowObj = Json::object();
            bool skipRow = false;

            for (int i = 0; i < colCount; ++i) {
                const std::string& colName = colNames[i];
                int colType = sqlite3_column_type(stmt, i);

                if (colType == SQLITE_INTEGER) {
                    rowObj.set(colName, static_cast<int64_t>(sqlite3_column_int64(stmt, i)));
                } else if (colType == SQLITE_FLOAT) {
                    rowObj.set(colName, sqlite3_column_double(stmt, i));
                } else if (colType == SQLITE_TEXT) {
                    const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                    std::string valStr = text ? text : "";
                    if (isSettings && colName == "key") {
                        if (valStr == "steam_api_key" || valStr == "window_rect") {
                            skipRow = true;
                        }
                    }
                    rowObj.set(colName, std::move(valStr));
                } else if (colType == SQLITE_NULL) {
                    rowObj.set(colName, nullptr);
                } else if (colType == SQLITE_BLOB) {
                    // BLOB unsupported -> skip
                }
            }

            if (!skipRow) {
                rowsArr.push(std::move(rowObj));
            }
        }
        sqlite3_finalize(stmt);
        tablesObj.set(tableName, std::move(rowsArr));
    }

    root.set("tables", std::move(tablesObj));
    std::string jsonStr = root.dump(true);
    if (!writeFileAtomic(path, jsonStr)) {
        throw std::runtime_error("failed to write JSON export file: " + narrow(path));
    }
}

void BackupManager::importJson(const std::wstring& path) {
    auto fileBytes = readFileBytes(path);
    if (!fileBytes) {
        throw std::runtime_error("failed to read import file: " + narrow(path));
    }

    Json doc = Json::parse(*fileBytes);
    if (!doc.isObject()) {
        throw std::runtime_error("invalid export format: root is not an object");
    }
    if (doc["format"].asString() != "playtime-export") {
        throw std::runtime_error("invalid export format: missing or invalid format identifier");
    }
    if (doc["version"].asInt(0) != 1) {
        throw std::runtime_error("unsupported export version: " + std::to_string(doc["version"].asInt(0)));
    }
    int64_t schema = doc["schema"].asInt(0);
    if (schema < 1 || schema > Database::kSchemaVersion) {
        throw std::runtime_error("unsupported export schema version: " + std::to_string(schema));
    }
    if (!doc.has("tables") || !doc["tables"].isObject()) {
        throw std::runtime_error("invalid export format: missing tables object");
    }

    // Snapshot current database as pre-import backup first
    createBackup(L"pre-import");

    // In one Database::Tx:
    // Note: Callers must reload SettingsManager, GameLibrary, etc. after importJson() completes.
    Database::Tx tx(db_);

    db_.exec("PRAGMA defer_foreign_keys=ON");

    // Delete all rows from those tables (children first, settings except steam_api_key/window_rect)
    for (const char* t : kDeleteOrder) {
        std::string sql = std::string("DELETE FROM ") + t;
        db_.exec(sql.c_str());
    }
    db_.exec("DELETE FROM settings WHERE key NOT IN ('steam_api_key', 'window_rect')");

    const Json& tablesObj = doc["tables"];
    std::unordered_set<std::string> processedTables;

    auto importTable = [&](const std::string& tableName) {
        if (!tablesObj.has(tableName)) return;
        const Json& rowsArr = tablesObj[tableName];
        if (!rowsArr.isArray()) return;

        auto validCols = getTableColumns(db_.handle(), tableName);
        if (validCols.empty()) return;

        bool isSettings = (tableName == "settings");

        for (const Json& row : rowsArr.items()) {
            if (!row.isObject()) continue;

            if (isSettings && row.has("key")) {
                const std::string& k = row["key"].asString();
                if (k == "steam_api_key" || k == "window_rect") {
                    continue;
                }
            }

            std::vector<std::string> cols;
            for (const auto& member : row.members()) {
                if (validCols.count(member.first) > 0) {
                    cols.push_back(member.first);
                }
            }
            if (cols.empty()) continue;

            std::string sql = "INSERT INTO " + tableName + " (";
            std::string placeholders;
            for (size_t i = 0; i < cols.size(); ++i) {
                if (i > 0) {
                    sql += ", ";
                    placeholders += ", ";
                }
                sql += cols[i];
                placeholders += "?";
            }
            sql += ") VALUES (" + placeholders + ")";

            sqlite3_stmt* stmt = nullptr;
            int rc = sqlite3_prepare_v2(db_.handle(), sql.c_str(), -1, &stmt, nullptr);
            if (rc != SQLITE_OK) {
                std::string msg = sqlite3_errmsg(db_.handle());
                throw DbError("prepare insert failed for " + tableName + ": " + msg, rc);
            }

            for (size_t i = 0; i < cols.size(); ++i) {
                int paramIdx = static_cast<int>(i + 1);
                const Json& val = row[cols[i]];
                if (val.isNull()) {
                    sqlite3_bind_null(stmt, paramIdx);
                } else if (val.isNumber()) {
                    if (val.asDouble() == static_cast<double>(val.asInt())) {
                        sqlite3_bind_int64(stmt, paramIdx, val.asInt());
                    } else {
                        sqlite3_bind_double(stmt, paramIdx, val.asDouble());
                    }
                } else if (val.isString()) {
                    sqlite3_bind_text(stmt, paramIdx, val.asString().data(),
                                      static_cast<int>(val.asString().size()), SQLITE_TRANSIENT);
                } else if (val.type() == Json::Type::Bool) {
                    sqlite3_bind_int64(stmt, paramIdx, val.asBool() ? 1 : 0);
                } else {
                    sqlite3_bind_null(stmt, paramIdx);
                }
            }

            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) {
                std::string msg = sqlite3_errmsg(db_.handle());
                throw DbError("insert row failed for " + tableName + ": " + msg, rc);
            }
        }
    };

    for (const char* t : kInsertOrder) {
        importTable(t);
        processedTables.insert(t);
    }
    for (const auto& member : tablesObj.members()) {
        if (processedTables.count(member.first) == 0) {
            importTable(member.first);
        }
    }

    // Check PRAGMA foreign_key_check returns no rows
    {
        sqlite3_stmt* fkStmt = nullptr;
        int rc = sqlite3_prepare_v2(db_.handle(), "PRAGMA foreign_key_check", -1, &fkStmt, nullptr);
        if (rc != SQLITE_OK) {
            std::string msg = sqlite3_errmsg(db_.handle());
            throw DbError("foreign_key_check prepare failed: " + msg, rc);
        }
        int fkRc = sqlite3_step(fkStmt);
        sqlite3_finalize(fkStmt);
        if (fkRc != SQLITE_DONE) {
            throw DbError("foreign_key_check found integrity violations in imported data", SQLITE_CONSTRAINT);
        }
    }

    tx.commit();
}

void BackupManager::exportCsv(const std::wstring& sessionsPath) {
    if (!db_.handle()) {
        throw DbError("database is not open", SQLITE_MISUSE);
    }

    // Sessions CSV:
    // session_id, game, account, start_local, end_local, duration_seconds, duration_hours, source, recovered, edited, executable
    std::string csvSessions = "\xEF\xBB\xBF";
    csvSessions += "session_id,game,account,start_local,end_local,duration_seconds,duration_hours,source,recovered,edited,executable\r\n";

    {
        auto& st = db_.prepare(
            "SELECT s.id, g.name, IFNULL(a.name, 'Unassigned'), s.start_utc, s.end_utc, s.duration,"
            " s.source, s.recovered, s.edited, IFNULL(s.exe_name, '')"
            " FROM sessions s"
            " JOIN games g ON g.id = s.game_id"
            " LEFT JOIN accounts a ON a.id = s.account_id"
            " ORDER BY s.start_utc ASC, s.id ASC");

        while (st.step()) {
            int64_t sessionId = st.i64(0);
            std::wstring gameName = st.str(1);
            std::wstring accName = st.str(2);
            if (accName.empty()) accName = L"Unassigned";
            int64_t startUtc = st.i64(3);
            int64_t endUtc = st.i64(4);
            int64_t duration = st.i64(5);
            std::wstring source = st.str(6);
            int64_t recovered = st.i64(7);
            int64_t edited = st.i64(8);
            std::wstring exeName = st.str(9);

            char durHoursBuf[32];
            snprintf(durHoursBuf, sizeof(durHoursBuf), "%.2f", static_cast<double>(duration) / 3600.0);

            csvSessions += std::to_string(sessionId) + ",";
            csvSessions += escapeCsv(narrow(gameName)) + ",";
            csvSessions += escapeCsv(narrow(accName)) + ",";
            csvSessions += escapeCsv(narrow(formatDateTime(startUtc))) + ",";
            csvSessions += escapeCsv(narrow(formatDateTime(endUtc))) + ",";
            csvSessions += std::to_string(duration) + ",";
            csvSessions += std::string(durHoursBuf) + ",";
            csvSessions += escapeCsv(narrow(source)) + ",";
            csvSessions += std::to_string(recovered) + ",";
            csvSessions += std::to_string(edited) + ",";
            csvSessions += escapeCsv(narrow(exeName)) + "\r\n";
        }
        st.reset();
    }

    // Totals CSV:
    // game, account, historical_seconds, tracked_seconds, total_seconds, total_hours
    std::wstring totalsPath;
    size_t dot = sessionsPath.find_last_of(L'.');
    size_t slash = sessionsPath.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
        totalsPath = sessionsPath.substr(0, dot) + L"-totals.csv";
    } else {
        totalsPath = sessionsPath + L"-totals.csv";
    }

    std::string csvTotals = "\xEF\xBB\xBF";
    csvTotals += "game,account,historical_seconds,tracked_seconds,total_seconds,total_hours\r\n";

    std::vector<std::pair<Id, std::wstring>> gamesList;
    {
        auto& gStmt = db_.prepare("SELECT id, name FROM games ORDER BY name COLLATE NOCASE ASC, id ASC");
        while (gStmt.step()) {
            gamesList.emplace_back(gStmt.i64(0), gStmt.str(1));
        }
        gStmt.reset();
    }

    PlaytimeCalculator calc(db_);
    for (const auto& [gameId, gameName] : gamesList) {
        auto accounts = calc.accountTotals(gameId);
        for (const auto& acc : accounts) {
            char hoursBuf[32];
            snprintf(hoursBuf, sizeof(hoursBuf), "%.2f", static_cast<double>(acc.total()) / 3600.0);

            csvTotals += escapeCsv(narrow(gameName)) + ",";
            csvTotals += escapeCsv(narrow(acc.name)) + ",";
            csvTotals += std::to_string(acc.baseline) + ",";
            csvTotals += std::to_string(acc.tracked) + ",";
            csvTotals += std::to_string(acc.total()) + ",";
            csvTotals += std::string(hoursBuf) + "\r\n";
        }
    }

    if (!writeFileAtomic(sessionsPath, csvSessions)) {
        throw std::runtime_error("failed to write sessions CSV file: " + narrow(sessionsPath));
    }
    if (!writeFileAtomic(totalsPath, csvTotals)) {
        throw std::runtime_error("failed to write totals CSV file: " + narrow(totalsPath));
    }
}

std::wstring BackupManager::openWithRecovery(Database& db, const std::wstring& dbPath, const std::wstring& backupDir) {
    try {
        db.open(dbPath);
        return L"";
    } catch (const DbError& e) {
        bool corrupt = e.isCorruption() ||
                       (e.code == SQLITE_CORRUPT) ||
                       (e.what() && std::string(e.what()).find("integrity") != std::string::npos);
        if (!corrupt) {
            throw;
        }

        db.close();

        LocalParts lp = toLocal(nowUtc());
        wchar_t timeBuf[64];
        swprintf(timeBuf, 64, L"%04d%02d%02d-%02d%02d%02d",
                 lp.year, lp.month, lp.day, lp.hour, lp.minute, lp.second);

        std::wstring corruptBase = dbPath + L".corrupt-" + timeBuf;
        std::wstring corruptPath = corruptBase;
        if (fileExists(corruptPath)) {
            int counter = 1;
            while (fileExists(corruptBase + L"-" + std::to_wstring(counter))) {
                counter++;
            }
            corruptPath = corruptBase + L"-" + std::to_wstring(counter);
        }

        MoveFileExW(dbPath.c_str(), corruptPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (fileExists(dbPath + L"-wal")) {
            MoveFileExW((dbPath + L"-wal").c_str(), (corruptPath + L"-wal").c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        if (fileExists(dbPath + L"-shm")) {
            MoveFileExW((dbPath + L"-shm").c_str(), (corruptPath + L"-shm").c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        DeleteFileW((dbPath + L"-wal").c_str());
        DeleteFileW((dbPath + L"-shm").c_str());

        auto backups = findBackups(backupDir, L"playtime-*.db");
        bool restored = false;
        std::wstring restoredName;
        std::wstring restoredDate;

        for (const auto& b : backups) {
            if (validateBackup(b.path)) {
                if (CopyFileW(b.path.c_str(), dbPath.c_str(), FALSE)) {
                    DeleteFileW((dbPath + L"-wal").c_str());
                    DeleteFileW((dbPath + L"-shm").c_str());
                    try {
                        db.open(dbPath);
                        restored = true;
                        restoredName = b.fileName;
                        restoredDate = formatDateTime(b.modifiedUtc);
                        break;
                    } catch (...) {
                        db.close();
                        DeleteFileW(dbPath.c_str());
                        DeleteFileW((dbPath + L"-wal").c_str());
                        DeleteFileW((dbPath + L"-shm").c_str());
                    }
                }
            }
        }

        if (restored) {
            return L"The database was damaged and has been restored from backup " +
                   restoredName + L" (" + restoredDate + L"). The damaged file was kept as " +
                   corruptPath + L".";
        }

        // If no valid backup: db.open(dbPath) creates a fresh database
        DeleteFileW(dbPath.c_str());
        DeleteFileW((dbPath + L"-wal").c_str());
        DeleteFileW((dbPath + L"-shm").c_str());
        db.open(dbPath);
        return L"The database was damaged and could not be recovered from backup. A fresh database was created. The damaged file was kept as " +
               corruptPath + L".";
    }
}

}  // namespace pt
