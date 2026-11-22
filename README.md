# NSD3Recomp

A native port of **Diablo III 2.6.2 (Nintendo Switch)**, built by recompiling the
decompiled source from [NSD3Decomp](https://github.com/loremipsum1077/NSD3Decomp)
for desktop platforms. macOS on Apple silicon is the first target.

**Status: the game does not run yet.** The decomp compiles and links into one
macOS executable. It starts, loads its data, and runs its main loop with no
window, no sound and no controller: nothing is drawn yet. See the [roadmap](docs/ROADMAP.md).

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

Requires CMake 3.24+, Ninja and the Xcode command line tools.

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
