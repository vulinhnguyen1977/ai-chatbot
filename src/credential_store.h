// credential_store.h -- keeps a signed-in session across restarts.
//
// The stored value is a live session token, so it is a credential in its own
// right and must not sit on disk in the clear. It is encrypted with the Windows
// Data Protection API, which derives the key from the logged-in Windows
// account. Two useful consequences:
//
//   * Another user on the same PC cannot read it.
//   * Copying the application folder to another machine or account leaves the
//     file undecryptable, so a remembered sign-in cannot travel with a USB
//     copy of the app.
//
// The server side is unchanged and still authoritative: the token is hashed in
// the database, expires, and can be revoked. This only decides whether the app
// has a token to present at startup.
#pragma once

#include <string>

namespace credentials {

// Encrypts and writes `payload`. Returns false if it could not be stored, in
// which case nothing is left behind.
bool save(const std::string& path, const std::string& payload, std::string* error = nullptr);

// Decrypts the file. Returns false when it is missing, corrupt, or was written
// by a different Windows account.
bool load(const std::string& path, std::string* payload, std::string* error = nullptr);

// Removes the file. Safe to call when it does not exist.
void clear(const std::string& path);

bool exists(const std::string& path);

}  // namespace credentials
