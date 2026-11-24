#pragma once

#include <string>

namespace pimewin {

struct Settings {
    std::wstring chain = L"default.cbor";
    // 0 = follow system, 1 = light, 2 = dark.
    uint32_t theme = 0;
    uint32_t page_size = 6;
    // 1 = Chinese (compose), 0 = English (pass through).
    uint32_t chinese = 1;
};

Settings LoadSettings();
void SaveSettings(const Settings& settings);

// Per-application Chinese/English memory, keyed by the image name.
bool LoadAppMode(const std::wstring& app, bool fallback);
void SaveAppMode(const std::wstring& app, bool chinese);

}  // namespace pimewin
