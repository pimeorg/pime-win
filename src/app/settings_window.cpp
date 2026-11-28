#include "app/settings_window.h"

#include <commdlg.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <pime/core.h>

#include "app/resource.h"
#include "app/rime_dict.h"
#include "common/chain_file.h"
#include "common/log.h"
#include "common/settings.h"
#include "common/user_dict_format.h"
#include "common/win_util.h"
#include "tsf/guids.h"
#include "tsf/ipc_client.h"
#include "tsf/register.h"

using Microsoft::WRL::ComPtr;

namespace pimewin {
namespace {

constexpr wchar_t kClassName[] = L"PimeSettingsWindow";
constexpr wchar_t kWindowTitle[] = L"PIME 拼音输入法";
constexpr wchar_t kVersion[] = L"1.0.0";

// Logical (96 dpi) metrics. The render target is given the window DPI, so
// every coordinate below is a DIP and needs no manual scaling.
constexpr float kWidth = 900.f;
constexpr float kHeight = 640.f;
constexpr float kSidebar = 224.f;
constexpr float kMargin = 32.f;
constexpr float kDictHeader = 36.f;
constexpr float kDictRowHeight = 40.f;

enum class Page { General, Appearance, Plugins, Dictionary, About };
enum class Menu { None, Chain, PageSize };

enum : int {
    kNone = -1,
    kNavGeneral = 1,
    kNavAppearance = 2,
    kNavPlugins = 3,
    kNavDictionary = 4,
    kNavAbout = 5,
    kChain = 10,
    kPageSize = 11,
    kPunctFull = 20,
    kPunctHalf = 21,
    kThemeSystem = 30,
    kThemeLight = 31,
    kThemeDark = 32,
    kOrientH = 33,
    kOrientV = 34,
    kSizeSmall = 35,
    kSizeMid = 36,
    kSizeLarge = 37,
    kToggle = 40,
    kApply = 50,
    kClose = 51,
    kInstall = 52,
    kUninstall = 53,
    kActivate = 54,
    kDictAdd = 60,
    kDictEdit = 61,
    kDictDelete = 62,
    kDictImport = 63,
    kDictExport = 64,
    kDictClear = 65,
    kDictRefresh = 66,
    kPreset = 70,      // kPreset + preset index
    kRestore = 79,
    kImportScheme = 80,
    kMenuItem = 100,
    kDictRow = 200,
    kPlugCard = 3000,   // kPlugCard + plugin index
    kPlugToggle = 3500, // kPlugToggle + plugin index
    kPlugSetting = 4000,  // kPlugSetting + plugin * 32 + spec index
};

struct Box {
    float l = 0, t = 0, r = 0, b = 0;
    bool Contains(float x, float y) const {
        return x >= l && x < r && y >= t && y < b;
    }
    float W() const { return r - l; }
    float H() const { return b - t; }
};

// A control inside an expanded plugin card. `spec` indexes the manifest's
// settings list, so the value is always interpreted with its declared type.
struct PlugControl {
    enum class Kind { Bool, Int, Enum };
    Box box;
    int spec = 0;
    Kind kind = Kind::Bool;
    int segments = 0;
};

struct PlugRow {
    int plugin = 0;
    Box card;
    Box toggle;
    bool expanded = false;
    std::vector<PlugControl> controls;
};

struct Layout {
    Box nav[5];
    Box chain;
    Box page_size;
    Box punct[2];
    Box theme[3];
    Box orient;
    Box size;
    Box toggle;
    Box apply;
    Box close;
    Box install;
    Box uninstall;
    Box activate;
    Box dict_tool[7];
    Box dict_list;
    Box preset[8];
    int preset_count = 0;
    Box restore;
    Box import_scheme;
    Box plug_view;
    std::vector<PlugRow> plug_rows;
    float plug_content = 0;
    Box menu;
    int menu_items = 0;
};

// One row of the 词库 page: display text plus the exact store values.
struct DictRow {
    std::wstring word;
    std::wstring code;
    uint32_t weight = 0;
    std::string code_raw;
    std::string word_raw;
};

// One plugin in the chain editor. Disabled plugins stay in the list so the
// user can switch them back on; only enabled ones are written to the chain.
struct EditPlugin {
    ChainPlugin plugin;
    int meta = -1;  // index into AppState::catalog, -1 when unknown
    bool enabled = true;
    bool structural = false;  // removing it would break the pipeline
};

struct Palette {
    D2D1_COLOR_F bg;
    D2D1_COLOR_F sidebar;
    D2D1_COLOR_F card;
    D2D1_COLOR_F border;
    D2D1_COLOR_F text;
    D2D1_COLOR_F secondary;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F accent_soft;
    D2D1_COLOR_F track;
    D2D1_COLOR_F hover;
    D2D1_COLOR_F danger;
    D2D1_COLOR_F success;
    D2D1_COLOR_F success_soft;
};

struct Tween {
    float value = 0;
    float from = 0;
    float to = 0;
    ULONGLONG start = 0;
    bool running = false;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    UINT dpi = 96;
    float width = kWidth;
    float height = kHeight;

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<IDWriteTextFormat> f_logo;
    ComPtr<IDWriteTextFormat> f_app;
    ComPtr<IDWriteTextFormat> f_page;
    ComPtr<IDWriteTextFormat> f_section;
    ComPtr<IDWriteTextFormat> f_label;
    ComPtr<IDWriteTextFormat> f_desc;
    ComPtr<IDWriteTextFormat> f_value;
    ComPtr<IDWriteTextFormat> f_small;
    ComPtr<IDWriteTextFormat> f_button;
    ComPtr<IDWriteTextFormat> f_badge;

    Page page = Page::General;
    Menu menu = Menu::None;
    int focus = kNone;
    bool focus_visible = false;
    float mx = -1000, my = -1000;
    bool mouse_in = false;
    bool system_dark = false;
    bool installed = false;

    Settings settings;
    Settings saved;
    std::vector<SchemeInfo> schemes;

    // Scheme editor. `edit_plugins` is the working copy; `plugin_chain` is the
    // scheme id it currently represents and `plugin_choice` the chip that is
    // highlighted. `plugin_custom` means a user edit shadows the package chain.
    std::vector<EditPlugin> edit_plugins;
    std::vector<PluginMeta> catalog;
    bool catalog_loaded = false;
    bool plugins_loaded = false;
    std::string plugin_chain;
    std::string plugin_choice;
    bool plugin_custom = false;
    bool plugin_dirty = false;
    int plugin_selected = -1;
    float plugin_scroll = 0;
    int plugin_drag = kNone;
    std::wstring plugin_status;
    bool plugin_status_ok = true;

    std::wstring status;
    bool status_ok = true;

    std::vector<DictRow> dict_rows;
    int dict_selected = -1;
    int dict_scroll = 0;
    bool dict_loaded = false;
    std::wstring dict_status;
    bool dict_status_ok = true;

    Tween toggle;

    Layout layout;
};

D2D1_COLOR_F Rgb(int r, int g, int b, float a = 1.f) {
    return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a);
}

D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

D2D1_COLOR_F Brighter(const D2D1_COLOR_F& c, float amount) {
    return D2D1::ColorF(c.r + (1.f - c.r) * amount,
                        c.g + (1.f - c.g) * amount,
                        c.b + (1.f - c.b) * amount, c.a);
}

Palette Pal(bool dark) {
    Palette p{};
    if (dark) {
        p.bg = Rgb(23, 24, 27);
        p.sidebar = Rgb(30, 32, 36);
        p.card = Rgb(36, 38, 43);
        p.border = Rgb(53, 56, 62);
        p.text = Rgb(241, 242, 244);
        p.secondary = Rgb(150, 156, 170);
        p.accent = Rgb(91, 140, 255);
        p.accent_soft = Rgb(38, 49, 76);
        p.track = Rgb(59, 63, 70);
        p.hover = Rgb(43, 46, 52);
        p.danger = Rgb(255, 107, 107);
        p.success = Rgb(76, 199, 118);
        p.success_soft = Rgb(32, 51, 41);
    } else {
        p.bg = Rgb(244, 246, 249);
        p.sidebar = Rgb(238, 241, 246);
        p.card = Rgb(255, 255, 255);
        p.border = Rgb(224, 228, 234);
        p.text = Rgb(27, 28, 32);
        p.secondary = Rgb(109, 115, 124);
        p.accent = Rgb(47, 107, 255);
        p.accent_soft = Rgb(232, 239, 255);
        p.track = Rgb(216, 220, 227);
        p.hover = Rgb(241, 243, 247);
        p.danger = Rgb(217, 58, 58);
        p.success = Rgb(47, 160, 79);
        p.success_soft = Rgb(232, 246, 237);
    }
    return p;
}

// ---- drawing helpers -------------------------------------------------------

void FillBox(AppState& s, Box b, float radius, const D2D1_COLOR_F& color) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(s.rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    s.rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(b.l, b.t, b.r, b.b), radius, radius),
        brush.Get());
}

void StrokeBox(AppState& s, Box b, float radius, const D2D1_COLOR_F& color,
               float width) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(s.rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    s.rt->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(b.l, b.t, b.r, b.b), radius, radius),
        brush.Get(), width);
}

void FillPlain(AppState& s, Box b, const D2D1_COLOR_F& color) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(s.rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    s.rt->FillRectangle(D2D1::RectF(b.l, b.t, b.r, b.b), brush.Get());
}

void FillEllipse(AppState& s, float cx, float cy, float radius,
                 const D2D1_COLOR_F& color) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(s.rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    s.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius),
                      brush.Get());
}

void DrawLine(AppState& s, float x0, float y0, float x1, float y1,
              const D2D1_COLOR_F& color, float width) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(s.rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    s.rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush.Get(),
                   width);
}

void DrawText(AppState& s, IDWriteTextFormat* format, const std::wstring& text,
              Box box, const D2D1_COLOR_F& color,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    if (text.empty() || box.W() <= 0) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(s.dwrite->CreateTextLayout(text.c_str(),
                                          static_cast<UINT32>(text.size()),
                                          format, box.W(), box.H(),
                                          layout.GetAddressOf()))) {
        return;
    }
    layout->SetTextAlignment(align);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(s.rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    s.rt->DrawTextLayout(D2D1::Point2F(box.l, box.t), layout.Get(), brush.Get());
}

// Same as DrawText, but a line that does not fit ends in an ellipsis. Used
// for the dictionary rows, where the text comes from the user.
void DrawTextEllipsis(AppState& s, IDWriteTextFormat* format,
                      const std::wstring& text, Box box,
                      const D2D1_COLOR_F& color,
                      DWRITE_TEXT_ALIGNMENT align =
                          DWRITE_TEXT_ALIGNMENT_LEADING) {
    if (text.empty() || box.W() <= 0) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(s.dwrite->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), format, box.W(),
            box.H(), layout.GetAddressOf()))) {
        return;
    }
    DWRITE_TRIMMING trimming{};
    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    ComPtr<IDWriteInlineObject> sign;
    if (SUCCEEDED(s.dwrite->CreateEllipsisTrimmingSign(
            layout.Get(), sign.GetAddressOf()))) {
        layout->SetTrimming(&trimming, sign.Get());
    }
    layout->SetTextAlignment(align);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(s.rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    s.rt->DrawTextLayout(D2D1::Point2F(box.l, box.t), layout.Get(), brush.Get());
}

float MeasureText(AppState& s, IDWriteTextFormat* format,
                  const std::wstring& text) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(s.dwrite->CreateTextLayout(text.c_str(),
                                          static_cast<UINT32>(text.size()),
                                          format, 4096.f, 128.f,
                                          layout.GetAddressOf()))) {
        return 0;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return 0;
    return metrics.width;
}

void DrawChevron(AppState& s, float cx, float cy, const D2D1_COLOR_F& color) {
    DrawLine(s, cx - 4, cy - 2, cx, cy + 2, color, 1.6f);
    DrawLine(s, cx, cy + 2, cx + 4, cy - 2, color, 1.6f);
}

void FocusRing(AppState& s, Box b, int id, float radius) {
    if (s.focus != id || !s.focus_visible) return;
    StrokeBox(s, Box{b.l - 2, b.t - 2, b.r + 2, b.b + 2}, radius + 2,
              Pal(s.system_dark).accent, 1.5f);
}

// ---- state helpers ---------------------------------------------------------

bool IsInstalled() {
    wchar_t clsid[64] = {};
    if (StringFromGUID2(kClsidTextService, clsid, 64) <= 0) return false;
    const std::wstring key = L"CLSID\\" + std::wstring(clsid) + L"\\InprocServer32";
    wchar_t path[MAX_PATH * 2] = {};
    DWORD size = sizeof(path);
    return RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ,
                        nullptr, path, &size) == ERROR_SUCCESS;
}

std::wstring ChainName(const std::wstring& file) {
    std::wstring name = file;
    if (name.size() > 5 && name.substr(name.size() - 5) == L".cbor") {
        name.resize(name.size() - 5);
    }
    return name;
}

// Bundled scheme packages plus the user's installs in
// %LOCALAPPDATA%\PIME\schemes. The scheme id is the stable identity.
std::vector<SchemeInfo> ListSchemes() {
    return ListSchemePackages(ModuleDirectory(nullptr), DataDirectory());
}

const SchemeInfo* FindScheme(const AppState& s, const std::wstring& id) {
    for (const SchemeInfo& scheme : s.schemes) {
        if (ToWide(scheme.id) == id) return &scheme;
    }
    return nullptr;
}

// What the dropdown shows; the id is the identity, this is only the label.
std::wstring SchemeName(const AppState& s, const std::wstring& id) {
    const SchemeInfo* scheme = FindScheme(s, id);
    if (!scheme) return id;
    if (!scheme->name.empty()) return ToWide(scheme->name);
    if (!scheme->description.empty()) return ToWide(scheme->description);
    return id;
}

std::wstring SchemeName(const AppState& s, const std::string& id) {
    return SchemeName(s, ToWide(id));
}

// A user edit of a scheme's pipeline lives next to the package in the data
// directory and shadows the bundled chain.
std::wstring SchemeOverlayPath(const std::wstring& id) {
    return PathJoin(PathJoin(PathJoin(DataDirectory(), L"schemes"), id),
                    L"custom.cbor");
}

bool HasSchemeOverlay(const std::wstring& id) {
    return FileExists(SchemeOverlayPath(id));
}

// Human labels for the pipeline roles, in the order a chain runs them.
const wchar_t* RoleLabel(const std::string& role) {
    if (role == "preprocess") return L"预处理";
    if (role == "parse") return L"解析";
    if (role == "translate") return L"翻译";
    if (role == "rank") return L"排序";
    if (role == "format") return L"格式化";
    if (role == "predict") return L"联想";
    if (role == "learn") return L"学习";
    if (role == "recognize") return L"识别";
    if (role == "service") return L"服务";
    return L"其他";
}

