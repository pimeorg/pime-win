#include "common/chain_file.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string_view>
#include <utility>

#include "common/win_util.h"

namespace pimewin {
namespace {

// ---- minimal CBOR ----------------------------------------------------------
//
// Chain files are produced by chainc and only ever contain definite-length
// maps, arrays, text strings, integers and booleans. That is all we parse;
// anything else is skipped rather than rejected so a future key cannot break
// the settings window.

struct Cursor {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t off = 0;
    bool ok = true;

    bool Need(uint64_t count) {
        if (!ok || count > size - off) {
            ok = false;
            return false;
        }
        return true;
    }

    bool Byte(uint8_t* out) {
        if (!Need(1)) return false;
        *out = data[off++];
        return true;
    }

    bool Head(uint8_t* major, uint64_t* value) {
        uint8_t first = 0;
        if (!Byte(&first)) return false;
        *major = first >> 5;
        const uint8_t extra = first & 0x1f;
        if (extra < 24) {
            *value = extra;
            return true;
        }
        uint64_t result = 0;
        if (extra == 24) {
            uint8_t v = 0;
            if (!Byte(&v)) return false;
            result = v;
        } else if (extra == 25) {
            uint8_t b[2] = {};
            if (!Need(2)) return false;
            std::memcpy(b, data + off, 2);
            off += 2;
            result = (static_cast<uint64_t>(b[0]) << 8) | b[1];
        } else if (extra == 26) {
            uint8_t b[4] = {};
            if (!Need(4)) return false;
            std::memcpy(b, data + off, 4);
            off += 4;
            for (int i = 0; i < 4; ++i) result = (result << 8) | b[i];
        } else if (extra == 27) {
            uint8_t b[8] = {};
            if (!Need(8)) return false;
            std::memcpy(b, data + off, 8);
            off += 8;
            for (int i = 0; i < 8; ++i) result = (result << 8) | b[i];
        } else {
            ok = false;  // indefinite length or reserved
            return false;
        }
        *value = result;
        return true;
    }

    bool Text(std::string* out) {
        uint8_t major = 0;
        uint64_t len = 0;
        if (!Head(&major, &len) || major != 3 || !Need(len)) {
            ok = false;
            return false;
        }
        out->assign(reinterpret_cast<const char*>(data + off),
                    static_cast<size_t>(len));
        off += static_cast<size_t>(len);
        return true;
    }

    bool Skip(int depth) {
        if (depth > 16) {
            ok = false;
            return false;
        }
        uint8_t major = 0;
        uint64_t len = 0;
        if (!Head(&major, &len)) return false;
        switch (major) {
            case 0:
            case 1:
            case 7:
                return true;
            case 2:
            case 3:
                return Need(len) && ((off += static_cast<size_t>(len)), true);
            case 4:
                for (uint64_t i = 0; i < len; ++i)
                    if (!Skip(depth + 1)) return false;
                return true;
            case 5:
                for (uint64_t i = 0; i < len * 2; ++i)
                    if (!Skip(depth + 1)) return false;
                return true;
            case 6:
                return Skip(depth + 1);
            default:
                ok = false;
                return false;
        }
    }

