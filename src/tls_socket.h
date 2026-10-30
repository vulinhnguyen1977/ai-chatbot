// tls_socket.h -- a blocking TCP socket that can be upgraded to TLS in place.
//
// Backed by Winsock + Schannel (the TLS stack built into Windows), which keeps
// the dependency footprint at zero for the SMTP client. STARTTLS needs the
// upgrade-in-place behaviour: the SMTP conversation begins in the clear and
// only then negotiates TLS on the same connection.
#pragma once

#include <string>

namespace net {

// Initialises Winsock once per process. Safe to call repeatedly.
bool init_winsock(std::string* error);

class TlsSocket {
public:
    TlsSocket();
    ~TlsSocket();
    TlsSocket(const TlsSocket&) = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;

    bool connect(const std::string& host, int port, int timeout_seconds, std::string* error);

    // Performs the TLS handshake on the already-connected socket. `host` is
    // used for SNI and certificate name validation.
    bool start_tls(const std::string& host, std::string* error);

    // Both operate on plaintext; encryption is applied transparently once
    // start_tls() has succeeded.
    bool write_all(const std::string& data, std::string* error);
    // Reads at least one byte, appending to `out`. Returns false on error or
    // clean remote close (check `closed`).
    bool read_some(std::string* out, bool* closed, std::string* error);

    void close();
    bool is_tls() const { return tls_active_; }

private:
    bool raw_send(const char* data, size_t len, std::string* error);
    bool raw_recv(char* buf, size_t len, size_t* received, std::string* error);
    bool tls_write(const std::string& data, std::string* error);
    bool tls_read(std::string* out, bool* closed, std::string* error);

    struct Impl;
    Impl* impl_;
    bool tls_active_ = false;
};

}  // namespace net
