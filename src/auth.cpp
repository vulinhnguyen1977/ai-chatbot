#include "auth.h"

#include <mutex>

#include "config.h"
#include "crypto.h"
#include "mailer.h"
#include "util.h"

namespace {

constexpr int64_t kSecondsPerDay = 86400;

AuthResult make_error(const std::string& message) {
    AuthResult r;
    r.status = AuthResult::Status::Error;
    r.message = message;
    return r;
}

AuthResult make_invalid(const std::string& message) {
    AuthResult r;
    r.status = AuthResult::Status::Invalid;
    r.message = message;
    return r;
}

std::string code_email_body(const std::string& name, const std::string& code,
                            const std::string& purpose, int64_t ttl_minutes) {
    std::string what = "verify your email address";
    if (purpose == code_purpose::kLogin) what = "finish signing in";
    else if (purpose == code_purpose::kDelete) what = "permanently delete your account";

    std::string body;
    body += "Hi " + (name.empty() ? std::string("there") : name) + ",\n\n";
    body += "Use this code to " + what + ":\n\n";
    body += "    " + code + "\n\n";
    body += "The code expires in " + std::to_string(ttl_minutes) + " minutes.\n";
    if (purpose == code_purpose::kDelete) {
        body +=
            "\nDeleting your account removes every chat and message you have saved.\n"
            "This cannot be undone.\n";
    }
    body += "\nIf you did not request this, you can ignore this email";
    if (purpose == code_purpose::kDelete) body += " -- nothing will be deleted";
    body += ".\n\n-- Middle School Maths & Physics Tutor\n";
    return body;
}

std::string code_email_subject(const std::string& purpose, const std::string& code) {
    if (purpose == code_purpose::kDelete) return "Confirm account deletion (" + code + ")";
    if (purpose == code_purpose::kLogin) return code + " is your sign-in code";
    return code + " is your verification code";
}

}  // namespace

Auth::Auth(db::Database& database, const Config& config, Mailer& mailer)
    : db_(database), config_(config), mailer_(mailer) {}

void Auth::audit(int64_t user_id, const std::string& event, const std::string& detail) {
    try {
        db::Stmt s(db_,
                   "INSERT INTO audit_log(user_id, event, detail, created_at) VALUES(?,?,?,?)");
        if (user_id > 0) s.bind(1, user_id); else s.bind_null(1);
        s.bind(2, event).bind(3, detail).bind(4, util::now_unix());
        s.run();
    } catch (const std::exception&) {
        // Audit is best-effort; never fail an operation because logging failed.
    }
}

bool Auth::load_user_by_id(int64_t id, User* out) {
    db::Stmt s(db_,
               "SELECT id, email, display_name, email_verified, created_at, "
               "COALESCE(last_login_at,0) FROM users WHERE id=?");
    s.bind(1, id);
    if (!s.step()) return false;
    out->id = s.col_int(0);
    out->email = s.col_text(1);
    out->display_name = s.col_text(2);
    out->email_verified = s.col_int(3) != 0;
    out->created_at = s.col_int(4);
    out->last_login_at = s.col_int(5);
    return true;
}

bool Auth::load_user_by_email(const std::string& email, User* out, std::string* password_hash) {
    db::Stmt s(db_,
               "SELECT id, email, display_name, email_verified, created_at, "
               "COALESCE(last_login_at,0), password_hash, failed_logins, locked_until "
               "FROM users WHERE email_lower=?");
    s.bind(1, util::to_lower(util::trim(email)));
    if (!s.step()) return false;
    out->id = s.col_int(0);
    out->email = s.col_text(1);
    out->display_name = s.col_text(2);
    out->email_verified = s.col_int(3) != 0;
    out->created_at = s.col_int(4);
    out->last_login_at = s.col_int(5);
    if (password_hash) *password_hash = s.col_text(6);
    return true;
}

