#include "host_context.h"

#if defined(MKW_HOST_CONTEXT_THREADS)
#include <pthread.h>

#include <atomic>
#include <cstdlib>
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
struct Context {
    Entry entry = nullptr;
    void* argument = nullptr;
    // The baton. A thread waits for its own flag to become true; whoever sets it hands over
    // all memory the previous holder wrote (release/acquire).
    std::atomic<bool> running{false};
    std::atomic<bool> abandoned{false};  // destroyed before it ever ran
    bool started = false;                // only touched by the context's own thread and Destroy
};

thread_local Context* t_self = nullptr;

// Waking a sleeping thread costs microseconds, but a context usually gets the baton back
// within a few microseconds (scheduler round trips), so spin briefly before sleeping. Measured
// with host_context_stress_tests on a 4-core x86-64 Linux host: a scheduler ping-pong drops from
// ~3.6 us to ~0.4 us per switch, two workers from ~12 us to ~0.5 us; with more runnable contexts
// than cores spinning stops paying off (~14 us -> ~19 us for eight).
constexpr int kSpinIterations = 4000;

void WaitForBaton(Context* context)
{
    for (int i = 0; i < kSpinIterations; ++i) {
        if (context->running.load(std::memory_order_acquire)) return;
#if defined(__x86_64__)
        __builtin_ia32_pause();
#endif
    }
    while (!context->running.load(std::memory_order_acquire)) {
        context->running.wait(false, std::memory_order_acquire);
    }
}

void* ThreadMain(void* argument)
{
    auto* context = static_cast<Context*>(argument);
    t_self = context;
    while (!context->running.load(std::memory_order_acquire)) {
        if (context->abandoned.load(std::memory_order_acquire)) {
            // Destroyed before its first Switch(): nothing references it any more.
            delete context;
            return nullptr;
        }
        context->running.wait(false, std::memory_order_acquire);
    }
    context->entry(context->argument);

    // A guest fiber must return through FiberProc's scheduler handoff, never off the end.
    std::abort();
}
} // namespace

bool InitializeScheduler(Handle* scheduler)
{
    auto* context = new Context();
    context->running.store(true, std::memory_order_relaxed);
    context->started = true;
    t_self = context;
    *scheduler = context;
    return true;
}

void ShutdownScheduler(Handle scheduler)
{
    auto* context = static_cast<Context*>(scheduler);
    if (t_self == context) {
        t_self = nullptr;
    }
    delete context;
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
    if (!context) {
        return;
    }
    // Only the baton holder calls Destroy, and a context only starts once it has been handed the
    // baton, so `started` is stable here. Switch() sets it on the destination before the handoff.
    if (!context->started) {
        context->abandoned.store(true, std::memory_order_release);
        context->running.notify_one();
        return;
    }
    // A context that has run is parked inside Switch() on a stack that native backends simply
    // free. A thread's stack cannot be dropped without unwinding it, so the thread stays parked
    // (it never gets the baton again) and its Context stays allocated for it.
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

    destination->started = true;
    source->running.store(false, std::memory_order_relaxed);
    destination->running.store(true, std::memory_order_release);
    destination->running.notify_one();
    WaitForBaton(source);
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

#endif

} // namespace HostContext
