#include "arena.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <intrin.h>

#include "hook.h"
#include "log.h"

namespace arena {

namespace {

// The game's C runtime (static, inside the game image).
constexpr uintptr_t kMalloc = 0x63A0E5, kFree = 0x637FE8, kCalloc = 0x63A057, kRealloc = 0x63949E,
                    kMsize = 0x6439E3;
// operator new (0x634E7A) calls malloc from here; its own caller's return address is then 6 slots up the stack
// (return, size, 3 locals, saved EBP). Diagnostics only.
constexpr uintptr_t kOperatorNewMallocCall = 0x634E97;
constexpr int kOperatorNewCallerSlot = 6;

constexpr uintptr_t kPreferredBases[] = {0x30000000, 0x38000000, 0x28000000, 0x40000000, 0x20000000};
constexpr size_t kReserve = 256u << 20;
constexpr size_t kCommitStep = 1u << 20;
constexpr uint32_t kMagic = 0xA7E4A7E4;

struct Header {  // 16 bytes in front of every block keeps the payload 16-byte aligned
    uint32_t capacity;
    uint32_t size_class;
    uint32_t magic;
    uint32_t seq;  // allocation number within the duel (diagnostics: the same object on both computers)
};

// Only small blocks (the game's objects) are reused. Bigger ones are mostly images and other presentation buffers,
// which the game often frees on its own clock (after drawing, when a sound ends): reusing them would make the two
// computers' addresses differ, so those are never reused (and from kHeapFrom on, not taken from the arena at all).
constexpr int kSmallClasses = 64;    // 16..1024 bytes in 16-byte steps, reused
constexpr int kLargeClasses = 12;    // powers of two 2 KB .. 4 MB
constexpr int kHugeClass = kSmallClasses + kLargeClasses + 1;  // exact sizes
constexpr size_t kHeapFrom = 64 * 1024;

uintptr_t g_base, g_end, g_committed, g_bump;
volatile LONG g_live;  // blocks currently allocated in the arena (any duel)
void* g_free[kHugeClass];
DWORD g_main_thread;
bool g_routing;
Stats g_stats;

using MallocFn = void*(__cdecl*)(size_t);
using FreeFn = void(__cdecl*)(void*);
using CallocFn = void*(__cdecl*)(size_t, size_t);
using ReallocFn = void*(__cdecl*)(void*, size_t);
using MsizeFn = size_t(__cdecl*)(void*);
MallocFn g_malloc;
FreeFn g_free_fn;
CallocFn g_calloc;
ReallocFn g_realloc;
MsizeFn g_msize;

// Debugging: every arena allocation of one stretch (size and callers), to compare the two computers.
struct RecordEntry {
    uint32_t size;
    void* callers[3];
};
constexpr int kRecordCapacity = 65536;
RecordEntry* g_record;
int g_record_count;

void Note(size_t n, void* caller)
{
    if (!g_record || g_record_count >= kRecordCapacity)
        return;
    RecordEntry& e = g_record[g_record_count++];
    e.size = static_cast<uint32_t>(n);
    memset(e.callers, 0, sizeof e.callers);
    CaptureStackBackTrace(3, 2, e.callers + 1, nullptr);  // further up (unreliable: the game omits frame pointers)
    e.callers[0] = caller;                                 // who called malloc
}

// Debugging: every arena free (thread column 1: inside the synchronized code, reused; 0: outside it on the game
// thread, not reused; else the freeing thread), to compare the two computers: the same block freed inside it on
// one and outside on the other makes their next addresses differ.
struct FreeEntry {
    uint32_t size, tick, thread, seq;
    void* callers[3];
};
constexpr int kFreeRecordCapacity = 1 << 18;
FreeEntry* g_free_record;
int g_free_record_count;
uint32_t g_tick_tag;
uint32_t g_alloc_seq;

bool Routing() { return g_routing && g_base && GetCurrentThreadId() == g_main_thread; }
bool Contains(const void* p)
{
    auto a = reinterpret_cast<uintptr_t>(p);
    return a >= g_base && a < g_end;
}
Header* HeaderOf(void* p) { return reinterpret_cast<Header*>(static_cast<char*>(p) - sizeof(Header)); }

void ClassFor(size_t n, int& cls, uint32_t& capacity)
{
    if (n == 0)
        n = 1;
    if (n <= 1024) {
        capacity = static_cast<uint32_t>((n + 15) & ~size_t(15));
        cls = static_cast<int>(capacity / 16) - 1;
        return;
    }
    for (int i = 0; i < kLargeClasses; ++i) {
        size_t c = size_t(2048) << i;
        if (n <= c) {
            capacity = static_cast<uint32_t>(c);
            cls = kSmallClasses + i;
            return;
        }
    }
    capacity = static_cast<uint32_t>((n + 0xFFFF) & ~size_t(0xFFFF));
    cls = kHugeClass;
}

void* Allocate(size_t n, void* caller)
{
    if (n >= kHeapFrom)
        return nullptr;  // buffers (pixels, sound): their addresses never order anything, so the normal heap
    int cls;
    uint32_t capacity;
    ClassFor(n, cls, capacity);
    Note(n, caller);
    if (cls < kSmallClasses && g_free[cls]) {
        void* p = g_free[cls];
        g_free[cls] = *static_cast<void**>(p);
        ++g_stats.reused;
        HeaderOf(p)->seq = ++g_alloc_seq;
        InterlockedIncrement(&g_live);
        memset(p, 0, capacity);  // a field read before it is written must look the same on both computers
        return p;
    }
    uintptr_t block = g_bump, payload = block + sizeof(Header), next = payload + capacity;
    if (next > g_end)
        return nullptr;  // arena full: caller falls back to the normal heap
    if (next > g_committed) {
        uintptr_t to = (next + kCommitStep - 1) & ~(kCommitStep - 1);
        if (!VirtualAlloc(reinterpret_cast<void*>(g_committed), to - g_committed, MEM_COMMIT, PAGE_READWRITE))
            return nullptr;
        g_committed = to;
    }
    g_bump = next;
    auto* h = reinterpret_cast<Header*>(block);
    h->capacity = capacity;
    h->size_class = cls;
    h->magic = kMagic;
    h->seq = ++g_alloc_seq;
    ++g_stats.allocated;
    InterlockedIncrement(&g_live);
    memset(reinterpret_cast<void*>(payload), 0, capacity);  // reused arena space holds older data
    return reinterpret_cast<void*>(payload);
}

void Release(void* p, void* caller)
{
    Header* h = HeaderOf(p);
    if (h->magic != kMagic) {
        Log("arena: bad free %p", p);
        return;
    }
    InterlockedDecrement(&g_live);
    // Only frees made by the synchronized code itself happen at the same point on both computers; reusing
    // anything else (a finished sound, a closed dialog) would make the next addresses differ.
    bool reuse = Routing() && h->size_class < kSmallClasses;
    if (g_free_record && g_free_record_count < kFreeRecordCapacity) {
        FreeEntry& e = g_free_record[g_free_record_count++];
        e.size = h->capacity;
        e.tick = g_tick_tag;
        e.seq = h->seq;
        e.thread = reuse ? 1 : GetCurrentThreadId() == g_main_thread ? 0 : GetCurrentThreadId();
        memset(e.callers, 0, sizeof e.callers);
        CaptureStackBackTrace(3, 2, e.callers + 1, nullptr);  // further up (unreliable)
        e.callers[0] = caller;                                 // who called free
    }
    if (reuse) {
        *static_cast<void**>(p) = g_free[h->size_class];
        g_free[h->size_class] = p;
    } else {
        InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(&g_stats.leaked_bytes), h->capacity);
    }
}

void* __cdecl HookMalloc(size_t n)
{
    if (Routing()) {
        void* caller = _ReturnAddress();
        if (caller == reinterpret_cast<void*>(kOperatorNewMallocCall))  // report operator new's caller instead
            caller = static_cast<void**>(_AddressOfReturnAddress())[kOperatorNewCallerSlot];
        if (void* p = Allocate(n, caller))
            return p;
    }
    return g_malloc(n);
}

void __cdecl HookFree(void* p)
{
    if (p && Contains(p)) {
        Release(p, _ReturnAddress());
        return;
    }
    g_free_fn(p);
}

void* __cdecl HookCalloc(size_t count, size_t size)
{
    if (Routing() && size && count <= 0x7FFFFFFF / size)
        if (void* p = Allocate(count * size, _ReturnAddress())) {
            memset(p, 0, count * size);
            return p;
        }
    return g_calloc(count, size);
}

void* __cdecl HookRealloc(void* p, size_t n)
{
    if (!p)
        return HookMalloc(n);
    if (!Contains(p) && !Routing())
        return g_realloc(p, n);
    if (n == 0) {
        HookFree(p);
        return nullptr;
    }
    size_t old = Contains(p) ? HeaderOf(p)->capacity : g_msize(p);
    void* q = HookMalloc(n);
    if (!q)
        return nullptr;
    memcpy(q, p, old < n ? old : n);
    HookFree(p);
    return q;
}

size_t __cdecl HookMsize(void* p)
{
    if (p && Contains(p))
        return HeaderOf(p)->capacity;
    return g_msize(p);
}

}  // namespace

