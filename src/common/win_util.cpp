#include "common/win_util.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>

namespace pimewin {
namespace {

constexpr wchar_t kSettingsPath[] = L"Software\\PIME";

}  // namespace

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size);
    return out;
}

std::wstring ModulePath(HMODULE module) {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied =
            GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (copied == 0) return {};
        if (copied < path.size()) {
            path.resize(copied);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring ModuleDirectory(HMODULE module) {
    std::wstring path = ModulePath(module);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(0, slash);
}

std::wstring PathJoin(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    std::wstring out = left;
    if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
    out += right;
    return out;
}

std::wstring PathFileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty() || DirectoryExists(path)) return true;
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS;
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>* out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = out->empty() ||
                    (ReadFile(file, out->data(), static_cast<DWORD>(out->size()),
                              &read, nullptr) &&
                     read == out->size());
    CloseHandle(file);
    return ok;
}

bool WriteFileBytes(const std::wstring& path,
                    const std::vector<uint8_t>& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok =
        bytes.empty() ||
        (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                   &written, nullptr) &&
         written == bytes.size());
    CloseHandle(file);
    return ok;
}

std::wstring LocalAppDataDirectory() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) {
        return L".";
    }
    std::wstring path(raw);
    CoTaskMemFree(raw);
    return path;
}

std::wstring DataDirectory() {
    return PathJoin(LocalAppDataDirectory(), L"PIME");
}

uint32_t SystemDpiForWindow(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn get_dpi = [] {
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<GetDpiForWindowFn>(
                            GetProcAddress(user32, "GetDpiForWindow"))
                      : nullptr;
    }();
    if (get_dpi && window) {
        const UINT dpi = get_dpi(window);
        if (dpi) return dpi;
    }
    HDC dc = GetDC(nullptr);
    const UINT dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX));
    ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96;
}

uint32_t ReadThemePreference() {
    return RegistryGetDword(HKEY_CURRENT_USER, kSettingsPath, L"Theme", 0);
}

bool SystemUsesLightTheme() {
    return RegistryGetDword(
               HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"AppsUseLightTheme", 1) != 0;
}

std::wstring RegistryGetString(HKEY root, const std::wstring& path,
                               const std::wstring& name,
                               const std::wstring& fallback) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return fallback;
    }
    DWORD type = 0;
    DWORD size = 0;
    std::wstring value;
    if (RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size) ==
            ERROR_SUCCESS &&
        type == REG_SZ && size >= sizeof(wchar_t)) {
        value.resize(size / sizeof(wchar_t));
        if (RegQueryValueExW(key, name.c_str(), nullptr, &type,
                             reinterpret_cast<BYTE*>(value.data()),
                             &size) == ERROR_SUCCESS) {
            while (!value.empty() && value.back() == L'\0') value.pop_back();
        } else {
            value = fallback;
        }
    } else {
        value = fallback;
    }
    RegCloseKey(key);
    return value;
}

uint32_t RegistryGetDword(HKEY root, const std::wstring& path,
                          const std::wstring& name, uint32_t fallback) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return fallback;
    }
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    const bool ok =
        RegQueryValueExW(key, name.c_str(), nullptr, &type,
                         reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD;
    RegCloseKey(key);
    return ok ? value : fallback;
}

bool RegistrySetString(HKEY root, const std::wstring& path,
                       const std::wstring& name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const bool ok =
        RegSetValueExW(key, name.c_str(), 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) ==
        ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

bool RegistrySetDword(HKEY root, const std::wstring& path,
                      const std::wstring& name, uint32_t value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const bool ok = RegSetValueExW(key, name.c_str(), 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&value),
                                   sizeof(value)) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

bool RegistryDeleteTree(HKEY root, const std::wstring& path) {
    const LSTATUS status = RegDeleteTreeW(root, path.c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool elevated = GetTokenInformation(token, TokenElevation, &elevation,
                                              size, &size) &&
                          elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

bool RunElevated(const std::wstring& command_line) {
    std::wstring exe = ModulePath(nullptr);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = command_line.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return false;
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

std::wstring ProcessImageName() {
    const HWND foreground = GetForegroundWindow();
    DWORD process_id = 0;
    if (foreground) {
        GetWindowThreadProcessId(foreground, &process_id);
    }
    if (!process_id) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 process_id);
    if (!process) return {};
    wchar_t path[MAX_PATH * 2] = {};
    DWORD size = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    std::wstring name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        name = PathFileName(path);
    }
    CloseHandle(process);
    return name;
}

UINT ConfigChangedMessage() {
    static const UINT message =
        RegisterWindowMessageW(L"PIME.ConfigChanged.{6C1D9A2E-31B7-4E52-9E0C-8A5D51A3C7F4}");
    return message;
}

void NotifyConfigChanged() {
    const UINT message = ConfigChangedMessage();
    if (!message) return;
    DWORD recipients = BSM_ALLCOMPONENTS;
    BroadcastSystemMessageW(BSF_POSTMESSAGE, &recipients, message, 0, 0);
}

}  // namespace pimewin
