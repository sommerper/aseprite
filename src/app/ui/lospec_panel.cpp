// Aseprite
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/ui/lospec_panel.h"

#include "app/commands/cmd_set_palette.h"
#include "app/commands/command.h"
#include "app/commands/commands.h"
#include "app/context_observer.h"
#include "app/doc.h"
#include "app/resource_finder.h"
#include "app/site.h"
#include "app/tools/ink.h"
#include "app/ui/editor/editor.h"
#include "app/ui/editor/editor_render.h"
#include "app/ui/search_entry.h"
#include "app/ui/skin/skin_theme.h"
#include "app/ui_context.h"
#include "base/fs.h"
#include "base/string.h"
#include "base/trim_string.h"
#include "doc/image_bits.h"
#include "doc/image_ref.h"
#include "doc/palette.h"
#include "doc/sprite.h"
#include "fmt/format.h"
#include "json11.hpp"
#include "net/http_headers.h"
#include "net/http_request.h"
#include "net/http_response.h"
#include "render/render.h"
#include "ui/box.h"
#include "ui/combobox.h"
#include "ui/graphics.h"
#include "ui/int_entry.h"
#include "ui/link_label.h"
#include "ui/listbox.h"
#include "ui/listitem.h"
#include "ui/manager.h"
#include "ui/menu.h"
#include "ui/message.h"
#include "ui/paint_event.h"
#include "ui/scale.h"
#include "ui/size_hint_event.h"
#include "ui/timer.h"
#include "ui/tooltips.h"
#include "ui/view.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace app {

using namespace ui;
using namespace app::skin;

namespace {

const char* kApiUrl = "https://api.lospec.com/api/v1/";
const char* kDailiesUrl = "https://lospec.com/dailies/";
const int kPageSize = 30;
const char* kCountParams[] = { "", "maxColors", "minColors", "numberOfColors" };
const char* kSorts[] = { "-publishedAt", "-downloads", "-likes" };

struct LospecPalette {
  std::string slug;
  std::string title;
  std::string author;
  std::vector<gfx::Color> colors;
};

// LOSPEC_API_KEY=... from the .env file of the source tree (the path
// comes from CMake), read at runtime so the key isn't in the binary.
std::string read_api_key()
{
  const std::string prefix = "LOSPEC_API_KEY=";
  std::ifstream file(LOSPEC_ENV_FILE);
  std::string line;
  while (std::getline(file, line)) {
    if (line.compare(0, prefix.size(), prefix) != 0)
      continue;

    std::string key = line.substr(prefix.size());
    auto junk = [](unsigned char c) { return std::isspace(c) || c == '"' || c == '\''; };
    key.erase(std::remove_if(key.begin(), key.end(), junk), key.end());
    return key;
  }
  return std::string();
}

std::string url_encode(const std::string& str)
{
  std::string out;
  for (const unsigned char c : str) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      out += char(c);
    else
      out += fmt::format("%{:02X}", c);
  }
  return out;
}

