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
    // Direct predecessor recorded by the scheme chain; only meaningful in a
    // host-owned chain file. `after_set` with an empty `after` means the entry
    // must run first; unset means the manager falls back to the last entry with
    // the same role.
    bool after_set = false;
    std::string after;
    std::vector<ChainSetting> settings;
};

struct ChainDoc {
    // Only set for the settings UI's custom chain: the preset it came from.
    std::string base;
    // Plugins the user switched off. Kept as full records so a disabled plugin
    // still keeps its slot and anchors for later insertions.
    std::vector<ChainPlugin> disabled;
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
    // Default predecessor for importing this plugin into a scheme. Empty with
    // `after_set` means "must be first"; unset means fall back to the last
    // entry with the same role.
    bool after_set = false;
    std::string after;
    std::vector<PluginSettingSpec> settings;
};

// Reads one plugin package's manifest.yaml and returns its metadata. Fails
// with a message when the file cannot be read or lacks id/role.
bool ReadPluginManifest(const std::wstring& path, PluginMeta* out,
                        std::string* error);

// Scans <plugins_dir>/*/manifest.yaml. A package that fails to parse is
// skipped: the settings UI can still show the plugin, just without controls.
std::vector<PluginMeta> LoadPluginCatalog(const std::wstring& plugins_dir);

// ---- scheme packages -------------------------------------------------------
//
// A scheme package is a self-contained directory:
//
//   <root>/schemes/<id>/
//     scheme.yaml        metadata + the plugin manifest
//     <chain>.cbor       the chain this scheme activates
//     plugins/<pkg>/     copies of every plugin the chain references
//
// Bundled packages live next to the executable; user-installed packages and
// edited chains live in %LOCALAPPDATA%\PIME\schemes. A user package with the
// same id shadows the bundled one.
struct SchemeInfo {
    std::string id;
    std::string name;
    std::string description;
    std::string version;
    std::string chain;   // chain file name inside the package
    std::wstring dir;    // package directory
    bool user = false;   // loaded from the writable data directory
};

bool ReadSchemeFile(const std::wstring& path, SchemeInfo* out,
                    std::string* error);

// Bundled packages first, then user packages (which replace same-id entries).
std::vector<SchemeInfo> ListSchemePackages(const std::wstring& resources_dir,
                                           const std::wstring& data_dir);

// Chain to activate for a scheme id. A user edit (schemes/<id>/custom.cbor)
// wins over the package's own chain; an empty result means "not installed".
std::wstring SchemeChainPath(const std::wstring& resources_dir,
                             const std::wstring& data_dir,
                             const std::string& id);

// The package's own chain, ignoring a user edit. Used by the editor to merge
// new plugins from an upgraded package into the user's saved pipeline.
std::wstring SchemeBaseChainPath(const std::wstring& resources_dir,
                                 const std::wstring& data_dir,
                                 const std::string& id);

// Plugin package directories a scheme carries, user copy first. Falls back to
// the legacy shared plugins/ directory when no scheme package is installed.
std::vector<std::wstring> SchemePluginDirs(const std::wstring& resources_dir,
                                           const std::wstring& data_dir,
                                           const std::string& id);

// Union of every installed scheme's plugin manifests, newest version first.
std::vector<PluginMeta> LoadPluginCatalogFromSchemes(
    const std::wstring& resources_dir, const std::wstring& data_dir);

}  // namespace pimewin
