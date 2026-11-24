#include "tsf/register.h"

#include <msctf.h>
#include <wrl/client.h>

#include <cwchar>

#include "common/win_util.h"
#include "tsf/guids.h"

using Microsoft::WRL::ComPtr;

namespace pimewin {
namespace {

std::wstring ClsidString() {
    wchar_t text[64] = {};
    StringFromGUID2(kClsidTextService, text, 64);
    return text;
}

std::wstring ClsidKeyPath() {
    return L"CLSID\\" + ClsidString();
}

bool WriteString(HKEY key, const wchar_t* name, const std::wstring& value) {
    const DWORD bytes =
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()),
                          bytes) == ERROR_SUCCESS;
}

// "0804:{CLSID}{PROFILE}" is the string form used by the Windows input
// framework to add a TIP to the user's installed input methods.
std::wstring ProfileString() {
    wchar_t profile[64] = {};
    StringFromGUID2(kGuidProfile, profile, 64);
    return L"0804:" + ClsidString() + profile;
}

// RegisterProfile only publishes the TIP; InstallLayoutOrTip is what adds it
// to (or removes it from) the current user's language input list. There is no
// UninstallLayoutOrTip export - removal passes ILOT_UNINSTALL in dwFlags and
// must happen before UnregisterProfile, while the profile still exists.
constexpr DWORD kIlotUninstall = 0x00000001;

bool CallInstallLayoutOrTip(DWORD flags) {
    HMODULE input = LoadLibraryW(L"input.dll");
    if (!input) return false;
    using InstallFn = HRESULT(WINAPI*)(LPCWSTR, DWORD);
    auto fn = reinterpret_cast<InstallFn>(GetProcAddress(input, "InstallLayoutOrTip"));
    const bool ok = fn && SUCCEEDED(fn(ProfileString().c_str(), flags));
    FreeLibrary(input);
    return ok;
}

ComPtr<ITfInputProcessorProfileMgr> ProfileManager() {
    ComPtr<ITfInputProcessorProfiles> profiles;
    if (FAILED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(profiles.GetAddressOf())))) {
        return nullptr;
    }
    ComPtr<ITfInputProcessorProfileMgr> manager;
    profiles.As(&manager);
    return manager;
}

}  // namespace

bool RegisterTextService(const std::wstring& dll_path) {
    bool ok = true;

    const std::wstring clsid_path = ClsidKeyPath();
    HKEY clsid = nullptr;
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, clsid_path.c_str(), 0, nullptr, 0,
                        KEY_WRITE, nullptr, &clsid,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }
    ok = WriteString(clsid, nullptr, kTextServiceDescription) && ok;
    HKEY inproc = nullptr;
    if (RegCreateKeyExW(clsid, L"InprocServer32", 0, nullptr, 0, KEY_WRITE,
                        nullptr, &inproc, nullptr) == ERROR_SUCCESS) {
        ok = WriteString(inproc, nullptr, dll_path) && ok;
        ok = WriteString(inproc, L"ThreadingModel", L"Apartment") && ok;
        RegCloseKey(inproc);
    } else {
        ok = false;
    }
    RegCloseKey(clsid);

    ComPtr<ITfInputProcessorProfileMgr> manager = ProfileManager();
    if (manager) {
        const HRESULT hr = manager->RegisterProfile(
            kClsidTextService, kLangId, kGuidProfile, kProfileDescription,
            static_cast<ULONG>(std::wcslen(kProfileDescription)), nullptr, 0,
            0, nullptr, 0, TRUE, 0);
        ok = SUCCEEDED(hr) && ok;
    } else {
        ok = false;
    }

    ComPtr<ITfCategoryMgr> categories;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(categories.GetAddressOf())))) {
        const GUID* const category_ids[] = {
            &GUID_TFCAT_TIP_KEYBOARD,
            &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
            &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
            &GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
        };
        for (const GUID* category : category_ids) {
            categories->RegisterCategory(kClsidTextService, *category,
                                         kClsidTextService);
        }
        // Display attributes are stored as atoms of their GUID, so the GUID
        // must be known to the category manager before the TIP can use it.
        TfGuidAtom attribute_atom = 0;
        categories->RegisterGUID(kGuidDisplayAttribute, &attribute_atom);
    } else {
        ok = false;
    }

    return ok;
}

bool AddToUserInputList() {
    return CallInstallLayoutOrTip(0);
}

bool UnregisterTextService() {
    bool ok = true;

    CallInstallLayoutOrTip(kIlotUninstall);

    // Best-effort: when the profile is already gone the profile manager fails,
    // but the registry cleanup below still removes every trace.
    ComPtr<ITfInputProcessorProfileMgr> manager = ProfileManager();
    if (manager) {
        manager->UnregisterProfile(kClsidTextService, kLangId, kGuidProfile, 0);
    } else {
        ok = false;
    }

    ComPtr<ITfCategoryMgr> categories;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(categories.GetAddressOf())))) {
        const GUID* const category_ids[] = {
            &GUID_TFCAT_TIP_KEYBOARD,
            &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
            &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
            &GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
        };
        for (const GUID* category : category_ids) {
            categories->UnregisterCategory(kClsidTextService, *category,
                                           kClsidTextService);
        }
    }

    ok = RegistryDeleteTree(HKEY_CLASSES_ROOT, ClsidKeyPath()) && ok;
    // UnregisterProfile leaves the Category/LanguageProfile records behind.
    const std::wstring ctf_tip = L"Microsoft\\CTF\\TIP\\" + ClsidString();
    ok = RegistryDeleteTree(HKEY_LOCAL_MACHINE, L"SOFTWARE\\" + ctf_tip) && ok;
    ok = RegistryDeleteTree(HKEY_CURRENT_USER, L"Software\\" + ctf_tip) && ok;
    return ok;
}

bool ActivateTextService() {
    ComPtr<ITfInputProcessorProfiles> profiles;
    if (FAILED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(profiles.GetAddressOf())))) {
        return false;
    }
    const HRESULT activated = profiles->ActivateLanguageProfile(
        kClsidTextService, kLangId, kGuidProfile);
    return SUCCEEDED(activated);
}

}  // namespace pimewin
