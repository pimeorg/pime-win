// Per-machine registration of the TSF text service: COM in-proc server,
// zh-CN keyboard profile and the TSF categories that describe it.
#pragma once

#include <string>

namespace pimewin {

bool RegisterTextService(const std::wstring& dll_path);
bool UnregisterTextService();

// Adds the registered profile to the current user's input-method list.
// Must run in a fresh process: input.dll ignores a profile that was just
// registered by the calling process, so the installer spawns itself once more.
bool AddToUserInputList();

// Switches the current thread's active TIP to PIME (used by the settings app
// so the user can start typing without digging through the language bar).
bool ActivateTextService();

}  // namespace pimewin
