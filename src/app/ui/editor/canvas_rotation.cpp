// Aseprite
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/ui/editor/canvas_rotation.h"

#include "app/tools/tool.h"
#include "app/ui/editor/editor.h"
#include "app/ui/editor/editor_state.h"
#include "app/ui/icon_button.h"
#include "app/ui/keyboard_shortcuts.h"
#include "app/ui/skin/skin_theme.h"
#include "app/ui/status_bar.h"
#include "base/pi.h"
#include "fmt/format.h"
#include "gfx/matrix.h"
#include "os/system.h"
#include "render/zoom.h"
#include "ui/display.h"
#include "ui/graphics.h"
#include "ui/message.h"
#include "ui/paint_event.h"
#include "ui/scale.h"
#include "ui/size_hint_event.h"
#include "ui/tooltips.h"
#include "ui/view.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace app {

using namespace ui;

namespace {

// Snapping is applied on release only, snapping while dragging makes the canvas jump.
constexpr double kAngleSnap = 4.0;     // Degrees around multiples of 90°
constexpr double kLerp = 0.45;         // Part of the remaining way animated each 16ms tick

// Around the center the pointer angle is too unstable to rotate.
int dead_zone_radius()
{
  return 40 * guiscale();
}

// Zoom changes with the pointer distance moved to/from the center (not
// the distance ratio, which goes crazy near the center): zoom x e every
// zoom_distance() pixels, ignoring the first zoom_deadband() pixels so a
// wobbly circle doesn't zoom.
double zoom_distance()
{
  return 250.0 * guiscale();
}
double zoom_deadband()
{
  return 30.0 * guiscale();
}

bool is_rotate_tool_active(const Editor* editor)
{
  return editor->getCurrentEditorTool()->getId() == "rotate_canvas";
}

gfx::PointF viewport_center(const Editor* editor)
{
  // Integer center so 90° steps keep screen pixels aligned.
  const gfx::Rect vp = View::getView(editor)->viewportBounds();
  return gfx::PointF(vp.x + vp.w / 2, vp.y + vp.h / 2);
}

gfx::Rect enclosing_rect(const gfx::RectF& rc)
{
  const int x = int(std::floor(rc.x));
  const int y = int(std::floor(rc.y));
  return gfx::Rect(x, y, int(std::ceil(rc.x2())) - x, int(std::ceil(rc.y2())) - y);
}

// Closest zoom where every sprite pixel has the same size (n:1 or 1:n).
// The editor redraws partial areas inconsistently at other zooms (a
// partially redrawn pixel can be 1px off, e.g. the brush preview), so
// those are only used while dragging/animating.
double whole_zoom(double scale)
{
  const double n = (scale >= 1.0 ? scale : 1.0 / scale);
  const double lo = std::floor(n);
  const double m = (n * n < lo * (lo + 1) ? lo : lo + 1); // Closest in log scale
  return (scale >= 1.0 ? m : 1.0 / m);
}

// Any scale as a zoom, not only Aseprite's zoom levels.
render::Zoom zoom_from_scale(double scale)
{
  // ponytail: 1/64 steps keep num/den small (Projection uses int math), finer steps would need
  // a double-based Projection
  constexpr int k = 64;
  scale = std::clamp(scale, 1.0 / 64, 64.0);
  int num = k;
  int den = k;
  if (scale >= 1.0)
    num = int(std::lround(scale * k));
  else
    den = int(std::lround(k / scale));
  const int g = std::gcd(num, den);
  return render::Zoom(num / g, den / g);
}

// Fills the context bar up to "widget" so it stays at the right end.
// (Can't be an expansive BoxFiller: ContextBar's size hint has 0
// width, so ui::Box would give it the whole bar.)
class RightAlign : public Widget {
public:
  explicit RightAlign(Widget* widget) : m_widget(widget) {}

private:
  void onSizeHint(SizeHintEvent& ev) override
  {
    ev.setSizeHint(std::max(0, ev.fitInWidth() - m_widget->sizeHint().w), 0);
  }

  Widget* m_widget;
};

// Pinch-like gesture with one finger fixed at the viewport center:
// moving around the center rotates, moving to/from it zooms.
class RotatingCanvasState : public EditorState {
public:
  bool isTemporalState() const override { return true; }
  LeaveAction onLeaveState(Editor*, EditorState*) override { return DiscardState; }

