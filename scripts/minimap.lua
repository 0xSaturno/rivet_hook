-- SPDX-FileCopyrightText: 2025-2026 Neptuwunium
--
-- SPDX-License-Identifier: EUPL-1.2

-- Feeds the HUD minimap (hudMod: exported/Overlay/{Overlay.html,css/MiniMap.css,
-- js/MiniMap.js}) with the hero's position and the pickups around them.
--
-- There is no way to push into a cohtml view from here: the only cohtml hook in
-- the runtime is DecodeURLString, a static with no View pointer, so there is no
-- View::TriggerEvent to call. Instead rivet.ui_publish rewrites the bytes of
-- small json assets the page polls. Slots are cycled because a view may cache a
-- response per url, so the page always asks for one whose contents have changed.
--
-- Keys:
--   F4   calibrate: press, then run FORWARD in a straight line for a couple of
--        seconds. The row matching the direction travelled is chosen.
--   F7   zoom in  (halve the range)
--   F8   zoom out (double the range)
--   F9   report position, pickup counts and range
--   F10  rescan for pickups (do this when you change planet)

local HERO = "Rivet"

-- find_actor and actors both walk the whole scene: 71,680 actors in Megalopolis,
-- enough to overrun the 8 ms callback budget and raise. So never scan on a timer,
-- pcall everything that scans, and leave a gap between attempts.
local RETRY_FRAMES = 120

-- Actor names are matched by substring. These are taken from the asset paths and
-- may need adjusting; F9 reports how many of each were found.
--   gold bolt   equipment/pickup/pup_bolt_gold_01/pup_bolt_gold_01.actor
--   raritanium  environment/global/gameplay/
--               gbl_gp_raritanium_crate_grp_bk_01/gbl_gp_raritanium_crate_grp_bk.actor
local GOLD_MATCH = "pup_bolt_gold"
local RAR_MATCH = "raritanium"

local SCAN_LIMIT = 512     -- handles requested per kind
local MAX_MARKERS = 16     -- published per kind; the slot is only 1 KB

-- How much world fits across the panel, in game units. Bigger = more of the
-- level on screen. Tuned live with F7/F8 so finding a good value costs nothing.
-- 100 is F7 held all the way down from the old 1600 default, and is what
-- actually reads well in play. RANGE_MIN is dropped below it so there is still
-- room to zoom further in.
local RANGE = 100
local RANGE_MIN = 25
local RANGE_MAX = 25600

-- Which row of the actor's transform basis points where Rivet is looking.
-- rivet.basis returns rows 0..2 and which is "forward" is an engine
-- convention, so it was measured rather than guessed: running forward and
-- averaging the direction travelled over 90 frames gave row 2 -> 1.000 with
-- rows 0 and 1 at ~0. F4 re-runs that if a vehicle sits differently.
--
-- Judging it by eye did not work and cost several attempts: a row that is 90
-- degrees out still turns the map at exactly the right rate.
local FORWARD_ROW = 2
local FORWARD_SIGN = 1

local SLOTS = 8            -- must match SLOTS in MiniMap.js
local slot = 0

local hero = nil
local since_scan = 0
local last = nil

-- pickups do not move, so scan once and cache
local gold = nil
local rar = nil

local function rescan()
  gold, rar = nil, nil
  rivet.log("minimap: cleared pickup cache, will rescan")
end

