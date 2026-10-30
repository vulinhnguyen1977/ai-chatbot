#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace util {

int64_t now_unix() { return static_cast<int64_t>(std::time(nullptr)); }

static std::tm gmtime_utc(int64_t ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm out{};
#if defined(_WIN32)
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
    return out;
}

std::string format_unix(int64_t ts) {
    std::tm tm = gmtime_utc(ts);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string rfc5322_date(int64_t ts) {
    static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    std::tm tm = gmtime_utc(ts);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d +0000", kDays[tm.tm_wday],
                  tm.tm_mday, kMonths[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min,
                  tm.tm_sec);
    return buf;
}

std::string to_hex(const std::vector<uint8_t>& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::string base64_encode(const uint8_t* data, size_t len) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < len) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(kTable[(n >> 6) & 63]);
        out.push_back(kTable[n & 63]);
        i += 3;
    }
    if (i + 1 == len) {
        uint32_t n = uint32_t(data[i]) << 16;
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.append("==");
    } else if (i + 2 == len) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(kTable[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::string base64_encode(const std::string& s) {
    return base64_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool contains_word(const std::string& lower_haystack, const std::string& lower_word) {
    if (lower_word.empty()) return false;
    size_t pos = 0;
    while ((pos = lower_haystack.find(lower_word, pos)) != std::string::npos) {
        bool left_ok = pos == 0 || !std::isalnum(static_cast<unsigned char>(lower_haystack[pos - 1]));
        size_t after = pos + lower_word.size();
        bool right_ok =
            after >= lower_haystack.size() ||
            !std::isalnum(static_cast<unsigned char>(lower_haystack[after]));
        if (left_ok && right_ok) return true;
        pos += 1;
    }
    return false;
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    return parts;
}

std::string utf8_truncate(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    // Back off while we are on a UTF-8 continuation byte (10xxxxxx).
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

bool is_valid_email(const std::string& email) {
    // Deliberately permissive: reject the obviously malformed and let the
    // 2FA round-trip prove the address actually works.
    if (email.size() < 6 || email.size() > 254) return false;
    size_t at = email.find('@');
    if (at == std::string::npos || at == 0) return false;
    if (email.find('@', at + 1) != std::string::npos) return false;
    const std::string domain = email.substr(at + 1);
    if (domain.size() < 3) return false;
    size_t dot = domain.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 2 > domain.size()) return false;
    for (char c : email) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == ';') return false;
        if (c == '\r' || c == '\n') return false;  // header-injection guard
    }
    if (domain.front() == '.' || domain.back() == '.') return false;
    if (util::contains(domain, "..")) return false;
    return true;
}

std::string password_problem(const std::string& password) {
    if (password.size() < 10) return "Password must be at least 10 characters.";
    if (password.size() > 200) return "Password must be at most 200 characters.";
    bool has_letter = false, has_digit = false;
    for (unsigned char c : password) {
        if (std::isalpha(c)) has_letter = true;
        if (std::isdigit(c)) has_digit = true;
    }
    if (!has_letter || !has_digit)
        return "Password must contain at least one letter and one number.";
    return "";
}

}  // namespace util
