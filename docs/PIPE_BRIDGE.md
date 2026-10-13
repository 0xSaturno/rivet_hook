<!--
SPDX-FileCopyrightText: 2025-2026 Neptuwunium

SPDX-License-Identifier: EUPL-1.2
-->

# Pipe bridge

`rivet_hook` can expose a Windows named pipe so an external process can query
and poke the running game without attaching a debugger. Off by default, since
it is an RPC surface inside the game process. In `rivet.toml`:

```toml
[bridge]
enabled = true
pipe_name = "rivet_hook"
```

The pipe is reachable at `\\.\pipe\<pipe_name>`. `tools/rivetctl.py` is a
client for it:

```bash
python tools/rivetctl.py ping
python tools/rivetctl.py scene.actors Rivet
python tools/rivetctl.py actor.hero
python tools/rivetctl.py actor.uid 9a2452401153515b
python tools/rivetctl.py scene.find_component HeroSkinManager
python tools/rivetctl.py scene.near -2908 3052 2060 200
python tools/rivetctl.py actor.dump 0x1234
python tools/rivetctl.py mem.read 0x7ff600000000 64
python tools/rivetctl.py component.capture StrafeCameraMover first on
python tools/rivetctl.py component.captures
```

Run `python tools/rivetctl.py help` for the full command list; it comes
straight from the running hook, so it never drifts out of date.

`scene.near <x> <y> <z> <radius> [limit]` lists the actors around a point,
nearest first, with their actor asset path and component classes: what a place
is made of.

`mem.watch` answers "what changes this?" with a hardware breakpoint in debug
register 0 of every thread alive when it is armed. `mem.watch <address> [length]`
records each instruction that writes the address (reported as the instruction
after the write, since a data breakpoint traps after it); `mem.watch <address> exec`
breaks when the instruction at the address runs and keeps the registers of the
last 16 hits. A leading `+` makes the address relative to the game module, so an
RVA from a disassembler works as is. `mem.watches` reads the results and
`mem.watch off` clears it. Threads created after arming are not covered.

```bash
python tools/rivetctl.py mem.watch +0x6794fa0 4
python tools/rivetctl.py mem.watches
python tools/rivetctl.py mem.watch off
```

## Events

