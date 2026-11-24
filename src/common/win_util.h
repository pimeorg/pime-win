// Small Win32 helpers shared by the TSF service, the settings app and tools.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pimewin {

std::string ToUtf8(const std::wstring& text);
std::wstring ToWide(const std::string& text);

std::wstring ModulePath(HMODULE module);
std::wstring ModuleDirectory(HMODULE module);
std::wstring PathJoin(const std::wstring& left, const std::wstring& right);
std::wstring PathFileName(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
bool FileExists(const std::wstring& path);
bool EnsureDirectory(const std::wstring& path);
bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>* out);
std::wstring LocalAppDataDirectory();
std::wstring DataDirectory();

std::wstring ProcessImageName();
uint32_t SystemDpiForWindow(HWND window);

// Returns 0 for "follow system", 1 for light, 2 for dark.
uint32_t ReadThemePreference();
bool SystemUsesLightTheme();

std::wstring RegistryGetString(HKEY root, const std::wstring& path,
                               const std::wstring& name,
                               const std::wstring& fallback);
uint32_t RegistryGetDword(HKEY root, const std::wstring& path,
                          const std::wstring& name, uint32_t fallback);
bool RegistrySetString(HKEY root, const std::wstring& path,
                       const std::wstring& name, const std::wstring& value);
bool RegistrySetDword(HKEY root, const std::wstring& path,
                      const std::wstring& name, uint32_t value);
bool RegistryDeleteTree(HKEY root, const std::wstring& path);

// True when the process token is a member of the local Administrators group
// and is running elevated.
bool IsElevated();

// Restarts this executable with the given command line through the UAC prompt.
bool RunElevated(const std::wstring& command_line);

// Message broadcast to text services when settings change.
UINT ConfigChangedMessage();
void NotifyConfigChanged();

}  // namespace pimewin
