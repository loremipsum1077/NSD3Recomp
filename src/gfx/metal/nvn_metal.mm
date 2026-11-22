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
// Done so far: device, window, textures as render targets, clears, presenting to
// the window, screenshots. Not yet: draws, shaders, texture contents, samplers,
// blend/depth/polygon state. Until then the window shows the game's clear colours.
#include "../null/nvn_null.h"
#include "host/window.h"

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

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

struct PoolState { void * memory; size_t size; };       // as in the do-nothing driver

struct TextureDesc {                // TextureBuilder is 0x80 bytes
    int width, height, depth;
    int format, target, flags, levels;
    PoolState * pool;
    ptrdiff_t offset;
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
}

void TextureBuilderSetSize3D(TextureDesc * b, int w, int h, int d) { b->width = w; b->height = h; b->depth = d; }
void TextureBuilderSetFormat(TextureDesc * b, int format) { b->format = format; }
void TextureBuilderSetTarget(TextureDesc * b, int target) { b->target = target; }
void TextureBuilderSetFlags(TextureDesc * b, int flags) { b->flags = flags; }
void TextureBuilderSetLevels(TextureDesc * b, int levels) { b->levels = levels < 1 ? 1 : levels; }
void TextureBuilderSetStorage(TextureDesc * b, PoolState * pool, ptrdiff_t offset) { b->pool = pool; b->offset = offset; }

size_t AlignUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

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
    int layers = b->target == 8 ? 6 : b->target == 2 ? 1 : b->depth > 1 && b->target == 4 ? b->depth : 1;
    size_t total = 0;
    for (int level = 0; level < b->levels; level++)
    {
        int w = b->width >> level ? b->width >> level : 1;
        int h = b->height >> level ? b->height >> level : 1;
        int d = b->target == 2 ? (b->depth >> level ? b->depth >> level : 1) : 1;
        total += LevelSize(f, w, h, layers * d);
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

// D3_SCREENSHOT=<file.png> saves the frame presented at D3_SCREENSHOT_FRAME (default 300).
void MaybeScreenshot(id<MTLTexture> texture)
{
    static const char * path = getenv("D3_SCREENSHOT");
    static unsigned at = getenv("D3_SCREENSHOT_FRAME") ? static_cast<unsigned>(atoi(getenv("D3_SCREENSHOT_FRAME"))) : 300;
    if (path && g_frame == at && texture.storageMode != MTLStorageModePrivate)
    {
        SaveScreenshot(texture, path);
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
            [done commit];
            if (getenv("D3_SCREENSHOT") && source)
            {
                [done waitUntilCompleted];
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
    {"nvnTextureBuilderGetStorageSize", (d3_nvn_function)TextureBuilderGetStorageSize},
    {"nvnTextureBuilderGetStorageAlignment", (d3_nvn_function)TextureBuilderGetStorageAlignment},
    {"nvnTextureInitialize", (d3_nvn_function)TextureInitialize},
    {"nvnTextureFinalize", (d3_nvn_function)TextureFinalize},
    {"nvnTextureGetWidth", (d3_nvn_function)TextureGetWidth},
    {"nvnTextureGetHeight", (d3_nvn_function)TextureGetHeight},
    {"nvnTextureGetFormat", (d3_nvn_function)TextureGetFormat},
    {"nvnWindowBuilderSetDefaults", (d3_nvn_function)WindowBuilderSetDefaults},
    {"nvnWindowBuilderSetPresentInterval", (d3_nvn_function)WindowBuilderSetPresentInterval},
    {"nvnWindowBuilderSetTextures", (d3_nvn_function)WindowBuilderSetTextures},
    {"nvnWindowInitialize", (d3_nvn_function)WindowInitialize},
    {"nvnCommandBufferSetRenderTargets", (d3_nvn_function)SetRenderTargets},
    {"nvnCommandBufferClearColor", (d3_nvn_function)ClearColor},
    {"nvnCommandBufferClearDepthStencil", (d3_nvn_function)ClearDepthStencil},
    {"nvnQueuePresentTexture", (d3_nvn_function)PresentTexture},
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