int RoleOrder(const std::string& role) {
    static const char* const kRoles[] = {
        "preprocess", "parse", "translate", "rank",
        "format",     "predict", "learn",  "recognize",
        "service",
    };
    for (int i = 0; i < static_cast<int>(std::size(kRoles)); ++i) {
        if (role == kRoles[i]) return i;
    }
    return static_cast<int>(std::size(kRoles));
}

D2D1_COLOR_F RoleColor(const std::string& role) {
    if (role == "preprocess") return Rgb(96, 125, 139);
    if (role == "parse") return Rgb(0, 150, 136);
    if (role == "translate") return Rgb(92, 107, 230);
    if (role == "rank") return Rgb(230, 145, 20);
    if (role == "format") return Rgb(60, 165, 95);
    if (role == "predict") return Rgb(150, 90, 200);
    if (role == "learn") return Rgb(220, 100, 150);
    if (role == "recognize") return Rgb(220, 80, 80);
    return Rgb(130, 136, 148);
}

// Display names for the bundled plugins. Unknown ids fall back to the id
// with the "pime-" prefix removed, which still reads fine.
std::wstring PluginName(const std::string& id) {
    static const struct {
        const char* id;
        const wchar_t* name;
    } kNames[] = {
        {"pime-keymap-default", L"默认键位"},
        {"pime-pinyin-parse", L"拼音解析"},
        {"pime-shape-parse", L"形码解析"},
        {"pime-pinyin-translate", L"拼音候选"},
        {"pime-special-candidates", L"特殊候选"},
        {"pime-shortcuts", L"快捷短语"},
        {"pime-hans-hant", L"简繁转换"},
        {"pime-shape-translate", L"字形候选"},
        {"pime-english-translate", L"英文候选"},
        {"pime-emoji-suggest", L"Emoji 联想"},
        {"pime-clip-code", L"剪贴板编码"},
        {"pime-candidates-format", L"候选排版"},
        {"pime-learn-user-dict", L"学习用户词"},
        {"pime-predict-context", L"上下文联想"},
        {"pime-app-mode", L"应用模式"},
        {"pime-dict-packs", L"词库包"},
        {"pime-personal-merge", L"个人词合并"},
        {"pime-model-manager", L"模型管理"},
        {"pime-shuangpin-standard", L"双拼方案"},
        {"pime-wubi-parse", L"五笔解析"},
        {"pime-wubi-translate", L"五笔候选"},
        {"pime-cangjie-parse", L"仓颉解析"},
        {"pime-cangjie-translate", L"仓颉候选"},
        {"pime-rank-default", L"默认排序"},
        {"pime-pinyin-rank", L"拼音排序"},
        {"pime-english-parse", L"英文解析"},
        {"pime-speech-sherpa", L"语音识别"},
    };
    for (const auto& entry : kNames) {
        if (id == entry.id) return entry.name;
    }
    std::string fallback = id;
    if (fallback.rfind("pime-", 0) == 0) fallback = fallback.substr(5);
    return ToWide(fallback);
}

// The catalog is sorted newest first per id, so the first match wins when the
// chain does not pin a version.
// Friendly labels for the setting keys that appear in the bundled chains.
// Unknown keys fall back to the raw key, which is still readable.
std::wstring SettingLabel(const std::string& key) {
    static const struct {
        const char* key;
        const wchar_t* label;
    } kLabels[] = {
        {"enabled", L"启用"},
        {"page_size", L"每页候选数"},
        {"max_candidates", L"候选上限"},
        {"completion", L"英文补全"},
        {"completion_limit", L"补全数量"},
        {"corrections", L"拼写纠正"},
        {"fuzzy", L"模糊音"},
        {"max_syllables", L"最大音节"},
        {"auto_correct", L"自动纠错"},
        {"tap_correct", L"轻点纠错"},
        {"english_passthrough", L"英文直通"},
        {"mixed_uppercase", L"混合大小写"},
        {"collocation_weight", L"搭配权重"},
        {"short_boost", L"短词加权"},
        {"cohesion_boost", L"词组凝聚"},
        {"learn_boost", L"学习加权"},
        {"recent_boost", L"最近使用"},
        {"profile_boost", L"场景加权"},
        {"user_boost", L"用户词加权"},
        {"hotword_boost", L"热词加权"},
        {"decay_days", L"记忆衰减（天）"},
        {"profile", L"使用场景"},
        {"space_selects_candidate", L"空格选词"},
        {"candidate_keys", L"候选快捷键"},
        {"number_selects_candidate", L"数字选词"},
        {"remember", L"记住应用状态"},
        {"observe", L"观察应用"},
        {"default_mode", L"默认模式"},
        {"min_len", L"最短长度"},
        {"max_len", L"最长长度"},
        {"v_mode", L"v 模式"},
        {"date", L"日期候选"},
        {"show_labels", L"显示序号"},
        {"show_comments", L"显示注释"},
        {"commit_on_enter", L"回车上屏"},
        {"commit_raw", L"上屏原码"},
        {"undo_window_ms", L"撤销窗口（毫秒）"},
        {"max_recent", L"最近词数量"},
        {"user_bigram", L"用户二元组"},
        {"continuation", L"续写联想"},
        {"collocation", L"搭配联想"},
        {"fallback", L"兜底联想"},
        {"show_command", L"命令提示"},
        {"max_code", L"最长编码"},
        {"scheme", L"双拼方案"},
        {"min_score", L"最低分数"},
        {"comment", L"注释"},
        {"max_entries", L"词条上限"},
        {"threads", L"线程数"},
        {"sample_rate", L"采样率"},
        {"model", L"模型"},
        {"endpoint", L"接口地址"},
        {"api_key", L"API Key"},
        {"secret_id", L"Secret ID"},
        {"secret_key", L"Secret Key"},
        {"app_id", L"App ID"},
        {"app_key", L"App Key"},
        {"access_key", L"Access Key"},
        {"resource_id", L"资源 ID"},
        {"hotword_list", L"热词表"},
        {"engine_model_type", L"引擎模型"},
        {"runtime_dir", L"运行目录"},
        {"model_dir", L"模型目录"},
        {"wav_file", L"WAV 文件"},
        {"custom_model", L"自定义模型"},
        {"disabled", L"停用列表"},
    };
    for (const auto& entry : kLabels) {
        if (key == entry.key) return entry.label;
    }
    return ToWide(key);
}

const PluginMeta* FindMeta(const AppState& s, const std::string& id,
                           const std::string& version) {
    for (const PluginMeta& meta : s.catalog) {
        if (meta.id != id) continue;
        if (version.empty() || meta.version == version) return &meta;
    }
    return nullptr;
}

std::vector<std::wstring> PresetList(const AppState& s) {
    std::vector<std::wstring> presets;
    for (const SchemeInfo& scheme : s.schemes) {
        presets.push_back(ToWide(scheme.id));
    }
    return presets;
}

int ChainIndex(const AppState& s) {
    const std::wstring current = ChainName(s.settings.chain);
    for (size_t i = 0; i < s.schemes.size(); ++i) {
        if (ToWide(s.schemes[i].id) == current) return static_cast<int>(i);
    }
    return 0;
}

int PageIndex(const AppState& s) {
    const int index = static_cast<int>(s.settings.page_size) - 3;
    return index < 0 ? 0 : (index > 6 ? 6 : index);
}

bool SameSettings(const Settings& a, const Settings& b) {
    return a.chain == b.chain && a.theme == b.theme &&
           a.page_size == b.page_size && a.chinese == b.chinese &&
           a.punctuation == b.punctuation &&
           a.candidate_vertical == b.candidate_vertical &&
           a.candidate_font == b.candidate_font;
}

bool Dirty(const AppState& s) {
    return !SameSettings(s.settings, s.saved) || s.plugin_dirty;
}

bool Hover(const AppState& s, Box b) {
    return s.mouse_in && s.menu == Menu::None && b.Contains(s.mx, s.my);
}

bool CandidateDark(const AppState& s) {
    if (s.settings.theme == 2) return true;
    if (s.settings.theme == 1) return false;
    return s.system_dark;
}

// ---- text formats ----------------------------------------------------------

