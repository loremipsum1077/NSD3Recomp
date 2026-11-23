// The Switch's graphics driver interface (NVN), implemented on Metal.
//
// The game's own Switch renderer runs unchanged and calls about 114 driver entry
// points. This file takes over those entry points when a window exists; anything
// it does not define falls back to the do-nothing driver in ../null/.
//
// How commands are handled. NVN records commands into a buffer and submits them
// later. The game records on one render thread and submits in recording order, so
// here every command takes effect as it is recorded, and submission is a no-op.
//
// State. The driver's objects (textures, builders, windows ...) are storage the
// game allocates at the size the Switch's driver needs. Each structure below lives
// at the start of that storage and must fit: see the size notes and static_asserts.
//
// Shaders. The game hands over GPU machine code. tools/shader_translate.py turns it
// into Metal source; this file looks each shader up by a hash of its code in the
// shader cache folder, and writes the code there when no translation exists yet, so
// that `python3 tools/shader_translate.py --cache <folder>` can fill the gap. A draw
// whose shaders are not translated is skipped and counted.
//
// Not done: stencil-cull criteria, scissor, 3D and array textures, texture views,
// draws that are not indexed. D3_METAL_STATS=1 prints what was drawn and skipped.
#include "../null/nvn_null.h"
#include "host/window.h"

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <mach/vm_page_size.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

