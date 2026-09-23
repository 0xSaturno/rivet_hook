<!--
SPDX-FileCopyrightText: 2025-2026 Neptuwunium

SPDX-License-Identifier: EUPL-1.2
-->

# Lua scripting

rivet_hook embeds Lua 5.4 and runs `scripts/*.lua` from the game directory. A
script gets per-frame and per-key callbacks and can read and write live engine
state without a rebuild, and it hot-reloads, so a change costs a key press
rather than a restart.

Off by default. In `rivet.toml`:

```toml
[scripts]
enabled = true
path = "scripts"
reload_key = "F6"
budget_ms = 8
check_interval = 10000
error_limit = 3
```

Scripts load in file name order, share one VM, and write to `rivet.log`. `print`
is redirected there too, since the game has no console.

## Where the code runs

Everything runs on the **game thread**, in the middle of the frame: the hook
sits on the callback the engine runs between actor update passes, the same slot
the game uses for its own mid frame actor code. Components are not ticking while
a script runs, so reads see a settled frame and writes land before the rest of
the frame's updates pick them up. The bridge pumps from the same place.

When actor updates stop (loading screens) or the hook could not be placed,
the `present` hook on the render thread stands in after a quarter of a second,
so the bridge and scripts keep running. That fallback is not a safe point: it
runs alongside the game thread. `python tools/rivetctl.py ping` shows which
thread has been pumping (`pump.game_pumps` against `pump.render_pumps`).

Two further consequences:

- A slow script is a visible stutter. Each callback gets `budget_ms`
  milliseconds of wall clock; overrun and it is stopped with an error.
- A callback that fails `error_limit` times in a row is switched off rather than
  spamming the log every frame. Fix it and reload.

Key presses arrive on the input thread, so they are queued and dispatched by the
next frame, not inline.

## API

### Lifecycle

| | |
|---|---|
| `rivet.on_frame(function(dt) end)` | once per presented frame, `dt` in seconds |
| `rivet.on_key(vk, function() end)` | on release of that virtual key |
| `rivet.log(...)` | writes one line to `rivet.log` |
| `rivet.is_key_down(vk)` | `GetAsyncKeyState` |
| `rivet.key(name)` | virtual key code by name, the same names `rivet.toml` takes |
| `rivet.time()` | seconds since the VM started |
| `rivet.frame()` | frames pumped since the VM started |

### Scene

| | |
|---|---|
| `rivet.scene_ready()` | whether the scene manager is up |
| `rivet.find_actor(name)` | actor handle by exact name, else first substring match, else `nil` |
| `rivet.actors([substring], [limit])` | array of handles, `limit` defaults to 64 |
| `rivet.hero()` | the player actor's handle, from the game's own hero record, or `nil` |
| `rivet.uid(handle)` | the actor's uid as 16 digit hex, or `nil` if it has none |
| `rivet.find_uid(uid)` | the loaded actor with that uid (hex text or integer), or `nil` |
| `rivet.find_component(class, [limit], [exact])` | handles of actors holding a live component of that class or one derived from it (`exact` skips derived), `limit` defaults to 64 |
| `rivet.name(handle)` | actor name |
| `rivet.position(handle)` | `x, y, z` |
| `rivet.set_position(handle, x, y, z)` | `"warp"` for the hero, `"write"` for anything else |
| `rivet.components(handle)` | array of component type names |
| `rivet.dump(handle)` | writes `rivet_actor_<name>.json`, returns the path |

