#pragma once

#include <windows.h>

namespace pimewin {

// Creates the settings window and runs its message loop. The caller owns COM.
int RunSettingsApp(HINSTANCE instance);

}  // namespace pimewin
