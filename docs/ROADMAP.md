# Roadmap

Goal: Diablo III 2.6.2 playable offline on macOS (Apple silicon), built from the
decompiled source, using data from the player's own copy. Other platforms follow.

Each milestone has one measurable exit test. A milestone is done when its test
passes on a clean checkout, not before.

| # | Milestone | Exit test | State |
|---|---|---|---|
| 0 | Foundations | Clean checkout configures, builds and reports whether game data is present | In progress |
| 1 | Compiles | Every decomp source file passes the host compiler | 1,296 of 1,526 |
| 2 | Links | One executable links with no undefined symbols | Not started |
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

Meter: `python3 tools/compile_probe.py`. Baseline on 2026-11-22: 1,296 of 1,526
files, with game code at 840 of 862.

- [ ] Per-library include paths and configuration for middleware (195 of the 230 failures are a header not found, almost all in `Contrib`)
- [ ] PCRE configuration (8 files)
- [ ] The 22 game files that fail: stricter modern-compiler rules (reference binding, ambiguous calls, conflicting declarations)
- [ ] Real build targets in CMake for every library and the game, replacing the probe's syntax check
- [ ] The decomp's structure size assertions all pass on the host
- [ ] Warnings triaged: anything that signals undefined behaviour on the host is listed

## 2. Links

- [ ] Generate the list of undefined symbols; group by SDK area
- [ ] `src/nn/`: every SDK function the game calls exists. Unimplemented ones stop the program and print their own name
- [ ] Graphics entry points likewise present and stopping
- [ ] The 8 functions the decomp has not written: write or stand in for each
- [ ] Meter: undefined symbol count, to zero

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

- [ ] FMOD output driver for the host audio device
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
| Graphics backend approach | Milestone 4 | New backend (A) |
| Graphics API on macOS | Milestone 4 | Metal directly, or a portable layer to ease milestone 9 |
| Shaders: translate or rewrite | Milestone 4 | Unknown until the data is inspected |
| Window and controller library | Milestone 4 | SDL (also covers Linux and Windows) |
| Mouse and keyboard play | Milestone 8 | The Switch version is controller-only; later |
