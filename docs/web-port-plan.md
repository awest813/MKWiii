# Web port: audit and plan

Status: plan accepted; decisions recorded under [Decisions](#decisions). Progress is tracked under
[Progress](#progress).

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
  cross-origin isolated with COOP/COEP headers), wasm SIMD128, and wasm exception handling. The
  first release targets current Chromium only; Firefox and Safari are follow-ups.

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

**What exists.** A checked mode already exists for hosts whose pages are larger than 4 KiB, and
Apple Silicon builds ship with it: `GuestFlat::RequiresCheckedAccess()` routes `FlatRead*` and
`FlatWrite*` to `Memory::Read*`/`Write*`. Those are inline lookups in the 1 MiB page bias tables
(`memory_access.h`: `g_fullReadablePageBias`, `g_fullWritablePageBias`, the sparse executable
sub-tables, `g_deferredReadCoveredPages`), so a plain RAM access costs one table load, and every
interception case is handled without traps. `guest_flat_memory_macos.cpp` already implements
`GuestFlat` with no fault handling at all.

**Plan (done, see Progress).**
- `__wasm__` defines no flat base, makes `RequiresCheckedAccess()` a compile-time `true`, and
  sends `FlatWriteRam*` through the checked path. The PSQ helpers already fall back when checked.
  The bias arithmetic wraps correctly modulo 2^32 with a 32-bit `uintptr_t`.
- `guest_flat_memory_web.cpp`: no reservation, plain zeroed allocations per region.
- An unmapped guest touch throws `Memory::AccessViolation` in checked mode, where flat x86 hosts
  commit the block and carry on. Apple Silicon builds already behave this way, so it is not a
  web-specific problem, but it is a cross-host difference worth knowing about.
- First optimization to try: `ResolveRangeHost` returns null in checked mode, which sends every
  range-lowered access through the out-of-line `*ResolvedFallback` helpers. Resolving a range that
  lies inside one plain 1 MiB page through the bias tables would keep those inline.
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
| JSPI | **Default (`MKW_HOST_CONTEXT_JSPI`).** Each guest thread is its own `WebAssembly.promising` stack, all on one JS thread; a switch parks the caller on a promise and resumes the target from a microtask. Chromium only, which the first release targets anyway. |
| One pthread per guest thread, with a baton | **Alternative (`MKW_HOST_CONTEXT_THREADS`).** Each guest thread is a real pthread (a Web Worker) that sleeps on its own futex, and one baton is handed around, so exactly one runs at a time. Works anywhere SharedArrayBuffer does, but guest threads then span several JS threads, which WebGPU does not allow (section 4). |
| Wasm stack switching proposal | Not shipped. Revisit when it is. |

**Plan (code done, see Progress).** Both backends live in `host_context.cpp`. JSPI is the web
default because it keeps every guest thread on the JS thread that owns WebGPU and the canvas, it
is faster under contention (Progress), and a destroyed context's parked stack is simply
garbage-collected. The rest of this list applies to the thread backend, which also runs natively
(`MKW_HOST_CONTEXT_THREADS`) and needs a prespawned pthread pool (`PTHREAD_POOL_SIZE`) on the web.
- **Thread-local state.** Every fiber used to share one host thread's TLS. Of the `thread_local`
  variables in the runtime, most are caches, log de-duplication or worker-thread flags, which are
  fine per thread. The guest-scoped ones (the fiber `CpuContext` handoff, the alarm and NAND
  recursion guards, the audio poll cursor, the GX begin/timing flags) now use
  `MKW_GUEST_DOMAIN_LOCAL`. The host FP mode that mirrors FPSCR[NI] is per thread, so a new guest
  thread applies its own on entry.
- **Ending contexts.** A guest thread whose entry function returns leaves through
  `HostContext::ExitTo()`, so its host thread hands over and exits; contexts destroyed before
  they ever ran exit too (the stress test checks both on Linux). A guest thread destroyed while
  parked mid-stack (for example cancelled, or exiting via `OSExitThread`) cannot be unwound, so its
  thread stays parked: one idle worker each. How often MKW does that still needs measuring.
- **Switch cost.** See Progress. Measure switches per frame with the existing perf stats in the
  browser.

### 3. Host threads and the browser event loop — M

**Today.** `RuntimeMain` (`runtime/src/main.cpp:1463–1520`) initializes aurora, starts
`GxWorker`, and then calls `InvokeIndirectCpu(entry)`, which runs the game forever. There are
dedicated host threads for the GX worker (`hle/gx/gx_worker.cpp:356`), the AX mixer
(`hle/audio/ax_mix.cpp:1361`), deferred networking, Wiimote I/O, music attenuation, and the VI
retrace timer. aurora adds the DVD thread, the render worker, and a pipeline-compile thread pool
(`lib/gfx/pipeline_cache.cpp:92`).

**Plan (revised for JSPI).** Guest code, aurora and WebGPU all run on the browser main thread,
and `main()` runs under JSPI so it can suspend. Two consequences:
- The main thread may never block. The places where the guest side waits (the scheduler idling
  until the next VI retrace or alarm, aurora waiting on GPU work or a buffer map) must suspend
  through JSPI and let the event loop run, which is also what presents each frame and delivers
  `mapAsync` callbacks. Short mutex waits against worker threads are tolerable (Emscripten spins
  on the main thread).
- Host worker threads stay pthreads (AX mixer, VI timer, deferred networking) as long as they
  never touch WebGPU or SDL. The runtime's GX worker and aurora's render worker and pipeline
  pool do touch WebGPU, so on the web they run inline on the main thread instead.

Use a fixed `INITIAL_MEMORY` sized for the guest regions plus aurora caches: memory growth with
shared memory works, but it is slow and invalidates JS views. Raise `STACK_SIZE` well above
Emscripten's 64 KiB default.

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
- Run everything WebGPU on the main thread (section 3): aurora's render worker and pipeline pool
  run inline there. That removes the need to funnel calls between threads.
- Replace timed `WaitAny` with a JSPI-suspending wait that yields to the event loop until the
  future completes (emdawnwebgpu delivers callbacks from the event loop).
- **Synchronous readbacks** (EFB peeks in `lib/gfx/color_peek.cpp` and `depth_peek.cpp`, the
  deferred-read materialization, `readback/gpu.cpp`): the guest stack suspends via JSPI until the
  map completes. Expect up to a frame of extra latency, and check that this doesn't change
  game-visible results.
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
- **Float-to-int casts.** NaN and out-of-range casts are undefined in C++, so the runtime gets
  whatever the host does: `INT_MIN` on x86. Wasm's saturating conversions (on by default in newer
  LLVM, so in current Emscripten) give `0`/`INT_MAX` instead. Web builds must compile with
  `-mno-nontrapping-fptoint`; `runtime/CMakeLists.txt` sets it for `MKW_PLATFORM_WEB`, and the
  ISA differential's `cast_to_int` digest fails if it is lost. The flag does not cover casts to
  *unsigned* types: `static_cast<uint32_t>(-1.0f)` is `0xFFFFFFFF` on x86 but `0` on wasm (and
  on AArch64). Runtime and HLE code casting possibly-negative floats to unsigned types needs an
  audit (translated code goes through the explicit fctiw/fctiwz helpers).
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

