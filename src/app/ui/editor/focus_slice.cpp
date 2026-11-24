// Aseprite
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/ui/editor/focus_slice.h"

#include "app/ui/editor/editor.h"
#include "doc/slice.h"
#include "doc/slices.h"
#include "doc/sprite.h"
#include "render/zoom.h"
#include "ui/system.h"
#include "ui/view.h"

#include <algorithm>

namespace app {

// Fraction of the viewport that the slice takes, so it doesn't touch
// the editor borders.
static const double kFill = 0.8;

void focus_slice_in_editor(const std::string& sliceName)
{
  ui::execute_from_ui_thread([sliceName] {
    Editor* editor = Editor::activeEditor();
    if (!editor || !editor->sprite())
      return;

    const doc::Slice* slice = editor->sprite()->slices().getByName(sliceName);
    if (!slice)
      return;

    const doc::SliceKey* key = slice->getByFrame(editor->frame());
    if (!key || key->bounds().isEmpty())
      return;

    const gfx::Rect bounds = key->bounds();
    const gfx::Size vp = ui::View::getView(editor)->viewportBounds().size();
    if (vp.w < 1 || vp.h < 1)
      return;

    editor->setZoom(
      render::Zoom::fromScale(std::min(kFill * vp.w / bounds.w, kFill * vp.h / bounds.h)));
    // Needed so the editor padding is recalculated for the new zoom
    // before centerInSpritePoint() uses it.
    editor->updateEditor(false);
    editor->selectSlice(slice);
    editor->centerInSpritePoint(bounds.center());
  });
}

} // namespace app
