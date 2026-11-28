#pragma once

#include <string>

namespace pimewin {

struct Settings {
    // Active scheme id with the ".cbor" suffix, e.g. "pinyin.cbor".
    std::wstring chain = L"pinyin.cbor";
    // 0 = follow system, 1 = light, 2 = dark.
    uint32_t theme = 0;
    uint32_t page_size = 6;
    // 1 = Chinese (compose), 0 = English (pass through).
    uint32_t chinese = 1;
    // 1 = full-width Chinese punctuation, 0 = ASCII punctuation.
    uint32_t punctuation = 1;
    // 0 = horizontal candidate strip, 1 = vertical list.
    uint32_t candidate_vertical = 0;
    // Candidate text size in DIP (12..24).
    uint32_t candidate_font = 15;
};

Settings LoadSettings();
void SaveSettings(const Settings& settings);

// Per-application Chinese/English memory, keyed by the image name.
bool LoadAppMode(const std::wstring& app, bool fallback);
void SaveAppMode(const std::wstring& app, bool chinese);

}  // namespace pimewin
