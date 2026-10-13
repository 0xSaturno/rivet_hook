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
without bit 63, which works for the session but should not be stored. The
actors of `rivet.spawn` are the exception to the bit: theirs has bit 63 too,
out of a range of the hook's own (`ee5f7eed...`), and is good for the session
only.
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

### Hero model

| | |
|---|---|
| `rivet.hero_look(path, [anims])` | the hero wears the model of that `.actor`, or that `.model` itself; `"applied"`, or `"loading"` while the asset loads. `anims` also puts on the asset's anim sets |
| `rivet.hero_look()` | the hero's own model and outfit back; `"restored"` |
| `rivet.hero_play_as(name)` | the game's own hero swap to `"ratchet"`, `"clank"`, `"rivet"` or `"kit"`; `"applied"` or `"loading"` |
| `rivet.hero_models([filter])` | the looks there are: `{ mods = { { path, mod } }, game = { { path, name } } }` |

```lua
rivet.hero_look("characters/hero/hero_Kit/hero_kit.actor")
rivet.hero_look("characters/mymod/my_rivet.model")
```

A `.model` can come from the game or from any mod folder, including a new path
no game file uses, and any number can be worn one after another in a session:
each one is let go when the next look or the restore replaces it. It animates
properly when it is rigged to Rivet's skeleton; a `.model` has no anim sets of
its own, so `anims` only applies to `.actor` paths.

`hero_models` lists every `.model` the mod paths in `rivet.toml` provide, with the
path entry each came from (a later mod replacing the same file wins), and the
game's own whole bodies on the gameplay skeleton. `filter` narrows both to paths,
mods or names containing it, in any case. The mod list is read once at startup,
like the mods themselves.

The last look put on is remembered in `rivet.toml` under `[hero_look]`, and a
restore forgets it. With `apply_on_launch = true` (the overlay's Hero tab has
the toggle) it goes back on once the hero first appears after the game starts.

`hero_play_as` is a different thing from a look: it is the game's own hero swap,
the same `TransformationEvent` the game sends, so the hero type, moves,
abilities and voice change with the model. The hero's actor asset is loaded
first when it is not, and the swap happens once it is. A worn look is dropped
before the swap, and a restore afterwards puts back the hero it is playing as
now, not the one it spawned as.

Only the look changes. Rivet keeps her moves, abilities, voice and weapons: the
hook makes the same model switch the game's own hero transformation makes and
skips the rest of it. Her outfit comes off while the other model is worn, and
comes back on restore.

An asset that is not loaded yet is loaded first and put on by the first pump
after it finishes. Without `anims` the model only animates properly when it
shares Rivet's skeleton. With it, the asset's own anim sets go on top of
Rivet's once the model has switched, so anything they cover plays the
character's own animation; moves only Rivet has still hold a bind pose. The
sets come off again on restore.

A respawned hero gets the look back about a second after it appears, with the
same anims choice. A cutscene that rebuilds the outfit can still undo it; call
`hero_look` again.

### Configs

| | |
|---|---|
| `rivet.configs([type], [limit])` | loaded configs as `{ id, type }`, `limit` defaults to 200 |
| `rivet.config(config)` | every field of one config as a table, plus `_type` |
| `rivet.config_set(config, field_path, value)` | write one numeric or boolean field; returns the previous value |

Configs are the game's authored tuning data, and about a thousand are loaded at
a time across some 180 classes: `HeroTraversalConfig`, `HeroMoverConfig`,
`HeroHoverbootConfig`, `WeaponConfig`, the `BotConfig*` family,
`CameraShakingConfig` and so on. `type` matches a class exactly, including every
class derived from it, or any class whose name contains it. `config` is the
config's asset path or its asset id as 16 hex digits, the `id` a listing
returns. `field_path` steps into nested structs with dots.

```lua
for _, c in ipairs(rivet.configs("HeroHoverbootConfig")) do
  rivet.log(c.id, c.type)
end
```

Edits are in place and live: anything that reads the config from then on sees
the new value. A component that copied a value when it started keeps its copy
until it is created again, so some edits need a respawn or a level reload to
show. Nothing is saved; the next time the config loads it is the authored one.

