// The do-nothing graphics driver: the loader the game calls, and the few entry
// points that have to do more than nothing for the game to keep going.
#include "nvn_null.h"

#include "host/window.h"
#include "nn/nn_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// D3_NVN_TRACE=all prints every driver call. D3_NVN_TRACE=frames prints one line
// per 60 frames the game presents, which is the quickest way to see that it is alive.
// D3_NVN_TRACE=new prints each entry point the first time it is called: with the
// Metal driver active these are the calls that still do nothing.
void d3_nvn_called(const char * name)
{
    static int trace = -1;
    static unsigned frames;
    if (trace < 0)
    {
        const char * mode = getenv("D3_NVN_TRACE");
        trace = !mode ? 0 : strcmp(mode, "frames") == 0 ? 2 : strcmp(mode, "new") == 0 ? 3 : 1;
    }
    if (trace == 1)
    {
        fprintf(stderr, "nvn: %s\n", name);
    }
    else if (trace == 3)
    {
        static const char * seen[512];
        static int count;
        for (int i = 0; i < count; i++)
        {
            if (seen[i] == name)
            {
                return;
            }
        }
        if (count < 512)
        {
            seen[count++] = name;
            fprintf(stderr, "nvn: first call of %s\n", name);
        }
    }
    else if (trace == 2 && strcmp(name, "nvnQueuePresentTexture") == 0 && ++frames % 60 == 0)
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        fprintf(stderr, "nvn: %u frames presented at %ld.%03ld s\n", frames, static_cast<long>(ts.tv_sec), ts.tv_nsec / 1000000);
    }
}

static bool g_metal;

static d3_nvn_function Lookup(const void * device, const char * name)
{
    (void)device;
    for (const d3_nvn_entry * e = d3_nvn_metal_overrides; g_metal && e->name; e++)
    {
        if (strcmp(e->name, name) == 0)
        {
            return e->function;
        }
    }
    for (const d3_nvn_entry * e = d3_nvn_overrides; e->name; e++)
    {
        if (strcmp(e->name, name) == 0)
        {
            return e->function;
        }
    }
    for (const d3_nvn_entry * e = d3_nvn_defaults; e->name; e++)
    {
        if (strcmp(e->name, name) == 0)
        {
            return e->function;
        }
    }
    return nullptr;
}

// The game asks which of the window's textures to draw the next frame into, and
// uses the answer as an index. Always the first; the result 0 means success.
static int AcquireFromWindow(void * window, void * sync, int * textureIndex)
{
    (void)window;
    (void)sync;
    d3_nvn_called("nvnWindowAcquireTexture");
    *textureIndex = 0;
    return 0;
}

static int AcquireFromQueue(void * queue, void * window, int * textureIndex)
{
    (void)queue;
    (void)window;
    d3_nvn_called("nvnQueueAcquireTexture");
    *textureIndex = 0;
    return 0;
}

// ---- memory pools and buffers ----
// The game supplies the memory for every pool and reads and writes it directly. The
// driver only has to remember where each pool and buffer is, and use the memory's own
// address as its "GPU address", so that an address the game hands back can be used.
// The state lives at the start of the game's own objects, which are far larger
// (builders 64 bytes, pools 256, buffers 48).

struct PoolState { void * memory; size_t size; };
struct BufferState { PoolState * pool; ptrdiff_t offset; size_t size; };

static void PoolBuilderSetDefaults(PoolState * builder) { builder->memory = nullptr; builder->size = 0; }
static void PoolBuilderSetStorage(PoolState * builder, void * memory, size_t size) { builder->memory = memory; builder->size = size; }
static void * PoolBuilderGetMemory(const PoolState * builder) { return builder->memory; }
static size_t PoolBuilderGetSize(const PoolState * builder) { return builder->size; }
static int PoolInitialize(PoolState * pool, const PoolState * builder) { *pool = *builder; return 1; }
static void * PoolMap(const PoolState * pool) { return pool->memory; }
static size_t PoolGetSize(const PoolState * pool) { return pool->size; }
static uint64_t PoolGetBufferAddress(const PoolState * pool) { return reinterpret_cast<uint64_t>(pool->memory); }

static void BufferBuilderSetDefaults(BufferState * builder) { builder->pool = nullptr; builder->offset = 0; builder->size = 0; }
static void BufferBuilderSetStorage(BufferState * builder, PoolState * pool, ptrdiff_t offset, size_t size)
{
    builder->pool = pool;
    builder->offset = offset;
    builder->size = size;
}
static BufferState g_lastBuffer;      // for the shader dump below
static int BufferInitialize(BufferState * buffer, const BufferState * builder) { *buffer = *builder; g_lastBuffer = *builder; return 1; }
static void * BufferMap(const BufferState * buffer)
{
    return buffer->pool ? static_cast<char *>(buffer->pool->memory) + buffer->offset : nullptr;
}
static uint64_t BufferGetAddress(const BufferState * buffer) { return reinterpret_cast<uint64_t>(BufferMap(buffer)); }
static ptrdiff_t BufferGetMemoryOffset(const BufferState * buffer) { return buffer->offset; }
static PoolState * BufferGetMemoryPool(const BufferState * buffer) { return buffer->pool; }
static size_t BufferGetSize(const BufferState * buffer) { return buffer->size; }

