#include "common/store_key.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <utility>

#include "common/log.h"
#include "common/win_util.h"

namespace pimewin {
namespace {

constexpr uint32_t kKeyBytes = 32;

bool ReadProtected(const std::wstring& path, std::vector<uint8_t>* key) {
    std::vector<uint8_t> blob;
    if (!ReadFileBytes(path, &blob) || blob.empty()) return false;
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(blob.size());
    in.pbData = blob.data();
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    bool ok = out.cbData == kKeyBytes;
    if (ok) key->assign(out.pbData, out.pbData + out.cbData);
    if (out.pbData) LocalFree(out.pbData);
    return ok;
}

bool WriteProtected(const std::wstring& path, const std::vector<uint8_t>& key) {
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(key.size());
    in.pbData = const_cast<BYTE*>(key.data());
    DATA_BLOB out{};
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    bool ok = false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = WriteFile(file, out.pbData, out.cbData, &written, nullptr) &&
             written == out.cbData;
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    LocalFree(out.pbData);
    return ok;
}

}  // namespace

bool LoadOrCreateStoreKey(const std::wstring& data_dir, std::vector<uint8_t>* key) {
    if (!key) return false;
    key->clear();
    if (!EnsureDirectory(data_dir)) return false;
    const std::wstring path = PathJoin(data_dir, L"store.key");

    if (FileExists(path)) {
        if (ReadProtected(path, key)) return true;
        WriteLog(LogLevel::Warn, "store",
                 "store key unreadable; learning and the user dictionary are off");
        return false;
    }

    std::vector<uint8_t> fresh(kKeyBytes);
    if (BCryptGenRandom(nullptr, fresh.data(), static_cast<ULONG>(fresh.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        WriteLog(LogLevel::Warn, "store", "cannot generate a store key");
        return false;
    }
    if (WriteProtected(path, fresh)) {
        *key = std::move(fresh);
        return true;
    }
    // Another process may have won the CREATE_NEW race; use its key.
    if (ReadProtected(path, key)) return true;
    WriteLog(LogLevel::Warn, "store",
             "cannot create store key; learning and the user dictionary are off");
    return false;
}

}  // namespace pimewin