-- Edit > ComfyUI...: runs a saved ComfyUI workflow headless with the
-- ComfyUI folder's assetgen.py. The selection (none = the whole canvas)
-- sets the output size and position; workflows that take an image get
-- the selected pixels when there are any. Results come back as one new
-- layer each, or as frames on one new layer. Workflows apply no palette,
-- or the sprite's with "Use current palette"; results are converted to
-- the sprite's palette when it isn't RGB. With "Pause to pick", a
-- workflow's pauses open here instead of accepting what was drawn.

local ASSETGEN = os.getenv("ASSETGEN")
  or "/home/ps/dev/ai/sommerper_comfyui/user/assetgen/assetgen.py"
local COMFY = app.fs.filePath(app.fs.filePath(app.fs.filePath(ASSETGEN)))
local WORKFLOWS = app.fs.joinPath(COMFY, "user", "default", "workflows")
-- ComfyUI and its venv live in this container; Aseprite on the host runs the jobs in there.
local CONTAINER = "ai-desktop"

-- How the dialog maps onto each workflow's settings (node titles as
-- `assetgen.py show <workflow>` lists them):
--   fields       text entries: { label, node title, input }; lines = true: "; " in the entry is a new line; also: another node that gets the same value;
--                widget: which of the node's values to start from, when it isn't the first
--   size         node whose width/height get the selection size
--   icon         node whose value gets the selection size, snapped down to 16/24/32/48
--   image        node whose image gets the selected pixels; withImage/withoutImage are extra settings for each case
--   keep, skip   Lua patterns for the output file names to load (default: every PNG)
--   frames       results as frames by default
--   palette      node whose image gets the current palette (default "Palette PNG"); withPalette: extra settings for that
--   data         the results are data, not colours: no sprite palette can hold them
--   offsets      the file names say where each result goes: _x12 and _y34, from the selection's corner
--   grid         draws with a grid LoRA, picked from the size; the number is how many views share one generated image
local WF = {
  { id = "krea2_pixel_art", moss = true, grid = 1,
    fields = { { "Prompt", "Prompt (edit the subject)", "f_string" } },
    size = "Pixel art size", keep = "^pixelart_sprite_" },
  { id = "krea2_pixel_background", grid = 1,
    fields = { { "Prompt", "Prompt", "text" } },
    size = "Pixel art size", keep = "^background_krea_%d+_%.png$" },
  { id = "moss_pixel_background",
    fields = { { "Prompt", "Prompt", "text" } },
    size = "Pixel art size", keep = "^background_moss_%d+_%.png$" },
  { id = "krea2_pixel_tile", moss = true, grid = 1,
    fields = { { "Subject", "Subject", "value" } },
    size = "Tile size", keep = "^tile_%d+_%.png$" },
  { id = "moss_pixel_icons",
    fields = { { "Icons (; between)", "Icons (one per line)", "lines", lines = true } },
    icon = "Icon size (16, 24, 32 or 48)", skip = "^icon_sheet_" },
  { id = "krea2_pixel_character", moss = true, grid = 4,
    fields = { { "Subject", "Subject", "value" } },
    size = "Pixel art size (one direction)", keep = "^char_dir_",
    -- ponytail: the selected image is always taken as the front view; add a direction picker if side views come up
    image = "Character image (image-to-image)",
    withImage = { "Directions.image_view=front" }, withoutImage = { "Directions.image_view=none" } },
  { id = "krea2_pixel_3d", moss = true, grid = 8,
    fields = { { "Subject", "Subject", "value" } },
    size = "Pixel art size (one direction)", keep = "^char3d_dir_",
    image = "Character image (clear, full body, front view)",
    withImage = { "Use text prompt (off = character image).value=false" },
    withoutImage = { "Use text prompt (off = character image).value=true" } },
  { id = "krea2_pixel_animation", frames = true, moss = true, grid = 1,
    fields = { { "Subject", "Subject", "value" } },
    size = "Pixel art size (text only)", keep = "^anim_frame_",
    image = "Sprite file (side view, facing right)",
    withImage = { "Use text prompt (off = sprite file).value=false" },
    withoutImage = { "Use text prompt (off = sprite file).value=true" } },
  { id = "krea2_pixel_vfx", frames = true, moss = true, grid = 1,
    fields = { { "First frame", "First frame (what Krea draws)", "value" },
               { "Motion", "Motion (what Wan animates)", "value" },
               { "Frames (0 = all)", "One-shot: trim + fade out", "frames", also = "Loop: find loop" } },
    size = "Effect size", keep = "^frame_%d+_%.png$" },
  { id = "krea2_pixel_portraits", moss = true, grid = 1,
    fields = { { "Character", "Character", "value" },
               { "Expressions (; between)", "Expressions (one per line, the portrait's own first)", "value", lines = true } },
    size = "Portrait size", skip = "sheet_%d+_%.png$",
    palette = "Palette PNG (your game's palette)",
    withPalette = { "Use the palette PNG (off = the character's own colours).value=true" },
    image = "My portrait (any size, square)",
    withImage = { "Use my portrait (off = draw one with Krea 2).value=true" },
    withoutImage = { "Use my portrait (off = draw one with Krea 2).value=false" } },
  { id = "moss_pixel_nine_slice",
    fields = { { "Frame", "Frame (what to draw)", "value" },
               { "Inside", "Inside (fill)", "value" } },
    size = "Picture size (128x96 panels, 128x64 buttons)", skip = "_preview_%d+_%.png$",
    withPalette = { "Use palette PNG (off = the frame's own 16 colours).value=true" } },
  { id = "moss_pixel_overhang", offsets = true,
    fields = { { "Overhang", "Overhang (what to draw)", "value" } },
    size = "Whole gate picture size" },
  { id = "moss_pixel_parallax", offsets = true,
    fields = { { "Layers (; between)", "Layers (back to front)", "layers", lines = true },
               { "Style", "Layers (back to front)", "style", widget = 2 } },
    size = "Layer size" },
  { id = "moss_pixel_platform",
    fields = { { "Top", "Top material", "value" },
               { "Body", "Body material", "value" } },
    size = "Tile size", skip = "^platform_map_" },
  { id = "moss_pixel_roadside",
    fields = { { "Objects (; between)", "Objects (one per line: what | height, or what | width x height)", "objects", lines = true } },
    skip = "^set_preview_" },
  { id = "moss_pixel_animate_combo",
    fields = { { "Subject", "Subject", "value" },
               { "Motion", "Motion description", "value" } },
    size = "Sprite size", keep = "^anim_combo_sheet_",
    image = "Character image (T-pose, full body, front view)",
    withImage = { "Use text prompt (off = character image).value=false" },
    withoutImage = { "Use text prompt (off = character image).value=true" } },
  { id = "moss_voxel_3d",
    fields = { { "Subject", "Subject", "value" } },
    keep = "^voxel_sprite_",
    image = "Your image (clear, whole object, front view)",
    withImage = { "Use text prompt (off = your image).value=false" },
    withoutImage = { "Use text prompt (off = your image).value=true" } },
  { id = "moss_spine_character",
    fields = { { "Character", "Character", "value" },
               { "Held item", "Held item (empty: nothing)", "value" } },
    image = "My image (full body, three-quarter view, plain or transparent background)",
    withImage = { "Use my image (off: MOSS draws one).value=true" },
    withoutImage = { "Use my image (off: MOSS draws one).value=false" } },
  { id = "moss_basic",
    fields = { { "Prompt", "Prompt", "text" } } },
  { id = "pixel_normals", fields = {}, needsImage = true, data = true,
    image = "Sprite or sheet (PNG with alpha)" },
}