// Responses are saved in <user config>/lospec/ so the panel works offline.
std::string cache_file(const std::string& path)
{
  ResourceFinder rf;
  rf.includeUserDir("lospec");
  const std::string dir = rf.getFirstOrCreateDefault();
  if (!base::is_directory(dir))
    base::make_all_directories(dir);

  std::string name = path;
  for (char& c : name)
    if (!std::isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.')
      c = '_';
  return base::join_path(dir, name + ".json");
}

json11::Json read_cache(const std::string& file)
{
  std::ifstream in(file);
  std::stringstream buf;
  buf << in.rdbuf();
  std::string err;
  return json11::Json::parse(buf.str(), err); // Null if missing/broken
}

void write_cache(const std::string& file, const json11::Json& data)
{
  std::ofstream(file) << data.dump();
}

LospecPalette palette_from_json(const json11::Json& json)
{
  LospecPalette pal;
  pal.slug = json["slug"].string_value();
  pal.title = json["title"].string_value();
  pal.author = (json["userName"].is_string() ? json["userName"].string_value() :
                                               json["user"]["name"].string_value());
  for (const auto& hex : json["colors"].array_items()) {
    const unsigned long rgb = std::strtoul(hex.string_value().c_str(), nullptr, 16);
    pal.colors.push_back(gfx::rgba((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255));
  }
  return pal;
}

doc::Palette to_doc_palette(const std::vector<gfx::Color>& colors)
{
  doc::Palette palette(doc::frame_t(0), int(colors.size()));
  for (int i = 0; i < int(colors.size()); ++i)
    palette.setEntry(
      i,
      doc::rgba(gfx::getr(colors[i]), gfx::getg(colors[i]), gfx::getb(colors[i]), 255));
  return palette;
}

// The frame as it would look in the palette: each pixel gets its
// nearest palette color (alpha is kept).
doc::ImageRef recolor(const doc::Sprite* sprite, doc::frame_t frame, const doc::Palette& palette)
{
  doc::ImageRef image(doc::Image::create(doc::IMAGE_RGB, sprite->width(), sprite->height()));
  render::Render().renderSprite(image.get(), sprite, frame);

  std::unordered_map<doc::color_t, doc::color_t> nearest;
  for (doc::color_t& c : doc::LockImageBits<doc::RgbTraits>(image.get())) {
    if (doc::rgba_geta(c) == 0)
      continue;
    auto [it, isNew] = nearest.try_emplace(c);
    if (isNew) {
      const int i =
        palette.findBestfit(doc::rgba_getr(c), doc::rgba_getg(c), doc::rgba_getb(c), 255, -1);
      it->second = (palette.getEntry(i) & doc::rgba_rgb_mask) | (c & doc::rgba_a_mask);
    }
    c = it->second;
  }
  return image;
}

// GET request to the Lospec API in a background thread, polled from
// the UI thread (like HttpLoader).
class Request {
public:
  using Handler = std::function<void(const json11::Json& data, const std::string& error)>;

  Request(const std::string& path, const std::string& key, Handler handler)
    : m_handler(std::move(handler))
    , m_request(kApiUrl + path)
  {
    net::HttpHeaders headers;
    headers.setHeader("Authorization", "Bearer " + key);
    m_request.setHeaders(headers);
    m_thread = std::thread([this] { run(); });
  }

  ~Request()
  {
    m_request.abort();
    m_thread.join();
  }

  bool done() const { return m_done; }
  void handle() { m_handler(m_data, m_error); }

private:
  void run()
  {
    std::stringstream body;
    net::HttpResponse response(&body);
    if (!m_request.send(response))
      m_error = "No connection";
    else if (response.status() == 401)
      m_error = "Invalid API key";
    else if (response.status() == 429)
      m_error = "Rate limited";
    else if (response.status() != 200)
      m_error = fmt::format("Lospec error {}", response.status());
    else {
      std::string err;
      m_data = json11::Json::parse(body.str(), err)["data"];
      if (!err.empty())
        m_error = "Bad response";
    }
    m_done = true;
  }

  Handler m_handler;
  net::HttpRequest m_request;
  std::thread m_thread;
  std::atomic<bool> m_done = false;
  json11::Json m_data;
  std::string m_error;
};

// Palette name, author and a strip with all its colors.
class PaletteItem : public ListItem {
public:
  explicit PaletteItem(LospecPalette pal) : ListItem(pal.title), m_pal(std::move(pal)) {}

  const LospecPalette& palette() const { return m_pal; }

  obs::signal<void(const gfx::Point&)> RightClick;

private:
  static int stripHeight() { return 8 * guiscale(); }

  bool onProcessMessage(Message* msg) override
  {
    if (msg->type() == kMouseDownMessage && static_cast<MouseMessage*>(msg)->right()) {
      RightClick(static_cast<MouseMessage*>(msg)->position());
      return true;
    }
    return ListItem::onProcessMessage(msg);
  }

  void onSizeHint(SizeHintEvent& ev) override
  {
    ev.setSizeHint(0, textHeight() + stripHeight() + 7 * guiscale());
  }

  void onPaint(PaintEvent& ev) override
  {
    Graphics* g = ev.graphics();
    auto theme = SkinTheme::get(this);
    const bool selected = isSelected();
    const gfx::Color fg = (selected ? theme->colors.listitemSelectedText() :
                                      theme->colors.listitemNormalText());
    gfx::Rect rc = clientBounds();

    g->fillRect(selected ? theme->colors.listitemSelectedFace() :
                           theme->colors.listitemNormalFace(),
                rc);
    const int s = guiscale();
    rc.shrink(gfx::Border(2 * s, s, 2 * s, 3 * s));

    g->drawText(text(), fg, gfx::ColorNone, rc.origin());

    // Author at the right if there is room
    const int authorW = g->measureText(m_pal.author).w;
    if (!m_pal.author.empty() && g->measureText(text()).w + authorW + 4 * guiscale() < rc.w) {
      g->drawText(m_pal.author,
                  selected ? fg : theme->colors.disabled(),
                  gfx::ColorNone,
                  gfx::Point(rc.x2() - authorW, rc.y));
    }

    const gfx::Rect strip(rc.x, rc.y2() - stripHeight(), rc.w, stripHeight());
    g->drawRect(gfx::seta(fg, 96), gfx::Rect(strip).enlarge(1));
    const int n = int(m_pal.colors.size());
    for (int i = 0; i < n; ++i) {
      const int x0 = strip.x + strip.w * i / n;
      const int x1 = strip.x + strip.w * (i + 1) / n;
      g->fillRect(m_pal.colors[i], gfx::Rect(x0, strip.y, std::max(1, x1 - x0), strip.h));
    }
  }

  LospecPalette m_pal;
};

// "More..." and tag suggestions in the results.
class ActionItem : public ListItem {
public:
  ActionItem(const std::string& text, std::function<void()> action)
    : ListItem(text)
    , action(std::move(action))
  {
  }

  const std::function<void()> action;

private:
  void onSizeHint(SizeHintEvent& ev) override
  {
    ListItem::onSizeHint(ev);
    ev.setSizeHint(0, ev.sizeHint().h); // Long tags don't scroll the list sideways
  }
};

// "Lospec ------" section title.
class Header : public Widget {
public:
  explicit Header(const std::string& text)
  {
    setText(text);
    setTransparent(true);
  }

private:
  void onSizeHint(SizeHintEvent& ev) override
  {
    ev.setSizeHint(0, textHeight() + 4 * guiscale());
  }

  void onPaint(PaintEvent& ev) override
  {
    Graphics* g = ev.graphics();
    const gfx::Color color = SkinTheme::get(this)->colors.workspaceText();
    const gfx::Rect rc = clientBounds();
    const int textW = g->measureText(text()).w;
    const int y = rc.y + (rc.h - textHeight()) / 2;
    g->drawText(text(), color, gfx::ColorNone, gfx::Point(rc.x, y));
    const int lineX = rc.x + textW + 3 * guiscale();
    g->drawHLine(color, lineX, rc.y + rc.h / 2, rc.x2() - lineX);
  }
};

class SearchField : public SearchEntry {
public:
  obs::signal<void()> Enter;

private:
  bool onProcessMessage(Message* msg) override
  {
    if (msg->type() == kKeyDownMessage && hasFocus()) {
      const KeyScancode scancode = static_cast<KeyMessage*>(msg)->scancode();
      if (scancode == kKeyEnter || scancode == kKeyEnterPad) {
        Enter();
        return true;
      }
    }
    return SearchEntry::onProcessMessage(msg);
  }
};

class LospecPanel : public VBox,
                    public ContextObserver {
public:
  explicit LospecPanel(TooltipManager* tooltips)
    : m_tooltips(tooltips)
    , m_key(read_api_key())
    , m_header("Lospec")
    , m_tag(kDailiesUrl, "")
    , m_timer(50, this)
  {
    m_search.setPlaceholder("Name/#tag");
    for (const char* mode : { "Any", "Max", "Min", "=" })
      m_countMode.addItem(mode);
    for (const char* sort : { "Newest", "Downloads", "Likes" })
      m_sort.addItem(sort);
    m_countMode.setExpansive(true);
    m_count.setValue(8);
    m_count.setEnabled(false);
    m_tooltips->addTooltipFor(&m_search,
                              "Enter: search a name, a #tag, or (empty) browse everything",
                              BOTTOM);
    m_tooltips->addTooltipFor(&m_countMode, "Number of colors", BOTTOM);
    m_tooltips->addTooltipFor(&m_sort, "Sort order (not for name searches)", BOTTOM);
    m_view.attachToView(&m_results);
    m_view.setExpansive(true);
    m_view.setVisible(false);
    m_status.setVisible(false);
    m_tooltips->addTooltipFor(&m_tag, "Lospec daily tag (click to open the dailies)", BOTTOM);

    addChild(&m_header);
    addChild(&m_tag);
    addChild(&m_daily);
    addChild(&m_search);
    m_countRow.addChild(&m_countMode);
    m_countRow.addChild(&m_count);
    addChild(&m_countRow);
    addChild(&m_sort);
    addChild(&m_status);
    addChild(&m_view);

    m_daily.Change.connect([this] { preview(m_daily, m_results); });
    m_results.Change.connect([this] {
      if (auto* item = dynamic_cast<ActionItem*>(m_results.getSelectedChild()))
        item->action();
      else
        preview(m_results, m_daily);
    });
    m_daily.DoubleClickItem.connect([this] { usePalette(m_daily); });
    m_results.DoubleClickItem.connect([this] { usePalette(m_results); });
    m_search.Enter.connect([this] { search(); });
    m_search.Change.connect([this] { m_sort.setEnabled(!isNameQuery()); });
    m_countMode.Change.connect([this] {
      m_count.setEnabled(m_countMode.getSelectedItemIndex() > 0);
      search();
    });
    m_count.Change.connect([this] { m_countTimer.start(); }); // Not on every digit
    m_countTimer.Tick.connect([this] {
      m_countTimer.stop();
      search();
    });
    m_sort.Change.connect([this] { search(); });
    m_timer.Tick.connect([this] { poll(); });

    if (m_key.empty()) {
      setStatus("No API key");
      m_tooltips->addTooltipFor(&m_status, "Add LOSPEC_API_KEY=... to " LOSPEC_ENV_FILE, BOTTOM);
      return;
    }

    getCached("palettes/daily?format=expanded",
              [this](const json11::Json& data, const std::string& error) {
                if (!error.empty())
                  return setStatus(error);
                clear(m_daily);
                m_daily.addChild(createItem(palette_from_json(data)));
                relayout();
              });
    getCached("dailytags/daily", [this](const json11::Json& data, const std::string& error) {
      if (error.empty())
        m_tag.setText("Daily #" + data["tag"].string_value());
      relayout();
    });
  }

  ~LospecPanel()
  {
    endPreview();
    for (ListBox* list : { &m_daily, &m_results })
      for (Widget* item : list->children())
        m_tooltips->removeTooltipFor(item);
  }

private:
  // Click a palette to see the image in it, Esc goes back. The selected
  // item is the previewed palette.
  void preview(ListBox& list, ListBox& other)
  {
    auto* item = dynamic_cast<PaletteItem*>(list.getSelectedChild());
    Editor* editor = UIContext::instance()->activeEditor();
    if (!item || !editor || !editor->document())
      return;

    other.selectChild(nullptr);
    m_previewColors = item->palette().colors;
    showPreview(editor->document(), editor->frame());
  }

  void showPreview(Doc* doc, doc::frame_t frame)
  {
    if (!m_previewDoc) {
      Manager::getDefault()->addMessageFilter(kKeyDownMessage, this);
      Manager::getDefault()->addMessageFilter(kMouseDownMessage, this);
      UIContext::instance()->add_observer(this);
      m_commandConn = UIContext::instance()->BeforeCommandExecution.connect(
        [this](CommandExecutionEvent& ev) {
          if (!keepsPreview(ev.command()->id()))
            endPreview();
        });
    }
    m_previewDoc = doc;
    m_previewFrame = frame;
    m_previewImage = recolor(doc->sprite(), frame, to_doc_palette(m_previewColors));
    // ponytail: a NORMAL overlay over the real image, semi-transparent
    // pixels mix with their old color a bit.
    Editor::renderEngine().setPreviewImage(nullptr,
                                           frame,
                                           m_previewImage.get(),
                                           nullptr,
                                           gfx::Point(0, 0),
                                           doc::BlendMode::NORMAL);
    invalidateEditors();
  }

  void endPreview()
  {
    if (!m_previewDoc)
      return;

    Manager::getDefault()->removeMessageFilter(kKeyDownMessage, this);
    Manager::getDefault()->removeMessageFilter(kMouseDownMessage, this);
    UIContext::instance()->remove_observer(this);
    m_commandConn.disconnect();

    Editor::renderEngine().removePreviewImage();
    invalidateEditors();
    m_previewDoc = nullptr;
    m_previewImage.reset();
    m_daily.selectChild(nullptr);
    m_results.selectChild(nullptr);
  }

  void invalidateEditors()
  {
    for (Editor* editor : UIContext::instance()->getAllEditorsIncludingPreview(m_previewDoc))
      editor->invalidate();
  }

  // Commands that only move around keep the preview, the rest (undo,
  // filters, etc.) change the image or show their own preview.
  static bool keepsPreview(const std::string& id)
  {
    for (const char* keep : { CommandId::Zoom(),
                              CommandId::Scroll(),
                              CommandId::FitScreen(),
                              CommandId::GotoFirstFrame(),
                              CommandId::GotoPreviousFrame(),
                              CommandId::GotoNextFrame(),
                              CommandId::GotoLastFrame(),
                              CommandId::GotoFrame(),
                              CommandId::PlayAnimation() })
      if (id == keep)
        return true;
    return false;
  }

  // The preview follows the frame and ends with another document
  void onActiveSiteChange(const Site& site) override
  {
    if (site.document() != m_previewDoc)
      endPreview();
    else if (site.frame() != m_previewFrame)
      showPreview(m_previewDoc, site.frame());
  }

  bool onProcessMessage(Message* msg) override
  {
    // Message filters while previewing
    if (msg->fromFilter()) {
      if (msg->type() == kKeyDownMessage &&
          static_cast<KeyMessage*>(msg)->scancode() == kKeyEsc) {
        endPreview();
        return true; // Used, so it doesn't deselect too
      }
      // Drawing (or any tool but scroll/zoom) shows the real colors
      if (msg->type() == kMouseDownMessage) {
        auto* editor = dynamic_cast<Editor*>(msg->recipient());
        tools::Ink* ink = (editor ? editor->getCurrentEditorInk() : nullptr);
        if (ink && !ink->isScrollMovement() && !ink->isZoom())
          endPreview();
      }
      return false;
    }
    return VBox::onProcessMessage(msg);
  }

  void get(const std::string& path, Request::Handler handler)
  {
    m_requests.push_back(std::make_unique<Request>(path, m_key, std::move(handler)));
    m_timer.start();
  }

  // Shows the saved response right away (if any), then the fresh one,
  // which is saved for next time (or offline).
  void getCached(const std::string& path, const Request::Handler& show)
  {
    const std::string file = cache_file(path);
    const json11::Json cached = read_cache(file);
    if (!cached.is_null())
      show(cached, std::string());

    get(path, [this, file, cached, show](const json11::Json& data, const std::string& error) {
      if (error.empty()) {
        write_cache(file, data);
        show(data, error);
      }
      else if (cached.is_null())
        show(data, error);
      else
        setStatus("Offline (saved)");
    });
  }

  void poll()
  {
    for (std::size_t i = 0; i < m_requests.size();) {
      if (m_requests[i]->done()) {
        std::unique_ptr<Request> request = std::move(m_requests[i]);
        m_requests.erase(m_requests.begin() + i);
        request->handle(); // Can add new requests
      }
      else
        ++i;
    }
    if (m_requests.empty())
      m_timer.stop();
  }

  // Name and #tag with the trailing spaces and case of the saved results
  std::string query() const
  {
    std::string query;
    base::trim_string(m_search.text(), query);
    return base::string_to_lower(query);
  }

  bool isNameQuery() const
  {
    const std::string q = query();
    return !q.empty() && q[0] != '#';
  }

  void search()
  {
    if (m_key.empty())
      return;

    const std::string q = query();
    if (isNameQuery())
      return loadResults("palettes/suggest/" + url_encode(q) + "?format=expanded", List::Name);

    // Browse everything or a #tag, filtered and sorted by Lospec
    m_browseTag = q.substr(std::min<std::size_t>(1, q.size()));
    std::string path = fmt::format("palettes?format=expanded&limit={}&sort={}",
                                   kPageSize,
                                   kSorts[m_sort.getSelectedItemIndex()]);
    if (!m_browseTag.empty())
      path += "&tag=" + url_encode(m_browseTag);
    if (const int mode = m_countMode.getSelectedItemIndex())
      path += fmt::format("&{}={}", kCountParams[mode], m_count.getValue());
    loadResults(path, List::Browse);
  }

  enum class List { Name, Browse, Similar };

  // Fills the results with the palettes from 'path'. Browse lists come
  // in pages, 'offset' is the page that "More..." loads.
  void loadResults(const std::string& path,
                   const List list,
                   const int offset = 0,
                   const std::string& title = std::string())
  {
    const int searchId = ++m_searchId;
    if (offset == 0) {
      clear(m_results);
      showResults(false);
    }
    setStatus("Searching...");

    const std::string page = (list == List::Browse ? fmt::format("{}&offset={}", path, offset) :
                                                     path);
    getCached(page, [=](const json11::Json& data, const std::string& error) {
      if (searchId != m_searchId) // Old search
        return;
      if (!error.empty())
        return setStatus(error);

      clear(m_results, offset); // The saved page is shown first
      for (const auto& json : data.array_items()) {
        LospecPalette pal = palette_from_json(json);
        // Name search has no filters, these are the colors we can check
        if (list != List::Name || fitsCount(int(pal.colors.size())))
          m_results.addChild(createItem(std::move(pal)));
      }
      if (list == List::Browse && int(data.array_items().size()) == kPageSize) {
        m_results.addChild(
          new ActionItem("More...", [=] { loadResults(path, list, offset + kPageSize); }));
      }

      const bool found = (m_results.firstChild() != nullptr);
      if (!found && list == List::Browse && !m_browseTag.empty())
        return suggestTags(searchId);
      setStatus(found ? title : "Nothing found");
      showResults(found);
    });
  }

  // A #tag without palettes, offer the closest tags
  void suggestTags(const int searchId)
  {
    getCached("tags/suggest/" + url_encode(m_browseTag) + "?list=palettes",
              [this, searchId](const json11::Json& data, const std::string& error) {
                if (searchId != m_searchId || m_results.firstChild()) // Old or shown
                  return;
                const auto& tags = data.array_items();
                if (!tags.empty() && tags[0]["tag"].string_value() == m_browseTag)
                  return setStatus("Nothing found"); // Known tag, only the filters left nothing
                for (const auto& json : tags) {
                  const std::string tag = "#" + json["tag"].string_value();
                  m_results.addChild(new ActionItem(tag, [this, tag] {
                    m_search.setText(tag);
                    search();
                  }));
                }
                const bool found = (m_results.firstChild() != nullptr);
                setStatus(found ? "Try these tags" : (error.empty() ? "Nothing found" : error));
                showResults(found);
              });
  }

  bool fitsCount(const int n) const
  {
    const int count = m_count.getValue();
    switch (m_countMode.getSelectedItemIndex()) {
      case 1:  return n <= count;
      case 2:  return n >= count;
      case 3:  return n == count;
      default: return true;
    }
  }

  void findSimilar(const LospecPalette& pal)
  {
    if (pal.colors.size() > 64) // Free API tier limit
      return setStatus("Max 64 colors");
    loadResults("palettes/" + url_encode(pal.slug) + "/similar",
                List::Similar,
                0,
                "Similar to " + pal.title);
  }

  void showMenu(const LospecPalette& pal, const gfx::Point& pos)
  {
    Menu menu;
    MenuItem similar("Find similar");
    menu.addChild(&similar);
    similar.Click.connect([this, pal] { findSimilar(pal); });
    menu.showPopup(pos, display());
  }

  Widget* createItem(LospecPalette pal)
  {
    const std::string tip = fmt::format(
      "{}{}{} ({} colors)\nClick to preview (Esc to go back), double-click to use it,\n"
      "right-click to find similar palettes",
      pal.title,
      pal.author.empty() ? "" : " by ",
      pal.author,
      pal.colors.size());
    auto* item = new PaletteItem(std::move(pal));
    item->RightClick.connect(
      [this, item](const gfx::Point& pos) { showMenu(item->palette(), pos); });
    m_tooltips->addTooltipFor(item, tip, BOTTOM);
    return item;
  }

  // Items from 'from' on. Deleted later because an item can clear its
  // own list ("More...", a menu).
  void clear(ListBox& list, const int from = 0)
  {
    while (int(list.children().size()) > from) {
      Widget* item = list.lastChild();
      m_tooltips->removeTooltipFor(item);
      list.removeChild(item);
      Manager::getDefault()->addToGarbage(item);
    }
  }

  void showResults(bool state)
  {
    m_view.setVisible(state);
    setExpansive(state); // Share the space with the palette while showing results
    m_view.updateView();
    relayout();
  }

  void setStatus(const std::string& text)
  {
    m_status.setText(text);
    m_status.setVisible(!text.empty());
    relayout();
  }

  void relayout()
  {
    if (parent())
      parent()->layout();
  }

  static void usePalette(ListBox& list)
  {
    auto* item = dynamic_cast<PaletteItem*>(list.getSelectedChild());
    if (!item)
      return;

    doc::Palette palette = to_doc_palette(item->palette().colors);
    auto* cmd = static_cast<SetPaletteCommand*>(
      Commands::instance()->byId(CommandId::SetPalette()));
    cmd->setPalette(&palette);
    UIContext::instance()->executeCommandFromMenuOrShortcut(cmd);
  }

  TooltipManager* m_tooltips;
  std::string m_key;
  Header m_header;
  LinkLabel m_tag;
  ListBox m_daily;
  SearchField m_search;
  HBox m_countRow;
  ComboBox m_countMode;
  IntEntry m_count{ 1, 256 };
  ComboBox m_sort;
  Label m_status{ "" };
  View m_view;
  ListBox m_results; // After m_view, so it's destroyed (and removed from m_view) first
  Timer m_timer;
  Timer m_countTimer{ 400, this };
  int m_searchId = 0;
  std::string m_browseTag;
  Doc* m_previewDoc = nullptr; // Previewing when not null
  doc::frame_t m_previewFrame = 0;
  std::vector<gfx::Color> m_previewColors;
  doc::ImageRef m_previewImage;
  obs::scoped_connection m_commandConn;
  std::vector<std::unique_ptr<Request>> m_requests; // Last member: joined first
};

} // anonymous namespace

ui::Widget* create_lospec_panel(ui::TooltipManager* tooltips)
{
  return new LospecPanel(tooltips);
}

} // namespace app