void MakeFormat(AppState& s, float size, DWRITE_FONT_WEIGHT weight,
                ComPtr<IDWriteTextFormat>* out) {
    if (FAILED(s.dwrite->CreateTextFormat(
            L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN",
            out->GetAddressOf()))) {
        return;
    }
    (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

bool CreateFormats(AppState& s) {
    if (!s.dwrite) return false;
    MakeFormat(s, 20.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &s.f_logo);
    MakeFormat(s, 16.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &s.f_app);
    MakeFormat(s, 19.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &s.f_page);
    MakeFormat(s, 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &s.f_section);
    MakeFormat(s, 13.f, DWRITE_FONT_WEIGHT_NORMAL, &s.f_label);
    MakeFormat(s, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, &s.f_desc);
    MakeFormat(s, 13.f, DWRITE_FONT_WEIGHT_NORMAL, &s.f_value);
    MakeFormat(s, 11.f, DWRITE_FONT_WEIGHT_NORMAL, &s.f_small);
    MakeFormat(s, 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &s.f_button);
    MakeFormat(s, 12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &s.f_badge);
    return s.f_page && s.f_button;
}

// ---- layout ----------------------------------------------------------------

Box ItemBox(const AppState& s, int index) {
    const Layout& l = s.layout;
    return Box{l.menu.l + 4, l.menu.t + 6 + index * 34.f,
               l.menu.r - 4, l.menu.t + 6 + (index + 1) * 34.f};
}

void BuildLayout(AppState& s) {
    Layout l{};
    const float w = s.width;
    const float h = s.height;
    const float x0 = kSidebar + kMargin;
    const float x1 = w - kMargin;

    for (int i = 0; i < 5; ++i) {
        l.nav[i] = Box{16.f, 116.f + i * 52.f, kSidebar - 16.f,
                       156.f + i * 52.f};
    }

    l.close = Box{x1 - 130.f - 12.f - 92.f, h - 52.f, x1 - 130.f - 12.f,
                  h - 18.f};
    l.apply = Box{x1 - 130.f, h - 52.f, x1, h - 18.f};

    if (s.page == Page::General) {
        const float card_top = 96.f;
        const float card_bottom = 298.f;
        const float row_left = x0 + 20.f;
        const float row_right = x1 - 20.f;
        l.chain = Box{row_right - 220.f, card_top + 25.f, row_right,
                      card_top + 61.f};
        l.page_size = Box{row_right - 130.f, card_top + 83.f, row_right,
                          card_top + 119.f};
        const float seg_right = row_right;
        const float seg_top = card_top + 141.f;
        l.punct[0] = Box{seg_right - 300.f, seg_top, seg_right - 150.f,
                         seg_top + 36.f};
        l.punct[1] = Box{seg_right - 150.f, seg_top, seg_right, seg_top + 36.f};
        l.toggle = Box{row_right - 44.f, 314.f + 22.f, row_right, 314.f + 46.f};
        (void)card_bottom;
        (void)row_left;
    } else if (s.page == Page::Appearance) {
        const float card_top = 96.f;
        const float left = x0 + 20.f;
        const float right = x1 - 20.f;
        const float gap = 12.f;
        const float tile_w = (right - left - gap * 2.f) / 3.f;
        for (int i = 0; i < 3; ++i) {
            l.theme[i] = Box{left + i * (tile_w + gap), card_top + 44.f,
                             left + i * (tile_w + gap) + tile_w,
                             card_top + 148.f};
        }
        const float card2_top = 280.f;
        l.orient = Box{right - 220.f, card2_top + 122.f, right,
                       card2_top + 158.f};
        l.size = Box{right - 220.f, card2_top + 178.f, right,
                     card2_top + 214.f};
    } else if (s.page == Page::Plugins) {
        const float left = x0;
        const float right = x1;
        const std::vector<std::wstring> presets = PresetList(s);
        l.preset_count = std::min(static_cast<int>(presets.size()), 8);
        const float gap = 8.f;
        const float chip_w = (right - left - gap * 3.f) / 4.f;
        for (int i = 0; i < l.preset_count; ++i) {
            const float chip_left = left + (i % 4) * (chip_w + gap);
            const float chip_top = 134.f + (i / 4) * 36.f;
            l.preset[i] = Box{chip_left, chip_top, chip_left + chip_w,
                              chip_top + 30.f};
        }
        l.import_scheme = Box{right - 96.f, 102.f, right - 16.f, 128.f};
        l.restore = Box{right - 188.f, 102.f, right - 108.f, 128.f};

        l.plug_view = Box{left, 256.f, right, h - 84.f};
        l.plug_rows.clear();
        float y = l.plug_view.t - s.plugin_scroll;
        std::string last_role;
        for (int i = 0; i < static_cast<int>(s.edit_plugins.size()); ++i) {
            const EditPlugin& entry = s.edit_plugins[i];
            const PluginMeta* meta =
                entry.meta >= 0 ? &s.catalog[entry.meta] : nullptr;
            const std::string role =
                meta && !meta->role.empty() ? meta->role : entry.plugin.role;
            if (i == 0 || role != last_role) {
                y += 24.f;
                last_role = role;
            }
            const bool expanded = i == s.plugin_selected;
            const bool has_desc = meta && !meta->description.empty();
            const int specs =
                meta ? static_cast<int>(meta->settings.size()) : 0;
            float height = 40.f;
            if (expanded) {
                height += 8.f;
                if (has_desc) height += 22.f;
                height += specs * 30.f;
                height += 6.f;
            }

            PlugRow row;
            row.plugin = i;
            row.expanded = expanded;
            row.card = Box{left + 28.f, y, right, y + height};
            row.toggle = Box{right - 56.f, y + 10.f, right - 20.f, y + 30.f};
            if (expanded && meta) {
                float control_top = y + 48.f + (has_desc ? 22.f : 0.f);
                for (int j = 0; j < specs; ++j) {
                    const PluginSettingSpec& spec = meta->settings[j];
                    PlugControl control;
                    control.spec = j;
                    if (spec.type == "bool") {
                        control.kind = PlugControl::Kind::Bool;
                        control.box = Box{right - 56.f, control_top + 5.f,
                                          right - 20.f, control_top + 25.f};
                    } else if (spec.type == "int") {
                        control.kind = PlugControl::Kind::Int;
                        control.box = Box{right - 270.f, control_top,
                                          right - 64.f, control_top + 30.f};
                    } else if (spec.type == "enum") {
                        control.kind = PlugControl::Kind::Enum;
                        control.segments =
                            static_cast<int>(spec.values.size());
                        control.box = Box{right - 300.f, control_top + 2.f,
                                          right - 16.f, control_top + 28.f};
                    } else {
                        control_top += 30.f;
                        continue;  // strings have no editor
                    }
                    row.controls.push_back(control);
                    control_top += 30.f;
                }
            }
            l.plug_rows.push_back(std::move(row));
            y += height + 6.f;
        }
        l.plug_content = y - (l.plug_view.t - s.plugin_scroll);
        const float max_scroll = std::max(0.f, l.plug_content - l.plug_view.H());
        if (s.plugin_scroll > max_scroll) {
            s.plugin_scroll = max_scroll;
            BuildLayout(s);  // re-run once with the clamped offset
            return;
        }
    } else if (s.page == Page::Dictionary) {
        constexpr int kTools = 7;
        constexpr float kGap = 8.f;
        const float width = (x1 - x0 - kGap * (kTools - 1)) / kTools;
        for (int i = 0; i < kTools; ++i) {
            const float left = x0 + i * (width + kGap);
            l.dict_tool[i] = Box{left, 96.f, left + width, 132.f};
        }
        l.dict_list = Box{x0, 144.f, x1, h - 104.f};
    } else {
        const float card_top = 338.f;
        l.install = Box{x0 + 20.f, card_top + 76.f, x0 + 168.f,
                        card_top + 112.f};
        l.uninstall = Box{x0 + 178.f, card_top + 76.f, x0 + 266.f,
                          card_top + 112.f};
        l.activate = Box{x0 + 276.f, card_top + 76.f, x0 + 412.f,
                         card_top + 112.f};
    }

    if (s.menu == Menu::Chain || s.menu == Menu::PageSize) {
        const Box& anchor =
            s.menu == Menu::Chain ? l.chain : l.page_size;
        const int count = s.menu == Menu::Chain
                              ? static_cast<int>(s.schemes.size())
                              : 7;
        const float height = 12.f + count * 34.f;
        Box menu{anchor.l, anchor.b + 4.f, anchor.l + anchor.W(),
                 anchor.b + 4.f + height};
        if (menu.b > h - 80.f) {
            menu = Box{anchor.l, anchor.t - 4.f - height,
                       anchor.l + anchor.W(), anchor.t - 4.f};
        }
        l.menu = menu;
        l.menu_items = count;
    }

    s.layout = l;
}

// ---- hit testing -----------------------------------------------------------

float DictRowsTop(const AppState& s) {
    return s.layout.dict_list.t + kDictHeader;
}

int DictVisibleRows(const AppState& s) {
    const float height = s.layout.dict_list.b - DictRowsTop(s);
    return height <= 0 ? 0 : static_cast<int>(height / kDictRowHeight);
}

int HitTest(const AppState& s, float x, float y) {
    if (s.menu != Menu::None) {
        for (int i = 0; i < s.layout.menu_items; ++i) {
            if (ItemBox(s, i).Contains(x, y)) return kMenuItem + i;
        }
        return kNone;
    }
    for (int i = 0; i < 5; ++i) {
        if (s.layout.nav[i].Contains(x, y)) return kNavGeneral + i;
    }
    if (s.page == Page::General) {
        if (s.layout.chain.Contains(x, y)) return kChain;
        if (s.layout.page_size.Contains(x, y)) return kPageSize;
        if (s.layout.punct[0].Contains(x, y)) return kPunctFull;
        if (s.layout.punct[1].Contains(x, y)) return kPunctHalf;
        if (s.layout.toggle.Contains(x, y)) return kToggle;
    } else if (s.page == Page::Appearance) {
        for (int i = 0; i < 3; ++i) {
            if (s.layout.theme[i].Contains(x, y)) return kThemeSystem + i;
        }
        if (s.layout.orient.Contains(x, y)) {
            const float seg = (x - s.layout.orient.l) / (s.layout.orient.W() / 2.f);
            return kOrientH + (seg >= 1.f ? 1 : 0);
        }
        if (s.layout.size.Contains(x, y)) {
            const float seg = (x - s.layout.size.l) / (s.layout.size.W() / 3.f);
            return kSizeSmall + std::clamp(static_cast<int>(seg), 0, 2);
        }
    } else if (s.page == Page::Plugins) {
        for (int i = 0; i < s.layout.preset_count; ++i) {
            if (s.layout.preset[i].Contains(x, y)) return kPreset + i;
        }
        if ((s.plugin_dirty || s.plugin_custom) &&
            s.layout.restore.Contains(x, y)) {
            return kRestore;
        }
        if (s.layout.import_scheme.Contains(x, y)) return kImportScheme;
        if (s.layout.plug_view.Contains(x, y)) {
            for (const PlugRow& row : s.layout.plug_rows) {
                if (!row.card.Contains(x, y)) continue;
                if (!s.edit_plugins[row.plugin].structural &&
                    row.toggle.Contains(x, y)) {
                    return kPlugToggle + row.plugin;
                }
                for (const PlugControl& control : row.controls) {
                    if (control.box.Contains(x, y)) {
                        return kPlugSetting + row.plugin * 32 + control.spec;
                    }
                }
                return kPlugCard + row.plugin;
            }
        }
    } else if (s.page == Page::Dictionary) {
        for (int i = 0; i < 7; ++i) {
            if (s.layout.dict_tool[i].Contains(x, y)) return kDictAdd + i;
        }
        const float top = DictRowsTop(s);
        if (y >= top && y < s.layout.dict_list.b) {
            const int row =
                s.dict_scroll + static_cast<int>((y - top) / kDictRowHeight);
            if (row >= 0 && row < static_cast<int>(s.dict_rows.size())) {
                return kDictRow + row;
            }
        }
    } else {
        if (s.layout.install.Contains(x, y)) return kInstall;
        if (s.layout.uninstall.Contains(x, y)) return kUninstall;
        if (s.layout.activate.Contains(x, y)) return kActivate;
    }
    if (s.layout.apply.Contains(x, y)) return kApply;
    if (s.layout.close.Contains(x, y)) return kClose;
    return kNone;
}

// ---- drawing ---------------------------------------------------------------

void DrawSelect(AppState& s, const Box& b, const std::wstring& value, int id,
                bool open) {
    const Palette p = Pal(s.system_dark);
    const bool hot = Hover(s, b);
    FillBox(s, b, 8.f, p.card);
    StrokeBox(s, b, 8.f, (hot || open) ? p.accent : p.border, 1.f);
    DrawText(s, s.f_value.Get(), value,
             Box{b.l + 14.f, b.t, b.r - 32.f, b.b}, p.text);
    DrawChevron(s, b.r - 18.f, b.t + b.H() / 2.f, p.secondary);
    FocusRing(s, b, id, 8.f);
}

void DrawSegmented(AppState& s, const Box& b, const wchar_t* const* labels,
                   int count, int selected, int first_id) {
    const Palette p = Pal(s.system_dark);
    const float pad = 3.f;
    FillBox(s, b, 9.f, p.track);
    const float inner_w = b.W() - pad * 2.f;
    const float seg_w = inner_w / count;
    for (int i = 0; i < count; ++i) {
        Box seg{b.l + pad + i * seg_w, b.t + pad,
                b.l + pad + (i + 1) * seg_w, b.b - pad};
        if (i == selected) {
            FillBox(s, seg, 7.f, p.card);
            StrokeBox(s, seg, 7.f, p.border, 1.f);
        } else if (Hover(s, seg)) {
            FillBox(s, seg, 7.f, p.hover);
        }
        DrawText(s, s.f_value.Get(), labels[i], seg,
                 i == selected ? p.text : p.secondary,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (s.focus >= first_id && s.focus < first_id + count) {
        FocusRing(s, b, s.focus, 9.f);
    }
}

void DrawToggle(AppState& s, const Box& b) {
    const Palette p = Pal(s.system_dark);
    const float t = s.toggle.value;
    FillBox(s, b, b.H() / 2.f, Mix(p.track, p.accent, t));
    const float knob = b.H() - 6.f;
    const float cx = b.l + 3.f + knob / 2.f + t * (b.W() - 6.f - knob);
    FillEllipse(s, cx, b.t + b.H() / 2.f, knob / 2.f, Rgb(255, 255, 255));
    FocusRing(s, b, kToggle, b.H() / 2.f);
}

enum class ButtonKind { Primary, Secondary, Danger };

void DrawButton(AppState& s, const Box& b, const wchar_t* label, int id,
                ButtonKind kind) {
    const Palette p = Pal(s.system_dark);
    const bool hot = Hover(s, b);
    if (kind == ButtonKind::Primary) {
        FillBox(s, b, 8.f, hot ? Brighter(p.accent, 0.12f) : p.accent);
        DrawText(s, s.f_button.Get(), label, b, Rgb(255, 255, 255),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        FillBox(s, b, 8.f, p.card);
        StrokeBox(s, b, 8.f, hot ? p.accent : p.border, 1.f);
        DrawText(s, s.f_button.Get(), label, b,
                 kind == ButtonKind::Danger ? p.danger : p.text,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    FocusRing(s, b, id, 8.f);
}

void DrawCard(AppState& s, const Box& b) {
    const Palette p = Pal(s.system_dark);
    FillBox(s, b, 12.f, p.card);
    StrokeBox(s, b, 12.f, p.border, 1.f);
}

void DrawRowLabel(AppState& s, const Box& card, float row_top,
                  const wchar_t* label, const wchar_t* description) {
    const Palette p = Pal(s.system_dark);
    DrawText(s, s.f_label.Get(), label,
             Box{card.l + 20.f, row_top + 6.f, card.r - 260.f, row_top + 30.f},
             p.text);
    DrawText(s, s.f_desc.Get(), description,
             Box{card.l + 20.f, row_top + 31.f, card.r - 260.f, row_top + 49.f},
             p.secondary);
}

void DrawSidebar(AppState& s) {
    const Palette p = Pal(s.system_dark);
    FillPlain(s, Box{0, 0, kSidebar, s.height}, p.sidebar);
    FillPlain(s, Box{kSidebar - 1.f, 0, kSidebar, s.height}, p.border);

    FillBox(s, Box{20.f, 24.f, 60.f, 64.f}, 11.f, p.accent);
    DrawText(s, s.f_logo.Get(), L"拼", Box{20.f, 24.f, 60.f, 64.f},
             Rgb(255, 255, 255), DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawText(s, s.f_app.Get(), L"PIME 拼音",
             Box{72.f, 26.f, kSidebar - 12.f, 46.f}, p.text);
    DrawText(s, s.f_small.Get(), L"Windows 输入法",
             Box{72.f, 46.f, kSidebar - 12.f, 64.f}, p.secondary);

    const Page pages[5] = {Page::General, Page::Appearance, Page::Plugins,
                           Page::Dictionary, Page::About};
    const wchar_t* names[5] = {L"常规", L"外观", L"方案包", L"词库", L"关于"};
    const int ids[5] = {kNavGeneral, kNavAppearance, kNavPlugins,
                        kNavDictionary, kNavAbout};
    for (int i = 0; i < 5; ++i) {
        const Box& b = s.layout.nav[i];
        const bool selected = s.page == pages[i];
        if (selected) {
            FillBox(s, b, 9.f, p.accent_soft);
            FillBox(s, Box{b.l + 1.f, b.t + 13.f, b.l + 4.f, b.b - 13.f},
                    2.f, p.accent);
        } else if (Hover(s, b)) {
            FillBox(s, b, 9.f, p.hover);
        }
        DrawText(s, selected ? s.f_button.Get() : s.f_label.Get(), names[i],
                 Box{b.l + 16.f, b.t, b.r - 8.f, b.b},
                 selected ? p.accent : p.text);
        FocusRing(s, b, ids[i], 9.f);
    }

    const float bottom = s.height;
    DrawLine(s, 20.f, bottom - 86.f, kSidebar - 20.f, bottom - 86.f,
             p.border, 1.f);
    FillEllipse(s, 24.f, bottom - 65.f, 3.5f,
                s.installed ? p.success : p.secondary);
    DrawText(s, s.f_small.Get(),
             s.installed ? L"输入法已安装" : L"输入法未安装",
             Box{34.f, bottom - 74.f, kSidebar - 16.f, bottom - 56.f},
             p.secondary);
    DrawText(s, s.f_small.Get(), L"版本 " + std::wstring(kVersion),
             Box{20.f, bottom - 44.f, kSidebar - 16.f, bottom - 26.f},
             p.secondary);
}

void DrawPageHeader(AppState& s, const wchar_t* title,
                    const wchar_t* description) {
    const Palette p = Pal(s.system_dark);
    const float x0 = kSidebar + kMargin;
    const float x1 = s.width - kMargin;
    DrawText(s, s.f_page.Get(), title, Box{x0, 28.f, x1, 56.f}, p.text);
    DrawText(s, s.f_desc.Get(), description, Box{x0, 58.f, x1, 76.f},
             p.secondary);
}

void DrawGeneralPage(AppState& s) {
    const Palette p = Pal(s.system_dark);
    const float x0 = kSidebar + kMargin;
    const float x1 = s.width - kMargin;

    const Box card1{x0, 96.f, x1, 298.f};
    DrawCard(s, card1);

    DrawRowLabel(s, card1, 110.f, L"输入方案", L"选择已安装的方案包");
    DrawSelect(s, s.layout.chain,
               SchemeName(s, ChainName(s.settings.chain)), kChain,
               s.menu == Menu::Chain);
    DrawLine(s, card1.l + 20.f, 168.f, card1.r - 20.f, 168.f, p.border, 1.f);

    DrawRowLabel(s, card1, 168.f, L"候选每页", L"每页显示的候选数量");
    DrawSelect(s, s.layout.page_size,
               std::to_wstring(s.settings.page_size) + L" 个", kPageSize,
               s.menu == Menu::PageSize);
    DrawLine(s, card1.l + 20.f, 226.f, card1.r - 20.f, 226.f, p.border, 1.f);

    DrawRowLabel(s, card1, 226.f, L"标点风格",
                 L"输入中按标点会先上屏候选词，再输入标点");
    const wchar_t* punct_labels[2] = {L"中文全角 ，。？！", L"英文半角 ,.?!"};
    const int punct_selected = s.settings.punctuation ? 0 : 1;
    DrawSegmented(s, Box{s.layout.punct[0].l, s.layout.punct[0].t,
                         s.layout.punct[1].r, s.layout.punct[0].b},
                  punct_labels, 2, punct_selected,
                  kPunctFull);

    const Box card2{x0, 314.f, x1, 418.f};
    DrawCard(s, card2);
    DrawRowLabel(s, card2, 314.f, L"启动时默认中文",
                 L"新应用默认进入中文输入");
    DrawToggle(s, s.layout.toggle);
    DrawText(s, s.f_small.Get(),
             L"每个应用会分别记住上次的中英文状态，按 Shift 轻按可切换。",
             Box{card2.l + 20.f, 386.f, card2.r - 20.f, 404.f}, p.secondary);

    const Box card3{x0, 434.f, x1, 554.f};
    DrawCard(s, card3);
    DrawText(s, s.f_section.Get(), L"快捷键",
             Box{card3.l + 20.f, card3.t + 14.f, card3.r - 20.f,
                 card3.t + 34.f},
             p.text);
    struct Shortcut {
        const wchar_t* key;
        const wchar_t* description;
    };
    const Shortcut shortcuts[] = {
        {L"Shift", L"轻按切换中英文"},
        {L"Space", L"选择第一个候选"},
        {L"数字键", L"选择对应候选"},
        {L"↑ ↓ / PgUp PgDn", L"翻页"},
        {L"v + 数字", L"符号与算式输入"},
        {L"Esc", L"取消当前输入"},
    };
    const float column = (card3.W() - 40.f) / 2.f;
    for (int i = 0; i < 6; ++i) {
        const float cx = card3.l + 20.f + (i % 2) * column;
        const float cy = card3.t + 44.f + (i / 2) * 24.f;
        DrawText(s, s.f_desc.Get(), shortcuts[i].key,
                 Box{cx, cy, cx + 116.f, cy + 22.f}, p.text);
        DrawText(s, s.f_desc.Get(), shortcuts[i].description,
                 Box{cx + 122.f, cy, cx + column - 8.f, cy + 22.f},
                 p.secondary);
    }
}

void DrawThemeTile(AppState& s, int index, const wchar_t* label, int id) {
    const Palette p = Pal(s.system_dark);
    const Box& b = s.layout.theme[index];
    const bool selected = static_cast<int>(s.settings.theme) == index;
    const bool hot = Hover(s, b);
    FillBox(s, b, 10.f, selected ? p.accent_soft : p.card);
    StrokeBox(s, b, 10.f, selected ? p.accent : (hot ? p.secondary : p.border),
              selected ? 1.6f : 1.f);

    const Box preview{b.l + 12.f, b.t + 12.f, b.r - 12.f, b.t + 58.f};
    D2D1_COLOR_F preview_bg = Rgb(246, 248, 251);
    D2D1_COLOR_F line = Rgb(205, 212, 222);
    if (index == 2) {
        preview_bg = Rgb(28, 30, 34);
        line = Rgb(70, 75, 84);
    } else if (index == 0) {
        preview_bg = s.system_dark ? Rgb(28, 30, 34) : Rgb(246, 248, 251);
        line = s.system_dark ? Rgb(70, 75, 84) : Rgb(205, 212, 222);
    }
    FillBox(s, preview, 7.f, preview_bg);
    FillBox(s, Box{preview.l + 10.f, preview.t + 12.f, preview.l + 10.f + 34.f,
                   preview.t + 17.f},
            2.5f, line);
    FillBox(s, Box{preview.l + 10.f, preview.t + 23.f, preview.l + 10.f + 58.f,
                   preview.t + 28.f},
            2.5f, line);
    FillBox(s, Box{preview.l + 10.f, preview.t + 34.f, preview.l + 10.f + 24.f,
                   preview.t + 39.f},
            2.5f, line);

    DrawText(s, s.f_value.Get(), label,
             Box{b.l + 8.f, b.t + 64.f, b.r - 8.f, b.t + 88.f},
             selected ? p.accent : p.text, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (selected) {
        FillEllipse(s, b.r - 16.f, b.t + 16.f, 7.f, p.accent);
        DrawText(s, s.f_badge.Get(), L"✓",
                 Box{b.r - 24.f, b.t + 7.f, b.r - 8.f, b.t + 25.f},
                 Rgb(255, 255, 255), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    FocusRing(s, b, id, 10.f);
}

void DrawCandidatePreview(AppState& s, const Box& card) {
    const Palette p = Pal(s.system_dark);
    const bool dark = CandidateDark(s);
    const D2D1_COLOR_F strip = dark ? Rgb(42, 43, 47) : Rgb(255, 255, 255);
    const D2D1_COLOR_F border = dark ? Rgb(58, 61, 68) : Rgb(224, 228, 234);
    const D2D1_COLOR_F text = dark ? Rgb(238, 239, 242) : Rgb(27, 28, 32);
    const D2D1_COLOR_F sub = dark ? Rgb(154, 160, 170) : Rgb(109, 115, 124);
    const D2D1_COLOR_F accent = dark ? Rgb(91, 140, 255) : Rgb(47, 107, 255);

    const Box strip_box{card.l + 20.f, card.t + 44.f, card.r - 20.f,
                        card.t + 108.f};
    FillBox(s, strip_box, 10.f, strip);
    StrokeBox(s, strip_box, 10.f, border, 1.f);

    const float cy = strip_box.t + strip_box.H() / 2.f;
    const Box badge{strip_box.l + 12.f, cy - 12.f, strip_box.l + 36.f,
                    cy + 12.f};
    FillBox(s, badge, 6.f, accent);
    DrawText(s, s.f_badge.Get(), s.settings.chinese ? L"中" : L"英", badge,
             Rgb(255, 255, 255), DWRITE_TEXT_ALIGNMENT_CENTER);

    const std::wstring pinyin = L"pinyin";
    const float pin_x = strip_box.l + 48.f;
    DrawText(s, s.f_value.Get(), pinyin,
             Box{pin_x, strip_box.t, pin_x + 90.f, strip_box.b}, text);
    const float pin_w = MeasureText(s, s.f_value.Get(), pinyin);
    DrawLine(s, pin_x + 1.f, cy + 10.f, pin_x + pin_w, cy + 10.f, accent, 1.f);

    DrawText(s, s.f_value.Get(),
             L"1 拼音    2 品    3 频    4 拼    5 聘",
             Box{pin_x + 100.f, strip_box.t, strip_box.r - 90.f, strip_box.b},
             text);
    DrawText(s, s.f_small.Get(), dark ? L"深色" : L"浅色",
             Box{strip_box.r - 80.f, strip_box.t, strip_box.r - 12.f,
                 strip_box.b},
             sub, DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void DrawAppearancePage(AppState& s) {
    const Palette p = Pal(s.system_dark);
    const float x0 = kSidebar + kMargin;
    const float x1 = s.width - kMargin;

    const Box card1{x0, 96.f, x1, 264.f};
    DrawCard(s, card1);
    DrawText(s, s.f_section.Get(), L"主题",
             Box{card1.l + 20.f, card1.t + 14.f, card1.r - 20.f,
                 card1.t + 34.f},
             p.text);
    DrawThemeTile(s, 0, L"跟随系统", kThemeSystem);
    DrawThemeTile(s, 1, L"浅色", kThemeLight);
    DrawThemeTile(s, 2, L"深色", kThemeDark);

    const Box card2{x0, 280.f, x1, 510.f};
    DrawCard(s, card2);
    DrawText(s, s.f_section.Get(), L"候选窗口",
             Box{card2.l + 20.f, card2.t + 14.f, card2.r - 20.f,
                 card2.t + 34.f},
             p.text);
    DrawCandidatePreview(s, card2);

    static const wchar_t* const kOrientLabels[] = {L"横排", L"竖排"};
    static const wchar_t* const kSizeLabels[] = {L"小", L"中", L"大"};
    DrawRowLabel(s, card2, card2.t + 122.f, L"排列方式",
                 L"单行横排，或多行竖排");
    DrawSegmented(s, s.layout.orient, kOrientLabels, 2,
                  s.settings.candidate_vertical ? 1 : 0, kOrientH);
    DrawRowLabel(s, card2, card2.t + 178.f, L"候选字号",
                 L"候选文字的显示大小");
    const int size_index = s.settings.candidate_font <= 14
                               ? 0
                               : (s.settings.candidate_font <= 17 ? 1 : 2);
    DrawSegmented(s, s.layout.size, kSizeLabels, 3, size_index, kSizeSmall);
}

// ---- plugin chain page -----------------------------------------------------

ChainValue CurrentValue(const EditPlugin& entry,
                        const PluginSettingSpec& spec) {
    for (const ChainSetting& setting : entry.plugin.settings) {
        if (setting.key == spec.key) return setting.value;
    }
    return spec.def;
}

void DrawMiniToggle(AppState& s, const Box& b, bool on) {
    const Palette p = Pal(s.system_dark);
    FillBox(s, b, b.H() / 2.f, on ? p.accent : p.track);
    const float knob = b.H() - 6.f;
    const float cx = on ? b.r - 3.f - knob / 2.f : b.l + 3.f + knob / 2.f;
    FillEllipse(s, cx, b.t + b.H() / 2.f, knob / 2.f, Rgb(255, 255, 255));
}

void DrawSlider(AppState& s, const Box& b, const PluginSettingSpec& spec,
                const ChainValue& value) {
    const Palette p = Pal(s.system_dark);
    const float min_value = static_cast<float>(spec.min);
    const float max_value =
        spec.max > spec.min ? static_cast<float>(spec.max)
                            : min_value + 1.f;
    const float raw = static_cast<float>(value.integer);
    const float t = std::clamp((raw - min_value) / (max_value - min_value),
                               0.f, 1.f);
    const float cy = b.t + b.H() / 2.f;
    FillBox(s, Box{b.l, cy - 3.f, b.r, cy + 3.f}, 3.f, p.track);
    FillBox(s, Box{b.l, cy - 3.f, b.l + b.W() * t, cy + 3.f}, 3.f, p.accent);
    FillEllipse(s, b.l + b.W() * t, cy, 7.f, p.card);
    FillEllipse(s, b.l + b.W() * t, cy, 5.5f, p.accent);
}

void DrawPluginsPage(AppState& s) {
    const Palette p = Pal(s.system_dark);
    const float x0 = kSidebar + kMargin;
    const float x1 = s.width - kMargin;
    const std::vector<std::wstring> presets = PresetList(s);
    const bool customized = s.plugin_dirty || s.plugin_custom;

    // Preset card.
    const Box card1{x0, 92.f, x1, 204.f};
    DrawCard(s, card1);
    DrawText(s, s.f_section.Get(), L"选择方案包",
             Box{x0 + 20.f, 100.f, x1 - 310.f, 118.f}, p.text);
    DrawText(s, s.f_small.Get(), L"切换后点「保存并应用」生效",
             Box{x0 + 20.f, 117.f, x1 - 200.f, 131.f}, p.secondary);
    if (customized) {
        DrawText(s, s.f_badge.Get(), L"已自定义",
                 Box{x1 - 300.f, 104.f, x1 - 200.f, 126.f}, p.accent,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        DrawButton(s, s.layout.restore, L"还原", kRestore,
                   ButtonKind::Secondary);
    }
    DrawButton(s, s.layout.import_scheme, L"导入…", kImportScheme,
               ButtonKind::Secondary);
    for (int i = 0; i < s.layout.preset_count; ++i) {
        const Box& chip = s.layout.preset[i];
        const bool selected =
            !s.plugin_choice.empty() &&
            s.plugin_choice == ToUtf8(presets[i]);
        const bool hot = Hover(s, chip);
        FillBox(s, chip, 8.f, selected ? p.accent_soft : p.bg);
        StrokeBox(s, chip, 8.f, selected ? p.accent : (hot ? p.secondary
                                                           : p.border),
                  selected ? 1.4f : 1.f);
        DrawText(s, selected ? s.f_button.Get() : s.f_value.Get(),
                 SchemeName(s, presets[i]),
                 Box{chip.l + 8.f, chip.t, chip.r - 8.f, chip.b},
                 selected ? p.accent : p.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (selected && s.plugin_dirty) {
            FillEllipse(s, chip.r - 10.f, chip.t + 10.f, 3.f, p.accent);
        }
    }

    // Pipeline card.
    const Box card2{x0, 216.f, x1, s.height - 80.f};
    DrawCard(s, card2);
    DrawText(s, s.f_section.Get(), L"插件流水线",
             Box{x0 + 20.f, 226.f, x1 - 200.f, 248.f}, p.text);
    DrawText(s, s.f_small.Get(),
             std::to_wstring(s.edit_plugins.size()) + L" 个插件",
             Box{x0 + 20.f, 226.f, x1 - 20.f, 248.f}, p.secondary,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    DrawLine(s, x0 + 20.f, 252.f, x1 - 20.f, 252.f, p.border, 1.f);

    const Box& view = s.layout.plug_view;
    s.rt->PushAxisAlignedClip(
        D2D1::RectF(view.l, view.t, view.r, view.b),
        D2D1_ANTIALIAS_MODE_ALIASED);

    std::string last_role;
    for (const PlugRow& row : s.layout.plug_rows) {
        const EditPlugin& entry = s.edit_plugins[row.plugin];
        const PluginMeta* meta =
            entry.meta >= 0 ? &s.catalog[entry.meta] : nullptr;
        const std::string role =
            meta && !meta->role.empty() ? meta->role : entry.plugin.role;

        if (role != last_role) {
            last_role = role;
            const float top = row.card.t - 24.f;
            FillEllipse(s, x0 + 10.f, top + 12.f, 4.f, RoleColor(role));
            DrawText(s, s.f_small.Get(), RoleLabel(role),
                     Box{x0 + 22.f, top, x0 + 90.f, top + 24.f},
                     p.secondary);
            const float label_w = MeasureText(s, s.f_small.Get(), RoleLabel(role));
            DrawLine(s, x0 + 30.f + label_w, top + 12.f, x1 - 20.f,
                     top + 12.f, p.border, 1.f);
        }

        const Box& card = row.card;
        const bool enabled = entry.enabled;
        FillBox(s, card, 10.f, p.bg);
        StrokeBox(s, card, 10.f, row.expanded ? p.accent : p.border,
                  row.expanded ? 1.4f : 1.f);
        FillBox(s, Box{card.l, card.t + 8.f, card.l + 3.f, card.t + 32.f},
                1.5f, enabled ? RoleColor(role) : p.track);

        const std::wstring name = PluginName(entry.plugin.id);
        DrawTextEllipsis(s, s.f_button.Get(), name,
                         Box{card.l + 14.f, card.t + 3.f, card.r - 110.f,
                             card.t + 24.f},
                         enabled ? p.text : p.secondary);
        std::wstring detail = ToWide(entry.plugin.id);
        if (!entry.plugin.version.empty()) {
            detail += L"  ·  v" + ToWide(entry.plugin.version);
        }
        if (!enabled) detail = L"已停用  ·  " + detail;
        DrawTextEllipsis(s, s.f_small.Get(), detail,
                         Box{card.l + 14.f, card.t + 21.f, card.r - 110.f,
                             card.t + 37.f},
                         p.secondary);

        if (entry.structural) {
            DrawText(s, s.f_small.Get(), L"必需",
                     Box{card.r - 56.f, card.t + 10.f, card.r - 20.f,
                         card.t + 30.f},
                     p.secondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            DrawMiniToggle(s, row.toggle, enabled);
            FocusRing(s, row.toggle, kPlugToggle + row.plugin,
                      row.toggle.H() / 2.f);
        }
        FocusRing(s, card, kPlugCard + row.plugin, 10.f);

        if (row.expanded) {
            DrawLine(s, card.l + 16.f, card.t + 40.f, card.r - 16.f,
                     card.t + 40.f, p.border, 1.f);
            float control_top = card.t + 48.f;
            if (meta && !meta->description.empty()) {
                DrawTextEllipsis(s, s.f_desc.Get(), ToWide(meta->description),
                                 Box{card.l + 20.f, card.t + 44.f,
                                     card.r - 20.f, card.t + 66.f},
                                 p.secondary);
                control_top += 22.f;
            }
            if (meta) {
                for (int j = 0; j < static_cast<int>(meta->settings.size());
                     ++j) {
                    const PluginSettingSpec& spec = meta->settings[j];
                    const float top = control_top + j * 30.f;
                    DrawText(s, s.f_desc.Get(), SettingLabel(spec.key),
                             Box{card.l + 20.f, top, card.r - 320.f,
                                 top + 30.f},
                             p.secondary);
                    const ChainValue value = CurrentValue(entry, spec);
                    if (spec.type == "bool") {
                        DrawMiniToggle(
                            s, Box{card.r - 56.f, top + 5.f, card.r - 20.f,
                                   top + 25.f},
                            value.boolean);
                    } else if (spec.type == "int") {
                        DrawSlider(
                            s, Box{card.r - 270.f, top, card.r - 64.f,
                                   top + 30.f},
                            spec, value);
                        DrawText(s, s.f_value.Get(),
                                 std::to_wstring(value.integer),
                                 Box{card.r - 58.f, top, card.r - 16.f,
                                     top + 30.f},
                                 p.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
                    } else if (spec.type == "enum") {
                        // The bundled chains use a handful of enum tokens;
                        // show them in Chinese and fall back to the raw token.
                        auto enum_label = [](const std::string& v) {
                            if (v == "general") return std::wstring(L"通用");
                            if (v == "chat") return std::wstring(L"聊天");
                            if (v == "code") return std::wstring(L"代码");
                            if (v == "formal") return std::wstring(L"正式");
                            if (v == "zh") return std::wstring(L"中文");
                            if (v == "en") return std::wstring(L"英文");
                            if (v == "xiaohe") return std::wstring(L"小鹤");
                            if (v == "ziranma") return std::wstring(L"自然码");
                            if (v == "microsoft") return std::wstring(L"微软");
                            if (v == "sogou") return std::wstring(L"搜狗");
                            return ToWide(v);
                        };
                        std::vector<std::wstring> wide;
                        std::vector<const wchar_t*> labels;
                        int selected = 0;
                        for (int v = 0;
                             v < static_cast<int>(spec.values.size()); ++v) {
                            wide.push_back(enum_label(spec.values[v]));
                            if (spec.values[v] == value.text) selected = v;
                        }
                        for (const std::wstring& label : wide) {
                            labels.push_back(label.c_str());
                        }
                        if (!labels.empty()) {
                            DrawSegmented(
                                s,
                                Box{card.r - 300.f, top + 2.f, card.r - 16.f,
                                    top + 28.f},
                                labels.data(),
                                static_cast<int>(labels.size()), selected,
                                -1000);
                        }
                    } else {
                        const bool secret = spec.type == "secret";
                        const std::wstring text =
                            secret && !value.text.empty()
                                ? std::wstring(6, L'•')
                                : (value.text.empty() ? L"未设置"
                                                      : ToWide(value.text));
                        DrawTextEllipsis(
                            s, s.f_small.Get(), text,
                            Box{card.r - 300.f, top, card.r - 16.f,
                                top + 30.f},
                            p.secondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
                    }
                }
                for (const PlugControl& control : row.controls) {
                    FocusRing(s, control.box,
                              kPlugSetting + row.plugin * 32 + control.spec,
                              6.f);
                }
            }
        }
    }

    s.rt->PopAxisAlignedClip();

    if (s.edit_plugins.empty()) {
        DrawText(s, s.f_desc.Get(), L"这个方案没有可显示的插件。",
                 Box{x0 + 20.f, view.t + 12.f, x1 - 20.f, view.t + 40.f},
                 p.secondary);
    }
}

void DrawAboutPage(AppState& s) {
    const Palette p = Pal(s.system_dark);
    const float x0 = kSidebar + kMargin;
    const float x1 = s.width - kMargin;

    const Box card1{x0, 96.f, x1, 324.f};
    DrawCard(s, card1);
    DrawText(s, s.f_section.Get(), L"应用信息",
             Box{card1.l + 20.f, card1.t + 14.f, card1.r - 20.f,
                 card1.t + 34.f},
             p.text);
    struct Row {
        const wchar_t* name;
        std::wstring value;
    };
    const Row rows[] = {
        {L"版本", kVersion},
        {L"输入法内核", L"pime-core（WAMR 插件运行时）"},
        {L"输入方案", SchemeName(s, ChainName(s.settings.chain))},
        {L"日志目录", DataDirectory()},
        {L"许可证", L"Apache License 2.0"},
    };
    float y = card1.t + 46.f;
    for (const Row& row : rows) {
        DrawText(s, s.f_desc.Get(), row.name,
                 Box{card1.l + 20.f, y, card1.l + 140.f, y + 32.f},
                 p.secondary);
        DrawText(s, s.f_value.Get(), row.value,
                 Box{card1.l + 140.f, y, card1.r - 20.f, y + 32.f}, p.text);
        y += 34.f;
    }

    const Box card2{x0, 338.f, x1, 466.f};
    DrawCard(s, card2);
    DrawText(s, s.f_section.Get(), L"输入法状态",
             Box{card2.l + 20.f, card2.t + 14.f, card2.r - 160.f,
                 card2.t + 38.f},
             p.text);
    const Box pill{card2.r - 92.f, card2.t + 16.f, card2.r - 20.f,
                   card2.t + 40.f};
    FillBox(s, pill, 12.f, s.installed ? p.success_soft : p.track);
    DrawText(s, s.f_small.Get(), s.installed ? L"已安装" : L"未安装", pill,
             s.installed ? p.success : p.secondary,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawText(s, s.f_desc.Get(),
             s.installed
                 ? L"已注册为 zh-CN 文本服务，可在系统输入法列表中选择「PIME 拼音」。"
                 : L"尚未安装。安装需要管理员权限，安装后即可在输入法列表里选择。",
             Box{card2.l + 20.f, card2.t + 44.f, card2.r - 20.f,
                 card2.t + 64.f},
             p.secondary);

    DrawButton(s, s.layout.install, L"安装或重新安装", kInstall,
               ButtonKind::Secondary);
    DrawButton(s, s.layout.uninstall, L"卸载", kUninstall, ButtonKind::Danger);
    DrawButton(s, s.layout.activate, L"切换到 PIME", kActivate,
               ButtonKind::Primary);
}

void DrawDictionaryPage(AppState& s) {
    const Palette p = Pal(s.system_dark);
    DrawButton(s, s.layout.dict_tool[0], L"添加", kDictAdd,
               ButtonKind::Primary);
    DrawButton(s, s.layout.dict_tool[1], L"编辑", kDictEdit,
               ButtonKind::Secondary);
    DrawButton(s, s.layout.dict_tool[2], L"删除", kDictDelete,
               ButtonKind::Secondary);
    DrawButton(s, s.layout.dict_tool[3], L"导入…", kDictImport,
               ButtonKind::Secondary);
    DrawButton(s, s.layout.dict_tool[4], L"导出…", kDictExport,
               ButtonKind::Secondary);
    DrawButton(s, s.layout.dict_tool[5], L"清空", kDictClear,
               ButtonKind::Danger);
    DrawButton(s, s.layout.dict_tool[6], L"刷新", kDictRefresh,
               ButtonKind::Secondary);

    const Box& card = s.layout.dict_list;
    DrawCard(s, card);
    const float top = DictRowsTop(s);
    // Columns scale with the card: the window is not always 900 DIP wide.
    const float inner = card.W() - 44.f;
    const float word_right = card.l + 20.f + inner * 0.42f;
    const float code_right = card.l + 20.f + inner * 0.78f;
    DrawText(s, s.f_small.Get(), L"词",
             Box{card.l + 20.f, card.t, word_right - 4.f, top}, p.secondary);
    DrawText(s, s.f_small.Get(), L"拼音码",
             Box{word_right, card.t, code_right - 4.f, top}, p.secondary);
    DrawText(s, s.f_small.Get(), L"权重",
             Box{code_right, card.t, card.r - 24.f, top}, p.secondary,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    DrawLine(s, card.l + 20.f, top, card.r - 20.f, top, p.border, 1.f);

    const int visible = DictVisibleRows(s);
    s.rt->PushAxisAlignedClip(D2D1::RectF(card.l, top, card.r, card.b),
                              D2D1_ANTIALIAS_MODE_ALIASED);
    const int last = std::min(static_cast<int>(s.dict_rows.size()),
                              s.dict_scroll + visible);
    for (int i = s.dict_scroll; i < last; ++i) {
        const DictRow& row = s.dict_rows[i];
        const float y = top + (i - s.dict_scroll) * kDictRowHeight;
        const Box box{card.l, y, card.r, y + kDictRowHeight};
        if (i == s.dict_selected) {
            FillBox(s, Box{card.l + 8.f, y + 1.f, card.r - 8.f,
                           y + kDictRowHeight - 1.f},
                    7.f, p.accent_soft);
        } else if (Hover(s, box)) {
            FillBox(s, Box{card.l + 8.f, y + 1.f, card.r - 8.f,
                           y + kDictRowHeight - 1.f},
                    7.f, p.hover);
        }
        DrawTextEllipsis(s, s.f_value.Get(), row.word,
                         Box{card.l + 20.f, y, word_right - 4.f,
                             y + kDictRowHeight},
                         p.text);
        DrawTextEllipsis(s, s.f_value.Get(), row.code,
                         Box{word_right, y, code_right - 4.f,
                             y + kDictRowHeight},
                         p.secondary);
        DrawText(s, s.f_value.Get(), std::to_wstring(row.weight),
                 Box{code_right, y, card.r - 24.f, y + kDictRowHeight},
                 p.secondary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    s.rt->PopAxisAlignedClip();

    const int total = static_cast<int>(s.dict_rows.size());
    if (visible > 0 && total > visible) {
        const float track_top = top + 4.f;
        const float track_bottom = card.b - 8.f;
        const float track_height = track_bottom - track_top;
        const float thumb_height =
            std::max(24.f, track_height * visible / static_cast<float>(total));
        const float progress = static_cast<float>(s.dict_scroll) /
                               static_cast<float>(total - visible);
        const float thumb_top =
            track_top + (track_height - thumb_height) * progress;
        FillBox(s, Box{card.r - 10.f, track_top, card.r - 4.f, track_bottom},
                3.f, p.track);
        FillBox(s, Box{card.r - 10.f, thumb_top, card.r - 4.f,
                       thumb_top + thumb_height},
                3.f, p.secondary);
    }

    if (s.dict_loaded && s.dict_rows.empty()) {
        DrawText(s, s.f_desc.Get(),
                 L"还没有词条。可以用「添加」自建，或从 Rime 词库导入。",
                 Box{card.l + 20.f, top + 12.f, card.r - 20.f, top + 40.f},
                 p.secondary);
    }

    DrawText(s, s.f_desc.Get(), s.dict_status,
             Box{card.l + 2.f, card.b + 6.f, card.r, card.b + 28.f},
             s.dict_status_ok ? p.secondary : p.danger);
}

void DrawFooter(AppState& s) {
    const Palette p = Pal(s.system_dark);
    const float x0 = kSidebar + kMargin;
    const float x1 = s.width - kMargin;
    DrawLine(s, x0, s.height - 64.f, x1, s.height - 64.f, p.border, 1.f);

    std::wstring message;
    D2D1_COLOR_F color = p.secondary;
    if (Dirty(s)) {
        message = L"有未保存的更改";
    } else if (s.page == Page::Plugins && !s.plugin_status.empty()) {
        message = s.plugin_status;
        color = s.plugin_status_ok ? p.accent : p.danger;
    } else if (!s.status.empty()) {
        message = s.status;
        color = s.status_ok ? p.accent : p.danger;
    }
    DrawText(s, s.f_desc.Get(), message,
             Box{x0 + 14.f, s.height - 52.f, x1 - 260.f, s.height - 18.f},
             color);
    if (!message.empty()) {
        FillEllipse(s, x0 + 4.f, s.height - 35.f, 3.f,
                    Dirty(s) ? p.secondary : color);
    }

    DrawButton(s, s.layout.close, L"关闭", kClose, ButtonKind::Secondary);
    DrawButton(s, s.layout.apply, L"保存并应用", kApply, ButtonKind::Primary);
}

void DrawMenu(AppState& s) {
    if (s.menu == Menu::None || s.layout.menu_items <= 0) return;
    const Palette p = Pal(s.system_dark);
    const Box& pop = s.layout.menu;
    FillBox(s, Box{pop.l + 2.f, pop.t + 6.f, pop.r + 2.f, pop.b + 8.f}, 8.f,
            Rgb(0, 0, 0, 0.16f));
    FillBox(s, pop, 8.f, p.card);
    StrokeBox(s, pop, 8.f, p.border, 1.f);

    if (s.menu == Menu::Chain) {
        const int selected = ChainIndex(s);
        for (size_t i = 0; i < s.schemes.size(); ++i) {
            const Box item = ItemBox(s, static_cast<int>(i));
            const bool hot = s.mouse_in && item.Contains(s.mx, s.my);
            if (hot) FillBox(s, item, 6.f, p.accent_soft);
            const bool current = static_cast<int>(i) == selected;
            DrawText(s, s.f_value.Get(), SchemeName(s, s.schemes[i].id),
                     Box{item.l + 12.f, item.t, item.r - 32.f, item.b},
                     current ? p.accent : p.text);
            if (current) {
                DrawText(s, s.f_badge.Get(), L"✓",
                         Box{item.r - 28.f, item.t, item.r - 8.f, item.b},
                         p.accent, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
    } else {
        for (int i = 0; i < 7; ++i) {
            const Box item = ItemBox(s, i);
            const bool hot = s.mouse_in && item.Contains(s.mx, s.my);
            if (hot) FillBox(s, item, 6.f, p.accent_soft);
            const bool current = s.settings.page_size ==
                                 static_cast<uint32_t>(i + 3);
            DrawText(s, s.f_value.Get(),
                     std::to_wstring(i + 3) + L" 个候选",
                     Box{item.l + 12.f, item.t, item.r - 32.f, item.b},
                     current ? p.accent : p.text);
            if (current) {
                DrawText(s, s.f_badge.Get(), L"✓",
                         Box{item.r - 28.f, item.t, item.r - 8.f, item.b},
                         p.accent, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
    }
}

void Draw(AppState& s) {
    const Palette p = Pal(s.system_dark);
    s.rt->Clear(p.bg);
    DrawSidebar(s);
    switch (s.page) {
        case Page::General:
            DrawPageHeader(s, L"常规", L"输入方案、候选数量与标点行为");
            DrawGeneralPage(s);
            break;
        case Page::Appearance:
            DrawPageHeader(s, L"外观", L"主题与候选窗口样式");
            DrawAppearancePage(s);
            break;
        case Page::Plugins:
            DrawPageHeader(s, L"方案包",
                           L"方案包 = 链 + 插件；点开任一插件即可开关、调参");
            DrawPluginsPage(s);
            break;
        case Page::Dictionary:
            DrawPageHeader(s, L"词库",
                           L"自造词与学习到的词；支持导入导出 Rime 词库");
            DrawDictionaryPage(s);
            break;
        case Page::About:
            DrawPageHeader(s, L"关于", L"安装状态与应用信息");
            DrawAboutPage(s);
            break;
    }
    DrawFooter(s);
    DrawMenu(s);
}

// ---- actions ---------------------------------------------------------------

void Invalidate(AppState& s, HWND window) {
    BuildLayout(s);
    InvalidateRect(window, nullptr, FALSE);
}

// ---- plugin chain editing --------------------------------------------------

void EnsureCatalog(AppState& s) {
    if (s.catalog_loaded) return;
    s.catalog =
        LoadPluginCatalogFromSchemes(ModuleDirectory(nullptr), DataDirectory());
    s.catalog_loaded = true;
}

bool ReadChainAt(const std::wstring& path, ChainDoc* doc) {
    if (path.empty()) return false;
    std::string error;
    return ReadChainFile(path, doc, &error);
}

// The chain the engine would activate for a scheme: a user edit wins.
bool ReadSchemeChain(const std::wstring& id, ChainDoc* doc) {
    return ReadChainAt(
        SchemeChainPath(ModuleDirectory(nullptr), DataDirectory(), ToUtf8(id)),
        doc);
}

// The package's own chain, used to merge in plugins an upgrade added.
bool ReadSchemeBaseChain(const std::wstring& id, ChainDoc* doc) {
    return ReadChainAt(SchemeBaseChainPath(ModuleDirectory(nullptr),
                                           DataDirectory(), ToUtf8(id)),
                       doc);
}

void AttachMeta(AppState& s) {
    for (EditPlugin& entry : s.edit_plugins) {
        entry.meta = -1;
        for (int i = 0; i < static_cast<int>(s.catalog.size()); ++i) {
            const PluginMeta& meta = s.catalog[i];
            if (meta.id != entry.plugin.id) continue;
            if (!entry.plugin.version.empty() &&
                meta.version != entry.plugin.version) {
                continue;
            }
            entry.meta = i;
            break;
        }
    }
}

// The structural plugins make the chain work at all: without a parse or a
// rank stage the engine rejects the build, so the UI does not let them be
// switched off. The first translate is the primary one (rank depends on it).
void MarkStructural(AppState& s) {
    int first_translate = -1;
    for (int i = 0; i < static_cast<int>(s.edit_plugins.size()); ++i) {
        const EditPlugin& entry = s.edit_plugins[i];
        const std::string role =
            entry.meta >= 0 ? s.catalog[entry.meta].role : entry.plugin.role;
        if (role == "translate" && first_translate < 0) first_translate = i;
    }
    for (int i = 0; i < static_cast<int>(s.edit_plugins.size()); ++i) {
        EditPlugin& entry = s.edit_plugins[i];
        const std::string role =
            entry.meta >= 0 ? s.catalog[entry.meta].role : entry.plugin.role;
        entry.structural =
            role == "preprocess" || role == "parse" || role == "rank" ||
            role == "format" || role == "recognize" || role.empty() ||
            i == first_translate;
    }
}

// Loads a scheme into the editor. When a user edit exists, the package's own
// chain is used as the base so plugins added by an upgrade still show up.
void LoadEditorFromScheme(AppState& s, const std::wstring& id) {
    EnsureCatalog(s);
    s.plugins_loaded = false;
    s.edit_plugins.clear();
    s.plugin_selected = -1;
    s.plugin_scroll = 0;
    s.plugin_dirty = false;
    s.plugin_status.clear();
    s.plugin_status_ok = true;
    s.plugin_chain = ToUtf8(id);
    s.plugin_choice = ToUtf8(id);
    s.plugin_custom = HasSchemeOverlay(id);

    ChainDoc doc;
    if (!ReadSchemeChain(id, &doc)) {
        s.plugins_loaded = true;
        s.plugin_status =
            L"无法读取方案包「" + SchemeName(s, id) + L"」，插件列表不可用。";
        s.plugin_status_ok = false;
        return;
    }

    ChainDoc base_doc;
    const bool merge = s.plugin_custom &&
                       ReadSchemeBaseChain(id, &base_doc) &&
                       !base_doc.plugins.empty();
    const std::vector<ChainPlugin>& source = merge ? base_doc.plugins
                                                   : doc.plugins;
    for (const ChainPlugin& plugin : source) {
        EditPlugin entry;
        entry.plugin = plugin;
        if (merge) {
            for (const ChainPlugin& override : doc.plugins) {
                if (override.id == plugin.id) {
                    entry.plugin.settings = override.settings;
                    break;
                }
            }
            entry.enabled =
                std::find(doc.disabled.begin(), doc.disabled.end(), plugin.id) ==
                doc.disabled.end();
        }
        s.edit_plugins.push_back(std::move(entry));
    }
    AttachMeta(s);
    MarkStructural(s);
    s.plugins_loaded = true;
}

void LoadPluginsEditor(AppState& s) {
    LoadEditorFromScheme(s, ChainName(s.settings.chain));
}

void SelectPreset(AppState& s, HWND window, int index) {
    const std::vector<std::wstring> presets = PresetList(s);
    if (index < 0 || index >= static_cast<int>(presets.size())) return;
    const std::wstring id = presets[index];
    s.settings.chain = id + L".cbor";
    LoadEditorFromScheme(s, id);
    if (s.plugin_status_ok) {
        s.plugin_status = L"已选择「" + SchemeName(s, id) + L"」，保存后生效";
    }
    Invalidate(s, window);
}

void RestorePreset(AppState& s, HWND window) {
    const std::wstring id = s.plugin_chain.empty()
                                ? ChainName(s.settings.chain)
                                : ToWide(s.plugin_chain);
    DeleteFileW(SchemeOverlayPath(id).c_str());
    LoadEditorFromScheme(s, id);
    s.plugin_status = L"已还原为方案包默认";
    s.plugin_status_ok = true;
    Invalidate(s, window);
}

void SetPluginSetting(AppState& s, int plugin, int spec_index,
                      const ChainValue& value) {
    if (plugin < 0 || plugin >= static_cast<int>(s.edit_plugins.size())) return;
    EditPlugin& entry = s.edit_plugins[plugin];
    if (entry.meta < 0) return;
    const PluginMeta& meta = s.catalog[entry.meta];
    if (spec_index < 0 || spec_index >= static_cast<int>(meta.settings.size())) {
        return;
    }
    const PluginSettingSpec& spec = meta.settings[spec_index];
    for (ChainSetting& setting : entry.plugin.settings) {
        if (setting.key == spec.key) {
            setting.value = value;
            s.plugin_dirty = true;
            return;
        }
    }
    entry.plugin.settings.push_back(ChainSetting{spec.key, value});
    s.plugin_dirty = true;
}

// Applies a pointer position to an int control.
void DragPluginSetting(AppState& s, int id, float x) {
    if (id < kPlugSetting) return;
    const int plugin = (id - kPlugSetting) / 32;
    const int spec_index = (id - kPlugSetting) % 32;
    if (plugin < 0 || plugin >= static_cast<int>(s.edit_plugins.size())) return;
    const EditPlugin& entry = s.edit_plugins[plugin];
    if (entry.meta < 0) return;
    const PluginMeta& meta = s.catalog[entry.meta];
    if (spec_index >= static_cast<int>(meta.settings.size())) return;
    const PluginSettingSpec& spec = meta.settings[spec_index];
    if (spec.type != "int" || spec.max <= spec.min) return;

    // The track is the control box minus the trailing value label.
    const Box track{s.width - kMargin - 270.f, 0.f, s.width - kMargin - 64.f,
                    0.f};
    const float t =
        std::clamp((x - track.l) / std::max(1.f, track.W()), 0.f, 1.f);
    ChainValue value;
    value.kind = ChainValue::Kind::Int;
    value.integer = spec.min + static_cast<int64_t>(
                                    std::lround(t * (spec.max - spec.min)));
    SetPluginSetting(s, plugin, spec_index, value);
}

// Keyboard/activation path: one step on an int, next value on an enum, a flip
// on a bool.
void StepPluginSetting(AppState& s, int id, int direction) {
    if (id < kPlugSetting) return;
    const int plugin = (id - kPlugSetting) / 32;
    const int spec_index = (id - kPlugSetting) % 32;
    if (plugin < 0 || plugin >= static_cast<int>(s.edit_plugins.size())) return;
    EditPlugin& entry = s.edit_plugins[plugin];
    if (entry.meta < 0) return;
    const PluginMeta& meta = s.catalog[entry.meta];
    if (spec_index >= static_cast<int>(meta.settings.size())) return;
    const PluginSettingSpec& spec = meta.settings[spec_index];
    const ChainValue current = CurrentValue(entry, spec);

    if (spec.type == "bool") {
        ChainValue value;
        value.kind = ChainValue::Kind::Bool;
        value.boolean = !current.boolean;
        SetPluginSetting(s, plugin, spec_index, value);
    } else if (spec.type == "enum" && !spec.values.empty()) {
        int index = 0;
        for (int i = 0; i < static_cast<int>(spec.values.size()); ++i) {
            if (spec.values[i] == current.text) index = i;
        }
        const int count = static_cast<int>(spec.values.size());
        index = ((index + direction) % count + count) % count;
        ChainValue value;
        value.kind = ChainValue::Kind::Text;
        value.text = spec.values[index];
        SetPluginSetting(s, plugin, spec_index, value);
    } else if (spec.type == "int" && spec.max > spec.min) {
        const int64_t step =
            std::max<int64_t>(1, (spec.max - spec.min) / 100);
        ChainValue value;
        value.kind = ChainValue::Kind::Int;
        value.integer =
            std::clamp<int64_t>(current.integer + direction * step, spec.min,
                                spec.max);
        SetPluginSetting(s, plugin, spec_index, value);
    }
}

void TogglePlugin(AppState& s, int plugin) {
    if (plugin < 0 || plugin >= static_cast<int>(s.edit_plugins.size())) return;
    EditPlugin& entry = s.edit_plugins[plugin];
    if (entry.structural) return;
    entry.enabled = !entry.enabled;
    s.plugin_dirty = true;
}

void ApplySettings(AppState& s, HWND window) {
    Settings out = s.settings;

    if (s.plugins_loaded) {
        const std::wstring id = s.plugin_chain.empty()
                                    ? ChainName(s.settings.chain)
                                    : ToWide(s.plugin_chain);
        if (s.plugin_dirty) {
            ChainDoc doc;
            doc.base = ToUtf8(id);
            for (const EditPlugin& entry : s.edit_plugins) {
                if (entry.enabled) {
                    doc.plugins.push_back(entry.plugin);
                } else {
                    doc.disabled.push_back(entry.plugin.id);
                }
            }
            const std::wstring dir =
                PathJoin(PathJoin(DataDirectory(), L"schemes"), id);
            std::string error;
            if (!EnsureDirectory(dir) ||
                !WriteChainFile(PathJoin(dir, L"custom.cbor"), doc, &error)) {
                s.status = L"保存方案包失败：" + ToWide(error);
                s.status_ok = false;
                Invalidate(s, window);
                return;
            }
            s.plugin_custom = true;
            s.plugin_dirty = false;
        }
        out.chain = id + L".cbor";
    }

    SaveSettings(out);
    s.settings = out;
    s.saved = out;
    NotifyConfigChanged();
    s.status = L"设置已保存并应用";
    s.status_ok = true;
    s.plugin_status = L"已保存并应用";
    s.plugin_status_ok = true;
    Invalidate(s, window);
}

// ---- user dictionary -------------------------------------------------------

std::wstring DictStatusText(int status) {
    switch (status) {
        case PIME_FULL:
            return L"词库已满，请先删除或清空部分词条";
        case PIME_DENIED:
            return L"无法访问词库存储（store key 不可用）";
        case PIME_NOT_FOUND:
            return L"词条不存在";
        case PIME_CLOSED:
            return L"无法连接输入法引擎";
        default:
            return L"输入法引擎返回错误 " + std::to_wstring(status);
    }
}

bool EnsureEngine(AppState& s) {
    IpcClient& ipc = IpcClient::Instance();
    if (ipc.ready()) return true;
    const std::wstring exe =
        PathJoin(ModuleDirectory(nullptr), L"pime_engine.exe");
    if (!ipc.Start(exe)) {
        s.dict_status = L"无法连接输入法引擎";
        s.dict_status_ok = false;
        return false;
    }
    return true;
}

// Fetches the whole dictionary. `note` prefixes the result line when this
// follows an edit; empty means this is a plain load.
void LoadDict(AppState& s, HWND window, const std::wstring& note = {}) {
    s.dict_rows.clear();
    s.dict_selected = -1;
    s.dict_scroll = 0;
    s.dict_loaded = false;
    if (!EnsureEngine(s)) {
        Invalidate(s, window);
        return;
    }

    uint32_t offset = 0;
    for (;;) {
        UserDictReply page;
        if (!IpcClient::Instance().UserDictList(offset, 5000, &page)) {
            s.dict_status = L"无法连接输入法引擎";
            s.dict_status_ok = false;
            Invalidate(s, window);
            return;
        }
        if (page.status != PIME_OK) {
            s.dict_status = DictStatusText(page.status);
            s.dict_status_ok = false;
            Invalidate(s, window);
            return;
        }
        for (const UserWord& word : page.words) {
            DictRow row;
            row.word = ToWide(word.word);
            row.code = ToWide(DisplayUserCode(word.code));
            row.weight = word.weight;
            row.code_raw = word.code;
            row.word_raw = word.word;
            s.dict_rows.push_back(std::move(row));
        }
        offset += static_cast<uint32_t>(page.words.size());
        if (page.words.empty() || offset >= page.total) break;
    }

    s.dict_loaded = true;
    const std::wstring count =
        L"共 " + std::to_wstring(s.dict_rows.size()) + L" 条";
    s.dict_status = note.empty() ? count : note + L"（" + count + L"）";
    s.dict_status_ok = true;
    Invalidate(s, window);
}

struct DictEntry {
    std::wstring code;  // display form, syllables separated by spaces
    std::wstring word;
    uint32_t weight = 100;
    UserWord value;  // normalized result, valid when the dialog returns IDOK
};

INT_PTR CALLBACK DictEntryProc(HWND dialog, UINT message, WPARAM wparam,
                               LPARAM lparam) {
    auto* entry = reinterpret_cast<DictEntry*>(
        GetWindowLongPtrW(dialog, GWLP_USERDATA));
    if (message == WM_INITDIALOG) {
        entry = reinterpret_cast<DictEntry*>(lparam);
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lparam);
        SetDlgItemTextW(dialog, IDC_DICT_CODE, entry->code.c_str());
        SetDlgItemTextW(dialog, IDC_DICT_WORD, entry->word.c_str());
        SetDlgItemTextW(dialog, IDC_DICT_WEIGHT,
                        std::to_wstring(entry->weight).c_str());
        SendDlgItemMessageW(dialog, IDC_DICT_CODE, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dialog, IDC_DICT_CODE));
        return FALSE;
    }
    if (message != WM_COMMAND) return FALSE;
    if (LOWORD(wparam) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (LOWORD(wparam) != IDOK) return FALSE;

    wchar_t code[256] = {};
    wchar_t word[1024] = {};
    wchar_t weight[32] = {};
    GetDlgItemTextW(dialog, IDC_DICT_CODE, code, 256);
    GetDlgItemTextW(dialog, IDC_DICT_WORD, word, 1024);
    GetDlgItemTextW(dialog, IDC_DICT_WEIGHT, weight, 32);

    UserWord value;
    if (!NormalizeUserCode(ToUtf8(code), &value.code)) {
        MessageBoxW(dialog,
                    L"拼音码不能为空，只能包含字母和撇号（音节之间可加空格）。",
                    L"词条无效", MB_ICONWARNING);
        return TRUE;
    }
    value.word = ToUtf8(word);
    value.weight = static_cast<uint32_t>(std::wcstoull(weight, nullptr, 10));
    std::string error;
    if (!ValidateUserWord(value, &error)) {
        MessageBoxW(dialog, ToWide(error).c_str(), L"词条无效",
                    MB_ICONWARNING);
        return TRUE;
    }
    entry->value = std::move(value);
    EndDialog(dialog, IDOK);
    return TRUE;
}

bool ShowDictEntryDialog(HWND window, DictEntry* entry) {
    const INT_PTR result =
        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_DICT_ENTRY),
                        window, DictEntryProc, reinterpret_cast<LPARAM>(entry));
    WriteLog(LogLevel::Info, "dict",
             "dialog result " + std::to_string(result) + " err " +
                 std::to_string(GetLastError()));
    return result == IDOK;
}

bool ApplyDictWord(AppState& s, const UserWord& word, std::wstring* message) {
    int status = PIME_OK;
    if (!IpcClient::Instance().UserDictSet({word}, &status)) {
        *message = L"无法连接输入法引擎";
        s.dict_status_ok = false;
        return false;
    }
    if (status != PIME_OK) {
        *message = DictStatusText(status);
        s.dict_status_ok = false;
        return false;
    }
    return true;
}

void AddDictEntry(AppState& s, HWND window) {
    DictEntry entry;
    if (!ShowDictEntryDialog(window, &entry)) return;
    std::wstring message = L"已添加「" + ToWide(entry.value.word) + L"」";
    ApplyDictWord(s, entry.value, &message);
    LoadDict(s, window, message);
}

void EditDictEntry(AppState& s, HWND window) {
    if (s.dict_selected < 0 ||
        s.dict_selected >= static_cast<int>(s.dict_rows.size())) {
        s.dict_status = L"请先在列表中选择一条词条";
        s.dict_status_ok = false;
        Invalidate(s, window);
        return;
    }
    const DictRow row = s.dict_rows[s.dict_selected];
    DictEntry entry;
    entry.code = row.code;
    entry.word = row.word;
    entry.weight = row.weight;
    if (!ShowDictEntryDialog(window, &entry)) return;

    std::wstring message = L"已保存词条";
    const bool applied = ApplyDictWord(s, entry.value, &message);
    // The key encodes code and word, so a changed pair is a new entry and the
    // old one has to go. Write first: a failed delete only leaves a duplicate.
    if (applied && (entry.value.code != row.code_raw ||
                    entry.value.word != row.word_raw)) {
        int removed = PIME_OK;
        if (!IpcClient::Instance().UserDictDelete(row.code_raw, row.word_raw,
                                                  &removed)) {
            message = L"无法连接输入法引擎";
        } else if (removed != PIME_OK && removed != PIME_NOT_FOUND) {
            message = DictStatusText(removed);
        }
    }
    LoadDict(s, window, message);
}

void DeleteDictEntry(AppState& s, HWND window) {
    if (s.dict_selected < 0 ||
        s.dict_selected >= static_cast<int>(s.dict_rows.size())) {
        s.dict_status = L"请先在列表中选择一条词条";
        s.dict_status_ok = false;
        Invalidate(s, window);
        return;
    }
    const DictRow row = s.dict_rows[s.dict_selected];
    const std::wstring question = L"删除词条「" + row.word + L"」？";
    if (MessageBoxW(window, question.c_str(), L"删除词条",
                    MB_ICONQUESTION | MB_YESNO) != IDYES) {
        return;
    }
    int status = PIME_OK;
    std::wstring message = L"已删除词条";
    if (!IpcClient::Instance().UserDictDelete(row.code_raw, row.word_raw,
                                              &status)) {
        message = L"无法连接输入法引擎";
    } else if (status != PIME_OK && status != PIME_NOT_FOUND) {
        message = DictStatusText(status);
    }
    LoadDict(s, window, message);
}

void ClearDict(AppState& s, HWND window) {
    if (MessageBoxW(window,
                    L"清空整个词库？这也会删除自动学习到的词，且无法撤销。",
                    L"清空词库", MB_ICONWARNING | MB_YESNO) != IDYES) {
        return;
    }
    int status = PIME_OK;
    std::wstring message = L"词库已清空";
    if (!IpcClient::Instance().UserDictClear(&status)) {
        message = L"无法连接输入法引擎";
    } else if (status != PIME_OK) {
        message = DictStatusText(status);
    }
    LoadDict(s, window, message);
}

void ImportDict(AppState& s, HWND window) {
    if (!EnsureEngine(s)) {
        Invalidate(s, window);
        return;
    }
    wchar_t path[1024] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter =
        L"Rime 词库 (*.yaml;*.txt)\0*.yaml;*.txt\0所有文件 (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 1024;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"导入词库";
    if (!GetOpenFileNameW(&dialog)) return;

    const RimeDictReadResult read = ReadRimeDict(path);
    if (!read.ok) {
        s.dict_status = L"导入失败：" + ToWide(read.error);
        s.dict_status_ok = false;
        Invalidate(s, window);
        return;
    }
    if (read.words.empty()) {
        s.dict_status = L"文件中没有可导入的词条";
        s.dict_status_ok = false;
        Invalidate(s, window);
        return;
    }

    constexpr size_t kBatch = 500;
    uint32_t imported = 0;
    int status = PIME_OK;
    for (size_t start = 0; start < read.words.size(); start += kBatch) {
        const size_t end = std::min(read.words.size(), start + kBatch);
        const std::vector<UserWord> batch(read.words.begin() + start,
                                          read.words.begin() + end);
        if (!IpcClient::Instance().UserDictSet(batch, &status)) {
            status = PIME_CLOSED;
            break;
        }
        if (status != PIME_OK) break;
        imported += static_cast<uint32_t>(batch.size());
    }

    std::wstring message =
        L"已导入 " + std::to_wstring(imported) + L" 条";
    if (read.skipped) {
        message += L"，跳过 " + std::to_wstring(read.skipped) + L" 条";
    }
    if (status != PIME_OK) message += L"；" + DictStatusText(status);
    LoadDict(s, window, message);
}

void ExportDict(AppState& s, HWND window) {
    if (!s.dict_loaded) {
        s.dict_status = L"词库尚未载入，请先刷新";
        s.dict_status_ok = false;
        Invalidate(s, window);
        return;
    }
    wchar_t path[1024] = {};
    lstrcpynW(path, L"pime_user.dict.yaml", 1024);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"Rime 词库 (*.dict.yaml)\0*.dict.yaml\0文本文件 (*.txt)\0*.txt\0";
    dialog.lpstrDefExt = L"yaml";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 1024;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"导出词库";
    if (!GetSaveFileNameW(&dialog)) return;

    std::wstring name = path;
    std::transform(name.begin(), name.end(), name.begin(), towlower);
    const bool plain =
        name.size() > 4 && name.substr(name.size() - 4) == L".txt";

    std::vector<UserWord> words;
    words.reserve(s.dict_rows.size());
    for (const DictRow& row : s.dict_rows) {
        words.push_back(UserWord{row.code_raw, row.word_raw, row.weight});
    }
    std::string error;
    if (!WriteRimeDict(path, words, plain, &error)) {
        s.dict_status = L"导出失败：" + ToWide(error);
        s.dict_status_ok = false;
        Invalidate(s, window);
        return;
    }
    s.dict_status = L"已导出 " + std::to_wstring(words.size()) + L" 条";
    s.dict_status_ok = true;
    Invalidate(s, window);
}

// Copies a scheme package the user picked into %LOCALAPPDATA%\PIME\schemes.
// The package directory must contain scheme.yaml; its own chain and plugins
// come along, so the import is one directory copy.
void ImportScheme(AppState& s, HWND window) {
    BROWSEINFOW browse{};
    browse.hwndOwner = window;
    browse.lpszTitle = L"选择方案包目录（含 scheme.yaml）";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item) return;
    wchar_t path[MAX_PATH] = {};
    const bool picked = SHGetPathFromIDListW(item, path) != FALSE;
    CoTaskMemFree(item);
    if (!picked) return;

    SchemeInfo info;
    std::string error;
    if (!ReadSchemeFile(PathJoin(path, L"scheme.yaml"), &info, &error)) {
        s.plugin_status = L"导入失败：目录里没有有效的 scheme.yaml";
        s.plugin_status_ok = false;
        Invalidate(s, window);
        return;
    }

    const std::wstring id = ToWide(info.id);
    const std::wstring target =
        PathJoin(PathJoin(DataDirectory(), L"schemes"), id);
    if (DirectoryExists(target)) {
        const std::wstring question =
            L"方案包「" + SchemeName(s, id) + L"」已存在，覆盖？";
        if (MessageBoxW(window, question.c_str(), L"导入方案包",
                        MB_ICONQUESTION | MB_YESNO) != IDYES) {
            return;
        }
        DeleteDirectory(target);
    }
    if (!EnsureDirectory(target) || !CopyDirectory(path, target)) {
        s.plugin_status = L"导入失败：无法复制方案包";
        s.plugin_status_ok = false;
        Invalidate(s, window);
        return;
    }

    s.schemes = ListSchemes();
    s.settings.chain = id + L".cbor";
    s.plugin_dirty = false;
    LoadEditorFromScheme(s, id);
    s.plugin_status = L"已导入方案包「" + SchemeName(s, id) + L"」，保存后生效";
    s.plugin_status_ok = true;
    Invalidate(s, window);
}

void ActivateById(AppState& s, HWND window, int id) {
    switch (id) {
        case kImportScheme:
            ImportScheme(s, window);
            return;
        case kNavGeneral:
        case kNavAppearance:
        case kNavPlugins:
        case kNavDictionary:
        case kNavAbout:
            s.page = static_cast<Page>(id - kNavGeneral);
            s.menu = Menu::None;
            s.focus = kNone;
            if (s.page == Page::Dictionary && !s.dict_loaded) {
                LoadDict(s, window);
                return;
            }
            if (s.page == Page::Plugins && !s.plugins_loaded) {
                LoadPluginsEditor(s);
            }
            Invalidate(s, window);
            return;
        case kDictAdd:
            AddDictEntry(s, window);
            return;
        case kDictEdit:
            EditDictEntry(s, window);
            return;
        case kDictDelete:
            DeleteDictEntry(s, window);
            return;
        case kDictImport:
            ImportDict(s, window);
            return;
        case kDictExport:
            ExportDict(s, window);
            return;
        case kDictClear:
            ClearDict(s, window);
            return;
        case kDictRefresh:
            LoadDict(s, window);
            return;
        case kChain:
            s.menu = s.menu == Menu::Chain ? Menu::None : Menu::Chain;
            Invalidate(s, window);
            return;
        case kPageSize:
            s.menu = s.menu == Menu::PageSize ? Menu::None : Menu::PageSize;
            Invalidate(s, window);
            return;
        case kPunctFull:
            s.settings.punctuation = 1;
            Invalidate(s, window);
            return;
        case kPunctHalf:
            s.settings.punctuation = 0;
            Invalidate(s, window);
            return;
        case kThemeSystem:
        case kThemeLight:
        case kThemeDark:
            s.settings.theme = static_cast<uint32_t>(id - kThemeSystem);
            Invalidate(s, window);
            return;
        case kOrientH:
        case kOrientV:
            s.settings.candidate_vertical = id == kOrientV ? 1 : 0;
            Invalidate(s, window);
            return;
        case kSizeSmall:
        case kSizeMid:
        case kSizeLarge:
            s.settings.candidate_font =
                id == kSizeSmall ? 13 : (id == kSizeMid ? 16 : 19);
            Invalidate(s, window);
            return;
        case kToggle: {
            s.settings.chinese = s.settings.chinese ? 0 : 1;
            s.toggle.from = s.toggle.value;
            s.toggle.to = s.settings.chinese ? 1.f : 0.f;
            s.toggle.start = GetTickCount64();
            s.toggle.running = true;
            SetTimer(window, 1, 16, nullptr);
            Invalidate(s, window);
            return;
        }
        case kApply:
            ApplySettings(s, window);
            return;
        case kClose:
            DestroyWindow(window);
            return;
        case kInstall:
            s.status = L"已请求安装，请在弹出的窗口中确认。";
            s.status_ok = true;
            RunElevated(L"--install");
            Invalidate(s, window);
            return;
        case kUninstall:
            s.status = L"已请求卸载，请在弹出的窗口中确认。";
            s.status_ok = true;
            RunElevated(L"--uninstall");
            Invalidate(s, window);
            return;
        case kActivate:
            if (ActivateTextService()) {
                s.status = L"已切换到 PIME，可以开始输入。";
                s.status_ok = true;
            } else {
                s.status = L"切换失败：请先安装输入法。";
                s.status_ok = false;
            }
            Invalidate(s, window);
            return;
        default:
            return;
    }
}

// Keeps the card being opened in view. A card taller than the viewport keeps
// its header at the top rather than scrolling past it.
void EnsurePluginVisible(AppState& s, int plugin) {
    BuildLayout(s);
    const Box& view = s.layout.plug_view;
    for (const PlugRow& row : s.layout.plug_rows) {
        if (row.plugin != plugin) continue;
        if (row.card.H() >= view.H()) {
            s.plugin_scroll += row.card.t - view.t;
        } else if (row.card.b > view.b) {
            s.plugin_scroll += row.card.b - view.b;
        } else if (row.card.t < view.t) {
            s.plugin_scroll -= view.t - row.card.t;
        }
        BuildLayout(s);
        return;
    }
}

void SelectPluginRow(AppState& s, HWND window, int plugin) {
    s.plugin_selected = s.plugin_selected == plugin ? -1 : plugin;
    s.plugin_scroll = std::max(0.f, s.plugin_scroll);
    EnsurePluginVisible(s, plugin);
    InvalidateRect(window, nullptr, FALSE);
}

void Click(AppState& s, HWND window, float x, float y) {
    if (s.menu != Menu::None) {
        const int hit = HitTest(s, x, y);
        if (hit >= kMenuItem) {
            const int index = hit - kMenuItem;
            if (s.menu == Menu::Chain &&
                index < static_cast<int>(s.schemes.size())) {
                s.settings.chain = ToWide(s.schemes[index].id) + L".cbor";
                // The plugin editor holds a snapshot of the old chain; force a
                // reload so Apply cannot write it back over this choice.
                s.plugins_loaded = false;
                s.edit_plugins.clear();
                s.plugin_selected = -1;
                s.plugin_dirty = false;
                s.plugin_chain.clear();
                s.plugin_choice.clear();
            } else if (s.menu == Menu::PageSize && index < 7) {
                s.settings.page_size = static_cast<uint32_t>(index + 3);
            }
        }
        s.menu = Menu::None;
        Invalidate(s, window);
        return;
    }
    const int hit = HitTest(s, x, y);
    if (hit == kNone) return;
    if (s.page == Page::Dictionary && hit >= kDictRow) {
        s.dict_selected = hit - kDictRow;
        s.focus = kNone;
        Invalidate(s, window);
        return;
    }
    if (s.page == Page::Plugins) {
        if (hit >= kPreset && hit < kPreset + s.layout.preset_count) {
            s.focus = hit;
            SelectPreset(s, window, hit - kPreset);
            return;
        }
        if (hit == kRestore) {
            s.focus = hit;
            RestorePreset(s, window);
            return;
        }
        if (hit >= kPlugCard && hit < kPlugToggle) {
            s.focus = hit;
            SelectPluginRow(s, window, hit - kPlugCard);
            return;
        }
        if (hit >= kPlugToggle && hit < kPlugSetting) {
            TogglePlugin(s, hit - kPlugToggle);
            s.focus = hit;
            Invalidate(s, window);
            return;
        }
        if (hit >= kPlugSetting) {
            const int plugin = (hit - kPlugSetting) / 32;
            const int spec = (hit - kPlugSetting) % 32;
            if (plugin < static_cast<int>(s.edit_plugins.size()) &&
                s.edit_plugins[plugin].meta >= 0) {
                const PluginMeta& meta = s.catalog[s.edit_plugins[plugin].meta];
                if (spec < static_cast<int>(meta.settings.size()) &&
                    meta.settings[spec].type == "int") {
                    DragPluginSetting(s, hit, x);
                    s.plugin_drag = hit;
                    SetCapture(window);
                } else {
                    StepPluginSetting(s, hit, 1);
                }
            }
            s.focus = hit;
            Invalidate(s, window);
            return;
        }
    }
    s.focus = hit;
    s.focus_visible = false;
    ActivateById(s, window, hit);
}

std::vector<int> FocusOrder(const AppState& s) {
    std::vector<int> order = {kNavGeneral, kNavAppearance, kNavPlugins,
                              kNavDictionary, kNavAbout};
    if (s.page == Page::General) {
        order.push_back(kChain);
        order.push_back(kPageSize);
        order.push_back(kPunctFull);
        order.push_back(kPunctHalf);
        order.push_back(kToggle);
    } else if (s.page == Page::Appearance) {
        order.push_back(kThemeSystem);
        order.push_back(kThemeLight);
        order.push_back(kThemeDark);
        order.push_back(kOrientH);
        order.push_back(kOrientV);
        order.push_back(kSizeSmall);
        order.push_back(kSizeMid);
        order.push_back(kSizeLarge);
    } else if (s.page == Page::Plugins) {
        for (int i = 0; i < s.layout.preset_count; ++i) {
            order.push_back(kPreset + i);
        }
        if (s.plugin_dirty || s.plugin_custom) {
            order.push_back(kRestore);
        }
        order.push_back(kImportScheme);
        for (const PlugRow& row : s.layout.plug_rows) {
            order.push_back(kPlugCard + row.plugin);
            if (!s.edit_plugins[row.plugin].structural) {
                order.push_back(kPlugToggle + row.plugin);
            }
            for (const PlugControl& control : row.controls) {
                order.push_back(kPlugSetting + row.plugin * 32 +
                                control.spec);
            }
        }
    } else if (s.page == Page::Dictionary) {
        for (int id = kDictAdd; id <= kDictRefresh; ++id) order.push_back(id);
    } else {
        order.push_back(kInstall);
        order.push_back(kUninstall);
        order.push_back(kActivate);
    }
    order.push_back(kApply);
    order.push_back(kClose);
    return order;
}

// ---- window plumbing -------------------------------------------------------

bool EnsureTarget(AppState& s) {
    if (!s.d2d) return false;
    if (s.rt) return true;
    RECT rc{};
    GetClientRect(s.window, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = static_cast<float>(s.dpi);
    props.dpiY = static_cast<float>(s.dpi);
    if (FAILED(s.d2d->CreateHwndRenderTarget(
            props,
            D2D1::HwndRenderTargetProperties(
                s.window,
                D2D1::SizeU(static_cast<UINT32>(rc.right),
                            static_cast<UINT32>(rc.bottom))),
            s.rt.GetAddressOf()))) {
        return false;
    }
    s.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    s.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    return true;
}

void UpdateTheme(AppState& s, HWND window) {
    s.system_dark = !SystemUsesLightTheme();
    const BOOL dark = s.system_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam,
                         LPARAM lparam) {
    auto* s = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }
        case WM_CREATE: {
            auto* self = reinterpret_cast<AppState*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
            self->window = window;
            self->dpi = GetDpiForWindow(window);
            self->system_dark = !SystemUsesLightTheme();
            self->installed = IsInstalled();
            self->settings = LoadSettings();
            self->schemes = ListSchemes();
            // A saved optional scheme may no longer be installed; fall back to
            // the first installed package (pinyin) instead of showing nothing.
            if (!FindScheme(*self, ChainName(self->settings.chain)) &&
                !self->schemes.empty()) {
                self->settings.chain =
                    ToWide(self->schemes.front().id) + L".cbor";
            }
            self->saved = self->settings;
            self->toggle.value = self->settings.chinese ? 1.f : 0.f;

            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                              self->d2d.GetAddressOf());
            DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(self->dwrite.GetAddressOf()));
            CreateFormats(*self);

            RECT rc{};
            GetClientRect(window, &rc);
            self->width = rc.right * 96.f / static_cast<float>(self->dpi);
            self->height = rc.bottom * 96.f / static_cast<float>(self->dpi);
            BuildLayout(*self);
            const BOOL dark = self->system_dark ? TRUE : FALSE;
            DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            if (s && s->d2d && s->dwrite) {
                if (EnsureTarget(*s)) {
                    s->rt->BeginDraw();
                    Draw(*s);
                    const HRESULT hr = s->rt->EndDraw();
                    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
                        s->rt.Reset();
                    }
                }
            }
            EndPaint(window, &paint);
            return 0;
        }
        case WM_SIZE: {
            if (!s) break;
            s->width = LOWORD(lparam) * 96.f / static_cast<float>(s->dpi);
            s->height = HIWORD(lparam) * 96.f / static_cast<float>(s->dpi);
            if (s->rt) {
                s->rt->Resize(D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
            }
            BuildLayout(*s);
            return 0;
        }
        case WM_DPICHANGED: {
            if (!s) break;
            s->dpi = HIWORD(wparam);
            s->rt.Reset();
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            s->width = kWidth;
            s->height = kHeight;
            RECT rect{0, 0, static_cast<LONG>(kWidth * s->dpi / 96.f),
                      static_cast<LONG>(kHeight * s->dpi / 96.f)};
            AdjustWindowRectExForDpi(
                &rect,
                static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)),
                FALSE, 0, s->dpi);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            BuildLayout(*s);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!s) break;
            const float x = static_cast<float>(GET_X_LPARAM(lparam));
            const float y = static_cast<float>(GET_Y_LPARAM(lparam));
            s->mx = x * 96.f / static_cast<float>(s->dpi);
            s->my = y * 96.f / static_cast<float>(s->dpi);
            if (s->plugin_drag != kNone) {
                DragPluginSetting(*s, s->plugin_drag, s->mx);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (!s->mouse_in) {
                s->mouse_in = true;
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
                TrackMouseEvent(&track);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (s) {
                s->mouse_in = false;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            if (!s) break;
            const float x = static_cast<float>(GET_X_LPARAM(lparam));
            const float y = static_cast<float>(GET_Y_LPARAM(lparam));
            Click(*s, window, x * 96.f / static_cast<float>(s->dpi),
                  y * 96.f / static_cast<float>(s->dpi));
            return 0;
        }
        case WM_LBUTTONUP:
            if (s && s->plugin_drag != kNone) {
                s->plugin_drag = kNone;
                ReleaseCapture();
                return 0;
            }
            break;
        case WM_LBUTTONDBLCLK: {
            if (!s) break;
            const float x = static_cast<float>(GET_X_LPARAM(lparam));
            const float y = static_cast<float>(GET_Y_LPARAM(lparam));
            const float dip_x = x * 96.f / static_cast<float>(s->dpi);
            const float dip_y = y * 96.f / static_cast<float>(s->dpi);
            const int hit = HitTest(*s, dip_x, dip_y);
            if (hit >= kDictRow) {
                s->dict_selected = hit - kDictRow;
                EditDictEntry(*s, window);
                return 0;
            }
            Click(*s, window, dip_x, dip_y);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (!s) break;
            if (s->page == Page::Plugins) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(window, &point);
                const float px = point.x * 96.f / static_cast<float>(s->dpi);
                const float py = point.y * 96.f / static_cast<float>(s->dpi);
                if (!s->layout.plug_view.Contains(px, py)) break;
                const float step = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -56.f
                                                                      : 56.f;
                const float max_scroll =
                    std::max(0.f, s->layout.plug_content -
                                      s->layout.plug_view.H());
                s->plugin_scroll =
                    std::clamp(s->plugin_scroll + step, 0.f, max_scroll);
                Invalidate(*s, window);
                return 0;
            }
            if (s->page != Page::Dictionary) break;
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(window, &point);
            if (!s->layout.dict_list.Contains(
                    point.x * 96.f / static_cast<float>(s->dpi),
                    point.y * 96.f / static_cast<float>(s->dpi))) {
                break;
            }
            const int step = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -3 : 3;
            const int visible = DictVisibleRows(*s);
            const int max_scroll = std::max(
                0, static_cast<int>(s->dict_rows.size()) - visible);
            s->dict_scroll =
                std::clamp(s->dict_scroll + step, 0, max_scroll);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_SETCURSOR:
            if (s && LOWORD(lparam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(window, &point);
                const float x = point.x * 96.f / static_cast<float>(s->dpi);
                const float y = point.y * 96.f / static_cast<float>(s->dpi);
                SetCursor(LoadCursorW(nullptr, HitTest(*s, x, y) != kNone
                                                   ? IDC_HAND
                                                   : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_KEYDOWN: {
            if (!s) break;
            if (wparam == VK_ESCAPE) {
                if (s->menu != Menu::None) {
                    s->menu = Menu::None;
                } else {
                    s->focus = kNone;
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_TAB) {
                std::vector<int> order = FocusOrder(*s);
                int index = -1;
                for (size_t i = 0; i < order.size(); ++i) {
                    if (order[i] == s->focus) index = static_cast<int>(i);
                }
                const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const int count = static_cast<int>(order.size());
                index = back ? (index <= 0 ? count - 1 : index - 1)
                             : (index + 1) % count;
                s->focus = order[index];
                s->focus_visible = true;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (s->page == Page::Plugins &&
                (wparam == VK_LEFT || wparam == VK_RIGHT) &&
                s->focus >= kPlugSetting) {
                StepPluginSetting(*s, s->focus,
                                  wparam == VK_RIGHT ? 1 : -1);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if ((wparam == VK_RETURN || wparam == VK_SPACE) &&
                s->focus != kNone) {
                if (s->focus >= kPreset && s->focus < kRestore) {
                    SelectPreset(*s, window, s->focus - kPreset);
                    return 0;
                }
                if (s->focus == kRestore) {
                    RestorePreset(*s, window);
                    return 0;
                }
                if (s->focus >= kPlugCard && s->focus < kPlugToggle) {
                    SelectPluginRow(*s, window, s->focus - kPlugCard);
                    return 0;
                }
                if (s->focus >= kPlugToggle && s->focus < kPlugSetting) {
                    TogglePlugin(*s, s->focus - kPlugToggle);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                if (s->focus >= kPlugSetting) {
                    StepPluginSetting(*s, s->focus, 1);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                ActivateById(*s, window, s->focus);
                return 0;
            }
            break;
        }
        case WM_TIMER:
            if (s && wparam == 1) {
                const float progress = static_cast<float>(GetTickCount64() -
                                                          s->toggle.start) /
                                       120.f;
                if (progress >= 1.f) {
                    s->toggle.value = s->toggle.to;
                    s->toggle.running = false;
                    KillTimer(window, 1);
                } else {
                    const float eased = 1.f - (1.f - progress) * (1.f - progress) *
                                                  (1.f - progress);
                    s->toggle.value =
                        s->toggle.from + (s->toggle.to - s->toggle.from) * eased;
                }
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_ACTIVATE:
            if (s && LOWORD(wparam) != WA_INACTIVE) {
                s->installed = IsInstalled();
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (s && lparam &&
                lstrcmpiW(reinterpret_cast<const wchar_t*>(lparam),
                          L"ImmersiveColorSet") == 0) {
                UpdateTheme(*s, window);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(window, 1);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int RunSettingsApp(HINSTANCE instance) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_SHARED));
    wc.lpszClassName = kClassName;
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    AppState state;
    state.instance = instance;

    const DWORD style =
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME;
    const UINT dpi = GetDpiForSystem();
    RECT rect{0, 0, MulDiv(static_cast<int>(kWidth), dpi, 96),
              MulDiv(static_cast<int>(kHeight), dpi, 96)};
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);
    HWND window = CreateWindowExW(0, kClassName, kWindowTitle, style,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  rect.right - rect.left,
                                  rect.bottom - rect.top, nullptr, nullptr,
                                  instance, &state);
    if (!window) return 1;

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace pimewin