    // Reads one scalar setting value. Unsupported types consume their body and
    // report false so the caller can leave the key out.
    bool Scalar(ChainValue* out) {
        uint8_t major = 0;
        uint64_t raw = 0;
        if (!Head(&major, &raw)) return false;
        switch (major) {
            case 0:
                out->kind = ChainValue::Kind::Int;
                out->integer = static_cast<int64_t>(raw);
                return true;
            case 1:
                out->kind = ChainValue::Kind::Int;
                out->integer = -1 - static_cast<int64_t>(raw);
                return true;
            case 3:
                out->kind = ChainValue::Kind::Text;
                out->text.assign(reinterpret_cast<const char*>(data + off),
                                 static_cast<size_t>(raw));
                off += static_cast<size_t>(raw);
                return true;
            case 7:
                if (raw == 20 || raw == 21) {
                    out->kind = ChainValue::Kind::Bool;
                    out->boolean = raw == 21;
                    return true;
                }
                return false;
            default:
                return false;
        }
    }
};

bool ReadPlugins(Cursor* c, std::vector<ChainPlugin>* out) {
    uint8_t major = 0;
    uint64_t count = 0;
    if (!c->Head(&major, &count) || major != 4) return false;
    for (uint64_t i = 0; i < count; ++i) {
        uint8_t entry_major = 0;
        uint64_t fields = 0;
        if (!c->Head(&entry_major, &fields) || entry_major != 5) return false;
        ChainPlugin plugin;
        for (uint64_t f = 0; f < fields; ++f) {
            std::string key;
            if (!c->Text(&key)) return false;
            if (key == "id") {
                if (!c->Text(&plugin.id)) return false;
            } else if (key == "version") {
                if (!c->Text(&plugin.version)) return false;
            } else if (key == "alias") {
                if (!c->Text(&plugin.alias)) return false;
            } else if (key == "role") {
                if (!c->Text(&plugin.role)) return false;
            } else if (key == "settings") {
                uint8_t settings_major = 0;
                uint64_t settings_count = 0;
                if (!c->Head(&settings_major, &settings_count) ||
                    settings_major != 5) {
                    return false;
                }
                for (uint64_t s = 0; s < settings_count; ++s) {
                    ChainSetting setting;
                    if (!c->Text(&setting.key)) return false;
                    ChainValue value;
                    if (c->Scalar(&value)) {
                        setting.value = std::move(value);
                        plugin.settings.push_back(std::move(setting));
                    }
                }
            } else if (!c->Skip(0)) {
                return false;
            }
        }
        if (!plugin.id.empty()) out->push_back(std::move(plugin));
    }
    return true;
}

void PutHead(std::vector<uint8_t>* out, uint8_t major, uint64_t value) {
    const uint8_t base = static_cast<uint8_t>(major << 5);
    if (value < 24) {
        out->push_back(static_cast<uint8_t>(base | value));
    } else if (value <= 0xff) {
        out->push_back(static_cast<uint8_t>(base | 24));
        out->push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xffff) {
        out->push_back(static_cast<uint8_t>(base | 25));
        out->push_back(static_cast<uint8_t>(value >> 8));
        out->push_back(static_cast<uint8_t>(value));
    } else if (value <= 0xffffffffull) {
        out->push_back(static_cast<uint8_t>(base | 26));
        for (int shift = 24; shift >= 0; shift -= 8)
            out->push_back(static_cast<uint8_t>(value >> shift));
    } else {
        out->push_back(static_cast<uint8_t>(base | 27));
        for (int shift = 56; shift >= 0; shift -= 8)
            out->push_back(static_cast<uint8_t>(value >> shift));
    }
}

void PutText(std::vector<uint8_t>* out, const std::string& text) {
    PutHead(out, 3, text.size());
    out->insert(out->end(), text.begin(), text.end());
}

void PutValue(std::vector<uint8_t>* out, const ChainValue& value) {
    switch (value.kind) {
        case ChainValue::Kind::Bool:
            out->push_back(value.boolean ? 0xf5 : 0xf4);
            return;
        case ChainValue::Kind::Int:
            if (value.integer >= 0) {
                PutHead(out, 0, static_cast<uint64_t>(value.integer));
            } else {
                PutHead(out, 1, static_cast<uint64_t>(-1 - value.integer));
            }
            return;
        case ChainValue::Kind::Text:
            PutText(out, value.text);
            return;
    }
}

void PutPlugin(std::vector<uint8_t>* out, const ChainPlugin& plugin) {
    uint64_t count = 1;  // id
    if (!plugin.version.empty()) ++count;
    if (!plugin.alias.empty()) ++count;
    if (!plugin.role.empty()) ++count;
    if (!plugin.settings.empty()) ++count;
    PutHead(out, 5, count);
    PutText(out, "id");
    PutText(out, plugin.id);
    if (!plugin.version.empty()) {
        PutText(out, "version");
        PutText(out, plugin.version);
    }
    if (!plugin.alias.empty()) {
        PutText(out, "alias");
        PutText(out, plugin.alias);
    }
    if (!plugin.role.empty()) {
        PutText(out, "role");
        PutText(out, plugin.role);
    }
    if (!plugin.settings.empty()) {
        PutText(out, "settings");
        PutHead(out, 5, plugin.settings.size());
        for (const ChainSetting& setting : plugin.settings) {
            PutText(out, setting.key);
            PutValue(out, setting.value);
        }
    }
}

// ---- manifest.yaml ---------------------------------------------------------
//
// Only a handful of fields are needed and every bundled manifest writes its
// settings as one flow map per line. This is a tolerant subset parser, not a
// general YAML reader.

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

std::string Unquote(std::string_view text) {
    text = Trim(text);
    if (text.size() >= 2) {
        const char front = text.front();
        if ((front == '"' || front == '\'') && text.back() == front) {
            text.remove_prefix(1);
            text.remove_suffix(1);
        }
    }
    return std::string(text);
}

// Splits on commas that are not inside [] or quotes.
std::vector<std::string> SplitFlow(std::string_view body) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    char quote = 0;
    for (char ch : body) {
        if (quote) {
            if (ch == quote) quote = 0;
            current.push_back(ch);
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
            current.push_back(ch);
        } else if (ch == '[' || ch == '{') {
            ++depth;
            current.push_back(ch);
        } else if (ch == ']' || ch == '}') {
            --depth;
            current.push_back(ch);
        } else if (ch == ',' && depth == 0) {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!Trim(current).empty()) parts.push_back(current);
    return parts;
}

size_t FindColon(std::string_view text) {
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (quote) {
            if (ch == quote) quote = 0;
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '[' || ch == '{') {
            ++depth;
        } else if (ch == ']' || ch == '}') {
            --depth;
        } else if (ch == ':' && depth == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

PluginSettingSpec ParseSettingSpec(std::string_view body) {
    PluginSettingSpec spec;
    std::string default_text;
    bool has_default = false;
    for (const std::string& part : SplitFlow(body)) {
        const size_t colon = FindColon(part);
        if (colon == std::string::npos) continue;
        const std::string key = Unquote(part.substr(0, colon));
        const std::string_view raw = Trim(part.substr(colon + 1));
        if (key == "key") {
            spec.key = Unquote(raw);
        } else if (key == "type") {
            spec.type = Unquote(raw);
        } else if (key == "default") {
            default_text = Unquote(raw);
            has_default = true;
        } else if (key == "min") {
            spec.min = std::strtoll(std::string(raw).c_str(), nullptr, 10);
        } else if (key == "max") {
            spec.max = std::strtoll(std::string(raw).c_str(), nullptr, 10);
        } else if (key == "values") {
            std::string_view list = raw;
            if (!list.empty() && list.front() == '[') {
                list.remove_prefix(1);
                if (!list.empty() && list.back() == ']') list.remove_suffix(1);
                for (const std::string& item : SplitFlow(list)) {
                    spec.values.push_back(Unquote(item));
                }
            }
        }
    }
    if (spec.key.empty() || spec.type.empty()) return {};
    spec.has_default = has_default;
    if (spec.type == "bool") {
        spec.def.kind = ChainValue::Kind::Bool;
        spec.def.boolean = default_text == "true";
    } else if (spec.type == "int") {
        spec.def.kind = ChainValue::Kind::Int;
        spec.def.integer = std::strtoll(default_text.c_str(), nullptr, 10);
    } else {
        spec.def.kind = ChainValue::Kind::Text;
        spec.def.text = default_text;
    }
    return spec;
}

PluginMeta ParseManifest(std::string_view text) {
    PluginMeta meta;
    bool in_settings = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;
        const std::string_view trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#') continue;

        const bool indented = line.front() == ' ' || line.front() == '\t';
        if (in_settings && indented) {
            if (trimmed.rfind("- {", 0) == 0 && trimmed.back() == '}') {
                PluginSettingSpec spec = ParseSettingSpec(
                    trimmed.substr(3, trimmed.size() - 4));
                if (!spec.key.empty()) {
                    meta.settings.push_back(std::move(spec));
                }
            }
            continue;
        }
        if (indented) continue;

        const size_t colon = trimmed.find(':');
        if (colon == std::string_view::npos) {
            in_settings = false;
            continue;
        }
        const std::string key(Trim(trimmed.substr(0, colon)));
        const std::string value = Unquote(trimmed.substr(colon + 1));
        if (key == "settings") {
            in_settings = true;
        } else {
            in_settings = false;
            if (key == "id") meta.id = value;
            else if (key == "version") meta.version = value;
            else if (key == "role") meta.role = value;
            else if (key == "description") meta.description = value;
        }
    }
    return meta;
}

// scheme.yaml is flat: only the top-level keys matter, the plugin manifest is
// an indented list the build reads. This is the same tolerant subset parser
// the manifests use.
SchemeInfo ParseScheme(std::string_view text) {
    SchemeInfo info;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(start, end - start);
        start = end + 1;
        const std::string_view trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#') continue;
        if (line.front() == ' ' || line.front() == '\t') continue;

        const size_t colon = trimmed.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string key(Trim(trimmed.substr(0, colon)));
        const std::string value = Unquote(trimmed.substr(colon + 1));
        if (key == "id") info.id = value;
        else if (key == "name") info.name = value;
        else if (key == "description") info.description = value;
        else if (key == "version") info.version = value;
        else if (key == "chain") info.chain = value;
    }
    return info;
}

std::wstring ParentDirectory(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// The common schemes lead the list; everything else is alphabetical by id.
int SchemeOrder(const std::string& id) {
    static const char* const kOrder[] = {"pinyin", "english"};
    for (int i = 0; i < static_cast<int>(std::size(kOrder)); ++i) {
        if (id == kOrder[i]) return i;
    }
    return static_cast<int>(std::size(kOrder));
}

bool NewerVersion(const std::string& a, const std::string& b) {
    const auto parts = [](const std::string& v) {
        std::vector<long> out;
        size_t start = 0;
        while (start <= v.size()) {
            size_t dot = v.find('.', start);
            if (dot == std::string::npos) dot = v.size();
            out.push_back(std::strtol(v.substr(start, dot - start).c_str(),
                                      nullptr, 10));
            start = dot + 1;
        }
        return out;
    };
    const std::vector<long> pa = parts(a);
    const std::vector<long> pb = parts(b);
    return std::lexicographical_compare(pb.begin(), pb.end(), pa.begin(),
                                        pa.end());
}

}  // namespace

bool ReadChainFile(const std::wstring& path, ChainDoc* out, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, &bytes)) {
        if (error) *error = "cannot read chain file";
        return false;
    }
    Cursor cursor{bytes.data(), bytes.size()};
    uint8_t major = 0;
    uint64_t count = 0;
    if (!cursor.Head(&major, &count) || major != 5) {
        if (error) *error = "chain is not a CBOR map";
        return false;
    }
    ChainDoc doc;
    for (uint64_t i = 0; i < count; ++i) {
        std::string key;
        if (!cursor.Text(&key)) break;
        if (key == "plugins") {
            if (!ReadPlugins(&cursor, &doc.plugins)) break;
        } else if (key == "base") {
            if (!cursor.Text(&doc.base)) break;
        } else if (key == "disabled") {
            uint8_t list_major = 0;
            uint64_t list_count = 0;
            if (!cursor.Head(&list_major, &list_count) || list_major != 4) break;
            for (uint64_t d = 0; d < list_count; ++d) {
                std::string id;
                if (!cursor.Text(&id)) break;
                doc.disabled.push_back(std::move(id));
            }
        } else if (!cursor.Skip(0)) {
            break;
        }
    }
    if (!cursor.ok) {
        if (error) *error = "chain file is corrupt";
        return false;
    }
    *out = std::move(doc);
    return true;
}

