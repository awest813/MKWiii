// Round-robin stress test for a HostContext backend: several contexts take turns through the
// scheduler for many rounds. Checks that exactly one context ever runs at a time, that every
// Switch() resumes the context it targeted at its own continuation point, that plain (non-atomic)
// state written by one context is visible to the next, and that destroying contexts - both ones
// that never ran and ones parked mid-run - leaves the others working. Prints the mean cost of one
// Switch() so backends can be compared.
#include "host_context.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

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

void Worker(void* argument)
{
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(argument));
    for (long round = 0;; ++round) {
        if (g_inside.fetch_add(1) != 0) Fail("two contexts ran at once");
        if (!HostContext::IsCurrent(g_workers[index])) Fail("IsCurrent lost the worker identity");
        if (g_expectedWorker != index) Fail("Switch resumed the wrong context");
        if (g_perWorker[index] != round) Fail("a worker lost its continuation point");
        ++g_perWorker[index];
        ++g_sharedCounter;
        g_inside.fetch_sub(1);
        HostContext::Switch(g_scheduler);
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
    for (int i = 0; i < g_workerCount; ++i) {
        g_workers[i] = HostContext::Create(256 * 1024, Worker, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        if (!g_workers[i]) Fail("Create");
    }

    // A context destroyed before it ever runs must not disturb anything.
    HostContext::Handle neverRun = HostContext::Create(64 * 1024, Worker, nullptr);
    if (!neverRun) Fail("Create (never run)");
    HostContext::Destroy(neverRun);

    const auto start = std::chrono::steady_clock::now();
    long switches = 0;
    for (int round = 0; round < kRounds; ++round) {
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