**A. Local web build (chosen for the first release).** Setup gains a `--target web` mode. It runs the same
translation, compiles with Emscripten instead of Clang, and produces a static folder
(`index.html`, `.js`, `.wasm`). Setup then serves it on `localhost` with COOP/COEP headers, plus
the optional socket bridge. The legal posture is identical to native, and the existing Setup and
Wheel Wizard flow extends naturally. The benefit is the browser sandbox and portability (for
example, ChromeOS or locked-down machines), not zero-install.

**B. In-browser build (chosen as a later spike, after A ships).** A hosted page ships only the runtime
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
| 1. Toolchain and ISA | **ISA part done.** Remaining: Emscripten toolchain file; build `runtime/tests` (`ppc_pair_fma_tests`, `psq_helpers_tests`, `runtime_config_tests`, `nand_*`, `test_expr`) for Node. (`ax_mix_kernels.h`, `gx_worker.cpp` and `host_cpu_baseline.cpp` already fall back to portable code off x86.) | ISA tests pass under Node with results bit-identical to native (**met** for the ISA differential). CI job added (**done**). |
| 2. Memory model | **Code done.** Remaining: build it with Emscripten, `ResolveRangeHost` fast path. | Synthetic-DOL recompilation test (`Launcher/Test-Recompilation.ps1` equivalent) runs headless (`--headless`) under Node. |
| 3. Guest threads | **Code done, tested natively.** Remaining: pthread pool sizing, browser switch-cost numbers. | `host_context_tests` plus a multi-thread synthetic DOL pass under Node, with switch-cost numbers recorded. |
| 4. aurora on the web | `emdawnwebgpu`, GPU-thread funnel, async pipelines, callback-based waits, readbacks, SDL3 Emscripten window, input, and audio. | aurora examples, then the synthetic DOL, render in Chromium. |
| 5. Boot the game | OPFS storage, disc import, `Config.toml` and NAND persistence, log download, fatal overlay. Retro Rewind static profile built and imported (`RetroRewind6` folder into OPFS) alongside the base game. | Local builds of PAL `RMCP01` and Retro Rewind both reach the title screen in Chromium. |
| 6. Parity and performance | Ghost-replay sync check against native for both profiles, audio, input, F10 overlay, profiling (memory path, NI flushing, byte swaps, readback latency). | Ghost replays stay in sync for both profiles. Full speed on the README's minimum hardware in Chromium. |
| 7. Packaging | Setup `--target web` for both the base game and Retro Rewind, localhost server with COOP/COEP, Wheel Wizard hook, docs. | One command produces and launches a web build of either profile. |
| 8. Option B spike | Translator on .NET wasm, `clang`/`wasm-ld` in browser workers, sharded compile into OPFS. | Written go/no-go with measured compile time and peak memory for a full build. |
| 9. Optional | Online play via the socket bridge, WebHID Wii Remotes, WebUSB GC adapter, Firefox and Safari, JSPI fibers. | Per item. |

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

