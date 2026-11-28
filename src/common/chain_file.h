// Reading and writing chain (.cbor) files from the settings app.
//
// A chain is a small CBOR map: { "plugins": [ {id, version?, alias?, role?,
// settings?} ] }. pime-core only reads "plugins"; the extra keys below let the
// settings UI round-trip its own custom chain and remember which preset it was
// derived from.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pimewin {

struct ChainValue {
    enum class Kind { Bool, Int, Text };
    Kind kind = Kind::Text;
    bool boolean = false;
    int64_t integer = 0;
    std::string text;
};

struct ChainSetting {
    std::string key;
    ChainValue value;
};

struct ChainPlugin {
    std::string id;
    std::string version;
    std::string alias;
    std::string role;
    std::vector<ChainSetting> settings;
};

struct ChainDoc {
    // Only set for the settings UI's custom chain: the preset it came from.
    std::string base;
    // Plugins the user switched off; kept so the UI can list them dimmed.
    std::vector<std::string> disabled;
    std::vector<ChainPlugin> plugins;
};

bool ReadChainFile(const std::wstring& path, ChainDoc* out, std::string* error);
bool WriteChainFile(const std::wstring& path, const ChainDoc& doc,
                    std::string* error);

// One settings entry from a plugin's manifest.yaml.
struct PluginSettingSpec {
    std::string key;
    std::string type;  // bool / int / enum / string / secret
    ChainValue def;
    bool has_default = false;
    int64_t min = 0;
    int64_t max = 0;
    std::vector<std::string> values;
};

struct PluginMeta {
    std::string id;
    std::string version;
    std::string role;
    std::string description;
    std::vector<PluginSettingSpec> settings;
};

// Scans <plugins_dir>/*/manifest.yaml. A package that fails to parse is
// skipped: the settings UI can still show the plugin, just without controls.
std::vector<PluginMeta> LoadPluginCatalog(const std::wstring& plugins_dir);

}  // namespace pimewin
