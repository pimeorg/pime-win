#pragma once

#include <windows.h>

namespace pimewin {

// RichEdit host used to exercise the text service outside a real editor.
// Activating the profile for this process only (--pime) keeps the user's
// system input method untouched.
int RunDemoApp(HINSTANCE instance, bool request_pime);

}  // namespace pimewin
