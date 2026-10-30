// config.h -- INI-style configuration with environment overrides.
//
// Precedence: environment variable > config file > built-in default.
// Every key maps to an env var by upper-casing and replacing '.' with '_',
// e.g. `mail.password` -> MSTUTOR_MAIL_PASSWORD. Secrets should be supplied
// through the environment so the shipped config file can stay in the zip.
#pragma once

#include <cstdint>
#include <map>
#include <string>

class Config {
public:
    // Loads `path` if it exists; missing file is not an error (defaults apply).
    void load(const std::string& path);

    std::string get(const std::string& key, const std::string& fallback = "") const;
    int64_t get_int(const std::string& key, int64_t fallback) const;
    double get_double(const std::string& key, double fallback) const;
    bool get_bool(const std::string& key, bool fallback) const;

    void set(const std::string& key, const std::string& value);

    // --- typed accessors for the settings used across the app -------------
    std::string db_path() const { return get("db.path", "data/mstutor.db"); }

    // --- model backend ----------------------------------------------------
    // "llamacpp" uses the engine and weights bundled in runtime/ and needs
    // nothing installed. "ollama" talks to an existing Ollama daemon instead.
    std::string llm_backend() const { return get("llm.backend", "llamacpp"); }
    // Only consulted when the bundled runtime is unavailable or the backend is
    // Ollama; otherwise the runtime reports the port it actually bound.
    std::string llm_url() const {
        return get("llm.url", llm_backend() == "ollama" ? "http://127.0.0.1:11434"
                                                        : "http://127.0.0.1:11500");
    }
    std::string llm_embed_url() const { return get("llm.embed_url", ""); }

    std::string ollama_model() const { return get("llm.model", "mstutor"); }
    std::string ollama_embed_model() const { return get("llm.embed_model", "nomic-embed-text"); }
    std::string guard_model_name() const {
        // Defaults to the main model so a stock install needs only one download.
        return get("llm.guard_model", ollama_model());
    }
    // "" leaves the model's own default; otherwise "true"/"false"/"low"/
    // "medium"/"high". See ChatOptions::think for why this is rarely turned off.
    std::string ollama_think() const { return get("llm.think", ""); }
    double ollama_temperature() const { return get_double("llm.temperature", 0.2); }
    int ollama_num_ctx() const { return static_cast<int>(get_int("llm.num_ctx", 8192)); }
    int ollama_timeout_s() const { return static_cast<int>(get_int("llm.timeout_s", 300)); }

    // Requirement: sessions expire 180 days after issue. This is an absolute
    // cap measured from sign-in and is never extended, including by the
    // "keep me signed in" option below.
    int64_t session_days() const { return get_int("auth.session_days", 180); }

    // Lets the app restore a sign-in after a restart.
    bool remember_enabled() const { return get_bool("auth.remember_enabled", true); }
    // Idle timeout: a session unused for this long stops working and the user
    // signs in again. 30 days is the usual ceiling for a "remember me" token,
    // and suits a local, single-user desktop app whose stored credential is
    // already encrypted to the Windows account.
    int64_t remember_days() const { return get_int("auth.remember_days", 30); }
    bool require_2fa() const { return get_bool("auth.require_2fa", true); }
    int64_t code_ttl_minutes() const { return get_int("auth.code_ttl_minutes", 10); }
    int max_code_attempts() const { return static_cast<int>(get_int("auth.max_code_attempts", 5)); }
    int max_failed_logins() const {
        return static_cast<int>(get_int("auth.max_failed_logins", 8));
    }
    int64_t lockout_minutes() const { return get_int("auth.lockout_minutes", 15); }

    // "smtp" sends real mail; "file" writes the message to mail.outbox_dir so
    // the whole 2FA flow can be exercised without credentials.
    std::string mail_transport() const { return get("mail.transport", "file"); }
    std::string mail_host() const { return get("mail.host", "smtp.gmail.com"); }
    int mail_port() const { return static_cast<int>(get_int("mail.port", 587)); }
    std::string mail_security() const { return get("mail.security", "starttls"); }
    std::string mail_username() const { return get("mail.username", ""); }
    std::string mail_password() const { return get("mail.password", ""); }
    std::string mail_from() const { return get("mail.from", mail_username()); }
    std::string mail_from_name() const { return get("mail.from_name", "Middle School Tutor"); }
    std::string mail_outbox() const { return get("mail.outbox_dir", "data/outbox"); }

    // --- answer-style presets ---------------------------------------------
    // The two entries the dropdown offers. Each names a file in runtime/models;
    // a preset whose file is absent is simply not offered.
    std::string preset_fast_model() const {
        return get("presets.fast", "qwen3-1.7b.gguf");
    }
    std::string preset_smart_model() const {
        return get("presets.smart", "qwen3-4b.gguf");
    }
    // Which preset a brand new account starts on.
    std::string preset_default() const { return get("presets.default", "smart"); }

    bool rag_enabled() const { return get_bool("rag.enabled", true); }
    int rag_top_k() const { return static_cast<int>(get_int("rag.top_k", 4)); }
    double rag_min_score() const { return get_double("rag.min_score", 0.35); }

    // When false the model-based topic classifier is skipped and only the
    // fast lexical gate runs. Useful on very small machines.
    bool guard_use_model() const { return get_bool("guard.use_model", true); }

    const std::map<std::string, std::string>& values() const { return values_; }

private:
    std::map<std::string, std::string> values_;
    static std::string env_key(const std::string& key);
};