std::string Auth::issue_code(const User& user, const std::string& purpose, std::string* error,
                             bool* went_to_outbox) {
    const std::string code = crypto::random_digits(6);
    const std::string challenge_id = crypto::random_token_hex(16);
    const int64_t now = util::now_unix();
    const int64_t expires = now + config_.code_ttl_minutes() * 60;

    {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Transaction tx(db_);
        // Supersede any outstanding code for the same purpose so only the most
        // recent email is usable.
        db::Stmt clear(db_,
                       "UPDATE email_codes SET consumed_at=? "
                       "WHERE user_id=? AND purpose=? AND consumed_at IS NULL");
        clear.bind(1, now).bind(2, user.id).bind(3, purpose);
        clear.run();

        db::Stmt ins(db_,
                     "INSERT INTO email_codes(user_id, purpose, code_hash, challenge_id, "
                     "created_at, expires_at) VALUES(?,?,?,?,?,?)");
        ins.bind(1, user.id)
            .bind(2, purpose)
            .bind(3, crypto::hash_short_code(code))
            .bind(4, challenge_id)
            .bind(5, now)
            .bind(6, expires);
        ins.run();
        tx.commit();
    }

    // Network I/O happens outside the database lock on purpose: an unreachable
    // SMTP server must not stall unrelated requests.
    Email mail;
    mail.to = user.email;
    mail.to_name = user.display_name;
    mail.subject = code_email_subject(purpose, code);
    mail.body = code_email_body(user.display_name, code, purpose, config_.code_ttl_minutes());

    std::string mail_error;
    if (!mailer_.send(mail, &mail_error)) {
        if (error) *error = mail_error;
        audit(user.id, "email_failed", purpose + ": " + mail_error);
        return "";
    }
    if (went_to_outbox) *went_to_outbox = !mailer_.is_live();
    return challenge_id;
}

AuthResult Auth::start_session(const User& user, int64_t inherited_expiry) {
    const std::string token = crypto::random_token_hex(32);
    const int64_t now = util::now_unix();
    const int64_t fresh_expiry = now + config_.session_days() * kSecondsPerDay;
    // Never let an inherited deadline push past a fresh one: it can only ever
    // shorten the life of the replacement, never extend it.
    const int64_t expires = (inherited_expiry > 0 && inherited_expiry < fresh_expiry)
                                ? inherited_expiry
                                : fresh_expiry;

    {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_,
                   "INSERT INTO sessions(user_id, token_hash, created_at, expires_at, "
                   "last_seen_at) VALUES(?,?,?,?,?)");
        s.bind(1, user.id)
            .bind(2, crypto::sha256_hex(token))
            .bind(3, now)
            .bind(4, expires)
            .bind(5, now);
        s.run();

        db::Stmt upd(db_, "UPDATE users SET last_login_at=?, failed_logins=0, locked_until=0 "
                          "WHERE id=?");
        upd.bind(1, now).bind(2, user.id);
        upd.run();
    }

    AuthResult r;
    r.status = AuthResult::Status::Ok;
    r.session_token = token;
    r.session_expires_at = expires;
    r.user = user;
    r.user.last_login_at = now;
    audit(user.id, "session_started",
          "expires " + util::format_unix(expires) + " UTC");
    return r;
}

