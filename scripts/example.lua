-- SPDX-FileCopyrightText: 2025-2026 Neptuwunium
--
-- SPDX-License-Identifier: EUPL-1.2

-- Copy this next to the game exe as scripts/example.lua and set
-- [scripts] enabled = true in rivet.toml. F6 reloads every script without a
-- restart, so editing this file does not cost a relaunch.

rivet.log("example.lua loaded")

-- Looking an actor up by name walks the whole scene. Measured in Megalopolis:
-- 71680 actors at about 130 ns each, so roughly 6 ms to reach the hero and 9 ms
-- for a full miss. That is most of a frame's budget, and the scan is stopped
-- with an error if it runs past it. So look the handle up once, hold on to it,
-- and only look again when it stops resolving - handles do not survive a level
-- change, and they differ every launch.
local hero = nil

local function get_hero()
  if hero ~= nil then
    -- cheap: resolves one handle, no scan
    local ok, name = pcall(rivet.name, hero)
    if ok and name == "Rivet" then
      return hero
    end

    hero = nil
  end

  if not rivet.scene_ready() then
    return nil
  end

  hero = rivet.find_actor("Rivet")
  if hero == nil then
    rivet.log("no actor called Rivet in this scene")
  end

  return hero
end

-- on_frame gets the real frame delta. Keep it cheap: it runs on the render
-- thread, so anything slow here is a visible stutter.
--
-- Smooth the frame time and invert that, never the other way round. Averaging
-- 1/dt lets one short frame dominate: present is called back to back sometimes,
-- and a 0.03 ms frame is 30000 fps going into the average.
local frame_time = 0

rivet.on_frame(function(dt)
  if dt > 0 then
    frame_time = frame_time == 0 and dt or frame_time * 0.98 + dt * 0.02
  end
end)

-- F1 bumps a prius field. Prius is authored config that nothing recomputes per
-- frame, so unlike the transform this write sticks. Whether anything acts on it
-- depends on the system: a value read once at component init will not notice.
rivet.on_key(rivet.key("F1"), function()
  local h = get_hero()
  if h == nil then
    return
  end

  local was = rivet.set_field(h, "AnimControllerComponent", "MaxUpdateDistance", 500.0)
  rivet.log("MaxUpdateDistance", was, "->", rivet.field(h, "AnimControllerComponent", "MaxUpdateDistance"))
end)

-- F2 reports where the hero is and what it is built out of.
rivet.on_key(rivet.key("F2"), function()
  local h = get_hero()
  if h == nil then
    return
  end

  local x, y, z = rivet.position(h)
  local fps = frame_time > 0 and 1 / frame_time or 0
  rivet.log(string.format("%s at %.2f %.2f %.2f, %.1f ms (%.0f fps)", rivet.name(h), x, y, z, frame_time * 1000, fps))

  local components = rivet.components(h)
  rivet.log(#components, "components, first few:", table.concat(components, ", ", 1, math.min(#components, 5)))

  -- a file field also returns its asset id, as hex text: ids run past 2^53,
  -- where a lua number would start rounding them
  local path, id = rivet.field(h, "AnimControllerComponent", "PerformanceSet")
  rivet.log("PerformanceSet", path, id)
end)
