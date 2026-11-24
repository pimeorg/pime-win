// Display attribute provider plumbing: one dotted-underline attribute for the
// composition text. The attribute is what tells TSF-aware applications how to
// draw the preedit while it is still being edited.
#pragma once

#include <windows.h>
#include <msctf.h>

namespace pimewin {

HRESULT CreateDisplayAttributeInfo(REFGUID guid,
                                   ITfDisplayAttributeInfo** out);
HRESULT CreateDisplayAttributeEnumerator(IEnumTfDisplayAttributeInfo** out);

}  // namespace pimewin
