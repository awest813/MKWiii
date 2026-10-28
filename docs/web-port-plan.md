# Web port: audit and plan

Status: proposal. Nothing here is implemented yet.

This document audits what stands between the current native runtime and a build that runs in a
browser (WebAssembly + WebGPU), and lays out a phased plan. File references are to this repo and
to the `aurora-main` submodule at `457903b`.

## Verdict

A web port is feasible, but it is a large project, and most of the work is in the runtime and in
aurora, not in the translator. Three things decide whether it works:

1. **Threading under the browser event loop.** The game never returns from `main()`. Guest
   threads are stackful coroutines. aurora calls WebGPU from several threads and blocks on GPU
   work with timed waits. Browsers allow none of this on the main thread, and WebGPU objects
   are tied to a single thread.
2. **Memory model.** The fast path assumes a fixed 4 GiB host reservation and uses page-fault
   traps for MMIO, EFB, and executable-write interception. WebAssembly has neither. A checked
   page-table path already exists, so the fix is a new fast path, not a redesign.
3. **Distribution.** The project does not ship translated binaries. A wasm build contains
   translated game code just like the native one, so it cannot be hosted publicly either. The
   web build has to be produced on the user's machine, the same way the native build is.

The translator emits portable C++ and sends every guest memory access through runtime helpers
(`MemoryInline::Flat*`), so it should need few or no codegen changes.

## Constraints

- **Legal posture is unchanged.** No Nintendo code or assets are hosted anywhere. The `.wasm` is
  derived from the user's disc, so it is a local build product, just like the native executable.
- **The accuracy bar is unchanged.** Ghost replays must stay in sync with Dolphin, Wii, and native
  WiiCompiled. That rules out any floating-point shortcut that changes results.
- **Browser floor.** The plan needs WebGPU, wasm threads (SharedArrayBuffer, so the page must be
  cross-origin isolated with COOP/COEP headers), wasm SIMD128, and wasm exception handling. In
  practice that means current Chromium first, with Firefox and Safari as follow-ups.

## Audit by subsystem

Effort: S (days), M (1–3 weeks), L (1–2 months), XL (more).

### 1. Guest memory model — L

**Today.** `runtime/include/guest_flat_memory.h` and `runtime/src/guest_flat_memory.cpp` reserve
4 GiB at a fixed host address (`guest_abi/flat_memory.h`: `kFixedFlatGuestBase`, with `#error`
for any other architecture). Translated accesses are `*(T*)(base + addr)` plus a byte swap.
Several cases rely on page protection and a SIGSEGV/vectored-exception handler
(`HandleAccessViolation`):
- MMIO reads,
- deferred EFB reads (`ProtectDeferredRange`),
- the executable-write guard (`RegisterExecutableRange`),
- on-demand commit of unmapped 64 KiB blocks.

MMIO *writes* are already checked inline (`FlatWriteNeedsPolicy`).

**Web blocker.** wasm32 linear memory tops out at 4 GiB, *including* the runtime, and has no
page protection and no fault handlers. An out-of-bounds access is a fatal trap.

**What exists.** A checked mode already exists for AArch64 hosts with pages larger than 4 KiB:
`GuestFlat::RequiresCheckedAccess()` routes `FlatRead*`/`FlatWrite*` to `Memory::Read*`/`Write*`,
which walk the 1 MiB page table (`memory_access.h`: `g_fullPageBias`, `g_fullReadablePageBias`,
`g_fullWritablePageBias`, the sparse executable sub-tables, and `g_deferredReadCoveredPages`).
That model covers every interception case without traps. Two problems:
- `Memory::Read32` and friends are out-of-line calls, which is too slow for every guest access.
- `FlatWriteRam*` and the PSQ helpers (`isa/ppc_isa_quantized.h:269,286`) still write directly
  through `MKW_FLAT_GUEST_BASE`.

