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

Everything runs on the **render thread**, from the same `present` hook the
bridge pumps from. That is the only place engine state is safe to touch, and it
has two consequences worth internalising:

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
| `rivet.name(handle)` | actor name |
| `rivet.position(handle)` | `x, y, z` |
| `rivet.set_position(handle, x, y, z)` | |
| `rivet.components(handle)` | array of component type names |
| `rivet.dump(handle)` | writes `rivet_actor_<name>.json`, returns the path |

`find_actor` prefers an exact name and falls back to the first substring match,
because substring alone is not good enough: in Megalopolis `"Rivet"` matches
`test_npc_Rivet_Cine` first, which sits earlier in the array and carries no
components.

`find_actor` and `actors` answer `nil` and `{}` while the scene manager is not
up yet, because a script waits either way. Everything else raises, since a
handle that does not resolve is a script bug.

**Look an actor up once and keep the handle.** Both scans walk the whole scene,
and both stop at the frame budget and raise if they hit it. Measured in
Megalopolis: 71,680 actors at about 130 ns each, so ~6 ms to reach the hero and
~9 ms for a full miss — most of an 8 ms budget, and over it once the frame
jitters. Resolving a handle you already hold costs about 0.5 us, so re-check a
cached handle with `rivet.name` instead of scanning again. Handles do not
survive a level change and differ every launch, so they are worth re-checking,
just not by scanning.

### Prius fields

| | |
|---|---|
| `rivet.field(handle, component, field, [element])` | the value; a string or file field also returns its hash or asset id as 16-digit hex |
| `rivet.set_field(handle, component, field, value, [element])` | writes, returns the previous value |

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

### Components

| | |
|---|---|
| `rivet.detour(component, slot, on)` | skip a component update |

Slots are `first`, `first_results`, `middle`, `last`, `async`,
`async_results`. This shares the bridge's detour pool, so `component.detours`
over the bridge reports the call and skip counters for anything a script
installed.

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
