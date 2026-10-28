// Round-robin stress test for a HostContext backend: several contexts take turns through the
// scheduler for many rounds. Checks that exactly one context ever runs at a time, that every
// Switch() resumes the context it targeted at its own continuation point, that plain (non-atomic)
// state written by one context is visible to the next, that each context's stack memory survives
// while it is switched out, that contexts leaving through ExitTo()
// hand over correctly, and that destroying contexts - ones that never ran, ones that exited and
// ones parked mid-run - leaves the others working. With the thread backend on Linux it also checks
// that the threads of never-run and exited contexts really end. Prints the mean cost of one
// Switch() so backends can be compared.
#include "host_context.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(MKW_HOST_CONTEXT_THREADS) && defined(__linux__)
#include <dirent.h>
#endif

namespace {
// Worker count is argv[1] (default 8); 1 measures a plain scheduler ping-pong.
constexpr int kMaxWorkers = 8;
int g_workerCount = kMaxWorkers;
constexpr int kRounds = 20000;

HostContext::Handle g_scheduler = nullptr;
HostContext::Handle g_workers[kMaxWorkers] = {};
std::atomic<int> g_inside{0};
int g_expectedWorker = -1;
long g_sharedCounter = 0;  // deliberately not atomic: the handoff must order it
long g_perWorker[kMaxWorkers] = {};

void Fail(const char* what)
{
    std::fprintf(stderr, "host_context_stress_tests: %s\n", what);
    std::abort();
}

#if defined(MKW_HOST_CONTEXT_THREADS) && defined(__linux__)
int CountThreads()
{
    int count = 0;
    if (DIR* tasks = opendir("/proc/self/task")) {
        while (const dirent* entry = readdir(tasks)) {
            if (entry->d_name[0] != '.') ++count;
        }
        closedir(tasks);
    }
    return count;
}

// Threads exit asynchronously after handing over, so give them a moment.
void ExpectThreadCount(int expected, const char* what)
{
    for (int attempt = 0; attempt < 1000 && CountThreads() != expected; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (CountThreads() != expected) Fail(what);
}
#endif

constexpr int kExitingRounds = 3;
HostContext::Handle g_exiting = nullptr;
int g_exitingRounds = 0;

// Runs a few rounds, then leaves for good the way FiberProc does.
void ExitingWorker(void*)
{
    for (int round = 0; round < kExitingRounds; ++round) {
        if (!HostContext::IsCurrent(g_exiting)) Fail("exiting worker lost its identity");
        ++g_exitingRounds;
        if (round + 1 < kExitingRounds) HostContext::Switch(g_scheduler);
    }
    HostContext::ExitTo(g_scheduler);
}

// Memory each context keeps on its own stack across switches. `volatile` and passing its address
// out keep it in addressable stack memory (a shadow stack on wasm) rather than in registers.
void FillStackCanary(volatile uint32_t* canary, int count, uint32_t seed)
{
    for (int i = 0; i < count; ++i) canary[i] = seed * 2654435761u + static_cast<uint32_t>(i);
}

bool CheckStackCanary(const volatile uint32_t* canary, int count, uint32_t seed)
{
    for (int i = 0; i < count; ++i) {
        if (canary[i] != seed * 2654435761u + static_cast<uint32_t>(i)) return false;
    }
    return true;
}

// Switches away from `depth` nested frames, each holding its own canary, so contexts suspend at
// different and changing stack depths, as guest threads do.
void SwitchFromDepth(int index, int depth)
{
    volatile uint32_t frame[32];
    const uint32_t seed = static_cast<uint32_t>(index * 1000 + depth);
    FillStackCanary(frame, 32, seed);
    if (depth > 0) {
        SwitchFromDepth(index, depth - 1);
    } else {
        HostContext::Switch(g_scheduler);
    }
    if (!CheckStackCanary(frame, 32, seed)) Fail("a context's stack frame changed while it was switched out");
}

void Worker(void* argument)
{
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(argument));
    volatile uint32_t canary[256];
    FillStackCanary(canary, 256, static_cast<uint32_t>(index) + 1);
    for (long round = 0;; ++round) {
        if (!CheckStackCanary(canary, 256, static_cast<uint32_t>(index) + 1)) {
            Fail("a context's stack memory changed while it was switched out");
        }
        if (g_inside.fetch_add(1) != 0) Fail("two contexts ran at once");
        if (!HostContext::IsCurrent(g_workers[index])) Fail("IsCurrent lost the worker identity");
        if (g_expectedWorker != index) Fail("Switch resumed the wrong context");
        if (g_perWorker[index] != round) Fail("a worker lost its continuation point");
        ++g_perWorker[index];
        ++g_sharedCounter;
        g_inside.fetch_sub(1);
        SwitchFromDepth(index, static_cast<int>((round * 7 + index * 3) % 24));
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1) {
        g_workerCount = std::atoi(argv[1]);
        if (g_workerCount < 1 || g_workerCount > kMaxWorkers) Fail("worker count must be 1..8");
    }
    if (!HostContext::InitializeScheduler(&g_scheduler) || !HostContext::IsCurrent(g_scheduler)) {
        Fail("scheduler initialization");
    }
#if defined(MKW_HOST_CONTEXT_THREADS) && defined(__linux__)
    // Sanitizer runtimes start helper threads on first thread creation; let that happen first.
    std::thread([] {}).join();
    const int baseThreads = CountThreads();
#endif

    // A context that leaves through ExitTo() hands back to the scheduler for good.
    g_exiting = HostContext::Create(64 * 1024, ExitingWorker, nullptr);
    if (!g_exiting) Fail("Create (exiting)");
    for (int round = 0; round < kExitingRounds; ++round) {
        HostContext::Switch(g_exiting);
        if (!HostContext::IsCurrent(g_scheduler)) Fail("ExitTo did not return to the scheduler");
    }
    if (g_exitingRounds != kExitingRounds) Fail("exiting worker ran the wrong number of rounds");
    HostContext::Destroy(g_exiting);

    // A context destroyed before it ever runs must not disturb anything.
    for (int i = 0; i < 16; ++i) {
        HostContext::Handle neverRun = HostContext::Create(64 * 1024, Worker, nullptr);
        if (!neverRun) Fail("Create (never run)");
        HostContext::Destroy(neverRun);
    }
#if defined(MKW_HOST_CONTEXT_THREADS) && defined(__linux__)
    ExpectThreadCount(baseThreads, "threads of exited or never-run contexts did not end");
#endif
    for (int i = 0; i < g_workerCount; ++i) {
        g_workers[i] = HostContext::Create(256 * 1024, Worker, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        if (!g_workers[i]) Fail("Create");
    }

    const auto start = std::chrono::steady_clock::now();
    long switches = 0;
    volatile uint32_t schedulerCanary[256];
    FillStackCanary(schedulerCanary, 256, 0xC0FFEEu);
    for (int round = 0; round < kRounds; ++round) {
        if (!CheckStackCanary(schedulerCanary, 256, 0xC0FFEEu)) {
            Fail("the scheduler's stack memory changed while it was switched out");
        }
        for (int i = 0; i < g_workerCount; ++i) {
            if (round == kRounds / 2 && i == g_workerCount - 1) {
                // Destroy one worker parked mid-run; the rest must carry on.
                HostContext::Destroy(g_workers[i]);
                g_workers[i] = nullptr;
            }
            if (!g_workers[i]) continue;
            g_expectedWorker = i;
            HostContext::Switch(g_workers[i]);
            switches += 2;
            if (!HostContext::IsCurrent(g_scheduler)) Fail("scheduler identity lost");
            if (g_inside.load() != 0) Fail("worker still running after switching back");
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    long expectedTotal = 0;
    for (int i = 0; i < g_workerCount; ++i) {
        const long expected = i == g_workerCount - 1 ? kRounds / 2 : kRounds;
        if (g_perWorker[i] != expected) Fail("a worker ran the wrong number of rounds");
        expectedTotal += expected;
    }
    if (g_sharedCounter != expectedTotal) Fail("shared state was not ordered by the handoff");

    for (int i = 0; i < g_workerCount; ++i) HostContext::Destroy(g_workers[i]);
    HostContext::ShutdownScheduler(g_scheduler);

    const double nanos = std::chrono::duration<double, std::nano>(elapsed).count();
    std::printf("host_context_stress_tests: %ld switches, %.0f ns per switch\n", switches,
                nanos / static_cast<double>(switches));
    return 0;
}
