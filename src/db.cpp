#include "db.h"

#include "sqlite3.h"

namespace db {

// ---------------------------------------------------------------- Database

Database::Database(const std::string& path) {
    int rc = sqlite3_open_v2(path.c_str(), &handle_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = handle_ ? sqlite3_errmsg(handle_) : "out of memory";
        if (handle_) sqlite3_close(handle_);
        handle_ = nullptr;
        throw DbError("cannot open database '" + path + "': " + msg);
    }
    // WAL keeps readers from blocking the writer, which matters because a
    // streaming answer writes its final message while the UI polls history.
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
    exec("PRAGMA synchronous=NORMAL;");
    sqlite3_busy_timeout(handle_, 5000);
}

Database::~Database() {
    if (handle_) sqlite3_close(handle_);
}

void Database::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(handle_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw DbError("SQL failed: " + msg);
    }
}

int64_t Database::last_insert_rowid() { return sqlite3_last_insert_rowid(handle_); }

int Database::changes() { return sqlite3_changes(handle_); }

void Database::migrate() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS schema_meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

-- ---------------------------------------------------------------- accounts
CREATE TABLE IF NOT EXISTS users (
  id             INTEGER PRIMARY KEY AUTOINCREMENT,
  email          TEXT    NOT NULL,
  email_lower    TEXT    NOT NULL UNIQUE,
  display_name   TEXT    NOT NULL,
  password_hash  TEXT    NOT NULL,
  email_verified INTEGER NOT NULL DEFAULT 0,
  created_at     INTEGER NOT NULL,
  last_login_at  INTEGER,
  failed_logins  INTEGER NOT NULL DEFAULT 0,
  locked_until   INTEGER NOT NULL DEFAULT 0
);

-- Sessions store only a SHA-256 of the bearer token, so a stolen database
-- file does not hand the attacker a set of live sessions.
CREATE TABLE IF NOT EXISTS sessions (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id       INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token_hash    TEXT    NOT NULL UNIQUE,
  created_at    INTEGER NOT NULL,
  expires_at    INTEGER NOT NULL,
  last_seen_at  INTEGER NOT NULL,
  revoked_at    INTEGER
);
CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);
CREATE INDEX IF NOT EXISTS idx_sessions_expiry ON sessions(expires_at);

-- Short-lived emailed codes: 2FA at login, address confirmation at signup,
-- and confirmation of an account deletion.
CREATE TABLE IF NOT EXISTS email_codes (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id      INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  purpose      TEXT    NOT NULL,
  code_hash    TEXT    NOT NULL,
  challenge_id TEXT    NOT NULL UNIQUE,
  created_at   INTEGER NOT NULL,
  expires_at   INTEGER NOT NULL,
  attempts     INTEGER NOT NULL DEFAULT 0,
  consumed_at  INTEGER
);
CREATE INDEX IF NOT EXISTS idx_email_codes_user ON email_codes(user_id, purpose);

-- Per-user settings (currently the Fast/Smart model choice). A key/value table
-- rather than columns on `users`, so a new setting needs no migration.
CREATE TABLE IF NOT EXISTS user_prefs (
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  key     TEXT    NOT NULL,
  value   TEXT    NOT NULL,
  PRIMARY KEY (user_id, key)
);

