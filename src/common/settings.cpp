#include "common/settings.h"

#include <windows.h>

#include "common/win_util.h"

namespace pimewin {
namespace {

constexpr wchar_t kRoot[] = L"Software\\PIME";
constexpr wchar_t kAppMode[] = L"Software\\PIME\\AppMode";

}  // namespace

Settings LoadSettings() {
    Settings settings;
    settings.chain = RegistryGetString(HKEY_CURRENT_USER, kRoot, L"Chain",
                                       L"default.cbor");
    settings.theme = RegistryGetDword(HKEY_CURRENT_USER, kRoot, L"Theme", 0);
    settings.page_size =
        RegistryGetDword(HKEY_CURRENT_USER, kRoot, L"PageSize", 6);
    settings.chinese = RegistryGetDword(HKEY_CURRENT_USER, kRoot, L"Chinese", 1);

    if (settings.chain.empty() || settings.chain.find(L"..") != std::wstring::npos ||
        settings.chain.find_first_of(L"\\/:") != std::wstring::npos) {
        settings.chain = L"default.cbor";
    }
    if (settings.theme > 2) settings.theme = 0;
    if (settings.page_size < 3) settings.page_size = 3;
    if (settings.page_size > 9) settings.page_size = 9;
    settings.chinese = settings.chinese ? 1 : 0;
    return settings;
}

void SaveSettings(const Settings& settings) {
    RegistrySetString(HKEY_CURRENT_USER, kRoot, L"Chain", settings.chain);
    RegistrySetDword(HKEY_CURRENT_USER, kRoot, L"Theme", settings.theme);
    RegistrySetDword(HKEY_CURRENT_USER, kRoot, L"PageSize", settings.page_size);
    RegistrySetDword(HKEY_CURRENT_USER, kRoot, L"Chinese", settings.chinese);
}

bool LoadAppMode(const std::wstring& app, bool fallback) {
    if (app.empty()) return fallback;
    return RegistryGetDword(HKEY_CURRENT_USER, kAppMode, app, fallback ? 1 : 0) != 0;
}

void SaveAppMode(const std::wstring& app, bool chinese) {
    if (app.empty()) return;
    RegistrySetDword(HKEY_CURRENT_USER, kAppMode, app, chinese ? 1 : 0);
}

}  // namespace pimewin
