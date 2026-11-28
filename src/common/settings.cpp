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
                                       L"pinyin.cbor");
    settings.theme = RegistryGetDword(HKEY_CURRENT_USER, kRoot, L"Theme", 0);
    settings.page_size =
        RegistryGetDword(HKEY_CURRENT_USER, kRoot, L"PageSize", 6);
    settings.chinese = RegistryGetDword(HKEY_CURRENT_USER, kRoot, L"Chinese", 1);
    settings.punctuation =
        RegistryGetDword(HKEY_CURRENT_USER, kRoot, L"Punctuation", 1);
    settings.candidate_vertical =
        RegistryGetDword(HKEY_CURRENT_USER, kRoot, L"CandidateVertical", 0);
    settings.candidate_font =
        RegistryGetDword(HKEY_CURRENT_USER, kRoot, L"CandidateFont", 15);

    // Migrate the pre-scheme layout: the pinyin chain used to be default.cbor,
    // and an edited chain lived in chains/custom.cbor. The latter becomes the
    // pinyin scheme's user edit so upgrades keep the user's pipeline.
    if (settings.chain == L"default.cbor") settings.chain = L"pinyin.cbor";
    if (settings.chain == L"custom.cbor") {
        const std::wstring legacy =
            PathJoin(PathJoin(DataDirectory(), L"chains"), L"custom.cbor");
        const std::wstring dir =
            PathJoin(PathJoin(DataDirectory(), L"schemes"), L"pinyin");
        const std::wstring overlay = PathJoin(dir, L"custom.cbor");
        std::vector<uint8_t> bytes;
        if (FileExists(legacy) && !FileExists(overlay) &&
            ReadFileBytes(legacy, &bytes) && EnsureDirectory(dir)) {
            WriteFileBytes(overlay, bytes);
        }
        settings.chain = L"pinyin.cbor";
    }
    if (settings.chain.empty() || settings.chain.find(L"..") != std::wstring::npos ||
        settings.chain.find_first_of(L"\\/:") != std::wstring::npos) {
        settings.chain = L"pinyin.cbor";
    }
    if (settings.theme > 2) settings.theme = 0;
    if (settings.page_size < 3) settings.page_size = 3;
    if (settings.page_size > 9) settings.page_size = 9;
    settings.chinese = settings.chinese ? 1 : 0;
    settings.punctuation = settings.punctuation ? 1 : 0;
    settings.candidate_vertical = settings.candidate_vertical ? 1 : 0;
    if (settings.candidate_font < 12) settings.candidate_font = 12;
    if (settings.candidate_font > 24) settings.candidate_font = 24;
    return settings;
}

void SaveSettings(const Settings& settings) {
    RegistrySetString(HKEY_CURRENT_USER, kRoot, L"Chain", settings.chain);
    RegistrySetDword(HKEY_CURRENT_USER, kRoot, L"Theme", settings.theme);
    RegistrySetDword(HKEY_CURRENT_USER, kRoot, L"PageSize", settings.page_size);
    RegistrySetDword(HKEY_CURRENT_USER, kRoot, L"Chinese", settings.chinese);
    RegistrySetDword(HKEY_CURRENT_USER, kRoot, L"Punctuation",
                     settings.punctuation);
    RegistrySetDword(HKEY_CURRENT_USER, kRoot, L"CandidateVertical",
                     settings.candidate_vertical);
    RegistrySetDword(HKEY_CURRENT_USER, kRoot, L"CandidateFont",
                     settings.candidate_font);
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