AuthResult Auth::sign_up(const std::string& email_raw, const std::string& display_name_raw,
                         const std::string& password) {
    const std::string email = util::trim(email_raw);
    const std::string display_name = util::trim(display_name_raw);

    if (!util::is_valid_email(email)) return make_invalid("Enter a valid email address.");
    if (display_name.empty() || display_name.size() > 60) {
        return make_invalid("Name must be between 1 and 60 characters.");
    }
    std::string pw_problem = util::password_problem(password);
    if (!pw_problem.empty()) return make_invalid(pw_problem);

    User user;
    const int64_t now = util::now_unix();
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt existing(db_, "SELECT id, email_verified FROM users WHERE email_lower=?");
        existing.bind(1, util::to_lower(email));
        if (existing.step()) {
            const int64_t id = existing.col_int(0);
            const bool verified = existing.col_int(1) != 0;
            if (verified) {
                // Do not confirm that the address is registered.
                return make_invalid(
                    "If that address can be registered, a verification code is on its way.");
            }
            // Unverified signup being retried: refresh the credentials so a
            // typo'd password on the first attempt is not permanently stuck.
            db::Stmt upd(db_,
                         "UPDATE users SET display_name=?, password_hash=?, email=? WHERE id=?");
            upd.bind(1, display_name)
                .bind(2, crypto::hash_password(password))
                .bind(3, email)
                .bind(4, id);
            upd.run();
            user.id = id;
        } else {
            db::Stmt ins(db_,
                         "INSERT INTO users(email, email_lower, display_name, password_hash, "
                         "email_verified, created_at) VALUES(?,?,?,?,0,?)");
            ins.bind(1, email)
                .bind(2, util::to_lower(email))
                .bind(3, display_name)
                .bind(4, crypto::hash_password(password))
                .bind(5, now);
            ins.run();
            user.id = db_.last_insert_rowid();
        }
    } catch (const std::exception& e) {
        return make_error(std::string("Could not create the account: ") + e.what());
    }

    user.email = email;
    user.display_name = display_name;
    user.created_at = now;

    std::string error;
    bool outbox = false;
    std::string challenge = issue_code(user, code_purpose::kSignup, &error, &outbox);
    if (challenge.empty()) {
        return make_error("Account created, but the verification email could not be sent: " +
                          error);
    }

    audit(user.id, "signup", email);
    AuthResult r;
    r.status = AuthResult::Status::NeedsVerification;
    r.challenge_id = challenge;
    r.user = user;
    r.code_in_outbox = outbox;
    r.message = "We sent a 6-digit code to " + email + ".";
    return r;
}

AuthResult Auth::log_in(const std::string& email_raw, const std::string& password) {
    const std::string email = util::trim(email_raw);
    // One message for every credential failure, so the endpoint cannot be used
    // to discover which addresses have accounts.
    const std::string kGeneric = "Incorrect email or password.";

    User user;
    std::string password_hash;
    int64_t locked_until = 0;
    int failed = 0;

    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_,
                   "SELECT id, email, display_name, email_verified, created_at, "
                   "COALESCE(last_login_at,0), password_hash, failed_logins, locked_until "
                   "FROM users WHERE email_lower=?");
        s.bind(1, util::to_lower(email));
        if (!s.step()) {
            // Spend comparable time on an unknown address so response latency
            // does not leak account existence.
            crypto::hash_password(password);
            return make_invalid(kGeneric);
        }
        user.id = s.col_int(0);
        user.email = s.col_text(1);
        user.display_name = s.col_text(2);
        user.email_verified = s.col_int(3) != 0;
        user.created_at = s.col_int(4);
        user.last_login_at = s.col_int(5);
        password_hash = s.col_text(6);
        failed = static_cast<int>(s.col_int(7));
        locked_until = s.col_int(8);
    } catch (const std::exception& e) {
        return make_error(std::string("Login failed: ") + e.what());
    }

    const int64_t now = util::now_unix();
    if (locked_until > now) {
        AuthResult r;
        r.status = AuthResult::Status::Locked;
        r.message = "Too many failed attempts. Try again in " +
                    std::to_string((locked_until - now) / 60 + 1) + " minute(s).";
        return r;
    }

    if (!crypto::verify_password(password, password_hash)) {
        std::lock_guard<std::mutex> lock(db_.mutex());
        const int next_failed = failed + 1;
        int64_t lock_until = 0;
        if (next_failed >= config_.max_failed_logins()) {
            lock_until = now + config_.lockout_minutes() * 60;
        }
        db::Stmt upd(db_, "UPDATE users SET failed_logins=?, locked_until=? WHERE id=?");
        upd.bind(1, static_cast<int64_t>(next_failed)).bind(2, lock_until).bind(3, user.id);
        upd.run();
        audit(user.id, "login_failed", "attempt " + std::to_string(next_failed));
        return make_invalid(kGeneric);
    }

    {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt upd(db_, "UPDATE users SET failed_logins=0, locked_until=0 WHERE id=?");
        upd.bind(1, user.id);
        upd.run();
    }

    const char* purpose =
        user.email_verified ? code_purpose::kLogin : code_purpose::kSignup;

    if (user.email_verified && !config_.require_2fa()) {
        return start_session(user);
    }

    std::string error;
    bool outbox = false;
    std::string challenge = issue_code(user, purpose, &error, &outbox);
    if (challenge.empty()) return make_error("Could not send the verification email: " + error);

    AuthResult r;
    r.status = AuthResult::Status::NeedsVerification;
    r.challenge_id = challenge;
    r.user = user;
    r.code_in_outbox = outbox;
    r.message = user.email_verified
                    ? "We sent a 6-digit sign-in code to " + user.email + "."
                    : "Your email is not confirmed yet. We sent a new code to " + user.email + ".";
    return r;
}