### Level scripts

| | |
|---|---|
| `rivet.script_nodes([filter], [limit])` | loaded script nodes as `{ component, actor, uid, class }`, classes containing `filter` |
| `rivet.script_node(component, [inputs])` | one node's wiring and vars, or `nil` and the reason |
| `rivet.signal(component, plug)` | fire an input plug on a script node |
| `rivet.signal(actor, class, plug, [nth])` | the same, finding the node by its actor and class |
| `rivet.hash(text)` | the engine's 32 bit string hash |

A zone's level scripts are graphs of script nodes wired together by plugs:
spawner waves, doors, cinematics, objectives. Every node is a component on an
actor without a scene object, most on an actor of their own and a few node
types packed onto one shared actor, so `rivet.actors` and
`rivet.find_component` never list them; `script_nodes` does. A node's `uid` is
its authored id and is the same every launch, so matching it in `script_nodes`
finds the node again after a reload (for a node with an actor of its own,
`rivet.find_uid(uid)` finds that actor too). A few uids show up twice, on
identical nodes of two copies of the same graph loaded at once. Its `component` handle is what
`script_node` and `signal` take, and changes every load.

`signal` queues the plug like the zone's own wiring would, and the node runs it
later in the same frame. `plug` is the plug's name (`"Start"`, `"In"`,
`"Activate"`…) or its hash as `0x` text. `nth` picks among several nodes of the
same class on one actor. Only script nodes can be signalled; anything else is
refused.

`script_node` answers:

| key | |
|---|---|
| `component`, `actor`, `class`, `uid` | the node |
| `graph_uid` | the graph node it was built from, shared by every copy of a subgraph |
| `zone` | asset id of the zone that loaded it |
| `outputs` | `{ plug, to = { component, actor, class }, to_plug }` per connection; one plug can fan out to several |
| `inputs` | the connections arriving, `{ plug, from = {...}, from_plug }`, found by scanning every loaded node (~16ms with 10k loaded); `inputs = false` skips the scan and leaves this out |
| `connections_in` | how many connections the node itself counts arriving |
| `vars` | `{ plug, var, live, type, value, dynamic, name }` per variable wired to it |

The engine keeps plugs as hashes only, so every plug comes with a `*_name` when
the name is known: a short built-in list of generic ones (`In`, `Out`, `Start`,
`Done`…), plus any names in `plug_names.txt` next to the game exe, one per line
(the last word of a line counts, `#` starts a comment). A node's input plugs
are not stored on the node, only on its senders, so an input nothing is wired
to is not listed but can still be fired. Var `type` is `bool`, `float`,
`vector`, `string` or `actors`; an `actors` value is a table of actor handles
with `group`, `count` and `uid`. `dynamic` marks engine driven globals, whose
stored value can lag behind what a node reads. Only globals have a `name`.

```lua
for _, node in ipairs(rivet.script_nodes("Spawner")) do
  local info = rivet.script_node(node.component)
  for _, out in ipairs(info.outputs) do
    rivet.log(node.class, out.plug_name or out.plug, "->", out.to.class, out.to_plug_name or out.to_plug)
  end
end
```

The overlay's Nodes tab is the same thing interactively: pick a node, click a
neighbour to follow the wiring, and fire any input plug by hand.

`rivet.hash` is the hash plug, event and class names all go through: a
reflected CRC32 seeded with `0xEDB88320` and no final xor.
`rivet.hash("PerformWarpEvent")` is the event's hash, `0x38008FE3`.

### Travel

| | |
|---|---|
| `rivet.checkpoints([filter])` | the level's checkpoints as `{ name, hash, region, area }`, names containing `filter` |
| `rivet.warp(checkpoint)` | warp the hero to a checkpoint, by name (`"CHK_SAV_01_LANDING"`) or `0x` hash |
| `rivet.fly(destination, [via])` | the ship's travel to any checkpoint: takeoff, planet tunnel, landing |
| `rivet.rift(checkpoint)`, `rivet.rift(x, y, z)` | the game's rift: pulled in, through the airlock, out at the target, on any planet |
| `rivet.zones(filter, [limit])` | the level's zones whose path contains `filter`, as `{ path, asset, route, checkpoint, region }` |
| `rivet.go(zone)` | go where a zone is loaded; returns what it did |
| `rivet.overlay(region, [load])` | load an overlay region on top of what is loaded, or unload it with `load = false` |

