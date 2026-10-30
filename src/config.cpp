#include "config.h"

#include <cstdlib>
#include <fstream>

#include "util.h"

std::string Config::env_key(const std::string& key) {
    std::string out = "MSTUTOR_";
    for (char c : key) out.push_back(c == '.' ? '_' : static_cast<char>(std::toupper(c)));
    return out;
}

void Config::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;  // defaults + environment still apply

    std::string line;
    std::string section;
    while (std::getline(in, line)) {
        // Strip a trailing CR so CRLF files parse identically.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = util::trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;

        if (t.front() == '[' && t.back() == ']') {
            section = util::to_lower(util::trim(t.substr(1, t.size() - 2)));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = util::to_lower(util::trim(t.substr(0, eq)));
        std::string value = util::trim(t.substr(eq + 1));
        // Allow quoting so values may keep leading/trailing spaces.
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (key.empty()) continue;
        values_[section.empty() ? key : section + "." + key] = value;
    }
}

void Config::set(const std::string& key, const std::string& value) {
    values_[util::to_lower(key)] = value;
}

std::string Config::get(const std::string& key, const std::string& fallback) const {
    if (const char* env = std::getenv(env_key(key).c_str())) {
        if (*env) return env;
    }
    auto it = values_.find(key);
    if (it != values_.end() && !it->second.empty()) return it->second;
    return fallback;
}

int64_t Config::get_int(const std::string& key, int64_t fallback) const {
    std::string raw = get(key);
    if (raw.empty()) return fallback;
    try {
        return std::stoll(raw);
    } catch (const std::exception&) {
        return fallback;  // a typo in the config must not take the server down
    }
}

double Config::get_double(const std::string& key, double fallback) const {
    std::string raw = get(key);
    if (raw.empty()) return fallback;
    try {
        return std::stod(raw);
    } catch (const std::exception&) {
        return fallback;
    }
}

bool Config::get_bool(const std::string& key, bool fallback) const {
    std::string raw = util::to_lower(get(key));
    if (raw.empty()) return fallback;
    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") return true;
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off") return false;
    return fallback;
}