AuthResult Auth::verify_code(const std::string& challenge_id, const std::string& code_raw) {
    const std::string code = util::trim(code_raw);
    const std::string kGeneric = "That code is not valid. Request a new one.";

    int64_t row_id = 0, user_id = 0, expires_at = 0;
    int attempts = 0;
    std::string purpose, code_hash;

    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_,
                   "SELECT id, user_id, purpose, code_hash, expires_at, attempts "
                   "FROM email_codes WHERE challenge_id=? AND consumed_at IS NULL");
        s.bind(1, challenge_id);
        if (!s.step()) return make_invalid(kGeneric);
        row_id = s.col_int(0);
        user_id = s.col_int(1);
        purpose = s.col_text(2);
        code_hash = s.col_text(3);
        expires_at = s.col_int(4);
        attempts = static_cast<int>(s.col_int(5));
    } catch (const std::exception& e) {
        return make_error(std::string("Verification failed: ") + e.what());
    }

    const int64_t now = util::now_unix();
    if (expires_at < now) return make_invalid("That code has expired. Request a new one.");
    if (attempts >= config_.max_code_attempts()) {
        // Burn the challenge outright rather than allowing further guesses.
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_, "UPDATE email_codes SET consumed_at=? WHERE id=?");
        s.bind(1, now).bind(2, row_id);
        s.run();
        return make_invalid("Too many incorrect attempts. Request a new code.");
    }

    if (!crypto::verify_short_code(code, code_hash)) {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_, "UPDATE email_codes SET attempts=attempts+1 WHERE id=?");
        s.bind(1, row_id);
        s.run();
        audit(user_id, "code_incorrect", purpose);
        return make_invalid(kGeneric);
    }

    User user;
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Transaction tx(db_);
        db::Stmt consume(db_, "UPDATE email_codes SET consumed_at=? WHERE id=?");
        consume.bind(1, now).bind(2, row_id);
        consume.run();
        if (purpose == code_purpose::kSignup) {
            db::Stmt verify(db_, "UPDATE users SET email_verified=1 WHERE id=?");
            verify.bind(1, user_id);
            verify.run();
        }
        tx.commit();
        if (!load_user_by_id(user_id, &user)) return make_error("Account no longer exists.");
    } catch (const std::exception& e) {
        return make_error(std::string("Verification failed: ") + e.what());
    }

    if (purpose == code_purpose::kDelete) {
        // Deletion is completed by confirm_account_deletion, which re-checks
        // that the caller owns the session. Signal success without a session.
        AuthResult r;
        r.status = AuthResult::Status::Ok;
        r.user = user;
        r.message = "Confirmed.";
        return r;
    }

    audit(user_id, "code_verified", purpose);
    return start_session(user);
}

