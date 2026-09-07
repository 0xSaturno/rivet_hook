<!--
SPDX-FileCopyrightText: 2025-2026 Neptuwunium

SPDX-License-Identifier: EUPL-1.2
-->

# ![rivet icon](Resources/icon32.png) rivet_hook [![Build](https://github.com/neptuwunium/rivet_hook/actions/workflows/build.yml/badge.svg)](https://github.com/neptuwunium/rivet_hook/actions/workflows/build.yml)

rivet_hook is a multipurpose modding framework for luna engine, specializing on rift apart

check out the [rivet repo](https://github.com/neptuwunium/rivet) for research.

## Installation

- Download `rivet_hook.zip` from the [latest release](https://github.com/neptuwunium/rivet_hook/releases/latest), and extract.

- Copy `hid.dll` to the game installation folder.
If the game crashes, copy `hid_win10.dll` instead **_and rename it to `hid.dll`._**

- Copy `rivet_hook.dll` to the game installation folder.

- Create `mods` directory in the game installation folder.

- Run the game once, it should not crash. Exit once you get to the main menu.
This is only done so rivet_hook can save the settings file.

- Modify `rivet.toml` in the game installation folder as needed.

## Installing Mods

### Overstrike .stage files

A mod installer for this kind of archive is being considered.

- Rename the .stage file, replacing `.stage` with `.zip` and extract to a folder.

- Move the folder to the `mods` folder in the game installation folder.

- Edit `rivet.toml` in the game installation folder, adding the folder path to `paths` under `[assets]`.

For example, the default rivet.toml will have this:

```toml
[assets]
# list of paths to load assets from, order is priority. first entry is least priority.
paths = ["mods/default"]
```

If I am installing a mod that has the folder path `mods/restore weapons`, it would end up looking like this:

```toml
[assets]
# list of paths to load assets from, order is priority. first entry is least priority.
paths = ["mods/default", "mods/restore weapons"]
```

with the `info.json` file being present in `mods/restore weapons`

## Building

Building requires [meson](https://mesonbuild.com/Getting-meson.html) 

Windows: Use the Visual Studio developer command prompt.

Linux: pass `--cross-file=src/x86_64-w64-mingw32.txt` to the `meson setup` command. 
This requires mingw64 and wine to be installed.

### HID Loader Proxy

```sh
meson setup build/hid hid --buildtype release --debug
meson compile -C build/hid -j 0
```

Copy `build/hid/hid.dll` to the game installation folder.

### Hook

```sh
meson setup build/hook . --buildtype release --debug
meson compile -C build/hook -j 0
```

Copy `build/hook/rivet_hook.dll` to the game installation folder.

## Lua scripting

rivet_hook embeds Lua 5.4 and runs `scripts/*.lua` from the game directory, with
per-frame and per-key callbacks over live engine state and hot reload.
Off by default; see [docs/LUA_SCRIPTING.md](docs/LUA_SCRIPTING.md).

## Features

- lua scripting with hot reload
- asset replacement without modifying archives
- ddl and asset manager version dumping
- disable crash handler (crs-handler.exe)
- re-attach internal engine logger
- automatic renderdoc attaching

### Future

I am currently investigating if the following is possible in the engine;

- in-game actor viewer
- hot reloading of assets
- no-clip
- freecam
- prevent the game from pausing when alt tabbed

### Caveats

Due to how texture streaming is handled in Rift Apart, DirectStorage is disabled when enabling the mod loader.
Load times will be negatively impacted, and some stutters may be observed if your CPU is not particularly fast.

At the moment only Rift Apart is supported. DDL dumping is supported on all luna engine titles.

## Notice

This project is not authorized, affiliated or endorsed by Sony Interactive Entertainment LLC, or Insomniac Games.

"Ratchet and Clank", and "Ratchet and Clank: Rift Apart" are registered trademarks or trademarks of Sony Interactive Entertainment LLC.

## Attribution

This project is in part possible due to research and code written and provided by several other talented programmers.

- [macton/DDLParser](https://github.com/macton/DDLParser)
