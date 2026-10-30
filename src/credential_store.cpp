#include "credential_store.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace credentials {
namespace {

// Additional entropy mixed into the DPAPI key. It is not a secret -- it lives
// in the binary -- but it does mean a blob from this application cannot be
// decrypted by another program simply calling CryptUnprotectData as the same
// user, which is the default behaviour without it.
const BYTE kEntropy[] = "mstutor/remember-me/v1";

DATA_BLOB make_blob(const void* data, size_t size) {
    DATA_BLOB blob;
    blob.pbData = static_cast<BYTE*>(const_cast<void*>(data));
    blob.cbData = static_cast<DWORD>(size);
    return blob;
}

std::string last_error(const char* what) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s failed (%lu)", what, GetLastError());
    return buffer;
}

}  // namespace

bool save(const std::string& path, const std::string& payload, std::string* error) {
    DATA_BLOB in = make_blob(payload.data(), payload.size());
    DATA_BLOB entropy = make_blob(kEntropy, sizeof(kEntropy));
    DATA_BLOB out{};

    // CRYPTPROTECT_UI_FORBIDDEN: never show a prompt. This runs during startup
    // and at sign-in, where a modal dialog from the crypto layer would be
    // baffling.
    if (!CryptProtectData(&in, L"Middle School Tutor sign-in", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        if (error) *error = last_error("CryptProtectData");
        return false;
    }

    bool ok = false;
    try {
        const fs::path target(path);
        if (target.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(target.parent_path(), ec);
        }
        // Written to a temporary file and renamed, so an interrupted write
        // cannot leave a half-file that fails to decrypt on the next start.
        const fs::path temporary = target.string() + ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (file) {
                file.write(reinterpret_cast<const char*>(out.pbData),
                           static_cast<std::streamsize>(out.cbData));
                ok = file.good();
            }
        }
        if (ok) {
            std::error_code ec;
            fs::rename(temporary, target, ec);
            if (ec) {
                // rename() does not overwrite on some filesystems; fall back.
                fs::remove(target, ec);
                fs::rename(temporary, target, ec);
                ok = !ec;
            }
            if (!ok) fs::remove(temporary, ec);
        } else {
            std::error_code ec;
            fs::remove(temporary, ec);
        }
        if (!ok && error) *error = "could not write " + path;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        ok = false;
    }

    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return ok;
}

bool load(const std::string& path, std::string* payload, std::string* error) {
    std::vector<char> encrypted;
    try {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            if (error) *error = "no stored sign-in";
            return false;
        }
        const std::streamsize size = file.tellg();
        // A remembered credential is a few hundred bytes; anything large is not
        // ours and is not worth handing to the crypto layer.
        if (size <= 0 || size > 64 * 1024) {
            if (error) *error = "stored sign-in has an implausible size";
            return false;
        }
        file.seekg(0);
        encrypted.resize(static_cast<size_t>(size));
        if (!file.read(encrypted.data(), size)) {
            if (error) *error = "could not read " + path;
            return false;
        }
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }

    DATA_BLOB in = make_blob(encrypted.data(), encrypted.size());
    DATA_BLOB entropy = make_blob(kEntropy, sizeof(kEntropy));
    DATA_BLOB out{};

    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &out)) {
        // The usual causes are a different Windows account, a different
        // machine, or a corrupt file. All mean "sign in again".
        if (error) *error = last_error("CryptUnprotectData");
        return false;
    }

    payload->assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

void clear(const std::string& path) {
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(path + ".tmp", ec);
}

bool exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

}  // namespace credentials
