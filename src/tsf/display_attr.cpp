#include "tsf/display_attr.h"

#include <oleauto.h>

#include <new>

#include "tsf/guids.h"

namespace pimewin {
namespace {

class DisplayAttributeInfo final : public ITfDisplayAttributeInfo {
public:
    DisplayAttributeInfo() = default;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_ITfDisplayAttributeInfo)) {
            *ppv = static_cast<ITfDisplayAttributeInfo*>(this);
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

    // ITfDisplayAttributeInfo
    STDMETHODIMP GetGUID(GUID* guid) override {
        if (!guid) return E_INVALIDARG;
        *guid = kGuidDisplayAttribute;
        return S_OK;
    }
    STDMETHODIMP GetDescription(BSTR* description) override {
        if (!description) return E_INVALIDARG;
        *description = SysAllocString(L"PIME 组合文本");
        return *description ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* attribute) override {
        if (!attribute) return E_INVALIDARG;
        *attribute = {};
        attribute->crText.type = TF_CT_NONE;
        attribute->crBk.type = TF_CT_NONE;
        attribute->lsStyle = TF_LS_DOT;
        attribute->fBoldLine = FALSE;
        attribute->crLine.type = TF_CT_COLORREF;
        attribute->crLine.cr = RGB(0x2f, 0x6b, 0xff);
        attribute->bAttr = TF_ATTR_INPUT;
        return S_OK;
    }
    STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE*) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP Reset() override { return E_NOTIMPL; }

private:
    ~DisplayAttributeInfo() = default;
    LONG ref_ = 1;
};

class DisplayAttributeEnumerator final : public IEnumTfDisplayAttributeInfo {
public:
    DisplayAttributeEnumerator() = default;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_IEnumTfDisplayAttributeInfo)) {
            *ppv = static_cast<IEnumTfDisplayAttributeInfo*>(this);
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

    STDMETHODIMP Next(ULONG count, ITfDisplayAttributeInfo** info,
                      ULONG* fetched) override {
        if (!info) return E_INVALIDARG;
        ULONG produced = 0;
        while (produced < count && index_ == 0) {
            ITfDisplayAttributeInfo* item = nullptr;
            const HRESULT hr = CreateDisplayAttributeInfo(kGuidDisplayAttribute,
                                                          &item);
            if (FAILED(hr)) break;
            info[produced++] = item;
            index_ = 1;
        }
        if (fetched) *fetched = produced;
        return produced == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override {
        if (index_ == 0 && count > 0) index_ = 1;
        return S_OK;
    }
    STDMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** out) override {
        if (!out) return E_INVALIDARG;
        auto* copy = new (std::nothrow) DisplayAttributeEnumerator();
        if (!copy) return E_OUTOFMEMORY;
        copy->index_ = index_;
        *out = copy;
        return S_OK;
    }

private:
    ~DisplayAttributeEnumerator() = default;
    LONG ref_ = 1;
    int index_ = 0;
};

}  // namespace

HRESULT CreateDisplayAttributeInfo(REFGUID guid,
                                   ITfDisplayAttributeInfo** out) {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (!IsEqualGUID(guid, kGuidDisplayAttribute)) return E_INVALIDARG;
    auto* info = new (std::nothrow) DisplayAttributeInfo();
    if (!info) return E_OUTOFMEMORY;
    *out = info;
    return S_OK;
}

HRESULT CreateDisplayAttributeEnumerator(IEnumTfDisplayAttributeInfo** out) {
    if (!out) return E_INVALIDARG;
    auto* enumerator = new (std::nothrow) DisplayAttributeEnumerator();
    if (!enumerator) return E_OUTOFMEMORY;
    *out = enumerator;
    return S_OK;
}

}  // namespace pimewin