  bool onMouseDown(Editor* editor, MouseMessage* msg) override
  {
    m_center = viewport_center(editor);
    m_spriteCenter = editor->screenToEditorF(gfx::Point(m_center));
    editor->captureMouse();
    return onMouseMove(editor, msg);
  }

  bool onMouseUp(Editor* editor, MouseMessage* msg) override
  {
    CanvasRotation& rot = editor->canvasRotation();
    double angle = rot.targetAngle();
    const double rightAngle = std::round(angle / 90.0) * 90.0;
    if (std::fabs(angle - rightAngle) < kAngleSnap)
      angle = rightAngle;

    rot.animateTo(editor,
                  angle,
                  whole_zoom(rot.targetScale(editor)),
                  gfx::Point(m_center),
                  m_spriteCenter);

    editor->backToPreviousState();
    editor->releaseMouse();
    return true;
  }

  bool onMouseMove(Editor* editor, MouseMessage* msg) override
  {
    const gfx::PointF v = gfx::PointF(msg->position()) - m_center;
    const double radius = std::hypot(v.x, v.y);
    CanvasRotation& rot = editor->canvasRotation();

    // Wait until the pointer leaves the dead zone and continue from
    // there (re-anchoring, so nothing jumps).
    if (radius < dead_zone_radius()) {
      m_anchored = false;
      return true;
    }
    if (!m_anchored) {
      m_anchored = true;
      m_startAngle = rot.targetAngle();
      m_startPointerAngle = std::atan2(v.y, v.x);
      m_startRadius = radius;
      m_startScale = rot.targetScale(editor);
      return true;
    }

    const double angle =
      m_startAngle + (std::atan2(v.y, v.x) - m_startPointerAngle) * 180.0 / PI;

    // Continuous deadband, no jump when leaving it
    double d = radius - m_startRadius;
    d = std::copysign(std::max(0.0, std::fabs(d) - zoom_deadband()), d);

    rot.animateTo(editor,
                  angle,
                  m_startScale * std::exp(d / zoom_distance()),
                  gfx::Point(m_center),
                  m_spriteCenter);
    editor->updateStatusBar();
    return true;
  }

  bool onSetCursor(Editor* editor, const gfx::Point&) override
  {
    editor->showMouseCursor(kCustomCursor, skin::SkinTheme::get(editor)->cursors.rotateN());
    return true;
  }

  bool onUpdateStatusBar(Editor* editor) override
  {
    StatusBar::instance()->setStatusText(0,
                                         fmt::format("Rotation: {}\xC2\xB0  Zoom: {:.1f}%",
                                                     std::lround(editor->canvasRotation().angle()),
                                                     editor->zoom().scale() * 100.0));
    return true;
  }

private:
  bool m_anchored = false;
  double m_startAngle = 0.0;
  double m_startPointerAngle = 0.0;
  double m_startRadius = 1.0;
  double m_startScale = 1.0;
  gfx::PointF m_center;
  gfx::PointF m_spriteCenter;
};

} // anonymous namespace

CanvasRotation::DirectDraw::DirectDraw(Editor* editor, ui::Graphics* g) : m_editor(editor)
{
  const CanvasRotation& rot = editor->canvasRotation();
  if (!rot.active())
    return;

  // Rotation center in Graphics local coordinates, as Graphics::drawPath()
  // applies the matrix before its offset (the only thing drawn here are paths).
  const gfx::PointF c = viewport_center(editor) - gfx::PointF(editor->bounds().origin());
  gfx::Matrix m;
  m.setRotate(float(rot.m_angle), float(c.x), float(c.y));
  g->save();
  g->concat(m);
  ++rot.m_unrotated;
  m_g = g;
}

CanvasRotation::DirectDraw::~DirectDraw()
{
  if (!m_g)
    return;

  m_g->restore();
  --m_editor->canvasRotation().m_unrotated;
}

CanvasRotation::RotateOutline::RotateOutline(const Editor* editor,
                                             ui::Graphics* g,
                                             const gfx::Point& pivot,
                                             bool enabled)
{
  const CanvasRotation& rot = editor->canvasRotation();
  if (!enabled || !rot.isRotated())
    return;

  gfx::Matrix m;
  m.setRotate(float(rot.m_angle), float(pivot.x), float(pivot.y));
  g->save();
  g->concat(m);
  m_g = g;
}

