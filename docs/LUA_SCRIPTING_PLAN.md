<!--
SPDX-FileCopyrightText: 2025-2026 Neptuwunium

SPDX-License-Identifier: EUPL-1.2
-->

# Lua scripting for rivet_hook — implementation plan

Written 2026-09-07 to hand to a fresh session. Everything marked **verified** was
measured against the running game; everything marked **assumed** was not.

## Status (implemented 2026-09-07)

Built as `src/scripting.{hpp,cpp}` plus a Lua 5.4.8 meson wrap. The API
reference is [LUA_SCRIPTING.md](LUA_SCRIPTING.md). Where the milestones stand:

| Milestone | State |
|---|---|
| 1. VM boots | **verified offline** |
| 2. Callbacks | **verified offline**, counters exposed as `script.status` |
| 3. Read only bindings | **verified in game** against `actor.get` and `actor.dump` |
| 4. Hot reload | **verified offline**, `script.reload` and F6 |
| 5. Writes | **verified in game**, write and read back on a live prius field |
| 6. Detour binding | **verified in game**, shares the bridge pool |

In game on Megalopolis: 46006 frames with the example loaded, 0.0004 ms a frame,
no callback disabled, and the game survived a deliberate infinite loop because
the budget stopped it. `rivet.position` and `rivet.field` matched `actor.get` and
the `actor.dump` json exactly; `set_field` wrote 500.0 over
`AnimControllerComponentPrius::MaxUpdateDistance` and read it back; a lua
installed detour on `StrafeCameraMover::middle` counted 1026 calls and reported
the dispatcher at +0xf23248, the same call site the plan records.

Two defects only the live run could expose, both fixed:

- `find_actor` matched on substring alone, so `"Rivet"` found
  `test_npc_Rivet_Cine` (earlier in the array, zero components) instead of the
  hero. An exact name now wins, with the first substring match as fallback.
- a string or file field returned its id as a lua number, which rounds past
  2^53: `PerformanceSet` read back 12034081025008338944 against a true
  12034081025008339780. Ids are hex text now.

"Verified offline" means an out of tree harness that stubs the engine and drives
the host directly: 36 checks covering the vm coming up, both callback kinds
firing, the budget stopping a runaway loop, the error limit switching a callback
off, reload replacing the loaded set, the sandbox removals, and the shipped
example loading. Everything that needs a live scene is still unproven, so
milestones 3, 5 and 6 stand as written but untested against the game.

Two things came out of the build that are not in the plan below:

- `ddl::is_writable` was added next to `is_readable`, sharing the same
  `VirtualQuery` cache. Plenty of readable engine memory is mapped read only and
  a blind store would fault, so `set_field` checks before it writes.
- `find_actor` and `actors` answer `nil` and `{}` while the scene manager is not
  up, rather than raising. Raising switched off any per frame lookup during a
  level load, because three consecutive errors disable a callback.
  `rivet.scene_ready()` tells the two cases apart.

## Goal

Run user-written Lua inside the game with per-frame and per-key callbacks, so
custom logic can read and edit live engine state without a rebuild. Scripts live
in `scripts/*.lua` next to the game exe and hot-reload.

**Non-goal: freecam.** See "Do not start with freecam" at the bottom. Pick a
different first demo or you will burn the session the way the last one went.

## What already exists (build on this, do not rediscover it)

| Piece | Where | State |
|---|---|---|
| DDL field walk with visitor sinks | `src/ddl_visit.{hpp,cpp}` | verified, 3 offline harnesses |
| Live prius reader + JSON dump | `src/ddl_inspector.{hpp,cpp}` | verified in game |
| Named-pipe bridge + command dispatch | `src/bridge.cpp` | verified |
| Client | `tools/rivetctl.py` | verified |
| Component update detours with counters | `src/bridge.cpp`, `component.detour` | verified |
| Per-frame hook point | `bridge::pump()` called from `present` in `src/overlay.cpp` | verified |

The bridge is *not* a substitute for Lua: it is one command per frame with
round-trip latency. Lua's whole reason to exist here is running **inside** the
frame.

## Design

### VM

Lua 5.4 via a meson wrap, added to the `loader_only == false` source list next to
`bridge.cpp`. Not LuaJIT — the x64 Windows build is more trouble than the speed
is worth here.

### Threading

Everything runs on the **render thread**, from the same `pump()` call in
`present` that the bridge uses. There is no other safe place to touch engine
state. Consequences:

- A slow script is a visible stutter. Budget it (see Safety).
- Lua and bridge commands must not run concurrently — reuse the existing
  single-request handoff, or take the same lock.

### Proposed API (v1)

```lua
rivet.on_frame(function(dt) end)      -- once per presented frame
rivet.on_key(vk, function() end)      -- from Overlay::HandleKeyPress
rivet.is_key_down(vk)                 -- GetAsyncKeyState
rivet.log(msg)                        -- to rivet.log

local a = rivet.find_actor("Rivet")   -- handle or nil
rivet.actors("Camera")                -- filtered list
rivet.name(a)
rivet.position(a)                     -- x, y, z
rivet.set_position(a, x, y, z)

rivet.field(a, "AnimControllerComponent", "MaxUpdateDistance")
rivet.set_field(a, "AnimControllerComponent", "MaxUpdateDistance", 50.0)

rivet.detour("StrafeCameraMover", "middle", true)   -- skip an update
```