void d3_nvn_last_buffer(void ** memory, size_t * size)
{
    *memory = BufferMap(&g_lastBuffer);
    *size = g_lastBuffer.size;
}

// ---- shaders ----
// The driver compiles nothing. For studying the game's shaders, D3_SHADER_DUMP=<folder>
// writes each one as it is handed over: NNNN.code is the GPU program (its size is that
// of the buffer the game just created for it), NNNN.control the first 4 KB of the
// accompanying control block, whose real size the driver is not told.

struct ShaderData { uint64_t data; const void * control; };

static int ProgramSetShaders(void * program, int count, const ShaderData * shaders)
{
    static const char * folder = getenv("D3_SHADER_DUMP");
    static int serial;
    (void)program;
    d3_nvn_called("nvnProgramSetShaders");
    for (int i = 0; folder && i < count; i++)
    {
        char path[1024];
        int n = __atomic_fetch_add(&serial, 1, __ATOMIC_RELAXED);
        const void * code = reinterpret_cast<const void *>(shaders[i].data);
        if (code && code == BufferMap(&g_lastBuffer))
        {
            snprintf(path, sizeof path, "%s/%04d.code", folder, n);
            if (FILE * f = fopen(path, "wb"))
            {
                fwrite(code, 1, g_lastBuffer.size, f);
                fclose(f);
            }
        }
        if (shaders[i].control)
        {
            snprintf(path, sizeof path, "%s/%04d.control", folder, n);
            if (FILE * f = fopen(path, "wb"))
            {
                fwrite(shaders[i].control, 1, 4096, f);
                fclose(f);
            }
        }
    }
    return 1;
}

// ---- counters ----
// The game marks a counter "in progress" and waits for the GPU to write the result.
// With no GPU, the result is written the moment it is requested: value 0, and the
// current time.

struct CounterData { uint64_t counter; uint64_t timestamp; };

static uint64_t NowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void ReportCounter(void * commandBuffer, int counterType, uint64_t address)
{
    (void)commandBuffer;
    (void)counterType;
    if (address)
    {
        CounterData * data = reinterpret_cast<CounterData *>(address);
        data->timestamp = NowNs();
        data->counter = 0;
    }
}

static uint64_t GetTimestampInNanoseconds(const void * device, const CounterData * data) { (void)device; return data->timestamp; }
static uint64_t GetCurrentTimestampInNanoseconds(const void * device) { (void)device; return NowNs(); }

// ---- events ----
// A number the GPU updates when it reaches a point in the command stream. The game
// uses one as a count of finished frames and waits for it to catch up. With no GPU,
// a signal takes effect the moment it is recorded.

struct EventState { uint32_t value; };

static void ApplySignal(EventState * event, int mode, uint32_t value)
{
    uint32_t old = __atomic_load_n(&event->value, __ATOMIC_RELAXED);
    __atomic_store_n(&event->value, mode == 0 ? value : mode == 1 ? old + value : old, __ATOMIC_RELEASE);
}

static int EventInitialize(EventState * event, const void * builder) { (void)builder; event->value = 0; return 1; }
static uint32_t EventGetValue(const EventState * event) { return __atomic_load_n(&event->value, __ATOMIC_ACQUIRE); }
static void EventSignal(EventState * event, int mode, uint32_t value) { ApplySignal(event, mode, value); }

static void CommandBufferSignalEvent(void * commandBuffer, const EventState * event, int mode, int location, int flags, uint32_t value)
{
    (void)commandBuffer;
    (void)location;
    (void)flags;
    ApplySignal(const_cast<EventState *>(event), mode, value);
}

// ---- presenting ----
// On the Switch, presenting a frame waits for the display, which is what paces the
// game. With nothing to display, wait here instead: 60 frames a second divided by
// the interval the game asked for.

static int g_presentInterval = 1;

static void SetPresentInterval(void * window, int interval)
{
    (void)window;
    g_presentInterval = interval < 1 ? 1 : interval;
}

void d3_present_pace(void)
{
    static uint64_t next;
    uint64_t now = NowNs();
    uint64_t frame = 1000000000ull / 60 * g_presentInterval;
    if (next < now || next > now + frame)
    {
        next = now;                 // first frame, or running behind: do not try to catch up
    }
    else
    {
        timespec wait = {0, static_cast<long>(next - now)};
        nanosleep(&wait, nullptr);
    }
    next += frame;
}

