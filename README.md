# NSD3Recomp

A native port of **Diablo III 2.6.2 (Nintendo Switch)**, built by recompiling the
decompiled source from [NSDiablo3Decomp](https://github.com/loremipsum1077/NSDiablo3Decomp)
for desktop platforms. macOS on Apple silicon is the first target.

> **Not affiliated with Blizzard Entertainment or Nintendo.** This is an unofficial
> fan project, not endorsed by either company. Diablo is a trademark of Blizzard
> Entertainment, Inc.; Nintendo Switch is a trademark of Nintendo. This repository
> contains no game data, game code or Nintendo SDK material. To use it you need
> your own legally obtained copy of Diablo III: Eternal Collection for Nintendo Switch.

**Status: the whole game should be playable, though not all of it has been verified.**
Every decomp source file compiles and links into one macOS executable that runs from
your own game data. The campaign has been played from a cold start into Act V, and
Adventure Mode works: the world map with bounties, Nephalem Rifts opened from the
obelisk, and the Act V town. Nothing seen so far points to a part of the game that
does not work. Seen working, in play and in scripted runs on 2026-11-23:

- Start-up, the gamma and safe-area screens, the welcome dialogs and the main menu.
- Hero Creation, including the class video panel and seasonal heroes (with the
  season mod), then the intro story and the first area.
- Combat, experience and levels, skills, quests, checkpoints and waypoints, towns,
  merchants and the stash.
- Adventure Mode: bounties on the world map, the Nephalem obelisk and rifts.
- Dying, and the resurrect choices that follow.
- Saving: heroes and progress are written to `save/`, and the same hero loads back
  in on the next start.
- Sound, through a replacement for FMOD: in a three-minute run about 1,700 sounds
  were decoded, none failed, and up to 43 played at once.

Not verified yet, or not working:

- Every act played through to the end, each act's bosses, Greater Rifts, and the
  Necromancer and other DLC content have not been checked one by one. Please open
  an issue if something breaks.
- Hero names come from `D3_HERO_NAME` (or "Hero"): there is no on-screen keyboard.
- The first run draws nothing until shaders are translated (see Setup).
- Online features are absent by design: network calls behave like a console with
  nothing attached, and console system dialogs do nothing.
- Only macOS on Apple silicon so far.

See the [roadmap](docs/ROADMAP.md) for the detail.

## Screenshots

Captured on an Apple silicon Mac, drawn at 2560x1440 with the camera pulled back to
1.5x its normal distance (the render-size and camera mods, see [Mods](#mods)).

| | |
|---|---|
| ![The Survivors' Enclave, the Act V town](docs/images/act5-survivors-enclave.jpg) | ![The Adventure Mode world map with bounties](docs/images/adventure-map.jpg) |
| Act V: the Survivors' Enclave | Adventure Mode: the world map and bounties |
| ![Choosing a rift at the Nephalem obelisk](docs/images/rift-obelisk.jpg) | ![Fighting demons inside a Nephalem Rift](docs/images/nephalem-rift.jpg) |
| The Nephalem obelisk | Inside a Nephalem Rift |
| ![Fighting the risen dead at the gates of New Tristram](docs/images/combat-gates.jpg) | ![A ten-kill massacre bonus at the gates](docs/images/massacre.jpg) |
| Act I: the gates of New Tristram | Ten kills at once: the massacre bonus |
| ![New Tristram at night, beside the stash](docs/images/new-tristram.jpg) | ![Buying from Tashun the Miner](docs/images/merchant.jpg) |
| New Tristram, beside the stash | A merchant's shop |
| ![The stash holding the Signet Ring of the Blizzard](docs/images/stash-ring.jpg) | ![The death screen inside a rift](docs/images/died-in-rift.jpg) |
| The stash, with the Signet Ring of the Blizzard | Dying in a rift |

## What is and is not in this repository

In: the build system, the platform layer that replaces the Switch SDK (system
services, graphics, audio, input), compatibility headers, and tools.

Not in, ever: game data, original executables, keys, or Nintendo SDK files. You
supply data from your own copy of the game in `game/`, which git ignores. The
decompiled game source also stays in its own repository and is referenced by path.

## Layout

| Path | Purpose |
|---|---|
| `src/host/` | Program entry, window, main loop, crash reporter |
| `src/nn/` | The Switch SDK services the game calls (`os`, `fs`, `hid`, `account`, ...), on macOS |
| `src/gfx/` | The NVN graphics driver: `metal/` draws, `null/` draws nothing |
| `src/audio/` | The FMOD functions the game uses, on a software mixer through SDL |
| `src/libc/`, `src/replace/` | Switch libc pieces and functions the decomp has no source for |
| `src/compat/` | Headers that let the decomp compile with a host compiler |
| `src/shader/` | Switch GPU instruction table used by the shader translator |
| `cmake/` | Build helpers, including how the decomp is located and how mods patch it |
| `config/` | Data symbols to extract from the dump; the list of mods |
| `tools/` | Shader translator, mods page, data extraction, progress meters |
| `docs/` | [Roadmap](docs/ROADMAP.md), [architecture](docs/ARCHITECTURE.md), [shaders](docs/SHADERS.md) |
| `game/` | **Ignored by git.** Your game dump |
| `save/`, `cache/` | **Ignored by git.** Save files; captured and translated shaders |
| `build/` | **Ignored by git.** Build output |

## Getting the game files

Nothing from the game is in this repository or the decomp, and nobody can give it to
you. You make it yourself from a copy you own:

1. **Own the game.** Diablo III: Eternal Collection for Nintendo Switch (title ID
   `01001B300B9BE000`), on cartridge or from the eShop.
2. **Have version 2.6.2.52309.** The title screen shows it in small print at the
   bottom (`C2.6.2.52309`). Any other version, older or newer, will not match.
   This is the version on the cartridge, with no updates installed. A copy that has
   been updated to a later version will not match.
3. **Dump it from your own Switch.** This needs a console that can run homebrew and
   a dumping tool such as nxdumptool, which reads the installed game with your
   console's own keys. Follow that tool's documentation. This project does not
   provide keys, firmware or instructions for getting around the console's
   protections; do not download the game or keys from anywhere else.
4. **Extract the program's ExeFS and RomFS** into `game/exefs/` and `game/romfs/`
   here. Allow about 30 GB of free space while extracting.
5. **Check them** from the decomp checkout:
   `python3 -B tools/verify_original_inputs.py --input-root ../NSD3Recomp/game`.
   It reports exactly which file is missing or does not match.

`game/` is ignored by git. Keep the dump private: do not commit, upload or share it.

## Setup

1. Check out NSDiablo3Decomp next to this repository (`../NSDiablo3Decomp`), or point
   `D3_DECOMP_DIR` at it.
2. Put your own dump of the game in `game/` (see [Getting the game files](#getting-the-game-files)):
   the program in `game/exefs/` (`main`) and the whole RomFS in `game/romfs/`
   (including `DiabloIIINX64r.nss`). The build reads both, and the game reads its
   data from `game/romfs/` when it runs.
3. Generate the decomp's private inputs (game Protobuf bindings and data tables)
   from that dump, once, in the decomp checkout:

```sh
cd ../NSDiablo3Decomp
python3 tools/bootstrap_game_data.py ../NSD3Recomp/game --build-protoc
cd ../NSD3Recomp
```

4. Build:

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
language code), `D3_PLAYER_NAME=...` (account nickname), `D3_HERO_NAME=...` (the
answer to the game's name prompt), `D3_GAME_DIR`, `D3_SAVE_DIR` and
`D3_SHADER_CACHE` (other folders than `game/`, `save/` and `cache/shaders/`),
`D3_NVN_TRACE=frames` or `all`,
`D3_INPUT_TRACE=1`, `D3_INPUT_SCRIPT="5:A 8:PLUS"` (press buttons at given seconds),
`D3_MOUSE_SCRIPT="40:left:0.5,0.4 45:move:0.3,0.3 50:right:0.6,0.5@2"` (with the Mouse
mod: click or move at a fraction of the window at given seconds, `@` holds that long;
the real pointer is ignored), `D3_MOUSE_TRACE=1` (what each click hit),
`D3_SCREENSHOT=file.png` with `D3_SCREENSHOT_FRAME=2500`, `D3_METAL_STATS=1`,
`D3_FS_TRACE=1`, `D3_TEXTURE_LIST=1`, `D3_DRAW_LOG=<frame>` and `D3_DRAW_SWEEP=<frame>,<n>`
(list a frame's draws; leave out a different slice of them each frame, to find the
one that draws wrongly), `D3_METAL_PIPELINES=1`, `D3_SHADER_SKIP=<hash>,...`, `D3_SOUND_STATS=1` (what was decoded and how loud the output is),
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

<img src="docs/images/mods-page.png" alt="The Mods page: higher resolution, pulled-back camera, faster heroes and offline seasons, with Build and Start game buttons" width="600">

- `-DD3_MOD_RENDER_SIZE=2560x1440`: draw the game at that size instead of the
  Switch's docked 1600x900. The game's memory budget for screen-sized buffers is
  raised to match. Turn it off with `-DD3_MOD_RENDER_SIZE=`.
- `-DD3_MOD_CAMERA_DISTANCE=1.5`: move the in-game camera to that multiple of its
  normal distance from the hero. Turn it off with `-DD3_MOD_CAMERA_DISTANCE=`.
- `-DD3_MOD_SEASON=1`: make that season number active without the online service,
  so the Season Journey and seasonal heroes are available. The game's data holds 12 sets
  of seasonal content that repeat, so 1 to 12 are all the distinct choices. Leaderboards and the other
  online parts of seasons stay unavailable. Turn it off with `-DD3_MOD_SEASON=`.
- `-DD3_MOD_RUN_SPEED=2.0`: heroes walk, run and sprint at that multiple of the
  game's speed. Turn it off again with `-DD3_MOD_RUN_SPEED=`.
- `-DD3_MOD_CHEATS=1`: cheats for testing. In the game, F1 raises the hero to level 70,
  F2 adds 100 paragon levels and F3 adds 10,000,000 gold. While the game runs, the Mods
  page's Cheats panel can do the same, add any amount of gold, and give any item, found
  by its in-game name. Everything goes
  through the game's own level-up and item code (`src/mods/cheats.cpp`), so it is
  saved with the hero like anything earned. The page and the game exchange small
  files in `<save folder>/cheats/`; if you start the game yourself with a different
  `D3_SAVE_DIR`, write commands to that folder's `cheats/commands.txt` instead
  (`level 70`, `paragon 100`, `gold 1000000`, `item Unique_Helm_002_p1`). Turn it off
  with `-DD3_MOD_CHEATS=`.
- `-DD3_MOD_MOUSE=1`: a mouse or trackpad alongside the controller. The Switch's menus
  only follow the controller, so the pointer drives them the same way: pointing at a
  menu entry selects it, a left click chooses it (A), and a click on a dialog's
  Accept or Cancel presses that button. In the game, holding the left button walks
  toward the pointer (it tilts the left stick) and holding the right button holds A,
  the primary skill, which also talks, opens and picks up. Other skills stay on the
  keyboard or controller. `src/mods/mouse.cpp`; turn it off with `-DD3_MOD_MOUSE=`.
- `-DD3_MOD_VENDOR=1`: Deckard Cain stands in New Tristram again, just up and to the
  left of the waypoint, in the story and in adventure mode, and trades. He sells custom
  legendary gems at their highest rank for 1,000,000 gold each. Each one is a copy of a
  real gem, keeping its look and power, with a new name and one more power: every so
  many kills while it is socketed in something you wear, you get a pylon's buff
  (Conduit Shard: Conduit every 10 kills; Shard of Power: Power every 25; Quickening
  Shard: Speed every 10). The gems are added to the game's item table as it loads, so
  they are saved with the hero; a hero holding one needs this mod on to load. Add gems
  in the table at the top of `src/mods/vendor.cpp`. Turn it off with `-DD3_MOD_VENDOR=`.

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

## Licence

The code in this repository is under the [MIT licence](LICENSE). That covers only
what is written here. It grants nothing for the game, its data, the decompiled
source in NSDiablo3Decomp, or anything from Nintendo's SDK. The Switch GPU
instruction table in `src/shader/` comes from envytools and keeps its own MIT
licence (`src/shader/LICENSE-envytools`).
