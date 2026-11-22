# Roadmap

Goal: Diablo III 2.6.2 playable offline on macOS (Apple silicon), built from the
decompiled source, using data from the player's own copy. Other platforms follow.

Each milestone has one measurable exit test. A milestone is done when its test
passes on a clean checkout, not before.

| # | Milestone | Exit test | State |
|---|---|---|---|
| 0 | Foundations | Clean checkout configures, builds and reports whether game data is present | In progress |
| 1 | Compiles | Every decomp source file compiles to object code on the host | Exit test passes: 1,524 of 1,524 |
| 2 | Links | One executable links with no undefined symbols | Exit test passes |
| 3 | Boots without a window | Startup runs to the main menu state with data loaded | Runs its main loop and plays the opening videos; menu not yet confirmed |
| 4 | First frame | Main menu drawn in a window | Window and input work; nothing is drawn yet |
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
`cmake --build build/macos --target d3_decomp -- -k 0` (real compile,
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
(2026-11-22), none now: `build/macos/d3` links (22.8 MB). Run, it stops inside the
game's static initialisers at the first SDK function that is still a stand-in.

How the 691 were resolved:

| What | Count | Where |
|---|---:|---|
| Switch SDK functions: stand-ins that stop and print their name | 350 | `src/nn/<area>/unimplemented.c` |
| FMOD functions and graphics-driver entry points: stand-ins | 77 | `src/audio/`, `src/gfx/` |
| Data the decomp does not define: generated from the player's dump | 286 | `tools/extract_data.py`, list in `config/extract_symbols.txt` |
| Functions our build calls that no source defines: stand-ins | 23 | `src/replace/unimplemented.c` |
| Real implementations | 13 | Physics mutex (4), `__errno_location`, `stdin`, protobuf's field skippers (7) |

- [x] `tools/gen_stubs.py` adds stand-ins from the link probe's results and never re-adds a symbol once it is defined
- [x] `tools/extract_data.py` writes typed-less C definitions (bytes, with pointers turned into string literals or symbol references) into the build directory; verified byte-for-byte against the decomp's copy of the program
- [x] One `d3` executable from the game, middleware, platform layer, gap-fillers and extracted data
- [ ] Extracted data is a stopgap. The decomp should define these 286 symbols as real source (work in progress in NSD3Decomp); remove each from `config/extract_symbols.txt` when it does
- [ ] Known weakness of the stopgap: 208 pointers lead to data that is private to one source file in the original. The extractor gives each table its own copy. If the game also has that variable and changes it at run time, the table will not see the change
- [ ] The 23 functions in `src/replace/unimplemented.c`: find out for each whether it is reachable, and get it written in the decomp if so

## 3. Boots without a window

Meter: run `D3_NVN_TRACE=frames build/macos/d3`. It prints a line per 60 frames
the game presents; if it needs a function that is still a stand-in it prints the
name and call stack and stops. On 2026-11-22 it initialises every subsystem,
starts 28 threads, loads about 1,700 shader programs and 430 textures, and runs its
main loop steadily (about 140 frames a second with nothing drawn). After 45
seconds it is decoding the opening videos. Whether it then reaches the main menu
has not been observed: with no controller there is no way to skip them yet.

- [x] The game's startup (`nninitStartup`, `nnMain`) called from `src/host/main.cpp`
- [x] `nn::os`: threads, locks, events, semaphores, reader/writer locks, thread-local slots, time, memory regions (`src/nn/os/`)
- [x] `nn::mem`: the game's allocator, on the host's allocator (`src/nn/mem/`)
- [x] `nn::fs`: `game/romfs` as the data volume, a save folder per user, files and directories (`src/nn/fs/`)
- [x] `nn::oe`, `nn::settings`, `nn::time`, `nn::diag`: real, minimal behaviour
- [x] Static initialisers run in the original program's order (`tools/link_order.py`)
- [x] The Switch libc's 32-byte mutex, for the three libraries that embed one (`src/libc/nnmusl_mutex.c`)
- [x] Crash reporter: what happened, registers and call stack (`src/host/crash.c`)
- [x] A graphics driver that draws nothing (`src/gfx/null/`): 498 entry points generated from the decomp's header, with real behaviour only where the game depends on an answer (memory pools, buffers, counters, events, frame acquisition)
- [x] Audio: every FMOD function reports "no sound device", which the game accepts (`src/audio/fmod_off.c`)
- [x] `nn::account`: one fixed local user with no network account
- [x] `nn::hid`: no controller connected
- [x] Network interface, wireless, amiibo, friends, add-on content, parental controls: report unavailable or empty
- [ ] Confirm the main menu is reached. Scripted button presses now get past the opening videos and the game starts drawing interface windows and text, but which screen it is cannot be told until something is drawn
- [ ] 92 stand-ins remain (sockets and TLS, on-screen keyboard, web pages, shop, error dialogs, 23 game functions with no source). None is reached during startup
- [ ] Not yet verified: none of the new `nn::os` or `nn::fs` code has tests, and only the paths startup has reached so far have run at all

## 4. First frame

- [x] A window (SDL2), opened when the game creates its display layer (`src/host/window.cpp`). `D3_HEADLESS=1` runs without one
- [x] Keyboard and game-controller input, presented to the game as one Pro Controller (`src/nn/hid/`). Keys are listed in the README
- [x] Closing the window sends the game its exit request
- [x] Frames paced to 60 a second, as the Switch's display would
- [x] Scripted input for testing without a person: `D3_INPUT_SCRIPT="5:A 8:PLUS"`; `D3_INPUT_TRACE=1` prints button changes
- [ ] **Investigate shaders** (see architecture): the game loads about 1,700 programs compiled for the Switch's GPU. Find out what the data holds and choose translate or rewrite
- [ ] Choose backend approach A or B by counting required functions. The do-nothing driver shows the game uses roughly 150 of the 498 entry points during startup and the first frames
- [ ] Swap chain on the window, textures, vertex and index buffers, render states
- [ ] UI rendering and fonts
- [ ] Not verified: input with a real controller (none was attached), window close, and whether the game's menus respond to the keys as mapped

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