**Plan.**
- Add a `__wasm__` configuration where `RequiresCheckedAccess()` is a `constexpr true` and the
  `Flat*` helpers inline the bias-table fast path: load `g_fullReadablePageBias[addr >> 20]`
  (or the writable table), add it, then load or store. Fall back to the existing slow path when
  the entry is zero. The bias tables are 4096 entries of 32-bit `uintptr_t` (16 KiB each), and
  the encoded `(host − guest) + 1` arithmetic wraps correctly modulo 2^32.
- Point `FlatWriteRam*` and the PSQ direct paths at `g_fullPageBias` instead of a flat base.
- New `guest_flat_memory_web.cpp`: no reservation, ordinary allocations for MEM1, MEM2, and the
  locked cache, and `HostPointer()` backed by the page table.
- **Parity gap to fix first:** on flat hosts, an unmapped guest touch silently commits a block and
  carries on (counted in `FaultCounters::unmapped`), but the checked path throws
  `Memory::AccessViolation("no mapped region")` (`runtime/src/memory.cpp:421`). The web build
  needs the flat behaviour, or the two hosts diverge on wild pointers.
- Possible later optimization: a masked "physical arena" (`arena + (addr & 0x1FFFFFFF)` covers
  the MEM1/MEM2 cached and uncached mirrors). It saves a table load but needs special handling for
  the locked cache and MMIO reads. Only worth it if profiling says so.

### 2. Guest threads (fibers) — L

**Today.** `runtime/include/fiber_manager.h` maps each guest `OSThread` to a host stackful
context. `runtime/src/host_context.cpp` is the seam: Win32 Fibers on Windows, a hand-written
`mkw_co_switch` on macOS (`platform/macos/co_switch.S`), and vendored libco on Linux. Each
context gets a 1 MiB host stack (`fiber_manager.cpp:264`). Switches happen deep inside
translated call stacks (`OSSleepThread`, `OSYieldThread`, and so on).

**Web blocker.** Wasm has no stack switching that has shipped. The candidates:

| Option | Verdict |
| --- | --- |
| Asyncify (`emscripten_fiber_t`) | Reject. Almost every translated function can reach a yield through an indirect call, so Asyncify would instrument nearly the whole program: a large size and speed cost. |
| JSPI | Viable later. Each guest thread becomes a suspended `WebAssembly.promising` stack that a scheduler resumes. Chromium only today, and each switch goes through the microtask queue. |
| One pthread per guest thread, with a baton | **Baseline.** Each guest thread is a real pthread (a Web Worker) that sleeps on its own semaphore. The scheduler hands one baton around, so exactly one runs at a time, which keeps cooperative semantics exact. Works anywhere SharedArrayBuffer does. |
| Wasm stack switching proposal | Not shipped. Revisit when it is. |

**Plan.** Add `host_context_web.cpp` built on the baton model, and prespawn the pthread pool
(`PTHREAD_POOL_SIZE`), because creating a worker needs the browser main thread to return to its
event loop.
- **Risk: thread-local state.** There are 59 `thread_local` uses across 20 runtime files (for
  example `g_currentCpuContext` in `guest_abi/cpu_context.h` and the NI/FTZ mirrors in
  `isa/ppc_isa_fpenv.h`). Today, every fiber shares one host thread's TLS. With one pthread per
  guest thread, each gets its own copy. Every one of these must be classified as either
  per-host-thread or per-guest-thread before this works.
- **Risk: switch cost.** Each handoff is a futex wake. Measure switches per frame with the
  existing perf stats before committing; JSPI is the fallback if this is too slow.

### 3. Host threads and the browser event loop — M

**Today.** `RuntimeMain` (`runtime/src/main.cpp:1463–1520`) initializes aurora, starts
`GxWorker`, and then calls `InvokeIndirectCpu(entry)`, which runs the game forever. There are
dedicated host threads for the GX worker (`hle/gx/gx_worker.cpp:356`), the AX mixer
(`hle/audio/ax_mix.cpp:1361`), deferred networking, Wiimote I/O, music attenuation, and the VI
retrace timer. aurora adds the DVD thread, the render worker, and a pipeline-compile thread pool
(`lib/gfx/pipeline_cache.cpp:92`).

