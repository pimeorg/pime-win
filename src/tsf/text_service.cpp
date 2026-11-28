// TSF text service: owns the composition in the focused document and forwards
// keys to pime_engine. The candidate strip and the mode badge are drawn in the
// engine, so replacing that process updates the UI without restarting hosts.
#include "tsf/text_service.h"

#include <ctffunc.h>
#include <msctf.h>
#include <oleauto.h>
#include <olectl.h>
#include <shellapi.h>
#include <pime/hal.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/settings.h"
#include "common/win_util.h"
#include "tsf/ipc_client.h"
#include "tsf/display_attr.h"
#include "tsf/guids.h"
#include "tsf/resource.h"

using Microsoft::WRL::ComPtr;

namespace pimewin {
namespace {

// Bit 4 (HOST_CANDIDATES): the host renders the candidate strip itself.
constexpr uint32_t kHostCaps = 1u << 4;
constexpr wchar_t kNotifyClassName[] = L"PimeNotifyWindow";
constexpr DWORD kLangBarSinkCookie = 0x50494D45;  // 'PIME'
constexpr UINT kMenuSettings = 1;

class TextServiceImpl;

class EditSession final : public ITfEditSession {
public:
    explicit EditSession(std::function<void(TfEditCookie)> body)
        : body_(std::move(body)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG value = InterlockedDecrement(&ref_);
        if (value == 0) delete this;
        return static_cast<ULONG>(value);
    }
    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        body_(ec);
        return S_OK;
    }

private:
    ~EditSession() = default;
    LONG ref_ = 1;
    std::function<void(TfEditCookie)> body_;
};

bool ModifierDown(int vkey) { return (GetKeyState(vkey) & 0x8000) != 0; }

class TextServiceImpl final : public ITfTextInputProcessorEx,
                              public ITfThreadMgrEventSink,
                              public ITfThreadFocusSink,
                              public ITfKeyEventSink,
                              public ITfCompositionSink,
                              public ITfDisplayAttributeProvider,
                              public ITfLangBarItemButton,
                              public ITfSource,
                              public ITfCompartmentEventSink {
public:
    TextServiceImpl() = default;

    // ---- IUnknown -------------------------------------------------------
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ITfTextInputProcessor) ||
            IsEqualIID(riid, IID_ITfTextInputProcessorEx)) {
            *ppv = static_cast<ITfTextInputProcessorEx*>(this);
        } else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink)) {
            *ppv = static_cast<ITfThreadMgrEventSink*>(this);
        } else if (IsEqualIID(riid, IID_ITfThreadFocusSink)) {
            *ppv = static_cast<ITfThreadFocusSink*>(this);
        } else if (IsEqualIID(riid, IID_ITfKeyEventSink)) {
            *ppv = static_cast<ITfKeyEventSink*>(this);
        } else if (IsEqualIID(riid, IID_ITfCompositionSink)) {
            *ppv = static_cast<ITfCompositionSink*>(this);
        } else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider)) {
            *ppv = static_cast<ITfDisplayAttributeProvider*>(this);
        } else if (IsEqualIID(riid, IID_ITfLangBarItem) ||
                   IsEqualIID(riid, IID_ITfLangBarItemButton)) {
            *ppv = static_cast<ITfLangBarItemButton*>(this);
        } else if (IsEqualIID(riid, IID_ITfSource)) {
            *ppv = static_cast<ITfSource*>(this);
        } else if (IsEqualIID(riid, IID_ITfCompartmentEventSink)) {
            *ppv = static_cast<ITfCompartmentEventSink*>(this);
        }
        if (!*ppv) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG value = InterlockedDecrement(&ref_);
        if (value == 0) delete this;
        return static_cast<ULONG>(value);
    }

    // ---- ITfTextInputProcessor -----------------------------------------
    STDMETHODIMP Activate(ITfThreadMgr* thread_mgr, TfClientId client_id) override {
        return ActivateEx(thread_mgr, client_id, 0);
    }
    STDMETHODIMP ActivateEx(ITfThreadMgr* thread_mgr, TfClientId client_id,
                            DWORD flags) override {
        (void)flags;
        if (!thread_mgr) return E_INVALIDARG;
        if (thread_mgr_) return S_OK;
        // The shell now runs in a host process of its own, so give it a log
        // file next to the engine's.
        InitLog(DataDirectory());
        thread_mgr_ = thread_mgr;
        client_id_ = client_id;

        ComPtr<ITfSource> source;
        if (SUCCEEDED(thread_mgr_.As(&source))) {
            source->AdviseSink(IID_ITfThreadMgrEventSink,
                               static_cast<ITfThreadMgrEventSink*>(this),
                               &thread_event_cookie_);
            source->AdviseSink(IID_ITfThreadFocusSink,
                               static_cast<ITfThreadFocusSink*>(this),
                               &thread_focus_cookie_);
        }
        if (SUCCEEDED(thread_mgr_.As(&keys_)) && keys_) {
            keys_->AdviseKeyEventSink(client_id_,
                                      static_cast<ITfKeyEventSink*>(this), TRUE);
            key_advised_ = true;
        }

        settings_ = LoadSettings();
        chinese_ = settings_.chinese != 0;

        // Publish the mode through TSF's keyboard open/close compartment so
        // the input indicator, Ctrl+Space and IME-aware apps see the same
        // 中/英 state as the engine.
        ComPtr<ITfCompartmentMgr> compartments;
        if (SUCCEEDED(thread_mgr_.As(&compartments)) &&
            SUCCEEDED(compartments->GetCompartment(
                GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                open_close_.ReleaseAndGetAddressOf()))) {
            ComPtr<ITfSource> open_close_source;
            if (SUCCEEDED(open_close_.As(&open_close_source))) {
                open_close_source->AdviseSink(
                    IID_ITfCompartmentEventSink,
                    static_cast<ITfCompartmentEventSink*>(this),
                    &open_close_cookie_);
            }
            SetOpenClose(chinese_);
        }

        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&CreatePimeTextService),
                           &module);
        // Staging keeps pime_engine.exe next to pime_tsf.dll.
        const std::wstring engine_exe = PathJoin(ModuleDirectory(module), L"pime_engine.exe");
        engine_ready_ = IpcClient::Instance().Start(engine_exe);
        if (!engine_ready_) {
            WriteLog(LogLevel::Error, "tsf",
                     "pime_engine failed to start from " + ToUtf8(engine_exe));
        } else {
            // Take the settings from the engine: HKCU reads from a packaged
            // host (Notepad and friends) see a virtualized, empty copy, which
            // would leave the shell stuck in English mode.
            settings_ = IpcClient::Instance().settings();
            chinese_ = settings_.chinese != 0;
            SetOpenClose(chinese_);
            WriteLog(LogLevel::Info, "tsf",
                     "mode from engine: chinese=" + std::to_string(settings_.chinese) +
                         " page=" + std::to_string(settings_.page_size));
        }

        CreateNotifyWindow();
        minimize_owner_ = this;
        if (!minimize_hook_) {
            minimize_hook_ = SetWinEventHook(
                EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr,
                &TextServiceImpl::MinimizeEventProc, 0, GetCurrentThreadId(),
                WINEVENT_OUTOFCONTEXT);
        }

        // Windows 8+ shows the 中/英 mode of the focused text service in the
        // taskbar input indicator, but only for the item whose GUID is
        // GUID_LBI_INPUTMODE.
        ComPtr<ITfLangBarItemMgr> langbar;
        if (SUCCEEDED(thread_mgr_.As(&langbar)) &&
            SUCCEEDED(langbar->AddItem(static_cast<ITfLangBarItemButton*>(this)))) {
            langbar_mgr_ = langbar;
        } else {
            WriteLog(LogLevel::Warn, "tsf",
                     "language bar item unavailable");
        }

        ComPtr<ITfDocumentMgr> focused;
        if (SUCCEEDED(thread_mgr_->GetFocus(&focused)) && focused) {
            OnSetFocus(focused.Get(), nullptr);
        }
        WriteLog(LogLevel::Info, "tsf", "activated");
        return S_OK;
    }
    STDMETHODIMP Deactivate() override {
        if (!thread_mgr_) return S_OK;
        LeaveDocument();
        if (minimize_hook_) {
            UnhookWinEvent(minimize_hook_);
            minimize_hook_ = nullptr;
        }
        if (minimize_owner_ == this) minimize_owner_ = nullptr;
        if (notify_hwnd_) {
            DestroyWindow(notify_hwnd_);
            notify_hwnd_ = nullptr;
        }
        if (keys_) {
            if (key_advised_) keys_->UnadviseKeyEventSink(client_id_);
            keys_.Reset();
            key_advised_ = false;
        }
        ComPtr<ITfSource> source;
        if (SUCCEEDED(thread_mgr_.As(&source))) {
            if (thread_event_cookie_) {
                source->UnadviseSink(thread_event_cookie_);
                thread_event_cookie_ = 0;
            }
            if (thread_focus_cookie_) {
                source->UnadviseSink(thread_focus_cookie_);
                thread_focus_cookie_ = 0;
            }
        }
        if (langbar_mgr_) {
            langbar_mgr_->RemoveItem(static_cast<ITfLangBarItemButton*>(this));
            langbar_mgr_.Reset();
        }
        langbar_sink_.Reset();
        if (open_close_) {
            ComPtr<ITfSource> open_close_source;
            if (open_close_cookie_ &&
                SUCCEEDED(open_close_.As(&open_close_source))) {
                open_close_source->UnadviseSink(open_close_cookie_);
            }
            open_close_cookie_ = 0;
            open_close_.Reset();
        }
        thread_mgr_.Reset();
        client_id_ = TF_CLIENTID_NULL;
        shift_down_ = shift_used_ = false;
        WriteLog(LogLevel::Info, "tsf", "deactivated");
        return S_OK;
    }

    // ---- ITfLangBarItemButton -------------------------------------------
    // The language bar reaches the item through ITfSource: it advises an
    // ITfLangBarItemSink here and we push OnUpdate through it when the mode
    // changes. The taskbar input indicator displays whichever item reports
    // GUID_LBI_INPUTMODE, so on Windows 8+ this is also the 中/英 button next
    // to the tray.
    STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* info) override {
        if (!info) return E_INVALIDARG;
        *info = TF_LANGBARITEMINFO{};
        info->clsidService = kClsidTextService;
        info->guidItem = GUID_LBI_INPUTMODE;
        info->dwStyle = TF_LBI_STYLE_BTN_BUTTON;
        info->ulSort = 0;
        lstrcpynW(info->szDescription, L"PIME 输入模式", TF_LBI_DESC_MAXLEN);
        return S_OK;
    }
    STDMETHODIMP GetStatus(DWORD* status) override {
        if (!status) return E_INVALIDARG;
        *status = 0;
        return S_OK;
    }
    STDMETHODIMP GetTooltipString(BSTR* text) override {
        if (!text) return E_INVALIDARG;
        *text = SysAllocString(chinese_ ? L"PIME 中文输入" : L"PIME 英文输入");
        return *text ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP Show(BOOL show) override {
        (void)show;
        return S_OK;
    }
    STDMETHODIMP GetText(BSTR* text) override {
        if (!text) return E_INVALIDARG;
        *text = SysAllocString(chinese_ ? L"中" : L"英");
        return *text ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP GetIcon(HICON* icon) override {
        if (!icon) return E_INVALIDARG;
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&CreatePimeTextService),
                           &module);
        // The language bar destroys the icon it receives, so this must be a
        // fresh load rather than LR_SHARED. Prefer the icon beside the DLL
        // (replaceable without touching this module) and fall back to the
        // embedded copy when an older install has none.
        const std::wstring path = PathJoin(
            ModuleDirectory(module), chinese_ ? L"mode_zh.ico" : L"mode_en.ico");
        *icon = static_cast<HICON>(LoadImageW(
            nullptr, path.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
        if (!*icon) {
            *icon = static_cast<HICON>(LoadImageW(
                module, MAKEINTRESOURCEW(chinese_ ? IDI_MODE_ZH : IDI_MODE_EN),
                IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                GetSystemMetrics(SM_CYSMICON), 0));
        }
        return *icon ? S_OK : E_FAIL;
    }
    STDMETHODIMP OnClick(TfLBIClick click, POINT point,
                         const RECT* area) override {
        (void)area;
        if (click == TF_LBI_CLK_LEFT) ToggleChinese();
        else if (click == TF_LBI_CLK_RIGHT) ShowContextMenu(point);
        return S_OK;
    }
    STDMETHODIMP InitMenu(ITfMenu*) override { return E_NOTIMPL; }
    STDMETHODIMP OnMenuSelect(UINT) override { return E_NOTIMPL; }

    // ---- ITfSource (language bar sink connection) -----------------------
    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* unknown,
                            DWORD* cookie) override {
        if (!cookie) return E_INVALIDARG;
        if (!IsEqualIID(riid, IID_ITfLangBarItemSink))
            return CONNECT_E_CANNOTCONNECT;
        if (langbar_sink_) return CONNECT_E_ADVISELIMIT;
        if (!unknown ||
            FAILED(unknown->QueryInterface(
                IID_ITfLangBarItemSink,
                reinterpret_cast<void**>(langbar_sink_.GetAddressOf())))) {
            return E_NOINTERFACE;
        }
        *cookie = kLangBarSinkCookie;
        return S_OK;
    }
    STDMETHODIMP UnadviseSink(DWORD cookie) override {
        if (cookie != kLangBarSinkCookie || !langbar_sink_)
            return CONNECT_E_NOCONNECTION;
        langbar_sink_.Reset();
        return S_OK;
    }

    void UpdateLangBar() {
        if (langbar_sink_) {
            langbar_sink_->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT |
                                    TF_LBI_TOOLTIP);
        }
    }

    // ---- mode synchronization with TSF --------------------------------
    void SetOpenClose(bool open) {
        if (!open_close_) return;
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_I4;
        value.lVal = open ? 1 : 0;
        open_close_->SetValue(client_id_, &value);
    }

    STDMETHODIMP OnChange(REFGUID guid) override {
        // Ctrl+Space, the touch keyboard and IME-aware apps toggle the mode
        // through this compartment; follow them instead of keeping a private
        // idea of 中/英. SetValue must not be called from here.
        if (!open_close_ ||
            !IsEqualGUID(guid, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE)) {
            return S_OK;
        }
        VARIANT value;
        VariantInit(&value);
        if (FAILED(open_close_->GetValue(&value))) return S_OK;
        const bool open = value.vt == VT_I4 && value.lVal != 0;
        VariantClear(&value);
        if (open != chinese_) ApplyMode(open);
        return S_OK;
    }

    // ---- host window minimize --------------------------------------------
    static void CALLBACK MinimizeEventProc(HWINEVENTHOOK, DWORD event, HWND,
                                           LONG, LONG, DWORD, DWORD) {
        if (event != EVENT_SYSTEM_MINIMIZESTART) return;
        // Minimizing does not move TSF focus, so no KillFocus arrives; without
        // this the topmost candidate strip keeps floating over other windows.
        if (minimize_owner_) minimize_owner_->OnHostMinimized();
    }

    void OnHostMinimized() {
        // Only hide the strip: cancelling the composition needs an edit
        // session the host can refuse while the window is being minimized.
        if (session_) IpcClient::Instance().HideCandidates(session_);
    }

    // ---- ITfThreadMgrEventSink ------------------------------------------
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* focus,
                            ITfDocumentMgr* previous) override {
        (void)previous;
        if (focus == document_.Get()) return S_OK;
        LeaveDocument();
        if (focus) {
            document_ = focus;
            ComPtr<ITfContext> context;
            if (SUCCEEDED(document_->GetTop(&context)) && context) {
                context_ = context;
                StartSession();
            }
        }
        return S_OK;
    }
    STDMETHODIMP OnPushContext(ITfContext*) override { return S_OK; }
    STDMETHODIMP OnPopContext(ITfContext*) override { return S_OK; }

    // ---- ITfThreadFocusSink ---------------------------------------------
    STDMETHODIMP OnSetThreadFocus() override {
        shift_down_ = shift_used_ = false;
        if (!engine_ready_) return S_OK;
        if (!session_ && context_) StartSession();
        else if (session_) IpcClient::Instance().Focus(session_, true);
        return S_OK;
    }
    STDMETHODIMP OnKillThreadFocus() override {
        shift_down_ = shift_used_ = false;
        if (composition_) CancelComposition();
        if (session_) {
            IpcClient::Instance().HideCandidates(session_);
            IpcClient::Instance().Focus(session_, false);
        }
        return S_OK;
    }

    // ---- ITfKeyEventSink ------------------------------------------------
    STDMETHODIMP OnSetFocus(BOOL) override { return S_OK; }
    STDMETHODIMP OnTestKeyDown(ITfContext*, WPARAM vkey, LPARAM lparam,
                               BOOL* eaten) override {
        if (!eaten) return E_INVALIDARG;
        // Runs for every key, including ones we do not claim: Shift + letter
        // must type an uppercase letter, not toggle the input mode.
        if (vkey != VK_SHIFT && shift_down_) shift_used_ = true;
        *eaten = ShouldClaim(vkey, lparam) ? TRUE : FALSE;
        return S_OK;
    }
    STDMETHODIMP OnTestKeyUp(ITfContext*, WPARAM vkey, LPARAM,
                             BOOL* eaten) override {
        if (!eaten) return E_INVALIDARG;
        // Key-up events only reach OnKeyUp when claimed here; the Shift tap
        // toggle lives in OnKeyUp, so claim the Shift release we saw pressed.
        *eaten = (vkey == VK_SHIFT && shift_down_) ? TRUE : FALSE;
        return S_OK;
    }
    STDMETHODIMP OnKeyDown(ITfContext*, WPARAM vkey, LPARAM lparam,
                           BOOL* eaten) override {
        if (!eaten) return E_INVALIDARG;
        *eaten = FALSE;
        if (vkey == VK_SHIFT) {
            shift_down_ = true;
            shift_used_ = false;
            return S_OK;
        }
        if (shift_down_) shift_used_ = true;
        if (!ShouldClaim(vkey, lparam) || !session_) return S_OK;

        // A mark typed while composing commits the selected candidate first,
        // then goes in as typed; while idle it is inserted directly. The
        // setting chooses full-width Chinese marks or ASCII. v-mode
        // expressions ("v1.5") keep their ASCII operators and single quotes
        // stay syllable separators ("xi'an").
        const std::string ascii = KeyText(vkey, lparam);
        const bool full_width = settings_.punctuation != 0;
        std::wstring punctuation;
        if (const wchar_t* mark = FullWidthMark(ascii)) {
            punctuation = full_width ? mark : ToWide(ascii);
        } else if (composition_text_.empty() && ascii == "'") {
            // Curly quotes alternate open/close, like every desktop IME. The
            // apostrophe is a syllable separator inside a preedit, so it only
            // becomes a quote while idle.
            punctuation = full_width ? (single_quote_open_ ? L"‘" : L"’") : L"'";
            if (full_width) single_quote_open_ = !single_quote_open_;
        } else if (ascii == "\"") {
            punctuation = full_width ? (double_quote_open_ ? L"“" : L"”") : L"\"";
            if (full_width) double_quote_open_ = !double_quote_open_;
        } else if (!composition_text_.empty() && IsPlainMark(ascii)) {
            // The chain has no use for this symbol in a pinyin buffer, so end
            // the preedit and type it; leaving it to the engine drops the key.
            punctuation = ToWide(ascii);
        }
        const bool v_mode =
            !composition_text_.empty() &&
            (composition_text_[0] == L'v' || composition_text_[0] == L'V');
        if (!punctuation.empty() && !(v_mode && IsVModeOperator(ascii))) {
            if (composition_ || candidates_up_) {
                KeyEvent space;
                space.keycode = VK_SPACE;
                space.action = PIME_KEY_DOWN;
                ApplyCommands(IpcClient::Instance().Key(session_, space).commands);
            }
            if (composition_ || candidates_up_) {
                // The space found nothing to commit; the mark still ends the
                // preedit, so drop the engine's copy of it.
                IpcClient::Instance().HideCandidates(session_);
                IpcClient::Instance().Reset(session_);
            }
            bool committed = false;
            if (context_) {
                committed = SUCCEEDED(RequestEdit([&](TfEditCookie ec) {
                    CommitInContext(ec, punctuation);
                }));
            }
            // A mark that could not be delivered must not vanish with the key:
            // let it reach the app as typed instead of swallowing it.
            if (committed) {
                *eaten = TRUE;
            } else {
                DropPreedit();
            }
            return S_OK;
        }

        KeyEvent event;
        event.keycode = static_cast<uint32_t>(vkey);
        // A packet carries the character rather than a key; give the chain the
        // key a physical keyboard would have sent so digit selection and the
        // keymap behave the same through a remote-control session.
        if (vkey == VK_PACKET) event.keycode = PacketKeycode(ascii);
        event.modifiers = 0;
        if (ModifierDown(VK_SHIFT)) event.modifiers |= 1;
        if (ModifierDown(VK_CONTROL)) event.modifiers |= 2;
        if (ModifierDown(VK_MENU)) event.modifiers |= 4;
        event.action = PIME_KEY_DOWN;
        event.text = KeyText(vkey, lparam);

        // The engine owns paging: it sees the same key and redraws its strip.
        const KeyResult result = IpcClient::Instance().Key(session_, event);
        // Only eat the key when its output actually reached the document; a
        // refused edit session must not leave the user with dead keys.
        const bool applied = ApplyCommands(result.commands);
        if (result.handled && applied) *eaten = TRUE;
        if (!applied) DropPreedit();
        return S_OK;
    }
    STDMETHODIMP OnKeyUp(ITfContext*, WPARAM vkey, LPARAM, BOOL* eaten) override {
        if (!eaten) return E_INVALIDARG;
        *eaten = FALSE;
        if (vkey != VK_SHIFT) return S_OK;
        const bool plain = !ModifierDown(VK_CONTROL) && !ModifierDown(VK_MENU) &&
                           !ModifierDown(VK_LWIN) && !ModifierDown(VK_RWIN);
        const bool tapped = shift_down_ && !shift_used_ && plain;
        shift_down_ = false;
        shift_used_ = false;
        if (tapped && engine_ready_) ToggleChinese();
        return S_OK;
    }
    STDMETHODIMP OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) override {
        if (eaten) *eaten = FALSE;
        return S_OK;
    }

    // ---- ITfCompositionSink ---------------------------------------------
    STDMETHODIMP OnCompositionTerminated(TfEditCookie, ITfComposition*) override {
        if (ending_composition_) return S_OK;
        composition_.Reset();
        composition_range_.Reset();
        composition_text_.clear();
        if (session_) {
            IpcClient::Instance().HideCandidates(session_);
            IpcClient::Instance().Reset(session_);
        }
        return S_OK;
    }

    // ---- ITfDisplayAttributeProvider ------------------------------------
    STDMETHODIMP EnumDisplayAttributeInfo(
        IEnumTfDisplayAttributeInfo** out) override {
        return CreateDisplayAttributeEnumerator(out);
    }
    STDMETHODIMP GetDisplayAttributeInfo(
        REFGUID guid, ITfDisplayAttributeInfo** out) override {
        return CreateDisplayAttributeInfo(guid, out);
    }