CanvasRotation::RotateOutline::~RotateOutline()
{
  if (m_g)
    m_g->restore();
}

CanvasRotation::~CanvasRotation()
{
  showDeadZone(nullptr, false);
}

void CanvasRotation::setAngle(Editor* editor, double degrees)
{
  degrees = std::remainder(degrees, 360.0);
  if (degrees == m_angle)
    return;

  m_angle = degrees;
  m_cos = std::cos(degrees * PI / 180.0);
  m_sin = std::sin(degrees * PI / 180.0);
  // Exact values for right angles so they map pixels to pixels.
  if (std::fmod(degrees, 90.0) == 0.0) {
    m_cos = std::round(m_cos);
    m_sin = std::round(m_sin);
  }
  m_deltaRemainder = gfx::PointF();
  editor->invalidate();
}

void CanvasRotation::animateTo(Editor* editor,
                               double angle,
                               double scale,
                               const gfx::Point& screenPos,
                               const gfx::PointF& spritePos)
{
  if (!m_timer) {
    m_timer = std::make_unique<ui::Timer>(16, editor);
    m_timer->Tick.connect([this, editor] { onTick(editor); });
  }
  if (!m_timer->isRunning())
    m_scale = editor->zoom().scale();

  m_targetAngle = angle;
  m_targetScale = std::clamp(scale, 1.0 / 64, 64.0);
  m_zoomAnchor = screenPos;
  m_zoomSpriteAnchor = spritePos;

  if (!m_timer->isRunning()) {
    m_timer->start();
    onTick(editor); // First step right away
  }
}

double CanvasRotation::targetAngle() const
{
  return (m_timer && m_timer->isRunning() ? m_targetAngle : m_angle);
}

double CanvasRotation::targetScale(const Editor* editor) const
{
  return (m_timer && m_timer->isRunning() ? m_targetScale : editor->zoom().scale());
}

void CanvasRotation::onTick(Editor* editor)
{
  HideBrushPreview hide(editor->brushPreview());

  const double da = std::remainder(m_targetAngle - m_angle, 360.0);
  const double ds = std::log(m_targetScale / m_scale);
  const bool done = (std::fabs(da) < 0.05 && std::fabs(ds) < 0.001);

  setAngle(editor, done ? m_targetAngle : m_angle + kLerp * da);
  m_scale = (done ? m_targetScale : m_scale * std::exp(kLerp * ds));

  const render::Zoom zoom = zoom_from_scale(m_scale);
  if (zoom != editor->zoom())
    zoomAt(editor, zoom, m_zoomAnchor, m_zoomSpriteAnchor);

  if (done)
    m_timer->stop();
  editor->updateStatusBar();
}

void CanvasRotation::onActiveToolChange(Editor* editor, const tools::Tool* tool)
{
  showDeadZone(editor, tool && tool->getId() == "rotate_canvas" && editor->isActive());
}

// A 90% transparent white circle on a display layer (no editor repaint needed).
void CanvasRotation::showDeadZone(Editor* editor, bool show)
{
  if (m_deadZoneDisplay) {
    m_deadZoneDisplay->removeLayer(m_deadZone);
    m_deadZoneDisplay = nullptr;
  }
  if (!show || !editor->display())
    return;

  const int r = dead_zone_radius();
  const int d = 2 * r + 2;
  if (!m_deadZone || m_deadZone->surface()->width() != d) {
    os::SurfaceRef surface = os::System::instance()->makeRgbaSurface(d, d);
    os::SurfaceLock lock(surface.get());
    surface->clear();
    os::Paint paint;
    paint.antialias(true);
    paint.style(os::Paint::Fill);
    paint.color(gfx::rgba(255, 255, 255, 26));
    surface->drawCircle(d / 2.0f, d / 2.0f, float(r), paint);

    m_deadZone = UILayer::Make();
    m_deadZone->setSurface(surface);
  }

  const gfx::Point c(viewport_center(editor));
  gfx::Region clip;
  editor->getDrawableRegion(clip, Widget::kCutTopWindows);
  m_deadZone->setPosition(c - gfx::Point(d / 2, d / 2));
  m_deadZone->setClipRegion(clip);

  m_deadZoneDisplay = editor->display();
  m_deadZoneDisplay->addLayer(m_deadZone);
}