`field`/`set_field` are the valuable ones — they fall out of the DDL walk that
already works, and prius fields are authored config that nothing recomputes per
frame, so **writes to them stick**.

### Hot reload

`script.reload` as a bridge command plus a key binding. This is the single
biggest quality-of-life win: today every change costs a full game restart,
because the DLL cannot be replaced while the game holds it. Script changes
should not.

## Milestones

Verify at each step before starting the next. Every time that discipline was
skipped last session, the conclusion was wrong.

1. **VM boots.** Lua subproject builds; `rivet.log(...)` from a script appears in
   `rivet.log`. No engine access yet.
2. **Callbacks.** `on_frame` fires each frame, `on_key` fires on the right key.
   Prove it with a counter exposed through a bridge command, not by eyeballing.
3. **Read-only bindings.** `find_actor`, `name`, `position`, `field`. Compare a
   script's output against `actor.dump` over the bridge for the same actor.
4. **Hot reload.** `script.reload` re-runs scripts without a restart.
5. **Writes.** `set_field` first (safe, sticks), `set_position` second.
6. **Detour binding.** Expose the existing pool to Lua.

## Safety requirements

These are not theoretical. Each one is a bug that was hit this session.

- **Instruction budget.** `lua_sethook` with `LUA_MASKCOUNT` plus a per-frame
  wall-clock cap. A runaway script must not hang the render thread. A
  `scene.actors` walk once stalled the game for 8.3 seconds.
- **Catch everything.** An exception escaping into `present` kills the game.
  Wrap every Lua entry point. Disable a script after N consecutive errors rather
  than spamming.
- **Never hand a raw engine pointer to anything that scans for a terminator.**
  Use `ddl::read_string`, which asks `VirtualQuery` where the region ends and
  truncates. Live instances really do hold stale pointers.
- **`sprintf_s` aborts the process on overflow.** It does not truncate. Use
  `_snprintf_s(..., _TRUNCATE, ...)`.
- **Gate it off by default.** `[scripts] enabled = false` in `rivet.toml`, same
  reasoning as `[bridge]`.
- **Readability caching.** `ddl::is_readable` caches regions and is reset once
  per frame by `ddl::reset_readable_cache()`. `VirtualQuery` costs ~0.15 ms in a
  loaded level; calling it per item is what caused the 8.3 s stall. If Lua adds
  new scan paths, keep them inside that cache's frame.

## Codebase hazards

- **Detour signature.** Component updates are
  `void *(void *components, uint32_t count, float delta)` — rcx, edx, xmm2.
  **Verified at the `middle` call site only** (`+0xf23248`); `first` and
  `async_results` are unverified. A detour typed `void *(void *)` silently
  corrupts count and delta and invalidates every measurement taken through it.
- **ASLR.** Module addresses and actor handles change every launch. Resolve from
  the component registry or by name, never from a recorded offset.
- **`rivet.toml`.** The hook rewrites it on shutdown, and until recently a
  `catch(...)` silently reset every setting to default and saved that over the
  file. The catch now logs; do not reintroduce a silent one. `LOAD_SETTING_KEY`
  had the same shape of bug — it indexed a key without checking it existed, so a
  missing one threw out of the whole load and took every setting after it with
  it. Fixed 2026-09-07.

  The earlier note here claiming the writer emits *unquoted* keys and that the
  quoted style trips the parser is **wrong**. Checked against a written file on
  2026-09-08: it emits quoted keys and sections (`["scripts"]`,
  `"enabled" = true`) and reads them straight back. Both styles are valid TOML,
  so hand-editing is safe either way.
- **The DLL cannot be replaced while the game runs.** A background watcher that
  waits on process exit and then copies is the least annoying workflow.

## Do not start with freecam

`SceneObject::transform` is a **byproduct**. Writes to it always land and are
always stamped over within a frame. `StrafeCameraMover::update_middle` is *not*
the writer — 2092 skipped calls with correct argument forwarding left the camera
completely normal. Three different attempts to find the writer failed.

The live lead is **photo mode**, which runs a parallel camera stack
(`PhotomodeLookCameraMover`, `PhotomodeCameraFreeMove`,
`PhotomodeCameraInputYawAndPitch`, orchestrated by `PhotoModeUpdaterComponent`)
and freezes *all* gameplay component dispatch at once — one global gate, not
per-system. The next instrument for that is a hardware-breakpoint write watch on
the transform address, not another guessed detour.

## Suggested first demo

Live config editing, because it plays to the part of the stack that is verified
and nothing fights the write:

```lua
local hero = rivet.find_actor("Rivet")

rivet.on_key(0x70, function()             -- F1
  rivet.set_field(hero, "AnimControllerComponent", "MaxUpdateDistance", 500.0)
  rivet.log("bumped anim update distance")
end)
```

