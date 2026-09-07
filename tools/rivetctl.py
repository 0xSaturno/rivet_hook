# SPDX-FileCopyrightText: 2025-2026 Neptuwunium
#
# SPDX-License-Identifier: EUPL-1.2

"""Talk to a running rivet_hook over its named pipe.

Requires [bridge] enabled = true in rivet.toml and the game to be running.

    python tools/rivetctl.py ping
    python tools/rivetctl.py scene.actors Rivet
    python tools/rivetctl.py actor.dump 0x1234
    python tools/rivetctl.py mem.read 0x7ff600000000 64
"""

import json
import struct
import sys
import time

PIPE = r"\\.\pipe\rivet_hook"
HEADER = struct.Struct("<I")


def call(request, pipe_name=PIPE, retries=3):
    """Send one request, return the decoded response."""
    last = None
    for attempt in range(retries):
        try:
            with open(pipe_name, "r+b", buffering=0) as pipe:
                pipe.write(HEADER.pack(len(request)) + request.encode("utf-8"))
                (length,) = HEADER.unpack(_read_exactly(pipe, HEADER.size))
                return json.loads(_read_exactly(pipe, length).decode("utf-8", "replace"))
        except OSError as error:
            # the server serves one client at a time and rebuilds the pipe
            # between connections, so a busy moment is normal
            last = error
            time.sleep(0.2 * (attempt + 1))

    raise SystemExit(f"could not reach {pipe_name}: {last}\n"
                     "is the game running with [bridge] enabled = true?")


def _read_exactly(pipe, count):
    chunks = []
    remaining = count
    while remaining:
        chunk = pipe.read(remaining)
        if not chunk:
            raise OSError("pipe closed mid-message")
        chunks.append(chunk)
        remaining -= len(chunk)

    return b"".join(chunks)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)

    response = call(" ".join(sys.argv[1:]))
    if not response.get("ok"):
        print(f"error: {response.get('error')}", file=sys.stderr)
        raise SystemExit(1)

    print(json.dumps(response.get("result"), indent=2))


if __name__ == "__main__":
    main()
