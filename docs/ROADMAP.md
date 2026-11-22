# Roadmap

Goal: Diablo III 2.6.2 playable offline on macOS (Apple silicon), built from the
decompiled source, using data from the player's own copy. Other platforms follow.

Each milestone has one measurable exit test. A milestone is done when its test
passes on a clean checkout, not before.

| # | Milestone | Exit test | State |
|---|---|---|---|
| 0 | Foundations | Clean checkout configures, builds and reports whether game data is present | In progress |
| 1 | Compiles | Every decomp source file compiles to object code on the host | Exit test passes: 1,524 of 1,524 |
| 2 | Links | One executable links with no undefined symbols | 263 undefined (from 691) |
| 3 | Boots without a window | Startup runs to the main menu state with data loaded | Not started |
| 4 | First frame | Main menu drawn in a window | Not started |
| 5 | In game | Create a hero, enter the first area, move and fight | Not started |
| 6 | Sound | Music, effects and voice play | Not started |
| 7 | Keeps progress | Saves and settings survive a restart | Not started |
| 8 | Release quality | A full act played without a crash; packaged as an app | Not started |
| 9 | More platforms | Linux, then Windows | Not started |

## 0. Foundations

- [x] Repository layout, build system, `game/` ignored by git
- [x] Decomp located by path (`D3_DECOMP_DIR`, `extern/NSD3Decomp`, or `../NSD3Decomp`)
- [x] Compile probe (`tools/compile_probe.py`)
- [ ] Decide how the decomp is pinned: git submodule at a fixed commit, or a recorded commit hash checked at configure time
- [ ] `tools/verify_game.py`: confirm the dump in `game/` is version 2.6.2 by file hashes, and report what is missing
- [ ] Document how to produce the dump from one's own copy
- [ ] Licence and contribution rules

## 1. Compiles

Meters: `python3 tools/compile_probe.py` (syntax only, 30 seconds) and
`cmake --build build/macos --target d3_game d3_middleware -- -k 0` (real compile,
about a minute). On 2026-11-22 all 1,524 files compile. The first probe that day
accepted 1,296.

- [x] Build targets in CMake for the game and every library, with per-library settings taken from the decomp's build (`cmake/DecompSources.cmake`)
- [x] 64-bit integer types made the same types as on the Switch (`src/compat/d3_host_prefix.h`); this cleared nearly every game-code error
- [x] macOS libc differences: fortified `memcpy` macros, byte-order macros, headers the source relied on indirectly
- [x] Stand-ins for two decomp headers tied to the Switch's C++ library and libc (`src/compat/`)
- [ ] Confirm the decomp's structure size assertions are active in this build (nothing fails, but the number checked has not been counted)
- [ ] Warnings are all switched off today. Turn them on and list anything that signals undefined behaviour on the host

## 2. Links

Meter: `python3 tools/link_probe.py`. 691 undefined symbols on the first attempt
(2026-11-22), 263 after stand-ins for the SDK and FMOD. None defined twice.

| Still missing | Count | What it needs |
|---|---:|---|
| Data that exists in the original but is not defined in the decomp | 238 | See below. About 224 are the game's (name tables, default lists, constants); the rest belong to middleware (PCRE tables, LibTomCrypt descriptors) |
| Functions | 14 | 9 in the game, 5 in middleware: write or stand in for each |
| Variables the decomp declares but never defines (static class members) | 9 | Define them |
| `main`, `stdin` | 2 | Join the host program to the game; one libc name curl expects |

- [x] Stand-ins for every SDK function the game calls: 350 across 28 SDK areas (`src/nn/<area>/unimplemented.c`). Each stops the program and prints its own name
- [x] Stand-ins for the 74 FMOD functions (`src/audio/`) and 3 graphics-driver entry points (`src/gfx/`)
- [x] `tools/gen_stubs.py` adds stand-ins from the link probe's results and never re-adds a symbol once it is defined
- [x] The four mutex functions the physics library needs, implemented for real (`src/libc/nnmusl_mutex.c`)
- [ ] The 238 data definitions. They are game content, so they cannot be stored here: write them in the decomp, or generate them at build time from the player's own copy of the program
- [ ] The 14 functions and 9 variables, one by one
- [ ] `main`: build one `d3` executable from the game, middleware and platform layer
- [ ] The 8 functions the decomp has not written: write or stand in for each

## 3. Boots without a window

- [ ] Call the game's startup (`nnMain`) from `src/host/main.cpp`
- [ ] `nn::os`: threads, locks, events, timers, memory
- [ ] `nn::fs`: mount `game/romfs` as the game's data volume; save volume as a folder
- [ ] `nn::account`, `nn::oe`, `nn::settings`, `nn::time`: minimal real behaviour
- [ ] Network, wireless, amiibo, friends: report unavailable cleanly
- [ ] Renderer and audio that accept every call and do nothing
- [ ] Meter: how far startup gets, from the game's own log

## 4. First frame

- [ ] **Investigate shaders first** (see architecture): what the data holds, and choose translate or rewrite
- [ ] Choose backend approach A or B by counting required functions
- [ ] Window, swap chain, textures, vertex and index buffers, render states
- [ ] UI rendering and fonts
- [ ] Controller and keyboard input reach the menu

## 5. In game

- [ ] World, actor and effect rendering
- [ ] Physics (Domino) and cloth behave as on the Switch
- [ ] Local server and client loop stable through the first quest
- [ ] A way to compare behaviour with the original: recorded inputs or logged game state

## 6. Sound

- [ ] Choose the audio route (see open decisions)
- [ ] Sound banks load from the dump; streamed music and voice

## 7. Keeps progress

- [ ] Save and load heroes and account data
- [ ] Settings persist; choose where they live per platform
- [ ] Decide whether Switch save files can be imported

## 8. Release quality

- [ ] Resolution and frame rate options beyond the Switch's
- [ ] Local co-op with several controllers
- [ ] Crash reporting to a local file
- [ ] macOS app bundle; first-run screen that asks for the game folder
- [ ] Automated build and tests

## 9. More platforms

- [ ] Linux on ARM64 and x86-64
- [ ] Windows x86-64
- [ ] Remove assumptions found to be specific to ARM or macOS

## Not goals

- Online play through Battle.net, seasons, or anything needing Blizzard's or Nintendo's servers
- Local wireless play and amiibo (they report unavailable)
- Distributing game data, the original program, or anything from the Nintendo SDK
- Versions of the game other than 2.6.2, for now

## Open decisions

| Decision | Needed by | Leaning |
|---|---|---|
| How the decomp is pinned | Milestone 0 | Submodule at a fixed commit |
| Where the 238 missing data definitions come from | Milestone 2 | Write them in the decomp where the debug info gives their type; extract the rest from the player's copy |
| Audio: FMOD's own macOS library (the player downloads it) or a replacement for the 74 functions used | Milestone 6 | Undecided |
| Graphics backend approach | Milestone 4 | New backend (A) |
| Graphics API on macOS | Milestone 4 | Metal directly, or a portable layer to ease milestone 9 |
| Shaders: translate or rewrite | Milestone 4 | Unknown until the data is inspected |
| Window and controller library | Milestone 4 | SDL (also covers Linux and Windows) |
| Mouse and keyboard play | Milestone 8 | The Switch version is controller-only; later |
