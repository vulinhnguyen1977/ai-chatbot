// db.h -- thin RAII wrapper over the vendored SQLite amalgamation.
//
// One connection is shared process-wide and guarded by a mutex. Callers must
// keep transactions short and must never hold a Stmt across a model call.
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace db {

class DbError : public std::runtime_error {
public:
    explicit DbError(const std::string& what) : std::runtime_error(what) {}
};

class Database;

// Prepared statement. Bind indices are 1-based, column indices 0-based,
// matching the SQLite C API.
class Stmt {
public:
    Stmt(Database& db, const std::string& sql);
    ~Stmt();
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt(Stmt&& other) noexcept;

    Stmt& bind(int index, int64_t value);
    Stmt& bind(int index, const std::string& value);
    Stmt& bind(int index, const char* value);
    Stmt& bind_blob(int index, const void* data, size_t len);
    Stmt& bind_null(int index);

    // Advances to the next row. Returns false when the result set is done.
    bool step();
    void run();  // step() to completion, for statements with no result rows.
    void reset();

    bool is_null(int col) const;
    int64_t col_int(int col) const;
    double col_double(int col) const;
    std::string col_text(int col) const;
    std::vector<uint8_t> col_blob(int col) const;

private:
    Database& db_;
    sqlite3_stmt* stmt_ = nullptr;
};

class Database {
public:
    // Opens (creating if needed) the database at `path` and applies the schema.
    explicit Database(const std::string& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* handle() { return handle_; }
    std::mutex& mutex() { return mutex_; }

    void exec(const std::string& sql);
    int64_t last_insert_rowid();
    int changes();

    // Applies the schema and any pending migrations. Idempotent.
    void migrate();

private:
    sqlite3* handle_ = nullptr;
    std::mutex mutex_;
};

// RAII transaction. Rolls back if commit() was never called -- so an exception
// escaping mid-way can never leave a half-written chat or user record.
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit();

private:
    Database& db_;
    bool active_ = true;
};

}  // namespace db