## Progress

### Done

- **ISA layer (Phase 1).** `__wasm__` branches in `isa/ppc_isa_config.h`, `ppc_isa_fpenv.h`,
  `ppc_isa_float.h` and `ppc_isa_quantized.h`. `runtime/tests/isa_differential_tests.cpp` hashes
  every paired-single/FP/PSQ helper over 200k edge-biased inputs with FPSCR[NI] clear and set;
  `runtime/web/isa-differential.sh` builds it natively and for wasm32-wasi (run under Node) and
  diffs the digests. All 55 compared digests are identical. CI job: `web_isa` in `build.yml`.
  - FPSCR[NI] on wasm: the pair primitives flush inputs (DAZ) and flush results with x86's
    after-rounding tininess rule, which matches native bit for bit. Helpers built from scalar C++
    (`ps_sum*`, `ps_sel`, `ps_res`, `ps_from_scalar`, the fused-single scalars, `fres`,
    `frsqrte`) inherit the host's *global* DAZ/FTZ natively; on wasm they only match on
    non-denormal operands. They are tagged `[host-daz]` and compared with NI clear only.
    Whether this matters depends on where the game sets NI (THP decode is the known case).
  - Fused multiply-add uses exact `std::fma` for now (wasm has no FMA instruction). The
    relaxed-SIMD fast path and its startup fusion probe are still to do.
- **Guest memory (Phase 2, code only).** No flat reservation on wasm; `RequiresCheckedAccess()`
  is a compile-time `true`, `FlatWriteRam*` takes the checked path, and
  `src/guest_flat_memory_web.cpp` backs regions with plain allocations.
- **Guest threads (Phase 3, code only).** Thread-backed `HostContext` (`MKW_HOST_CONTEXT_THREADS`,
  default on wasm) with an atomic baton, plus `MKW_GUEST_DOMAIN_LOCAL`
  (`include/guest_domain_local.h`) for the guest-scoped state that relied on fibers sharing one
  host thread's TLS. `host_context_tests` and the new `host_context_stress_tests` run against it
  natively in the platform test suite.

### Measured

- Switch cost (`host_context_stress_tests`, 4-core x86-64 Linux):
  - libco (native Linux): about 20 ns.
  - Thread backend, native pthreads: about 0.4 µs ping-pong and 0.5 µs with two workers, but
    14–19 µs once there are more runnable contexts than cores.
  - Thread backend, Emscripten under Node: about 0.6 µs ping-pong, 1 µs with two workers, 6 µs
    with four (after replacing libc++'s polling `atomic::wait` with a real futex; it was 526 µs).
  - JSPI backend, Emscripten under Node 24: 0.6–1 µs at every worker count.
  - Browser numbers are still needed.

### Found along the way

- musl's `fma` (wasi-libc and Emscripten) returns `a*c + b` when `b` is zero, so a nonzero
  product that underflows comes back as `+0` instead of `-0`. Guarded in `PpcFmaDoubleInline`.
- Native x86-64-v2 builds (no FMA) flip the sign of a NaN subtractor in `ps_msub`/`ps_nmsub`
  (`ffc00001` where v3 gives `7fc00001`): the scalar fallback negates the subtractor before
  `std::fma`. Pre-existing and unrelated to the web port; not fixed here.
- Wasm's saturating float-to-int conversions would change out-of-range casts from x86's
  `INT_MIN` (see section 6); pinned off with `-mno-nontrapping-fptoint`.
- With two NaN operands, native x86 itself is not deterministic for commutative ops: clang may
  swap `mulps`/`addps` operands, so which payload wins depends on inlining.

### Next

- Install Emscripten and build the platform tests, the ISA differential and the runtime for
  `wasm32-unknown-emscripten` (this environment only had a WASI sysroot without threads,
  exceptions or a filesystem).
- Web platform layer: signal handlers, paths, Discord, music ducking and the CPU-baseline check.
- Phase 0 spikes (b) and (c) in a real browser, then Phase 4 (aurora).

## Decisions

- **Distribution:** option A (local web build via Setup) for the first release, then an option B
  (in-browser build) feasibility spike as Phase 8.
- **Browsers:** Chromium-only for the first release.
- **Retro Rewind:** in scope from Phase 5, before base-game parity is signed off; both profiles
  share every exit criterion from then on.
- **aurora:** web changes land in the main `patchzyy/aurora-wiicompiled` fork behind an
  `AURORA_WEB` / `__EMSCRIPTEN__` switch, not on a long-lived branch, so native and web stay on
  one renderer. Native performance must not regress from these changes.