namespace {

// ---- statistics (D3_METAL_STATS=1) ----

enum Stat { kDrawn, kNoShader, kNoPipeline, kNoBuffer, kNoTarget, kUnsupported, kStatCount };
unsigned g_stats[kStatCount];
const char * const kStatNames[kStatCount] = {"drawn", "skipped: shader not translated", "skipped: pipeline rejected",
                                             "skipped: data not in a known memory pool", "skipped: no render target",
                                             "skipped: primitive or index type not supported"};

// ---- formats ----

struct FormatInfo {
    MTLPixelFormat metal;
    int bytes;          // per pixel, or per 4x4 block when `block` is set
    bool block;
    bool depth;
};

FormatInfo Format(int nvn)
{
    switch (nvn)
    {
    case 1: return {MTLPixelFormatR8Unorm, 1};
    case 5: return {MTLPixelFormatR16Float, 2};
    case 6: return {MTLPixelFormatR16Unorm, 2};
    case 10: return {MTLPixelFormatR32Float, 4};
    case 13: return {MTLPixelFormatRG8Unorm, 2};
    case 17: return {MTLPixelFormatRG16Float, 4};
    case 22: return {MTLPixelFormatRG32Float, 8};
    case 37: case 109: return {MTLPixelFormatRGBA8Unorm, 4};
    case 38: return {MTLPixelFormatRGBA8Snorm, 4};
    case 39: return {MTLPixelFormatRGBA8Uint, 4};
    case 41: return {MTLPixelFormatRGBA16Float, 8};
    case 42: return {MTLPixelFormatRGBA16Unorm, 8};
    case 43: return {MTLPixelFormatRGBA16Snorm, 8};
    case 46: return {MTLPixelFormatRGBA32Float, 16};
    case 50: return {MTLPixelFormatDepth16Unorm, 2, false, true};
    case 51: case 52: return {MTLPixelFormatDepth32Float, 4, false, true};
    case 53: case 54: return {MTLPixelFormatDepth32Float_Stencil8, 4, false, true};     // no 24-bit depth on Apple GPUs
    case 55: case 56: return {MTLPixelFormatRGBA8Unorm_sRGB, 4};
    case 59: return {MTLPixelFormatA1BGR5Unorm, 2};
    case 60: return {MTLPixelFormatB5G6R5Unorm, 2};
    case 61: return {MTLPixelFormatRGB10A2Unorm, 4};
    case 63: return {MTLPixelFormatRG11B10Float, 4};
    case 65: case 66: return {MTLPixelFormatBC1_RGBA, 8, true};
    case 67: return {MTLPixelFormatBC2_RGBA, 16, true};
    case 68: return {MTLPixelFormatBC3_RGBA, 16, true};
    case 69: case 70: return {MTLPixelFormatBC1_RGBA_sRGB, 8, true};
    case 71: return {MTLPixelFormatBC2_RGBA_sRGB, 16, true};
    case 72: return {MTLPixelFormatBC3_RGBA_sRGB, 16, true};
    case 73: return {MTLPixelFormatBC4_RUnorm, 8, true};
    case 75: return {MTLPixelFormatBC5_RGUnorm, 16, true};
    case 77: return {MTLPixelFormatBC7_RGBAUnorm, 16, true};
    case 78: return {MTLPixelFormatBC7_RGBAUnorm_sRGB, 16, true};
    case 153: case 154: return {MTLPixelFormatBGRA8Unorm, 4};
    case 155: case 156: return {MTLPixelFormatBGRA8Unorm_sRGB, 4};
    default: return {MTLPixelFormatInvalid, 4};
    }
}

// ---- object state, kept at the start of the game's own objects ----

// MemoryPool is 256 bytes. The first two fields are what the do-nothing driver keeps,
// so its pool and buffer functions keep working on these objects.
struct PoolState {
    void * memory;
    size_t size;
    void * buffer;              // retained id<MTLBuffer> over the same memory, no copy
    uintptr_t base;             // start of that buffer (the pool's memory rounded down to a page)
    size_t length;
};

struct BufferState { PoolState * pool; ptrdiff_t offset; size_t size; };   // as in the do-nothing driver

struct TextureDesc {                // TextureBuilder is 0x80 bytes
    int width, height, depth;
    int format, target, flags, levels;
    uint8_t swizzle[4];             // which source channel each of r, g, b, a reads: 0 zero, 1 one, 2 r, 3 g, 4 b, 5 a
    PoolState * pool;
    ptrdiff_t offset;
    ptrdiff_t stride;               // bytes per row of a plain-row ("linear") texture; 0 if not set
};

struct TextureState {               // Texture is 0xc0 bytes
    TextureDesc desc;
    void * metal;                   // retained id<MTLTexture>
    bool cleared;                   // a clear is waiting for the next render pass
    float clearColor[4];
    float clearDepth;
    int clearStencil;
    bool clearDepthPending, clearStencilPending;
};
static_assert(sizeof(TextureDesc) <= 0x80 && sizeof(TextureState) <= 0xc0, "texture state must fit the game's objects");

struct WindowDesc {                 // WindowBuilder is 0x40 bytes, Window 0x180
    int count;
    TextureState * textures[4];
    int interval;
};
static_assert(sizeof(WindowDesc) <= 0x40, "window state must fit the game's objects");

struct SamplerDesc {                // SamplerBuilder and Sampler are 0x60 bytes
    int minFilter, magFilter;
    int wrap[3];
    void * metal;                   // retained id<MTLSamplerState>
};

// The small state objects are only 4 or 8 bytes, so their settings are packed.
struct AttribState { uint32_t format : 8, stream : 5, offset : 18, set : 1; };      // VertexAttribState: 4 bytes
struct StreamState { int32_t stride; int32_t divisor; };                              // VertexStreamState: 8 bytes
struct BlendDesc { uint8_t srcColor, dstColor, srcAlpha, dstAlpha, opColor, opAlpha, target, pad; };   // BlendState: 8 bytes
struct ColorDesc { uint32_t blendEnable; };                                           // ColorState
struct MaskDesc { uint32_t mask; };                                                   // ChannelMaskState: 4 bits per target
struct PolygonDesc { uint32_t cull : 2, frontFace : 1, mode : 2; };                   // PolygonState
struct DepthDesc {                                                                    // DepthStencilState: 8 bytes
    uint64_t depthTest : 1, depthWrite : 1, depthFunc : 4, stencilTest : 1;
    uint64_t frontFunc : 4, frontFail : 4, frontDepthFail : 4, frontPass : 4;
    uint64_t backFunc : 4, backFail : 4, backDepthFail : 4, backPass : 4;
};
static_assert(sizeof(AttribState) == 4 && sizeof(StreamState) == 8 && sizeof(BlendDesc) == 8 && sizeof(DepthDesc) == 8 &&
              sizeof(PolygonDesc) == 4, "packed state sizes");

struct ProgramState {               // Program is 0xc0 bytes
    void * function;                // retained id<MTLFunction>, null if the shader has no translation
    int stage;                      // 0 vertex, 1 fragment
    const void * literals;          // the shader's pool of literal constants (its c[1])
    uint32_t literalSize;
    uint64_t hash;                  // of its code: the name of its file in the shader cache
};

struct UniformBinding { uint64_t address; size_t size; };
struct TextureBinding { TextureState * texture; SamplerDesc * sampler; };

// Everything the next draw depends on.
struct DrawState {
    ProgramState * program[2];
    AttribState attribs[16];
    int attribCount;
    StreamState streams[16];
    UniformBinding vertexBuffers[16];
    UniformBinding uniforms[2][14];
    TextureBinding textures[2][16];
    BlendDesc blend;
    ColorDesc color;
    MaskDesc mask;
    PolygonDesc polygon;
    DepthDesc depth;
    float blendColor[4];
    int stencilRef;
    int viewport[4];
    float depthRange[2];
    bool viewportSet;
} g_draw;

// ---- the device ----

id<MTLDevice> g_device;
id<MTLCommandQueue> g_queue;
CAMetalLayer * g_layer;
id<MTLRenderPipelineState> g_presentPipeline;

// The render thread's work in progress.
id<MTLCommandBuffer> g_commands;
id<MTLRenderCommandEncoder> g_pass;
TextureState * g_color;
TextureState * g_depth;
unsigned g_frame;

id<MTLTexture> Metal(const TextureState * t)
{
    return t && t->metal ? (__bridge id<MTLTexture>)t->metal : nil;
}

id<MTLCommandBuffer> Commands()
{
    if (!g_commands)
    {
        g_commands = [g_queue commandBuffer];
    }
    return g_commands;
}

void EndPass()
{
    if (g_pass)
    {
        [g_pass endEncoding];
        g_pass = nil;
    }
}

// Starts a render pass on the current targets if none is open. Clears that were
// requested since the last pass are applied as the pass begins.
id<MTLRenderCommandEncoder> Pass()
{
    if (g_pass)
    {
        return g_pass;
    }
    if (!Metal(g_color) && !Metal(g_depth))
    {
        return nil;
    }
    MTLRenderPassDescriptor * d = [MTLRenderPassDescriptor renderPassDescriptor];
    if (id<MTLTexture> color = Metal(g_color))
    {
        d.colorAttachments[0].texture = color;
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
        d.colorAttachments[0].loadAction = g_color->cleared ? MTLLoadActionClear : MTLLoadActionLoad;
        const float * c = g_color->clearColor;
        d.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
        g_color->cleared = false;
    }
    if (id<MTLTexture> depth = Metal(g_depth))
    {
        d.depthAttachment.texture = depth;
        d.depthAttachment.storeAction = MTLStoreActionStore;
        d.depthAttachment.loadAction = g_depth->clearDepthPending ? MTLLoadActionClear : MTLLoadActionLoad;
        d.depthAttachment.clearDepth = g_depth->clearDepth;
        if (depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
        {
            d.stencilAttachment.texture = depth;
            d.stencilAttachment.storeAction = MTLStoreActionStore;
            d.stencilAttachment.loadAction = g_depth->clearStencilPending ? MTLLoadActionClear : MTLLoadActionLoad;
            d.stencilAttachment.clearStencil = static_cast<uint32_t>(g_depth->clearStencil);
        }
        g_depth->clearDepthPending = g_depth->clearStencilPending = false;
    }
    g_pass = [Commands() renderCommandEncoderWithDescriptor:d];
    return g_pass;
}

// A clear with no draw after it still has to happen: open a pass and close it.
void FlushClears()
{
    bool pending = (g_color && g_color->cleared) || (g_depth && (g_depth->clearDepthPending || g_depth->clearStencilPending));
    if (pending && !g_pass)
    {
        Pass();
    }
    EndPass();
}

// ---- memory pools ----
// The game owns the memory of every pool and writes vertices, indices and constants
// into it directly. Each pool is wrapped in a Metal buffer over the same pages, so
// the GPU reads what the game wrote without a copy.

PoolState * g_pools[512];
int g_poolCount;

int PoolInitialize(PoolState * pool, const PoolState * builder)
{
    @autoreleasepool
    {
        memset(pool, 0, sizeof(*pool));
        pool->memory = builder->memory;
        pool->size = builder->size;
        if (!pool->memory || !pool->size)
        {
            return 1;
        }
        uintptr_t page = vm_page_size;
        uintptr_t start = reinterpret_cast<uintptr_t>(pool->memory) & ~(page - 1);
        uintptr_t end = (reinterpret_cast<uintptr_t>(pool->memory) + pool->size + page - 1) & ~(page - 1);
        id<MTLBuffer> buffer = [g_device newBufferWithBytesNoCopy:reinterpret_cast<void *>(start)
                                                           length:end - start
                                                          options:MTLResourceStorageModeShared
                                                      deallocator:nil];
        if (!buffer)
        {
            fprintf(stderr, "metal: could not wrap a %zu-byte memory pool; draws that use it will be skipped\n", pool->size);
            return 1;
        }
        pool->buffer = (__bridge_retained void *)buffer;
        pool->base = start;
        pool->length = end - start;
        if (g_poolCount < 512)
        {
            g_pools[g_poolCount++] = pool;
        }
        return 1;
    }
}

void PoolFinalize(PoolState * pool)
{
    for (int i = 0; i < g_poolCount; i++)
    {
        if (g_pools[i] == pool)
        {
            g_pools[i] = g_pools[--g_poolCount];
            break;
        }
    }
    if (pool->buffer)
    {
        CFRelease(pool->buffer);
        pool->buffer = nullptr;
    }
}

// The Metal buffer and offset for an address inside some pool.
id<MTLBuffer> BufferAt(uint64_t address, size_t size, NSUInteger * offset)
{
    for (int i = 0; i < g_poolCount; i++)
    {
        PoolState * p = g_pools[i];
        if (address >= p->base && address + size <= p->base + p->length)
        {
            *offset = address - p->base;
            return (__bridge id<MTLBuffer>)p->buffer;
        }
    }
    return nil;
}

// Constant blocks change between draws within a frame, and the GPU reads them only
// after the frame is submitted, so each draw gets its own copy. The copies live in
// large buffers that are reused a few frames later.
const int kScratchBuffers = 4;
const NSUInteger kScratchSize = 96 << 20;
id<MTLBuffer> g_scratch[kScratchBuffers];
NSUInteger g_scratchUsed;

id<MTLBuffer> Snapshot(const void * data, size_t size, NSUInteger * offset)
{
    int slot = g_frame % kScratchBuffers;
    if (!g_scratch[slot])
    {
        g_scratch[slot] = [g_device newBufferWithLength:kScratchSize options:MTLResourceStorageModeShared];
    }
    NSUInteger at = (g_scratchUsed + 255) & ~static_cast<NSUInteger>(255);
    if (at + size > kScratchSize)
    {
        return nil;
    }
    memcpy(static_cast<char *>(g_scratch[slot].contents) + at, data, size);
    g_scratchUsed = at + size;
    *offset = at;
    return g_scratch[slot];
}

// ---- device ----

int DeviceInitialize(void * device, const void * builder)
{
    (void)device;
    (void)builder;
    return 1;
}

// ---- textures ----

void TextureBuilderSetDefaults(TextureDesc * b)
{
    memset(b, 0, sizeof(*b));
    b->width = b->height = b->depth = 1;
    b->target = 1;      // 2D
    b->levels = 1;
    b->swizzle[0] = 2; b->swizzle[1] = 3; b->swizzle[2] = 4; b->swizzle[3] = 5;
}

// Fonts and masks are single-channel textures that the game reads as alpha.
void TextureBuilderSetSwizzle(TextureDesc * b, int r, int g, int bl, int a)
{
    b->swizzle[0] = r; b->swizzle[1] = g; b->swizzle[2] = bl; b->swizzle[3] = a;
}

MTLTextureSwizzle Swizzle(int s)
{
    switch (s)
    {
    case 0: return MTLTextureSwizzleZero;
    case 1: return MTLTextureSwizzleOne;
    case 3: return MTLTextureSwizzleGreen;
    case 4: return MTLTextureSwizzleBlue;
    case 5: return MTLTextureSwizzleAlpha;
    default: return MTLTextureSwizzleRed;
    }
}

void TextureBuilderSetSize3D(TextureDesc * b, int w, int h, int d) { b->width = w; b->height = h; b->depth = d; }
void TextureBuilderSetFormat(TextureDesc * b, int format) { b->format = format; }
void TextureBuilderSetTarget(TextureDesc * b, int target) { b->target = target; }
void TextureBuilderSetFlags(TextureDesc * b, int flags) { b->flags = flags; }
void TextureBuilderSetLevels(TextureDesc * b, int levels) { b->levels = levels < 1 ? 1 : levels; }
void TextureBuilderSetStorage(TextureDesc * b, PoolState * pool, ptrdiff_t offset) { b->pool = pool; b->offset = offset; }

void TextureBuilderSetStride(TextureDesc * b, ptrdiff_t stride) { b->stride = stride; }

size_t AlignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

// A texture made with the LINEAR flag keeps its texels as plain rows, not in the
// tiled layout. The game writes into these directly (it asks for the row size) and
// rewrites them while running.
const int kTextureFlagLinear = 0x10;

size_t RowStride(const TextureDesc & d)
{
    return d.stride > 0 ? static_cast<size_t>(d.stride) : static_cast<size_t>(d.width) * Format(d.format).bytes;
}

// Size of one mip level in the Switch GPU's block-linear layout: rows of 64-byte
// by 8-row tiles, stacked into blocks of up to 16 tiles that shrink for small levels.
size_t LevelSize(const FormatInfo & f, int w, int h, int layers)
{
    size_t wb = f.block ? (static_cast<size_t>(w) + 3) / 4 * f.bytes : static_cast<size_t>(w) * f.bytes;
    size_t rows = f.block ? (static_cast<size_t>(h) + 3) / 4 : static_cast<size_t>(h);
    size_t blockHeight = 16;
    while (blockHeight > 1 && AlignUp(rows, 8) <= 8 * blockHeight / 2)
    {
        blockHeight /= 2;
    }
    return AlignUp(wb, 64) * AlignUp(rows, 8 * blockHeight) * layers;
}

size_t TextureBuilderGetStorageSize(const TextureDesc * b)
{
    FormatInfo f = Format(b->format);
    if (b->flags & kTextureFlagLinear)
    {
        return AlignUp(RowStride(*b) * b->height, 0x1000);
    }
    int layers = b->target == 8 ? 6 : b->target == 2 ? 1 : b->depth > 1 && b->target == 4 ? b->depth : 1;
    size_t total = 0;
    for (int level = 0; level < b->levels; level++)
    {
        int w = b->width >> level ? b->width >> level : 1;
        int h = b->height >> level ? b->height >> level : 1;
        int d = b->target == 2 ? (b->depth >> level ? b->depth >> level : 1) : 1;
        // A 3D level is stored in whole blocks of up to 16 slices.
        total += LevelSize(f, w, h, layers * (b->target == 2 ? static_cast<int>(AlignUp(d, 16)) : 1));
    }
    return AlignUp(total, 0x1000);
}

size_t TextureBuilderGetStorageAlignment(const TextureDesc *)
{
    return 0x1000;
}

int TextureInitialize(TextureState * t, const TextureDesc * b)
{
    @autoreleasepool
    {
        memset(t, 0, sizeof(*t));
        t->desc = *b;
        FormatInfo f = Format(b->format);
        if (f.metal == MTLPixelFormatInvalid)
        {
            fprintf(stderr, "metal: texture format %d is not mapped yet; the texture is left empty\n", b->format);
            return 1;
        }
        MTLTextureDescriptor * d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f.metal
                                                                                      width:b->width
                                                                                     height:b->height
                                                                                  mipmapped:b->levels > 1];
        d.mipmapLevelCount = b->levels;
        if (b->target == 8)
        {
            d.textureType = MTLTextureTypeCube;
        }
        else if (b->target == 2)
        {
            d.textureType = MTLTextureType3D;
            d.depth = b->depth;
        }
        d.usage = MTLTextureUsageShaderRead | (f.block ? 0 : MTLTextureUsageRenderTarget);
        if (b->swizzle[0] != 2 || b->swizzle[1] != 3 || b->swizzle[2] != 4 || b->swizzle[3] != 5)
        {
            d.swizzle = MTLTextureSwizzleChannelsMake(Swizzle(b->swizzle[0]), Swizzle(b->swizzle[1]), Swizzle(b->swizzle[2]),
                                                      Swizzle(b->swizzle[3]));
            d.usage = MTLTextureUsageShaderRead;     // a remapped texture cannot also be drawn into
        }
        d.storageMode = f.depth ? MTLStorageModePrivate : MTLStorageModeShared;
        id<MTLTexture> texture = [g_device newTextureWithDescriptor:d];
        if (!texture)
        {
            fprintf(stderr, "metal: could not create a %dx%d texture of format %d\n", b->width, b->height, b->format);
            return 1;
        }
        t->metal = (__bridge_retained void *)texture;
        return 1;
    }
}

void TextureFinalize(TextureState * t)
{
    if (t->metal)
    {
        if (g_color == t || g_depth == t)
        {
            EndPass();
            g_color = g_color == t ? nullptr : g_color;
            g_depth = g_depth == t ? nullptr : g_depth;
        }
        CFRelease(t->metal);
        t->metal = nullptr;
    }
}

ptrdiff_t TextureGetStride(const TextureState * t) { return static_cast<ptrdiff_t>(RowStride(t->desc)); }
int TextureGetWidth(const TextureState * t) { return t->desc.width; }
int TextureGetHeight(const TextureState * t) { return t->desc.height; }
int TextureGetFormat(const TextureState * t) { return t->desc.format; }

// ---- window ----

void WindowBuilderSetDefaults(WindowDesc * b) { memset(b, 0, sizeof(*b)); b->interval = 1; }
void WindowBuilderSetPresentInterval(WindowDesc * b, int interval) { b->interval = interval; }

void WindowBuilderSetTextures(WindowDesc * b, int count, TextureState * const * textures)
{
    b->count = count > 4 ? 4 : count;
    for (int i = 0; i < b->count; i++)
    {
        b->textures[i] = textures[i];
    }
}

int WindowInitialize(WindowDesc * window, const WindowDesc * builder)
{
    *window = *builder;
    return 1;
}

// ---- render targets and clears ----

void SetRenderTargets(void * cb, int numColors, TextureState * const * colors, const void * colorViews, TextureState * depth,
                      const void * depthView)
{
    (void)cb;
    (void)colorViews;
    (void)depthView;
    d3_nvn_called("nvnCommandBufferSetRenderTargets");
    TextureState * color = numColors > 0 && colors ? colors[0] : nullptr;
    if (color == g_color && depth == g_depth)
    {
        return;
    }
    @autoreleasepool
    {
        FlushClears();
        g_color = color;
        g_depth = depth;
    }
}

void ClearColor(void * cb, int index, const float * color, int mask)
{
    (void)cb;
    (void)mask;
    d3_nvn_called("nvnCommandBufferClearColor");
    if (index != 0 || !g_color)
    {
        return;
    }
    @autoreleasepool
    {
        EndPass();
        memcpy(g_color->clearColor, color, sizeof(g_color->clearColor));
        g_color->cleared = true;
    }
}

void ClearDepthStencil(void * cb, float depth, int depthMask, int stencil, int stencilMask)
{
    (void)cb;
    d3_nvn_called("nvnCommandBufferClearDepthStencil");
    if (!g_depth)
    {
        return;
    }
    @autoreleasepool
    {
        EndPass();
        if (depthMask)
        {
            g_depth->clearDepth = depth;
            g_depth->clearDepthPending = true;
        }
        if (stencilMask)
        {
            g_depth->clearStencil = stencil;
            g_depth->clearStencilPending = true;
        }
    }
}

// ---- texture contents ----
// The game stores texel data in the pool the texture was created on, in the Switch
// GPU's "block linear" layout: 64-byte by 8-row tiles with a fixed order inside each
// tile, stacked into blocks. FlushTexels says the data is ready; it is rearranged
// into plain rows and handed to Metal.

size_t TileOffset(size_t x, size_t y)
{
    return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}

void Deswizzle(const uint8_t * src, uint8_t * dst, size_t rowBytes, size_t rows)
{
    size_t blockHeight = 16;
    while (blockHeight > 1 && AlignUp(rows, 8) <= 8 * blockHeight / 2)
    {
        blockHeight /= 2;
    }
    size_t tilesAcross = AlignUp(rowBytes, 64) / 64;
    size_t blockBytes = 512 * blockHeight;
    for (size_t y = 0; y < rows; y++)
    {
        size_t tileRow = y / 8;
        size_t rowBase = (tileRow / blockHeight) * tilesAcross * blockBytes + (tileRow % blockHeight) * 512;
        for (size_t x = 0; x < rowBytes; x += 16)
        {
            size_t n = rowBytes - x < 16 ? rowBytes - x : 16;
            memcpy(dst + y * rowBytes + x, src + rowBase + (x / 64) * blockBytes + TileOffset(x, y), n);
        }
    }
}

// A 3D texture is grouped by slice instead of by row: each block is one tile high and
// up to 16 slices deep. Established from the game's 64x64x64 colour lookup table,
// whose contents are (nearly) their own coordinates.
void Deswizzle3D(const uint8_t * src, uint8_t * dst, size_t rowBytes, size_t rows, size_t slices)
{
    size_t blockHeight = 1, blockDepth = 16;
    while (blockDepth > 1 && slices <= blockDepth / 2)
    {
        blockDepth /= 2;
    }
    size_t tilesAcross = AlignUp(rowBytes, 64) / 64;
    size_t blocksDown = AlignUp(rows, 8 * blockHeight) / (8 * blockHeight);
    size_t sliceBytes = 512 * blockHeight;                  // one slice of one block
    size_t blockBytes = sliceBytes * blockDepth;
    for (size_t z = 0; z < slices; z++)
    {
        size_t zBase = (z / blockDepth) * tilesAcross * blocksDown * blockBytes + (z % blockDepth) * sliceBytes;
        for (size_t y = 0; y < rows; y++)
        {
            size_t tileRow = y / 8;
            size_t rowBase = zBase + (tileRow / blockHeight) * tilesAcross * blockBytes + (tileRow % blockHeight) * 512;
            for (size_t x = 0; x < rowBytes; x += 16)
            {
                size_t n = rowBytes - x < 16 ? rowBytes - x : 16;
                memcpy(dst + (z * rows + y) * rowBytes + x, src + rowBase + (x / 64) * blockBytes + TileOffset(x, y), n);
            }
        }
    }
}

void TextureFlushTexels(TextureState * t, const void * view, const void * region)
{
    (void)view;
    (void)region;
    d3_nvn_called("nvnTextureFlushTexels");
    id<MTLTexture> texture = Metal(t);
    if (!texture || !t->desc.pool || !t->desc.pool->memory)
    {
        return;
    }
    if (t->desc.target == 2)
    {
        @autoreleasepool
        {
            // Only the first mip level; the game's 3D textures are small lookup tables.
            FormatInfo f = Format(t->desc.format);
            size_t rowBytes = static_cast<size_t>(t->desc.width) * f.bytes;
            NSMutableData * plain = [NSMutableData dataWithLength:rowBytes * t->desc.height * t->desc.depth];
            Deswizzle3D(static_cast<const uint8_t *>(t->desc.pool->memory) + t->desc.offset, static_cast<uint8_t *>(plain.mutableBytes),
                        rowBytes, t->desc.height, t->desc.depth);
            [texture replaceRegion:MTLRegionMake3D(0, 0, 0, t->desc.width, t->desc.height, t->desc.depth)
                       mipmapLevel:0
                             slice:0
                         withBytes:plain.bytes
                       bytesPerRow:rowBytes
                     bytesPerImage:rowBytes * t->desc.height];
            static int shown;
            if (shown++ < 4 && getenv("D3_METAL_STATS"))
            {
                fprintf(stderr, "metal: 3D texture %dx%dx%d format %d uploaded\n", t->desc.width, t->desc.height, t->desc.depth, t->desc.format);
            }
        }
        return;
    }
    if (t->desc.flags & kTextureFlagLinear)
    {
        @autoreleasepool
        {
            [texture replaceRegion:MTLRegionMake2D(0, 0, t->desc.width, t->desc.height)
                       mipmapLevel:0
                             slice:0
                         withBytes:static_cast<const uint8_t *>(t->desc.pool->memory) + t->desc.offset
                       bytesPerRow:RowStride(t->desc)
                     bytesPerImage:0];
        }
        return;
    }
    @autoreleasepool
    {
        FormatInfo f = Format(t->desc.format);
        const uint8_t * src = static_cast<const uint8_t *>(t->desc.pool->memory) + t->desc.offset;
        int faces = t->desc.target == 8 ? 6 : 1;
        NSMutableData * plain = [NSMutableData data];
        for (int face = 0; face < faces; face++)
        {
            for (int level = 0; level < t->desc.levels; level++)
            {
                int w = t->desc.width >> level ? t->desc.width >> level : 1;
                int h = t->desc.height >> level ? t->desc.height >> level : 1;
                size_t rowBytes = f.block ? (static_cast<size_t>(w) + 3) / 4 * f.bytes : static_cast<size_t>(w) * f.bytes;
                size_t rows = f.block ? (static_cast<size_t>(h) + 3) / 4 : static_cast<size_t>(h);
                plain.length = rowBytes * rows;
                Deswizzle(src, static_cast<uint8_t *>(plain.mutableBytes), rowBytes, rows);
                [texture replaceRegion:MTLRegionMake2D(0, 0, w, h)
                           mipmapLevel:level
                                 slice:face
                             withBytes:plain.bytes
                           bytesPerRow:rowBytes
                         bytesPerImage:0];
                src += LevelSize(f, w, h, 1);
            }
        }
    }
}

// ---- samplers, and the tables that give textures and samplers their numbers ----

void SamplerBuilderSetDefaults(SamplerDesc * b)
{
    memset(b, 0, sizeof(*b));
    b->minFilter = 0;
    b->magFilter = 0;
    b->wrap[0] = b->wrap[1] = b->wrap[2] = 7;      // clamp to edge
}

void SamplerBuilderSetMinMagFilter(SamplerDesc * b, int minFilter, int magFilter) { b->minFilter = minFilter; b->magFilter = magFilter; }
void SamplerBuilderSetWrapMode(SamplerDesc * b, int s, int t, int r) { b->wrap[0] = s; b->wrap[1] = t; b->wrap[2] = r; }

MTLSamplerAddressMode Wrap(int mode)
{
    switch (mode)
    {
    case 1: return MTLSamplerAddressModeRepeat;
    case 6: return MTLSamplerAddressModeMirrorRepeat;
    case 2: case 3: return MTLSamplerAddressModeMirrorClampToEdge;
    case 5: case 4: return MTLSamplerAddressModeClampToBorderColor;
    default: return MTLSamplerAddressModeClampToEdge;
    }
}

int SamplerInitialize(SamplerDesc * sampler, const SamplerDesc * builder)
{
    @autoreleasepool
    {
        *sampler = *builder;
        MTLSamplerDescriptor * d = [MTLSamplerDescriptor new];
        d.minFilter = (builder->minFilter == 0 || builder->minFilter == 2 || builder->minFilter == 4) ? MTLSamplerMinMagFilterNearest
                                                                                                      : MTLSamplerMinMagFilterLinear;
        d.magFilter = builder->magFilter == 0 ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        d.mipFilter = builder->minFilter < 2 ? MTLSamplerMipFilterNotMipmapped
                                             : builder->minFilter < 4 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
        d.sAddressMode = Wrap(builder->wrap[0]);
        d.tAddressMode = Wrap(builder->wrap[1]);
        d.rAddressMode = Wrap(builder->wrap[2]);
        sampler->metal = (__bridge_retained void *)[g_device newSamplerStateWithDescriptor:d];
        return 1;
    }
}

const int kMaxIds = 16384;
TextureState * g_textureById[kMaxIds];
SamplerDesc * g_samplerById[kMaxIds];

void TexturePoolRegisterTexture(const void * pool, int id, TextureState * texture, const void * view)
{
    (void)pool;
    (void)view;
    if (id >= 0 && id < kMaxIds)
    {
        g_textureById[id] = texture;
    }
}

void SamplerPoolRegisterSampler(const void * pool, int id, SamplerDesc * sampler)
{
    (void)pool;
    if (id >= 0 && id < kMaxIds)
    {
        g_samplerById[id] = sampler;
    }
}

uint64_t DeviceGetTextureHandle(const void * device, int textureId, int samplerId)
{
    (void)device;
    return static_cast<uint64_t>(static_cast<uint32_t>(textureId)) << 32 | static_cast<uint32_t>(samplerId);
}

void BindTexture(void * cb, int stage, int index, uint64_t handle)
{
    (void)cb;
    uint32_t textureId = static_cast<uint32_t>(handle >> 32), samplerId = static_cast<uint32_t>(handle);
    if (stage < 0 || stage > 1 || index < 0 || index >= 16)
    {
        return;
    }
    g_draw.textures[stage][index].texture = textureId < kMaxIds ? g_textureById[textureId] : nullptr;
    g_draw.textures[stage][index].sampler = samplerId < kMaxIds ? g_samplerById[samplerId] : nullptr;
}

// ---- small state objects ----

void AttribSetDefaults(AttribState * a) { memset(a, 0, sizeof(*a)); }
void AttribSetFormat(AttribState * a, int format, ptrdiff_t offset) { a->format = format; a->offset = static_cast<uint32_t>(offset); a->set = 1; }
void AttribSetStreamIndex(AttribState * a, int stream) { a->stream = stream; }
void StreamSetDefaults(StreamState * s) { s->stride = 0; s->divisor = 0; }
void StreamSetStride(StreamState * s, ptrdiff_t stride) { s->stride = static_cast<int32_t>(stride); }

void BlendSetDefaults(BlendDesc * b) { *b = BlendDesc{2, 1, 2, 1, 1, 1, 0, 0}; }      // one, zero, add
void BlendSetBlendFunc(BlendDesc * b, int srcColor, int dstColor, int srcAlpha, int dstAlpha)
{
    b->srcColor = srcColor; b->dstColor = dstColor; b->srcAlpha = srcAlpha; b->dstAlpha = dstAlpha;
}
void BlendSetBlendEquation(BlendDesc * b, int color, int alpha) { b->opColor = color; b->opAlpha = alpha; }
void ColorSetDefaults(ColorDesc * c) { c->blendEnable = 0; }
void ColorSetBlendEnable(ColorDesc * c, int index, int enable)
{
    c->blendEnable = enable ? c->blendEnable | 1u << index : c->blendEnable & ~(1u << index);
}
void MaskSetDefaults(MaskDesc * m) { m->mask = 0xffffffffu; }
void MaskSetChannelMask(MaskDesc * m, int index, int r, int g, int b, int a)
{
    uint32_t bits = (r ? 1u : 0) | (g ? 2u : 0) | (b ? 4u : 0) | (a ? 8u : 0);
    m->mask = (m->mask & ~(15u << (index * 4))) | bits << (index * 4);
}
void PolygonSetDefaults(PolygonDesc * p) { p->cull = 0; p->frontFace = 1; p->mode = 2; }
void PolygonSetCullFace(PolygonDesc * p, int face) { p->cull = face; }
void PolygonSetFrontFace(PolygonDesc * p, int frontFace) { p->frontFace = frontFace; }
void PolygonSetPolygonMode(PolygonDesc * p, int mode) { p->mode = mode; }

void DepthSetDefaults(DepthDesc * d)
{
    memset(d, 0, sizeof(*d));
    d->depthWrite = 1;
    d->depthFunc = 2;       // less
    d->frontFunc = d->backFunc = 8;                 // always
    d->frontFail = d->frontDepthFail = d->frontPass = 1;    // keep
    d->backFail = d->backDepthFail = d->backPass = 1;
}
void DepthSetDepthTestEnable(DepthDesc * d, int on) { d->depthTest = on != 0; }
void DepthSetDepthWriteEnable(DepthDesc * d, int on) { d->depthWrite = on != 0; }
void DepthSetDepthFunc(DepthDesc * d, int func) { d->depthFunc = func; }
void DepthSetStencilTestEnable(DepthDesc * d, int on) { d->stencilTest = on != 0; }
void DepthSetStencilFunc(DepthDesc * d, int face, int func)
{
    if (face & 1) d->frontFunc = func;
    if (face & 2) d->backFunc = func;
}
void DepthSetStencilOp(DepthDesc * d, int face, int fail, int depthFail, int pass)
{
    if (face & 1) { d->frontFail = fail; d->frontDepthFail = depthFail; d->frontPass = pass; }
    if (face & 2) { d->backFail = fail; d->backDepthFail = depthFail; d->backPass = pass; }
}

// ---- binding state for the next draw ----

void BindVertexAttribState(void * cb, int count, const AttribState * states)
{
    (void)cb;
    g_draw.attribCount = count > 16 ? 16 : count;
    memcpy(g_draw.attribs, states, sizeof(AttribState) * g_draw.attribCount);
}

void BindVertexStreamState(void * cb, int count, const StreamState * states)
{
    (void)cb;
    memcpy(g_draw.streams, states, sizeof(StreamState) * (count > 16 ? 16 : count));
}

void BindVertexBuffers(void * cb, int first, int count, const UniformBinding * ranges)
{
    (void)cb;
    for (int i = 0; i < count && first + i < 16; i++)
    {
        g_draw.vertexBuffers[first + i] = ranges[i];
    }
}

void BindUniformBuffer(void * cb, int stage, int index, uint64_t address, size_t size)
{
    (void)cb;
    if (stage >= 0 && stage <= 1 && index >= 0 && index < 14)
    {
        g_draw.uniforms[stage][index] = UniformBinding{address, size};
    }
}

// An update recorded between draws applies to the draws after it. Commands take
// effect as they are recorded here, so the bytes are written straight away.
void UpdateUniformBuffer(void * cb, uint64_t buffer, size_t bufferSize, ptrdiff_t offset, size_t size, const void * data)
{
    (void)cb;
    (void)bufferSize;
    if (buffer && data)
    {
        memcpy(reinterpret_cast<void *>(buffer + offset), data, size);
    }
}

void BindBlendState(void * cb, const BlendDesc * b) { (void)cb; g_draw.blend = *b; }
void BindColorState(void * cb, const ColorDesc * c) { (void)cb; g_draw.color = *c; }
void BindChannelMaskState(void * cb, const MaskDesc * m) { (void)cb; g_draw.mask = *m; }
void BindPolygonState(void * cb, const PolygonDesc * p) { (void)cb; g_draw.polygon = *p; }
void BindDepthStencilState(void * cb, const DepthDesc * d) { (void)cb; g_draw.depth = *d; }
void SetBlendColor(void * cb, const float * c) { (void)cb; memcpy(g_draw.blendColor, c, sizeof(g_draw.blendColor)); }
void SetStencilRef(void * cb, int face, int ref) { (void)cb; (void)face; g_draw.stencilRef = ref; }
void SetDepthRange(void * cb, float n, float f) { (void)cb; g_draw.depthRange[0] = n; g_draw.depthRange[1] = f; }

void SetViewport(void * cb, int x, int y, int w, int h)
{
    (void)cb;
    g_draw.viewport[0] = x; g_draw.viewport[1] = y; g_draw.viewport[2] = w; g_draw.viewport[3] = h;
    g_draw.viewportSet = true;
}

// ---- shaders ----

struct ShaderData { uint64_t data; const void * control; };

NSString * CacheFolder()
{
    static NSString * folder;
    if (!folder)
    {
        const char * env = getenv("D3_SHADER_CACHE");
        folder = @(env ? env : D3_DEFAULT_SHADER_CACHE);
        [[NSFileManager defaultManager] createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
    }
    return folder;
}

NSMutableDictionary<NSNumber *, id> * g_functions;      // code hash -> id<MTLFunction>, or NSNull when there is none
unsigned g_missingShaders;

// `instructionBytes` is how much of the code is instructions; literal constants and
// padding follow, and must not be handed to the translator as if they were code.
id<MTLFunction> FunctionFor(const void * code, size_t size, size_t instructionBytes, int * stage, uint64_t * hashOut)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; i++)
    {
        hash = (hash ^ static_cast<const uint8_t *>(code)[i]) * 0x100000001b3ull;
    }
    *hashOut = hash;
    uint32_t word0;
    memcpy(&word0, static_cast<const uint8_t *>(code) + 0x30, 4);
    *stage = ((word0 >> 10) & 15) == 5 ? 1 : 0;
    if (!g_functions)
    {
        g_functions = [NSMutableDictionary dictionary];
    }
    id known = g_functions[@(hash)];
    if (known)
    {
        return known == [NSNull null] ? nil : known;
    }
    NSString * base = [CacheFolder() stringByAppendingPathComponent:[NSString stringWithFormat:@"%016llx", hash]];
    NSString * source = [NSString stringWithContentsOfFile:[base stringByAppendingString:@".metal"] encoding:NSUTF8StringEncoding error:nil];
    // For finding a shader that draws wrongly: D3_SHADER_SKIP lists hashes (separated by
    // commas) to treat as untranslated, so their draws are skipped.
    static const char * skip = getenv("D3_SHADER_SKIP");
    if (skip && strstr(skip, [NSString stringWithFormat:@"%016llx", hash].UTF8String))
    {
        g_functions[@(hash)] = [NSNull null];
        return nil;
    }
    id<MTLFunction> function = nil;
    if (source)
    {
        NSError * error = nil;
        id<MTLLibrary> library = [g_device newLibraryWithSource:source options:nil error:&error];
        function = [library newFunctionWithName:*stage ? @"d3_fragment" : @"d3_vertex"];
        if (!function)
        {
            fprintf(stderr, "metal: translated shader %016llx does not compile: %s\n", hash, error.localizedDescription.UTF8String);
        }
    }
    else
    {
        // Leave the code where the translator will find it.
        NSString * codePath = [base stringByAppendingString:@".code"];
        if (![[NSFileManager defaultManager] fileExistsAtPath:codePath])
        {
            [[NSData dataWithBytes:code length:instructionBytes] writeToFile:codePath atomically:YES];
        }
        g_missingShaders++;
    }
    g_functions[@(hash)] = function ?: [NSNull null];
    return function;
}