Then reach for actor spawning — `SpawnBot` already works from the overlay's
Spawn Actor tab and takes an actor path.

## Live model swapping

Requested as a demo: swap the hero actor's model at runtime, from a script.
This is about **runtime** swapping - static replacement through `mods/` is
already solved and is not what is being asked for.

**The engine already does this, as an action component.** Verified from
`component.info` plus the type definitions in `ddl.json`:

```
ActorSwapModelActionPrius   size=40
   @8   enum  ActionType
   @16  file  Model            <- one asset reference, that is the whole payload

ModelSwapOnEventPrius       size=48
   @8   file  AlternateModel
   @32  struct ListenEvent     <- "The event that we should listen for."

WeakspotModelSwapPrius      size=200
   @136 file  DefaultModel
   @160 file  DestroyedModel
   @184 file[] IntermediateModels
```

So a swap needs exactly one asset ref. The open question is not *what data*
but *how an Action is invoked*: find the action/event dispatch and drive
`ActorSwapModelAction` against the hero actor.

**Searched for a live `ModelSwapOnEvent` instance and found none** in the
Megalopolis/Sargasso area: breakables use `BreakableComponent` + `Health`, crates
add `StackableCrate`, and the enemy `enm_blarg_bombthrower` (76 components)
carries `WeakspotFiltered` whose prius did not decode. Worth retrying in a level
with destructible armour or a boss, but do not spend long on it.

**The event mechanism is the useful part, and it is visible.** `ListenEvent`
resolves to `EventBase` (size 312):

```
@40   u32    Frame
@44   u32    SenderHandle
@48   u32    TrackedNameHash      <- events are identified by a name hash
@52   u32    EventLocatorHash
@64   struct Orientation
@288  u8     RequireActivated
```

So events are **hash keyed**. Invoking one means finding the event dispatch
function and supplying a name hash plus a sender handle - not constructing an
opaque object. The CRC32/CRC64 implementations needed to compute those hashes are
already verified against engine ground truth (see [[ref_omnitool_toolkit]] notes:
8/8 asset ids reproduced).

**The full event chain is mapped.** 2042 DDL types derive from `EventBase`, and
61 event components are registered. The relevant three:

```
SendEventActionPrius        size=40
   @8   enum   ActionType
   @16  struct Event -> EventBase        <- fill in an event and send it
   @32  bool   SendToSelfOnly
   @33  bool   RequireActivated
   @34  bool   Synced                    "True to sync to client machines."

OnReceiveEventActionPrius   size=56
   @16  bool   ResetOnListen
   @17  bool   OnlySelfEvents
   @18  bool   InitialListen
   @24  struct[] FieldFilters -> EventFilter
   @40  struct ListenEvent -> EventBase

ModelSwapOnEventPrius       size=48
   @8   file   AlternateModel
   @32  struct ListenEvent -> EventBase
```

So the supported wiring is: **`SendEventAction` sends, `ModelSwapOnEvent`
listens, and they are matched by `TrackedNameHash`.**

Note a subtlety: `Event` occupies 16 bytes inside a 40 byte prius, but
`EventBase` is 312 bytes at runtime. So a struct field in *authored* config is a
reference/descriptor, not an inline copy - do not assume prius struct layout
equals runtime struct layout. (Struct field widths vary: `TargetLocationData`
gives `Position` 48 bytes.)

### The one remaining unknown

How to **invoke an Action at runtime**. Actions are components carrying prius
config, triggered by the engine's script/action system; we have not found that
entry point. Two routes:

- **For the hero specifically**, `ActorSwapModelAction` is the direct one - it
  needs only `ActionType` + a `Model` file ref, no event round trip. But the hero
  actor does not carry that component, so it would have to be added or the action
  invoked standalone.
- **Via events**, send the event a `ModelSwapOnEvent` listens for. Requires the
  target actor to already carry that component (authored), which the hero does
  not.

Suggested attack: detour `OnReceiveEventAction`/`OnBaseEventAction` (both have
`first` updates, so they are dispatchable and detourable today) and capture their
arguments to see how an event arrives. That needs argument capture added to the
detour pool - the counters alone are not enough. Same instrument, one small
extension.

(These sizes also independently re-confirm `DDLRuntimeFile == 0x18`:
`8 + 4 + 4 + 24 = 40` and `8 + 24 + 16 = 48`, both exact.)

**Do not poke the model pointer directly.** Prius is authored config read at
component init; writing the field later does nothing because nothing re-reads
it. That is the same failure shape as the camera - the value is writable, the
system that consumes it is not ours. A `DDLRuntimeFile` write is also
`{char *value, int32 length, uint64 asset_id, pad}`, so the path string has to
outlive the write.

**Fallback if invoking an Action proves impractical: redirect at load time.** `LOAD_ASSET` is already hooked (for path
logging) and the mod loader already substitutes by path. Serving a different
asset when the engine asks for `hero_rivet.model` performs the swap through the
engine's own loading path, with no live rebinding. It applies on load rather than
instantly, which is the correct trade — the same "cooperate with the engine"
lesson photo mode taught. The harvested asset id to path pairs are exactly the
data a redirect map needs.
