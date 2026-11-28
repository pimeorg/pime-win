#include "app/rime_dict.h"

#include <windows.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "common/win_util.h"

namespace pimewin {
namespace {

constexpr uint32_t kDefaultWeight = 100;

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// True for something that can only be a pinyin code, not a word.
bool LooksLikeCode(std::string_view text) {
    if (text.empty()) return false;
    for (char c : text) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ' ' ||
              c == '\'')) {
            return false;
        }
    }
    return true;
}

// Rime weights may be floats; keep the integer part and never round a
// nonzero weight down to zero.
uint32_t ParseWeight(std::string_view text) {
    uint64_t whole = 0;
    size_t i = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        whole = whole * 10 + static_cast<uint64_t>(text[i] - '0');
        if (whole > kMaxWeight) return kMaxWeight;
        ++i;
    }
    bool fraction = false;
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            if (text[i] != '0') fraction = true;
            ++i;
        }
    }
    if (whole == 0 && fraction) whole = 1;
    return ClampUserWeight(whole);
}

}  // namespace

RimeDictReadResult ReadRimeDict(const std::wstring& path) {
    RimeDictReadResult result;
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, &bytes)) {
        result.error = "无法读取文件";
        return result;
    }
    std::string_view all(reinterpret_cast<const char*>(bytes.data()),
                         bytes.size());
    if (all.substr(0, 3) == "\xEF\xBB\xBF") all.remove_prefix(3);

    // Column order from the front matter, when it names one.
    int text_column = -1;
    int code_column = -1;
    int weight_column = -1;
    bool in_header = false;
    bool reading_columns = false;
    int column_index = 0;

    std::unordered_map<std::string, size_t> seen;
    size_t pos = 0;
    while (pos < all.size()) {
        const size_t newline = all.find('\n', pos);
        const size_t end = newline == std::string_view::npos ? all.size() : newline;
        const std::string_view line = Trim(all.substr(pos, end - pos));
        pos = end + 1;

        if (line.empty() || line.front() == '#') continue;

        if (in_header) {
            if (line == "...") {
                in_header = false;
                continue;
            }
            if (line.front() == '-') {
                if (reading_columns) {
                    const std::string_view name = Trim(line.substr(1));
                    if (name == "text") text_column = column_index;
                    else if (name == "code") code_column = column_index;
                    else if (name == "weight") weight_column = column_index;
                    ++column_index;
                }
                continue;
            }
            reading_columns = line.rfind("columns:", 0) == 0;
            if (reading_columns) column_index = 0;
            continue;
        }
        if (line == "---") {
            in_header = true;
            reading_columns = false;
            column_index = 0;
            continue;
        }

        std::vector<std::string_view> fields;
        size_t start = 0;
        for (;;) {
            const size_t tab = line.find('\t', start);
            if (tab == std::string_view::npos) {
                fields.push_back(line.substr(start));
                break;
            }
            fields.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }

        int text_index = 0;
        int code_index = 1;
        int weight_index = 2;
        if (text_column >= 0 && code_column >= 0) {
            text_index = text_column;
            code_index = code_column;
            weight_index = weight_column;
        } else if (fields.size() >= 2 && LooksLikeCode(fields[0]) &&
                   !LooksLikeCode(fields[1])) {
            // PIME style: code first, then the word.
            code_index = 0;
            text_index = 1;
        }
        if (static_cast<size_t>(std::max(text_index, code_index)) >=
            fields.size()) {
            ++result.skipped;
            continue;
        }

        UserWord word;
        if (!NormalizeUserCode(Trim(fields[code_index]), &word.code)) {
            ++result.skipped;
            continue;
        }
        word.word = std::string(Trim(fields[text_index]));
        word.weight = weight_index >= 0 &&
                              static_cast<size_t>(weight_index) < fields.size()
                          ? ParseWeight(Trim(fields[weight_index]))
                          : kDefaultWeight;
        if (word.weight == 0) word.weight = kDefaultWeight;
        std::string error;
        if (!ValidateUserWord(word, &error)) {
            ++result.skipped;
            continue;
        }

        const std::string key = MakeUserDictKey(word.code, word.word);
        const auto it = seen.find(key);
        if (it != seen.end()) {
            result.words[it->second].weight = word.weight;
        } else {
            seen.emplace(key, result.words.size());
            result.words.push_back(std::move(word));
        }
    }

    result.ok = true;
    return result;
}

bool WriteRimeDict(const std::wstring& path, const std::vector<UserWord>& words,
                   bool plain, std::string* error) {
    std::string out;
    if (!plain) {
        out +=
            "---\n"
            "name: pime_user\n"
            "version: \"1.0\"\n"
            "sort: by_weight\n"
            "use_preset_vocabulary: false\n"
            "columns:\n"
            "  - text\n"
            "  - code\n"
            "  - weight\n"
            "...\n";
    }
    for (const UserWord& word : words) {
        out += word.word;
        out.push_back('\t');
        out += DisplayUserCode(word.code);
        out.push_back('\t');
        out += std::to_string(word.weight);
        out.push_back('\n');
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = "无法创建文件";
        return false;
    }
    DWORD written = 0;
    const bool ok =
        WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written,
                  nullptr) != FALSE &&
        written == out.size();
    CloseHandle(file);
    if (!ok) {
        *error = "写入文件失败";
        return false;
    }
    return true;
}

}  // namespace pimewin
