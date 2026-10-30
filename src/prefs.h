// prefs.h -- small per-user key/value settings.
//
// Kept in its own table rather than as columns on `users` so adding a setting
// never needs a schema migration on an existing install.
#pragma once

#include <cstdint>
#include <string>

#include "db.h"

namespace prefs {

// Keys currently in use.
inline constexpr const char* kModelPreset = "model_preset";
// Application-wide echo of the last preset anyone selected. The model server
// starts before anyone has signed in, so the per-user value is not yet known;
// this lets the common single-user case still boot straight into the right
// model instead of loading one and immediately swapping it.
inline constexpr const char* kLastModelPreset = "last_model_preset";

// Returns `fallback` when the user has never set the key.
std::string get(db::Database& database, int64_t user_id, const std::string& key,
                const std::string& fallback);

// Stores the value, replacing any previous one. Failures are swallowed: a
// preference that cannot be saved must never break the action the user was
// actually performing.
void set(db::Database& database, int64_t user_id, const std::string& key,
         const std::string& value);

// Application-wide settings, not tied to an account.
std::string get_app(db::Database& database, const std::string& key,
                    const std::string& fallback);
void set_app(db::Database& database, const std::string& key, const std::string& value);

}  // namespace prefs
