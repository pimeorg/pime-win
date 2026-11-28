#include "common/user_dict_format.h"

#include <chrono>
#include <cstddef>
#include <unordered_set>
#include <utility>

namespace pimewin {
namespace {

// Syllables taken from pinyin.dict's first column; used only to re-insert
// Rime's spaces when showing or exporting a code.
constexpr char kSyllableTable[] =
    "a ai an ang ao ba bai ban bang bao bei ben beng bi bian biao bie bin bing "
    "bo bu ca cai can cang cao ce cen ceng cha chai chan chang chao che chen "
    "cheng chi chong chou chu chua chuai chuan chuang chui chun chuo ci cong cou "
    "cu cuan cui cun cuo "
    "da dai dan dang dao de dei den deng di dia dian diao die ding diu dong dou "
    "du duan dui dun duo e ei en eng er fa fan fang fei fen feng fo fou fu "
    "ga gai gan gang gao ge gei gen geng gong gou gu gua guai guan guang gui gun "
    "guo ha hai han hang hao he hei hen heng hong hou hu hua huai huan huang hui "
    "hun huo "
    "ji jia jian jiang jiao jie jin jing jiong jiu ju juan jue jun ka kai kan "
    "kang kao ke kei ken keng kong kou ku kua kuai kuan kuang kui kun kuo "
    "la lai lan lang lao le lei leng li lia lian liang liao lie lin ling liu long "
    "lou lu luan lun luo lv lve "
    "ma mai man mang mao me mei men meng mi mian miao mie min ming miu mo mou mu "
    "na nai nan nang nao ne nei nen neng ni nian niang niao nie nin ning niu "
    "nong nou nu nuan nun nuo nv nve "
    "o ou pa pai pan pang pao pei pen peng pi pian piao pie pin ping po pou pu "
    "qi qia qian qiang qiao qie qin qing qiong qiu qu quan que qun ran rang rao "
    "re ren reng ri rong rou ru rua ruan rui run ruo "
    "sa sai san sang sao se sen seng sha shai shan shang shao she shei shen sheng "
    "shi shou shu shua shuai shuan shuang shui shun shuo si song sou su suan sui "
    "sun suo "
    "ta tai tan tang tao te tei teng ti tian tiao tie ting tong tou tu tuan tui "
    "tun tuo "
    "wa wai wan wang wei wen weng wo wu xi xia xian xiang xiao xie xin xing "
    "xiong xiu xu xuan xue xun "
    "ya yan yang yao ye yi yin ying yo yong you yu yuan yue yun za zai zan zang "
    "zao ze zei zen zeng "
    "zha zhai zhan zhang zhao zhe zhei zhen zheng zhi zhong zhou zhu zhua zhuai "
    "zhuan zhuang zhui zhun zhuo zi zong zou zu zuan zui zun zuo";

constexpr size_t kMaxSyllable = 6;  // "zhuang"

bool IsSyllable(std::string_view text) {
    static const std::unordered_set<std::string_view> table = [] {
        std::unordered_set<std::string_view> set;
        const std::string_view all(kSyllableTable);
        size_t pos = 0;
        while (pos < all.size()) {
            size_t end = all.find(' ', pos);
            if (end == std::string_view::npos) end = all.size();
            if (end > pos) set.insert(all.substr(pos, end - pos));
            pos = end + 1;
        }
        return set;
    }();
    return table.count(text) != 0;
}

// Whole-string UTF-8 check; rejects overlongs and surrogates cheaply enough
// for the handful of entries a person can type.
bool ValidUtf8(std::string_view text) {
    size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        size_t extra = 0;
        if (lead < 0x80) {
            extra = 0;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            extra = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            extra = 2;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= text.size()) return false;
        for (size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80)
                return false;
        }
        i += extra + 1;
    }
    return true;
}

uint32_t TodayUtcDay() {
    const auto hours = std::chrono::duration_cast<std::chrono::hours>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return static_cast<uint32_t>(hours / 24);
}

}  // namespace