static void PresentTexture(void * queue, void * window, int textureIndex)
{
    (void)queue;
    (void)window;
    (void)textureIndex;
    d3_nvn_called("nvnQueuePresentTexture");
    d3_present_pace();
}

const d3_nvn_entry d3_nvn_overrides[] = {
    {"nvnDeviceGetProcAddress", (d3_nvn_function)Lookup},
    {"nvnProgramSetShaders", (d3_nvn_function)ProgramSetShaders},
    {"nvnWindowSetPresentInterval", (d3_nvn_function)SetPresentInterval},
    {"nvnQueuePresentTexture", (d3_nvn_function)PresentTexture},
    {"nvnEventInitialize", (d3_nvn_function)EventInitialize},
    {"nvnEventGetValue", (d3_nvn_function)EventGetValue},
    {"nvnEventSignal", (d3_nvn_function)EventSignal},
    {"nvnCommandBufferSignalEvent", (d3_nvn_function)CommandBufferSignalEvent},
    {"nvnMemoryPoolBuilderSetDefaults", (d3_nvn_function)PoolBuilderSetDefaults},
    {"nvnMemoryPoolBuilderSetStorage", (d3_nvn_function)PoolBuilderSetStorage},
    {"nvnMemoryPoolBuilderGetMemory", (d3_nvn_function)PoolBuilderGetMemory},
    {"nvnMemoryPoolBuilderGetSize", (d3_nvn_function)PoolBuilderGetSize},
    {"nvnMemoryPoolInitialize", (d3_nvn_function)PoolInitialize},
    {"nvnMemoryPoolMap", (d3_nvn_function)PoolMap},
    {"nvnMemoryPoolGetSize", (d3_nvn_function)PoolGetSize},
    {"nvnMemoryPoolGetBufferAddress", (d3_nvn_function)PoolGetBufferAddress},
    {"nvnBufferBuilderSetDefaults", (d3_nvn_function)BufferBuilderSetDefaults},
    {"nvnBufferBuilderSetStorage", (d3_nvn_function)BufferBuilderSetStorage},
    {"nvnBufferInitialize", (d3_nvn_function)BufferInitialize},
    {"nvnBufferMap", (d3_nvn_function)BufferMap},
    {"nvnBufferGetAddress", (d3_nvn_function)BufferGetAddress},
    {"nvnBufferGetMemoryOffset", (d3_nvn_function)BufferGetMemoryOffset},
    {"nvnBufferGetMemoryPool", (d3_nvn_function)BufferGetMemoryPool},
    {"nvnBufferGetSize", (d3_nvn_function)BufferGetSize},
    {"nvnCommandBufferReportCounter", (d3_nvn_function)ReportCounter},
    {"nvnDeviceGetTimestampInNanoseconds", (d3_nvn_function)GetTimestampInNanoseconds},
    {"nvnDeviceGetCurrentTimestampInNanoseconds", (d3_nvn_function)GetCurrentTimestampInNanoseconds},
    {"nvnWindowAcquireTexture", (d3_nvn_function)AcquireFromWindow},
    {"nvnQueueAcquireTexture", (d3_nvn_function)AcquireFromQueue},
    {nullptr, nullptr},
};

// The game's first call into the driver: it asks for the lookup function by name.
extern "C" d3_nvn_function nvnBootstrapLoader(const char * name)
{
    static bool started;
    if (!started)
    {
        started = true;
        g_metal = d3_nvn_metal_start() != 0;
    }
    return Lookup(nullptr, name);
}

// The driver's memory set-up. The do-nothing driver allocates nothing.
namespace nv {

void SetGraphicsAllocator(void * (*)(unsigned long, unsigned long, void *), void (*)(void *, void *),
                          void * (*)(void *, unsigned long, void *), void *)
{
}

void InitializeGraphics(void *, unsigned long)
{
}

}  // namespace nv

// nn::vi: the display and the layer the game draws to. Creating the layer opens the
// host window; the handles themselves are placeholders that only have to be non-null.
namespace nn { namespace vi {

class Display;
class Layer;
enum ScalingMode { ScalingMode_None };

static char g_display, g_layer, g_window;

void Initialize()
{
}

Result OpenDefaultDisplay(Display ** out)
{
    *out = reinterpret_cast<Display *>(&g_display);
    return ResultSuccess();
}

Result CreateLayer(Layer ** out, Display *)
{
    d3_window_create();
    *out = reinterpret_cast<Layer *>(&g_layer);
    return ResultSuccess();
}

Result SetLayerScalingMode(Layer *, ScalingMode)
{
    return ResultSuccess();
}

Result GetNativeWindow(void ** out, Layer *)
{
    *out = &g_window;
    return ResultSuccess();
}

}}  // namespace nn::vi
