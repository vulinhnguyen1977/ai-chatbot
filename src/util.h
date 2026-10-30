// util.h -- small, dependency-free helpers shared across the application.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace util {

// ---- time -------------------------------------------------------------
// Seconds since the Unix epoch (UTC).
int64_t now_unix();
// Formats a Unix timestamp as "YYYY-MM-DD HH:MM:SS" in UTC.
std::string format_unix(int64_t ts);
// RFC 5322 date, e.g. "Fri, 08 Aug 2026 12:00:00 +0000". Used in mail headers.
std::string rfc5322_date(int64_t ts);

// ---- encoding ---------------------------------------------------------
std::string to_hex(const std::vector<uint8_t>& bytes);
std::string base64_encode(const uint8_t* data, size_t len);
std::string base64_encode(const std::string& s);

// ---- strings ----------------------------------------------------------
std::string trim(const std::string& s);
std::string to_lower(const std::string& s);
bool starts_with(const std::string& s, const std::string& prefix);
bool contains(const std::string& haystack, const std::string& needle);
// Case-insensitive whole-word search. Word boundaries are non-alphanumerics,
// so "work" does not match inside "homework".
bool contains_word(const std::string& lower_haystack, const std::string& lower_word);
std::vector<std::string> split(const std::string& s, char delim);
// Truncates on a UTF-8 code point boundary so we never emit half a character.
std::string utf8_truncate(const std::string& s, size_t max_bytes);

// ---- validation -------------------------------------------------------
bool is_valid_email(const std::string& email);
// Returns an empty string when acceptable, otherwise a human-readable reason.
std::string password_problem(const std::string& password);

}  // namespace util
