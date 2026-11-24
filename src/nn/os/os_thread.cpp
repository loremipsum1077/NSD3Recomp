// nn::os threads, thread-local slots, time, memory and process information.
#include "os_types.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <atomic>
#include <mutex>
#include <pthread.h>

namespace {

const int kTlsSlots = 32;
void (*g_tlsDestructors[kTlsSlots])(unsigned long);
bool g_tlsUsed[kTlsSlots];
std::mutex g_tlsMutex;

nn::os::ThreadType g_mainThread;
thread_local nn::os::ThreadType * t_current;
thread_local unsigned long t_tls[kTlsSlots];

int g_argc;
char ** g_argv;

// The game calls into this file from its static initialisers, which can run before
// this file's own. Anything with a constructor is therefore created on first use.
std::mutex g_regionMutex;
std::map<unsigned long, size_t> & Regions()
{
    static std::map<unsigned long, size_t> * regions = new std::map<unsigned long, size_t>;
    return *regions;
}

void * ThreadEntry(void * p)
{
    nn::os::ThreadType * thread = static_cast<nn::os::ThreadType *>(p);
    t_current = thread;
#ifdef __APPLE__
    pthread_setname_np(thread->namePointer ? thread->namePointer : thread->name);
#endif
    thread->function(thread->argument);
    for (int i = 0; i < kTlsSlots; i++)
    {
        if (g_tlsDestructors[i] && t_tls[i])
        {
            g_tlsDestructors[i](t_tls[i]);
        }
    }
    return nullptr;
}

int64_t NowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

}  // namespace

void d3_set_host_args(int argc, char ** argv)
{
    g_argc = argc;
    g_argv = argv;
}

