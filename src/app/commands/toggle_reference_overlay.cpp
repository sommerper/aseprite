// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/commands/command.h"
#include "app/i18n/strings.h"
#include "app/pref/preferences.h"
#include "app/ui/status_bar.h"

namespace app {

// Turns on/off the fading of all layers above the topmost reference
// layer (displayed as one merged layer). The opacity is configured
// with the slider in the context bar. It only affects the editor
// view (not the preview window, export, or saved files).
class ToggleReferenceOverlayCommand : public Command {
public:
  ToggleReferenceOverlayCommand() : Command(CommandId::ToggleReferenceOverlay()) {}

private:
  bool onChecked(Context* ctx) override
  {
    return Preferences::instance().editor.refOverlayEnabled();
  }

  void onExecute(Context* ctx) override
  {
    auto& pref = Preferences::instance().editor;
    pref.refOverlayEnabled(!pref.refOverlayEnabled());

    if (ctx->isUIAvailable()) {
      if (StatusBar* statusBar = StatusBar::instance()) {
        statusBar->showTip(1000,
                           pref.refOverlayEnabled() ?
                             Strings::statusbar_tips_ref_overlay_on(pref.refOverlayOpacity()) :
                             Strings::statusbar_tips_ref_overlay_off());
      }
    }
  }
};

Command* CommandFactory::createToggleReferenceOverlayCommand()
{
  return new ToggleReferenceOverlayCommand;
}

} // namespace app