AuthResult Auth::resend_code(const std::string& challenge_id) {
    int64_t user_id = 0;
    std::string purpose;
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_,
                   "SELECT user_id, purpose FROM email_codes "
                   "WHERE challenge_id=? AND consumed_at IS NULL");
        s.bind(1, challenge_id);
        if (!s.step()) return make_invalid("That request has expired. Start again.");
        user_id = s.col_int(0);
        purpose = s.col_text(1);
    } catch (const std::exception& e) {
        return make_error(std::string("Could not resend: ") + e.what());
    }

    User user;
    {
        std::lock_guard<std::mutex> lock(db_.mutex());
        if (!load_user_by_id(user_id, &user)) return make_error("Account no longer exists.");
    }

    std::string error;
    bool outbox = false;
    std::string fresh = issue_code(user, purpose, &error, &outbox);
    if (fresh.empty()) return make_error("Could not send the email: " + error);

    AuthResult r;
    r.status = AuthResult::Status::NeedsVerification;
    r.challenge_id = fresh;
    r.user = user;
    r.code_in_outbox = outbox;
    r.message = "A new code is on its way to " + user.email + ".";
    return r;
}

bool Auth::authenticate(const std::string& token, User* out) {
    if (token.size() < 32) return false;
    const std::string hash = crypto::sha256_hex(token);
    const int64_t now = util::now_unix();

    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_,
                   "SELECT s.id, s.user_id, s.expires_at, s.revoked_at, s.last_seen_at "
                   "FROM sessions s WHERE s.token_hash=?");
        s.bind(1, hash);
        if (!s.step()) return false;
        const int64_t session_id = s.col_int(0);
        const int64_t user_id = s.col_int(1);
        const int64_t expires_at = s.col_int(2);
        const bool revoked = !s.is_null(3);
        const int64_t last_seen_at = s.col_int(4);
        if (revoked || expires_at <= now) return false;

        // Idle timeout, alongside the absolute one. A session left untouched
        // for longer than this is treated as abandoned; without it, a stored
        // credential would stay usable for the whole 180 days no matter how
        // long the machine sat unopened.
        const int64_t idle_limit = config_.remember_days() * kSecondsPerDay;
        if (idle_limit > 0 && last_seen_at > 0 && now - last_seen_at > idle_limit) {
            db::Stmt expire(db_, "UPDATE sessions SET revoked_at=? WHERE id=?");
            expire.bind(1, now).bind(2, session_id);
            expire.run();
            return false;
        }

        db::Stmt touch(db_, "UPDATE sessions SET last_seen_at=? WHERE id=?");
        touch.bind(1, now).bind(2, session_id);
        touch.run();

        return load_user_by_id(user_id, out);
    } catch (const std::exception&) {
        return false;
    }
}

AuthResult Auth::restore_session(const std::string& token) {
    User user;
    if (!authenticate(token, &user)) {
        return make_invalid("Your saved sign-in has expired. Please sign in again.");
    }

    // Carry the original absolute deadline onto the replacement token, so
    // rotating on every launch cannot roll the 180-day cap forward forever.
    int64_t inherited_expiry = 0;
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_, "SELECT expires_at FROM sessions WHERE token_hash=?");
        s.bind(1, crypto::sha256_hex(token));
        if (s.step()) inherited_expiry = s.col_int(0);
    } catch (const std::exception&) {
    }

    AuthResult refreshed = start_session(user, inherited_expiry);
    if (!refreshed.ok()) return refreshed;

    // Retire the old token only once the new one exists, so a failure part-way
    // through leaves the user signed in rather than locked out.
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_,
                   "UPDATE sessions SET revoked_at=? WHERE token_hash=? AND revoked_at IS NULL");
        s.bind(1, util::now_unix()).bind(2, crypto::sha256_hex(token));
        s.run();
    } catch (const std::exception&) {
    }

    audit(user.id, "session_restored", "");
    return refreshed;
}

void Auth::log_out(const std::string& token) {
    if (token.empty()) return;
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_, "UPDATE sessions SET revoked_at=? WHERE token_hash=? AND revoked_at IS NULL");
        s.bind(1, util::now_unix()).bind(2, crypto::sha256_hex(token));
        s.run();
    } catch (const std::exception&) {
    }
}

