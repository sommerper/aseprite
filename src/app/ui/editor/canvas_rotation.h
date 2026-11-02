// Aseprite
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_UI_EDITOR_CANVAS_ROTATION_H_INCLUDED
#define APP_UI_EDITOR_CANVAS_ROTATION_H_INCLUDED
#pragma once

#include "gfx/point.h"
#include "gfx/rect.h"
#include "gfx/region.h"
#include "gfx/size.h"
#include "os/surface.h"
#include "ui/layer.h"
#include "ui/timer.h"

#include <memory>

namespace render {
class Zoom;
}
namespace ui {
class Display;
class Graphics;
class Message;
class MouseMessage;
class PaintEvent;
class TooltipManager;
class Widget;
} // namespace ui

namespace app {
class Editor;
namespace tools {
class Tool;
}

// Free canvas rotation around the viewport center (hold the "Rotate
// Canvas" quicktool key and drag, see startGesture()).
//
// This lives outside Editor to keep the fork's changes to upstream
// files small. The Editor keeps painting in its usual unrotated
// "logical" screen space into an offscreen surface, which is drawn
// rotated on the screen. Editor's screen<->editor coordinate
// functions apply the rotation, so physical mouse positions still
// map to the right sprite pixels. Inside an Unrotated scope they
// return logical coordinates (that's how painting works).
class CanvasRotation {
public:
  class Unrotated {
  public:
    explicit Unrotated(const CanvasRotation& rot) : m_rot(rot) { ++m_rot.m_unrotated; }
    ~Unrotated() { --m_rot.m_unrotated; }

  private:
    const CanvasRotation& m_rot;
  };

  // Draws directly on the screen (outside onPaint) through the
  // rotation, e.g. marching ants.
  class DirectDraw {
  public:
    DirectDraw(Editor* editor, ui::Graphics* g);
    ~DirectDraw();

  private:
    Editor* m_editor;
    ui::Graphics* m_g = nullptr;
  };

  // Rotates the brush outline that BrushPreview strokes around "pivot".
  class RotateOutline {
  public:
    RotateOutline(const Editor* editor, ui::Graphics* g, const gfx::Point& pivot, bool enabled);
    ~RotateOutline();

  private:
    ui::Graphics* m_g = nullptr;
  };

  ~CanvasRotation();

  // Degrees, clockwise.
  double angle() const { return m_angle; }
  void setAngle(Editor* editor, double degrees);

  // Quick lerp to the given angle and zoom scale, keeping "spritePos"
  // at "screenPos" while zooming.
  void animateTo(Editor* editor,
                 double angle,
                 double scale,
                 const gfx::Point& screenPos,
                 const gfx::PointF& spritePos);
  double targetAngle() const;
  double targetScale(const Editor* editor) const;

  bool isRotated() const { return m_angle != 0.0; }
  // True if coordinates must be rotated (not inside an Unrotated scope).
  bool active() const { return isRotated() && m_unrotated == 0; }

  // Animates the rotation back to 0 (keeps the zoom).
  void reset(Editor* editor);

  // Starts the rotate/zoom gesture if the "rotate_canvas" tool is active.
  static bool startGesture(Editor* editor, ui::MouseMessage* msg);

  // Double-clicking the dead zone with the "rotate_canvas" tool resets the rotation.
  static bool onDoubleClick(Editor* editor, ui::MouseMessage* msg);

  // True if "msg" presses the "rotate_canvas" quicktool shortcut.
  static bool isQuicktoolPressed(const ui::Message* msg);

  // Adds the "reset rotation" button at the end of the context bar.
  static void addResetButton(ui::Widget* contextBar, ui::TooltipManager* tooltips);

  // Shows the gesture dead zone while the "rotate_canvas" tool is active.
  void onActiveToolChange(Editor* editor, const tools::Tool* tool);

  // Editor hooks, they return false when they don't apply.
  bool paint(Editor* editor, ui::PaintEvent& ev);
  bool drawSpriteClipped(Editor* editor, const gfx::Region& spriteRgn);

  // Zooms keeping "spritePos" at "screenPos".
  void zoomAt(Editor* editor,
              const render::Zoom& zoom,
              const gfx::Point& screenPos,
              const gfx::PointF& spritePos);

  // Rotation-aware versions of Editor coordinate functions.
  gfx::Point screenToEditor(const Editor* editor, const gfx::Point& pt) const;
  gfx::Point screenToEditorCeiling(const Editor* editor, const gfx::Point& pt) const;
  gfx::PointF screenToEditorF(const Editor* editor, const gfx::Point& pt) const;
  gfx::Point editorToScreen(const Editor* editor, const gfx::Point& pt) const;
  gfx::PointF editorToScreenF(const Editor* editor, const gfx::PointF& pt) const;
  gfx::Rect screenToEditor(const Editor* editor, const gfx::Rect& rc) const;
  gfx::Rect editorToScreen(const Editor* editor, const gfx::Rect& rc) const;
  gfx::RectF editorToScreenF(const Editor* editor, const gfx::RectF& rc) const;

  // Converts a screen delta (e.g. a hand tool drag) into a scroll
  // delta. Keeps the rounding remainder so slow drags don't drift.
  gfx::Point toLogicalDelta(const gfx::Point& delta) const;

  // Moves an axis-aligned box (given in logical coords) so its center
  // matches the rotated center.
  gfx::Rect centerToPhysical(const Editor* editor, gfx::Rect rc) const;

  // Size with room to rotate an outline of the given size.
  gfx::Size outlineSize(const gfx::Size& size) const;

private:
  void drawRotated(Editor* editor, ui::Graphics* g, const ui::PaintEvent* ev);
  void onTick(Editor* editor);
  void showDeadZone(Editor* editor, bool show);
  gfx::PointF rotate(const Editor* editor, const gfx::PointF& pt, double sin) const;
  gfx::PointF toPhysical(const Editor* editor, const gfx::PointF& pt) const;
  gfx::PointF toLogical(const Editor* editor, const gfx::PointF& pt) const;
  gfx::Point toLogical(const Editor* editor, const gfx::Point& pt) const;
  gfx::RectF bounds(const Editor* editor, const gfx::RectF& rc, double sin) const;

  double m_angle = 0.0;
  double m_cos = 1.0;
  double m_sin = 0.0;
  mutable int m_unrotated = 0;
  mutable gfx::PointF m_deltaRemainder;
  os::SurfaceRef m_surface; // Offscreen surface reused between paints

  // Animation
  std::unique_ptr<ui::Timer> m_timer;
  double m_targetAngle = 0.0;
  double m_scale = 1.0; // Animated zoom scale (not rounded to a render::Zoom)
  double m_targetScale = 1.0;
  gfx::Point m_zoomAnchor;
  gfx::PointF m_zoomSpriteAnchor;

  ui::UILayerRef m_deadZone;
  ui::Display* m_deadZoneDisplay = nullptr; // Display where m_deadZone is shown
};

} // namespace app

#endif
