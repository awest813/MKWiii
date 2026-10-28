// Web (wasm32) GuestFlat backend. There is no flat reservation (see guest_abi/flat_memory.h):
// each backing store is an ordinary zeroed allocation in linear memory, every guest access goes
// through the checked page-table path, and MMIO / deferred-read / executable-write interception
// is done by that path rather than by page protections, exactly as on hosts whose pages are
// larger than a Wii page. The interface therefore only has to describe where each region lives.
#include "guest_flat_memory.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace GuestFlat {
namespace {

// Matches the Wii's own page size, so region bases and the MEM1/MEM2 offsets stay valid.
constexpr uint64_t kWebPageSize = kGuestPageSize;

struct Store {
    Backing kind = Backing::Owned;
    uint32_t owned = 0;
    uint64_t size = 0;
    uint8_t* host = nullptr;
};

struct Mapping {
    uint32_t base = 0;
    uint64_t size = 0;
    uint8_t* host = nullptr;
};

std::mutex g_mutex;
std::vector<Store> g_stores;
std::vector<Mapping> g_mappings;
std::vector<RegionRequest> g_layout;
bool g_active = false;

uint64_t RoundUp(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

// Cached and uncached views of MEM1 / MEM2 share one store at their physical offset.
uint64_t Offset(const RegionRequest& r) {
    if (r.backing == Backing::Mem1) return r.base & 0x1fffffffu;
    if (r.backing == Backing::Mem2) return (r.base & 0x1fffffffu) - 0x10000000u;
    return 0;
}

bool Same(const std::vector<RegionRequest>& a, const std::vector<RegionRequest>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const auto& x, const auto& y) { return x.base == y.base && x.size == y.size && x.backing == y.backing; });
}

void FreeStores(std::vector<Store>& stores) {
    for (auto& s : stores) std::free(s.host);
    stores.clear();
}

} // namespace

bool IsActive() { return g_active; }

void Initialize(const std::vector<RegionRequest>& regions) {
    std::lock_guard lock(g_mutex);
    if (g_active) {
        if (!Same(g_layout, regions))
            throw std::runtime_error("flat guest layout cannot be remapped");
        return;
    }

    for (size_t i = 0; i < regions.size(); ++i) {
        const auto& r = regions[i];
        if (r.size == 0) continue;
        if (r.size > kGuestSpaceSize - r.base ||
            r.base % kWebPageSize != 0 || Offset(r) % kWebPageSize != 0) {
            throw std::runtime_error("invalid flat guest memory region");
        }
        const uint64_t end = static_cast<uint64_t>(r.base) + RoundUp(r.size, kWebPageSize);
        for (size_t j = 0; j < i; ++j) {
            const auto& prior = regions[j];
            if (prior.size == 0) continue;
            const uint64_t priorEnd = static_cast<uint64_t>(prior.base) + RoundUp(prior.size, kWebPageSize);
            if (r.base < priorEnd && prior.base < end)
                throw std::runtime_error("overlapping flat guest memory regions");
        }
    }

    std::vector<Store> stores;
    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        auto it = std::find_if(stores.begin(), stores.end(), [&](const Store& s) {
            return s.kind == r.backing && s.owned == owned;
        });
        const uint64_t need = RoundUp(Offset(r) + r.size, kWebPageSize);
        if (it == stores.end())
            stores.push_back({r.backing, owned, need, nullptr});
        else
            it->size = std::max(it->size, need);
    }

    for (auto& s : stores) {
        // calloc: guest RAM starts zeroed, as the anonymous mappings on other hosts do.
        s.host = static_cast<uint8_t*>(std::calloc(1, static_cast<size_t>(s.size)));
        if (s.host == nullptr) {
            FreeStores(stores);
            throw std::runtime_error("unable to allocate the guest memory backing store");
        }
    }

    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        const auto& s = *std::find_if(stores.begin(), stores.end(), [&](const Store& x) {
            return x.kind == r.backing && x.owned == owned;
        });
        g_mappings.push_back({r.base, r.size, s.host + Offset(r)});
    }

    g_layout = regions;
    g_stores = std::move(stores);
    g_active = true;
}

uint8_t* HostPointer(uint32_t a) {
    for (const auto& m : g_mappings) {
        if (a >= m.base && uint64_t(a - m.base) < m.size)
            return m.host + (a - m.base);
    }
    return nullptr;
}

// Nothing is protected, so there is nothing to (un)protect or to resolve from a fault: the
// checked path consults the deferred-read and executable-guard tables directly.
void ProtectDeferredRange(uint32_t, size_t) {}
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}
FaultCounters Counters() { return {}; }
void LogFaultSummary() noexcept {}
bool HandleAccessViolation(void*, bool) noexcept { return false; }

} // namespace GuestFlat
