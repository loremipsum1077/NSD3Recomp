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
| 3 | Boots without a window | Startup runs to the main menu state with data loaded | Exit test passes: reaches Hero Creation (seen in milestone 4's screenshots) |
| 4 | First frame | Main menu drawn in a window | Exit test passes: Hero Creation and its welcome dialog are drawn, with defects listed below |
| 5 | In game | Create a hero, enter the first area, move and fight | Hero created, first area entered, hero moves; fighting not tried |
| 6 | Sound | Music, effects and voice play | In progress: sound plays; not yet listened to and checked |
| 7 | Keeps progress | Saves and settings survive a restart | Not started |
| 8 | Release quality | A full act played without a crash; packaged as an app | Not started |
| 9 | More platforms | Linux, then Windows | Not started |

## 0. Foundations

- [x] Repository layout, build system, `game/` ignored by git
- [x] Decomp located by path (`D3_DECOMP_DIR`, `extern/NSD3Decomp`, or `../NSD3Decomp`)
- [x] Compile probe (`tools/compile_probe.py`)
- [ ] Decide how the decomp is pinned: git submodule at a fixed commit, or a recorded commit hash checked at configure time
- [ ] `tools/verify_game.py`: confirm the dump in `game/` is version 2.6.2 by file hashes, and report what is missing
- [x] Document how to produce the dump from one's own copy (README, "Getting the game files")
- [x] Licence: MIT for this repository's own code (`LICENSE`)
- [ ] Contribution rules

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
- [x] Audio: every FMOD function reports "no sound device", which the game accepts (since replaced, see milestone 6)
- [x] `nn::account`: one fixed local user with no network account
- [x] `nn::hid`: no controller connected
- [x] Network interface, wireless, amiibo, friends, add-on content, parental controls: report unavailable or empty
- [x] Main menu reached and driven by input (milestone 4 screenshots). Earlier note: Scripted button presses now get past the opening videos and the game starts drawing interface windows and text, but which screen it is cannot be told until something is drawn
- [ ] 92 stand-ins remain (sockets and TLS, on-screen keyboard, web pages, shop, error dialogs, 23 game functions with no source). None is reached during startup
- [ ] Not yet verified: none of the new `nn::os` or `nn::fs` code has tests, and only the paths startup has reached so far have run at all

## 4. First frame

- [x] A window (SDL2), opened when the game creates its display layer (`src/host/window.cpp`). `D3_HEADLESS=1` runs without one
- [x] Keyboard and game-controller input, presented to the game as one Pro Controller (`src/nn/hid/`). Keys are listed in the README
- [x] Closing the window sends the game its exit request
- [x] Frames paced to 60 a second, as the Switch's display would
- [x] Scripted input for testing without a person: `D3_INPUT_SCRIPT="5:A 8:PLUS"`; `D3_INPUT_TRACE=1` prints button changes
- [x] Investigate shaders: no source in the data, only Switch GPU code plus a complete reflection table; they must be translated ([findings](SHADERS.md))
- [x] Obtain the GPU instruction encoding: envytools' table, MIT-licensed; 43 instruction types cover the start-up shaders (`tools/shader_stats.py`)
- [x] Shader translator, stage 1: every shader disassembles to readable text with reflection names (`tools/shader_disasm.py`); control flow is forward-only except two loops
- [x] `c[1]` is each shader's own pool of literal constants, stored after its code; the control block gives its offset and size
- [x] Stage 2: emit Metal shading language (`tools/shader_translate.py`): 990 of 1,010 captured shaders translate and all 990 compile
- [ ] The 20 shaders that use instructions not handled yet (3D textures, gather, size queries, a few rare integer forms)
- [ ] Move the translator into the program (it is a Python tool today) with an on-disk cache
- [x] Stage 3: translated shaders draw the menu correctly, which supports the inferred rules for texture reads, literal pools, integer inputs and interpolation; the rules for depth-only shaders and `fmnmx` are still unconfirmed
- [x] Backend approach chosen: implement the driver interface on Metal (B). The game uses 114 of the 498 entry points (see architecture)
- [x] Metal driver, part 1 (`src/gfx/metal/nvn_metal.mm`): device, the game's textures as render targets, clears, presenting to the window, screenshots (`D3_SCREENSHOT=file.png`, `D3_SCREENSHOT_FRAME=n`)
- [x] Part 2, draws: vertex and index data read straight from the game's memory pools, a copy of the constant blocks per draw, pipelines built from translated shaders, blend/depth/polygon/stencil state. About 1,230 draws a frame at 60 frames a second on Hero Creation
- [x] Part 3, textures: contents rearranged from the Switch GPU's tiled layout; compressed formats; samplers; channel remapping (fonts)
- [x] Shader cache: the driver writes each shader's code to `cache/shaders/`; `python3 tools/shader_translate.py --cache cache/shaders` translates what is missing. 1,268 of the 1,312 start-up shaders translate
- [x] A private static C++ library for the whole program (see architecture): without it the game's replacement of `operator new` collided with the system's libraries
- [x] Hero Creation renders correctly apart from the video panel: hero models, lighting, effects and colour grading all match what the screen should look like
- [ ] The class video panel is solid green. No video frame is ever handed to the driver, so this is not a drawing fault: playback is not running. The video archives are opened at start-up (`enUS_CommonCutscene.cpk` and the per-act ones), so the data is found. Videos as such do work: the owner saw cinematics playing in copies of the game started with an empty save folder. Why this panel's video does not start is unknown
- [ ] 2 start-up shaders do not translate (three integer instruction forms)
- [ ] The first run has no translated shaders and draws nothing; translation should happen inside the program
- [x] Plain-row ("linear") textures: row size reported to the game and contents uploaded as rows
- [x] Soft particles (glows, fog, fire) read a copy of the depth buffer; reading the attached one showed as random dark squares
- [x] Closing the window ends the game: it is asked to exit, and the process ends 3 seconds later (the game's own shutdown does not finish yet)
- [x] Per-frame scratch memory grows on demand, and small draws copy only the vertices they use; running out left later draws without constants (whole screens one colour)
- [ ] Not implemented in the driver: scissor, stencil-cull criteria, array textures, mip levels of 3D textures, texture views, non-indexed draws
- [ ] Each frame waits for the GPU to finish before the game continues, and small draws copy their data. Both are simple and safe and both cost time; neither has been measured on a heavy scene
- [x] The queue's finish request is honoured (recorded draws are completed before the game continues)
- [ ] Not checked: anything beyond Hero Creation; resizing the window; whether colours are exact (sRGB handling was not examined)
- [ ] UI rendering and fonts
- [ ] Not verified: input with a real controller (none was attached), window close, and whether the game's menus respond to the keys as mapped

## 5. In game

Reached on 2026-11-22 by scripted input from a cold start: create a Barbarian, accept
the default name, start the game, arrive outside New Tristram with the HUD up, and
walk under stick input. The game wrote account, profile and hero files to `save/`.

- [x] Hero creation through to the first area, by scripted input
- [x] World, hero, lighting and shadows, HUD and minimap are drawn
- [x] Movement with the left stick
- [x] Attacking: the A button plays the attack and a monster walks into view. Whether hits land and monsters die has not been confirmed
- [x] Cloth streaks fixed. The cloth simulation, its vertex data, the shader and its constants were all correct at the moment each draw was recorded. The game rewrites the memory behind its per-frame geometry later in the same frame, and the driver, which copies nothing, was drawing from the later contents. Small draws now use a copy of their vertices and indices taken when they are recorded (`src/gfx/metal/nvn_metal.mm`). How the Switch's driver avoids the same problem was not established; the three non-matching cloth functions in the decomp were not at fault
- [ ] Picking things up, talking to characters, in-game menus: none tried
- (Not a defect: the empty globe is the Barbarian's Fury, which starts empty.)
- [ ] Whether a saved hero loads again on the next run: not checked (the second run created another hero)
- [ ] Hero names: the on-screen keyboard answers with `D3_HERO_NAME` or "Hero"; there is no text entry

Still to do from the original list:


- [ ] World, actor and effect rendering
- [ ] Physics (Domino) and cloth behave as on the Switch
- [ ] Local server and client loop stable through the first quest
- [ ] A way to compare behaviour with the original: recorded inputs or logged game state

## 6. Sound

- [x] Audio route: the 74 FMOD functions are written here on a software mixer (`src/audio/fmod_host.cpp`, output through SDL)
- [x] Ogg Vorbis files and sample banks (Vorbis and FMOD ADPCM) decode; in a 70 second run into the game, 1,242 sounds, none failed
- [x] Playing, pausing, looping, volume, pitch, groups, distance attenuation and left/right placement
- [ ] Listen to it: nobody has yet. Measured only: real signal reaches the device, up to 14 sounds at once, no clipping
- [ ] Decode streams as they play. Everything is decoded in full when created, which costs memory (music most of all)
- [ ] Effects: reverb, the game's DSP units, occlusion, doppler, priorities when voices run out
- [ ] Video soundtracks (the game-filled stream) checked against a playing video

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
| Audio: FMOD's own macOS library (the player downloads it) or a replacement for the 74 functions used | Milestone 6 | Replacement written here |
| Graphics backend approach | Milestone 4 | Decided: implement the driver interface on Metal (B) |
| Graphics API on macOS | Milestone 4 | Decided: Metal directly. Other platforms will need their own driver file |
| Shaders: translate or rewrite | Milestone 4 | Decided: translate ([findings](SHADERS.md)) |
| Source of the Switch GPU instruction encoding | Milestone 4 | Decided: envytools (MIT), kept in an ignored folder |
| Window and controller library | Milestone 4 | SDL (also covers Linux and Windows) |
| Mouse and keyboard play | Milestone 8 | The Switch version is controller-only; later |
