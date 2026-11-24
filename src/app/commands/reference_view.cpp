// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/commands/command.h"
#include "app/commands/new_params.h"
#include "app/i18n/strings.h"
#include "app/ui/editor/editor.h"
#include "app/ui/status_bar.h"

namespace app {

// View-only switch between showing only the reference layers and
// only the other layers (layer visibility flags are not modified).
struct ReferenceViewParams : public NewParams {
  // "flip": only reference <-> only other layers
  // "all": back to the normal view
  Param<std::string> mode{ this, "flip", "mode" };
};

class ReferenceViewCommand : public CommandWithNewParams<ReferenceViewParams> {
public:
  ReferenceViewCommand() : CommandWithNewParams<ReferenceViewParams>(CommandId::ReferenceView()) {}

private:
  bool showAll() const { return params().mode() == "all"; }

  bool onEnabled(Context* ctx) override
  {
    return !showAll() || Editor::refView() != render::RefView::All;
  }

  void onExecute(Context* ctx) override
  {
    render::RefView refView;
    std::string tip;
    if (showAll()) {
      refView = render::RefView::All;
      tip = Strings::statusbar_tips_ref_view_all();
    }
    else if (Editor::refView() == render::RefView::RefOnly) {
      refView = render::RefView::OthersOnly;
      tip = Strings::statusbar_tips_ref_view_others_only();
    }
    else {
      refView = render::RefView::RefOnly;
      tip = Strings::statusbar_tips_ref_view_ref_only();
    }
    Editor::setRefView(refView);

    if (StatusBar* statusBar = StatusBar::instance())
      statusBar->showTip(1000, tip);
  }

  std::string onGetFriendlyName() const override
  {
    return showAll() ? Strings::commands_ReferenceView_All() : Strings::commands_ReferenceView();
  }
};

Command* CommandFactory::createReferenceViewCommand()
{
  return new ReferenceViewCommand;
}

} // namespace app
