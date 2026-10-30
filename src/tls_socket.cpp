#include "tls_socket.h"

#define SECURITY_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Older MinGW SDK headers omit a few Schannel constants.
#ifndef SCH_USE_STRONG_CRYPTO
#define SCH_USE_STRONG_CRYPTO 0x00400000
#endif
#ifndef SECBUFFER_ALERT
#define SECBUFFER_ALERT 17
#endif

namespace net {
namespace {

constexpr size_t kIoChunk = 16384;
// A TLS record is capped at 16KB of plaintext plus overhead; 64KB of pending
// ciphertext is a generous ceiling that still bounds memory on a bad peer.
constexpr size_t kMaxPendingCiphertext = 128 * 1024;

std::string win_error(const char* what, long code) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s failed (0x%08lx)", what, static_cast<unsigned long>(code));
    return buf;
}

}  // namespace

bool init_winsock(std::string* error) {
    static std::once_flag once;
    static bool ok = false;
    static std::string err;
    std::call_once(once, [] {
        WSADATA data;
        int rc = WSAStartup(MAKEWORD(2, 2), &data);
        ok = (rc == 0);
        if (!ok) err = win_error("WSAStartup", rc);
    });
    if (!ok && error) *error = err;
    return ok;
}

struct TlsSocket::Impl {
    SOCKET sock = INVALID_SOCKET;
    CredHandle cred{};
    CtxtHandle ctx{};
    bool cred_ok = false;
    bool ctx_ok = false;
    SecPkgContext_StreamSizes sizes{};
    std::string pending_cipher;  // received, not yet decrypted
    std::string pending_plain;   // decrypted, not yet handed to the caller
};

TlsSocket::TlsSocket() : impl_(new Impl) {}

TlsSocket::~TlsSocket() {
    close();
    delete impl_;
}

void TlsSocket::close() {
    if (impl_->ctx_ok) {
        DeleteSecurityContext(&impl_->ctx);
        impl_->ctx_ok = false;
    }
    if (impl_->cred_ok) {
        FreeCredentialsHandle(&impl_->cred);
        impl_->cred_ok = false;
    }
    if (impl_->sock != INVALID_SOCKET) {
        shutdown(impl_->sock, SD_BOTH);
        closesocket(impl_->sock);
        impl_->sock = INVALID_SOCKET;
    }
    tls_active_ = false;
}

bool TlsSocket::connect(const std::string& host, int port, int timeout_seconds,
                        std::string* error) {
    if (!init_winsock(error)) return false;

    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%d", port);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    int rc = getaddrinfo(host.c_str(), port_str, &hints, &result);
    if (rc != 0 || !result) {
        if (error) *error = "cannot resolve host '" + host + "'";
        return false;
    }

    SOCKET s = INVALID_SOCKET;
    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        DWORD tv = static_cast<DWORD>(timeout_seconds) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(result);

    if (s == INVALID_SOCKET) {
        if (error) *error = "cannot connect to " + host + ":" + port_str;
        return false;
    }
    impl_->sock = s;
    return true;
}

