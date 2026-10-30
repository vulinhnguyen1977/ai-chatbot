#include "prefs.h"

#include <mutex>

namespace prefs {

std::string get(db::Database& database, int64_t user_id, const std::string& key,
                const std::string& fallback) {
    if (user_id <= 0) return fallback;
    try {
        std::lock_guard<std::mutex> lock(database.mutex());
        db::Stmt s(database, "SELECT value FROM user_prefs WHERE user_id=? AND key=?");
        s.bind(1, user_id).bind(2, key);
        if (s.step()) return s.col_text(0);
    } catch (const std::exception&) {
    }
    return fallback;
}

void set(db::Database& database, int64_t user_id, const std::string& key,
         const std::string& value) {
    if (user_id <= 0) return;
    try {
        std::lock_guard<std::mutex> lock(database.mutex());
        db::Stmt s(database,
                   "INSERT INTO user_prefs(user_id, key, value) VALUES(?,?,?) "
                   "ON CONFLICT(user_id, key) DO UPDATE SET value=excluded.value");
        s.bind(1, user_id).bind(2, key).bind(3, value);
        s.run();
    } catch (const std::exception&) {
    }
}

std::string get_app(db::Database& database, const std::string& key,
                    const std::string& fallback) {
    try {
        std::lock_guard<std::mutex> lock(database.mutex());
        db::Stmt s(database, "SELECT value FROM schema_meta WHERE key=?");
        s.bind(1, key);
        if (s.step()) return s.col_text(0);
    } catch (const std::exception&) {
    }
    return fallback;
}

void set_app(db::Database& database, const std::string& key, const std::string& value) {
    try {
        std::lock_guard<std::mutex> lock(database.mutex());
        db::Stmt s(database, "INSERT OR REPLACE INTO schema_meta(key, value) VALUES(?,?)");
        s.bind(1, key).bind(2, value);
        s.run();
    } catch (const std::exception&) {
    }
}

}  // namespace prefs