private:
    ~TextServiceImpl() = default;

    static LRESULT CALLBACK NotifyWndProc(HWND hwnd, UINT message,
                                          WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<TextServiceImpl*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        if (message == kEngineWakeMessage) {
            self->OnWake();
            return 0;
        }
        if (message == self->config_message_ && self->config_message_) {
            self->ReloadSettings();
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void CreateNotifyWindow() {
        static std::once_flag once;
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&TextServiceImpl::NotifyWndProc),
                           &module);
        std::call_once(once, [module] {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &TextServiceImpl::NotifyWndProc;
            wc.hInstance = module;
            wc.lpszClassName = kNotifyClassName;
            RegisterClassExW(&wc);
        });
        notify_hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                       kNotifyClassName, L"", WS_POPUP, 0, 0, 0,
                                       0, nullptr, nullptr, module, this);
        config_message_ = ConfigChangedMessage();
    }

    // Claim a key only when OnKeyDown will really consume it. A TIP that says
    // "mine" here and then declines in OnKeyDown loses the key in hosts that
    // do not re-dispatch it (Scintilla, remote-control sessions), which is how
    // digits and "/" stop working in Chinese mode.
    bool ShouldClaim(WPARAM vkey, LPARAM lparam) {
        if (vkey == VK_SHIFT) return true;
        if (!engine_ready_ || !session_ || !chinese_) return false;
        if (ModifierDown(VK_CONTROL) || ModifierDown(VK_MENU) ||
            ModifierDown(VK_LWIN) || ModifierDown(VK_RWIN)) {
            return false;
        }
        const std::string text = KeyText(vkey, lparam);
        if (composition_ || candidates_up_) {
            // While a preedit is up the IME owns every key that can extend,
            // pick, commit or cancel it, including digits and space.
            return !text.empty() || IsEditingKey(vkey);
        }
        // Idle: letters start a preedit and Chinese marks are converted; a
        // digit, space or plain symbol would only be handed back unchanged, so
        // leave it to the app instead of claiming and dropping it.
        return IsLetter(text) || IsPunctuationKey(text);
    }

    static bool IsPunctuationKey(const std::string& text) {
        return FullWidthMark(text) != nullptr || text == "\"" || text == "'";
    }

    // Single ASCII letter. Everything the chain can put into a pinyin buffer.
    static bool IsLetter(const std::string& text) {
        if (text.size() != 1) return false;
        const unsigned char c = static_cast<unsigned char>(text[0]);
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    // A printable single-byte symbol that is not a letter, digit or space:
    // punctuation the chain cannot consume, so the preedit ends and it types.
    static bool IsPlainMark(const std::string& text) {
        if (text.size() != 1) return false;
        const unsigned char c = static_cast<unsigned char>(text[0]);
        if (c <= 0x20 || c >= 0x7F) return false;
        // The apostrophe is a syllable separator inside a preedit ("xi'an"),
        // not a mark that ends it.
        if (c == '\'') return false;
        if ((c >= '0' && c <= '9') || IsLetter(text)) return false;
        return true;
    }

    // Characters that belong to a v-mode expression ("v1/2+3"); the engine must
    // keep them in the buffer instead of committing on them.
    static bool IsVModeOperator(const std::string& text) {
        if (text.size() != 1) return false;
        switch (text[0]) {
            case '+': case '-': case '*': case '/': case '%': case '^':
            case '(': case ')': case '.': case '=':
                return true;
            default:
                return (text[0] >= '0' && text[0] <= '9');
        }
    }

    static bool IsEditingKey(WPARAM vkey) {
        switch (vkey) {
            case VK_BACK:
            case VK_RETURN:
            case VK_ESCAPE:
            case VK_SPACE:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_HOME:
            case VK_END:
            case VK_UP:
            case VK_DOWN:
            case VK_DELETE:
                return true;
            default:
                return false;
        }
    }

    // Remote-control tools and on-screen keyboards inject keys as unicode
    // packets (VK_PACKET, the character rides in lparam) or as bare scan codes
    // with no virtual key. ToUnicodeEx needs a virtual key, so resolve one from
    // the scan code and take the packet character directly; otherwise those
    // keys look like "no text", are never claimed, and the pinyin never starts.
    static std::string KeyText(WPARAM vkey, LPARAM lparam) {
        const UINT scan = static_cast<UINT>((lparam >> 16) & 0xFF);
        if (vkey == VK_PACKET) {
            const wchar_t packet = static_cast<wchar_t>((lparam >> 16) & 0xFFFF);
            if (packet < 0x20 || packet == 0x7F) return {};
            return ToUtf8(std::wstring(1, packet));
        }
        if (!vkey) {
            vkey = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
            if (!vkey) return {};
        }
        BYTE state[256] = {};
        if (!GetKeyboardState(state)) return {};
        wchar_t buffer[8] = {};
        const int count = ToUnicodeEx(
            static_cast<UINT>(vkey), scan, state, buffer, 8, 4,
            GetKeyboardLayout(GetCurrentThreadId()));
        if (count <= 0) return {};
        std::wstring text(buffer, buffer + count);
        if (text.size() == 1 && (text[0] < 0x20 || text[0] == 0x7F)) return {};
        return ToUtf8(text);
    }

    // Virtual key for a character injected as a unicode packet, so digits still
    // select candidates and letters still go through the chain's keymap.
    static uint32_t PacketKeycode(const std::string& text) {
        if (text.size() != 1) return VK_PACKET;
        const unsigned char c = static_cast<unsigned char>(text[0]);
        if (c >= 'a' && c <= 'z') return 'A' + (c - 'a');
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= '0' && c <= '9') return c;
        if (c == ' ') return VK_SPACE;
        return VK_PACKET;
    }

    // Full-width form of an ASCII mark, or null when the key is not
    // punctuation. Only single-byte ASCII input can match. Quotes are not
    // here: they alternate open/close and need state, handled by the caller.
    static const wchar_t* FullWidthMark(const std::string& text) {
        if (text.size() != 1) return nullptr;
        switch (text[0]) {
            case ',': return L"，";
            case '.': return L"。";
            case '?': return L"？";
            case '!': return L"！";
            case ';': return L"；";
            case ':': return L"：";
            case '\\': return L"、";
            case '<': return L"《";
            case '>': return L"》";
            case '(': return L"（";
            case ')': return L"）";
            case '[': return L"【";
            case ']': return L"】";
            case '{': return L"｛";
            case '}': return L"｝";
            case '~': return L"～";
            case '@': return L"＠";
            case '#': return L"＃";
            case '$': return L"￥";
            case '%': return L"％";
            case '^': return L"＾";
            case '&': return L"＆";
            case '*': return L"＊";
            case '-': return L"－";
            case '_': return L"＿";
            case '+': return L"＋";
            case '=': return L"＝";
            case '|': return L"｜";
            case '/': return L"／";
            case '`': return L"｀";
            default: return nullptr;
        }
    }

    HRESULT RequestEdit(const std::function<void(TfEditCookie)>& body) {
        if (!context_) return E_FAIL;
        auto* session = new (std::nothrow) EditSession(body);
        if (!session) return E_OUTOFMEMORY;
        HRESULT session_result = E_FAIL;
        const HRESULT request = context_->RequestEditSession(
            client_id_, session, TF_ES_SYNC | TF_ES_READWRITE, &session_result);
        session->Release();
        if (FAILED(request)) {
            WriteLog(LogLevel::Warn, "tsf",
                     "RequestEditSession failed: " +
                         std::to_string(static_cast<long>(request)));
        } else if (FAILED(session_result)) {
            WriteLog(LogLevel::Warn, "tsf",
                     "edit session failed: " +
                         std::to_string(static_cast<long>(session_result)));
        }
        return SUCCEEDED(request) ? session_result : request;
    }

    void StartSession() {
        if (!engine_ready_ || !context_ || session_) return;
        app_ = ProcessImageName();
        // Modern IMEs remember the Chinese/English state per application. The
        // engine resolves it: a packaged host's own HKCU read is virtualized
        // and would always miss.
        bool chinese = chinese_;
        session_ = IpcClient::Instance().OpenSession(
            notify_hwnd_, app_, PIME_FIELD_TEXT, kHostCaps, settings_.page_size,
            &chinese);
        chinese_ = chinese;
        SetOpenClose(chinese_);
        UpdateLangBar();
        if (session_) {
            IpcClient::Instance().Focus(session_, true);
            WriteLog(LogLevel::Info, "tsf",
                     "session " + std::to_string(session_) + " for " +
                         ToUtf8(app_));
        }
    }

    void LeaveDocument() {
        if (composition_) CancelComposition();
        candidates_up_ = false;
        if (session_) {
            IpcClient::Instance().HideCandidates(session_);
            IpcClient::Instance().Focus(session_, false);
            IpcClient::Instance().CloseSession(session_);
            session_ = 0;
        }
        composition_.Reset();
        composition_range_.Reset();
        composition_text_.clear();
        context_.Reset();
        document_.Reset();
    }

    void CancelComposition() {
        if (!composition_) {
            composition_range_.Reset();
            return;
        }
        RequestEdit([this](TfEditCookie ec) { EndCompositionInContext(ec, false); });
        composition_.Reset();
        composition_range_.Reset();
        composition_text_.clear();
    }

    // The host refused the edit: drop our copy of the preedit and the chain's
    // buffer, so the next key starts clean instead of composing on top of text
    // that never landed in the document.
    void DropPreedit() {
        composition_.Reset();
        composition_range_.Reset();
        composition_text_.clear();
        candidates_up_ = false;
        if (session_) IpcClient::Instance().Reset(session_);
    }

    void EndCompositionInContext(TfEditCookie ec, bool keep_text) {
        if (!composition_) return;
        if (!keep_text && composition_range_) {
            composition_range_->SetText(ec, 0, L"", 0);
        }
        ending_composition_ = true;
        composition_->EndComposition(ec);
        ending_composition_ = false;
    }

    void UpdateCompositionInContext(TfEditCookie ec, const std::wstring& text) {
        if (!context_) return;
        if (!composition_) {
            ComPtr<ITfContextComposition> owner;
            if (!SUCCEEDED(context_.As(&owner)) || !owner) return;
            // Pass the selection range explicitly: some hosts (RichEdit in
            // particular) reject a null range with E_INVALIDARG.
            TF_SELECTION selection{};
            ULONG fetched = 0;
            ITfRange* selection_range = nullptr;
            if (context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection,
                                       &fetched) == S_OK &&
                fetched > 0) {
                selection_range = selection.range;
            }
            ComPtr<ITfComposition> created;
            const HRESULT started = owner->StartComposition(
                ec, selection_range, static_cast<ITfCompositionSink*>(this),
                &created);
            if (selection_range) selection_range->Release();
            if (FAILED(started) || !created) {
                WriteLog(LogLevel::Warn, "tsf",
                         "StartComposition failed: " +
                             std::to_string(static_cast<long>(started)));
                return;
            }
            composition_ = created;
            if (FAILED(composition_->GetRange(&composition_range_))) {
                ending_composition_ = true;
                composition_->EndComposition(ec);
                ending_composition_ = false;
                composition_.Reset();
                return;
            }
        }
        if (composition_range_) {
            const HRESULT set = composition_range_->SetText(
                ec, 0, text.c_str(), static_cast<LONG>(text.size()));
            if (FAILED(set)) {
                WriteLog(LogLevel::Warn, "tsf",
                         "composition SetText failed: " +
                             std::to_string(static_cast<long>(set)));
            }
            // Preview text is otherwise indistinguishable from committed text
            // in hosts that only draw the default caret. The property holds a
            // TfGuidAtom, not the attribute object itself.
            if (attr_atom_ == 0) {
                ComPtr<ITfCategoryMgr> categories;
                if (SUCCEEDED(CoCreateInstance(
                        CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(categories.GetAddressOf()))) &&
                    categories) {
                    categories->RegisterGUID(kGuidDisplayAttribute,
                                             &attr_atom_);
                }
            }
            ComPtr<ITfProperty> property;
            if (attr_atom_ != 0 &&
                SUCCEEDED(context_->GetProperty(
                    GUID_PROP_ATTRIBUTE, property.GetAddressOf())) &&
                property) {
                VARIANT value{};
                value.vt = VT_I4;
                value.lVal = static_cast<LONG>(attr_atom_);
                property->SetValue(ec, composition_range_.Get(), &value);
            }
            // RichEdit keeps its caret where the composition started, so the
            // insertion point would blink under the first letter of the
            // pinyin instead of after the last one.
            ComPtr<ITfRange> caret;
            if (SUCCEEDED(composition_range_->Clone(&caret)) && caret &&
                SUCCEEDED(caret->Collapse(ec, TF_ANCHOR_END))) {
                TF_SELECTION selection{};
                selection.range = caret.Get();
                selection.style.ase = TF_AE_NONE;
                selection.style.fInterimChar = FALSE;
                context_->SetSelection(ec, 1, &selection);
            }
        }
        composition_text_ = text;
    }

    void CommitInContext(TfEditCookie ec, const std::wstring& text) {
        if (composition_) {
            if (composition_range_) {
                composition_range_->SetText(
                    ec, 0, text.c_str(), static_cast<LONG>(text.size()));
            }
            // EndComposition leaves the caret at the composition start in
            // RichEdit hosts, so the next composition would land before the
            // text that was just committed. Remember the end and reselect it.
            ComPtr<ITfRange> caret;
            if (composition_range_ &&
                SUCCEEDED(composition_range_->Clone(&caret))) {
                caret->Collapse(ec, TF_ANCHOR_END);
            }
            ending_composition_ = true;
            composition_->EndComposition(ec);
            ending_composition_ = false;
            if (caret && context_) {
                TF_SELECTION selection{};
                selection.range = caret.Get();
                selection.style.ase = TF_AE_NONE;
                selection.style.fInterimChar = FALSE;
                context_->SetSelection(ec, 1, &selection);
            }
        } else if (context_ && !text.empty()) {
            // RichEdit rejects a null output range even with TF_IAS_NOQUERY,
            // so ask for the range; it also leaves the selection where it was,
            // which would put the next character before this one.
            ComPtr<ITfInsertAtSelection> insert;
            ComPtr<ITfRange> inserted;
            if (SUCCEEDED(context_.As(&insert)) && insert &&
                SUCCEEDED(insert->InsertTextAtSelection(
                    ec, 0, text.c_str(), static_cast<LONG>(text.size()),
                    inserted.GetAddressOf())) &&
                inserted) {
                inserted->Collapse(ec, TF_ANCHOR_END);
                TF_SELECTION selection{};
                selection.range = inserted.Get();
                selection.style.ase = TF_AE_NONE;
                selection.style.fInterimChar = FALSE;
                context_->SetSelection(ec, 1, &selection);
            }
        }
        composition_.Reset();
        composition_range_.Reset();
        composition_text_.clear();
    }

    bool CaretRect(TfEditCookie ec, RECT* out) {
        if (!context_ || !out) return false;
        ComPtr<ITfContextView> view;
        if (FAILED(context_->GetActiveView(&view)) || !view) return false;
        ComPtr<ITfRange> range;
        if (composition_range_) composition_range_->Clone(&range);
        if (!range) {
            TF_SELECTION selection{};
            ULONG fetched = 0;
            if (context_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection,
                                       &fetched) != S_OK ||
                fetched == 0) {
                return false;
            }
            range.Attach(selection.range);
        }
        if (!range) return false;
        RECT rect{};
        BOOL clipped = FALSE;
        if (FAILED(view->GetTextExt(ec, range.Get(), &rect, &clipped))) {
            return false;
        }
        if (rect.bottom <= rect.top) rect.bottom = rect.top + 24;
        *out = rect;
        return true;
    }

    // Returns false when the engine asked for text output the host refused (no
    // focus context, or a failing edit session). Callers then let the key reach
    // the app instead of eating it: swallowing input that never lands is how a
    // Chinese-mode session ends up with every key dead.
    bool ApplyCommands(const std::vector<UiCommand>& commands) {
        std::wstring composition_text;
        bool update_composition = false;
        std::wstring commit_text;
        bool commit = false;
        bool show = false;
        bool have_items = false;

        for (const UiCommand& command : commands) {
            switch (command.kind) {
                case UiKind::Composition:
                    composition_text = ToWide(command.text);
                    update_composition = true;
                    break;
                case UiKind::Commit:
                    commit_text = ToWide(command.text);
                    commit = true;
                    break;
                case UiKind::Candidates:
                    // The engine draws the strip itself; the shell only tracks
                    // whether one is up, so it knows which keys to claim.
                    show = true;
                    have_items = !command.candidates.empty();
                    break;
                case UiKind::HideCandidates:
                    candidates_up_ = false;
                    break;
                case UiKind::Beep:
                    MessageBeep(MB_OK);
                    break;
                case UiKind::Notify:
                    break;
            }
        }

        RECT caret{};
        bool have_caret = false;
        bool applied = true;
        if (commit || update_composition || show) {
            const HRESULT edit = RequestEdit([&](TfEditCookie ec) {
                if (commit) {
                    CommitInContext(ec, commit_text);
                } else if (update_composition) {
                    if (composition_text.empty()) {
                        EndCompositionInContext(ec, false);
                        composition_.Reset();
                        composition_range_.Reset();
                        composition_text_.clear();
                    } else {
                        UpdateCompositionInContext(ec, composition_text);
                    }
                }
                have_caret = CaretRect(ec, &caret);
            });
            // A page turn only refreshes the caret; only real text output has
            // to land before the key counts as eaten.
            if (commit || update_composition) applied = SUCCEEDED(edit);
        }
        // The strip lives in the engine; tell it where the text is now.
        if (have_caret && session_) IpcClient::Instance().SendCaret(session_, caret);

        if (commit) {
            candidates_up_ = false;
            // A commit always ends the composition here, but the chain's
            // parser only drops its buffer on space/enter/candidate commits;
            // a digit pick leaves the raw pinyin behind and the next letter
            // would compose on top of it ("zhongguo" + "a"). Reset so the
            // engine state matches the editor.
            if (session_) IpcClient::Instance().Reset(session_);
            return applied;
        }
        if (show) candidates_up_ = have_items;
        return applied;
    }

    void ToggleChinese() {
        ApplyMode(!chinese_);
        SetOpenClose(chinese_);
    }

    // Right-click menu on the taskbar 中/英 button. TPM_RETURNCMD hands the
    // chosen item back here, so no WM_COMMAND handling is needed; the hidden
    // notify window only serves as the same-thread owner TrackPopupMenu wants.
    void ShowContextMenu(POINT point) {
        if (!notify_hwnd_) return;
        const HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, kMenuSettings, L"设置...");
        SetForegroundWindow(notify_hwnd_);
        const UINT cmd = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, point.x,
            point.y, 0, notify_hwnd_, nullptr);
        DestroyMenu(menu);
        PostMessageW(notify_hwnd_, WM_NULL, 0, 0);
        if (cmd == kMenuSettings) OpenSettings();
    }

    // The 设置 item launches the settings app, which dist keeps beside
    // pime_tsf.dll.
    void OpenSettings() {
        if (HWND existing = FindWindowW(L"PimeSettingsWindow", nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
            return;
        }
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&CreatePimeTextService),
                           &module);
        const std::wstring dir = ModuleDirectory(module);
        const std::wstring exe = PathJoin(dir, L"pime.exe");
        if (!FileExists(exe)) {
            WriteLog(LogLevel::Error, "tsf", "settings exe missing: " + ToUtf8(exe));
            return;
        }
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                nullptr, L"open", exe.c_str(), nullptr, dir.c_str(),
                SW_SHOWNORMAL)) <= 32) {
            WriteLog(LogLevel::Error, "tsf", "settings launch failed");
        }
    }

    // Applies a mode change from Shift, the language bar button or the
    // keyboard open/close compartment.
    void ApplyMode(bool chinese) {
        chinese_ = chinese;
        settings_.chinese = chinese_ ? 1 : 0;
        if (composition_) CancelComposition();
        candidates_up_ = false;
        if (session_) IpcClient::Instance().Reset(session_);

        RECT caret{};
        bool have_caret = false;
        if (context_) {
            RequestEdit([&](TfEditCookie ec) { have_caret = CaretRect(ec, &caret); });
        }
        if (!have_caret) {
            POINT point{};
            GetCursorPos(&point);
            caret = RECT{point.x, point.y, point.x, point.y};
        }
        // Persist only the per-app mode, and only through the engine: a
        // packaged host writes HKCU into a private, virtualized copy that no
        // other process and no later session ever sees. The engine also
        // flashes the badge at this caret.
        if (engine_ready_) {
            IpcClient::Instance().SaveAppMode(session_, app_, chinese_, caret);
        }
        UpdateLangBar();
    }

    void OnWake() {
        if (!session_) return;
        // The strip's 中/英 chip was clicked in the engine; it already
        // persisted the mode, so only mirror it here.
        bool chinese = false;
        if (IpcClient::Instance().TakeModeChange(session_, &chinese)) {
            chinese_ = chinese;
            settings_.chinese = chinese_ ? 1 : 0;
            candidates_up_ = false;
            if (composition_) CancelComposition();
            SetOpenClose(chinese_);
            UpdateLangBar();
        }
        std::vector<UiCommand> commands;
        if (IpcClient::Instance().TakeAsync(session_, &commands)) {
            ApplyCommands(commands);
        }
    }

    void ReloadSettings() {
        // Settings come from the engine, never from this process's HKCU: inside
        // a packaged host (Notepad, Settings, ...) the registry read is
        // virtualized and would never see a change made elsewhere.
        Settings next = LoadSettings();
        if (IpcClient::Instance().RefreshSettings()) {
            next = IpcClient::Instance().settings();
        }
        const bool page_changed = next.page_size != settings_.page_size;
        const bool chain_changed = next.chain != settings_.chain;
        // Keep this app's 中/英: a page-size or theme change must not snap the
        // session back to the global default.
        next.chinese = chinese_ ? 1 : 0;
        settings_ = next;
        WriteLog(LogLevel::Info, "tsf",
                 "settings reloaded page=" + std::to_string(settings_.page_size) +
                     " chain=" + ToUtf8(settings_.chain));
        if (engine_ready_ && chain_changed) {
            IpcClient::Instance().SwitchChain(ToUtf8(settings_.chain));
        }
        if (page_changed) {
            if (composition_) CancelComposition();
            candidates_up_ = false;
            if (session_) {
                IpcClient::Instance().CloseSession(session_);
                session_ = 0;
                StartSession();
            }
        }
        UpdateLangBar();
    }

    LONG ref_ = 1;
    ComPtr<ITfThreadMgr> thread_mgr_;
    ComPtr<ITfKeystrokeMgr> keys_;
    ComPtr<ITfLangBarItemMgr> langbar_mgr_;
    ComPtr<ITfLangBarItemSink> langbar_sink_;
    ComPtr<ITfCompartment> open_close_;
    DWORD open_close_cookie_ = 0;
    TfClientId client_id_ = TF_CLIENTID_NULL;
    DWORD thread_event_cookie_ = 0;
    DWORD thread_focus_cookie_ = 0;
    bool key_advised_ = false;

    ComPtr<ITfDocumentMgr> document_;
    ComPtr<ITfContext> context_;
    ComPtr<ITfComposition> composition_;
    ComPtr<ITfRange> composition_range_;
    TfGuidAtom attr_atom_ = 0;
    std::wstring composition_text_;
    bool ending_composition_ = false;

    // The engine draws the strip; this only remembers whether one is up so
    // ShouldClaim can claim editing keys while candidates are showing.
    bool candidates_up_ = false;

    SessionId session_ = 0;
    bool engine_ready_ = false;
    std::wstring app_;
    Settings settings_;
    bool chinese_ = true;
    bool shift_down_ = false;
    bool shift_used_ = false;
    // Alternating curly quotes (“ ” / ‘ ’), flipped as each one is typed.
    bool double_quote_open_ = true;
    bool single_quote_open_ = true;

    HWND notify_hwnd_ = nullptr;
    UINT config_message_ = 0;
    HWINEVENTHOOK minimize_hook_ = nullptr;
    static thread_local TextServiceImpl* minimize_owner_;
};

thread_local TextServiceImpl* TextServiceImpl::minimize_owner_ = nullptr;

}  // namespace

HRESULT CreatePimeTextService(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    auto* service = new (std::nothrow) TextServiceImpl();
    if (!service) return E_OUTOFMEMORY;
    const HRESULT hr = service->QueryInterface(riid, ppv);
    service->Release();
    return hr;
}

}  // namespace pimewin