void Auth::log_out_all_sessions(int64_t user_id) {
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_, "UPDATE sessions SET revoked_at=? WHERE user_id=? AND revoked_at IS NULL");
        s.bind(1, util::now_unix()).bind(2, user_id);
        s.run();
    } catch (const std::exception&) {
    }
}

AuthResult Auth::request_account_deletion(int64_t user_id) {
    User user;
    {
        std::lock_guard<std::mutex> lock(db_.mutex());
        if (!load_user_by_id(user_id, &user)) return make_error("Account no longer exists.");
    }

    std::string error;
    bool outbox = false;
    std::string challenge = issue_code(user, code_purpose::kDelete, &error, &outbox);
    if (challenge.empty()) return make_error("Could not send the confirmation email: " + error);

    audit(user_id, "deletion_requested", "");
    AuthResult r;
    r.status = AuthResult::Status::NeedsVerification;
    r.challenge_id = challenge;
    r.user = user;
    r.code_in_outbox = outbox;
    r.message = "We sent a confirmation code to " + user.email +
                ". Entering it will permanently delete your account.";
    return r;
}

AuthResult Auth::confirm_account_deletion(int64_t user_id, const std::string& challenge_id,
                                          const std::string& code) {
    // Re-verify the code, and require that the challenge belongs to the signed
    // in user -- otherwise a leaked challenge id from another account could be
    // replayed here.
    int64_t challenge_owner = 0;
    std::string purpose;
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Stmt s(db_,
                   "SELECT user_id, purpose FROM email_codes WHERE challenge_id=?");
        s.bind(1, challenge_id);
        if (!s.step()) return make_invalid("That code is not valid. Request a new one.");
        challenge_owner = s.col_int(0);
        purpose = s.col_text(1);
    } catch (const std::exception& e) {
        return make_error(std::string("Deletion failed: ") + e.what());
    }
    if (challenge_owner != user_id || purpose != code_purpose::kDelete) {
        return make_invalid("That code is not valid. Request a new one.");
    }

    AuthResult verified = verify_code(challenge_id, code);
    if (verified.status != AuthResult::Status::Ok) return verified;

    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        db::Transaction tx(db_);
        // ON DELETE CASCADE clears sessions, codes, chats and messages.
        db::Stmt del(db_, "DELETE FROM users WHERE id=?");
        del.bind(1, user_id);
        del.run();
        // Detach audit rows rather than deleting them: keep the security trail,
        // drop the link to a person who asked to be removed.
        db::Stmt anon(db_, "UPDATE audit_log SET user_id=NULL, detail='[erased]' WHERE user_id=?");
        anon.bind(1, user_id);
        anon.run();
        tx.commit();
    } catch (const std::exception& e) {
        return make_error(std::string("Deletion failed: ") + e.what());
    }

    audit(0, "account_deleted", "");
    AuthResult r;
    r.status = AuthResult::Status::Ok;
    r.message = "Your account and all of its chats have been deleted.";
    return r;
}

void Auth::purge_expired() {
    const int64_t now = util::now_unix();
    try {
        std::lock_guard<std::mutex> lock(db_.mutex());
        // Past the absolute deadline, already revoked, or abandoned for longer
        // than the idle limit. The last case matters because an idle session is
        // otherwise only noticed when someone tries to use it.
        const int64_t idle_limit = config_.remember_days() * kSecondsPerDay;
        db::Stmt s1(db_,
                    "DELETE FROM sessions WHERE expires_at <= ? OR revoked_at IS NOT NULL "
                    "OR (? > 0 AND last_seen_at > 0 AND ? - last_seen_at > ?)");
        s1.bind(1, now).bind(2, idle_limit).bind(3, now).bind(4, idle_limit);
        s1.run();
        // Keep consumed/expired codes briefly for audit, then drop them.
        db::Stmt s2(db_, "DELETE FROM email_codes WHERE expires_at <= ?");
        s2.bind(1, now - 86400);
        s2.run();
    } catch (const std::exception&) {
    }
}