// static
void CanvasRotation::addResetButton(ui::Widget* contextBar, ui::TooltipManager* tooltips)
{
  auto* button = new IconButton(skin::SkinTheme::get(contextBar)->parts.arrowCircleCw());
  button->InitTheme.connect(
    [button] { button->setIcon(skin::SkinTheme::get(button)->parts.arrowCircleCw()); });
  button->Click.connect([] {
    if (Editor* editor = Editor::activeEditor())
      editor->canvasRotation().reset(editor);
  });
  tooltips->addTooltipFor(button, "Reset canvas rotation", BOTTOM);

  contextBar->addChild(new RightAlign(button));
  contextBar->addChild(button);
}

void CanvasRotation::reset(Editor* editor)
{
  animateTo(editor,
            0.0,
            targetScale(editor),
            gfx::Point(viewport_center(editor)),
            editor->spritePointInCenter());
}

// static
bool CanvasRotation::onDoubleClick(Editor* editor, ui::MouseMessage* msg)
{
  const gfx::PointF v = gfx::PointF(msg->position()) - viewport_center(editor);
  if (msg->middle() || !is_rotate_tool_active(editor) ||
      std::hypot(v.x, v.y) >= dead_zone_radius())
    return false;

  editor->canvasRotation().reset(editor);
  return true;
}

// static
bool CanvasRotation::startGesture(Editor* editor, ui::MouseMessage* msg)
{
  if (msg->middle() || !is_rotate_tool_active(editor))
    return false;

  EditorStatePtr state(new RotatingCanvasState);
  editor->setState(state);
  state->onMouseDown(editor, msg);
  return true;
}

// static
bool CanvasRotation::isQuicktoolPressed(const ui::Message* msg)
{
  ui::Shortcut shortcut;
  const tools::Tool* tool =
    KeyboardShortcuts::instance()->getCurrentQuicktool(msg, nullptr, shortcut);
  return tool && tool->getId() == "rotate_canvas";
}

bool CanvasRotation::paint(Editor* editor, ui::PaintEvent& ev)
{
  if (!active())
    return false;

  drawRotated(editor, ev.graphics(), &ev);
  return true;
}

bool CanvasRotation::drawSpriteClipped(Editor* editor, const gfx::Region& spriteRgn)
{
  if (!isRotated())
    return false;

  // Inside our own painting this would land unrotated on the screen,
  // and the area is being painted anyway.
  if (m_unrotated)
    return true;

  gfx::Region rgn(spriteRgn);
  editor->expandRegionByTiledMode(rgn, false);

  gfx::Region screenRgn;
  for (const gfx::Rect& rc : rgn)
    screenRgn |= gfx::Region(editorToScreen(editor, rc).enlarge(1));

  gfx::Region drawable;
  editor->getDrawableRegion(drawable, Widget::kCutTopWindows);
  screenRgn &= drawable;
  screenRgn.offset(-editor->bounds().origin());

  // Draw now (like the unrotated version does) instead of
  // invalidating: the brush preview calls this from inside paints.
  GraphicsPtr g = editor->getGraphics(editor->clientBounds());
  for (const gfx::Rect& rc : screenRgn) {
    IntersectClip clip(g.get(), rc);
    if (clip)
      drawRotated(editor, g.get(), nullptr);
  }
  return true;
}