-- moss = true: krea2_<name> also has a moss_<name> copy, where "Krea" in a node title reads "MOSS".
local function mossCopy(v)
  if type(v) == "string" then return (v:gsub("Krea 2", "MOSS"):gsub("Krea", "MOSS")) end
  if type(v) ~= "table" then return v end
  local out = {}
  for k, x in pairs(v) do out[k] = mossCopy(x) end
  return out
end

for i = #WF, 1, -1 do
  if WF[i].moss then
    local copy = mossCopy(WF[i])
    copy.id = WF[i].id:gsub("^krea2_", "moss_")
    copy.grid = nil
    table.insert(WF, i + 1, copy)
  end
end

-- The grid LoRAs draw one art pixel as a square of this many generated pixels, so the LoRA and the generation
-- size go together. Picks the one that makes the whole generated image closest to 1 megapixel; nil if none fits.
local GRID = { { 32, "k2-pixel32.safetensors" }, { 16, "k2-pixel64.safetensors" }, { 8, "k2-pixel128.safetensors" } }

local function pickGrid(width, height, views)
  local lora, megapixels, off
  for _, g in ipairs(GRID) do
    local mp = g[1] ^ 2 * width * height / 2 ^ 20
    local o = math.abs(math.log(mp * views, 2))
    if mp >= 0.25 and mp * views <= 4 and (not off or o < off) then lora, megapixels, off = g[2], mp, o end
  end
  -- k2-pixel32 falls apart this small; this one is made for it, at the same 32 px.
  if lora and math.max(width, height) <= 16 then lora = "krea-2-pixelart-16x16.safetensors" end
  return lora, megapixels