**Plan.** Build with `-sPROXY_TO_PTHREAD`, so `main()` and everything it spawns run in workers
and may block. The browser main thread only owns the DOM: the canvas (transferred as an
`OffscreenCanvas`), input events, and audio. Use a fixed `INITIAL_MEMORY` sized for the guest
regions plus aurora caches. Memory growth with shared memory works, but it is slow and
invalidates JS views. Raise `STACK_SIZE` and `DEFAULT_PTHREAD_STACK_SIZE` well above Emscripten's
64 KiB default.

### 4. Graphics: aurora, Dawn, WebGPU — XL

**Today.** aurora uses native Dawn (`dawn::native` in `lib/webgpu/gpu.cpp`, `lib/gfx/frame.cpp`,
`lib/gfx/pipeline_cache.cpp`, `lib/webgpu/gpu_prof.cpp`), including:
- Dawn toggles,
- timed `Instance::WaitAny` with 1–5 s timeouts (`gpu.cpp:790,898,1103`),
- shared texture memory and MetalFX on macOS,
- a zstd-compressed on-disk pipeline cache.

WebGPU calls come from the game thread (`lib/gx/gx.cpp`, `lib/gfx/texture.cpp`, and others), the
render worker, and the pipeline thread pool. The aurora fork has no Emscripten code. Two good
signs: it already requests the `Compatibility` feature level, and it treats BC compression and
dual-source blending as optional features with fallbacks (`texture_convert.cpp`
`BuildRGBA8FromBC1`).

**Web blockers.**
- In the browser, a `GPUDevice` and everything created from it live on one thread.
- Timed waits are not allowed.
- `mapAsync` and presentation only make progress when that thread returns to its event loop.

**Plan.**
- Build aurora against Dawn's `emdawnwebgpu` port. It keeps the `wgpu::` C++ API, so most
  renderer code compiles unchanged.
- Make aurora's render worker the single **GPU thread**. It owns the `OffscreenCanvas` and runs a
  non-blocking loop that returns to the event loop every frame.
- Turn every WebGPU call made off that thread into a queued closure. `render_worker::QueueItem`
  already carries a `std::function`. Where a caller needs the result right away, it blocks on a
  future, which is allowed because callers are workers.
- Replace pipeline-pool compilation with `CreateRenderPipelineAsync` on the GPU thread.
- Replace `WaitAny` with callbacks.
- **Synchronous readbacks** (EFB peeks in `lib/gfx/color_peek.cpp` and `depth_peek.cpp`, the
  deferred-read materialization, `readback/gpu.cpp`): the guest thread blocks on a futex while
  the GPU thread maps the buffer and yields. Expect up to a frame of extra latency, and check that
  this doesn't change game-visible results.
- Compile out on web: Dawn toggles, shared texture memory, MetalFX, timestamp queries, and the
  on-disk pipeline cache (or move it to OPFS).
- These changes belong in `patchzyy/aurora-wiicompiled` behind a `__EMSCRIPTEN__` or
  `AURORA_WEB` switch.

### 5. ISA layer and floating-point determinism — M

**Today.**
- `isa/ppc_isa_config.h`, `ppc_isa_fpenv.h`, `ppc_isa_float.h`, and `ppc_isa_quantized.h` have
  only `__x86_64__` and `__aarch64__` branches (and `#error` otherwise).
- `hle/audio/ax_mix_kernels.h`, `hle/gx/gx_worker.cpp`, and `host_cpu_baseline.cpp` also use
  SSE/NEON.
- FPSCR[NI] is modelled by setting the host FTZ/DAZ bits (MXCSR on x86, FPCR on AArch64), so
  arithmetic flushing costs nothing.
- Paired-single FMA uses `_mm_fmadd_ps` or `vfma_f32`. There is already a non-FMA x86-64-v2
  fallback through scalar `std::fma`.