bool TlsSocket::raw_send(const char* data, size_t len, std::string* error) {
    size_t sent = 0;
    while (sent < len) {
        int n = ::send(impl_->sock, data + sent, static_cast<int>(len - sent), 0);
        if (n <= 0) {
            if (error) *error = win_error("send", WSAGetLastError());
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool TlsSocket::raw_recv(char* buf, size_t len, size_t* received, std::string* error) {
    int n = ::recv(impl_->sock, buf, static_cast<int>(len), 0);
    if (n < 0) {
        if (error) *error = win_error("recv", WSAGetLastError());
        return false;
    }
    *received = static_cast<size_t>(n);  // 0 means the peer closed cleanly
    return true;
}

bool TlsSocket::start_tls(const std::string& host, std::string* error) {
    if (impl_->sock == INVALID_SOCKET) {
        if (error) *error = "start_tls on a closed socket";
        return false;
    }

    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    // grbitEnabledProtocols is left at 0 on purpose: that defers protocol
    // selection to the OS policy, which is what Microsoft recommends so the
    // app inherits future deprecations of old TLS versions automatically.
    sc.grbitEnabledProtocols = 0;
    sc.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;

    SECURITY_STATUS ss = AcquireCredentialsHandleA(nullptr, const_cast<char*>(UNISP_NAME_A),
                                                   SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr,
                                                   nullptr, &impl_->cred, nullptr);
    if (ss != SEC_E_OK) {
        if (error) *error = win_error("AcquireCredentialsHandle", ss);
        return false;
    }
    impl_->cred_ok = true;

    // No ISC_REQ_MANUAL_CRED_VALIDATION: certificate chain and hostname
    // checking are left to Schannel, driven by SCH_CRED_AUTO_CRED_VALIDATION.
    const DWORD req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                      ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR;

    std::string host_mutable = host;
    DWORD out_flags = 0;

    // --- first token: client hello ---------------------------------------
    SecBuffer out_buf{};
    out_buf.BufferType = SECBUFFER_TOKEN;
    SecBufferDesc out_desc{SECBUFFER_VERSION, 1, &out_buf};

    ss = InitializeSecurityContextA(&impl_->cred, nullptr, host_mutable.data(), req, 0, 0, nullptr,
                                    0, &impl_->ctx, &out_desc, &out_flags, nullptr);
    if (ss != SEC_I_CONTINUE_NEEDED) {
        if (error) *error = win_error("InitializeSecurityContext(initial)", ss);
        return false;
    }
    impl_->ctx_ok = true;
    if (out_buf.pvBuffer && out_buf.cbBuffer) {
        bool sent = raw_send(static_cast<const char*>(out_buf.pvBuffer), out_buf.cbBuffer, error);
        FreeContextBuffer(out_buf.pvBuffer);
        if (!sent) return false;
    }

    // --- handshake loop ---------------------------------------------------
    std::string incoming = impl_->pending_cipher;
    impl_->pending_cipher.clear();

    // Set when the server asked for a client certificate: the same input has
    // to be replayed rather than waiting for bytes that will never arrive.
    bool reuse_input = false;
    int anonymous_retries = 0;

    for (;;) {
        // Top up the buffer whenever Schannel says the message is incomplete
        // (or before the very first read).
        if (!reuse_input && incoming.empty()) {
            char chunk[kIoChunk];
            size_t got = 0;
            if (!raw_recv(chunk, sizeof(chunk), &got, error)) return false;
            if (got == 0) {
                if (error) *error = "peer closed connection during TLS handshake";
                return false;
            }
            incoming.append(chunk, got);
        }
        reuse_input = false;

        SecBuffer in_bufs[2]{};
        in_bufs[0].BufferType = SECBUFFER_TOKEN;
        in_bufs[0].pvBuffer = incoming.data();
        in_bufs[0].cbBuffer = static_cast<unsigned long>(incoming.size());
        in_bufs[1].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc in_desc{SECBUFFER_VERSION, 2, in_bufs};

        SecBuffer token{};
        token.BufferType = SECBUFFER_TOKEN;
        SecBufferDesc token_desc{SECBUFFER_VERSION, 1, &token};

        out_flags = 0;
        ss = InitializeSecurityContextA(&impl_->cred, &impl_->ctx, host_mutable.data(), req, 0, 0,
                                        &in_desc, 0, nullptr, &token_desc, &out_flags, nullptr);

        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            if (incoming.size() > kMaxPendingCiphertext) {
                if (error) *error = "TLS handshake message exceeded the size limit";
                return false;
            }
            char chunk[kIoChunk];
            size_t got = 0;
            if (!raw_recv(chunk, sizeof(chunk), &got, error)) return false;
            if (got == 0) {
                if (error) *error = "peer closed connection during TLS handshake";
                return false;
            }
            incoming.append(chunk, got);
            continue;
        }

        if (token.pvBuffer && token.cbBuffer) {
            bool sent = raw_send(static_cast<const char*>(token.pvBuffer), token.cbBuffer, error);
            FreeContextBuffer(token.pvBuffer);
            if (!sent) return false;
        }

        if (ss == SEC_I_INCOMPLETE_CREDENTIALS) {
            // The server requested (optionally) a client certificate -- Gmail's
            // submission port does this. We have none to offer, so re-drive the
            // handshake over the same input, which makes Schannel send an empty
            // certificate list and continue anonymously.
            if (++anonymous_retries > 3) {
                if (error) *error = "server insists on a client certificate";
                return false;
            }
            reuse_input = true;
            continue;
        }

        if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) {
            // Whatever Schannel did not consume belongs to the next step.
            if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0) {
                incoming = incoming.substr(incoming.size() - in_bufs[1].cbBuffer);
            } else {
                incoming.clear();
            }
            if (ss == SEC_E_OK) break;
            continue;
        }

        if (error) *error = win_error("InitializeSecurityContext", ss);
        return false;
    }

    ss = QueryContextAttributes(&impl_->ctx, SECPKG_ATTR_STREAM_SIZES, &impl_->sizes);
    if (ss != SEC_E_OK) {
        if (error) *error = win_error("QueryContextAttributes(STREAM_SIZES)", ss);
        return false;
    }

    impl_->pending_cipher = incoming;  // application data that arrived early
    tls_active_ = true;
    return true;
}