end

local plugin_, dlg, job

local function q(s) return "'" .. s:gsub("'", "'\\''") .. "'" end

local function byId(id)
  for _, w in ipairs(WF) do
    if w.id == id then return w end
  end
end

local function read(path)
  local f = io.open(path, "rb")
  if not f then return nil end
  local s = f:read("a")
  f:close()
  return s
end

local function status(text) dlg:modify{ id = "status", text = text } end

local function savedNodes(w)
  local data = read(app.fs.joinPath(WORKFLOWS, w.id .. ".json"))
  return data and json.decode(data).nodes or {}
end

-- The texts of w's fields: as last used, else as saved in the workflow.
local function texts(w)
  local last = plugin_.preferences.texts[w.id] or {}
  local nodes, out = {}, {}
  for _, n in ipairs(savedNodes(w)) do
    if n.title then nodes[n.title] = n end
  end
  for i, field in ipairs(w.fields) do
    local n = nodes[field[2]]
    local v = n and n.widgets_values and n.widgets_values[field.widget or 1]
    local t = v and tostring(math.tointeger(v) or v) or "" -- json.decode makes every number a float: 12.0
    out[i] = last[i] or (field.lines and t:gsub("\n", "; ") or t)
  end
  return out
end

-- Whether w can take a palette at all, which shows the checkbox.
local function usesPalette(w)
  for _, n in ipairs(savedNodes(w)) do
    if n.title == "Use palette" then return true end
  end
  return false
end

-- The strength saved in w's "Dither" node (0 to 1); nil if it has none, which hides the dither controls.
local function savedDither(w)
  for _, n in ipairs(savedNodes(w)) do
    if n.title == "Dither" then return n.widgets_values[1] end
  end
end

-- Dither sits on the palette path, so without a palette the controls would do nothing.
local function ditherEnabled()
  dlg:modify{ id = "dither", enabled = dlg.data.palette }
  dlg:modify{ id = "ditherAmount", enabled = dlg.data.palette and dlg.data.dither }
end

local function pick(w)
  local t = texts(w)
  for i = 1, 3 do
    local field = w.fields[i]
    dlg:modify{ id = "f" .. i, visible = field ~= nil, label = field and field[1] or "", text = t[i] or "" }
  end
  dlg:modify{ id = "frames", selected = w.frames or false }
  dlg:modify{ id = "palette", visible = usesPalette(w) }
  local d = savedDither(w)
  dlg:modify{ id = "dither", visible = d ~= nil, selected = (d or 0) > 0 }
  -- Saved as off: start the slider where turning it on does something (the workflows suggest 0.1 to 0.2).
  dlg:modify{ id = "ditherAmount", visible = d ~= nil, value = (d or 0) > 0 and math.floor(d * 100 + 0.5) or 15 }
  ditherEnabled()
  local pauses = false
  for _, n in ipairs(savedNodes(w)) do pauses = pauses or n.type:find("Pick") ~= nil end
  dlg:modify{ id = "pause", visible = pauses }
end