// Paints the editor (or only its sprite area if "ev" is nullptr) in
// an offscreen surface and draws it rotated in the clip bounds of "g".
void CanvasRotation::drawRotated(Editor* editor, ui::Graphics* g, const ui::PaintEvent* ev)
{
  const gfx::Rect clip = g->getClipBounds();
  if (clip.isEmpty())
    return;

  // Also marks the clip as dirty (drawing through the matrix below
  // marks the unrotated bounds).
  const gfx::Color face = skin::SkinTheme::get(editor)->colors.editorFace();
  g->fillRect(face, clip);

  // Area in logical editor client coordinates that covers the clip once rotated.
  const gfx::Point origin = editor->bounds().origin();
  const gfx::RectF logical = bounds(editor, gfx::Rect(clip).offset(origin), -m_sin);
  const gfx::Rect area = enclosing_rect(logical).offset(-origin).enlarge(1);

  os::Surface* dst = g->getInternalSurface();
  if (!m_surface || m_surface->width() < area.w || m_surface->height() < area.h ||
      m_surface->colorSpace() != dst->colorSpace()) {
    const int w = std::max(area.w, m_surface ? m_surface->width() : 0);
    const int h = std::max(area.h, m_surface ? m_surface->height() : 0);
    m_surface = os::System::instance()->makeRgbaSurface(w, h, dst->colorSpace());
  }

  {
    Unrotated unrotated(*this);
    Graphics offscreen(nullptr, m_surface, -area.x, -area.y);
    offscreen.saveClip();
    offscreen.clipRect(area);
    offscreen.fillRect(face, area);

    if (ev) {
      PaintEvent offscreenEv(editor, &offscreen);
      offscreenEv.setTransparentBg(ev->isTransparentBg());
      editor->onPaint(offscreenEv);
    }
    // Don't use onPaint() here, it hides/shows the brush preview,
    // which is what calls us in the middle of its hide()/show().
    else if (editor->sprite()) {
      editor->drawBackground(&offscreen);
      editor->drawSpriteUnclippedRect(&offscreen, editor->sprite()->bounds());
    }

    offscreen.restoreClip();
  }

  const gfx::PointF c = viewport_center(editor) - gfx::PointF(origin) +
                        gfx::PointF(g->getInternalDeltaX(), g->getInternalDeltaY());
  gfx::Matrix m;
  m.setRotate(float(m_angle), float(c.x), float(c.y));

  const bool rightAngle = (std::fmod(m_angle, 90.0) == 0.0);
  g->save();
  g->concat(m);
  g->drawSurface(m_surface.get(),
                 gfx::Rect(0, 0, area.w, area.h),
                 area,
                 os::Sampling(rightAngle ? os::Sampling::Filter::Nearest :
                                           os::Sampling::Filter::Linear),
                 nullptr);
  g->restore();
}

void CanvasRotation::zoomAt(Editor* editor,
                            const render::Zoom& zoom,
                            const gfx::Point& screenPos,
                            const gfx::PointF& spritePos)
{
  HideBrushPreview hide(editor->brushPreview());
  const gfx::PointF logicalPos = toLogical(editor, gfx::PointF(screenPos));

  render::Projection proj = editor->projection();
  proj.setZoom(zoom);
  const gfx::Point padding = editor->calcExtraPadding(proj);
  const gfx::Rect vp = View::getView(editor)->viewportBounds();
  const gfx::Point scroll(
    int(std::lround(vp.x + padding.x + proj.applyX(spritePos.x) - logicalPos.x)),
    int(std::lround(vp.y + padding.y + proj.applyY(spritePos.y) - logicalPos.y)));

  editor->setZoom(zoom);
  editor->updateEditor(false);
  editor->setEditorScroll(scroll);
}

gfx::Point CanvasRotation::screenToEditor(const Editor* editor, const gfx::Point& pt) const
{
  const gfx::Point lpt = toLogical(editor, pt);
  Unrotated unrotated(*this);
  return editor->screenToEditor(lpt);
}

gfx::Point CanvasRotation::screenToEditorCeiling(const Editor* editor, const gfx::Point& pt) const
{
  const gfx::Point lpt = toLogical(editor, pt);
  Unrotated unrotated(*this);
  return editor->screenToEditorCeiling(lpt);
}

gfx::PointF CanvasRotation::screenToEditorF(const Editor* editor, const gfx::Point& pt) const
{
  const gfx::Point lpt = toLogical(editor, pt);
  Unrotated unrotated(*this);
  return editor->screenToEditorF(lpt);
}

gfx::Point CanvasRotation::editorToScreen(const Editor* editor, const gfx::Point& pt) const
{
  Unrotated unrotated(*this);
  const gfx::PointF p = toPhysical(editor, gfx::PointF(editor->editorToScreen(pt)));
  return gfx::Point(int(std::lround(p.x)), int(std::lround(p.y)));
}

gfx::PointF CanvasRotation::editorToScreenF(const Editor* editor, const gfx::PointF& pt) const
{
  Unrotated unrotated(*this);
  return toPhysical(editor, editor->editorToScreenF(pt));
}

