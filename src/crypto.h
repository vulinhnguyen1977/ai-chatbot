// crypto.h -- password hashing, token generation and digests.
//
// Everything here is backed by Windows CNG (bcrypt.dll), which ships with the
// OS. That keeps the shipped tree free of OpenSSL binaries: the client can
// unzip and build without hunting for native crypto libraries.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace crypto {

// Cryptographically secure random bytes. Throws std::runtime_error on failure
// -- we must never silently fall back to a weak source for salts or tokens.
std::vector<uint8_t> random_bytes(size_t n);

// Lowercase hex string of `n` random bytes.
std::string random_token_hex(size_t n = 32);

// Uniform random decimal string of `digits` length, e.g. "049321". Used for
// the emailed 2FA code. Rejection sampling keeps the digits unbiased.
std::string random_digits(size_t digits);

std::vector<uint8_t> sha256(const std::string& data);
std::string sha256_hex(const std::string& data);

// PBKDF2-HMAC-SHA512.
std::vector<uint8_t> pbkdf2_sha512(const std::string& password,
                                   const std::vector<uint8_t>& salt,
                                   uint32_t iterations,
                                   size_t out_len);

// Encodes as "pbkdf2_sha512$<iterations>$<salt_hex>$<hash_hex>" so the cost
// parameter travels with the hash and can be raised later without a migration.
std::string hash_password(const std::string& password);
// Verifies against the encoded form above. Returns false on any malformed
// input rather than throwing, so a corrupt row cannot crash a login.
bool verify_password(const std::string& password, const std::string& encoded);

// Same encoding, cheaper cost -- for the short-lived, rate-limited 2FA code.
std::string hash_short_code(const std::string& code);
bool verify_short_code(const std::string& code, const std::string& encoded);

// Length-independent comparison for secrets.
bool constant_time_equals(const std::string& a, const std::string& b);

}  // namespace crypto