bool TlsSocket::tls_write(const std::string& data, std::string* error) {
    const size_t max_chunk = impl_->sizes.cbMaximumMessage;
    size_t offset = 0;
    std::vector<char> scratch(impl_->sizes.cbHeader + max_chunk + impl_->sizes.cbTrailer);

    while (offset < data.size()) {
        size_t n = data.size() - offset;
        if (n > max_chunk) n = max_chunk;

        std::memcpy(scratch.data() + impl_->sizes.cbHeader, data.data() + offset, n);

        SecBuffer bufs[4]{};
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer = scratch.data();
        bufs[0].cbBuffer = impl_->sizes.cbHeader;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer = scratch.data() + impl_->sizes.cbHeader;
        bufs[1].cbBuffer = static_cast<unsigned long>(n);
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer = scratch.data() + impl_->sizes.cbHeader + n;
        bufs[2].cbBuffer = impl_->sizes.cbTrailer;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};

        SECURITY_STATUS ss = EncryptMessage(&impl_->ctx, 0, &desc, 0);
        if (ss != SEC_E_OK) {
            if (error) *error = win_error("EncryptMessage", ss);
            return false;
        }
        // EncryptMessage rewrites cbBuffer with the real header/trailer sizes.
        size_t total = bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer;
        if (!raw_send(scratch.data(), total, error)) return false;
        offset += n;
    }
    return true;
}

bool TlsSocket::tls_read(std::string* out, bool* closed, std::string* error) {
    *closed = false;

    for (;;) {
        if (!impl_->pending_plain.empty()) {
            out->append(impl_->pending_plain);
            impl_->pending_plain.clear();
            return true;
        }

        if (!impl_->pending_cipher.empty()) {
            SecBuffer bufs[4]{};
            bufs[0].BufferType = SECBUFFER_DATA;
            bufs[0].pvBuffer = impl_->pending_cipher.data();
            bufs[0].cbBuffer = static_cast<unsigned long>(impl_->pending_cipher.size());
            bufs[1].BufferType = SECBUFFER_EMPTY;
            bufs[2].BufferType = SECBUFFER_EMPTY;
            bufs[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};

            SECURITY_STATUS ss = DecryptMessage(&impl_->ctx, &desc, 0, nullptr);

            if (ss == SEC_E_OK || ss == SEC_I_RENEGOTIATE) {
                std::string plain, extra;
                for (int i = 0; i < 4; ++i) {
                    if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].pvBuffer) {
                        plain.append(static_cast<const char*>(bufs[i].pvBuffer), bufs[i].cbBuffer);
                    } else if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].pvBuffer) {
                        extra.append(static_cast<const char*>(bufs[i].pvBuffer), bufs[i].cbBuffer);
                    }
                }
                impl_->pending_cipher = extra;
                if (!plain.empty()) {
                    out->append(plain);
                    return true;
                }
                // SEC_I_RENEGOTIATE would need the handshake loop re-entered.
                // No SMTP server we target requests it, so treat it as fatal
                // rather than silently continuing on an unnegotiated context.
                if (ss == SEC_I_RENEGOTIATE) {
                    if (error) *error = "server requested TLS renegotiation (unsupported)";
                    return false;
                }
                continue;  // record held no application data; decrypt the next
            }

            if (ss == SEC_I_CONTEXT_EXPIRED) {
                *closed = true;
                return false;
            }
            if (ss != SEC_E_INCOMPLETE_MESSAGE) {
                if (error) *error = win_error("DecryptMessage", ss);
                return false;
            }
            // fall through to read more ciphertext
        }

        if (impl_->pending_cipher.size() > kMaxPendingCiphertext) {
            if (error) *error = "TLS record exceeded the size limit";
            return false;
        }

        char chunk[kIoChunk];
        size_t got = 0;
        if (!raw_recv(chunk, sizeof(chunk), &got, error)) return false;
        if (got == 0) {
            *closed = true;
            return false;
        }
        impl_->pending_cipher.append(chunk, got);
    }
}

bool TlsSocket::write_all(const std::string& data, std::string* error) {
    if (tls_active_) return tls_write(data, error);
    return raw_send(data.data(), data.size(), error);
}

bool TlsSocket::read_some(std::string* out, bool* closed, std::string* error) {
    if (tls_active_) return tls_read(out, closed, error);

    *closed = false;
    char chunk[kIoChunk];
    size_t got = 0;
    if (!raw_recv(chunk, sizeof(chunk), &got, error)) return false;
    if (got == 0) {
        *closed = true;
        return false;
    }
    out->append(chunk, got);
    return true;
}

}  // namespace net