-- The job turns each PNG into "<w> <h>\n" + RGBA bytes: Image{ fromFile } would
-- go through File > Open and put every result into the recent files.
local CONVERT = [[import glob, sys, PIL.Image as I
for p in glob.glob(sys.argv[1] + "/*.png"):
    im = I.open(p).convert("RGBA")
    open(p + ".rgba", "wb").write(b"%d %d\n" % im.size + im.tobytes())]]

-- Runs next to assetgen.py (which doesn't pass a client id, so ComfyUI
-- broadcasts its progress): follows ComfyUI's websocket for the prompt id
-- assetgen.py logs, and keeps "<node id> <step> <steps>" in the job's progress file.
-- With "pick" it also hands the workflow's pauses to Aseprite: a pause's images and
-- ask.json go in the job folder, and answer.json goes back to the waiting node.
local WATCH = [[import asyncio, io, json, os, re, sys, time, aiohttp, PIL.Image as I
d, pick = sys.argv[1], len(sys.argv) > 2

def show(*fields):
    open(d + "/progress.tmp", "w").write(" ".join(map(str, fields)))
    os.replace(d + "/progress.tmp", d + "/progress")

async def ask(s, url, kind, data):
    for i, image in enumerate(data["images"], 1):
        async with s.get(url + "/view", params=image) as r:
            im = I.open(io.BytesIO(await r.read())).convert("RGBA")
        open(f"{d}/ask_{i}.rgba", "wb").write(b"%d %d\n" % im.size + im.tobytes())
    json.dump({**data, "kind": kind, "count": len(data["images"]), "deadline": time.time() + data["timeout"]}, open(d + "/ask.tmp", "w"))
    os.replace(d + "/ask.tmp", d + "/ask.json")

async def main():
    pid = current = None
    url = os.environ.get("COMFY_URL", "http://127.0.0.1:8188")
    async with aiohttp.ClientSession() as s, s.ws_connect(url + "/ws") as ws:
        while not os.path.exists(d + "/done"):
            if os.path.exists(d + "/answer.json"):
                async with s.post(url + "/pixel_picker", data=open(d + "/answer.json", "rb").read()):
                    os.remove(d + "/answer.json")
            try:
                m = await ws.receive(timeout=0.25)
            except asyncio.TimeoutError:
                continue
            if m.type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR):
                return
            if m.type != aiohttp.WSMsgType.TEXT:  # preview images
                continue
            msg = m.json()
            data = msg.get("data") or {}
            pid = pid or (re.findall(r"queued (\S+),", open(d + "/log").read()) or [None])[0]
            current = data.get("prompt_id", current)
            if msg["type"] in ("pixel_frame_picker", "pixel_image_picker", "pixel_turnaround_picker"):
                if pick and current == pid:  # the event doesn't say whose; only one prompt runs at a time
                    await ask(s, url, msg["type"], data)
                continue
            if msg["type"] == "pixel_picker_done":
                if os.path.exists(d + "/ask.json"):
                    os.remove(d + "/ask.json")
                continue
            if not pid or data.get("prompt_id") != pid:
                continue
            if msg["type"] == "executing" and data["node"] is not None:
                show(data["node"], 0, 0)
            elif msg["type"] == "progress":
                show(data["node"], data["value"], data["max"])
            elif msg["type"] in ("executing", "execution_success", "execution_error", "execution_interrupted"):
                return

asyncio.run(main())]]

local function rgba(path)
  local data = read(path .. ".rgba")
  local w, h, at = data:match("^(%d+) (%d+)\n()")
  local img = Image(tonumber(w), tonumber(h))
  img.bytes = data:sub(at)
  return img
end

local function load(path, spr)
  local img = rgba(path)
  if spr.colorMode == ColorMode.RGB then return img end
  -- Drawing RGB on an indexed or gray image picks the closest colours, and transparent stays transparent.
  -- ponytail: Aseprite compares 5 bits per channel, so palette entries closer than that become the first one; look up exact colours first if that matters
  local spec = spr.spec
  spec.width, spec.height = img.width, img.height
  local out = Image(spec)
  out:drawImage(img)
  return out
end