- AArch64 already rewrites NaN results to the x86 answer (`PpcResolveNanLanes*`).

**Web blockers.**
- Wasm has no FP control register, so there is no FTZ/DAZ: denormals are always IEEE.
- Wasm has no scalar fused multiply-add, so `std::fma` becomes a slow software routine.
- NaN payloads from wasm arithmetic are nondeterministic by spec.
- Wasm has no byte-swap instruction. `__builtin_bswap32` expands to shifts and masks on *every*
  guest load and store.

**Plan.** Add a third, `__wasm__` branch built on `<wasm_simd128.h>`, using the 2-lane pair layout
from the AArch64 branch:
- **NaN:** reuse the AArch64 resolver approach unchanged.
- **FTZ:** flush in software while NI is active. The existing `g_mkwNiFlushThreshold` mirror
  already provides a branchless mask. The THP IDCT comment (`ppc_isa_fpenv.h`) measured about 2×
  for software flushing, so profile with NI on.
- **FMA:** use relaxed-SIMD `relaxed_madd`, which is fused on FMA-capable hardware, and verify at
  startup with a known fused-vs-unfused probe. If the probe fails, fall back to the existing
  `PpcAccuratePsMaddLane*` scalar path, or refuse to run. Do **not** silently use unfused
  multiply-add: it breaks ghost sync.
- **Byte swap:** measure first. Optimize later only if profiling shows it matters (for example,
  with a SIMD shuffle on bulk copies).

### 6. Translator and generated code — S to M

**Today.** The generated C++ has no intrinsics, inline asm, `setjmp`, or computed `goto`. All
guest memory access goes through runtime helpers (`CodeGen/CxxLinearCodeGenerator.FlatGuestMemory.cs`).
I found no assumption that pointers are 64-bit.

**Web risks.**
- **Indirect calls must match exactly.** Wasm traps on any signature mismatch in an indirect
  call. `TranslatedFunctionInfo::entryPoint` is a `void*` that is cast back to a typed pointer,
  so every such cast must use exactly the original signature. Build debug configurations with
  `-sASSERTIONS` to catch mismatches.
- **Host stack depth.** Each guest call is a host call. V8 puts wasm frames on its own native
  stack with a fixed limit, separate from Emscripten's shadow stack. Deep guest stacks combined
  with large translated frames could hit "maximum call stack size exceeded". Measure early.
- **Module size and compile time.** The whole game becomes a single module. Browsers compile it
  quickly with their baseline compiler, then optimize in the background. Track `.wasm` size from
  the first full build. Running `wasm-opt` over a module this large may be too slow; per-shard
  `-O2` without LTO is the likely default.
- **Build graph.** `emit-build-shards` and `runtime/CMakeLists.txt` hard-code x86 `-march`
  profiles, and Windows uses `__regcall` (`ppc_isa_config.h`). The web build needs an Emscripten
  toolchain file and per-target flags.

### 7. Exceptions and crash handling — S

**Today.** `main.cpp` installs vectored exception handlers on Windows (`:1119`) and
`sigaction(SIGSEGV/SIGBUS)` on POSIX (`:1213`), and catches `Memory::AccessViolation` and
`std::exception` at the top level.

**Plan.** Build with `-fwasm-exceptions` so the existing `try`/`catch` keeps working. Compile out
the signal and SEH handlers. Catch wasm traps (`WebAssembly.RuntimeError`) in JS, then show the
same fatal report as an HTML overlay with a "download log" button. On web,
`ShowRuntimeFatalPopup` and `WriteFatalLogImpl` both go through that overlay.

### 8. Filesystem, disc image, persistence — M

**Today.**
- `hle/storage/dvd.cpp` reads an extracted disc (`boot.bin`, the FST) with `std::ifstream`.
- NAND saves, `Config.toml`, and logs live in platform paths (`include/nand_path.h`,
  `platform/host_platform.cpp`).
