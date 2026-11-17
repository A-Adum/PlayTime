#include "core/Database.h"

#include <sqlite3.h>

#include "util/Str.h"

namespace pt {

namespace {
[[noreturn]] void throwDb(sqlite3* db, int rc, const char* context) {
    std::string msg = context;
    msg += ": ";
    msg += db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    throw DbError(msg, rc);
}

// Default helper-process exclusions applied to install-directory matches.
const wchar_t* kDefaultExclusions[] = {
    L"*crash*",        L"*uninst*",       L"unins???.exe",    L"*setup*.exe",      L"*redist*",
    L"vc_redist*",     L"dxsetup.exe",    L"dxwebsetup.exe",  L"*updater*",        L"*launcher*",
    L"easyanticheat*", L"eac*.exe",       L"beservice*",      L"battleye*",        L"*report*.exe",
    L"cefsharp*",      L"*webhelper*",    L"qtwebengineprocess.exe", L"*prereq*",  L"*installer*",
    L"dotnet*",        L"touchup.exe",    L"cleanup.exe",     L"*overlay*",        L"*helper*",
};

const char* kSchemaV1 = R"SQL(
CREATE TABLE games(
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL,
  cover_path TEXT,
  created_at INTEGER NOT NULL,
  preferred_account_id INTEGER REFERENCES accounts(id) ON DELETE SET NULL
);
CREATE TABLE game_aliases(
  game_id INTEGER NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  alias TEXT NOT NULL,
  PRIMARY KEY(game_id, alias)
);
CREATE TABLE accounts(
  id INTEGER PRIMARY KEY,
  game_id INTEGER NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  name TEXT NOT NULL,
  launcher TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL
);
CREATE INDEX accounts_game ON accounts(game_id);
CREATE TABLE account_identifiers(
  id INTEGER PRIMARY KEY,
  account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
  launcher TEXT NOT NULL,
  external_id TEXT NOT NULL,
  display_name TEXT NOT NULL DEFAULT ''
);
CREATE INDEX account_identifiers_lookup ON account_identifiers(launcher, external_id);
CREATE TABLE game_executables(
  id INTEGER PRIMARY KEY,
  game_id INTEGER NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  exe_name TEXT NOT NULL,
  full_path TEXT
);
CREATE INDEX game_executables_game ON game_executables(game_id);
CREATE TABLE tracking_rules(
  id INTEGER PRIMARY KEY,
  game_id INTEGER REFERENCES games(id) ON DELETE CASCADE,
  kind TEXT NOT NULL CHECK(kind IN ('install_dir','exclude')),
  pattern TEXT NOT NULL,
  launcher TEXT,
  launcher_app_id TEXT
);
CREATE INDEX tracking_rules_game ON tracking_rules(game_id);
CREATE INDEX tracking_rules_launcher ON tracking_rules(launcher, launcher_app_id);
CREATE TABLE playtime_baselines(
  id INTEGER PRIMARY KEY,
  game_id INTEGER NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  account_id INTEGER REFERENCES accounts(id) ON DELETE CASCADE,
  seconds INTEGER NOT NULL,
  source TEXT NOT NULL DEFAULT 'manual',
  imported_total INTEGER,
  updated_at INTEGER NOT NULL
);
CREATE UNIQUE INDEX playtime_baselines_key ON playtime_baselines(game_id, IFNULL(account_id, 0));
CREATE TABLE sessions(
  id INTEGER PRIMARY KEY,
  game_id INTEGER NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  account_id INTEGER REFERENCES accounts(id) ON DELETE SET NULL,
  start_utc INTEGER NOT NULL,
  end_utc INTEGER NOT NULL,
  duration INTEGER NOT NULL CHECK(duration >= 0),
  source TEXT NOT NULL CHECK(source IN ('auto','manual')),
  is_open INTEGER NOT NULL DEFAULT 0,
  recovered INTEGER NOT NULL DEFAULT 0,
  edited INTEGER NOT NULL DEFAULT 0,
  exe_name TEXT,
  exe_path TEXT,
  pid INTEGER,
  proc_created INTEGER
);
CREATE INDEX sessions_game ON sessions(game_id, account_id);
CREATE INDEX sessions_start ON sessions(start_utc);
CREATE INDEX sessions_open ON sessions(is_open) WHERE is_open = 1;
CREATE TABLE discovery_ignores(
  launcher TEXT NOT NULL,
  app_id TEXT NOT NULL,
  PRIMARY KEY(launcher, app_id)
);
CREATE TABLE settings(
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
)SQL";
}  // namespace

bool DbError::isCorruption() const {
    int primary = code & 0xFF;
    return primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB;
}

// ---------------- Stmt ----------------

Stmt::Stmt(sqlite3* db, const char* sql) : db_(db) {
    int rc = sqlite3_prepare_v3(db, sql, -1, SQLITE_PREPARE_PERSISTENT, &st_, nullptr);
    if (rc != SQLITE_OK) throwDb(db, rc, "prepare");
}

Stmt::~Stmt() { sqlite3_finalize(st_); }

Stmt& Stmt::bind(int idx, int64_t v) {
    sqlite3_bind_int64(st_, idx, v);
    return *this;
}

Stmt& Stmt::bind(int idx, std::wstring_view v) {
    std::string u = narrow(v);
    sqlite3_bind_text(st_, idx, u.data(), (int)u.size(), SQLITE_TRANSIENT);
    return *this;
}

Stmt& Stmt::bindNull(int idx) {
    sqlite3_bind_null(st_, idx);
    return *this;
}

Stmt& Stmt::bindAccount(int idx, Id account) {
    return account == kUnassigned ? bindNull(idx) : bind(idx, account);
}

Stmt& Stmt::bindOptText(int idx, const std::wstring& v) { return v.empty() ? bindNull(idx) : bind(idx, v); }

bool Stmt::step() {
    int rc = sqlite3_step(st_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    sqlite3_reset(st_);
    throwDb(db_, rc, "step");
}

void Stmt::reset() {
    sqlite3_reset(st_);
    sqlite3_clear_bindings(st_);
}

int64_t Stmt::i64(int col) const { return sqlite3_column_int64(st_, col); }

std::wstring Stmt::str(int col) const {
    auto p = (const char*)sqlite3_column_text(st_, col);
    return p ? widen(std::string_view(p, (size_t)sqlite3_column_bytes(st_, col))) : std::wstring();
}

bool Stmt::isNull(int col) const { return sqlite3_column_type(st_, col) == SQLITE_NULL; }

// ---------------- Database ----------------

Database::~Database() { close(); }

void Database::open(const std::wstring& path) {
    close();
    int rc = sqlite3_open_v2(narrow(path).c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc);
        sqlite3_close(db_);
        db_ = nullptr;
        throw DbError("open: " + msg, rc);
    }
    path_ = path;
    try {
        configure();
        std::string check = quickCheck();
        if (check != "ok") throw DbError("integrity check failed: " + check, SQLITE_CORRUPT);
        migrate();
    } catch (...) {
        close();
        throw;
    }
}

void Database::openMemory() {
    close();
    int rc = sqlite3_open(":memory:", &db_);
    if (rc != SQLITE_OK) throwDb(db_, rc, "open");
    path_.clear();
    configure();
    migrate();
}

void Database::close() {
    cache_.clear();
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

void Database::configure() {
    sqlite3_busy_timeout(db_, 3000);
    exec("PRAGMA foreign_keys=ON");
    if (!path_.empty()) {
        // WAL keeps checkpoints cheap; FULL sync makes every committed edit durable immediately.
        exec("PRAGMA journal_mode=WAL");
        exec("PRAGMA synchronous=FULL");
        exec("PRAGMA wal_autocheckpoint=256");
    }
    exec("PRAGMA cache_size=-512");
    exec("PRAGMA temp_store=MEMORY");
}

void Database::migrate() {
    int v = userVersion();
    if (v > kSchemaVersion)
        throw DbError("database was created by a newer PlayTime version (schema " + std::to_string(v) + ")",
                      SQLITE_MISMATCH);
    if (v == kSchemaVersion) return;
    Tx tx(*this);
    if (v < 1) {
        exec(kSchemaV1);
        auto& ins = prepare("INSERT INTO tracking_rules(game_id, kind, pattern) VALUES(NULL, 'exclude', ?)");
        for (auto* p : kDefaultExclusions) {
            ins.bind(1, p);
            ins.run();
        }
    }
    // Future migrations: if (v < 2) { ... }
    exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str());
    tx.commit();
}

Stmt& Database::prepare(const char* sql) {
    if (!db_) throw DbError("database is not open", SQLITE_MISUSE);
    auto it = cache_.find(sql);
    if (it == cache_.end()) it = cache_.emplace(sql, std::make_unique<Stmt>(db_, sql)).first;
    it->second->reset();
    return *it->second;
}

void Database::exec(const char* sql) {
    if (!db_) throw DbError("database is not open", SQLITE_MISUSE);
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : sqlite3_errstr(rc);
        sqlite3_free(err);
        throw DbError("exec: " + msg, rc);
    }
}

int64_t Database::lastInsertId() const { return sqlite3_last_insert_rowid(db_); }
int Database::changes() const { return sqlite3_changes(db_); }

int Database::userVersion() {
    Stmt st(db_, "PRAGMA user_version");
    return st.step() ? (int)st.i64(0) : 0;
}

std::string Database::quickCheck() {
    Stmt st(db_, "PRAGMA quick_check");
    return st.step() ? narrow(st.str(0)) : std::string();
}

Database::Tx::Tx(Database& db) : db_(db) { db_.exec("SAVEPOINT pt_tx"); }

Database::Tx::~Tx() {
    if (!done_) {
        try {
            db_.exec("ROLLBACK TO pt_tx");
            db_.exec("RELEASE pt_tx");
        } catch (...) {
        }
    }
}

void Database::Tx::commit() {
    db_.exec("RELEASE pt_tx");
    done_ = true;
}

}  // namespace pt
