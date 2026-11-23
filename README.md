# NSD3Recomp

A native port of **Diablo III 2.6.2 (Nintendo Switch)**, built by recompiling the
decompiled source from [NSD3Decomp](https://github.com/loremipsum1077/NSD3Decomp)
for desktop platforms. macOS on Apple silicon is the first target.

**Status: the game does not run yet.** The decomp compiles and links into one
macOS executable. It starts, loads its data, and draws the game's Hero Creation
menu and the game world in a window through Metal, with keyboard and controller
input and with sound. Parts of the picture are still missing and play beyond
walking and attacking in the first area has not been tried. See the [roadmap](docs/ROADMAP.md).

## What is and is not in this repository

In: the build system, the platform layer that replaces the Switch SDK (system
services, graphics, audio, input), compatibility headers, and tools.

Not in, ever: game data, original executables, keys, or Nintendo SDK files. You
supply data from your own copy of the game in `game/`, which git ignores. The
decompiled game source also stays in its own repository and is referenced by path.

## Layout

| Path | Purpose |
|---|---|
| `src/host/` | Program entry, window and main loop |
| `src/compat/` | Headers that let the decomp compile with a host compiler |
| `src/` (more) | Platform layers, added per milestone: see [architecture](docs/ARCHITECTURE.md) |
| `cmake/` | Build helpers, including how the decomp is located |
| `tools/` | Progress meters and data checks |
| `docs/` | Roadmap, architecture |
| `game/` | **Ignored by git.** Your game dump |
| `build/` | **Ignored by git.** Build output |

## Setup

1. Check out NSD3Decomp next to this repository (`../NSD3Decomp`), or point
   `D3_DECOMP_DIR` at it.
2. Put your own Diablo III 2.6.2 dump in `game/`: the program in `game/exefs/`
   (`main`) and the RomFS contents in `game/romfs/` (including
   `DiabloIIINX64r.nss`). The build reads both.
3. Build:

```sh
cmake --preset macos
cmake --build build/macos
./build/macos/d3
```

Requires CMake 3.24+, Ninja, the Xcode command line tools, SDL2 and LLVM
(`brew install sdl2 llvm`; LLVM supplies the static C++ library the program links).

The first run draws nothing: the game's shaders have to be translated once.
Run the program for half a minute, quit, then:

```sh
python3 tools/shader_translate.py --cache cache/shaders
```

and run it again. Repeat when a new area shows missing objects.

## Controls

A game controller maps by position: the right-hand face button is the Switch's A.

| Keyboard | Switch |
|---|---|
| W A S D | Left stick |
| I J K L | Right stick |
| Arrow keys | D-pad |
| Space or Return | A |
| Escape | B |
| E / Q | X / Y |
| 1 / 2 / 3 / 4 | L / R / ZL / ZR |
| Tab / Backspace | Plus / Minus |

The same table is on the Mods page (`Mods.command`).

## Useful switches

Environment variables: `D3_HEADLESS=1` (no window), `D3_LANGUAGE=fr` (a Switch
language code), `D3_PLAYER_NAME=...`, `D3_NVN_TRACE=frames` or `all`,
`D3_INPUT_TRACE=1`, `D3_INPUT_SCRIPT="5:A 8:PLUS"` (press buttons at given seconds),
`D3_SCREENSHOT=file.png` with `D3_SCREENSHOT_FRAME=2500`, `D3_METAL_STATS=1`,
`D3_FS_TRACE=1`, `D3_TEXTURE_LIST=1`, `D3_DRAW_LOG=<frame>` and `D3_DRAW_SWEEP=<frame>,<n>`
(list a frame's draws; leave out a different slice of them each frame, to find the
one that draws wrongly), `D3_SOUND_STATS=1` (what was decoded and how loud the output is),
`D3_SOUND_DUMP=<directory>` (first decoded samples as WAV), `D3_NO_SOUND=1`.

## Mods

A deliberate change to the game is made here, never in the decomp, which has to stay
identical to the original. A mod is a build option that compiles a patched copy of
one decomp source file in place of the original (`cmake/DecompSources.cmake`,
`tools/patch_source.py`). All mods are off by default.

The easy way to choose them: double-click `Mods.command` (or run
`python3 tools/mods.py`). It opens a page in your browser, served from this computer
only, with a checkbox for each mod, a Build button and a Start game button. The mods
it offers are listed in `config/mods.json`.

- `-DD3_MOD_RENDER_SIZE=2560x1440`: draw the game at that size instead of the
  Switch's docked 1600x900. The game's memory budget for screen-sized buffers is
  raised to match. Turn it off with `-DD3_MOD_RENDER_SIZE=`.
- `-DD3_MOD_CAMERA_DISTANCE=1.5`: move the in-game camera to that multiple of its
  normal distance from the hero. Turn it off with `-DD3_MOD_CAMERA_DISTANCE=`.
- `-DD3_MOD_SEASON=1`: make that season number active without the online service,
  so the Season Journey and seasonal heroes are available. Leaderboards and the other
  online parts of seasons stay unavailable. Turn it off with `-DD3_MOD_SEASON=`.
- `-DD3_MOD_RUN_SPEED=2.0`: heroes walk, run and sprint at that multiple of the
  game's speed. Turn it off again with `-DD3_MOD_RUN_SPEED=`.

## Measuring progress

```sh
python3 tools/compile_probe.py
```

reports how many of the decomp's source files the host compiler accepts, and the
most common reasons for the rest. To compile everything and count what is still
missing at link time:

```sh
cmake --build build/macos --target d3_decomp -- -k 0
python3 tools/link_probe.py
```
