#pragma once
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/Model.h"

struct sqlite3;
struct sqlite3_stmt;

namespace pt {

class DbError : public std::runtime_error {
public:
    DbError(const std::string& msg, int code) : std::runtime_error(msg), code(code) {}
    int code;
    bool isCorruption() const;
};

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql);
    ~Stmt();
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int idx, int64_t v);
    Stmt& bind(int idx, int v) { return bind(idx, (int64_t)v); }
    Stmt& bind(int idx, std::wstring_view v);
    Stmt& bind(int idx, const std::wstring& v) { return bind(idx, std::wstring_view(v)); }
    Stmt& bind(int idx, const wchar_t* v) { return bind(idx, std::wstring_view(v)); }
    Stmt& bindNull(int idx);
    Stmt& bindAccount(int idx, Id account);  // 0 -> NULL
    Stmt& bindOptText(int idx, const std::wstring& v);  // empty -> NULL

    bool step();  // true when a row is available; throws on error
    void run() { step(); reset(); }
    void reset();

    int64_t i64(int col) const;
    std::wstring str(int col) const;
    bool isNull(int col) const;

private:
    sqlite3* db_;
    sqlite3_stmt* st_ = nullptr;
};

// DatabaseManager: owns the SQLite connection, schema migrations and a prepared-statement cache.
class Database {
public:
    static constexpr int kSchemaVersion = 1;

    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Opens (creating if needed), verifies integrity and migrates. Throws DbError.
    void open(const std::wstring& path);
    void openMemory();
    void close();
    bool isOpen() const { return db_ != nullptr; }
    const std::wstring& path() const { return path_; }

    // Cached prepared statement, keyed by the address of the SQL literal. Returned already reset.
    Stmt& prepare(const char* sql);
    void exec(const char* sql);
    int64_t lastInsertId() const;
    int changes() const;
    int userVersion();
    std::string quickCheck();  // "ok" when healthy
    sqlite3* handle() const { return db_; }

    // Nestable transaction via SAVEPOINT; rolls back unless commit() is called.
    class Tx {
    public:
        explicit Tx(Database& db);
        ~Tx();
        void commit();
        Tx(const Tx&) = delete;
        Tx& operator=(const Tx&) = delete;

    private:
        Database& db_;
        bool done_ = false;
    };

private:
    void configure();
    void migrate();

    sqlite3* db_ = nullptr;
    std::wstring path_;
    std::unordered_map<const char*, std::unique_ptr<Stmt>> cache_;
};

}  // namespace pt