-- ------------------------------------------------------------------- chat
CREATE TABLE IF NOT EXISTS chats (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  title      TEXT    NOT NULL,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  deleted_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_chats_user ON chats(user_id, updated_at DESC);

CREATE TABLE IF NOT EXISTS messages (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  chat_id     INTEGER NOT NULL REFERENCES chats(id) ON DELETE CASCADE,
  role        TEXT    NOT NULL,          -- 'user' | 'assistant'
  content     TEXT    NOT NULL,
  created_at  INTEGER NOT NULL,
  off_topic   INTEGER NOT NULL DEFAULT 0,
  sources     TEXT                        -- JSON array of retrieved citations
);
CREATE INDEX IF NOT EXISTS idx_messages_chat ON messages(chat_id, id);

-- ---------------------------------------------------- retrieval / corpus
CREATE TABLE IF NOT EXISTS doc_chunks (
  id       INTEGER PRIMARY KEY AUTOINCREMENT,
  source   TEXT    NOT NULL,             -- textbook / dataset name
  subject  TEXT    NOT NULL,             -- 'math' | 'physics'
  grade    TEXT,                         -- e.g. '7' or '6-8'
  locator  TEXT,                         -- page or section reference
  content  TEXT    NOT NULL,
  token_est INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_chunks_subject ON doc_chunks(subject);

-- Embeddings are stored as a raw little-endian float32 blob. The corpus is
-- small enough (tens of thousands of chunks) that a linear cosine scan in
-- memory beats pulling in a vector-index dependency.
CREATE TABLE IF NOT EXISTS doc_vectors (
  chunk_id INTEGER PRIMARY KEY REFERENCES doc_chunks(id) ON DELETE CASCADE,
  dim      INTEGER NOT NULL,
  model    TEXT    NOT NULL,
  vec      BLOB    NOT NULL
);

-- Full-text index so retrieval still works before embeddings are built.
CREATE VIRTUAL TABLE IF NOT EXISTS doc_fts USING fts5(
  content,
  content='doc_chunks',
  content_rowid='id',
  tokenize='porter unicode61'
);
CREATE TRIGGER IF NOT EXISTS doc_chunks_ai AFTER INSERT ON doc_chunks BEGIN
  INSERT INTO doc_fts(rowid, content) VALUES (new.id, new.content);
END;
CREATE TRIGGER IF NOT EXISTS doc_chunks_ad AFTER DELETE ON doc_chunks BEGIN
  INSERT INTO doc_fts(doc_fts, rowid, content) VALUES('delete', old.id, old.content);
END;
CREATE TRIGGER IF NOT EXISTS doc_chunks_au AFTER UPDATE ON doc_chunks BEGIN
  INSERT INTO doc_fts(doc_fts, rowid, content) VALUES('delete', old.id, old.content);
  INSERT INTO doc_fts(rowid, content) VALUES (new.id, new.content);
END;

-- ------------------------------------------------------------------ audit
CREATE TABLE IF NOT EXISTS audit_log (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id    INTEGER,
  event      TEXT    NOT NULL,
  detail     TEXT,
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_audit_user ON audit_log(user_id, id DESC);
)SQL");
    exec("INSERT OR REPLACE INTO schema_meta(key, value) VALUES('version', '1');");
}

// -------------------------------------------------------------------- Stmt

Stmt::Stmt(Database& database, const std::string& sql) : db_(database) {
    if (sqlite3_prepare_v2(db_.handle(), sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
        throw DbError(std::string("prepare failed: ") + sqlite3_errmsg(db_.handle()) +
                      " -- SQL: " + sql);
    }
}

Stmt::~Stmt() {
    if (stmt_) sqlite3_finalize(stmt_);
}

Stmt::Stmt(Stmt&& other) noexcept : db_(other.db_), stmt_(other.stmt_) { other.stmt_ = nullptr; }

Stmt& Stmt::bind(int index, int64_t value) {
    if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) throw DbError("bind int failed");
    return *this;
}

Stmt& Stmt::bind(int index, const std::string& value) {
    // SQLITE_TRANSIENT: SQLite copies, so the caller's string may die first.
    if (sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        throw DbError("bind text failed");
    }
    return *this;
}

Stmt& Stmt::bind(int index, const char* value) { return bind(index, std::string(value)); }

Stmt& Stmt::bind_blob(int index, const void* data, size_t len) {
    if (sqlite3_bind_blob(stmt_, index, data, static_cast<int>(len), SQLITE_TRANSIENT) !=
        SQLITE_OK) {
        throw DbError("bind blob failed");
    }
    return *this;
}

Stmt& Stmt::bind_null(int index) {
    if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) throw DbError("bind null failed");
    return *this;
}

bool Stmt::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw DbError(std::string("step failed: ") + sqlite3_errmsg(db_.handle()));
}

void Stmt::run() {
    while (step()) {
    }
}

void Stmt::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

bool Stmt::is_null(int col) const {
    return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

int64_t Stmt::col_int(int col) const { return sqlite3_column_int64(stmt_, col); }

double Stmt::col_double(int col) const { return sqlite3_column_double(stmt_, col); }

std::string Stmt::col_text(int col) const {
    const unsigned char* p = sqlite3_column_text(stmt_, col);
    if (!p) return {};
    int n = sqlite3_column_bytes(stmt_, col);
    return std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(n));
}

std::vector<uint8_t> Stmt::col_blob(int col) const {
    const void* p = sqlite3_column_blob(stmt_, col);
    int n = sqlite3_column_bytes(stmt_, col);
    if (!p || n <= 0) return {};
    const uint8_t* b = static_cast<const uint8_t*>(p);
    return std::vector<uint8_t>(b, b + n);
}

// ------------------------------------------------------------- Transaction

Transaction::Transaction(Database& database) : db_(database) { db_.exec("BEGIN IMMEDIATE;"); }

Transaction::~Transaction() {
    if (active_) {
        // Destructors must not throw; a failed rollback is already fatal to
        // this transaction and SQLite will unwind it when the handle closes.
        try {
            db_.exec("ROLLBACK;");
        } catch (const std::exception&) {
        }
    }
}

void Transaction::commit() {
    if (!active_) return;
    db_.exec("COMMIT;");
    active_ = false;
}

}  // namespace db
