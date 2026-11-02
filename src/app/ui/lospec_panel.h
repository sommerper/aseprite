// Aseprite
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_UI_LOSPEC_PANEL_H_INCLUDED
#define APP_UI_LOSPEC_PANEL_H_INCLUDED
#pragma once

namespace ui {
class TooltipManager;
class Widget;
} // namespace ui

namespace app {

// Lospec palette browser for the color bar: daily tag and palette,
// and a palette search (click a palette to preview the image with it,
// double-click to use it). Needs LOSPEC_API_KEY=... in the .env file
// of the source tree.
ui::Widget* create_lospec_panel(ui::TooltipManager* tooltips);

} // namespace app

#endif
