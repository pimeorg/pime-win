// Glue between pime-win's host UI and the user-dictionary entries that
// pime-pinyin-rank keeps in its plugin KV store.
//
// Key:   usr/<code>\x1f<word>   (code is PIME's raw: lower-case letters, no
//                               spaces, apostrophes kept)
// Value: <count>:<day>          (the same counter format rank writes)
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pimewin {

struct UserWord {
    std::string code;
    std::string word;
    uint32_t weight = 1;
};

constexpr char kUserDictAlias[] = "pime-pinyin-rank";
constexpr char kUserDictPrefix[] = "usr/";
constexpr char kUserDictSep = '\x1f';
constexpr uint32_t kMaxCodeBytes = 32;
constexpr uint32_t kMaxWordBytes = 64;
constexpr uint32_t kMaxWeight = 1000000;
// rank's kMaxKey is 128; the key includes the prefix and separator.
constexpr uint32_t kMaxDictKeyBytes = 128;

// Lower-cases, drops whitespace, keeps [a-z']. Returns false for any other
// byte or an empty/oversized result.
bool NormalizeUserCode(std::string_view input, std::string* out);

// Checks an already-normalized entry. `error` receives a short reason.
bool ValidateUserWord(const UserWord& word, std::string* error);

std::string MakeUserDictKey(const std::string& code, const std::string& word);
bool ParseUserDictKey(std::string_view key, std::string* code, std::string* word);

std::string EncodeUserDictValue(uint32_t count);
uint32_t DecodeUserDictValue(std::string_view value);

// Inserts Rime-style spaces between syllables ("nihao" -> "ni hao"). Unknown
// stretches are left untouched so an imported odd code still round-trips.
std::string DisplayUserCode(const std::string& code);

uint32_t ClampUserWeight(uint64_t weight);

}  // namespace pimewin