int ProgramInitialize(ProgramState * p, void * device)
{
    (void)device;
    memset(p, 0, sizeof(*p));
    return 1;
}

int ProgramSetShaders(ProgramState * p, int count, const ShaderData * shaders)
{
    d3_nvn_called("nvnProgramSetShaders");
    if (count < 1 || !shaders[0].data)
    {
        return 1;
    }
    @autoreleasepool
    {
        const void * code = reinterpret_cast<const void *>(shaders[0].data);
        void * last = nullptr;
        size_t size = 0;
        d3_nvn_last_buffer(&last, &size);
        if (last != code || size < 0x80)
        {
            return 1;       // the size of the code is only known from the buffer the game just made for it
        }
        size_t instructionBytes = size;
        if (shaders[0].control)
        {
            uint32_t end;
            memcpy(&end, static_cast<const uint8_t *>(shaders[0].control) + 0x6f8, 4);
            if (end >= 0x80 && end <= size)
            {
                instructionBytes = end;
            }
        }
        id<MTLFunction> function = FunctionFor(code, size, instructionBytes, &p->stage, &p->hash);
        p->function = function ? (__bridge_retained void *)function : nullptr;
        // The control block says where the shader's literal constants sit inside its code.
        if (shaders[0].control)
        {
            uint32_t poolSize, poolOffset;
            memcpy(&poolSize, static_cast<const uint8_t *>(shaders[0].control) + 0x6fc, 4);
            memcpy(&poolOffset, static_cast<const uint8_t *>(shaders[0].control) + 0x700, 4);
            if (poolSize && poolOffset + poolSize <= size)
            {
                p->literals = static_cast<const uint8_t *>(code) + poolOffset;
                p->literalSize = poolSize;
            }
        }
        return 1;
    }
}