- Setup extracts the disc with `nod` (`Launcher/WiiCompiled.Setup.Common/NodToolProvider.cs`)
  and accepts ISO, GCM, GCZ, CISO, WBFS, WIA, and RVZ.

**Plan.**
- Use WasmFS with the OPFS backend. It is synchronous inside workers, which fits because all
  game code runs in pthreads.
- On first run, the user picks their disc image (or the extracted folder) and it is extracted into
  OPFS once. Extraction can be `nod` compiled to wasm (it is Rust), or done by Setup when the
  user builds locally.
- Keep game files separate from the `.wasm` either way, so a future in-browser build (option B
  below) can share the importer.
- Saves and config persist automatically in OPFS. Add export and import of the NAND save folder,
  so users don't lose saves when they clear site data.

### 9. Audio — S to M

**Today.** `runtime/src/audio_backend.cpp` uses SDL3 audio streams, and the AX mix runs on its own
thread.

**Plan.** SDL3's Emscripten audio backend may be sufficient. If it is driven from the main thread
in a way that conflicts with `PROXY_TO_PTHREAD`, feed an `AudioWorklet` from a SharedArrayBuffer
ring buffer instead. Browsers only start audio after a user gesture, so the page needs a
click-to-start screen. Drop music ducking (`music_attenuation.cpp`, which uses MPRIS, SMTC, or
CoreAudio).

### 10. Input — S (gamepads and keyboard), L (Wii Remotes and GC adapter)

**Today.** SDL3 gamepads with per-port mapping and Dolphin input expressions. The GC adapter
(WUP-028) goes through libusb. Real Wii Remotes use emulated Bluetooth HCI (`hle/bluetooth/`,
`wiimote/`), and `wiimote_platform_unix.cpp` loads hidraw helpers with `dlopen`.

**Plan.** SDL3's Emscripten backend maps the browser Gamepad API and keyboard, so the F10 mapping
UI and expressions carry over. The GC adapter (WebUSB) and Wii Remotes (WebHID) are
Chromium-only follow-ups, off by default.

### 11. Networking — L (optional)

**Today.** `hle/net/` emulates IOS sockets over native BSD TCP and UDP sockets
(`network_socket.cpp`) and uses mbedTLS for SSL.

**Web blocker.** Browsers have no raw TCP or UDP sockets.

**Plan.** Treat online play as a late, optional phase. In the local-build model (below), the
same local helper that serves the page can also run a socket bridge: Emscripten's POSIX socket
proxying, or a small WebSocket-to-TCP/UDP relay, on localhost. That makes Wiimmfi and Retro WFC
reachable, including UDP peer-to-peer, with no change to the HLE layer.

### 12. Platform extras to drop or replace on web — S

- Discord presence: drop.
- Window placement persistence: drop.
- MetalFX: drop.
- `host_cpu_baseline.cpp`: replace with feature detection at page load (WebGPU, threads/COOP+COEP,
  SIMD128, relaxed SIMD, wasm exceptions), with a clear "unsupported browser" page.
- Run log: write to OPFS and offer it as a download, so the existing bug-report flow still works.

## Distribution models (maintainer decision needed)

**A. Local web build (recommended first).** Setup gains a `--target web` mode. It runs the same
translation, compiles with Emscripten instead of Clang, and produces a static folder
(`index.html`, `.js`, `.wasm`). Setup then serves it on `localhost` with COOP/COEP headers, plus
the optional socket bridge. The legal posture is identical to native, and the existing Setup and
Wheel Wizard flow extends naturally. The benefit is the browser sandbox and portability (for
example, ChromeOS or locked-down machines), not zero-install.

**B. In-browser build (stretch, needs a feasibility spike).** A hosted page ships only the runtime
and the toolchain: the translator (.NET on wasm), plus `clang` and `wasm-ld` compiled to wasm.
The user drops in their disc and everything is translated and compiled in browser workers, then
cached in OPFS. Nothing derived from the game leaves the user's machine. The risk is compile time
and memory: the native setup already needs about 20 GB of scratch space, and a single wasm clang
instance is capped at 4 GiB.

