# Architecture

## Approach

This is a **source port**. The decomp provides the game as C and C++ source; we
compile that source with the host's compiler and replace everything below it that
belonged to the Switch. We do not translate the original machine code.

The game's source is treated as read-only. Whatever the port needs is provided
underneath it, so the same decomp checkout serves both projects. Where a decomp
file genuinely cannot be used as it is, the fix is in this order of preference:

1. Provide what it expects (a header in `src/compat/`, a function in a platform layer).
2. Leave the file out of the build and provide a replacement here (`src/replace/`).
3. Change the decomp, guarded so its matching build is unaffected. Last resort,
   and done in that repository through its own landing rules.

## Layers

```
        game code            decomp: src/X, src/Contrib, src/BlizzardCore   (unchanged)
  ------------------------
     Switch SDK interface    nn::os, nn::fs, nn::hid, nn::account ...  +  NVN graphics
  ------------------------
       platform layer        this repository
  ------------------------
     macOS / Linux / Windows
```

The original binary contains 2,191 functions of SDK and runtime code that have no
source in the decomp. The platform layer stands in for all of them.

## Source layout

Directories are created when their milestone starts. An empty tree tells nobody
anything, so only what exists is in the repository.

| Directory | Contents | Milestone |
|---|---|---|
| `src/host/` | `main`, startup, window, main loop, paths, configuration | 0, 3 |
| `src/compat/` | Headers bridging Switch toolchain and host toolchain | 1 |
| `src/nn/<area>/` | The SDK's system services implemented on the host, one directory per SDK area (`os`, `fs`, `hid`, `account`, `socket`, `oe`, `settings`, `time` ...). `unimplemented.c` in each lists what is still a stand-in | 2, 3 |
| `src/libc/` | Pieces of the Switch's C library that the source depends on by layout or name | 2 |
| `src/gfx/` | The renderer (see Graphics below) | 4 |
| `src/audio/` | Audio: route to be decided (see FMOD below) | 6 |
| `src/replace/` | Gap-fillers for things the decomp lacks; each goes away when the decomp has the real thing | 2 |
| `config/` | Lists that drive the tools (names only, no content) | 2 |
| `tools/` | Progress meters, game-data verification | all |
| `tests/` | Tests of the platform layer | 2 onward |

## What the game uses from the SDK

Measured in the decomp source (occurrences of each `nn::` area):

| Area | Uses | Plan |
|---|---:|---|
| `os` (threads, mutexes, events, memory) | 273 | Implement on pthreads |
| `account` (user profiles) | 152 | One fixed local user |
| `ldn` (local wireless play) | 126 | Stub: reports unavailable |
| `nfp` (amiibo) | 109 | Stub: reports unavailable |
| `fs` (files, save data) | 77 | Implement: `game/romfs` and a save folder |
| `hid` (controllers, touch) | 75 | Implement on the host's controller API |
| `friends` | 67 | Stub |
| `socket`, `nifm`, `ssl`, `nsd` (network) | 103 | Stub first; real sockets only if online play becomes a goal |
| `oe` (operating mode, focus, performance) | 44 | Implement minimally |
| `settings`, `time`, `util`, `diag`, `result` | 119 | Implement |
| `swkbd`, `web`, `err`, `vi`, `aoc`, others | ~50 | Stub or minimal |

About 80 decomp source files touch `nn::` at all; 30 of them are in the game client.

## Graphics

The game draws through its own interface (`src/X/Client/Gfx/Gfx*.h`), with one
backend for the Switch in `Gfx/NX64/` (six files) written against NVN, Nintendo's
graphics API. Two ways to replace it:

- **A. New backend.** Leave `Gfx/NX64/` out of the build and write a host backend
  in `src/gfx/` against the same game-side interface.
- **B. NVN on the host.** Keep `Gfx/NX64/` and implement the NVN calls it makes.

Preferred: **A**, because the game-side interface is small and already
platform-neutral, while NVN is large. To be confirmed at milestone 4 by counting
the functions each option requires.

**Shaders have no source in the data**, only machine code for the Switch's GPU
and a reflection table of names. They have to be translated; see
[SHADERS.md](SHADERS.md). The reflection table is only visible above the driver
interface, which is a further reason to prefer approach A.

## Audio, physics and other middleware

The decomp includes source for most middleware the game links: Domino (physics),
Lua, zlib, Ogg Vorbis, PCRE, protobuf, curl, DemonWare. They are built from that
source and need no platform work beyond compiling. DemonWare and curl are only
for online services.

**FMOD (audio) is the exception.** The decomp has its headers only; the library
itself is part of the 2,191 functions without source. The game calls 74 FMOD
functions. They must come from FMOD's own macOS library, which each person would
download themselves, or from a replacement written here.

## Data the decomp does not define

286 variables that exist in the original program (name tables, tag tables,
constants, a few middleware tables) are declared in the decomp but not defined,
because the decomp tracks functions. They are game content and cannot be stored
here. `tools/extract_data.py` generates C definitions for them at build time from
the player's dump: each becomes a block of bytes in which every pointer is a
string literal or a reference to another symbol. This is a stopgap until the
decomp defines them as real source.

## How the decomp is made to compile on the host

Nothing in the decomp is edited. Three mechanisms, all in `src/compat/` and
`cmake/DecompSources.cmake`:

- **A prefix header** (`d3_host_prefix.h`) is read before every decomp file. Its
  main job: on macOS `int64_t` is `long long`, on the Switch it is `long`. The
  game's source treats `uint64` and `unsigned long` as one type, so the prefix
  makes them one type on macOS too. Platform-layer code that shares C++
  interfaces with the game must be compiled with the same prefix.
- **Stand-in headers** placed earlier on the include path than the decomp's own,
  for the few decomp headers written against the Switch's libraries.
- **Per-library settings** copied from the decomp's build where they change
  behaviour (for example which libraries treat `char` as unsigned).

The decomp's top-level `include/` directory is not used: it rebinds libc names to
Switch symbols.

## Rules for platform-layer code

Learned the hard way while bringing startup up:

- **No global objects with constructors.** The game calls into the platform layer
  from its own static initialisers, which can run first. Create such state on
  first use.
- **Fit inside the game's objects.** An SDK object is as big as the Switch made it
  (a light event is 12 bytes, a libc mutex 32). Never hand its address to a host
  function that expects the host's larger type; keep the state in the bytes
  available, or keep a pointer there.
- **`malloc` in this program is the game's.** It reaches the host's allocator
  through `nn::mem`, so mixing is safe, but the host allocator itself has to be
  called by its own name (`src/nn/mem/mem.cpp`).
- **Basic types as the Switch spells them.** A function's linker name includes its
  parameter types: `unsigned long`, not `uint64_t`.

## Platform differences to watch

macOS on Apple silicon was chosen first because it is the same processor family
as the Switch: 64-bit ARM, same pointer and integer sizes, and `char` is signed on
both. Known differences that could matter where the game reads data files straight
into structures or relies on type sizes:

- `int64_t` is `long long` on macOS and `long` on the Switch. Handled: see the prefix header above.
- `long double` is 128-bit on the Switch, 64-bit on macOS.
- `wchar_t` is unsigned on the Switch, signed on macOS.
- The C++ standard library's own types are laid out differently.
- Memory pages are 16 KB on macOS, 4 KB on the Switch.

Apart from the first, none of these has been checked against the game yet.