namespace nn { namespace os {

// ---- threads ----

ThreadType * GetCurrentThread()
{
    if (!t_current)
    {
        // The thread that entered main is the game's main thread. Any other thread the
        // host made (the sound loader, SDL's audio thread) needs an identity of its own:
        // sharing the main thread's made a recursive mutex held by the main thread look
        // already owned by the caller, so both entered the game's heap at once.
        static std::atomic<bool> mainClaimed{false};
#ifdef __APPLE__
        bool isMain = pthread_main_np() != 0;
#else
        bool isMain = !mainClaimed.exchange(true);
#endif
        if (isMain)
        {
            mainClaimed = true;
            t_current = &g_mainThread;
        }
        else
        {
            static thread_local ThreadType hostThread;
            hostThread.handle = pthread_self();
            hostThread.state = 2;
            t_current = &hostThread;
        }
    }
    return t_current;
}

// The game supplies its own stack memory. It is not used: host stacks have alignment
// and minimum-size rules the Switch's do not. The requested size is honoured.
Result CreateThread(ThreadType * thread, void (*function)(void *), void * argument, void *, unsigned long stackSize, int priority)
{
    memset(thread, 0, sizeof(*thread));
    thread->function = function;
    thread->argument = argument;
    thread->stackSize = stackSize;
    thread->priority = priority;
    thread->state = 1;
    return ResultSuccess();
}

Result CreateThread(ThreadType * thread, void (*function)(void *), void * argument, void * stack, unsigned long stackSize, int priority, int)
{
    return CreateThread(thread, function, argument, stack, stackSize, priority);
}

void StartThread(ThreadType * thread)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    size_t size = thread->stackSize < 0x80000 ? 0x80000 : thread->stackSize;
    size = (size + 0xffff) & ~static_cast<size_t>(0xffff);
    pthread_attr_setstacksize(&attr, size);
    thread->state = 2;
    if (pthread_create(&thread->handle, &attr, ThreadEntry, thread) != 0)
    {
        fprintf(stderr, "nn::os::StartThread: the host could not create a thread\n");
        abort();
    }
    pthread_attr_destroy(&attr);
}

void WaitThread(ThreadType * thread)
{
    if (thread->state == 2)
    {
        pthread_join(thread->handle, nullptr);
        thread->state = 3;
    }
}

void DestroyThread(ThreadType * thread)
{
    WaitThread(thread);
    thread->state = 0;
}

void SetThreadName(ThreadType * thread, const char * name)
{
    strncpy(thread->name, name ? name : "", sizeof(thread->name) - 1);
    thread->namePointer = thread->name;
}

// Priorities and core assignment are left to the host's scheduler.
int ChangeThreadPriority(ThreadType * thread, int priority)
{
    int old = thread->priority;
    thread->priority = priority;
    return old;
}

void SetThreadCoreMask(ThreadType *, int, unsigned long)
{
}

void SleepThread(TimeSpan span)
{
    if (span.ns <= 0)
    {
        return;
    }
    timespec ts = {static_cast<time_t>(span.ns / 1000000000), static_cast<long>(span.ns % 1000000000)};
    while (nanosleep(&ts, &ts) != 0)
    {
    }
}

void YieldThread()
{
    sched_yield();
}

// ---- thread-local slots ----

Result AllocateTlsSlot(TlsSlot * slot, void (*destructor)(unsigned long))
{
    std::lock_guard<std::mutex> lock(g_tlsMutex);
    for (int i = 0; i < kTlsSlots; i++)
    {
        if (!g_tlsUsed[i])
        {
            g_tlsUsed[i] = true;
            g_tlsDestructors[i] = destructor;
            slot->value = i;
            return ResultSuccess();
        }
    }
    fprintf(stderr, "nn::os::AllocateTlsSlot: all %d slots are in use\n", kTlsSlots);
    abort();
}

void FreeTlsSlot(TlsSlot slot)
{
    std::lock_guard<std::mutex> lock(g_tlsMutex);
    g_tlsUsed[slot.value] = false;
    g_tlsDestructors[slot.value] = nullptr;
}

unsigned long GetTlsValue(TlsSlot slot)
{
    return t_tls[slot.value];
}

void SetTlsValue(TlsSlot slot, unsigned long value)
{
    t_tls[slot.value] = value;
}

void SetMemoryAllocatorForThreadLocal(void * (*)(unsigned long, unsigned long), void (*)(void *, unsigned long))
{
}

// ---- time: one tick is one nanosecond ----

Tick GetSystemTick()
{
    return Tick{NowNs()};
}

long GetSystemTickFrequency()
{
    return 1000000000;
}

TimeSpan ConvertToTimeSpan(Tick tick)
{
    TimeSpan span;
    span.ns = tick.value;
    return span;
}

// ---- memory ----
// The game reserves address ranges and then asks for pages inside them. The host
// hands out memory on first touch, so a reservation is already usable and the
// page calls have nothing to do.

Result SetMemoryHeapSize(unsigned long)
{
    return ResultSuccess();
}

Result AllocateAddressRegion(unsigned long * address, unsigned long size)
{
    void * p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
    {
        fprintf(stderr, "nn::os::AllocateAddressRegion: the host refused %lu bytes\n", size);
        abort();
    }
    std::lock_guard<std::mutex> lock(g_regionMutex);
    Regions()[reinterpret_cast<unsigned long>(p)] = size;
    *address = reinterpret_cast<unsigned long>(p);
    return ResultSuccess();
}

void FreeAddressRegion(unsigned long address)
{
    std::lock_guard<std::mutex> lock(g_regionMutex);
    auto it = Regions().find(address);
    if (it != Regions().end())
    {
        munmap(reinterpret_cast<void *>(address), it->second);
        Regions().erase(it);
    }
}

Result AllocateMemoryPages(unsigned long, unsigned long)
{
    return ResultSuccess();
}

Result FreeMemoryPages(unsigned long, unsigned long)
{
    return ResultSuccess();
}

// The Switch gives the game about 3.2 GB. Report that, so memory budgets the game
// derives from it stay what they were. Usage figures are not tracked yet.
void QueryMemoryInfo(MemoryInfo * info)
{
    memset(info, 0, sizeof(*info));
    info->totalAvailableMemorySize = 0xCD000000ull;
}

// ---- process ----

int GetHostArgc()
{
    return g_argc;
}

char ** GetHostArgv()
{
    return g_argv;
}

// Crashes are left to the host's own crash handling for now.
void SetUserExceptionHandler(void (*)(UserExceptionInfo *), void *, unsigned long, UserExceptionInfo *)
{
}

}}  // namespace nn::os

// The SDK's C spellings (used by curl).
extern "C" {
uint32_t nnosCreateThread(nn::os::ThreadType * t, void (*f)(void *), void * arg, void * stack, size_t size, int priority) { return nn::os::CreateThread(t, f, arg, stack, size, priority).value; }
void nnosStartThread(nn::os::ThreadType * t) { nn::os::StartThread(t); }
void nnosWaitThread(nn::os::ThreadType * t) { nn::os::WaitThread(t); }
void nnosDestroyThread(nn::os::ThreadType * t) { nn::os::DestroyThread(t); }
nn::os::ThreadType * nnosGetCurrentThread(void) { return nn::os::GetCurrentThread(); }
int nnosGetThreadPriority(const nn::os::ThreadType * t) { return t->priority; }
void nnosSetThreadNamePointer(nn::os::ThreadType * t, const char * name) { t->namePointer = name; }
void nnosSleepThread(int64_t ns) { nn::TimeSpan s; s.ns = ns; nn::os::SleepThread(s); }
int64_t nnosGetSystemTick(void) { return nn::os::GetSystemTick().value; }
int64_t nnosGetSystemTickFrequency(void) { return nn::os::GetSystemTickFrequency(); }
int64_t nnosConvertTickToNanoSeconds(int64_t tick) { return tick; }
}
