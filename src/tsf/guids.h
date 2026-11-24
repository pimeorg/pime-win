// Stable identifiers shared by the text service, the registration code and
// the settings app. Keep them in sync with docs/README.md.
#pragma once

#include <windows.h>

namespace pimewin {

inline const CLSID kClsidTextService = {
    0x6c1d9a2e,
    0x31b7,
    0x4e52,
    {0x9e, 0x0c, 0x8a, 0x5d, 0x51, 0xa3, 0xc7, 0xf5}};

inline const GUID kGuidProfile = {
    0x6c1d9a2e,
    0x31b7,
    0x4e52,
    {0x9e, 0x0c, 0x8a, 0x5d, 0x51, 0xa3, 0xc7, 0xf6}};

inline const GUID kGuidDisplayAttribute = {
    0x6c1d9a2e,
    0x31b7,
    0x4e52,
    {0x9e, 0x0c, 0x8a, 0x5d, 0x51, 0xa3, 0xc7, 0xf7}};

inline constexpr LANGID kLangId = 0x0804;  // zh-CN
inline constexpr wchar_t kProfileDescription[] = L"PIME 拼音";
inline constexpr wchar_t kTextServiceDescription[] = L"PIME 输入法";

}  // namespace pimewin
