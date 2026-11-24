#pragma once

#include <windows.h>
#include <unknwn.h>

namespace pimewin {

// Creates the TSF text service object. The class factory in dllmain.cpp calls
// this with IID_ITfTextInputProcessorEx.
HRESULT CreatePimeTextService(REFIID riid, void** ppv);

}  // namespace pimewin
