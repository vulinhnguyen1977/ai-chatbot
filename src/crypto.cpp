#include "crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <stdexcept>

#include "util.h"

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace crypto {
namespace {

// OWASP's 2023 password-storage guidance for PBKDF2-HMAC-SHA512.
constexpr uint32_t kPasswordIterations = 210000;
// The 2FA code is six digits, expires in minutes and is attempt-limited, so a
// lighter cost is appropriate; it keeps login latency reasonable.
constexpr uint32_t kShortCodeIterations = 50000;
constexpr size_t kSaltLen = 16;
constexpr size_t kHashLen = 32;

std::vector<uint8_t> from_hex(const std::string& hex, bool* ok) {
    *ok = false;
    if (hex.size() % 2 != 0) return {};
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = -1, lo = -1;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        hi = nib(hex[i]);
        lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    *ok = true;
    return out;
}

std::string encode_hash(const std::string& secret, uint32_t iterations) {
    std::vector<uint8_t> salt = random_bytes(kSaltLen);
    std::vector<uint8_t> hash = pbkdf2_sha512(secret, salt, iterations, kHashLen);
    char header[64];
    std::snprintf(header, sizeof(header), "pbkdf2_sha512$%u$", iterations);
    return std::string(header) + util::to_hex(salt) + "$" + util::to_hex(hash);
}

bool verify_encoded(const std::string& secret, const std::string& encoded) {
    std::vector<std::string> parts = util::split(encoded, '$');
    if (parts.size() != 4 || parts[0] != "pbkdf2_sha512") return false;

    uint32_t iterations = 0;
    try {
        unsigned long parsed = std::stoul(parts[1]);
        // Guard against a tampered row asking for a denial-of-service cost.
        if (parsed == 0 || parsed > 5000000UL) return false;
        iterations = static_cast<uint32_t>(parsed);
    } catch (const std::exception&) {
        return false;
    }

    bool ok = false;
    std::vector<uint8_t> salt = from_hex(parts[2], &ok);
    if (!ok || salt.empty()) return false;
    std::vector<uint8_t> expected = from_hex(parts[3], &ok);
    if (!ok || expected.empty()) return false;

    std::vector<uint8_t> actual = pbkdf2_sha512(secret, salt, iterations, expected.size());
    return constant_time_equals(util::to_hex(actual), util::to_hex(expected));
}

}  // namespace

std::vector<uint8_t> random_bytes(size_t n) {
    std::vector<uint8_t> out(n);
    if (n == 0) return out;
    NTSTATUS st = BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n),
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st != STATUS_SUCCESS) {
        throw std::runtime_error("BCryptGenRandom failed; refusing to use a weak random source");
    }
    return out;
}

std::string random_token_hex(size_t n) { return util::to_hex(random_bytes(n)); }

std::string random_digits(size_t digits) {
    std::string out;
    out.reserve(digits);
    while (out.size() < digits) {
        std::vector<uint8_t> buf = random_bytes(digits);
        for (uint8_t b : buf) {
            // 250 == 25 * 10; discarding the tail keeps each digit uniform.
            if (b >= 250) continue;
            out.push_back(static_cast<char>('0' + (b % 10)));
            if (out.size() == digits) break;
        }
    }
    return out;
}

std::vector<uint8_t> sha256(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != STATUS_SUCCESS) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider(SHA256) failed");
    }
    std::vector<uint8_t> out(32);
    NTSTATUS st = BCryptHash(alg, nullptr, 0,
                             reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                             static_cast<ULONG>(data.size()), out.data(),
                             static_cast<ULONG>(out.size()));
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st != STATUS_SUCCESS) throw std::runtime_error("BCryptHash(SHA256) failed");
    return out;
}

std::string sha256_hex(const std::string& data) { return util::to_hex(sha256(data)); }

std::vector<uint8_t> pbkdf2_sha512(const std::string& password,
                                   const std::vector<uint8_t>& salt,
                                   uint32_t iterations,
                                   size_t out_len) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    // BCRYPT_ALG_HANDLE_HMAC_FLAG is required for the handle to be usable as a PRF.
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA512_ALGORITHM, nullptr,
                                              BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (st != STATUS_SUCCESS) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider(SHA512/HMAC) failed");
    }
    std::vector<uint8_t> out(out_len);
    st = BCryptDeriveKeyPBKDF2(
        alg, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()),
        const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()),
        iterations, out.data(), static_cast<ULONG>(out.size()), 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st != STATUS_SUCCESS) throw std::runtime_error("BCryptDeriveKeyPBKDF2 failed");
    return out;
}

std::string hash_password(const std::string& password) {
    return encode_hash(password, kPasswordIterations);
}

bool verify_password(const std::string& password, const std::string& encoded) {
    return verify_encoded(password, encoded);
}

std::string hash_short_code(const std::string& code) {
    return encode_hash(code, kShortCodeIterations);
}

bool verify_short_code(const std::string& code, const std::string& encoded) {
    return verify_encoded(code, encoded);
}

bool constant_time_equals(const std::string& a, const std::string& b) {
    // Fold the length difference into the result instead of returning early,
    // so timing does not reveal how much of the secret matched.
    unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0 && a.size() == b.size();
}

}  // namespace crypto
