// Aseprite
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_UI_EDITOR_FOCUS_SLICE_H_INCLUDED
#define APP_UI_EDITOR_FOCUS_SLICE_H_INCLUDED
#pragma once

#include <string>

namespace app {

// Zooms and centers the active editor on the given slice, and selects
// it. Used by the --slice CLI option when the UI is running (e.g. to
// jump to the slice an external editor/engine asked for).
//
// The work is queued in the UI message loop because this is called
// while the CLI is still being processed, when the editor doesn't have
// its final size yet.
void focus_slice_in_editor(const std::string& sliceName);

} // namespace app

#endif
