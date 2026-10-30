#include "mailer.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "config.h"
#include "crypto.h"
#include "tls_socket.h"
#include "util.h"

namespace {

// True when every byte is printable US-ASCII, i.e. safe to drop into a header
// without MIME encoded-word wrapping.
bool is_ascii(const std::string& s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

// RFC 2047 encoded word so non-ASCII subjects survive transport intact.
std::string encode_header_value(const std::string& s) {
    if (is_ascii(s)) return s;
    return "=?UTF-8?B?" + util::base64_encode(s) + "?=";
}

// Strips CR/LF from anything interpolated into a header. Callers already
// validate addresses, but this is the last line of defence against header
// injection through a display name.
std::string header_safe(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != '\r' && c != '\n') out.push_back(c);
    }
    return out;
}

// SMTP dot-stuffing: a line consisting of a single '.' would otherwise end
// the DATA section early (RFC 5321 s4.5.2).
std::string dot_stuff(const std::string& body) {
    std::string out;
    out.reserve(body.size() + 32);
    bool at_line_start = true;
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (at_line_start && c == '.') out.push_back('.');
        out.push_back(c);
        at_line_start = (c == '\n');
    }
    return out;
}

// Normalises bare LF to CRLF, which SMTP requires.
std::string to_crlf(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) out.push_back('\r');
        out.push_back(s[i]);
    }
    return out;
}

std::string sanitize_filename(const std::string& s) {
    std::string out;
    for (char c : s) {
        out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' ||
                       c == '_')
                          ? c
                          : '_');
    }
    return out.empty() ? std::string("message") : out;
}

// ---------------------------------------------------------------- SMTP I/O

class SmtpSession {
public:
    explicit SmtpSession(net::TlsSocket& sock) : sock_(sock) {}

    // Reads one complete SMTP reply, handling multi-line "250-" continuations.
    // Returns the numeric code, or -1 on transport failure.
    int read_reply(std::string* text, std::string* error) {
        for (;;) {
            std::string line;
            if (extract_complete_reply(&line)) {
                if (text) *text = line;
                if (line.size() < 3) {
                    if (error) *error = "malformed SMTP reply";
                    return -1;
                }
                return std::atoi(line.substr(0, 3).c_str());
            }
            bool closed = false;
            if (!sock_.read_some(&buffer_, &closed, error)) {
                if (error && closed) *error = "server closed the connection";
                return -1;
            }
            if (buffer_.size() > 64 * 1024) {
                if (error) *error = "SMTP reply too large";
                return -1;
            }
        }
    }

    bool send_line(const std::string& line, std::string* error) {
        return sock_.write_all(line + "\r\n", error);
    }

    // Sends `line` and requires the reply code to be `expected`.
    bool command(const std::string& line, int expected, std::string* error,
                 std::string* reply_out = nullptr) {
        if (!send_line(line, error)) return false;
        std::string reply;
        int code = read_reply(&reply, error);
        if (reply_out) *reply_out = reply;
        if (code < 0) return false;
        if (code != expected) {
            if (error) {
                *error = "SMTP command rejected (expected " + std::to_string(expected) + ", got " +
                         std::to_string(code) + "): " + util::utf8_truncate(reply, 200);
            }
            return false;
        }
        return true;
    }

private:
    // A multi-line reply repeats the code with a '-' separator ("250-EXT") and
    // terminates on the first line that uses a space ("250 OK"). `out` receives
    // the whole reply so callers can inspect advertised capabilities.
    bool extract_complete_reply(std::string* out) {
        size_t search = 0;
        while (true) {
            size_t eol = buffer_.find("\r\n", search);
            if (eol == std::string::npos) return false;
            const size_t line_len = eol - search;
            if (line_len == 3 || (line_len >= 4 && buffer_[search + 3] == ' ')) {
                *out = buffer_.substr(0, eol);
                buffer_.erase(0, eol + 2);
                return true;
            }
            search = eol + 2;
        }
    }

    net::TlsSocket& sock_;
    std::string buffer_;
};

}  // namespace

Mailer::Mailer(const Config& config) {
    transport_ = util::to_lower(config.mail_transport());
    host_ = config.mail_host();
    port_ = config.mail_port();
    security_ = util::to_lower(config.mail_security());
    username_ = config.mail_username();
    password_ = config.mail_password();
    from_ = config.mail_from();
    from_name_ = config.mail_from_name();
    outbox_dir_ = config.mail_outbox();

    // Only claim live delivery when we actually have somewhere to send.
    live_ = (transport_ == "smtp") && !host_.empty() && !from_.empty();
    if (transport_ == "smtp" && !live_) transport_ = "file";
}