local function place(j, files)
  local spr = j.spr
  app.sprite = spr -- results go where they were asked for, even after switching tabs
  app.refresh() -- load() converts to the current palette, which follows the active sprite only after this
  app.transaction("ComfyUI " .. j.w.id, function()
    local layer
    for i, path in ipairs(files) do
      local frame = j.frame
      if j.frames then
        frame = j.frame + i - 1
        while #spr.frames < frame do spr:newEmptyFrame() end
      end
      if i == 1 or not j.frames then
        layer = spr:newLayer()
        layer.name = j.frames and j.w.id or ("%s %d"):format(j.w.id, i)
      end
      local pos = j.pos
      if j.w.offsets then
        local name = app.fs.fileName(path)
        pos = Point(pos.x + (tonumber(name:match("_x(%d+)")) or 0), pos.y + (tonumber(name:match("_y(%d+)")) or 0))
      end
      spr:newCel(layer, frame, load(path, spr), pos)
    end
  end)
  app.refresh()
end

-- 0-based numbers of the tiles in this state, as the pause nodes count them.
local function which(marks, state)
  local out = {}
  for i, m in ipairs(marks) do
    if m == state then out[#out + 1] = i - 1 end
  end
  return out
end

-- The workflows' pauses, like their browser popups. Clicking a tile marks it.
--   start        how the tiles begin
--   dim          tiles in this state look left out
--   border       the colour around marked tiles
--   buttons      { text, when it can be pressed, the answer }; the timeout presses the last one, if it can be pressed
-- ponytail: no FPS or animated preview for frames; the results get no frame times anyway, and play in Aseprite
local PICKERS = {
  pixel_frame_picker = { title = "Click the frames to leave out", start = true, dim = false, buttons = {
    { "Send", function(m) return #which(m, true) > 0 end, function(m) return { selection = which(m, true) } end } } },
  pixel_turnaround_picker = { title = "Click the ones that don't fit, then Redo", start = false, dim = true,
    border = Color{ r = 229, g = 72, b = 77 }, buttons = {
    { "Redo marked", function(m) return #which(m, true) > 0 end, function(m) return { redo = which(m, true) } end },
    { "Done", function() return true end, function() return {} end } } },
  pixel_image_picker = { title = "Click the images to keep, then regenerate the others, or keep one and use it", start = false,
    border = Color{ r = 70, g = 200, b = 110 }, buttons = {
    { "Regenerate the others", function(m) return #which(m, false) > 0 end, function(m) return { redo = which(m, false) } end },
    { "Use the kept image", function(m) return #which(m, true) == 1 end, function(m) return { use = which(m, true)[1] } end } } },
}

local function openPicker(j, ask)
  local kind = PICKERS[ask.kind]
  -- The largest tiles that fit the window, leaving room for the dialog's own title, label and buttons.
  local cols, tile = 1, 0
  local roomW, roomH = app.window.width // app.uiScale - 40, app.window.height // app.uiScale - 80
  for c = 1, ask.count do
    local t = math.min(roomW // c - 4, roomH // math.ceil(ask.count / c) - 18, 192)
    if t > tile then cols, tile = c, t end
  end
  local cellW, cellH = tile + 4, tile + 18
  local imgs, marks, locked = {}, {}, {}
  for i = 1, ask.count do
    imgs[i] = rgba(app.fs.joinPath(j.dir, "ask_" .. i))
    marks[i] = kind.start
  end
  for _, f in ipairs(ask.fixed or {}) do locked[math.tointeger(f) + 1] = true end

  local pd, p
  local function answer(a)
    if j.picker ~= p then return end
    j.picker = nil
    a.id = ask.id
    local tmp = app.fs.joinPath(j.dir, "answer.tmp")
    local f = io.open(tmp, "w")
    f:write(json.encode(a))
    f:close()
    os.rename(tmp, app.fs.joinPath(j.dir, "answer.json"))
    pd:close()
  end
  local function refresh()
    for i, b in ipairs(kind.buttons) do pd:modify{ id = "b" .. i, enabled = b[2](marks) } end
    pd:repaint()
  end
  pd = Dialog{ title = kind.title, onclose = function() answer{ cancel = true } end }
  pd:canvas{ width = cols * cellW, height = math.ceil(ask.count / cols) * cellH,
    onpaint = function(ev)
      local gc = ev.context
      for i, img in ipairs(imgs) do
        local x, y = (i - 1) % cols * cellW, (i - 1) // cols * cellH
        gc.color = Color{ gray = 64 }
        gc:fillRect(Rectangle(x, y, tile, tile))
        local s = tile / math.max(img.width, img.height)
        if s >= 1 then s = s // 1 end -- whole pixels when enlarging
        local w, h = math.floor(img.width * s), math.floor(img.height * s)
        gc.opacity = marks[i] == kind.dim and 90 or 255
        gc:drawImage(img, Rectangle(0, 0, img.width, img.height), Rectangle(x + (tile - w) // 2, y + (tile - h) // 2, w, h))
        gc.opacity = 255
        if marks[i] and kind.border then
          gc.color = kind.border
          gc.strokeWidth = 3
          gc:strokeRect(Rectangle(x, y, tile, tile))
        end
        local label = ask.labels and ask.labels[i] or tostring(i - 1)
        while gc:measureText(label).width > tile do label = label:sub(1, -2) end
        gc.color = app.theme.color.text
        gc:fillText(label, x, y + tile + 2)
      end
    end,
    onmousedown = function(ev)
      local col, i = ev.x // cellW, ev.y // cellH * cols + ev.x // cellW + 1
      if col < cols and imgs[i] and not locked[i] then
        marks[i] = not marks[i]
        refresh()
      end
    end }
  pd:label{ id = "left" }
  for i, b in ipairs(kind.buttons) do
    pd:button{ id = "b" .. i, text = b[1], onclick = function() answer(b[3](marks)) end }
  end
  pd:button{ text = "Cancel", onclick = function() answer{ cancel = true } end }
  p = {
    close = function() j.picker = nil; pd:close() end,
    tick = function()
      local left = math.ceil(ask.deadline - os.time())
      pd:modify{ id = "left", text = ("Goes on by itself in %ds"):format(math.max(left, 0)) }
      local last = kind.buttons[#kind.buttons]
      if left <= 0 and last[2](marks) then answer(last[3](marks)) end
    end }
  j.picker = p
  p.tick()
  pd:show{ wait = false }
  refresh() -- after show: a canvas only paints by itself when it's resized, and tick has already laid it out
end

-- Keeps the picker in step with the pause the watcher saved in ask.json.
local function followAsk(j)
  local text = read(app.fs.joinPath(j.dir, "ask.json"))
  local ask = text and json.decode(text)
  -- Over without an answer from here: answered in a browser tab, or timed out.
  if j.picker and (not ask or ask.id ~= j.asked) then j.picker.close() end
  if ask and ask.id ~= j.asked then
    j.asked = ask.id
    openPicker(j, ask)
  end
  if j.picker then j.picker.tick() end
end

local function poll()
  local code = read(app.fs.joinPath(job.dir, "done"))
  if not code then
    followAsk(job)
    local node, step, steps = (read(app.fs.joinPath(job.dir, "progress")) or ""):match("^(%S+) (%S+) (%S+)")
    local now = node and (job.titles[node] or ("node " .. node)) or "waiting for ComfyUI"
    if node and steps ~= "0" then now = ("%s %s/%s"):format(now, step, steps) end
    return status(("Running %s: %s (%ds)"):format(job.w.id, now, os.time() - job.start))
  end
  job.timer:stop()
  local j = job
  job = nil
  if j.picker then j.picker.close() end
  dlg:modify{ id = "go", enabled = true }
  if tonumber(code) ~= 0 then
    local log = read(app.fs.joinPath(j.dir, "log")) or ""
    return status("Failed: " .. (log:match("([^\n]+)%s*$") or ("exit code " .. code)))
  end
  local files = {}
  for path in (read(app.fs.joinPath(j.dir, "files")) or ""):gmatch("[^\n]+") do
    local name = app.fs.fileName(path)
    if name:match("%.png$") and (not j.w.keep or name:match(j.w.keep)) and not (j.w.skip and name:match(j.w.skip)) then
      files[#files + 1] = path
    end
  end
  if #files == 0 then return status("Done, but no images came back.") end
  local ok, err = pcall(place, j, files)
  status(ok and ("Added %d %s%s."):format(#files, j.frames and "frame" or "layer", #files == 1 and "" or "s")
            or ("Couldn't add the results: " .. tostring(err)))
end

local function submit()
  local spr = app.sprite
  if job then return end
  if not spr then return status("Open a sprite first.") end
  local w = byId(dlg.data.wf)
  if w.data and spr.colorMode ~= ColorMode.RGB then
    return status(w.id .. " makes RGB data: Sprite > Color Mode > RGB first.")
  end
  local b = spr.selection.isEmpty and spr.bounds or spr.selection.bounds
  -- In ComfyUI's temp folder: the same path in the container and on the host.
  -- ponytail: job folders (the selection, results, log) stay until that folder is cleared
  local dir = app.fs.joinPath(COMFY, "temp", ("aseprite-%d-%d"):format(os.time(), math.random(1e6)))
  app.fs.makeAllDirectories(app.fs.joinPath(dir, "out"))

  local sets, entered = {}, {}
  if usesPalette(w) then
    sets[1] = "Use palette.value=" .. tostring(dlg.data.palette)
    if dlg.data.palette then
      local png = app.fs.joinPath(dir, "palette.png")
      spr.palettes[1]:saveAs(png) -- ponytail: the first palette, like the selection below
      sets[2] = ("%s.image=%s"):format(w.palette or "Palette PNG", png)
      for _, s in ipairs(w.withPalette or {}) do sets[#sets + 1] = s end
      if savedDither(w) then
        sets[#sets + 1] = ("Dither.strength=%g"):format(dlg.data.dither and dlg.data.ditherAmount / 100 or 0)
      end
    end
  end
  for i, field in ipairs(w.fields) do
    entered[i] = dlg.data["f" .. i]
    local t = field.lines and entered[i]:gsub("%s*;%s*", "\n") or entered[i]
    sets[#sets + 1] = ("%s.%s=%s"):format(field[2], field[3], t)
    if field.also then sets[#sets + 1] = ("%s.%s=%s"):format(field.also, field[3], t) end
  end
  plugin_.preferences.texts[w.id] = entered
  plugin_.preferences.workflow = w.id

  if w.size then
    sets[#sets + 1] = ("%s.width=%d"):format(w.size, b.width)
    sets[#sets + 1] = ("%s.height=%d"):format(w.size, b.height)
  end
  if w.grid then
    local lora, mp = pickGrid(b.width, b.height, w.grid)
    if lora then
      sets[#sets + 1] = "Pixel art LoRA.lora_name=" .. lora
      sets[#sets + 1] = ("%s.megapixels=%g"):format(w.size, mp)
    else -- too big or small for any of them
      sets[#sets + 1] = "Pixel art LoRA.strength_model=0"
    end
  end
  if w.icon then
    local size = 16
    for _, s in ipairs{ 24, 32, 48 } do
      if s <= math.max(b.width, b.height) then size = s end
    end
    sets[#sets + 1] = ("%s.value=%d"):format(w.icon, size)
  end
  if w.image then
    local img = Image(spr.spec)
    img:drawSprite(spr, app.frame)
    img = Image(img, b)
    -- One plain colour (nothing drawn, or only the background) = no image.
    local use = not img:isPlain(img:getPixel(0, 0))
    if use then
      local png = app.fs.joinPath(dir, "selection.png")
      -- ponytail: the first palette; sprites with one palette per frame would need the frame's
      img:saveAs{ filename = png, palette = spr.palettes[1] }
      sets[#sets + 1] = ("%s.image=%s"):format(w.image, png)
    elseif w.needsImage then
      return status(w.id .. " works on an image: select some pixels.")
    end
    for _, s in ipairs((use and w.withImage or not use and w.withoutImage) or {}) do
      sets[#sets + 1] = s
    end
  end

  local path = function(name) return q(app.fs.joinPath(dir, name)) end
  local cmd = q(ASSETGEN) .. " run " .. w.id .. " -o " .. path("out")
  for _, s in ipairs(sets) do cmd = cmd .. " --set " .. q(s) end
  if dlg.data.seed then cmd = cmd .. " --seed " .. math.random(0, 2147483647) end
  if dlg.data.pause then cmd = cmd .. " --pick" end
  local python = (read(ASSETGEN) or ""):match("^#!(%S+)") or "python3" -- the venv's, it has PIL
  local run = ("%s -I -c %s %s > /dev/null 2> %s & %s && %s -I -c %s %s"):format(
    q(python), q(WATCH), q(dir) .. (dlg.data.pause and " pick" or ""), path("watch.log"), cmd, q(python), q(CONVERT), path("out"))
  if not app.fs.isFile(python) then -- the venv's python only exists in the container
    run = ("podman exec %s sh -c %s"):format(CONTAINER, q(run))
  end
  -- "done" appears (by mv) only once everything has finished and the exit code is written.
  os.execute(("({ %s; } > %s 2> %s; echo $? > %s; mv %s %s) &"):format(
    run, path("files"), path("log"), path("code"), path("code"), path("done")))

  job = { w = w, spr = spr, dir = dir, start = os.time(), frames = dlg.data.frames,
          frame = app.frame.frameNumber, pos = Point(b.x, b.y), titles = {} }
  for _, n in ipairs(savedNodes(w)) do job.titles[("%d"):format(n.id)] = n.title or n.type end
  job.timer = Timer{ interval = 1.0, ontick = poll }
  job.timer:start()
  dlg:modify{ id = "go", enabled = false }
  status(("Running %s..."):format(w.id))
end

local function open()
  if dlg then return dlg:show{ wait = false } end
  local ids = {}
  for _, w in ipairs(WF) do ids[#ids + 1] = w.id end
  local w = byId(plugin_.preferences.workflow or "") or WF[1]
  -- autofit TOP: the height follows the shown fields, the width stays as set below.
  dlg = Dialog{ title = "ComfyUI", autofit = Align.TOP }
  dlg:combobox{ id = "wf", label = "Workflow", options = ids, option = w.id,
                onchange = function() pick(byId(dlg.data.wf)) end }
  dlg:entry{ id = "f1", label = "Prompt" }
  dlg:entry{ id = "f2", label = "Prompt" }
  dlg:entry{ id = "f3", label = "Prompt" }
  dlg:check{ id = "frames", label = "Results", text = "As frames (off: one layer each)" }
  dlg:check{ id = "palette", label = "Palette", text = "Use current palette (off: no palette)", onclick = ditherEnabled }
  dlg:check{ id = "dither", label = "Dither", text = "Amount", onclick = ditherEnabled }
  dlg:slider{ id = "ditherAmount", min = 0, max = 100, value = 15 }
  dlg:check{ id = "pause", label = "Pause", text = "Pause to pick (off: accept what's drawn)" }
  dlg:check{ id = "seed", label = "Seed", text = "New each run (off: the workflow's own)", selected = true }
  dlg:label{ id = "status", text = "Select an area (none = the whole canvas)." }
  dlg:button{ id = "go", text = "&Generate", focus = true, onclick = submit }
  dlg:button{ text = "&Close", onclick = function() dlg:close() end }
  pick(w)
  dlg:show{ wait = false }
  local b = dlg.bounds -- room for long prompts
  dlg.bounds = Rectangle(b.x - b.width / 2, b.y, b.width * 2, b.height)
end

function init(plugin)
  plugin_ = plugin
  plugin.preferences.texts = plugin.preferences.texts or {}
  -- What was saved before the workflows got a prefix for their model.
  for _, w in ipairs(WF) do
    local old = w.id == "moss_pixel_icons" and "pixel_icons" or w.id:match("^krea2_(.+)")
    if old and plugin.preferences.texts[old] then
      plugin.preferences.texts[w.id] = plugin.preferences.texts[w.id] or plugin.preferences.texts[old]
      plugin.preferences.texts[old] = nil
    end
    if old and plugin.preferences.workflow == old then plugin.preferences.workflow = w.id end
  end
  plugin:newCommand{ id = "ComfyUI", title = "ComfyUI...", group = "edit_insert", onclick = open }
end
