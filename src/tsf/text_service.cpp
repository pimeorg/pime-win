// TSF text service: owns the composition in the focused document, forwards
// keys to pime-core through CoreEngine and renders pime-core's commands with
// the layered candidate window.
#include "tsf/text_service.h"

#include <msctf.h>
#include <oleauto.h>
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
#include "engine/core_engine.h"
#include "tsf/display_attr.h"
#include "tsf/guids.h"
#include "ui/candidate_window.h"

using Microsoft::WRL::ComPtr;

namespace pimewin {
namespace {

// Bit 4 (HOST_CANDIDATES): the host renders the candidate strip itself.
constexpr uint32_t kHostCaps = 1u << 4;
constexpr wchar_t kNotifyClassName[] = L"PimeNotifyWindow";

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

enum class Nav { None, Prev, Next, First, Last };

bool DarkFor(uint32_t theme) {
    if (theme == 1) return false;
    if (theme == 2) return true;
    return !SystemUsesLightTheme();
}

bool ModifierDown(int vkey) { return (GetKeyState(vkey) & 0x8000) != 0; }

int PageAfter(int page, Nav nav) {
    switch (nav) {
        case Nav::Prev:
            return page > 0 ? page - 1 : 0;
        case Nav::Next:
        case Nav::Last:
            return page + 1;
        case Nav::First:
        case Nav::None:
            return 0;
    }
    return 0;
}

class TextServiceImpl final : public ITfTextInputProcessorEx,
                              public ITfThreadMgrEventSink,
                              public ITfThreadFocusSink,
                              public ITfKeyEventSink,
                              public ITfCompositionSink,
                              public ITfDisplayAttributeProvider,
                              public CandidateWindow::Delegate {
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
        theme_dark_ = DarkFor(settings_.theme);

        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&CreatePimeTextService),
                           &module);
        EngineConfig config;
        // Staging keeps chains/ and plugins/ next to pime_tsf.dll.
        config.resources_dir = ModuleDirectory(module);
        config.data_dir = DataDirectory();
        config.chain = ToUtf8(settings_.chain);
        config.page_size = settings_.page_size;
        engine_ready_ = CoreEngine::Instance().Start(config);
        if (!engine_ready_) {
            WriteLog(LogLevel::Error, "tsf",
                     "pime-core failed to start from " +
                         ToUtf8(config.resources_dir));
        }

