// Layouts the platform layer keeps inside the game's nn::os objects. See nn_types.h.
#pragma once
#include "nn/nn_types.h"
#include <pthread.h>

namespace nn { namespace os {

typedef void (*ThreadFunction)(void *);
enum EventClearMode { EventClearMode_ManualClear = 0, EventClearMode_AutoClear = 1 };

struct ThreadType {                 // game size 0x1c0
    ThreadFunction function;
    void * argument;
    size_t stackSize;
    int priority;
    uint8_t state;                  // 0 none, 1 created, 2 started, 3 finished and joined
    pthread_t handle;
    const char * namePointer;
    char name[32];
    uint8_t reserved[0x1c0 - 0x50];
};
static_assert(sizeof(ThreadType) == 0x1c0, "ThreadType");

struct MutexType {                  // game size 0x20
    ThreadType * owner;
    int nestCount;
    bool recursive;
    uint8_t reserved[0x20 - 0x10];
};
static_assert(sizeof(MutexType) == 0x20, "MutexType");

struct EventType {                  // game size 0x28
    bool signaled;
    bool autoClear;
    uint8_t reserved[0x28 - 2];
};
static_assert(sizeof(EventType) == 0x28, "EventType");

struct SystemEventType {            // game size 0x30; starts with an EventType
    EventType event;
    uint8_t reserved[0x30 - 0x28];
};
static_assert(sizeof(SystemEventType) == 0x30, "SystemEventType");

struct LightEventType {             // game size 0xc
    bool signaled;
    bool autoClear;
    uint8_t reserved[0xc - 2];
};
static_assert(sizeof(LightEventType) == 0xc, "LightEventType");

struct SemaphoreType {              // game size 0x28
    int count;
    int maxCount;
    uint8_t reserved[0x28 - 8];
};
static_assert(sizeof(SemaphoreType) == 0x28, "SemaphoreType");

struct ReaderWriterLockType {       // game size 0x30
    ThreadType * writer;
    int writeNest;
    int readers;
    uint8_t reserved[0x30 - 0x10];
};
static_assert(sizeof(ReaderWriterLockType) == 0x30, "ReaderWriterLockType");

struct TlsSlot { uint32_t value; };
struct Tick { int64_t value; };

struct MemoryInfo {                 // game size 0x38
    uint64_t totalAvailableMemorySize;
    size_t totalUsedMemorySize;
    size_t totalMemoryHeapSize;
    size_t allocatedMemoryHeapSize;
    size_t programSize;
    size_t totalThreadStackSize;
    int threadCount;
};
static_assert(sizeof(MemoryInfo) == 0x38, "MemoryInfo");

struct UserExceptionInfo;

ThreadType * GetCurrentThread();

}}  // namespace nn::os

// Set by the host's main before the game starts.
void d3_set_host_args(int argc, char ** argv);