void BindProgram(void * cb, ProgramState * p, int stageBits)
{
    (void)cb;
    (void)stageBits;
    if (p)
    {
        g_draw.program[p->stage] = p;
    }
}

// ---- pipelines ----

MTLVertexFormat VertexFormat(int nvn, bool integer)
{
    switch (nvn)
    {
    case 10: return MTLVertexFormatFloat;
    case 22: return MTLVertexFormatFloat2;
    case 34: return MTLVertexFormatFloat3;
    case 46: return MTLVertexFormatFloat4;
    case 5: return MTLVertexFormatHalf;
    case 17: return MTLVertexFormatHalf2;
    case 41: return MTLVertexFormatHalf4;
    case 37: return integer ? MTLVertexFormatUChar4 : MTLVertexFormatUChar4Normalized;
    case 39: return MTLVertexFormatUChar4;
    case 38: return MTLVertexFormatChar4Normalized;
    case 42: return integer ? MTLVertexFormatUShort4 : MTLVertexFormatUShort4Normalized;
    case 43: return MTLVertexFormatShort4Normalized;
    case 44: return MTLVertexFormatUShort4;
    case 45: return MTLVertexFormatShort4;
    case 61: return MTLVertexFormatUInt1010102Normalized;
    case 13: return MTLVertexFormatUChar2Normalized;
    case 18: return MTLVertexFormatUShort2Normalized;
    default: return MTLVertexFormatInvalid;
    }
}

