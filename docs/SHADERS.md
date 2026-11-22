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
  largest is 1,128. Counting by the leading bits of each instruction, 20 patterns
  cover 95% of all instructions and 50 cover 99.8%.
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

## Open questions

- The instruction encoding has to come from a reference. None is on this machine.
  Options: download public emulator documentation, or work it out from the data
  with the reflection table as a guide (slower).
- Any emulator source used as a reference is under a copyleft licence. The
  translator must be written from the documented encoding, not copied.
- What the control block contains has not been decoded.
- Shaders loaded after start-up have not been looked at; an area may use
  instructions the start-up set does not.

## Reproducing

`D3_SHADER_DUMP=<folder> build/macos/d3` writes each program's GPU code and the
first 4 KB of its control block as the game hands them to the driver. The complete
files with the reflection section were captured once, with a temporarily modified
copy of the game's shader-loading file; that is not part of the build.