        CreateNotifyWindow();
        window_.SetDark(theme_dark_);
        WriteLog(LogLevel::Info, "tsf",
                 window_.Create(this) ? "candidate window created"
                                      : "candidate window create failed");
        minimize_owner_ = this;
        if (!minimize_hook_) {
            minimize_hook_ = SetWinEventHook(
                EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr,
                &TextServiceImpl::MinimizeEventProc, 0, GetCurrentThreadId(),
                WINEVENT_OUTOFCONTEXT);
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
        window_.Destroy();
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
        thread_mgr_.Reset();
        client_id_ = TF_CLIENTID_NULL;
        shift_down_ = shift_used_ = false;
        WriteLog(LogLevel::Info, "tsf", "deactivated");
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
        candidates_.clear();
        candidates_visible_ = false;
        page_ = 0;
        window_.Hide();
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
        else if (session_) CoreEngine::Instance().Focus(session_, true);
        return S_OK;
    }
    STDMETHODIMP OnKillThreadFocus() override {
        shift_down_ = shift_used_ = false;
        if (composition_) CancelComposition();
        candidates_.clear();
        candidates_visible_ = false;
        page_ = 0;
        window_.Hide();
        if (session_) CoreEngine::Instance().Focus(session_, false);
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
        const std::wstring converted = ChinesePunctuation(ascii);
        const bool full_width = settings_.punctuation != 0;
        std::wstring punctuation;
        if (!converted.empty()) {
            punctuation = full_width ? converted : ToWide(ascii);
        } else if (composition_text_.empty() && ascii == "'") {
            punctuation = full_width ? L"’" : L"'";
        }
        const bool v_mode =
            !composition_text_.empty() &&
            (composition_text_[0] == L'v' || composition_text_[0] == L'V');
        const bool v_operator = ascii == "." || ascii == "(" || ascii == ")";
        if (!punctuation.empty() && !(v_mode && v_operator)) {
            if (composition_ || candidates_visible_) {
                KeyEvent space;
                space.keycode = VK_SPACE;
                space.action = PIME_KEY_DOWN;
                ApplyCommands(CoreEngine::Instance().Key(session_, space).commands);
            }
            if (composition_ || candidates_visible_) {
                // The space found nothing to commit; the mark still ends the
                // preedit, so drop the engine's copy of it.
                candidates_.clear();
                candidates_visible_ = false;
                page_ = 0;
                window_.Hide();
                CoreEngine::Instance().Reset(session_);
            }
            if (context_) {
                RequestEdit([&](TfEditCookie ec) { CommitInContext(ec, punctuation); });
            }
            *eaten = TRUE;
            return S_OK;
        }

        KeyEvent event;
        event.keycode = static_cast<uint32_t>(vkey);
        event.modifiers = 0;
        if (ModifierDown(VK_SHIFT)) event.modifiers |= 1;
        if (ModifierDown(VK_CONTROL)) event.modifiers |= 2;
        if (ModifierDown(VK_MENU)) event.modifiers |= 4;
        event.action = PIME_KEY_DOWN;
        event.text = KeyText(vkey, lparam);

        const Nav nav = NavForKey(vkey);
        page_ = PageAfter(page_, nav);
        const KeyResult result = CoreEngine::Instance().Key(session_, event);
        ApplyCommands(result.commands);
        if (result.handled) *eaten = TRUE;
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
        candidates_.clear();
        candidates_visible_ = false;
        page_ = 0;
        window_.Hide();
        if (session_) CoreEngine::Instance().Reset(session_);
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

    // ---- CandidateWindow::Delegate --------------------------------------
    void OnCandidatePicked(int slot) override {
        if (!session_ || slot < 0 || slot > 8) return;
        KeyEvent event;
        event.keycode = static_cast<uint32_t>('1' + slot);
        event.action = PIME_KEY_DOWN;
        page_ = 0;
        const KeyResult result = CoreEngine::Instance().Key(session_, event);
        ApplyCommands(result.commands);
    }
    void OnPageRequested(int direction) override {
        if (!session_) return;
        if (direction == 0) return;
        const Nav nav = direction < 0 ? Nav::Prev : Nav::Next;
        page_ = PageAfter(page_, nav);
        KeyEvent event;
        event.keycode = direction < 0 ? VK_PRIOR : VK_NEXT;
        event.action = PIME_KEY_DOWN;
        const KeyResult result = CoreEngine::Instance().Key(session_, event);
        ApplyCommands(result.commands);
    }
    void OnModeToggled() override { ToggleChinese(); }

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

    bool ShouldClaim(WPARAM vkey, LPARAM lparam) {
        if (vkey == VK_SHIFT) return true;
        if (!engine_ready_ || !session_ || !chinese_) return false;
        if (ModifierDown(VK_CONTROL) || ModifierDown(VK_MENU) ||
            ModifierDown(VK_LWIN) || ModifierDown(VK_RWIN)) {
            return false;
        }
        if (!KeyText(vkey, lparam).empty()) return true;
        if (composition_ || candidates_visible_) return IsEditingKey(vkey);
        return false;
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

    static Nav NavForKey(WPARAM vkey) {
        switch (vkey) {
            case VK_PRIOR:
            case VK_UP:
                return Nav::Prev;
            case VK_NEXT:
            case VK_DOWN:
                return Nav::Next;
            case VK_HOME:
                return Nav::First;
            case VK_END:
                return Nav::Last;
            default:
                return Nav::None;
        }
    }

    static std::string KeyText(WPARAM vkey, LPARAM lparam) {
        BYTE state[256] = {};
        if (!GetKeyboardState(state)) return {};
        wchar_t buffer[8] = {};
        const int count = ToUnicodeEx(
            static_cast<UINT>(vkey), static_cast<UINT>((lparam >> 16) & 0xFF),
            state, buffer, 8, 4, GetKeyboardLayout(GetCurrentThreadId()));
        if (count <= 0) return {};
        std::wstring text(buffer, buffer + count);
        if (text.size() == 1 && (text[0] < 0x20 || text[0] == 0x7F)) return {};
        return ToUtf8(text);
    }

    // Full-width form of the ASCII mark, or empty when the key is not
    // punctuation. Only single-byte ASCII input can match.
    static std::wstring ChinesePunctuation(const std::string& text) {
        if (text.size() != 1) return {};
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
            default: return {};
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
        // Modern IMEs remember the Chinese/English state per application.
        chinese_ = LoadAppMode(app_, settings_.chinese != 0);
        session_ = CoreEngine::Instance().OpenSession(
            notify_hwnd_, app_, PIME_FIELD_TEXT, kHostCaps, settings_.page_size);
        if (session_) {
            CoreEngine::Instance().Focus(session_, true);
            WriteLog(LogLevel::Info, "tsf",
                     "session " + std::to_string(session_) + " for " +
                         ToUtf8(app_));
        }
    }

    void LeaveDocument() {
        if (composition_) CancelComposition();
        window_.Hide();
        candidates_.clear();
        candidates_visible_ = false;
        page_ = 0;
        if (session_) {
            CoreEngine::Instance().Focus(session_, false);
            CoreEngine::Instance().CloseSession(session_);
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

    void ApplyCommands(const std::vector<UiCommand>& commands) {
        std::wstring composition_text;
        bool update_composition = false;
        std::wstring commit_text;
        bool commit = false;
        bool show = false;
        bool hide = false;
        std::vector<CandidateItem> items;

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
                    items.reserve(command.candidates.size());
                    for (const UiCandidate& candidate : command.candidates) {
                        items.push_back({ToWide(candidate.text),
                                         ToWide(candidate.comment),
                                         ToWide(candidate.label)});
                    }
                    show = true;
                    break;
                case UiKind::HideCandidates:
                    hide = true;
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
        if (commit || update_composition) {
            RequestEdit([&](TfEditCookie ec) {
                if (commit) {
                    CommitInContext(ec, commit_text);
                } else if (composition_text.empty()) {
                    EndCompositionInContext(ec, false);
                    composition_.Reset();
                    composition_range_.Reset();
                    composition_text_.clear();
                } else {
                    UpdateCompositionInContext(ec, composition_text);
                }
                if (show && !commit) have_caret = CaretRect(ec, &caret);
            });
        } else if (show) {
            RequestEdit([&](TfEditCookie ec) { have_caret = CaretRect(ec, &caret); });
        }

        if (commit) {
            candidates_.clear();
            candidates_visible_ = false;
            page_ = 0;
            window_.Hide();
            // A commit always ends the composition here, but the chain's
            // parser only drops its buffer on space/enter/candidate commits;
            // a digit pick leaves the raw pinyin behind and the next letter
            // would compose on top of it ("zhongguo" + "a"). Reset so the
            // engine state matches the editor.
            if (session_) CoreEngine::Instance().Reset(session_);
            return;
        }
        if (hide) {
            candidates_.clear();
            candidates_visible_ = false;
            window_.Hide();
            return;
        }
        if (!show) return;
        if (items.empty()) {
            candidates_.clear();
            candidates_visible_ = false;
            window_.Hide();
            return;
        }
        if (!have_caret) {
            POINT point{};
            GetCursorPos(&point);
            caret = RECT{point.x, point.y, point.x, point.y};
        }
        candidates_ = std::move(items);
        candidates_visible_ = true;
        window_.SetDark(theme_dark_);
        window_.ShowCandidates(
            candidates_, page_, page_ > 0,
            candidates_.size() >= settings_.page_size, chinese_, caret);
    }

    void ToggleChinese() {
        chinese_ = !chinese_;
        settings_.chinese = chinese_ ? 1 : 0;
        // Persist only the per-app mode. Writing the whole settings blob from
        // inside the host would, under MSIX registry virtualization, freeze a
        // private copy of every value in that package, so later changes made
        // in the settings window would never reach the app.
        SaveAppMode(app_, chinese_);
        if (composition_) CancelComposition();
        candidates_.clear();
        candidates_visible_ = false;
        page_ = 0;
        window_.Hide();
        if (session_) CoreEngine::Instance().Reset(session_);

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
        window_.SetDark(theme_dark_);
        window_.ShowModeBadge(chinese_, caret);
    }

    void OnWake() {
        if (!session_) return;
        std::vector<UiCommand> commands;
        if (!CoreEngine::Instance().TakeAsync(session_, &commands)) return;
        // Reset() (mode toggle) makes the engine emit a stale hide command
        // that would erase the mode badge the same millisecond it appears.
        if (window_.showing_badge()) return;
        ApplyCommands(commands);
    }

    void ReloadSettings() {
        const Settings next = LoadSettings();
        const bool page_changed = next.page_size != settings_.page_size;
        const bool chain_changed = next.chain != settings_.chain;
        settings_ = next;
        chinese_ = settings_.chinese != 0;
        theme_dark_ = DarkFor(settings_.theme);
        window_.SetDark(theme_dark_);
        if (engine_ready_ && chain_changed) {
            CoreEngine::Instance().SwitchChain(ToUtf8(settings_.chain));
        }
        if (page_changed) {
            if (composition_) CancelComposition();
            candidates_.clear();
            candidates_visible_ = false;
            window_.Hide();
            if (session_) {
                CoreEngine::Instance().CloseSession(session_);
                session_ = 0;
                StartSession();
            }
        }
    }

    LONG ref_ = 1;
    ComPtr<ITfThreadMgr> thread_mgr_;
    ComPtr<ITfKeystrokeMgr> keys_;
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

    CandidateWindow window_;
    std::vector<CandidateItem> candidates_;
    bool candidates_visible_ = false;
    int page_ = 0;

    SessionId session_ = 0;
    bool engine_ready_ = false;
    std::wstring app_;
    Settings settings_;
    bool chinese_ = true;
    bool theme_dark_ = false;
    bool shift_down_ = false;
    bool shift_used_ = false;

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
