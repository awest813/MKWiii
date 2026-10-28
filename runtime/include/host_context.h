#pragma once

#include <cstddef>

// HostContext is the deliberately small boundary between the guest scheduler
// and the host's cooperative-context facility. Windows uses native Fibers;
// Linux uses libco's host backend; Intel macOS uses its System V x86-64 backend. macOS AArch64 uses
// the local assembly backend because it must preserve Darwin's platform-reserved
// x18 register, which libco's AArch64 backend does not save. Its handles are
// only valid on the thread that initialized the scheduler.
//
// Wasm has no native stack switching (Asyncify would instrument nearly every
// translated function). Two backends cover it:
// - MKW_HOST_CONTEXT_JSPI (the web default): each context is its own suspendable
//   stack via JavaScript Promise Integration, all on one JS thread. That keeps
//   every guest thread on the thread that owns WebGPU and the canvas, and a
//   destroyed context's suspended stack is simply garbage-collected. Chromium.
// - MKW_HOST_CONTEXT_THREADS: each context is a real host thread and Switch()
//   hands a single baton between them, so exactly one runs at a time and only
//   at Switch() calls. Works wherever pthreads do; any host can opt in, which is
//   how it is tested natively. See guest_domain_local.h for the state it affects.
#if defined(__EMSCRIPTEN__) && !defined(MKW_HOST_CONTEXT_THREADS) && !defined(MKW_HOST_CONTEXT_JSPI)
#define MKW_HOST_CONTEXT_JSPI 1
#elif defined(__wasm__) && !defined(__EMSCRIPTEN__) && !defined(MKW_HOST_CONTEXT_THREADS)
#define MKW_HOST_CONTEXT_THREADS 1
#endif

namespace HostContext {

using Handle = void*;
using Entry = void (*)(void*);

bool InitializeScheduler(Handle* scheduler);
void ShutdownScheduler(Handle scheduler);

Handle Create(std::size_t stackSize, Entry entry, void* argument);
void Destroy(Handle context);
bool IsCurrent(Handle context);
void Switch(Handle target);

// Ends the calling (non-scheduler) context: `target` runs next and the caller is never resumed.
// The caller's entry function must return immediately after this call. On fiber backends this
// is Switch(target), which never returns, and a later Destroy() frees the abandoned stack. The
// thread backend returns instead, so the entry function unwinds normally and the context's
// thread hands over to `target` and exits once it has.
void ExitTo(Handle target);

} // namespace HostContext