std::string Mailer::build_mime(const Email& email) const {
    const std::string from_display = header_safe(from_name_);
    const std::string to_display = header_safe(email.to_name);

    std::ostringstream out;
    out << "Date: " << util::rfc5322_date(util::now_unix()) << "\r\n";
    out << "From: " << encode_header_value(from_display) << " <" << header_safe(from_) << ">\r\n";
    if (!to_display.empty()) {
        out << "To: " << encode_header_value(to_display) << " <" << header_safe(email.to) << ">\r\n";
    } else {
        out << "To: <" << header_safe(email.to) << ">\r\n";
    }
    out << "Subject: " << encode_header_value(header_safe(email.subject)) << "\r\n";
    // A domain-less message id would be rejected by strict receivers.
    std::string domain = "mstutor.local";
    size_t at = from_.find('@');
    if (at != std::string::npos && at + 1 < from_.size()) domain = from_.substr(at + 1);
    out << "Message-ID: <" << crypto::random_token_hex(12) << "@" << header_safe(domain) << ">\r\n";
    out << "MIME-Version: 1.0\r\n";
    out << "Content-Type: text/plain; charset=UTF-8\r\n";
    out << "Content-Transfer-Encoding: 8bit\r\n";
    out << "Auto-Submitted: auto-generated\r\n";
    out << "\r\n";
    out << to_crlf(email.body);
    return out.str();
}

bool Mailer::send(const Email& email, std::string* error) {
    if (!util::is_valid_email(email.to)) {
        if (error) *error = "invalid recipient address";
        return false;
    }
    if (transport_ == "smtp") return send_smtp(email, error);
    return send_file(email, error);
}

bool Mailer::send_file(const Email& email, std::string* error) {
    try {
        std::filesystem::create_directories(outbox_dir_);
    } catch (const std::exception& e) {
        if (error) *error = std::string("cannot create outbox: ") + e.what();
        return false;
    }

    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%lld", static_cast<long long>(util::now_unix()));
    std::filesystem::path path =
        std::filesystem::path(outbox_dir_) /
        (std::string(stamp) + "-" + sanitize_filename(email.to) + "-" +
         crypto::random_token_hex(4) + ".eml");

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        if (error) *error = "cannot write to outbox";
        return false;
    }
    out << build_mime(email);
    out.close();

    // Loud on purpose: in file mode this line is how the operator finds the code.
    std::printf("[mail] written to %s\n", path.string().c_str());
    std::fflush(stdout);
    return true;
}

bool Mailer::send_smtp(const Email& email, std::string* error) {
    net::TlsSocket sock;
    if (!sock.connect(host_, port_, 30, error)) return false;

    const bool implicit_tls = (security_ == "implicit" || security_ == "ssl" || security_ == "tls");
    if (implicit_tls && !sock.start_tls(host_, error)) return false;

    SmtpSession smtp(sock);

    std::string reply;
    if (smtp.read_reply(&reply, error) != 220) {
        if (error && error->empty()) *error = "unexpected SMTP greeting: " + reply;
        return false;
    }

    // The EHLO argument should be a name for *us*; a bracketed literal is the
    // correct form when we have no resolvable hostname.
    const std::string ehlo = "EHLO [127.0.0.1]";
    if (!smtp.command(ehlo, 250, error, &reply)) return false;

    if (!implicit_tls && security_ == "starttls") {
        if (!smtp.command("STARTTLS", 220, error)) return false;
        if (!sock.start_tls(host_, error)) return false;
        // RFC 3207: the EHLO must be repeated once the channel is encrypted,
        // because the server may advertise different capabilities (AUTH in
        // particular is normally hidden until TLS is up).
        if (!smtp.command(ehlo, 250, error, &reply)) return false;
    }

    if (!username_.empty()) {
        if (!sock.is_tls()) {
            if (error) *error = "refusing to send SMTP credentials over an unencrypted connection";
            return false;
        }
        // AUTH LOGIN is the most widely accepted mechanism, including Gmail
        // app passwords and Microsoft 365 SMTP AUTH.
        if (!smtp.command("AUTH LOGIN", 334, error)) return false;
        if (!smtp.command(util::base64_encode(username_), 334, error)) return false;
        if (!smtp.command(util::base64_encode(password_), 235, error)) {
            if (error) *error = "SMTP authentication failed (check mail.username/mail.password)";
            return false;
        }
    }

    if (!smtp.command("MAIL FROM:<" + header_safe(from_) + ">", 250, error)) return false;
    if (!smtp.command("RCPT TO:<" + header_safe(email.to) + ">", 250, error)) return false;
    if (!smtp.command("DATA", 354, error)) return false;

    std::string payload = dot_stuff(build_mime(email)) + "\r\n.\r\n";
    if (!sock.write_all(payload, error)) return false;
    if (smtp.read_reply(&reply, error) != 250) {
        if (error && error->empty()) *error = "server rejected the message: " + reply;
        return false;
    }

    // Best-effort: the message is already accepted, so a QUIT failure is not
    // worth reporting as a send failure.
    std::string ignored;
    smtp.send_line("QUIT", &ignored);
    sock.close();
    return true;
}
