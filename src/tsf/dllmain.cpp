// COM entry points for the TSF in-proc server. Registration lives in
// register.cpp (driven by pime-settings), so this file only creates objects.
#include <windows.h>

#include <new>

#include "tsf/guids.h"
#include "tsf/text_service.h"

namespace {

LONG g_lock_count = 0;

class ClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
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
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid,
                                void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (outer) return CLASS_E_NOAGGREGATION;
        return pimewin::CreatePimeTextService(riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) {
            InterlockedIncrement(&g_lock_count);
        } else {
            InterlockedDecrement(&g_lock_count);
        }
        return S_OK;
    }

private:
    ~ClassFactory() = default;
    LONG ref_ = 1;
};

}  // namespace

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, pimewin::kClsidTextService)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) ClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    // The text service keeps process-wide state (pime-core) for the lifetime
    // of the host; unloading is never worthwhile.
    return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