bool NormalizeUserCode(std::string_view input, std::string* out) {
    std::string code;
    code.reserve(input.size());
    for (char c : input) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || c == '\'')) return false;
        code.push_back(c);
    }
    if (code.empty() || code.size() > kMaxCodeBytes) return false;
    *out = std::move(code);
    return true;
}

bool ValidateUserWord(const UserWord& word, std::string* error) {
    if (word.code.empty()) {
        *error = "拼音码不能为空";
        return false;
    }
    if (word.code.size() > kMaxCodeBytes) {
        *error = "拼音码太长";
        return false;
    }
    for (char c : word.code) {
        if (!((c >= 'a' && c <= 'z') || c == '\'')) {
            *error = "拼音码只能包含字母和撇号";
            return false;
        }
    }
    if (word.word.empty()) {
        *error = "词不能为空";
        return false;
    }
    if (word.word.size() > kMaxWordBytes) {
        *error = "词太长";
        return false;
    }
    if (!ValidUtf8(word.word)) {
        *error = "词不是有效的 UTF-8 文本";
        return false;
    }
    for (unsigned char c : word.word) {
        if (c < 0x20 || c == 0x7f) {
            *error = "词不能包含换行或控制字符";
            return false;
        }
    }
    if (word.weight == 0 || word.weight > kMaxWeight) {
        *error = "权重需在 1 到 1000000 之间";
        return false;
    }
    if (MakeUserDictKey(word.code, word.word).size() >= kMaxDictKeyBytes) {
        *error = "拼音码与词合计太长";
        return false;
    }
    return true;
}

std::string MakeUserDictKey(const std::string& code, const std::string& word) {
    std::string key(kUserDictPrefix);
    key += code;
    key.push_back(kUserDictSep);
    key += word;
    return key;
}

bool ParseUserDictKey(std::string_view key, std::string* code,
                      std::string* word) {
    constexpr std::string_view prefix(kUserDictPrefix,
                                      sizeof(kUserDictPrefix) - 1);
    if (key.size() <= prefix.size() ||
        key.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    const size_t sep = key.find(kUserDictSep, prefix.size());
    if (sep == std::string_view::npos || sep == prefix.size() ||
        sep + 1 >= key.size()) {
        return false;
    }
    *code = std::string(key.substr(prefix.size(), sep - prefix.size()));
    *word = std::string(key.substr(sep + 1));
    return true;
}

std::string EncodeUserDictValue(uint32_t count) {
    return std::to_string(ClampUserWeight(count)) + ":" +
           std::to_string(TodayUtcDay());
}

uint32_t DecodeUserDictValue(std::string_view value) {
    uint64_t count = 0;
    size_t i = 0;
    while (i < value.size() && value[i] >= '0' && value[i] <= '9') {
        count = count * 10 + static_cast<uint64_t>(value[i] - '0');
        if (count > kMaxWeight) return kMaxWeight;
        ++i;
    }
    return static_cast<uint32_t>(count);
}

std::string DisplayUserCode(const std::string& code) {
    std::string out;
    out.reserve(code.size() + code.size() / 3);
    size_t i = 0;
    while (i < code.size()) {
        if (code[i] == '\'') {
            out.push_back('\'');
            ++i;
            continue;
        }
        size_t matched = 0;
        for (size_t len = kMaxSyllable; len >= 1; --len) {
            if (i + len > code.size()) continue;
            if (IsSyllable(std::string_view(code).substr(i, len))) {
                matched = len;
                break;
            }
        }
        if (matched == 0) {
            out.push_back(code[i]);
            ++i;
            continue;
        }
        if (!out.empty() && out.back() != ' ' && out.back() != '\'')
            out.push_back(' ');
        out.append(code, i, matched);
        i += matched;
    }
    return out;
}

uint32_t ClampUserWeight(uint64_t weight) {
    if (weight > kMaxWeight) return kMaxWeight;
    return static_cast<uint32_t>(weight);
}

}  // namespace pimewin