local function scan(match)
  local ok, handles = pcall(rivet.actors, match, SCAN_LIMIT)
  if not ok or handles == nil then
    return nil
  end

  local out = {}
  for i = 1, #handles do
    local ok2, x, _, z = pcall(rivet.position, handles[i])
    if ok2 and x ~= nil then
      -- the handle is kept so the entry can be re-checked later: a pickup that
      -- gets collected has its actor destroyed, and should leave the map
      out[#out + 1] = { x = x, z = z, h = handles[i] }
    end
  end

  -- report a couple of names so a match that finds nothing can be corrected
  -- against what the scene actually calls these things
  local sample = ""
  for i = 1, math.min(3, #handles) do
    local ok3, name = pcall(rivet.name, handles[i])
    if ok3 and name ~= nil then
      sample = sample .. " " .. name
    end
  end
  rivet.log(string.format("minimap: found %d for %q%s", #out, match, sample))
  return out
end

-- Rivet's heading, as a rotation to apply to the world. atan2(x, z) so that a
-- forward vector pointing down +z reads as zero and the map turns with her.
-- the three basis rows flattened to the ground plane, as {x, z} pairs
local function basis_rows()
  local ok, r0x, _, r0z, r1x, _, r1z, r2x, _, r2z = pcall(rivet.basis, hero)
  if not ok or r0x == nil then return nil end
  return { { r0x, r0z }, { r1x, r1z }, { r2x, r2z } }
end

local function heading()
  local rows = basis_rows()
  if rows == nil then return nil end

  local fx, fz = rows[FORWARD_ROW + 1][1], rows[FORWARD_ROW + 1][2]
  if fx == nil or fz == nil then return nil end
  if (fx * fx + fz * fz) < 0.0001 then return nil end   -- looking straight up or down

  return math.atan(fx * FORWARD_SIGN, fz * FORWARD_SIGN)
end

-- Offsets of everything close enough to land on the panel. Anything further out
-- would only pile up against the edge, and skipping it keeps the published
-- document inside the slot.
-- A collected pickup has its actor destroyed, so its handle stops resolving.
-- Only entries about to be drawn are checked - the ones off the panel do not
-- matter yet, and that keeps the per frame cost to the handful in range rather
-- than the whole level. Resolving a handle already held is ~0.5 us.
--
-- The name is compared too, not just resolvability: an actor slot can be reused
-- by something unrelated, which would otherwise leave a ghost marker behind.
local function alive(entry, match)
  if entry.dead then
    return false
  end
  local ok, name = pcall(rivet.name, entry.h)
  if not ok or name == nil or not string.find(name, match, 1, true) then
    entry.dead = true     -- sticky, so a collected pickup is never re-checked
    return false
  end
  return true
end

local function nearby(list, x, z, half, sin_a, cos_a, match)
  local out = {}
  if list == nil then
    return out
  end

  for i = 1, #list do
    local entry = list[i]
    if not entry.dead then
      local wx = entry.x - x
      local wz = entry.z - z

      -- rotate the world offset into view space, so the panel turns with Rivet
      -- and the blip at the centre always faces up
      local dx = wx * cos_a - wz * sin_a
      local dz = wx * sin_a + wz * cos_a

      -- alive() last: it is the only part that touches the scene, so the cheap
      -- range test rejects most entries before it runs
      if dx > -half and dx < half and dz > -half and dz < half and alive(entry, match) then
        -- %.0f not %d: lua 5.4 raises on a float for %d, and these are world
        -- offsets. The page only needs whole pixels of precision anyway.
        out[#out + 1] = string.format("[%.0f,%.0f]", dx, dz)
        if #out >= MAX_MARKERS then
          break
        end
      end
    end
  end

  return out
end

local function send_to_ui(x, z)
  local half = RANGE / 2

  -- -a, because turning right must sweep the world left under the blip
  local a = heading()
  local sin_a, cos_a = 0.0, 1.0
  if a ~= nil then
    sin_a, cos_a = math.sin(-a), math.cos(-a)
  end

  local g = nearby(gold, x, z, half, sin_a, cos_a, GOLD_MATCH)
  local c = nearby(rar, x, z, half, sin_a, cos_a, RAR_MATCH)
  local body = string.format('{"x":%.2f,"z":%.2f,"r":%.0f,"t":%.0f,"g":[%s],"c":[%s]}',
    x, z, RANGE, rivet.frame(), table.concat(g, ","), table.concat(c, ","))

  -- the slot is a fixed 1 KB; drop the markers rather than publish nothing
  if #body > 1024 then
    body = string.format('{"x":%.2f,"z":%.2f,"r":%.0f,"t":%.0f,"g":[],"c":[]}',
      x, z, RANGE, rivet.frame())
  end

  local ok = rivet.ui_publish(slot, body)
  slot = (slot + 1) % SLOTS
  return ok
end

-- Calibration by walking.
--
-- The first attempt had you face a pickup and aim; that was inconclusive
-- because aiming by eye is not accurate enough - the reading came back 0.734
-- against row 0 and 0.679 against row 2, which square-sum to 1.000, i.e. the
-- aim sat 45 degrees between two perpendicular axes and told us nothing.
--
-- Running forward is unambiguous: the position delta IS the forward direction,
-- and averaging it over many frames cancels the wobble.
local cal_on = false
local cal_n = 0
local cal_prev = nil
local cal_score = {}

local CAL_SAMPLES = 90        -- frames of movement to collect
local CAL_MIN_STEP = 0.02     -- ignore frames where she is basically still

local function cal_begin()
  cal_on = true
  cal_n = 0
  cal_prev = nil
  cal_score = {}
  for i = 1, 6 do cal_score[i] = 0 end
  rivet.log("minimap: calibrating - run FORWARD in a straight line")
end

local function cal_step(x, z)
  local rows = basis_rows()
  if rows == nil then return end

  if cal_prev == nil then
    cal_prev = { x = x, z = z }
    return
  end

  local mx, mz = x - cal_prev.x, z - cal_prev.z
  cal_prev.x, cal_prev.z = x, z

  local ml = math.sqrt(mx * mx + mz * mz)
  if ml < CAL_MIN_STEP then return end     -- standing still says nothing
  mx, mz = mx / ml, mz / ml

  for row = 0, 2 do
    local fx, fz = rows[row + 1][1], rows[row + 1][2]
    local fl = math.sqrt(fx * fx + fz * fz)
    if fl > 0.01 then
      for si, sign in ipairs({ 1, -1 }) do
        local dot = (fx * sign / fl) * mx + (fz * sign / fl) * mz
        local slot_i = row * 2 + si
        cal_score[slot_i] = cal_score[slot_i] + dot
      end
    end
  end

  cal_n = cal_n + 1
  if cal_n < CAL_SAMPLES then return end

  cal_on = false
  local best, best_row, best_sign = -2, FORWARD_ROW, FORWARD_SIGN
  for row = 0, 2 do
    for si, sign in ipairs({ 1, -1 }) do
      local avg = cal_score[row * 2 + si] / cal_n
      rivet.log(string.format("minimap:   row %d sign %2d -> %.3f", row, sign, avg))
      if avg > best then
        best, best_row, best_sign = avg, row, sign
      end
    end
  end

  FORWARD_ROW, FORWARD_SIGN = best_row, best_sign
  rivet.log(string.format("minimap: picked row %d sign %d (%.3f over %d samples)."
    .. " Near 1.0 is dead on; near 0 means you strafed or turned while running.",
    best_row, best_sign, best, cal_n))
end

rivet.on_key(rivet.key("F4"), cal_begin)

rivet.on_frame(function()
  if not rivet.scene_ready() then
    hero = nil
    return
  end

  if hero == nil then
    since_scan = since_scan + 1
    if since_scan < RETRY_FRAMES then
      return
    end
    since_scan = 0

    -- the scan can overrun the budget and raise, which is not a reason to lose
    -- the callback; try again after the next gap.
    local ok, found = pcall(rivet.find_actor, HERO)
    if not ok or found == nil then
      return
    end
    hero = found
  end

  -- rivet.position raises on a stale handle rather than answering nil, and a
  -- callback is retired after 3 consecutive errors. Handles go stale on any
  -- scene swap, which is routine, so catch it and re-resolve next frame.
  local ok, x, y, z = pcall(rivet.position, hero)
  if not ok then
    hero = nil
    since_scan = RETRY_FRAMES
    return
  end
  if x == nil then
    return
  end

  -- one kind per frame, so a scan that overruns cannot cost double
  if gold == nil then
    gold = scan(GOLD_MATCH)
  elseif rar == nil then
    rar = scan(RAR_MATCH)
  end

  -- x/z is the ground plane; y is height and has no place on a flat map
  last = { x = x, y = y, z = z }
  if cal_on then
    cal_step(x, z)
  end
  send_to_ui(x, z)
end)

local function zoom(factor)
  RANGE = math.max(RANGE_MIN, math.min(RANGE_MAX, math.floor(RANGE * factor)))
  rivet.log(string.format("minimap: range %.0f units across the panel", RANGE))
end

rivet.on_key(rivet.key("F7"), function() zoom(0.5) end)
rivet.on_key(rivet.key("F8"), function() zoom(2.0) end)

local function live_count(list)
  if list == nil then return "not scanned" end
  local n = 0
  for i = 1, #list do
    if not list[i].dead then n = n + 1 end
  end
  return string.format("%d of %d", n, #list)
end

rivet.on_key(rivet.key("F9"), function()
  if last == nil then
    rivet.log("minimap: no position yet (hero not resolved)")
    return
  end
  rivet.log(string.format("minimap: world %.1f, %.1f, %.1f", last.x, last.y, last.z))
  rivet.log(string.format("minimap: gold %s, raritanium %s, range %.0f",
    live_count(gold), live_count(rar), RANGE))
  local a = heading()
  rivet.log(string.format("minimap: forward row %d sign %d, heading %s",
    FORWARD_ROW, FORWARD_SIGN,
    a and string.format("%.1f deg", a * 180 / math.pi) or "unavailable"))
end)

rivet.on_key(rivet.key("F10"), rescan)

rivet.log("minimap: loaded, tracking " .. HERO .. " (F4 calibrate, F7/F8 zoom, F9 report, F10 rescan)")
