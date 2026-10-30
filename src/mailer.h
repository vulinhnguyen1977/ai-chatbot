// mailer.h -- outbound email for two-factor codes.
//
// Two transports:
//   "smtp"  real delivery over TLS, implemented on Windows Schannel so no
//           OpenSSL binary has to be shipped in the zip.
//   "file"  writes a .eml into data/outbox instead of sending. This is the
//           default so a fresh unzip can exercise the entire 2FA flow before
//           the client has configured a mailbox.
#pragma once

#include <string>

class Config;

struct Email {
    std::string to;
    std::string to_name;
    std::string subject;
    std::string body;  // plain text, UTF-8
};

class Mailer {
public:
    explicit Mailer(const Config& config);

    // Returns true on success. On failure `error` receives a diagnostic that
    // is safe to log but is never shown to the end user verbatim.
    bool send(const Email& email, std::string* error);

    // True when real SMTP delivery is configured. The UI uses this to tell the
    // user to check the outbox folder instead of their inbox.
    bool is_live() const { return live_; }

    std::string outbox_dir() const { return outbox_dir_; }

private:
    bool send_file(const Email& email, std::string* error);
    bool send_smtp(const Email& email, std::string* error);
    std::string build_mime(const Email& email) const;

    std::string transport_;
    std::string host_;
    int port_ = 587;
    std::string security_;  // starttls | implicit | none
    std::string username_;
    std::string password_;
    std::string from_;
    std::string from_name_;
    std::string outbox_dir_;
    bool live_ = false;
};