`set_position` on the hero sends a `PerformWarpEvent` (see [Events](#events)), so
the move sticks and the camera follows; the warp lands later in the same frame,
so `rivet.position` reads the old spot until the next one. On any other actor it
writes the transform, which the engine overwrites within a frame.

`find_actor` prefers an exact name and falls back to the first substring match,
because substring alone is not good enough: in Megalopolis `"Rivet"` matches
`test_npc_Rivet_Cine` first, which sits earlier in the array and carries no
components.

`find_actor` and `actors` answer `nil` and `{}` while the scene manager is not
up yet, because a script waits either way. Everything else raises, since a
handle that does not resolve is a script bug.

**Prefer `rivet.hero` and `rivet.find_component` over a name scan.** Both answer
from the engine's own indexes: `hero` is a single read, and `find_component`
walks the live component list (about 60,000 entries in a loaded level) rather
than every actor slot. `find_component` takes an exact class name, the same
names `rivet.components` returns.

**To remember an actor across loads, keep its uid, not its handle.** A handle
names a slot and goes stale on any level change; a uid is the actor's identity
and, for zone placed actors (bit 63 set: a leading hex digit of `8` or higher),
the same every launch. Spawned actors, the hero included, get a runtime uid
without bit 63, which works for the session but should not be stored.
`rivet.find_uid` is a single probe of the engine's own uid table.

**Look an actor up once and keep the handle.** Both scans walk the whole scene,
and both stop at the frame budget and raise if they hit it. Measured in
Megalopolis: 71,680 actors at about 130 ns each, so ~6 ms to reach the hero and
~9 ms for a full miss — most of an 8 ms budget, and over it once the frame
jitters. Resolving a handle you already hold costs about 0.5 us, so re-check a
cached handle with `rivet.name` instead of scanning again. Handles do not
survive a level change and differ every launch, so they are worth re-checking,
just not by scanning.

**Guard the calls that raise, or a routine event kills the callback.** A stale
handle raises rather than answering `nil`, and a callback is switched off after
`scripts.error_limit` consecutive errors. A handle goes stale on any level
change or rift, which is normal play, so an unguarded `rivet.position` on a
cached handle is a callback that dies the first time the player travels:

```lua
local ok, x, y, z = pcall(rivet.position, hero)
if not ok then
  hero = nil       -- re-resolve on a later frame
  return
end
```

The same applies to the scans: they raise when they overrun the budget, which is
a bad frame rather than a bug, so `pcall` those too and try again later.

### Prius fields

| | |
|---|---|
| `rivet.field(handle, component, field, [element])` | the value; a string or file field also returns its hash or asset id as 16-digit hex |
| `rivet.set_field(handle, component, field, value, [element])` | writes, returns the previous value |

Some components share one prius between every instance of the actor type
(`rivet.component(...).shared` is `true`, `prius_behavior` is `ReadOnly`). A
`set_field` on one of those changes every copy at once; the write still goes
through, and `rivet.log` says so the first time it happens for that component.

`element` is 1-based and only meaningful for a fixed array field.

The second return value for a string or file field is **hex text, not a number**:
asset ids run past 2^53, where a Lua number stops counting by ones, so returning
one as a number reports a rounded id while looking exact.

Prius is authored config that nothing recomputes per frame, so unlike the
transform these writes stick. Whether anything *acts* on one is a separate
question: a field read once at component init will not notice.

Only numeric and boolean fields can be written. Strings and file references
store a pointer whose target would have to outlive the write, and the 64 bit id
types do not survive a round trip through a Lua number, so all of those are
refused rather than silently mangled. Every write is range checked against the
field's own width and confirmed to land in writable memory first.

### Raw instance memory

| | |
|---|---|
| `rivet.component(handle, name)` | `{ address, size, handle, prius, prius_size, prius_behavior, shared }`, addresses as hex text |
| `rivet.read(address, length)` | `length` bytes as space separated hex, or `nil` if unreadable; capped at 512 |

Not everything a component holds is in its prius. The equipped skin, for one, is
runtime state living in the instance, and `rivet.field` cannot see any of it.
These two read the instance directly so a script can diff it against itself over
time - see [`scripts/watch_skin.lua`](../scripts/watch_skin.lua), which learns
which offsets churn every frame and then reports only the ones that do not.

Addresses are hex text for the same reason asset ids are: they run past what a
Lua number counts exactly.

### Components

| | |
|---|---|
| `rivet.detour(component, slot, on)` | skip a component update |

Slots are `first`, `first_results`, `middle`, `last`, `async`,
`async_results`. This shares the bridge's detour pool, so `component.detours`
over the bridge reports the call and skip counters for anything a script
installed.

### Events

| | |
|---|---|
| `rivet.queue_event(name, options)` | queue an engine event; returns its address as hex text |
| `rivet.on_event(name, fn)` | `fn(ev)` for every event of that class, or a class derived from it |

Most cross-system verbs in the game are events: warps, damage, vanity overrides,
time scale requests, ui sounds, cinematic triggers. `name` is the event class
name (`"PerformWarpEvent"`) or its name hash as hex text (`"0x38008fe3"`); the
hash is the class's DDL type id. `python tools/rivetctl.py event.classes Warp`
lists what is registered and `event.info <name>` lists a class's fields.

`options` is a table, every key optional:

| key | |
|---|---|
| `target` / `targets` | one handle, or a list of up to 64 |
| `sender` | handle the event claims to come from |
| `broadcast` | defaults to `true` with no targets, `false` with them |
| `exclude` | the targets are excluded instead of addressed |
| `radius` | broadcast radius |
| `delay` | seconds before it is delivered |
| `position` | `{x, y, z}`; otherwise the engine uses the sender's position |
| `fields` | `{ ["Destination.Position.X"] = 12.5, ResetCamera = true }` |

The engine allocates the event, fills in its defaults and hands it back still
unsent; `fields` are written into it then, before it is dispatched later in the
frame. A path steps into nested structs with dots. Only numeric and boolean
fields can be written, and every path is checked before anything is queued.

```lua
local hero = rivet.hero()
rivet.queue_event("PerformWarpEvent", { target = hero, fields = {
  ["Destination.Position.X"] = 10, ["Destination.Position.Y"] = 0,
  ["Destination.Position.Z"] = 5, ResetCamera = true,
} })

rivet.on_event("PerformWarpEvent", function(ev)
  rivet.log(ev.class, ev.sender, ev.fields.Destination.Position.X)
end)
```

`ev` is `{ class, sender, targets, broadcast, address, fields }`, with `fields`
decoded like `rivet.field` does, nested structs as tables. Dynamic arrays are
left out of it; `event.watch` over the bridge records them.

Callbacks see the events that go through the main event queue, picked up once
per pump. Events queued from worker threads and delivered in the same frame
never enter that queue and are not seen. `on_event("EventBase", fn)` sees
everything else, which is a few hundred calls a frame: filter by class rather
than doing that.

Verified live on 2026-09-23: all 2197 classes read back, `event.tail` shows the
game's own traffic, and a `PerformWarpEvent` sent to the hero moved them to the
written `Destination.Position` (Y is up). Warps go exactly where they are told,
off a ledge included. `python tools/rivetctl.py event.status` says whether the
class table read back sane after a game patch.

### Time and camera

| | |
|---|---|
| `rivet.time_scale()` | the speed the game is running at right now |
| `rivet.time_scale(scale, [channel], [ramp])` | ask for `scale` on a channel, `"Game"` by default |
| `rivet.clear_time_scale([channel])` | put a channel back to normal speed |
| `rivet.fov_scale([scale])` | read or set the multiplier on the camera's field of view |

The game runs at the lowest scale any of its 25 time scale channels asks for
(the cinematic channel outranks the rest), and eases toward it at `ramp` per
second, 30 unless given. Physics follows the same scale. Channels are named as
the game names them, with or without the leading `k`: `"Game"`, `"Dodge"`,
`"HeroAimMode"`, or a number `0`-`24`. A scale holds until it is cleared or the
level unloads, and a script's `"Game"` request does not stop the game's own
slow-mo moments from going lower. Both calls refuse to run while the pump is on
the render thread (during loads), because clearing a channel destroys the
components that were driving it.

`fov_scale` is the same value the game's field of view slider writes, so it is
often 1.25 rather than 1.0, and applying the graphics settings puts the slider's
value back. Kept between 0.1 and 4.

#### Free camera

| | |
|---|---|
| `rivet.camera()` | `x, y, z, yaw, pitch, fov` of the view the player sees |
| `rivet.camera_detach()` | the view stops following the game, starting where the game camera is |
| `rivet.camera_set(x, y, z, [yaw], [pitch], [fov])` | move the detached view; left out keeps current |
| `rivet.camera_attach()` | give the view back to the game |
| `rivet.camera_detached()` | whether it is detached |
| `rivet.shake_block([blocked])` | override camera shake on the view (`true` blocks, `false` allows, `"game"` hands it back to the option); returns whether it is blocked |

Angles are degrees. Yaw 0 looks down +Z and turns toward +X, pitch is positive
looking up and is kept within ±89. `fov` is the camera's own field of view,
before `fov_scale`.

Detaching hands the renderer a camera the hook owns, through the engine's own
debug camera slot, while the game keeps updating its real camera underneath. So
Rivet still answers to input, and aiming and camera relative movement go by the
game camera, not the free one. Sound follows the free camera. The view stays
detached across level loads until `camera_attach`.

The game copies its camera shake option into the bit `shake_block` reads every
frame, so with shake turned off in the options it already reads `true`, and an
override is held and re-applied every pump until `shake_block("game")`. Shake
only ever moves the game's camera, never the free one, and shakes the game marks
as forced get through either way.

[`scripts/freecam.lua`](../scripts/freecam.lua) is a ready to use free camera
on the numpad and the arrow keys.

### HUD messages

| | |
|---|---|
| `rivet.notify(text, [options])` | show `text` in one of the game's own HUD message slots |

`options` is a table: `type` (default `"generic"`), `duration` in seconds
(default 3, at most 60), and `sub` for a smaller second line.

```lua
rivet.notify("Freecam on")
rivet.notify("Slow motion", { type = "center", duration = 2 })
rivet.notify("Arena", { type = "planet", sub = "Wave 3" })
```

Only the `planet` and `tutorial` slots draw `sub`; the vanilla `hud.html` binds
nothing but the message for the others, so their second line is dropped.

The types are the game's own slots: `generic`, `center`, `pickup`,
`location`, `planet`, `corner`, `tutorial`, `arena_wave` and `arena_reward`.
Each slot shows one message at a time and a new one replaces it, including one
the game itself put there. The text goes through the game's icon markup like
any of its own messages. Nothing shows while HUD messages are turned off in the
game's options, and `notify` raises saying so instead of failing silently.

### Outfits

| | |
|---|---|
| `rivet.vanity_equip(bundle, [actor])` | put on an owned armor piece the way the game does; `true` if anything new went on |
| `rivet.vanity_owns(bundle, [actor])` | whether that piece is unlocked |

`bundle` is the bundle config's asset path or its asset id as 16 hex digits.
`actor` defaults to the hero, the only actor that carries a vanity inventory.
Each armor set has a head, torso and legs bundle:

```lua
local set = "configs/hero/vanity/vanitybundles/carbonox_armor/"
for _, part in ipairs({ "head", "torso", "legs" }) do
  rivet.vanity_equip(set .. "hero_vanity_bundle_" .. part .. "_carbonox_armor.config")
end
```

This goes through the game's own vanity inventory, so the parts load, the
choice is the same one the pause menu makes, and it may be saved like one.
Pieces the hero has not unlocked are refused: the game looks the bundle's config
up without checking it exists, so ownership is checked first.

### The game UI

| | |
|---|---|
| `rivet.ui_publish(slot, text)` | write utf-8 text into a ui slot a cohtml page can poll; `true` if it took |

`slot` is `0..7`, and each holds 1024 bytes. They are registered during asset
load as ordinary mod assets at `ui/loaded/exported/hud/mm_0.json` through
`mm_7.json`, so a page reaches them the way it reaches any other file it ships
with.

Publishing does not create or replace an asset, it only rewrites the bytes of a
buffer already in the table. That is deliberate: the asset map is read by loader
threads while a script runs on the render thread, and never touching the map
means no lock is needed. Text shorter than the slot is padded with spaces rather
than shortening the buffer, so a reader can never see a short read and
`JSON.parse` ignores the tail. A reader *can* still catch a write in progress and
get a torn document; it should catch the parse error and skip that tick.

There is no push in the other direction. The only cohtml hook in the runtime is
`cohtml::Library::DecodeURLString`, a static with no `View` pointer, so
`View::TriggerEvent` cannot be called and a page cannot be notified. It has to
poll. `cohtml.WindowsDesktop.dll` exports 235 symbols and `View` and `System`
are absent from all of them, and from the RTTI, so reaching a view would mean
walking vtables from `Library::Initialize` against Cohtml 1.13.1.3 headers.

Things worth knowing before writing the page half, all of which cost a debugging
round trip:

- `fetch` does not exist in this cohtml. Use `XMLHttpRequest`.
- `document.documentElement.clientWidth` answers `0`, so nothing can be
  positioned by measuring the viewport.
- A view may cache a response per url. Cycle the slots and have the page walk
  the same ring, so it always asks for one whose contents have changed.
- A relative url resolves inside the requesting document's own folder. From
  `exported/Overlay/` the slots are `../HUD/mm_0.json`.
- `HUD.html` is loaded into five views and `Overlay.html` into one, so anything
  added at body level in the HUD is drawn once per view, each at that view's
  scale. New UI belongs in Overlay, which is a single full-screen view authored
  in 1920x1080 pixels.

[`scripts/minimap.lua`](../scripts/minimap.lua) is a worked example: it publishes
the hero's position and nearby pickups every frame, and the page that reads it
draws them on a minimap.

## Driving it from outside

With `[bridge] enabled = true` as well:

```bash
python tools/rivetctl.py script.status
```

```bash
python tools/rivetctl.py script.reload
```

```bash
python tools/rivetctl.py script.exec "return rivet.name(rivet.find_actor('Rivet'))"
```

`script.status` is the counter report: what loaded, what is registered, how many
times each kind of callback has run, how long the last frame's dispatch took,
and the last error with its traceback. It is how a callback is proven to have
fired rather than eyeballed.

## Sandbox

`io`, `package` and `debug` are not opened, and `os` keeps only its clocks —
`execute`, `exit`, `remove`, `rename`, `tmpname`, `getenv` and `setlocale` are
removed. A script still runs arbitrary code inside the game process, which is
why `[scripts] enabled` defaults to false, but it cannot reach the disk or start
a process by accident, and `require` cannot pull a foreign dll into the game's
address space.

## Example

See [`scripts/example.lua`](../scripts/example.lua).