Every planet is a region of one level, and the level names its spawn points:
checkpoints, around 860 of them, from landing pads (`CHK_BLIZAR_A_LANDINGPAD`)
to mission steps, arenas and pocket dimensions. `warp` is the engine's own
checkpoint warp, the one level scripts trigger: the screen fades, the
checkpoint's region loads when it is not the current one (another planet,
say), and the hero is placed on its spawn point, still the same hero.

Warping moves the save's current checkpoint, as the game's own warps do. A
checkpoint on a planet or in a mission the save has not reached can leave the
save there. `area` is the top level region a checkpoint is in, named after its
file: `Savali`, `Savali (open world)`, `PrisonShip`.

`fly` is the ship's own travel. Accepting the planet menu hands the ship's
level script two checkpoint names, a planet tunnel (`CHK_TRANSITION_TO_SAVALI`,
one per planet) and a destination; the ship takes off, the tunnel's script plays
the flight and warps on to the destination. `fly` hands the ship the same
accept with any destination, so it needs a ship loaded nearby (the one whose
menu screen is nearest the hero is used). `via` picks the tunnel, as its full
name or just the planet part (`"SAVALI"`); left out, it is the tunnel of the
destination's planet. Warping straight into a tunnel checkpoint does not work:
the tunnel is a cinematic space with nothing to stand on.

`rift` is the game's passive shift, the rift that pulls the hero in, glides
them through an airlock while the far end loads, and lets them out to land.
The engine's `PassiveShiftController` runs it with four portals, and `rift`
spawns its own, once, from two test portal actors the game ships: one opens
next to the hero and pulls them in, the second is where they come out in the
airlock (the spot in the sky the game's own shifts use), the third opens in
front of them there, and the last is moved over the target. The first rift
loads the portal actors and opens as soon as they are in. The hero keeps some
of the glide's speed and lands a few dozen metres on, in the way they were
heading. If a shift has not ended after a minute it is stopped. The target can
be on another planet: the airlock loads it while the hero glides, as in the
story, and the spawned portals belong to no zone, so they outlive the unload.
When the portal actors cannot be loaded, the level's own rift portals are
borrowed instead, which only reaches the hero's own planet. The airlock is
plain sky: the story's airlock look comes from mission actors (a conduit actor
and a portal override volume) that only its missions load.

The engine never loads a single `.zone`. A level is a tree of regions, and a
region loads a list of zones: a planet's instanced areas, the 128 m tiles of an
open world that stream in by distance, and overlays, which load on top of
whatever else is loaded (pocket dimensions, Clank puzzles, mission content).
`rivet.go` looks up which regions list a zone and takes the first route that
works:

| route | |
|---|---|
| `loaded` | the zone is in the global region, always loaded |
| `checkpoint` | a warp to a checkpoint in the zone's region; for a tile, the checkpoint nearest to it in the same open world |
| `overlay` | the zone's overlay region is loaded on top, where its author placed it. The hero does not move |
| `story` | the zone is in a story overlay, out of reach (see below) |
| `none` | the zone is only in a region no checkpoint loads |

Most overlays belong to the story: a planet's state before and after each
mission, arena rounds, mission props. The game's custom overlay system loads
those while the save's mission state calls for them and unloads them within a
frame otherwise, so `rivet.overlay` refuses them rather than pretend. The rest,
around 25, are loaded on request by the rifts and puzzles and stay loaded when
asked for: pocket dimensions, Clank and hacking puzzles, arena platforms.
`level.regions` marks each overlay with `story` and whether it is `loaded`.

`zone` is a full path, the zone's asset id as 16 hex digits, or any part of
the path that only one zone has. Paths match without regard to case.

