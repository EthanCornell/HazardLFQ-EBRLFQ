// hazard_pointer.hpp  — header-only, C++17, ASan/TSan clean
#pragma once
#include <atomic>
#include <array>
#include <thread>
#include <vector>
#include <functional>
#include <cassert>
#include <cstddef>

namespace hp {

// ────────── Tunables ────────────────────────────────────────────────
constexpr unsigned kHazardsPerThread = 2;                 // K
constexpr unsigned kMaxThreads       = 128;               // upper bound N
constexpr unsigned kRFactor          = 2;                 // R = H * kRFactor
// -------------------------------------------------------------------

// Forward-declared housekeeping
void   scan();
size_t thread_index();

// ────────── 1.  Hazard-pointer slots (shared)  ──────────────────────
struct HazardSlot {
    std::atomic<void*> ptr { nullptr };
};
inline std::array<HazardSlot, kMaxThreads * kHazardsPerThread> g_slots{};

// ────────── 2.  Per-thread bookkeeping  ─────────────────────────────
struct RetiredNode {
    void*                   raw;
    std::function<void(void*)> deleter;
};
inline thread_local struct ThreadRec {
    // hazard-pointer indices owned by this thread (≤K each)
    std::array<HazardSlot*, kHazardsPerThread> h { nullptr,nullptr };
    // private stack of retired nodes
    std::vector<RetiredNode>                   retired;
} tls;

// Acquire an unused hazard slot for *this* thread
inline HazardSlot* acquire_slot() {
    for (auto& s : g_slots) {
        void* exp = nullptr;
        if (s.ptr.compare_exchange_strong(exp, reinterpret_cast<void*>(1),
                                          std::memory_order_acq_rel))
            return &s;                  // index recorded in tls.h
    }
    throw std::runtime_error("hp: out of global hazard slots");
}

// Public RAII guard that publishes a single hazard pointer
class Guard {
    HazardSlot* s_;
public:
    Guard() {
        // lazily allocate a slot from the thread’s pool
        unsigned i{};
        for (; i<kHazardsPerThread && !tls.h[i]; ++i);
        if (i==kHazardsPerThread) i = 0;                 // cycle
        if (!tls.h[i]) tls.h[i] = acquire_slot();
        s_ = tls.h[i];
    }
    Guard(const Guard&)            = delete;
    Guard& operator=(const Guard&) = delete;

    template<typename T>
    T* protect(std::atomic<T*>& src) {
        T* p;
        do {
            p = src.load(std::memory_order_acquire);
            s_->ptr.store(p, std::memory_order_release);
        } while (p != src.load(std::memory_order_acquire));
        return p;
    }
    void clear() noexcept { s_->ptr.store(nullptr, std::memory_order_release); }
    ~Guard() { clear(); }
};

// ────────── 3.  Retire / Scan (Figures 2 & 3) ───────────────────────
inline void retire(void* p, std::function<void(void*)> deleter) {
    tls.retired.push_back({p, std::move(deleter)});
    const size_t H = kHazardsPerThread * kMaxThreads;
    const size_t R = H * kRFactor;                       // R ≥ H + Ω(H)  ✔ :contentReference[oaicite:4]{index=4}:contentReference[oaicite:5]{index=5}
    if (tls.retired.size() >= R) scan();
}

inline void scan() {
    // Stage 1: snapshot all current hazard pointers
    std::vector<void*> hazard_snapshot;
    hazard_snapshot.reserve(kHazardsPerThread * kMaxThreads);
    for (auto& s : g_slots) {
        void* p = s.ptr.load(std::memory_order_acquire);
        if (p && p!=reinterpret_cast<void*>(1)) hazard_snapshot.push_back(p);
    }

    // Stage 2: check our retired list against the snapshot
    auto it = tls.retired.begin();
    while (it != tls.retired.end()) {
        if (std::find(hazard_snapshot.begin(), hazard_snapshot.end(),
                      it->raw) == hazard_snapshot.end()) {
            // safe to reclaim
            it->deleter(it->raw);
            it = tls.retired.erase(it);
        } else {
            ++it;
        }
    }
}

// Convenience overload for objects
template<typename T>
inline void retire(T* obj) {
    retire(static_cast<void*>(obj),
           [](void* p){ delete static_cast<T*>(p); });
}

} // namespace hp