**C. Direct PPC-to-wasm backend in the translator.** Not recommended. It would discard the C++
path whose output has been proven accurate against hardware, along with the runtime-header
inlining it depends on.

## Phased plan

Each phase ends with something testable and adds a CI job, so it doesn't regress.

| Phase | Work | Exit criterion |
| --- | --- | --- |
| 0. Decide and spike | Pick a distribution model. Spikes: (a) baton-fiber switch cost in a browser, (b) aurora example running on `emdawnwebgpu` in a worker with `OffscreenCanvas`, (c) relaxed-SIMD FMA fusion probe across the target browsers and CPUs. | Written go/no-go for each spike. |
| 1. Toolchain and ISA | Emscripten toolchain file, `__wasm__` branches in the ISA headers, `ax_mix_kernels.h`, and `gx_worker.cpp`. Build `runtime/tests` (`ppc_pair_fma_tests`, `psq_helpers_tests`, `runtime_config_tests`, `nand_*`, `test_expr`) for Node. | ISA tests pass under Node with results bit-identical to native. CI job added. |
| 2. Memory model | Wasm checked fast path, `guest_flat_memory_web.cpp`, fix the unmapped-access parity gap, move `FlatWriteRam*` and PSQ off the flat base. | Synthetic-DOL recompilation test (`Launcher/Test-Recompilation.ps1` equivalent) runs headless (`--headless`) under Node. |
| 3. Guest threads | `host_context_web.cpp` (baton), `thread_local` classification, pthread pool. | `host_context_tests` plus a multi-thread synthetic DOL pass under Node, with switch-cost numbers recorded. |
| 4. aurora on the web | `emdawnwebgpu`, GPU-thread funnel, async pipelines, callback-based waits, readbacks, SDL3 Emscripten window, input, and audio. | aurora examples, then the synthetic DOL, render in Chromium. |
| 5. Boot the game | OPFS storage, disc import, `Config.toml` and NAND persistence, log download, fatal overlay. | Local build of PAL `RMCP01` reaches the title screen in Chromium. |
| 6. Parity and performance | Ghost-replay sync check against native, audio, input, F10 overlay, profiling (memory path, NI flushing, byte swaps, readback latency). | Ghost replays stay in sync. Full speed on the README's minimum hardware in Chromium. |
| 7. Packaging | Setup `--target web`, localhost server with COOP/COEP, Wheel Wizard hook, docs. | One command produces and launches a web build. |
| 8. Optional | Online play via the socket bridge, WebHID Wii Remotes, WebUSB GC adapter, Firefox and Safari, JSPI fibers, option B spike, Retro Rewind web profile. | Per item. |

## Top risks

1. **Performance.** The native minimum spec is already a GTX 1650 and an i5-8400. Wasm adds
   explicit memory checks, a table lookup per access, software byte swaps, and software FTZ, and
   the baton handoff makes guest thread switches slower. Measure in Phase 0 and 3 before
   investing in packaging.
2. **WebGPU single-thread funnel.** This is the largest change to aurora, and it has to be
   upstreamed to the fork without hurting native performance.
3. **Determinism.** Ghost sync depends on fused multiply-add and NI handling. If the FMA probe
   fails on a browser and CPU combination, that combination must be refused, not degraded.
4. **Host stack limits** for deep guest call chains in V8 workers.
5. **Distribution.** Option A gives a sandboxed local build, not a "visit a URL and play" port.
   Option B is the only route to that without hosting game-derived code, and it is unproven.

## Open questions

- Which distribution model should we target: A only, or A now and a B spike later?
- Is a Chromium-only first release acceptable?
- Should Retro Rewind be in scope before base-game parity (Phase 6)?
- Should the web-specific aurora changes live in the main fork behind a flag, or on a separate
  branch?
