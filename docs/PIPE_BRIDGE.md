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
python tools/rivetctl.py actor.dump 0x1234
python tools/rivetctl.py mem.read 0x7ff600000000 64
python tools/rivetctl.py component.capture StrafeCameraMover first on
python tools/rivetctl.py component.captures
```

Run `python tools/rivetctl.py help` for the full command list; it comes
straight from the running hook, so it never drifts out of date.

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
python tools/rivetctl.py script.signal 2113000 SpawnerAction Start
```

See [LUA_SCRIPTING.md](LUA_SCRIPTING.md#level-scripts).

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