gfx::Rect CanvasRotation::screenToEditor(const Editor* editor, const gfx::Rect& rc) const
{
  Unrotated unrotated(*this);
  return editor->screenToEditor(enclosing_rect(bounds(editor, rc, -m_sin)));
}

gfx::Rect CanvasRotation::editorToScreen(const Editor* editor, const gfx::Rect& rc) const
{
  Unrotated unrotated(*this);
  return enclosing_rect(bounds(editor, editor->editorToScreen(rc), m_sin));
}

gfx::RectF CanvasRotation::editorToScreenF(const Editor* editor, const gfx::RectF& rc) const
{
  Unrotated unrotated(*this);
  return bounds(editor, editor->editorToScreenF(rc), m_sin);
}

gfx::Point CanvasRotation::toLogicalDelta(const gfx::Point& delta) const
{
  if (!isRotated())
    return delta;

  const double x = delta.x * m_cos + delta.y * m_sin + m_deltaRemainder.x;
  const double y = -delta.x * m_sin + delta.y * m_cos + m_deltaRemainder.y;
  const gfx::Point result(int(std::lround(x)), int(std::lround(y)));
  m_deltaRemainder = gfx::PointF(x - result.x, y - result.y);
  return result;
}

gfx::Rect CanvasRotation::centerToPhysical(const Editor* editor, gfx::Rect rc) const
{
  if (!isRotated())
    return rc;

  const gfx::PointF c = toPhysical(editor, gfx::PointF(rc.x + rc.w / 2.0, rc.y + rc.h / 2.0));
  rc.setOrigin(gfx::Point(int(std::lround(c.x - rc.w / 2.0)), int(std::lround(c.y - rc.h / 2.0))));
  return rc;
}

gfx::Size CanvasRotation::outlineSize(const gfx::Size& size) const
{
  if (!isRotated())
    return size;

  // Grow by even amounts so BrushPreview keeps the outline centered at the same point.
  const int d = int(std::ceil(std::hypot(size.w, size.h)));
  return gfx::Size(size.w + (std::max(0, d - size.w) + 1) / 2 * 2,
                   size.h + (std::max(0, d - size.h) + 1) / 2 * 2);
}

gfx::PointF CanvasRotation::rotate(const Editor* editor, const gfx::PointF& pt, double sin) const
{
  const gfx::PointF c = viewport_center(editor);
  const double x = pt.x - c.x;
  const double y = pt.y - c.y;
  return gfx::PointF(c.x + x * m_cos - y * sin, c.y + x * sin + y * m_cos);
}

gfx::PointF CanvasRotation::toPhysical(const Editor* editor, const gfx::PointF& pt) const
{
  return rotate(editor, pt, m_sin);
}

gfx::PointF CanvasRotation::toLogical(const Editor* editor, const gfx::PointF& pt) const
{
  return rotate(editor, pt, -m_sin);
}

// Logical pixel that contains the center of the given physical pixel.
gfx::Point CanvasRotation::toLogical(const Editor* editor, const gfx::Point& pt) const
{
  const gfx::PointF p = toLogical(editor, gfx::PointF(pt.x + 0.5, pt.y + 0.5));
  return gfx::Point(int(std::floor(p.x)), int(std::floor(p.y)));
}

// Bounding box of the rectangle rotated by asin(sin).
gfx::RectF CanvasRotation::bounds(const Editor* editor, const gfx::RectF& rc, double sin) const
{
  const gfx::PointF corners[] = {
    rotate(editor, gfx::PointF(rc.x, rc.y), sin),
    rotate(editor, gfx::PointF(rc.x2(), rc.y), sin),
    rotate(editor, gfx::PointF(rc.x, rc.y2()), sin),
    rotate(editor, gfx::PointF(rc.x2(), rc.y2()), sin),
  };
  gfx::PointF a = corners[0], b = corners[0];
  for (const gfx::PointF& p : corners) {
    a = gfx::PointF(std::min(a.x, p.x), std::min(a.y, p.y));
    b = gfx::PointF(std::max(b.x, p.x), std::max(b.y, p.y));
  }
  return gfx::RectF(a.x, a.y, b.x - a.x, b.y - a.y);
}

} // namespace app
