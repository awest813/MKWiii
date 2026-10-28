#include "host_context.h"

#if defined(MKW_HOST_CONTEXT_JSPI)
#include <emscripten.h>
#include <emscripten/stack.h>

#include <cstdint>
#include <cstdlib>
#elif defined(MKW_HOST_CONTEXT_THREADS)
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

#if defined(MKW_HOST_CONTEXT_JSPI)

// Every context runs on this one JS thread. A context is a separate `WebAssembly.promising` call
// into mkw_host_context_run; Switch() calls a suspending import that parks the caller on a promise
// and queues the target, which starts (first time) or has its own parked promise resolved. Only
// one context runs at a time and only Switch()/ExitTo() hand over, as with fibers. JSPI needs the
// stack between the promising entry and the suspending import to be wasm only, which holds for
// guest code: it is never entered from a JS callback.
//
// JSPI switches the engine's stack, but not the C "shadow" stack Emscripten keeps in linear memory
// for address-taken locals: that is one region per thread behind one stack-pointer global. So each
// context gets its own shadow stack (Create's stackSize), the pointer is saved when a context
// suspends and restored, with the overflow-check limits, whenever one starts or resumes.
//
// The runtime must be linked with -sJSPI -sJSPI_EXPORTS=mkw_host_context_run.
extern "C" void _emscripten_stack_restore(uintptr_t stackPointer);

namespace {
struct Context {
    Entry entry = nullptr;
    void* argument = nullptr;
    bool started = false;
    Context* exitTarget = nullptr;
    // Shadow stack: [stackEnd, stackBase), growing down. The scheduler keeps the thread's own.
    void* stackAllocation = nullptr;
    uintptr_t stackBase = 0;
    uintptr_t stackEnd = 0;
    uintptr_t savedStackPointer = 0;
};

Context* g_current = nullptr;
} // namespace

// STACK_OVERFLOW_CHECK=2 builds check every stack-pointer write against limits of their own, set
// through this export (Emscripten's pthread startup does the same).
EM_JS(void, mkw_host_context_js_set_checked_limits, (uintptr_t base, uintptr_t end), {
    wasmExports["__set_stack_limits"]?.(base, end);
});

namespace {
void UseStack(const Context* context, uintptr_t stackPointer)
{
    emscripten_stack_set_limits(reinterpret_cast<void*>(context->stackBase),
                                reinterpret_cast<void*>(context->stackEnd));
    mkw_host_context_js_set_checked_limits(context->stackBase, context->stackEnd);
    _emscripten_stack_restore(stackPointer);
}
} // namespace

EM_JS(void, mkw_host_context_js_init, (), {
    // (No `??=` here: C++ would read it as a trigraph.)
    if (Module.mkwHostContext) return;
    Module.mkwHostContext = {
        parked: new Map(),
        // Resume a parked context, or start one that has never run. Queued as a microtask so the
        // caller finishes suspending (or returning) before the target touches anything.
        schedule(target) {
            queueMicrotask(() => {
                const resume = this.parked.get(target);
                if (resume) {
                    this.parked.delete(target);
                    resume();
                    return;
                }
                // Move onto the new context's own shadow stack before its first frame.
                wasmExports["mkw_host_context_enter_stack"](target);
                wasmExports["mkw_host_context_run"](target).catch((error) => {
                    // An exception escaping a context has no wasm caller left to catch it.
                    console.error("HostContext: a context ended with an exception", error);
                    throw error;
                });
            });
        },
    };
});

EM_JS(void, mkw_host_context_js_schedule, (void* target), {
    Module.mkwHostContext.schedule(target);
});

EM_ASYNC_JS(void, mkw_host_context_js_switch, (void* source, void* target), {
    const jspi = Module.mkwHostContext;
    const parked = new Promise((resolve) => jspi.parked.set(source, resolve));
    jspi.schedule(target);
    await parked;
});

EM_JS(void, mkw_host_context_js_forget, (void* context), {
    // Dropping the resolver lets the engine collect the parked stack.
    Module.mkwHostContext?.parked.delete(context);
});

// Called from JS right before mkw_host_context_run starts a context; it has no frame of its own.
extern "C" EMSCRIPTEN_KEEPALIVE void mkw_host_context_enter_stack(void* argument)
{
    const auto* context = static_cast<const Context*>(argument);
    // One aligned slot below the top: Emscripten's stack-overflow checking (STACK_OVERFLOW_CHECK=2)
    // rejects a stack pointer equal to the stack base.
    UseStack(context, context->stackBase - 16);
}

extern "C" EMSCRIPTEN_KEEPALIVE void mkw_host_context_run(void* argument)
{
    auto* context = static_cast<Context*>(argument);
    g_current = context;
    context->entry(context->argument);
    if (!context->exitTarget) {
        // A guest fiber must leave through ExitTo(), never by simply returning.
        std::abort();
    }
    // This stack ends here; the target runs once it has.
    mkw_host_context_js_schedule(context->exitTarget);
}

bool InitializeScheduler(Handle* scheduler)
{
    mkw_host_context_js_init();
    auto* context = new Context();
    context->started = true;
    context->stackBase = emscripten_stack_get_base();
    context->stackEnd = emscripten_stack_get_end();
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
    // stackSize sizes the shadow stack; the engine's own stack for the context grows as needed.
    constexpr std::size_t kAlignment = 16;
    stackSize = (stackSize + kAlignment - 1) & ~(kAlignment - 1);
    void* stack = std::aligned_alloc(kAlignment, stackSize);
    if (!stack) {
        return nullptr;
    }
    auto* context = new Context();
    context->entry = entry;
    context->argument = argument;
    context->stackAllocation = stack;
    context->stackEnd = reinterpret_cast<uintptr_t>(stack);
    context->stackBase = context->stackEnd + stackSize;
    return context;
}

void Destroy(Handle handle)
{
    auto* context = static_cast<Context*>(handle);
    if (!context) {
        return;
    }
    // Never started: nothing references it. Parked: forgetting its resolver means it is never
    // resumed and its stack is collected. Exited: its stack is already gone. Only the running
    // context could still use the record, and it never destroys itself.
    mkw_host_context_js_forget(context);
    std::free(context->stackAllocation);
    delete context;
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
    destination->started = true;
    source->savedStackPointer = emscripten_stack_get_current();
    mkw_host_context_js_switch(source, destination);
    // Resumed: whichever context ran last left its stack pointer behind.
    UseStack(source, source->savedStackPointer);
    g_current = source;
}

void ExitTo(Handle target)
{
    // mkw_host_context_run hands over once the entry function has returned.
    g_current->exitTarget = static_cast<Context*>(target);
}

#elif defined(MKW_HOST_CONTEXT_THREADS)

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