MTLBlendFactor BlendFactor(int f)
{
    switch (f)
    {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 97: return MTLBlendFactorBlendColor;
    case 98: return MTLBlendFactorOneMinusBlendColor;
    case 99: return MTLBlendFactorBlendAlpha;
    case 100: return MTLBlendFactorOneMinusBlendAlpha;
    default: return MTLBlendFactorOne;
    }
}

MTLBlendOperation BlendOp(int op)
{
    switch (op)
    {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

MTLCompareFunction Compare(int f)
{
    switch (f)
    {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

MTLStencilOperation StencilOp(int op)
{
    switch (op)
    {
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

struct PipelineKey {
    void * vertex;
    void * fragment;
    AttribState attribs[16];
    int32_t strides[16];
    BlendDesc blend;
    uint32_t blendEnable, mask;
    uint32_t colorFormat, depthFormat;
};

NSMutableDictionary<NSData *, id> * g_pipelines;
NSMutableDictionary<NSNumber *, id<MTLDepthStencilState>> * g_depthStates;

id<MTLRenderPipelineState> Pipeline()
{
    id<MTLFunction> vertex = (__bridge id<MTLFunction>)g_draw.program[0]->function;
    id<MTLFunction> fragment = (__bridge id<MTLFunction>)g_draw.program[1]->function;
    PipelineKey key;
    memset(&key, 0, sizeof(key));
    key.vertex = g_draw.program[0]->function;
    key.fragment = g_draw.program[1]->function;
    for (MTLVertexAttribute * a in vertex.vertexAttributes)
    {
        NSUInteger i = a.attributeIndex;
        if (a.active && i < 16 && i < static_cast<NSUInteger>(g_draw.attribCount))
        {
            key.attribs[i] = g_draw.attribs[i];
            key.strides[g_draw.attribs[i].stream] = g_draw.streams[g_draw.attribs[i].stream].stride;
        }
    }
    key.blend = g_draw.blend;
    key.blendEnable = g_draw.color.blendEnable & 1;
    key.mask = g_draw.mask.mask & 15;
    key.colorFormat = static_cast<uint32_t>(Metal(g_color).pixelFormat);
    key.depthFormat = static_cast<uint32_t>(Metal(g_depth).pixelFormat);
    if (!g_pipelines)
    {
        g_pipelines = [NSMutableDictionary dictionary];
    }
    NSData * k = [NSData dataWithBytes:&key length:sizeof(key)];
    id known = g_pipelines[k];
    if (known)
    {
        return known == [NSNull null] ? nil : known;
    }
    MTLRenderPipelineDescriptor * d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vertex;
    d.fragmentFunction = fragment;
    MTLVertexDescriptor * layout = [MTLVertexDescriptor vertexDescriptor];
    bool ok = true;
    for (MTLVertexAttribute * a in vertex.vertexAttributes)
    {
        NSUInteger i = a.attributeIndex;
        if (!a.active)
        {
            continue;
        }
        if (i >= static_cast<NSUInteger>(g_draw.attribCount) || !g_draw.attribs[i].set)
        {
            ok = false;     // the shader reads an input the game did not describe
            break;
        }
        bool integer = a.attributeType == MTLDataTypeUInt4 || a.attributeType == MTLDataTypeInt4;
        MTLVertexFormat format = VertexFormat(g_draw.attribs[i].format, integer);
        if (format == MTLVertexFormatInvalid)
        {
            fprintf(stderr, "metal: vertex format %d is not mapped yet\n", g_draw.attribs[i].format);
            ok = false;
            break;
        }
        int stream = g_draw.attribs[i].stream;
        layout.attributes[i].format = format;
        layout.attributes[i].offset = g_draw.attribs[i].offset;
        layout.attributes[i].bufferIndex = 16 + stream;
        layout.layouts[16 + stream].stride = g_draw.streams[stream].stride;
        layout.layouts[16 + stream].stepFunction = MTLVertexStepFunctionPerVertex;
    }
    d.vertexDescriptor = layout;
    if (Metal(g_color))
    {
        MTLRenderPipelineColorAttachmentDescriptor * c = d.colorAttachments[0];
        c.pixelFormat = Metal(g_color).pixelFormat;
        c.blendingEnabled = key.blendEnable != 0;
        c.sourceRGBBlendFactor = BlendFactor(g_draw.blend.srcColor);
        c.destinationRGBBlendFactor = BlendFactor(g_draw.blend.dstColor);
        c.sourceAlphaBlendFactor = BlendFactor(g_draw.blend.srcAlpha);
        c.destinationAlphaBlendFactor = BlendFactor(g_draw.blend.dstAlpha);
        c.rgbBlendOperation = BlendOp(g_draw.blend.opColor);
        c.alphaBlendOperation = BlendOp(g_draw.blend.opAlpha);
        c.writeMask = ((key.mask & 1) ? MTLColorWriteMaskRed : 0) | ((key.mask & 2) ? MTLColorWriteMaskGreen : 0) |
                      ((key.mask & 4) ? MTLColorWriteMaskBlue : 0) | ((key.mask & 8) ? MTLColorWriteMaskAlpha : 0);
    }
    if (id<MTLTexture> depth = Metal(g_depth))
    {
        d.depthAttachmentPixelFormat = depth.pixelFormat;
        if (depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
        {
            d.stencilAttachmentPixelFormat = depth.pixelFormat;
        }
    }
    NSError * error = nil;
    id<MTLRenderPipelineState> state = ok ? [g_device newRenderPipelineStateWithDescriptor:d error:&error] : nil;
    if (ok && !state)
    {
        static int shown;
        if (shown++ < 12)
        {
            fprintf(stderr, "metal: pipeline rejected: %s\n", error.localizedDescription.UTF8String);
        }
    }
    // D3_METAL_PIPELINES=1 lists each new combination of shaders and target, with the
    // cache file names of the shaders, for tracking down a pass that draws wrongly.
    if (getenv("D3_METAL_PIPELINES"))
    {
        fprintf(stderr, "metal: pipeline vs %016llx fs %016llx -> %lux%lu format %d blend %d%s\n", g_draw.program[0]->hash,
                g_draw.program[1]->hash, static_cast<unsigned long>(Metal(g_color).width), static_cast<unsigned long>(Metal(g_color).height),
                g_color ? g_color->desc.format : 0, static_cast<int>(key.blendEnable), state ? "" : "  REJECTED");
    }
    g_pipelines[k] = state ?: [NSNull null];
    return state;
}

id<MTLDepthStencilState> DepthState()
{
    uint64_t packed;
    memcpy(&packed, &g_draw.depth, sizeof(packed));
    if (!g_depthStates)
    {
        g_depthStates = [NSMutableDictionary dictionary];
    }
    id<MTLDepthStencilState> state = g_depthStates[@(packed)];
    if (!state)
    {
        const DepthDesc & s = g_draw.depth;
        MTLDepthStencilDescriptor * d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = s.depthTest ? Compare(s.depthFunc) : MTLCompareFunctionAlways;
        d.depthWriteEnabled = s.depthTest && s.depthWrite;
        if (s.stencilTest)
        {
            MTLStencilDescriptor * front = [MTLStencilDescriptor new];
            front.stencilCompareFunction = Compare(s.frontFunc);
            front.stencilFailureOperation = StencilOp(s.frontFail);
            front.depthFailureOperation = StencilOp(s.frontDepthFail);
            front.depthStencilPassOperation = StencilOp(s.frontPass);
            MTLStencilDescriptor * back = [MTLStencilDescriptor new];
            back.stencilCompareFunction = Compare(s.backFunc);
            back.stencilFailureOperation = StencilOp(s.backFail);
            back.depthFailureOperation = StencilOp(s.backDepthFail);
            back.depthStencilPassOperation = StencilOp(s.backPass);
            d.frontFaceStencil = front;
            d.backFaceStencil = back;
        }
        state = [g_device newDepthStencilStateWithDescriptor:d];
        g_depthStates[@(packed)] = state;
    }
    return state;
}

// ---- drawing ----

// The game's final gamma pass reads the picture and writes it back to the same
// texture, which the Switch's GPU allows when each pixel only reads itself. Metal
// does not, so a draw that samples its own target reads from a copy made just before.
id<MTLTexture> g_feedbackCopy;

id<MTLTexture> CopyOfTarget(id<MTLTexture> target)
{
    if (!g_feedbackCopy || g_feedbackCopy.width != target.width || g_feedbackCopy.height != target.height ||
        g_feedbackCopy.pixelFormat != target.pixelFormat)
    {
        MTLTextureDescriptor * d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:target.pixelFormat
                                                                                      width:target.width
                                                                                     height:target.height
                                                                                  mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        g_feedbackCopy = [g_device newTextureWithDescriptor:d];
    }
    FlushClears();          // the copy must see everything drawn so far
    id<MTLBlitCommandEncoder> blit = [Commands() blitCommandEncoder];
    [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(target.width, target.height, 1)
                toTexture:g_feedbackCopy destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    return g_feedbackCopy;
}

void DrawElementsBaseVertex(void * cb, int primitive, int indexType, int count, uint64_t indices, int baseVertex)
{
    (void)cb;
    d3_nvn_called("nvnCommandBufferDrawElementsBaseVertex");
    @autoreleasepool
    {
        if (!g_draw.program[0] || !g_draw.program[1] || !g_draw.program[0]->function || !g_draw.program[1]->function)
        {
            g_stats[kNoShader]++;
            return;
        }
        MTLPrimitiveType type;
        switch (primitive)
        {
        case 0: type = MTLPrimitiveTypePoint; break;
        case 1: type = MTLPrimitiveTypeLine; break;
        case 3: type = MTLPrimitiveTypeLineStrip; break;
        case 4: type = MTLPrimitiveTypeTriangle; break;
        case 5: type = MTLPrimitiveTypeTriangleStrip; break;
        default: g_stats[kUnsupported]++; return;
        }
        if (indexType != 1 && indexType != 2)
        {
            g_stats[kUnsupported]++;
            return;
        }
        if (!Metal(g_color) && !Metal(g_depth))
        {
            g_stats[kNoTarget]++;
            return;
        }
        id<MTLRenderPipelineState> pipeline = Pipeline();
        if (!pipeline)
        {
            g_stats[kNoPipeline]++;
            return;
        }
        id<MTLTexture> selfCopy = nil;
        for (int stage = 0; stage < 2 && !selfCopy; stage++)
        {
            for (int i = 0; i < 16; i++)
            {
                if (Metal(g_color) && Metal(g_draw.textures[stage][i].texture) == Metal(g_color))
                {
                    selfCopy = CopyOfTarget(Metal(g_color));
                    break;
                }
            }
        }
        id<MTLRenderCommandEncoder> pass = Pass();
        NSUInteger offset = 0;
        id<MTLBuffer> indexBuffer = BufferAt(indices, static_cast<size_t>(count) * (indexType == 1 ? 2 : 4), &offset);
        if (!pass || !indexBuffer)
        {
            g_stats[kNoBuffer]++;
            return;
        }
        NSUInteger indexOffset = offset;

        // Small draws are drawn from a copy of their vertices and indices, taken now.
        // The game rewrites the memory behind its per-frame geometry (cloth, effects,
        // interface) later in the same frame, after recording the draw but before
        // Metal has run it; without the copy such draws pick up the later contents.
        // Large draws are static meshes and are read in place.
        const int kCopyIndexLimit = 6000;
        const size_t kCopyByteLimit = 256 << 10;
        bool copied = false;
        uint32_t maxIndex = 0;
        if (count <= kCopyIndexLimit)
        {
            for (int k = 0; k < count; k++)
            {
                uint32_t idx = indexType == 1 ? reinterpret_cast<const uint16_t *>(indices)[k] : reinterpret_cast<const uint32_t *>(indices)[k];
                maxIndex = idx > maxIndex ? idx : maxIndex;
            }
            copied = baseVertex >= 0;
            // Every stream the shader reads must be small enough, or none is copied:
            // the copies start at the draw's first vertex and the originals do not.
            size_t total = 0;
            id<MTLFunction> vf = (__bridge id<MTLFunction>)g_draw.program[0]->function;
            for (MTLVertexAttribute * a in vf.vertexAttributes)
            {
                if (!a.active || a.attributeIndex >= 16)
                {
                    continue;
                }
                int stream = g_draw.attribs[a.attributeIndex].stream;
                size_t bytes = static_cast<size_t>(g_draw.streams[stream].stride) * (static_cast<size_t>(maxIndex) + 1);
                total += bytes;
                if (!g_draw.streams[stream].stride || bytes > kCopyByteLimit || !g_draw.vertexBuffers[stream].address)
                {
                    copied = false;
                }
            }
            if (g_scratchUsed + total + (static_cast<size_t>(count) * 4) + 4096 > kScratchSize)
            {
                copied = false;
            }
        }

        [pass setRenderPipelineState:pipeline];
        [pass setDepthStencilState:DepthState()];
        [pass setStencilReferenceValue:static_cast<uint32_t>(g_draw.stencilRef)];
        [pass setBlendColorRed:g_draw.blendColor[0] green:g_draw.blendColor[1] blue:g_draw.blendColor[2] alpha:g_draw.blendColor[3]];
        [pass setCullMode:g_draw.polygon.cull == 1 ? MTLCullModeFront : g_draw.polygon.cull == 2 ? MTLCullModeBack : MTLCullModeNone];
        [pass setFrontFacingWinding:g_draw.polygon.frontFace ? MTLWindingCounterClockwise : MTLWindingClockwise];
        [pass setTriangleFillMode:g_draw.polygon.mode == 1 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        if (g_draw.viewportSet)
        {
            MTLViewport v = {static_cast<double>(g_draw.viewport[0]), static_cast<double>(g_draw.viewport[1]),
                             static_cast<double>(g_draw.viewport[2]), static_cast<double>(g_draw.viewport[3]),
                             g_draw.depthRange[0], g_draw.depthRange[1] > g_draw.depthRange[0] ? g_draw.depthRange[1] : 1.0};
            [pass setViewport:v];
        }

        // Vertex data: straight from the game's pools.
        id<MTLFunction> vertex = (__bridge id<MTLFunction>)g_draw.program[0]->function;
        for (MTLVertexAttribute * a in vertex.vertexAttributes)
        {
            if (!a.active || a.attributeIndex >= 16)
            {
                continue;
            }
            int stream = g_draw.attribs[a.attributeIndex].stream;
            if (copied)
            {
                size_t stride = static_cast<size_t>(g_draw.streams[stream].stride);
                const uint8_t * first = reinterpret_cast<const uint8_t *>(g_draw.vertexBuffers[stream].address) + stride * baseVertex;
                id<MTLBuffer> copy = Snapshot(first, stride * (static_cast<size_t>(maxIndex) + 1), &offset);
                [pass setVertexBuffer:copy offset:offset atIndex:16 + stream];
                continue;
            }
            id<MTLBuffer> buffer = BufferAt(g_draw.vertexBuffers[stream].address, 1, &offset);
            if (!buffer)
            {
                g_stats[kNoBuffer]++;
                return;
            }
            [pass setVertexBuffer:buffer offset:offset atIndex:16 + stream];
        }

        // Constant blocks (a copy per draw), literal pools, textures.
        for (int stage = 0; stage < 2; stage++)
        {
            for (int i = 0; i < 14; i++)
            {
                const UniformBinding & u = g_draw.uniforms[stage][i];
                if (!u.address || !u.size)
                {
                    continue;
                }
                id<MTLBuffer> copy = Snapshot(reinterpret_cast<const void *>(u.address), u.size, &offset);
                if (!copy)
                {
                    continue;
                }
                if (stage == 0)
                {
                    [pass setVertexBuffer:copy offset:offset atIndex:i + 3];
                }
                else
                {
                    [pass setFragmentBuffer:copy offset:offset atIndex:i + 3];
                }
            }
            const ProgramState * p = g_draw.program[stage];
            if (p->literals)
            {
                id<MTLBuffer> copy = Snapshot(p->literals, p->literalSize, &offset);
                if (copy && stage == 0)
                {
                    [pass setVertexBuffer:copy offset:offset atIndex:1];
                }
                else if (copy)
                {
                    [pass setFragmentBuffer:copy offset:offset atIndex:1];
                }
            }
            for (int i = 0; i < 16; i++)
            {
                const TextureBinding & t = g_draw.textures[stage][i];
                id<MTLTexture> texture = Metal(t.texture);
                id<MTLSamplerState> sampler = t.sampler && t.sampler->metal ? (__bridge id<MTLSamplerState>)t.sampler->metal : nil;
                if (!texture)
                {
                    continue;
                }
                if (texture == Metal(g_color))
                {
                    texture = selfCopy;     // see CopyOfTarget
                }
                if (stage == 0)
                {
                    [pass setVertexTexture:texture atIndex:i];
                    if (sampler) [pass setVertexSamplerState:sampler atIndex:i];
                }
                else
                {
                    [pass setFragmentTexture:texture atIndex:i];
                    if (sampler) [pass setFragmentSamplerState:sampler atIndex:i];
                }
            }
        }

        if (copied)
        {
            size_t indexBytes = static_cast<size_t>(count) * (indexType == 1 ? 2 : 4);
            id<MTLBuffer> copy = Snapshot(reinterpret_cast<const void *>(indices), indexBytes, &offset);
            if (copy)
            {
                indexBuffer = copy;
                indexOffset = offset;
            }
        }
        [pass drawIndexedPrimitives:type
                         indexCount:count
                          indexType:indexType == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                        indexBuffer:indexBuffer
                  indexBufferOffset:indexOffset
                      instanceCount:1
                         baseVertex:copied ? 0 : baseVertex
                       baseInstance:0];
        g_stats[kDrawn]++;
    }
}

// ---- finishing early ----
// The game asks the queue to finish when it is about to reuse memory that draws
// already recorded still refer to (its vertex buffers for cloth and other per-frame
// geometry are rewritten several times a frame). Nothing is copied here, so those
// draws have to reach the GPU and complete before the game carries on.
void QueueFinish(void * queue)
{
    (void)queue;
    d3_nvn_called("nvnQueueFinish");
    @autoreleasepool
    {
        TextureState * color = g_color;
        TextureState * depth = g_depth;
        FlushClears();
        id<MTLCommandBuffer> done = g_commands;
        g_commands = nil;
        if (done)
        {
            [done commit];
            [done waitUntilCompleted];
        }
        g_color = color;        // the next draw opens a new pass on the same targets
        g_depth = depth;
    }
}

// ---- presenting ----

const char * kPresentSource =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 position [[position]]; float2 uv; };\n"
    "vertex V present_vertex(uint id [[vertex_id]]) {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    V v; v.position = float4(p * 2.0 - 1.0, 0.0, 1.0); v.uv = float2(p.x, 1.0 - p.y); return v;\n"
    "}\n"
    "fragment float4 present_fragment(V v [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
    "    constexpr sampler s(filter::linear);\n"
    "    return float4(t.sample(s, v.uv).rgb, 1.0);\n"
    "}\n";

void SaveScreenshot(id<MTLTexture> texture, const char * path)
{
    NSUInteger w = texture.width, h = texture.height;
    NSMutableData * data = [NSMutableData dataWithLength:w * h * 4];
    [texture getBytes:data.mutableBytes bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    if (texture.pixelFormat == MTLPixelFormatBGRA8Unorm || texture.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB)
    {
        uint8_t * p = static_cast<uint8_t *>(data.mutableBytes);
        for (NSUInteger i = 0; i < w * h; i++)
        {
            uint8_t b = p[i * 4];
            p[i * 4] = p[i * 4 + 2];
            p[i * 4 + 2] = b;
        }
    }
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
    CGImageRef image = CGImageCreate(w, h, 8, 32, w * 4, space, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, provider, nullptr,
                                     false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8 *>(path), strlen(path), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    if (dest)
    {
        CGImageDestinationAddImage(dest, image, nullptr);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
        fprintf(stderr, "metal: frame %u saved to %s (%lux%lu)\n", g_frame, path, static_cast<unsigned long>(w), static_cast<unsigned long>(h));
    }
    CFRelease(url);
    CGImageRelease(image);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(space);
}

// D3_SCREENSHOT=<file.png> saves the frame presented at D3_SCREENSHOT_FRAME (default
// 300). Several frames can be listed, separated by commas; each is then saved as
// <file>-<frame>.png.
void MaybeScreenshot(id<MTLTexture> texture)
{
    static const char * path = getenv("D3_SCREENSHOT");
    static const char * frames = getenv("D3_SCREENSHOT_FRAME") ? getenv("D3_SCREENSHOT_FRAME") : "300";
    if (!path || texture.storageMode == MTLStorageModePrivate)
    {
        return;
    }
    bool many = strchr(frames, ',') != nullptr;
    for (const char * p = frames; *p;)
    {
        char * end;
        unsigned long at = strtoul(p, &end, 10);
        if (end == p)
        {
            break;
        }
        if (at == g_frame)
        {
            char name[1100];
            const char * dot = strrchr(path, '.');
            if (many && dot)
            {
                snprintf(name, sizeof name, "%.*s-%lu%s", static_cast<int>(dot - path), path, at, dot);
            }
            else
            {
                snprintf(name, sizeof name, "%s", path);
            }
            SaveScreenshot(texture, name);
        }
        p = *end == ',' ? end + 1 : end;
    }
}

void PresentTexture(void * queue, WindowDesc * window, int index)
{
    (void)queue;
    d3_nvn_called("nvnQueuePresentTexture");
    @autoreleasepool
    {
        FlushClears();
        g_color = g_depth = nullptr;
        g_frame++;
        g_scratchUsed = 0;
        if (g_frame % 300 == 0 && getenv("D3_METAL_STATS"))
        {
            fprintf(stderr, "metal: frame %u, %u shaders without a translation; last 300 frames:", g_frame, g_missingShaders);
            for (int i = 0; i < kStatCount; i++)
            {
                if (g_stats[i])
                {
                    fprintf(stderr, " [%s: %u]", kStatNames[i], g_stats[i]);
                }
                g_stats[i] = 0;
            }
            fputc('\n', stderr);
        }
        id<MTLTexture> source = index >= 0 && index < window->count ? Metal(window->textures[index]) : nil;
        id<CAMetalDrawable> drawable = source ? [g_layer nextDrawable] : nil;
        if (drawable)
        {
            MTLRenderPassDescriptor * d = [MTLRenderPassDescriptor renderPassDescriptor];
            d.colorAttachments[0].texture = drawable.texture;
            d.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            d.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> pass = [Commands() renderCommandEncoderWithDescriptor:d];
            [pass setRenderPipelineState:g_presentPipeline];
            [pass setFragmentTexture:source atIndex:0];
            [pass drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [pass endEncoding];
            [Commands() presentDrawable:drawable];
        }
        id<MTLCommandBuffer> done = g_commands;
        g_commands = nil;
        if (done)
        {
            // Wait for the GPU to finish the frame. The game writes the next frame's
            // vertices into the same memory the GPU reads from (nothing is copied), and
            // on the Switch presenting holds it back long enough for that to be safe.
            [done commit];
            [done waitUntilCompleted];
            if (getenv("D3_SCREENSHOT") && source)
            {
                MaybeScreenshot(source);
            }
        }
    }
    d3_present_pace();
}

}  // namespace

extern "C" const d3_nvn_entry d3_nvn_metal_overrides[] = {
    {"nvnDeviceInitialize", (d3_nvn_function)DeviceInitialize},
    {"nvnTextureBuilderSetDefaults", (d3_nvn_function)TextureBuilderSetDefaults},
    {"nvnTextureBuilderSetSize3D", (d3_nvn_function)TextureBuilderSetSize3D},
    {"nvnTextureBuilderSetFormat", (d3_nvn_function)TextureBuilderSetFormat},
    {"nvnTextureBuilderSetTarget", (d3_nvn_function)TextureBuilderSetTarget},
    {"nvnTextureBuilderSetFlags", (d3_nvn_function)TextureBuilderSetFlags},
    {"nvnTextureBuilderSetLevels", (d3_nvn_function)TextureBuilderSetLevels},
    {"nvnTextureBuilderSetStorage", (d3_nvn_function)TextureBuilderSetStorage},
    {"nvnTextureBuilderSetStride", (d3_nvn_function)TextureBuilderSetStride},
    {"nvnTextureGetStride", (d3_nvn_function)TextureGetStride},
    {"nvnTextureBuilderSetSwizzle", (d3_nvn_function)TextureBuilderSetSwizzle},
    {"nvnTextureBuilderGetStorageSize", (d3_nvn_function)TextureBuilderGetStorageSize},
    {"nvnTextureBuilderGetStorageAlignment", (d3_nvn_function)TextureBuilderGetStorageAlignment},
    {"nvnTextureInitialize", (d3_nvn_function)TextureInitialize},
    {"nvnTextureFinalize", (d3_nvn_function)TextureFinalize},
    {"nvnTextureGetWidth", (d3_nvn_function)TextureGetWidth},
    {"nvnTextureGetHeight", (d3_nvn_function)TextureGetHeight},
    {"nvnTextureGetFormat", (d3_nvn_function)TextureGetFormat},
    {"nvnTextureFlushTexels", (d3_nvn_function)TextureFlushTexels},
    {"nvnMemoryPoolInitialize", (d3_nvn_function)PoolInitialize},
    {"nvnMemoryPoolFinalize", (d3_nvn_function)PoolFinalize},
    {"nvnSamplerBuilderSetDefaults", (d3_nvn_function)SamplerBuilderSetDefaults},
    {"nvnSamplerBuilderSetMinMagFilter", (d3_nvn_function)SamplerBuilderSetMinMagFilter},
    {"nvnSamplerBuilderSetWrapMode", (d3_nvn_function)SamplerBuilderSetWrapMode},
    {"nvnSamplerInitialize", (d3_nvn_function)SamplerInitialize},
    {"nvnTexturePoolRegisterTexture", (d3_nvn_function)TexturePoolRegisterTexture},
    {"nvnSamplerPoolRegisterSampler", (d3_nvn_function)SamplerPoolRegisterSampler},
    {"nvnDeviceGetTextureHandle", (d3_nvn_function)DeviceGetTextureHandle},
    {"nvnCommandBufferBindTexture", (d3_nvn_function)BindTexture},
    {"nvnVertexAttribStateSetDefaults", (d3_nvn_function)AttribSetDefaults},
    {"nvnVertexAttribStateSetFormat", (d3_nvn_function)AttribSetFormat},
    {"nvnVertexAttribStateSetStreamIndex", (d3_nvn_function)AttribSetStreamIndex},
    {"nvnVertexStreamStateSetDefaults", (d3_nvn_function)StreamSetDefaults},
    {"nvnVertexStreamStateSetStride", (d3_nvn_function)StreamSetStride},
    {"nvnBlendStateSetDefaults", (d3_nvn_function)BlendSetDefaults},
    {"nvnBlendStateSetBlendFunc", (d3_nvn_function)BlendSetBlendFunc},
    {"nvnBlendStateSetBlendEquation", (d3_nvn_function)BlendSetBlendEquation},
    {"nvnColorStateSetDefaults", (d3_nvn_function)ColorSetDefaults},
    {"nvnColorStateSetBlendEnable", (d3_nvn_function)ColorSetBlendEnable},
    {"nvnChannelMaskStateSetDefaults", (d3_nvn_function)MaskSetDefaults},
    {"nvnChannelMaskStateSetChannelMask", (d3_nvn_function)MaskSetChannelMask},
    {"nvnPolygonStateSetDefaults", (d3_nvn_function)PolygonSetDefaults},
    {"nvnPolygonStateSetCullFace", (d3_nvn_function)PolygonSetCullFace},
    {"nvnPolygonStateSetFrontFace", (d3_nvn_function)PolygonSetFrontFace},
    {"nvnPolygonStateSetPolygonMode", (d3_nvn_function)PolygonSetPolygonMode},
    {"nvnDepthStencilStateSetDefaults", (d3_nvn_function)DepthSetDefaults},
    {"nvnDepthStencilStateSetDepthTestEnable", (d3_nvn_function)DepthSetDepthTestEnable},
    {"nvnDepthStencilStateSetDepthWriteEnable", (d3_nvn_function)DepthSetDepthWriteEnable},
    {"nvnDepthStencilStateSetDepthFunc", (d3_nvn_function)DepthSetDepthFunc},
    {"nvnDepthStencilStateSetStencilTestEnable", (d3_nvn_function)DepthSetStencilTestEnable},
    {"nvnDepthStencilStateSetStencilFunc", (d3_nvn_function)DepthSetStencilFunc},
    {"nvnDepthStencilStateSetStencilOp", (d3_nvn_function)DepthSetStencilOp},
    {"nvnCommandBufferBindVertexAttribState", (d3_nvn_function)BindVertexAttribState},
    {"nvnCommandBufferBindVertexStreamState", (d3_nvn_function)BindVertexStreamState},
    {"nvnCommandBufferBindVertexBuffers", (d3_nvn_function)BindVertexBuffers},
    {"nvnCommandBufferBindUniformBuffer", (d3_nvn_function)BindUniformBuffer},
    {"nvnCommandBufferUpdateUniformBuffer", (d3_nvn_function)UpdateUniformBuffer},
    {"nvnCommandBufferBindBlendState", (d3_nvn_function)BindBlendState},
    {"nvnCommandBufferBindColorState", (d3_nvn_function)BindColorState},
    {"nvnCommandBufferBindChannelMaskState", (d3_nvn_function)BindChannelMaskState},
    {"nvnCommandBufferBindPolygonState", (d3_nvn_function)BindPolygonState},
    {"nvnCommandBufferBindDepthStencilState", (d3_nvn_function)BindDepthStencilState},
    {"nvnCommandBufferSetBlendColor", (d3_nvn_function)SetBlendColor},
    {"nvnCommandBufferSetStencilRef", (d3_nvn_function)SetStencilRef},
    {"nvnCommandBufferSetDepthRange", (d3_nvn_function)SetDepthRange},
    {"nvnCommandBufferSetViewport", (d3_nvn_function)SetViewport},
    {"nvnProgramInitialize", (d3_nvn_function)ProgramInitialize},
    {"nvnProgramSetShaders", (d3_nvn_function)ProgramSetShaders},
    {"nvnCommandBufferBindProgram", (d3_nvn_function)BindProgram},
    {"nvnCommandBufferDrawElementsBaseVertex", (d3_nvn_function)DrawElementsBaseVertex},
    {"nvnWindowBuilderSetDefaults", (d3_nvn_function)WindowBuilderSetDefaults},
    {"nvnWindowBuilderSetPresentInterval", (d3_nvn_function)WindowBuilderSetPresentInterval},
    {"nvnWindowBuilderSetTextures", (d3_nvn_function)WindowBuilderSetTextures},
    {"nvnWindowInitialize", (d3_nvn_function)WindowInitialize},
    {"nvnCommandBufferSetRenderTargets", (d3_nvn_function)SetRenderTargets},
    {"nvnCommandBufferClearColor", (d3_nvn_function)ClearColor},
    {"nvnCommandBufferClearDepthStencil", (d3_nvn_function)ClearDepthStencil},
    {"nvnQueuePresentTexture", (d3_nvn_function)PresentTexture},
    {"nvnQueueFinish", (d3_nvn_function)QueueFinish},
    {"nvnQueueFlush", (d3_nvn_function)QueueFinish},
    {nullptr, nullptr},
};

// Sets Metal up on the host window's layer. False when there is no window (headless
// runs), in which case the do-nothing driver handles everything.
extern "C" int d3_nvn_metal_start(void)
{
    @autoreleasepool
    {
        void * layer = d3_window_metal_layer();
        if (!layer)
        {
            return 0;
        }
        g_device = MTLCreateSystemDefaultDevice();
        if (!g_device)
        {
            fprintf(stderr, "metal: no Metal device; nothing will be drawn\n");
            return 0;
        }
        g_queue = [g_device newCommandQueue];
        g_layer = (__bridge CAMetalLayer *)layer;
        g_layer.device = g_device;
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.framebufferOnly = YES;
        g_layer.displaySyncEnabled = NO;       // pacing is done by the driver, as on the Switch

        NSError * error = nil;
        id<MTLLibrary> library = [g_device newLibraryWithSource:@(kPresentSource) options:nil error:&error];
        MTLRenderPipelineDescriptor * p = [MTLRenderPipelineDescriptor new];
        p.vertexFunction = [library newFunctionWithName:@"present_vertex"];
        p.fragmentFunction = [library newFunctionWithName:@"present_fragment"];
        p.colorAttachments[0].pixelFormat = g_layer.pixelFormat;
        g_presentPipeline = library ? [g_device newRenderPipelineStateWithDescriptor:p error:&error] : nil;
        if (!g_presentPipeline)
        {
            fprintf(stderr, "metal: could not build the present pipeline: %s\n", error.localizedDescription.UTF8String);
            return 0;
        }
        return 1;
    }
}