bool WriteChainFile(const std::wstring& path, const ChainDoc& doc,
                    std::string* error) {
    std::vector<uint8_t> bytes;
    uint64_t count = 5;  // api, id, description, core, plugins
    if (!doc.base.empty()) ++count;
    if (!doc.disabled.empty()) ++count;
    PutHead(&bytes, 5, count);
    PutText(&bytes, "api");
    PutHead(&bytes, 0, 1);
    PutText(&bytes, "id");
    PutText(&bytes, "pime-custom");
    PutText(&bytes, "description");
    PutText(&bytes, "自定义方案");
    PutText(&bytes, "core");
    PutText(&bytes, ">=1.0.0");
    if (!doc.base.empty()) {
        PutText(&bytes, "base");
        PutText(&bytes, doc.base);
    }
    if (!doc.disabled.empty()) {
        PutText(&bytes, "disabled");
        PutHead(&bytes, 4, doc.disabled.size());
        for (const std::string& id : doc.disabled) PutText(&bytes, id);
    }
    PutText(&bytes, "plugins");
    PutHead(&bytes, 4, doc.plugins.size());
    for (const ChainPlugin& plugin : doc.plugins) PutPlugin(&bytes, plugin);

    if (!WriteFileBytes(path, bytes)) {
        if (error) *error = "cannot write chain file";
        return false;
    }
    return true;
}

