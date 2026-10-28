#include "host_context.h"

#if defined(MKW_HOST_CONTEXT_THREADS)
#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#if defined(__EMSCRIPTEN__)
#include <cmath>

#include <emscripten/threading.h>
#endif
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__) && defined(__aarch64__)
#include <sys/mman.h>
#include <unistd.h>

extern "C" void mkw_co_switch(void** targetSp, void** sourceSp);
extern "C" void* mkw_co_init(void* stackTop, void (*entry)(void*), void* argument);
#elif defined(__linux__) || (defined(__APPLE__) && defined(__x86_64__))
#include <libco.h>

#include <cstdlib>
#include <unordered_map>
#else
#error "HostContext needs a supported cooperative-context backend"
#endif

namespace HostContext {

#if defined(MKW_HOST_CONTEXT_THREADS)

namespace {
// Contexts are never freed: a context's thread can still be inside notify_one() on another
// context when that one is destroyed, and a Context is a few dozen bytes. Threads, which are what
// actually cost (one Web Worker and its stack each on the web), do exit where they can.
struct Context {
    Entry entry = nullptr;
    void* argument = nullptr;
    // The baton (0 or 1, a 32-bit word so it can be a futex). A thread waits for its own flag to
    // become 1; whoever sets it hands over all memory the previous holder wrote (release/acquire).
    std::atomic<uint32_t> running{0};
    // Set while the thread is (about to be) blocked on `running`, so a handoff only pays for a
    // wake when one is needed: a context still spinning picks the baton up by itself.
    std::atomic<uint32_t> sleeping{0};
    // Fields below are only written by the baton holder, so the baton orders them.
    bool started = false;      // has been handed the baton at least once
    bool abandoned = false;    // destroyed before it ever ran: its thread exits instead
    Context* exitTarget = nullptr;  // set by ExitTo(): hand over to this, then exit
};

thread_local Context* t_self = nullptr;

// libc++ implements atomic::wait with a futex only on Linux (and a few BSDs); elsewhere, including
// Emscripten, it polls with a sleeping backoff, which made each wake cost hundreds of
// microseconds under Node. Use Emscripten's futex (Atomics.wait/notify) directly there.
bool HasBaton(const Context* context)
{
    return context->running.load(std::memory_order_acquire) != 0;
}

// `sleeping` and `running` form a Dekker handshake (both sides store, then load, sequentially
// consistent): either the sleeper sees the baton before blocking or the giver sees the sleeper
// and wakes it. The futex wait itself only blocks while `running` is still 0.
void SleepUntilBaton(Context* context)
{
    context->sleeping.store(1, std::memory_order_seq_cst);
    while (context->running.load(std::memory_order_seq_cst) == 0) {
#if defined(__EMSCRIPTEN__)
        emscripten_futex_wait(&context->running, 0, INFINITY);
#else
        context->running.wait(0, std::memory_order_acquire);
#endif
    }
    context->sleeping.store(0, std::memory_order_relaxed);
}

void GiveBaton(Context* context)
{
    context->running.store(1, std::memory_order_seq_cst);
    if (context->sleeping.load(std::memory_order_seq_cst) == 0) {
        return;
    }
#if defined(__EMSCRIPTEN__)
    emscripten_futex_wake(&context->running, 1);
#else
    context->running.notify_one();
#endif
}

// Waking a sleeping thread costs microseconds, but a context usually gets the baton back
// within a few microseconds (scheduler round trips), so spin briefly before sleeping. Measured
// with host_context_stress_tests on a 4-core x86-64 Linux host:
// - native x86-64 (pause in the loop, 4000 iterations): a scheduler ping-pong drops from ~3.6 us
//   to ~0.4 us per switch, two workers from ~12 us to ~0.5 us; with more runnable contexts than
//   cores spinning stops paying off (~14 us -> ~19 us for eight).
// - Emscripten under Node (no pause, so iterations are far cheaper; 40000 iterations): ~0.6 us
//   ping-pong, ~1 us for two workers, ~6 us for four, against 11-16 us with 4000.
#if defined(__x86_64__)
constexpr int kSpinIterations = 4000;
#else
constexpr int kSpinIterations = 40000;
#endif

void WaitForBaton(Context* context)
{
    for (int i = 0; i < kSpinIterations; ++i) {
        if (HasBaton(context)) return;
#if defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }
    SleepUntilBaton(context);
}

void HandOver(Context* source, Context* destination)
{
    destination->started = true;
    source->running.store(0, std::memory_order_relaxed);
    GiveBaton(destination);
}

void* ThreadMain(void* argument)
{
    auto* context = static_cast<Context*>(argument);
    t_self = context;
    WaitForBaton(context);
    if (context->abandoned) {
        // Destroy() handed over only so this thread could exit; the baton is not really ours.
        return nullptr;
    }
    context->entry(context->argument);
    if (!context->exitTarget) {
        // A guest fiber must leave through ExitTo(), never by simply returning.
        std::abort();
    }
    HandOver(context, context->exitTarget);
    return nullptr;
}
} // namespace

bool InitializeScheduler(Handle* scheduler)
{
    auto* context = new Context();
    context->running.store(1, std::memory_order_relaxed);
    context->started = true;
    t_self = context;
    *scheduler = context;
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    if (t_self == static_cast<Context*>(scheduler)) {
        t_self = nullptr;
    }
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    auto* context = new Context();
    context->entry = entry;
    context->argument = argument;

    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
        delete context;
        return nullptr;
    }
    pthread_attr_setstacksize(&attributes, stackSize);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int created = pthread_create(&thread, &attributes, ThreadMain, context);
    pthread_attr_destroy(&attributes);
    if (created != 0) {
        delete context;
        return nullptr;
    }
    return context;
}

void Destroy(Handle handle)
{
    auto* context = static_cast<Context*>(handle);
    if (!context || context->abandoned) {
        return;
    }
    if (!context->started) {
        // Wake it with `running` set (a futex wait only ends on a value change); it sees
        // `abandoned` and exits without touching anything else.
        context->abandoned = true;
        context->started = true;
        GiveBaton(context);
        return;
    }
    // A context that left through ExitTo() has handed over and its thread is exiting on its own.
    // One parked inside Switch() would need its stack unwound to exit, which native backends never
    // do (they free the stack as is), so its thread stays parked and is never handed the baton.
}

bool IsCurrent(Handle context)
{
    return context != nullptr && context == t_self;
}

void Switch(Handle target)
{
    auto* destination = static_cast<Context*>(target);
    Context* source = t_self;
    if (!destination || destination == source) {
        return;
    }

    HandOver(source, destination);
    WaitForBaton(source);
}

void ExitTo(Handle target)
{
    // ThreadMain hands over once the entry function has returned and its frames are gone, so
    // nothing of the exiting context runs concurrently with `target`.
    t_self->exitTarget = static_cast<Context*>(target);
}

#elif defined(_WIN32)

namespace {
thread_local bool g_convertedScheduler = false;
}

bool InitializeScheduler(Handle* scheduler)
{
    void* context = ConvertThreadToFiber(nullptr);
    g_convertedScheduler = context != nullptr;
    if (!context) {
        context = GetCurrentFiber();
    }
    *scheduler = context;
    return context != nullptr;
}

void ShutdownScheduler(Handle scheduler)
{
    if (scheduler && g_convertedScheduler) {
        ConvertFiberToThread();
    }
    g_convertedScheduler = false;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    return CreateFiber(stackSize, entry, argument);
}

void Destroy(Handle context)
{
    if (context) {
        DeleteFiber(context);
    }
}

bool IsCurrent(Handle context)
{
    return context != nullptr && GetCurrentFiber() == context;
}

void Switch(Handle target)
{
    SwitchToFiber(target);
}

void ExitTo(Handle target)
{
    Switch(target);
}

#elif defined(__APPLE__) && defined(__aarch64__)

namespace {
struct Context {
    void* savedStackPointer = nullptr;
    void* stack = nullptr;
    std::size_t stackSize = 0;
};

// Guest scheduling is confined to the initialized main host thread. Keeping
// this as ordinary process state also avoids relying on Darwin TLS internals
// while executing on a manually managed stack.
Context* g_current = nullptr;
}

bool InitializeScheduler(Handle* scheduler)
{
    auto* context = new Context();
    g_current = context;
    *scheduler = context;
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    auto* context = static_cast<Context*>(scheduler);
    if (g_current == context) {
        g_current = nullptr;
    }
    delete context;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    auto* context = new Context();
    const std::size_t guardSize = static_cast<std::size_t>(getpagesize());
    const std::size_t totalSize = stackSize + guardSize;
    context->stack = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
    if (context->stack == MAP_FAILED) {
        delete context;
        return nullptr;
    }
    // Fault on stack overflow instead of corrupting the preceding mapping.
    if (mprotect(context->stack, guardSize, PROT_NONE) != 0) {
        munmap(context->stack, totalSize);
        delete context;
        return nullptr;
    }
    context->stackSize = totalSize;

    auto* stackTop = static_cast<char*>(context->stack) + totalSize;
    context->savedStackPointer = mkw_co_init(stackTop, entry, argument);
    return context;
}

void Destroy(Handle context)
{
    auto* nativeContext = static_cast<Context*>(context);
    if (!nativeContext) {
        return;
    }
    if (nativeContext->stack) {
        munmap(nativeContext->stack, nativeContext->stackSize);
    }
    delete nativeContext;
}

bool IsCurrent(Handle context)
{
    return context != nullptr && context == g_current;
}

void Switch(Handle target)
{
    auto* destination = static_cast<Context*>(target);
    Context* source = g_current;
    if (!destination || destination == source) {
        return;
    }

    g_current = destination;
    mkw_co_switch(&destination->savedStackPointer, &source->savedStackPointer);
    g_current = source;
}

void ExitTo(Handle target)
{
    Switch(target);
}

#elif defined(__linux__) || (defined(__APPLE__) && defined(__x86_64__))

namespace {
struct Context {
    cothread_t native = nullptr;
    Entry entry = nullptr;
    void* argument = nullptr;
    bool ownsNative = false;
};

thread_local Context* g_current = nullptr;
thread_local std::unordered_map<cothread_t, Context*> g_contexts;

void ContextEntry()
{
    const auto found = g_contexts.find(co_active());
    if (found == g_contexts.end() || !found->second || !found->second->entry) {
        std::abort();
    }

    Context* context = found->second;
    g_current = context;
    context->entry(context->argument);

    // A guest fiber must return through FiberProc's scheduler handoff. There
    // is no valid native caller to return to from libco's entry trampoline.
    std::abort();
}
} // namespace

bool InitializeScheduler(Handle* scheduler)
{
    auto* context = new Context();
    context->native = co_active();
    if (!context->native) {
        delete context;
        return false;
    }

    g_current = context;
    g_contexts.emplace(context->native, context);
    *scheduler = context;
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    auto* context = static_cast<Context*>(scheduler);
    if (!context) {
        return;
    }

    g_contexts.erase(context->native);
    if (g_current == context) {
        g_current = nullptr;
    }
    delete context;
}

Handle Create(std::size_t stackSize, Entry entry, void* argument)
{
    auto* context = new Context();
    context->entry = entry;
    context->argument = argument;
    context->native = co_create(static_cast<unsigned int>(stackSize), ContextEntry);
    context->ownsNative = context->native != nullptr;
    if (!context->native) {
        delete context;
        return nullptr;
    }

    g_contexts.emplace(context->native, context);
    return context;
}

void Destroy(Handle context)
{
    auto* nativeContext = static_cast<Context*>(context);
    if (!nativeContext) {
        return;
    }

    g_contexts.erase(nativeContext->native);
    if (nativeContext->ownsNative) {
        co_delete(nativeContext->native);
    }
    delete nativeContext;
}

bool IsCurrent(Handle context)
{
    return context != nullptr && context == g_current;
}

void Switch(Handle target)
{
    auto* destination = static_cast<Context*>(target);
    Context* source = g_current;
    if (!destination || destination == source) {
        return;
    }

    g_current = destination;
    co_switch(destination->native);
    g_current = source;
}

void ExitTo(Handle target)
{
    Switch(target);
}

#endif

} // namespace HostContext
