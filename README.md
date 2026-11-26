# NSD3Recomp

A native port of **Diablo III 2.7.7 (Nintendo Switch)**, the game's last update, built
by recompiling the decompiled source from
[NSDiablo3Decomp](https://github.com/loremipsum1077/NSDiablo3Decomp) for desktop
platforms. macOS on Apple silicon is the first target.

> **Not affiliated with Blizzard Entertainment or Nintendo.** This is an unofficial
> fan project, not endorsed by either company. Diablo is a trademark of Blizzard
> Entertainment, Inc.; Nintendo Switch is a trademark of Nintendo. This repository
> contains no game data, game code or Nintendo SDK material. To use it you need
> your own legally obtained copy of Diablo III: Eternal Collection for Nintendo Switch.

## Branches

- **`dev`**: 2.7.7, built from the decomp's `dev` branch. All work happens here.
- **`2.6.2`**: builds the cartridge version 2.6.2 from the decomp's `2.6.2` branch. It is
  kept as it is and changes only if 2.6.2 itself needs a fix.

## Status

**The whole game should be playable, though not all of it has been verified.**
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
- Saving: heroes and progress are written to `save/2.7.7/`, and the same hero loads back
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
2. **You need two dumps.** 2.7.7 ships no debug file, so the build also reads the one
   in the cartridge version:
   - the **cartridge version 2.6.2.52309**, with no updates installed. The title screen
     shows it in small print at the bottom (`C2.6.2.52309`);
   - the game **updated to 2.7.7** (`C2.7.7.92380`), dumped with the update applied.
3. **Dump it from your own Switch.** This needs a console that can run homebrew and
   a dumping tool such as nxdumptool, which reads the installed game with your
   console's own keys. Follow that tool's documentation. This project does not
   provide keys, firmware or instructions for getting around the console's
   protections; do not download the game or keys from anywhere else.
4. **Extract each program's ExeFS and RomFS**: 2.6.2's into `game/exefs/` and
   `game/romfs/`, and 2.7.7's into `game/2.7.7/exefs/` and `game/2.7.7/romfs/`. Allow
   about 30 GB of free space for each while extracting.
5. **Check the 2.6.2 dump** from the decomp checkout:
   `python3 -B tools/verify_original_inputs.py --input-root ../NSD3Recomp/game`.
   It reports exactly which file is missing or does not match. The 2.7.7 program is
   checked in the next section.

`game/` is ignored by git. Keep the dump private: do not commit, upload or share it.

## Setup

1. Check out NSDiablo3Decomp next to this repository (`../NSDiablo3Decomp`), or point
   `D3_DECOMP_DIR` at it.
2. Put your own dumps in `game/` and `game/2.7.7/` (see
   [Getting the game files](#getting-the-game-files)). The build reads 2.7.7's program
   and 2.6.2's debug file, and the game reads its data from `game/2.7.7/romfs/` when
   it runs.
3. Generate the decomp's private inputs (the 2.7.7 program as an ELF, game Protobuf
   bindings and data tables) from those dumps, once, in the decomp checkout. These are
   the steps in its README's
   [Setting up for 2.7.7](https://github.com/loremipsum1077/NSDiablo3Decomp#setting-up-for-277):

```sh
cd ../NSDiablo3Decomp
python3 setup.py
toolchain/nx-decomp-tools-binaries/macos/nx2elf ../NSD3Recomp/game/2.7.7/exefs/main --export-elf data/2.7.7/main.elf
shasum -a 256 data/2.7.7/main.elf   # b462b3f68d0afc80eb2331b2b18c96a86046ef5a459bedc2439aa7959cfa13c5
python3 tools/bootstrap_game_data.py ../NSD3Recomp/game --build-protoc
python3 tools/bootstrap_protobuf.py
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
python3 tools/shader_translate.py --cache cache/shaders-2.7.7
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
`D3_SHADER_CACHE` (other folders than `game/2.7.7/`, `save/2.7.7/` and
`cache/shaders-2.7.7/`),
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
  of seasonal content that repeat, so 1 to 12 already cover every Season Journey; 14 to 37
  also give that season's theme with `-DD3_MOD_THEME=Season`. Leaderboards and the other
  online parts of seasons stay unavailable. Turn it off with `-DD3_MOD_SEASON=`.
- `-DD3_MOD_THEME=Season`: turn on a season theme. In 2.7.7 the themes are
  "community buffs" that Blizzard's servers switched on for a season, so offline none
  start. `Season` takes the theme of the season `D3_MOD_SEASON` makes active, from a table
  of seasons 14 to 37 in `src/mods/theme.cpp` (following
  [d3hack](https://github.com/god-jester/d3hack)'s season map; season 23's theme is built
  into 2.7.7). Otherwise the value is the buff names, comma-separated: `DoubleGoblins` (season 14),
  `DoubleBountyBags` (15), `RoyalGrandeur` (16), `LegacyOfNightmares` (17),
  `TriunesWill` (18), `Pandemonium` (19), `KanaiPowers` (20, 31), `TrialsOfTempests`
  (21), `ShadowClones,FourthKanaisCubeSlot` (22, 33), `EtherealItems` (24, 32), `SoulShards`
  (25, 30, 36), `SwarmRifts` (26), `SanctifiedItems` (27, 34), `DarkAlchemy` (28) and
  `NestingPortals` (29); the game also reads `DoubleBloodShards`, `DoubleRiftKeystones` and
  `ParagonCap`.
  The theme applies to every hero. With any theme on, the Altar of Rites and Visions of
  Enmity are on too, as in 2.7.7. Turn it off with `-DD3_MOD_THEME=`.
- `-DD3_MOD_EVENTS=Anniversary`: events Blizzard's servers ran, comma-separated.
  `Anniversary` opens the Darkening of Tristram (its portal is in New Tristram) and gives
  the anniversary buff; without it the dungeon still opens every January, from the game's
  own dates. `CowLevel` is the game's not-a-cow-level easter egg. `DoubleBloodShards` and
  `DoubleRiftKeystones` double those rewards. The first two change the defaults of the
  settings the servers would have sent; the doubles are community buffs, given as the
  theme mod gives its own. `src/mods/events.cpp`; turn it off with `-DD3_MOD_EVENTS=`.
- `-DD3_MOD_CHALLENGE=rotate`: Challenge Rifts offline. On the Switch the game
  downloaded each week's challenge as two files from Blizzard's servers:
  `challengerift_config.dat` (the challenge's number, times and orb multiplier) and
  `challengerift_NN.dat` (the rift, and the hero and account to play it with). Offline
  nothing arrives, so the game reads them from a folder instead: `D3_CHALLENGE_DIR`,
  else `rift_data` in the game folder. No challenge files ship with this project, as
  they hold another player's hero and account; use files you saved yourself. With
  `rotate` the distinct challenges in the folder take turns, moving on each time a reward
  is earned; a number (`-DD3_MOD_CHALLENGE=3`) always plays `challengerift_03.dat`.
  The challenge never expires, the reward cache counts offline, and leaderboards stay
  unavailable. Challenge Rifts open once a hero on the account has cleared a solo
  Greater Rift; the `rifts 70` cheat below does that. `src/mods/challenge.cpp`; turn it
  off with `-DD3_MOD_CHALLENGE=`.
- `-DD3_MOD_RUN_SPEED=2.0`: heroes walk, run and sprint at that multiple of the
  game's speed. Turn it off again with `-DD3_MOD_RUN_SPEED=`.
- `-DD3_MOD_CHEATS=1`: cheats for testing. In the game, F1 raises the hero to level 70,
  F2 adds 100 paragon levels and F3 adds 10,000,000 gold. While the game runs, the Mods
  page's Cheats panel can do the same, add any amount of gold, give any item, found
  by its in-game name, unlock Kanai's Cube, count greater rifts up to a tier as cleared
  solo (which opens Challenge Rifts), and give Horadric Caches from any act (for
  the game's current difficulty). Everything goes
  through the game's own level-up and item code (`src/mods/cheats.cpp`), so it is
  saved with the hero like anything earned. The page and the game exchange small
  files in `<save folder>/cheats/`; if you start the game yourself with a different
  `D3_SAVE_DIR`, write commands to that folder's `cheats/commands.txt` instead
  (`level 70`, `paragon 100`, `gold 1000000`, `item Unique_Helm_002_p1`, `cube`, `rifts 70`,
  `cache 3 5` for five act III caches). Turn it off
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
- `-DD3_MOD_CORRUPT=1`: Kanai's Cube gains a last page, Corrupt Legendary: 1 legendary
  item, 25 Death's Breath, 5 Forgotten Souls and 1 Corrupted Angel Flesh. The item gains
  a hidden affix holding one boss skill, picked at random from 27 (Diablo, Belial,
  Azmodan, Zoltun Kulle, Izual, Maghda, the Skeleton King, Siegebreaker, Malthael,
  Urzael, the Spider Queen and Adria), and the tooltip names it. While it is worn, each
  hit has a chance (15%, scaled by the skill's proc coefficient; at most once every 1.5
  seconds per item) to cast that skill from the hero. Corrupting the item again rerolls
  the skill. A hero holding a corrupted item needs this mod on to load. The skills are
  listed at the top of `src/mods/corrupt.cpp`. Turn it off with `-DD3_MOD_CORRUPT=`.

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