namespace {

void ScanPluginCatalog(const std::wstring& plugins_dir,
                       std::vector<PluginMeta>* catalog) {
    const std::wstring pattern = PathJoin(plugins_dir, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (data.cFileName[0] == L'.') continue;
        std::vector<uint8_t> bytes;
        const std::wstring manifest =
            PathJoin(PathJoin(plugins_dir, data.cFileName), L"manifest.yaml");
        if (!ReadFileBytes(manifest, &bytes)) continue;
        const std::string text(reinterpret_cast<const char*>(bytes.data()),
                               bytes.size());
        PluginMeta meta = ParseManifest(text);
        if (!meta.id.empty()) catalog->push_back(std::move(meta));
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

// Ids ascending, newest version first; a scheme's plugin copies and the legacy
// shared directory can both contribute, so drop repeated (id, version) pairs.
void SortPluginCatalog(std::vector<PluginMeta>* catalog) {
    std::sort(catalog->begin(), catalog->end(),
              [](const PluginMeta& a, const PluginMeta& b) {
                  if (a.id != b.id) return a.id < b.id;
                  return NewerVersion(a.version, b.version);
              });
    catalog->erase(std::unique(catalog->begin(), catalog->end(),
                               [](const PluginMeta& a, const PluginMeta& b) {
                                   return a.id == b.id &&
                                          a.version == b.version;
                               }),
                   catalog->end());
}

void ScanSchemeDir(const std::wstring& root, bool user,
                   std::vector<SchemeInfo>* out) {
    const std::wstring pattern = PathJoin(root, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (data.cFileName[0] == L'.') continue;
        const std::wstring dir = PathJoin(root, data.cFileName);
        SchemeInfo info;
        std::string error;
        if (!ReadSchemeFile(PathJoin(dir, L"scheme.yaml"), &info, &error)) {
            continue;
        }
        info.user = user;
        bool replaced = false;
        for (SchemeInfo& existing : *out) {
            if (existing.id == info.id) {
                existing = std::move(info);
                replaced = true;
                break;
            }
        }
        if (!replaced) out->push_back(std::move(info));
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

}  // namespace

std::vector<PluginMeta> LoadPluginCatalog(const std::wstring& plugins_dir) {
    std::vector<PluginMeta> catalog;
    ScanPluginCatalog(plugins_dir, &catalog);
    SortPluginCatalog(&catalog);
    return catalog;
}

bool ReadSchemeFile(const std::wstring& path, SchemeInfo* out,
                    std::string* error) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, &bytes)) {
        if (error) *error = "cannot read scheme file";
        return false;
    }
    const std::string text(reinterpret_cast<const char*>(bytes.data()),
                           bytes.size());
    SchemeInfo info = ParseScheme(text);
    if (info.id.empty() || info.chain.empty()) {
        if (error) *error = "scheme file lacks id or chain";
        return false;
    }
    info.dir = ParentDirectory(path);
    *out = std::move(info);
    return true;
}

std::vector<SchemeInfo> ListSchemePackages(const std::wstring& resources_dir,
                                           const std::wstring& data_dir) {
    std::vector<SchemeInfo> schemes;
    ScanSchemeDir(PathJoin(resources_dir, L"schemes"), false, &schemes);
    ScanSchemeDir(PathJoin(data_dir, L"schemes"), true, &schemes);
    std::sort(schemes.begin(), schemes.end(),
              [](const SchemeInfo& a, const SchemeInfo& b) {
                  const int oa = SchemeOrder(a.id);
                  const int ob = SchemeOrder(b.id);
                  return oa != ob ? oa < ob : a.id < b.id;
              });
    return schemes;
}

std::wstring SchemeBaseChainPath(const std::wstring& resources_dir,
                                 const std::wstring& data_dir,
                                 const std::string& id) {
    const std::wstring name = ToWide(id);
    SchemeInfo info;
    std::string error;
    const std::wstring user_dir = PathJoin(PathJoin(data_dir, L"schemes"), name);
    if (ReadSchemeFile(PathJoin(user_dir, L"scheme.yaml"), &info, &error)) {
        const std::wstring path = PathJoin(user_dir, ToWide(info.chain));
        if (FileExists(path)) return path;
    }

    const std::wstring bundled_dir =
        PathJoin(PathJoin(resources_dir, L"schemes"), name);
    if (ReadSchemeFile(PathJoin(bundled_dir, L"scheme.yaml"), &info, &error)) {
        const std::wstring path = PathJoin(bundled_dir, ToWide(info.chain));
        if (FileExists(path)) return path;
    }

    // Legacy layout from before scheme packages: <resources>/chains/<id>.cbor.
    const std::wstring legacy =
        PathJoin(PathJoin(resources_dir, L"chains"), name + L".cbor");
    if (FileExists(legacy)) return legacy;
    return std::wstring();
}

std::wstring SchemeChainPath(const std::wstring& resources_dir,
                             const std::wstring& data_dir,
                             const std::string& id) {
    const std::wstring user_dir =
        PathJoin(PathJoin(data_dir, L"schemes"), ToWide(id));
    const std::wstring overlay = PathJoin(user_dir, L"custom.cbor");
    if (FileExists(overlay)) return overlay;
    return SchemeBaseChainPath(resources_dir, data_dir, id);
}

std::vector<std::wstring> SchemePluginDirs(const std::wstring& resources_dir,
                                           const std::wstring& data_dir,
                                           const std::string& id) {
    const std::wstring name = ToWide(id);
    std::vector<std::wstring> dirs;
    const std::wstring user =
        PathJoin(PathJoin(PathJoin(data_dir, L"schemes"), name), L"plugins");
    if (DirectoryExists(user)) dirs.push_back(user);
    const std::wstring bundled = PathJoin(
        PathJoin(PathJoin(resources_dir, L"schemes"), name), L"plugins");
    if (DirectoryExists(bundled)) dirs.push_back(bundled);
    if (dirs.empty()) {
        const std::wstring legacy = PathJoin(resources_dir, L"plugins");
        if (DirectoryExists(legacy)) dirs.push_back(legacy);
    }
    return dirs;
}

std::vector<PluginMeta> LoadPluginCatalogFromSchemes(
    const std::wstring& resources_dir, const std::wstring& data_dir) {
    std::vector<PluginMeta> catalog;
    std::vector<std::wstring> scanned;
    for (const SchemeInfo& scheme : ListSchemePackages(resources_dir, data_dir)) {
        for (const std::wstring& dir :
             SchemePluginDirs(resources_dir, data_dir, scheme.id)) {
            if (std::find(scanned.begin(), scanned.end(), dir) !=
                scanned.end()) {
                continue;
            }
            scanned.push_back(dir);
            ScanPluginCatalog(dir, &catalog);
        }
    }
    if (scanned.empty()) {
        ScanPluginCatalog(PathJoin(resources_dir, L"plugins"), &catalog);
    }
    SortPluginCatalog(&catalog);
    return catalog;
}

}  // namespace pimewin