void Install()
{
    g_main_thread = GetCurrentThreadId();
    for (uintptr_t want : kPreferredBases) {
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(want), kReserve, MEM_RESERVE, PAGE_NOACCESS)) {
            g_base = reinterpret_cast<uintptr_t>(p);
            break;
        }
    }
    if (!g_base) {
        Log("arena: could not reserve address space; online duels may go out of sync");
        return;
    }
    g_end = g_base + kReserve;
    g_committed = g_bump = g_base;
    SuspendOtherThreads(true);  // another thread could be inside malloc or free while they are patched
    g_malloc = reinterpret_cast<MallocFn>(HookInline(kMalloc, 5, reinterpret_cast<void*>(HookMalloc)));
    g_free_fn = reinterpret_cast<FreeFn>(HookInline(kFree, 7, reinterpret_cast<void*>(HookFree)));
    g_calloc = reinterpret_cast<CallocFn>(HookInline(kCalloc, 8, reinterpret_cast<void*>(HookCalloc)));
    g_realloc = reinterpret_cast<ReallocFn>(HookInline(kRealloc, 7, reinterpret_cast<void*>(HookRealloc)));
    g_msize = reinterpret_cast<MsizeFn>(HookInline(kMsize, 7, reinterpret_cast<void*>(HookMsize)));
    SuspendOtherThreads(false);
    Log("arena: %u MB reserved at %08X", static_cast<unsigned>(kReserve >> 20), static_cast<unsigned>(g_base));
}