`event.*` is the engine's event queue, the same one `rivet.queue_event` and
`rivet.on_event` use (see [LUA_SCRIPTING.md](LUA_SCRIPTING.md#events)):

```bash
python tools/rivetctl.py event.status
python tools/rivetctl.py event.classes Warp
python tools/rivetctl.py event.info PerformWarpEvent
python tools/rivetctl.py event.tail 50
python tools/rivetctl.py event.tail Damage 20
python tools/rivetctl.py event.watch PerformWarpEvent on
python tools/rivetctl.py event.captures
python tools/rivetctl.py event.send PerformWarpEvent '{"target": "hero", "fields": {"ResetCamera": true}}'
```

`event.tail` answers from the last 1024 events the hook saw, without their
fields. `event.watch <class> on` also keeps the decoded fields of every event of
that class, or a derived one, for `event.captures` to return (the last 128).

`event.send` is the other command whose argument is not split on spaces: after
the class name the rest of the line is a JSON object with the same keys as the
Lua options table. A handle may be written as a number, as text in any base, or
as `"hero"`.

## Time and camera

```bash
python tools/rivetctl.py time.status
python tools/rivetctl.py time.scale 0.5
python tools/rivetctl.py time.scale 0.25 Game 5
python tools/rivetctl.py time.clear
python tools/rivetctl.py camera.fov
python tools/rivetctl.py camera.fov 1.6
python tools/rivetctl.py camera.get
python tools/rivetctl.py camera.detach
python tools/rivetctl.py camera.set 10 25 -40 90 -20
python tools/rivetctl.py camera.attach
python tools/rivetctl.py camera.shake off
```

`camera.set <x> <y> <z> [yaw] [pitch] [fov]` only works while detached.
`camera.shake off` blocks camera shake, `on` lets it through again.

`time.scale <scale> [channel] [ramp]` and `time.clear [channel]` are
`rivet.time_scale` and `rivet.clear_time_scale`; `time.status` lists the
applied scale and every channel asking for something other than 1.
`camera.fov [scale]` reads or sets the field of view multiplier. See
[LUA_SCRIPTING.md](LUA_SCRIPTING.md#time-and-camera).

## HUD messages

```bash
python tools/rivetctl.py hud.notify Hello from the bridge
python tools/rivetctl.py hud.message center 4 Slow motion on
```

The text is the rest of the line, spaces included. See
[LUA_SCRIPTING.md](LUA_SCRIPTING.md#hud-messages) for the types.

## Outfits

```bash
python tools/rivetctl.py vanity.owns configs/hero/vanity/vanitybundles/carbonox_armor/hero_vanity_bundle_torso_carbonox_armor.config
python tools/rivetctl.py vanity.equip configs/hero/vanity/vanitybundles/carbonox_armor/hero_vanity_bundle_torso_carbonox_armor.config
```

Both act on the hero. See [LUA_SCRIPTING.md](LUA_SCRIPTING.md#outfits).

## Hero model

```bash
python tools/rivetctl.py hero.look characters/hero/hero_Kit/hero_kit.actor
python tools/rivetctl.py hero.look characters/hero/hero_Kit/hero_kit.actor anims
python tools/rivetctl.py hero.look characters/mymod/my_rivet.model
python tools/rivetctl.py hero.look
python tools/rivetctl.py hero.restore
python tools/rivetctl.py hero.models [filter]
python tools/rivetctl.py hero.play_as kit
python tools/rivetctl.py hero.apply_on_launch on
```

`hero.look <.actor or .model> [anims]` is `rivet.hero_look`, `anims` putting on the asset's anim sets too; with no path it reports what is
worn and what is still loading. `hero.restore` puts the hero's own look back. `hero.models [filter]` lists the
`.model` files the mod paths provide and the game's own whole bodies.
`hero.play_as <ratchet|clank|rivet|kit>` is `rivet.hero_play_as`, and
`hero.apply_on_launch <on|off>` switches putting the remembered look back on
after a launch.
See [LUA_SCRIPTING.md](LUA_SCRIPTING.md#hero-model).

## Configs

```bash
python tools/rivetctl.py config.list Hero
python tools/rivetctl.py config.get 85ead7ccfb74ae51
python tools/rivetctl.py config.set 85ead7ccfb74ae51 SomeField 1.5
```

`config.list [type] [limit]`, `config.get <config>` and
`config.set <config> <field.path> <value>` are `rivet.configs`, `rivet.config`
and `rivet.config_set`. See [LUA_SCRIPTING.md](LUA_SCRIPTING.md#configs).

## Level scripts

```bash
python tools/rivetctl.py script.nodes Spawner
python tools/rivetctl.py script.node 3148523
python tools/rivetctl.py script.signal 3148523 Start
python tools/rivetctl.py script.signal 2113000 SpawnerAction Start
```

`script.nodes [filter] [limit]` lists nodes with their `component` handle,
`script.node <component>` is `rivet.script_node`, and `script.signal` takes
either a component and a plug or an actor, a class and a plug. See
[LUA_SCRIPTING.md](LUA_SCRIPTING.md#level-scripts).

## Travel

```bash
python tools/rivetctl.py level.checkpoints LANDING
python tools/rivetctl.py level.warp CHK_SAV_01_LANDING
python tools/rivetctl.py level.fly CHK_BLIZAR_A_LANDINGPAD
python tools/rivetctl.py level.fly CHK_ZURK_LANDING_SHIP SAVALI
python tools/rivetctl.py level.rift CHK_SAV_01_LANDING
python tools/rivetctl.py level.rift -150 390 4
```

`level.fly <destination> [via]` is `rivet.fly`: the ship's travel, takeoff,
planet tunnel and landing, to any checkpoint. It needs a ship loaded nearby.
`level.rift <checkpoint | x y z>` is `rivet.rift`: the game's rift, through
the airlock to a checkpoint on any planet, or a position on the hero's.

```bash
python tools/rivetctl.py level.zones savali/tile_a21
python tools/rivetctl.py level.go savali/Tile_A21/Tile_A21_gp.zone
python tools/rivetctl.py level.overlay SARG_PocketDim_01
```

`level.checkpoints [filter] [limit]` and `level.warp <checkpoint name|0xhash>`
are `rivet.checkpoints` and `rivet.warp`; `level.zones <filter> [limit]`,
`level.go <zone>` and `level.overlay <region>` are `rivet.zones`, `rivet.go` and
`rivet.overlay`, with `level.unload <region>` for `rivet.overlay(region, false)`.
`level.regions [filter] [limit]` lists regions with their type and area, and
for overlays whether they are `loaded` and whether the `story` drives them. The
`go`, `overlay` and `unload` targets are the rest of the line, since some level
paths hold spaces. See [LUA_SCRIPTING.md](LUA_SCRIPTING.md#travel).

## Spawned actors

```bash
python tools/rivetctl.py actor.spawn environment/template/template_cube/template_cube.actor -150 390 4
python tools/rivetctl.py actor.spawn environment/template/template_cube/template_cube.actor -150 390 4 45 4 0.5 4
python tools/rivetctl.py actor.spawned
python tools/rivetctl.py actor.destroy 2113000
python tools/rivetctl.py actor.destroy all
```

`actor.spawn <actor asset path> <x> <y> <z> [yaw] [sx sy sz]` is `rivet.spawn`:
an actor from any `.actor`, at a position, turned `yaw` degrees and scaled per
axis, activated. The path is one argument, so a path with a space in it has to
go through `script.exec` and `rivet.spawn`. The command takes 4, 5 or 8
arguments: a scale needs the yaw before it. Every number has to parse whole,
so `-150,5` is refused. It answers with the actor's `handle`, `uid`, `state`
and `flags`. When the asset is not loaded yet the answer has
`"state": "loading"`, the `uid`, and `null` for `handle` and `flags`, and the
hook spawns the actor itself once the asset is in: sending the command again
would ask for a second actor. `actor.spawned` or `actor.uid <uid>` has the
handle from then on. `actor.spawned` is `rivet.spawned`, with the reason the
last waiting spawn was dropped as `last_error`, and
`actor.destroy <handle|all>` is `rivet.destroy`, which only takes handles
`actor.spawn` gave out and answers with `destroyed`: for `all`, how many
actors it asked the engine to destroy or stopped waiting for. See
[LUA_SCRIPTING.md](LUA_SCRIPTING.md#spawned-actors).

## Mesh collision (experimental)

**This is experimental and can crash the game.** The game ships its collision
cooked, and the hook cannot write that format. Instead it hands triangles to
the Havok mesh builder that is linked into the exe, and gets a static body for
the result through the queue the engine's own zones load their physics with.
The builder has no caller in the shipped game, so the first time it runs is
when the hook runs it. Use a save you have a copy of, try things above ground
the game already has, and remove every test body before travelling anywhere.

It is off unless `rivet.toml` says otherwise, and comes on a stage at a time:

```toml
[collision]
stage = 0
max_triangles = 2048
```

| stage | adds | what can go wrong |
|---|---|---|
| 0 | nothing. The exe is not even scanned | |
| 1 | `collision.status`, `collision.inspect`: reads only | nothing, no engine call is made |
| 2 | `collision.selftest`: three constructors on hook buffers, one heap block taken and given back | a wrong address or prototype |
| 3 | `collision.borrow`, `collision.bodies`, `collision.enable`, `collision.disable`, `collision.remove`: a second body out of a loaded zone's own shape | a wrong body description, read by the engine a frame later |
| 4 | `collision.cook`, `collision.shapes`, `collision.release`: the mesh cook, no body | the builder itself |
| 5 | `collision.body`: a cooked shape as a body | a shape the engine does not like |

`collision.stage [0-5]` reads the stage or sets it for the session, without a
restart; the file keeps what it has. The first time the stage leaves 0 the hook
scans the exe for the engine calls, once, which can be felt as a short hitch.
A command above the stage is refused with the stage it needs, and the stage
does not go below 3 while a body is still there, since nothing would follow it
any more. Every engine call is made on the game thread only, after checking
that the thread has a Havok memory router and that there is a physics world,
and a call that queues something also that physics is switched on and the
queue has room. Each has a line in `rivet.log` written before it, so a crash
names its step. A fault in an engine call is caught and switches the whole
feature off until the game restarts, since nothing is known about the state
the engine was left in: from then on only `collision.stage` and
`collision.status` answer. Catching a fault takes the MSVC build of the hook,
so any other build stops at stage 1 (`faults_caught` in `collision.stage`).
The layouts were measured on one build of the exe; on any other only stage 1
works.

```bash
python tools/rivetctl.py collision.stage 1
python tools/rivetctl.py collision.status
python tools/rivetctl.py collision.inspect navmesh_test_overlay
python tools/rivetctl.py collision.stage 2
python tools/rivetctl.py collision.selftest ctor
python tools/rivetctl.py collision.selftest heap
python tools/rivetctl.py collision.stage 3
python tools/rivetctl.py collision.borrow navmesh_test_overlay 0 30 0
python tools/rivetctl.py collision.bodies
python tools/rivetctl.py collision.disable 1
python tools/rivetctl.py collision.enable 1
python tools/rivetctl.py collision.remove 1
python tools/rivetctl.py collision.stage 4
python tools/rivetctl.py collision.cook quad:4 0x6BF
python tools/rivetctl.py collision.cook mods/mymap/floor.obj
python tools/rivetctl.py collision.shapes
python tools/rivetctl.py collision.stage 5
python tools/rivetctl.py collision.body 2 1204 121 -840
python tools/rivetctl.py collision.remove all
python tools/rivetctl.py collision.release all
```

`collision.status` is what was found and what the engine has right now: how
often each signature matched, the cross checks between them, the class names
behind the vtables, the Havok memory router of the thread that answered, the
world, whether physics is on and paused, the two queues and the last error.
`collision.inspect <zone>` walks a loaded zone's physics: the zone slot, the
system and system data, materials, body descriptions, the bodies in the world
and the shape tree with types, user data, boxes and triangle counts. A mesh's
data runs come split into `tag`, `first` and `count` next to the `raw` value,
the split being a reading of the shipped files. The zone is named as for
`level.go`. Both only read.

`collision.selftest [ctor|heap]` answers with the bytes each constructor left
and how they differ from what it is known to write; with no argument it runs
both, and skips the heap when a constructor failed.

`collision.borrow <zone> <dx> <dy> <dz>` queues a second static body made of
that zone's root shape, offset from the zone's own. A zone placed by a matrix
(`placed_by_matrix` in `collision.inspect`) is refused, its own body being
somewhere else than its description was written for. The shape still belongs
to the zone, so `level.unload` is refused while such a body exists, and a body
whose zone goes anyway is taken out of the world and listed as `orphaned`.
`level.warp`, `level.go` by a checkpoint, `level.fly` and `level.rift` are
refused while any body made here exists, borrowed or cooked: none has been
through a planet change yet. The game's own travel is not held back by any of
this, so remove the bodies before using it too.

`collision.bodies` lists the bodies with their `state`: `queued` until the
engine has made the body, `created` until a physics step has put it in the
world (a paused game stops there), then `live`; `disabling` and `disabled`
after `collision.disable <body>`, which `collision.enable <body>` undoes;
`removing` after `collision.remove <body|all>`. A removal takes the body out
of the world first, the way a zone's physics is switched off, and destroys it
once the engine has done that, so the body stays listed for a few frames.
Three more states are bodies the hook leaves alone, each with the reason as
its `error`: `orphaned`; `broken`, when what the engine made of the
description is not the one body it should be; and `lost`, when the physics
world is another than the one the body was made in, or the body's system is
not the one the engine made for it. The world is looked at on every pump, and
when it has changed every body and every cooked shape is dropped as `lost`
with no call made on it. An `orphaned` or `broken` body cannot be removed any
more: the commands it holds back say to restart the game.

`collision.cook <mesh> [flags]` cooks a mesh and makes no body. `mesh` is
`quad:<size>`, a flat square of that many metres around the origin, or a
Wavefront `.obj`, relative to the game's folder unless the path is absolute;
the path is one argument, so one with a space in it has to go through
`script.exec`. Of the file only `v x y z` and `f a b c` lines are read, with
1 based or negative indices and anything after a `/` ignored; faces have to be
triangles, wound like the game's own, so that `(b - a) x (c - a)` points out
of the walkable side. A mesh is refused when a coordinate is not finite or
more than 4096 from the origin, when a triangle has two corners within a
centimetre of each other or all three in a line, or when it has more than
`max_triangles` triangles (which can be raised to 65536): the cook runs inside
a frame, and a large mesh is a visible hitch. `flags` are the gameplay flags
every triangle gets, `0x43F` unless given, `0x7FF` at most and without
`0x100`; they are decimal or `0x` hex. The vertices and triangles reach the
builder in the order the mesh has them, less the vertices no triangle uses.
The answer has the `checks` the new shape passed or failed and what was
`measured` on it as the builder left it, in the same form `collision.inspect`
uses, so the two can be compared; `written` is the user data and tag codec the
hook then wrote into a shape that passed. A shape that failed a check is
`rejected` and no body is made of it. `collision.shapes` lists the cooked
shapes with their reference counts, and `collision.release <shape|all>` frees
one no body uses.

`collision.body <shape> <x> <y> <z> [yaw]` queues a static body made of a
cooked shape at a position, turned `yaw` degrees about the up axis. It is
listed, switched and removed like a borrowed one. What the engine reads of a
body is kept until the game exits, so a session makes at most 256 bodies and
256 shapes. See [LUA_SCRIPTING.md](LUA_SCRIPTING.md#mesh-collision-experimental).

## Wire format

One connection, one client at a time. Each message, request or response, is a
4-byte little-endian length prefix followed by that many bytes of UTF-8:

```
[ uint32 length ][ length bytes of payload ]
```

A request payload is one command line, e.g. `mem.read 0x1234 64`. Arguments
split on single spaces with no quoting, so an argument cannot itself contain a
space (`script.exec`, `event.send` and the `hud.*` text are the exceptions).

A response payload is JSON:

```json
{ "ok": true, "result": { ... } }
{ "ok": false, "error": "message" }
```

`rivetctl.py` retries a few times before giving up: the server serves one
client at a time and rebuilds the pipe between connections, so a connection
attempt landing in that gap is normal, not a failure.

## Threading

The pipe runs on its own thread, entirely separate from the engine's frame
loop. A request that needs engine state (`scene.actors`, `mem.read`, anything
touching an actor or component) is hoisted onto the game thread, between actor
update passes (or onto the render thread while actor updates are stopped, see
`LUA_SCRIPTING.md`): the pipe thread stashes the request, wakes the pump, and
blocks on an event the pump signals once the response is ready. This is why an unresponsive game (not
presenting frames) times out a request after 10 seconds instead of hanging
the pipe thread forever - the answer to "is the game alive" has to come from
the game.

A handful of commands (`ping`, `help`, `log.tail`, `stats`) are answered
directly on the pipe thread without touching the render thread at all, since
they don't need engine state.

## `script.exec`

`script.exec` is the one command whose argument is not split on spaces: the
rest of the line after the command word is passed to Lua as-is, so a full
chunk with its own spacing survives:

```bash
python tools/rivetctl.py script.exec "return rivet.name(rivet.find_actor('Rivet'))"
```

The result is whatever the chunk returns, JSON-encoded. See
[LUA_SCRIPTING.md](LUA_SCRIPTING.md) for what the chunk can call.

## Writing another client

Any language that can open a Windows named pipe and do length-prefixed framing
works. The request is plain text, the response is JSON, and `rivetctl.py`'s
`call()` is about 15 lines - reading it is faster than re-deriving the framing
from this doc.
