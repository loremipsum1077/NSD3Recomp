# Shaders: what the game data contains

Investigated 2026-11-22 by running the game with the do-nothing graphics driver
and capturing what it loads. Numbers are from start-up through the opening videos;
more shaders load with each area of the game.

## Findings

- **No shader source is shipped.** Each compiled shader file has exactly two
  sections: machine code for the Switch's GPU, and a reflection table. There is no
  assembly listing, no debug section and no source text.
- **The reflection table is complete and readable.** It names every input
  (`vs_TEXCOORD0` ...), every constant in the constant block (`CBMain`: `fogColor`,
  `matWorldViewProj`, `alphaTestRef`, `hitFlashColorMul` ...) with its offset, and
  the samplers. The game itself reads this table to bind things.
- **The shaders came from HLSL.** Names such as `hlslcc_mtx4x4matWorldViewProj` are
  the mark of HLSLcc, a tool that converts Direct3D shaders to GLSL. So the Switch
  shaders are the PC game's shaders, converted and recompiled. The intermediate
  GLSL is not in the data.
- **They are small and repetitive.** 1,678 programs are created at start-up, 1,312
  of them distinct, 3.7 MB of code in all. A typical shader is 60 instructions; the
  largest is 1,128.
- **43 different instructions cover everything.** Decoded against a reference
  table (below), the 238,057 instructions in the start-up set use 43 instruction
  types; 48 words (0.02%) are not instructions at all. Three of them, multiply-add,
  multiply and add on floats, are two thirds of all code.
- **Vertex and fragment only.** No geometry, tessellation or compute shaders.

## What this means

Rewriting by hand is out: there is no source to work from and over a thousand
variants. The practical route is a **translator** from the Switch GPU's machine
code to the host's shading language, run on the player's own data, with results
cached in an ignored folder. Three things make that tractable here:

1. The instruction variety is small, so the translator needs a few dozen
   instruction types, not the GPU's whole set.
2. The reflection table supplies every name, offset and binding, which is the part
   a translator normally has to guess.
3. Every shader came through the same two tools, so the code is regular.

Switch emulators translate this same machine code, which shows it can be done, and
their public documentation describes the instruction encoding.

## Consequence for the renderer design

The graphics driver (NVN) is handed only the GPU code and a control block. The
reflection table stays on the game's side of that boundary. A renderer that
replaces the game's Switch backend files (`Gfx/NX64/`, approach A in the
architecture document) sees the whole compiled-shader file, reflection included;
one that implements the driver interface underneath them (approach B) does not.
This favours approach A, or B with one extra hook at shader creation.

## Instructions used

`python3 tools/shader_stats.py <dump folder>` prints this table from a dump.

| Group | Instructions (uses) |
|---|---|
| Float arithmetic | `ffma` 74,632, `fmul` 57,963, `fadd` 26,012, `fmnmx` 5,186, `fmul32i` 148, `fadd32i` 137, `rro` 750 |
| Functions (reciprocal, square root, sine, exponent ...) | `mufu` 10,108 |
| Reading inputs and constants, writing outputs | `ld` 13,785, `st` 13,223, `ipa` 8,226 |
| Moves and constants | `mov` 4,910, `mov32i` 1,548, `sel` 6 |
| Conversions | `i2f` 4,458, `f2f` 28, `f2i` 8, `i2i` 5 |
| Comparison and flow | `bra` 3,975, `isetp` 2,638, `fsetp` 822, `fset` 175, `psetp` 1, `kil` 579, `exit` 1,313, `nop` 3,284 |
| Textures | `texs` 2,561, `tex` 18, `tld4s` 10, `tlds` 1, `txq` 1, `depbar` 208 |
| Integer and bit operations | `xmad` 1,198, `lop` 51, `lop3` 14, `iadd` 10, `iadd3` 3, `imnmx` 3, `shl` 3, `shr` 3, `bfi` 1, `lop32i` 1 |
| Other | `vset` 3 |

## The reference

The Switch's GPU belongs to NVIDIA's GM107 ("Maxwell") family. The instruction
table used here is from envytools (`envydis/gm107.c`), an MIT-licensed
reverse-engineering project, downloaded with the owner's permission into
`extern/reference/`, which git ignores. It gives each instruction's bit pattern,
name and operand fields. It does not describe the 0x50-byte program header or the
control block.

## Open questions

- The program header (which inputs and outputs a shader uses) and the control block
  have not been decoded. The reference does not cover them; the reflection table
  may make them unnecessary.
- Shaders loaded after start-up have not been looked at; an area may use
  instructions the start-up set does not. `tools/shader_stats.py` will show any.
- Branches: 3,975 `bra` in 1,312 shaders. How many are plain if/else and how many
  are loops decides how much control-flow recovery the translator needs.

## Reproducing

`D3_SHADER_DUMP=<folder> build/macos/d3` writes each program's GPU code and the
first 4 KB of its control block as the game hands them to the driver. The complete
files with the reflection section were captured once, with a temporarily modified
copy of the game's shader-loading file; that is not part of the build.
