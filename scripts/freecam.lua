-- SPDX-FileCopyrightText: 2025-2026 Neptuwunium
--
-- SPDX-License-Identifier: EUPL-1.2

-- A free camera. Copy this next to the game exe as scripts/freecam.lua.
--
--   numpad 0           detach / reattach the camera
--   numpad 8 / 5       forward / back
--   numpad 4 / 6       left / right
--   numpad 9 / 3       up / down
--   arrow keys         look
--   numpad + / -       faster / slower
--   shift              four times as fast while held
--   numpad .           camera shake on / off, overriding the game option
--
-- The game keeps running on its own camera while this one is detached, so Rivet
-- still answers to the controller and keyboard, and aiming still works from the
-- game camera. rivet.time_scale(0.1) pairs well with it for slow motion shots.
-- Everything is on the numpad so it stays clear of minimap.lua's F keys.
-- Movement goes by real time, so it keeps its speed under any time scale.

local speed = 8      -- metres per second
local turn = 90      -- degrees per second

local function down(name)
  return rivet.is_key_down(rivet.key(name))
end

-- a short note on the hud as well as the log. pcall, because the hud refuses
-- while messages are off in the options, and that should not stop the camera
local function say(text)
  rivet.log("freecam: " .. text)
  pcall(rivet.notify, text, { type = "corner", duration = 1.5 })
end

rivet.on_key(rivet.key("NUMPAD0"), function()
  if rivet.camera_detached() then
    rivet.camera_attach()
    say("Camera attached")
  else
    rivet.camera_detach()
    say("Free camera")
  end
end)

rivet.on_key(rivet.key("DECIMAL"), function()
  local blocked = rivet.shake_block(not rivet.shake_block())
  say("Camera shake " .. (blocked and "off" or "on"))
end)

rivet.on_key(rivet.key("ADD"), function()
  speed = speed * 2
  say("Speed " .. speed)
end)

rivet.on_key(rivet.key("SUBTRACT"), function()
  speed = math.max(speed / 2, 0.25)
  say("Speed " .. speed)
end)

rivet.on_frame(function(dt)
  if not rivet.camera_detached() then
    return
  end

  -- a hitch, or the first frame after a load, should not fling the camera
  dt = math.min(dt, 0.1)

  local x, y, z, yaw, pitch = rivet.camera()

  if down("LEFT") then yaw = yaw - turn * dt end
  if down("RIGHT") then yaw = yaw + turn * dt end
  if down("UP") then pitch = pitch + turn * dt end
  if down("DOWN") then pitch = pitch - turn * dt end

  local move = speed * dt * (down("SHIFT") and 4 or 1)
  local r = math.rad(yaw)
  local p = math.rad(pitch)

  -- forward follows the look direction, pitch included; strafing stays level
  local fx, fy, fz = math.sin(r) * math.cos(p), math.sin(p), math.cos(r) * math.cos(p)
  local rx, rz = math.cos(r), -math.sin(r)

  local forward = (down("NUMPAD8") and 1 or 0) - (down("NUMPAD5") and 1 or 0)
  local strafe = (down("NUMPAD6") and 1 or 0) - (down("NUMPAD4") and 1 or 0)
  local rise = (down("NUMPAD9") and 1 or 0) - (down("NUMPAD3") and 1 or 0)

  x = x + (fx * forward + rx * strafe) * move
  y = y + fy * forward * move + rise * move
  z = z + (fz * forward + rz * strafe) * move

  rivet.camera_set(x, y, z, yaw, pitch)
end)

rivet.log("freecam.lua loaded: numpad 0 to detach the camera")
