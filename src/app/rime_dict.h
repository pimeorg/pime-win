// Reading and writing user dictionaries in Rime's file formats.
//
// Accepted on import: Rime's `*.dict.yaml` (front matter, `columns:` order,
// `text/code/weight` rows), Rime's `custom_phrase.txt`, and PIME's own
// `code<TAB>word<TAB>freq` tables.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/user_dict_format.h"

namespace pimewin {

struct RimeDictReadResult {
    bool ok = false;
    std::string error;  // set when !ok
    std::vector<UserWord> words;
    uint32_t skipped = 0;
};

RimeDictReadResult ReadRimeDict(const std::wstring& path);

// Writes `words` as a Rime dict.yaml, or as a headerless
// `word<TAB>code<TAB>weight` table when `plain` is set. UTF-8, no BOM.
bool WriteRimeDict(const std::wstring& path, const std::vector<UserWord>& words,
                   bool plain, std::string* error);

}  // namespace pimewin