uintptr_t BeginDuel(uintptr_t start)
{
    if (!g_base)
        return 0;
    // Each duel gets fresh space (memory from earlier duels may still be in use) and empty free lists. Once
    // everything from earlier duels has been freed, start over at the beginning of the arena.
    Log("arena: new duel; %ld blocks from earlier duels still in use", static_cast<long>(g_live));
    if (g_live == 0)
        g_bump = g_base;
    uintptr_t mine = (g_bump + kCommitStep - 1) & ~(kCommitStep - 1);
    if (start && start >= mine && start < g_end)
        mine = start;
    else if (start)
        Log("arena: host's duel start %08X is not available here, using %08X", static_cast<unsigned>(start),
            static_cast<unsigned>(mine));
    g_bump = mine;
    if (g_bump > g_committed)
        g_committed = g_bump;  // VirtualAlloc commits on demand from here (MEM_COMMIT works on reserved pages)
    memset(g_free, 0, sizeof g_free);
    g_stats = Stats();
    g_stats.start = mine;
    g_alloc_seq = 0;
    return mine;
}

void SetRouting(bool on) { g_routing = on; }

Stats GetStats()
{
    Stats s = g_stats;
    s.used_to = g_bump;
    return s;
}

void StartRecording()
{
    if (!g_record)
        g_record = static_cast<RecordEntry*>(
            VirtualAlloc(nullptr, kRecordCapacity * sizeof(RecordEntry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_record_count = 0;
}

void MarkRecording(uint32_t tag)
{
    if (!g_record || g_record_count >= kRecordCapacity)
        return;
    RecordEntry& e = g_record[g_record_count++];
    e.size = 0xFFFFFFFF;
    e.callers[0] = reinterpret_cast<void*>(static_cast<uintptr_t>(tag));
    e.callers[1] = e.callers[2] = nullptr;
}

bool Recording() { return g_record != nullptr; }

void StartFreeRecording()
{
    if (!g_free_record)
        g_free_record = static_cast<FreeEntry*>(VirtualAlloc(nullptr, kFreeRecordCapacity * sizeof(FreeEntry),
                                                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_free_record_count = 0;
}

void SetTickTag(uint32_t tick) { g_tick_tag = tick; }

void StopFreeRecording(const std::string& path)
{
    if (!g_free_record)
        return;
    if (FILE* f = fopen(path.c_str(), "w")) {
        for (int i = 0; i < g_free_record_count; ++i) {
            const FreeEntry& e = g_free_record[i];
            fprintf(f, "%u %u %u #%u %p %p %p\n", e.tick, e.size, e.thread, e.seq, e.callers[0], e.callers[1],
                    e.callers[2]);
        }
        fclose(f);
    }
    VirtualFree(g_free_record, 0, MEM_RELEASE);
    g_free_record = nullptr;
}

void StopRecording(const std::string& path)
{
    if (!g_record)
        return;
    if (FILE* f = fopen(path.c_str(), "w")) {
        for (int i = 0; i < g_record_count; ++i) {
            const RecordEntry& e = g_record[i];
            if (e.size == 0xFFFFFFFF)
                fprintf(f, "-- %u\n", static_cast<unsigned>(reinterpret_cast<uintptr_t>(e.callers[0])));
            else
                fprintf(f, "%u %p %p %p\n", e.size, e.callers[0], e.callers[1], e.callers[2]);
        }
        fclose(f);
    }
    VirtualFree(g_record, 0, MEM_RELEASE);
    g_record = nullptr;
}

}  // namespace arena
