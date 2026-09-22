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
python tools/rivetctl.py actor.dump 0x1234
python tools/rivetctl.py mem.read 0x7ff600000000 64
python tools/rivetctl.py component.capture StrafeCameraMover first on
python tools/rivetctl.py component.captures
```

Run `python tools/rivetctl.py help` for the full command list; it comes
straight from the running hook, so it never drifts out of date.

## Wire format

One connection, one client at a time. Each message, request or response, is a
4-byte little-endian length prefix followed by that many bytes of UTF-8:

```
[ uint32 length ][ length bytes of payload ]
```

A request payload is one command line, e.g. `mem.read 0x1234 64`. Arguments
split on single spaces with no quoting, so an argument cannot itself contain a
space (`script.exec` is the exception - see below).

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
touching an actor or component) is hoisted onto the render thread: the pipe
thread stashes the request, wakes the pump, and blocks on an event the pump
signals once the response is ready. This is why an unresponsive game (not
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