```lua
for _, c in ipairs(rivet.checkpoints("LANDING")) do
  rivet.log(c.area, c.name)
end
rivet.warp("CHK_NEFCITY_SHIP")

for _, z in ipairs(rivet.zones("savali/tile_a21")) do
  rivet.log(z.path, z.route, z.checkpoint)
end
rivet.go("savali/Tile_A21/Tile_A21_gp.zone")
rivet.overlay("SARG_PocketDim_01")
```

The overlay's Travel tab has the same three views: checkpoints by area, a zone
search with a Go button, and the overlay regions with Load and Unload.

### Spawned actors

| | |
|---|---|
| `rivet.spawn(path, x, y, z, [yaw], [sx, sy, sz])` | spawn the actor of an `.actor` and activate it; returns `handle, uid, state` |
| `rivet.destroy(handle)` | destroy an actor `spawn` made; `true`. `rivet.destroy("all")` destroys every one, drops the ones still loading and returns how many |
| `rivet.spawned()` | the actors `spawn` made, as `{ handle, uid, state, path, x, y, z, yaw, sx, sy, sz }` |

`spawn` is the engine's own spawn from an actor asset, the one the rift's
portals come from, and then the activate call the game's own spawns make right
after it. What the actor is, model, collision and behaviour, comes from the
asset. `yaw` is in degrees about the up axis, as the camera has it: 0 faces +Z
and turns toward +X. The scale is per axis, 1 when left out, and has to be
between 0.001 and 1000. Both go in with the spawn, where the engine sets the
actor up from them; `set_position` afterwards is the same nudge it is on any
other actor, so to move, turn or resize one, destroy it and spawn it again.
The position has to be within 4096 of the origin on every axis, which is as
far as a level and its physics reach.

`state` is `"active"`, or `"inactive"` when the engine did not activate the
actor. An asset that is not loaded yet is loaded first, and `spawn` returns
`nil, uid, "loading"`: the first pump after the asset is in spawns the actor,
with that uid, and `rivet.find_uid(uid)` or `rivet.spawned()` has its handle
from then on. Calling `spawn` again in the meantime asks for a second actor. A
waiting spawn is dropped, with a line in `rivet.log`, when its asset fails to
load, when 30 seconds go by without the asset loaded and a hero in the level,
or when the level changes under it: actor updates stopping for more than a
second, as they do for a load, or the hero being another actor than when it
was asked for. A path with no asset behind it raises at once.

Each actor gets a uid from a range of the hook's own, with bit 63 set like a
placed actor's, since that is the form the engine's uid lookups find. It is
good for the session, not across launches. The actors of one asset share their
prius, so `rivet.set_field` on one changes every actor of that asset, spawned
or placed.

`destroy` deactivates the actor, which takes it out of the scene, and hands it
to the engine's own deferred destroy: the engine frees it at the start of the
next actor update, and until then `spawned` lists it as `"destroying"`. It
only takes handles `spawn` gave out, so it cannot reach the hero or a level
actor. A spawned actor belongs to no zone; the hook gives it the flag the
engine's level unload goes by, so it is destroyed with the level, and it drops
out of `spawned` once the engine has destroyed it. There can be 256 at a time,
loading ones included.

`rivet.destroy("all")` also drops the spawns that are still loading. That is
the only way to call one of those off, since it has no handle yet. The number
it returns is how many actors it asked the engine to destroy or stopped
waiting for; one that was on its way out already is not counted again.

`spawn` and `destroy` refuse to run while the pump is on the render thread
(during loads), and `spawn` also while there is no hero. All three raise when
the hook did not find the engine's calls.

```lua
local cube = "environment/template/template_cube/template_cube.actor"
local x, y, z = rivet.position(rivet.hero())
local handle, uid, state = rivet.spawn(cube, x, y - 1, z + 4, 45, 4, 0.5, 4)

-- later, when it is in the way
handle = handle or rivet.find_uid(uid)
if handle then
  rivet.destroy(handle)
end
```

### Mesh collision (experimental)

