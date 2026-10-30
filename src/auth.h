// auth.h -- accounts, sessions and email two-factor verification.
//
// Design notes:
//  * Sessions carry an absolute expiry of `auth.session_days` (180 by default)
//    measured from issue, so every user is forced back through login and 2FA
//    at least twice a year regardless of activity.
//  * Only a SHA-256 of the session token is persisted.
//  * Login responses are deliberately uniform: an unknown address and a wrong
//    password produce the same message, so the API cannot be used to enumerate
//    registered users.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db.h"

class Config;
class Mailer;

struct User {
    int64_t id = 0;
    std::string email;
    std::string display_name;
    bool email_verified = false;
    int64_t created_at = 0;
    int64_t last_login_at = 0;
};

struct AuthResult {
    enum class Status {
        Ok,                 // session_token is populated
        NeedsVerification,  // challenge_id is populated; a code was emailed
        Invalid,            // bad credentials / bad code -- message is user-safe
        Locked,             // too many failed attempts
        Error               // unexpected server-side failure
    };

    Status status = Status::Error;
    std::string message;
    std::string challenge_id;
    std::string session_token;
    int64_t session_expires_at = 0;
    User user;
    // True when the code went to an on-disk outbox rather than a real mailbox.
    bool code_in_outbox = false;

    bool ok() const { return status == Status::Ok; }
};

// Purposes for an emailed one-time code.
namespace code_purpose {
inline constexpr const char* kSignup = "signup_verify";
inline constexpr const char* kLogin = "login_2fa";
inline constexpr const char* kDelete = "delete_account";
}  // namespace code_purpose

class Auth {
public:
    Auth(db::Database& database, const Config& config, Mailer& mailer);

    // Creates the account and emails a confirmation code. The account exists
    // but cannot log in until the address is confirmed.
    AuthResult sign_up(const std::string& email, const std::string& display_name,
                       const std::string& password);

    // Verifies the password, then emails a 2FA code. Never returns a session
    // directly while auth.require_2fa is on.
    AuthResult log_in(const std::string& email, const std::string& password);

    // Consumes an emailed code. On success for signup/login purposes this
    // issues the session.
    AuthResult verify_code(const std::string& challenge_id, const std::string& code);

    // Re-sends a code for an outstanding challenge, invalidating the old one.
    AuthResult resend_code(const std::string& challenge_id);

    // Resolves a bearer token to a user, enforcing both the absolute expiry and
    // the idle timeout. Returns false when the session is missing, revoked,
    // past its 180-day life, or unused for longer than auth.remember_days.
    bool authenticate(const std::string& token, User* out);

    // Restores a sign-in from a token that was persisted across a restart.
    //
    // On success the token is rotated: a fresh one is issued and the old one is
    // revoked, so a copy of the stored file taken earlier stops working as soon
    // as the real user opens the app. The replacement inherits the original
    // absolute expiry, so rotation cannot be used to extend a session past the
    // 180-day cap.
    AuthResult restore_session(const std::string& token);

    void log_out(const std::string& token);
    void log_out_all_sessions(int64_t user_id);

    // Step 1 of account deletion: emails a confirmation code.
    AuthResult request_account_deletion(int64_t user_id);
    // Step 2: verifies the code and removes the user and all owned rows.
    AuthResult confirm_account_deletion(int64_t user_id, const std::string& challenge_id,
                                        const std::string& code);

    // Deletes expired sessions and stale codes. Called at startup and hourly.
    void purge_expired();

    void audit(int64_t user_id, const std::string& event, const std::string& detail);

private:
    // Creates a code row and mails it. Returns the challenge id, or "" on error.
    std::string issue_code(const User& user, const std::string& purpose, std::string* error,
                           bool* went_to_outbox);
    // `inherited_expiry` of 0 starts the 180-day clock now; a non-zero value
    // carries forward an existing session's absolute deadline, which is what
    // keeps token rotation from resetting it.
    AuthResult start_session(const User& user, int64_t inherited_expiry = 0);
    bool load_user_by_id(int64_t id, User* out);
    bool load_user_by_email(const std::string& email, User* out, std::string* password_hash);

    db::Database& db_;
    const Config& config_;
    Mailer& mailer_;
};