| | |
|---|---|
| `rivet.collision_stage([stage])` | `{ stage, max_stage, available, reason, measured_build, faults_caught, latched, last_error }`; with a number, sets the stage for this session |
| `rivet.collision_status()` | what was resolved and what the engine has right now |
| `rivet.collision_inspect(zone)` | a loaded zone's physics: system, system data, bodies, shape tree |
| `rivet.collision_selftest([what])` | the constructor and heap tests; `what` is `"ctor"` or `"heap"` for one of them |
| `rivet.collision_borrow(zone, dx, dy, dz)` | queue a second body out of that zone's root shape, offset from the zone's own; a zone placed by a matrix is refused |
| `rivet.collision_bodies()` | `{ made, limit, paused, bodies }`, each body with its `state` |
| `rivet.collision_enable(body, [on])` | queue a body into the world, or out of it when `on` is `false` |
| `rivet.collision_disable(body)` | queue a body out of the world |
| `rivet.collision_remove(body)` | remove a body for good; `"all"` for every one that can go right now |
| `rivet.collision_cook(mesh, [flags])` | cook `"quad:<size>"` or a `.obj` into a shape, with no body; the answer has its `checks`, what was `measured` on it and what the hook has `written` into it |
| `rivet.collision_shapes()` | `{ cooked, limit, max_triangles, shapes }`, each shape with its reference count |
| `rivet.collision_release(shape)` | free a cooked shape no body uses; `"all"` for every such shape |
| `rivet.collision_body(shape, x, y, z, [yaw])` | queue a static body made of a cooked shape |

**Experimental: any of these past stage 1 can crash the game.** Collision for
a mesh that was not shipped with the game, made while it runs: the hook hands
triangles to the Havok mesh builder inside the exe and lets the engine make a
static body of the result, through the queue its own zones use. None of that
has run in the shipped game before the hook ran it, so it is switched on a
stage at a time with `[collision] stage` in `rivet.toml`, which is 0, off,
unless set:

| stage | |
|---|---|
| 1 | reads only: `collision_status`, `collision_inspect` |
| 2 | the first engine calls: `collision_selftest` |
| 3 | a body out of a loaded zone's own shape: `collision_borrow`, `collision_bodies`, `collision_enable`, `collision_disable`, `collision_remove` |
| 4 | the mesh cook, no body: `collision_cook`, `collision_shapes`, `collision_release` |
| 5 | a cooked shape as a body: `collision_body` |

A call above the stage raises, naming the stage it needs.
`rivet.collision_stage(n)` moves the stage for the session only, and not
below 3 while a body is still there. Everything but `collision_stage` and
`collision_status` raises while the pump is on the render thread, and a fault
inside an engine call switches the whole feature off until the game restarts,
after which only those two still answer. Catching a fault takes the MSVC
build of the hook: any other build stops at stage 1, and says so as
`faults_caught`. Body, shape and stage numbers and the flags have to be whole
numbers in range, nothing is cut down to fit. The stages, the states a body
goes through, the `.obj` subset and the checks a mesh has to pass are
described in [PIPE_BRIDGE.md](PIPE_BRIDGE.md#mesh-collision-experimental);
the functions here are the bridge's commands and return its answers as
tables, with a key that would be `null` left out.

A body is made by the engine a frame or two after it was asked for, and is
only in the world once a physics step has run, so a script polls for `"live"`
rather than using the body in the same frame. Removing is deferred the same
way: the body leaves the world first and is destroyed after, and a shape can
be released once no body of it is listed any more. Remove every body before a
warp or any other travel: `rivet.warp`, `rivet.go` by a checkpoint,
`rivet.fly` and `rivet.rift` raise while one is still there, and
`rivet.overlay(region, false)` while one borrows a loaded zone's shape. The
game's own travel is not held back, and a physics world that is not the one a
body was made in drops the body, and every cooked shape, as `"lost"`.

```lua
-- a 4 m square to stand on, three metres over the hero
local x, y, z = rivet.position(rivet.hero())
local shape = rivet.collision_cook("quad:4")
if shape.usable then
  local body = rivet.collision_body(shape.shape, x, y + 3, z)
  rivet.log("body", body.body, body.state)
end

-- later
rivet.collision_remove("all")
```

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
address space. The exception is `rivet.collision_cook`, which opens the `.obj`
a script names and reads it, and only from `[collision] stage` 4 on.

## Example

See [`scripts/example.lua`](../scripts/example.lua).
