/**********************************************************************
 *  lockfree_queue_alpha.cpp  –  one-file build of:
 *      1) hazard_pointer.hpp
 *      2) lockfree_queue.hpp
 *      3) tests  (with debug prints)
 *
 *  ▶ Quick build (small loops, no sanitizers) ──────────────────────
 *      g++ -std=c++20 -O2 lockfree_queue_all.cpp && ./a.out
 *
 *  ▶ ASan build (still “quick” sizes) ──────────────────────────────
 *      g++ -std=c++20 -O1 -g -fsanitize=address \
 *          -fno-omit-frame-pointer lockfree_queue_all.cpp && ./a.out
 *
 *  ▶ Stress (original big loops)  add  -DSTRESS_TESTS  ︙
 *  ▶ Super-chatty debug           add  -DDEBUG_QUEUE
 *********************************************************************/

/**********************************************************************
 *  hazard_pointer.hpp  –  Minimal, header-only HP implementation
 *
 *  Algorithm:  Maged M. Michael, “Hazard Pointers” (IEEE TPDS, 2004)
 *  Key idea:   Each thread publishes at most K pointers it is about to
 *              dereference; any node not present in the global snapshot
 *              is safe to free.
 *********************************************************************/
#pragma once
#include <atomic>
#include <array>
#include <vector>
#include <functional>
#include <algorithm>
#include <stdexcept>

namespace hp {

/* Tunables --------------------------------------------------------- */
constexpr unsigned kHazardsPerThread = 2;   // K in the paper
constexpr unsigned kMaxThreads       = 128; // worst-case thread count
constexpr unsigned kRFactor          = 2;   // R = H×factor (H = K×N)

/* ───────────────── Global HP table ──────────────────────────────── */
struct Slot {                                // one slot == one HP
    std::atomic<void*> ptr { nullptr };      // nullptr   -> unused
};                                           // (void*)1  -> reserved
inline std::array<Slot, kHazardsPerThread * kMaxThreads> g_slots{};

/* ───────────────── Per-thread bookkeeping ───────────────────────── */
struct Retired {                             // logically removed node
    void*                      raw;
    std::function<void(void*)> del;          // how to free it
};
inline thread_local struct TLS {
    std::array<Slot*, kHazardsPerThread> hp { nullptr,nullptr }; // my HP slots
    std::vector<Retired>                 retired;                // private list
} tls;

/* Acquire an unused global slot, mark it “reserved” (= (void*)1)   */
inline Slot* acquire_slot() {
    for (auto& s : g_slots) {
        void* exp = nullptr;
        if (s.ptr.compare_exchange_strong(exp, reinterpret_cast<void*>(1),
                                          std::memory_order_acq_rel))
            return &s;
    }
    throw std::runtime_error("hp: all hazard slots exhausted");
}

/* RAII guard: 1) reserves a slot, 2) publishes a pointer, 3) clears */
class Guard {
    Slot* s_;
public:
    Guard() {
        /* lazily assign one of this thread’s K slots */
        unsigned i = 0; while (i < kHazardsPerThread && !tls.hp[i]) ++i;
        if (i == kHazardsPerThread) i = 0;           // rotate if all used
        if (!tls.hp[i]) tls.hp[i] = acquire_slot();
        s_ = tls.hp[i];
    }
    Guard(const Guard&)            = delete;
    Guard& operator=(const Guard&) = delete;

    /* Publish & validate until the pointer is stable (Fig. 2, line 4) */
    template<class Ptr>
    Ptr* protect(const std::atomic<Ptr*>& src) {
        Ptr* p;
        do {
            p = src.load(std::memory_order_acquire);
            s_->ptr.store(p, std::memory_order_release);
        } while (p != src.load(std::memory_order_acquire)); // re-check
        return p;
    }
    void clear() { s_->ptr.store(nullptr, std::memory_order_release); }
    ~Guard()     { clear(); }
};

/* Forward decl so `retire()` can call `scan()` */
inline void scan();

/* Retire a node: push on list, run `scan()` when list length ≥ R */
inline void retire(void* p, std::function<void(void*)> d) {
    tls.retired.push_back({p, std::move(d)});
    const std::size_t H = kHazardsPerThread * kMaxThreads;
    if (tls.retired.size() >= H * kRFactor) scan();          // amortised O(1)
}

/* Scan: 1) snapshot all HPs, 2) reclaim nodes not in snapshot */
inline void scan() {
    /* 1. Build snapshot (≡ HP-set S in paper) */
    std::vector<void*> snap;
    snap.reserve(kHazardsPerThread * kMaxThreads);
    for (auto& s : g_slots) {
        void* p = s.ptr.load(std::memory_order_acquire);
        if (p && p != reinterpret_cast<void*>(1)) snap.push_back(p);
    }
    /* 2. Move through retired list and free safe nodes */
    auto it = tls.retired.begin();
    while (it != tls.retired.end()) {
        if (std::find(snap.begin(), snap.end(), it->raw) == snap.end()) {
            it->del(it->raw);                   // no hazard ⇒ reclaim
            it = tls.retired.erase(it);
        } else ++it;
    }
}

/* Convenience wrapper deducing the deleter */
template<class T>
inline void retire(T* p) {
    retire(static_cast<void*>(p),
           [](void* q){ delete static_cast<T*>(q); });
}

} // namespace hp



/**********************************************************************
 *  lockfree_queue.hpp – Michael & Scott FIFO queue (1996) + HP GC
 *********************************************************************/
#pragma once
#include "hazard_pointer.hpp"
#include <atomic>
#include <new>
#include <utility>

namespace lfq {

template<class T>
class Queue {
    /* ───────── Single linked node (dummy allowed) ──────────────── */
    struct Node {
        std::atomic<Node*> next{nullptr};
        alignas(T) unsigned char storage[sizeof(T)];
        bool has_value;                              // dummy nodes have none

        template<class... Args>
        explicit Node(bool dummy, Args&&... args)
            : has_value(!dummy)
        {
            if (!dummy)                               // placement-new payload
                ::new (storage) T(std::forward<Args>(args)...);
        }
        T&       value()       { return *std::launder(reinterpret_cast<T*>(storage)); }
        const T& value() const { return *std::launder(reinterpret_cast<const T*>(storage)); }
        ~Node() { if (has_value) value().~T(); }
    };

    /* head_ points at dummy; tail_ points at last node            */
    std::atomic<Node*> head_, tail_;

public:
    Queue() {
        Node* d = new Node(true);          // initial dummy
        head_.store(d); tail_.store(d);
    }
    Queue(const Queue&)            = delete;
    Queue& operator=(const Queue&) = delete;

    /* ───────────────── enqueue (multi-producer) ────────────────── */
    template<class... Args>
    void enqueue(Args&&... args);

    /* ───────────────── dequeue (multi-consumer) ────────────────── */
    bool dequeue(T& out);

    bool empty() const;
    ~Queue();
};

/* ===================  Implementation  ============================ */
template<class T>
template<class... Args>
void Queue<T>::enqueue(Args&&... args)
{
    /* 1. Allocate node off-list; never blocks.                       */
    Node* n = new Node(false, std::forward<Args>(args)...);

    /* 2. Guard the *tail node* so it can’t be reclaimed mid-loop.    */
    hp::Guard hTail;             // producers need just one HP slot

    for (;;) {
        /* 3. Publish & read current tail pointer.                    */
        Node* tail = hTail.protect(tail_);

        /* 4. Read tail->next.                                        */
        Node* next = tail->next.load(std::memory_order_acquire);

        /* 5. Verify tail is still consistent; otherwise retry.       */
        if (tail != tail_.load(std::memory_order_acquire))
            continue;

        if (next == nullptr) {
            /* 6-a: The queue is quiescent => link our node.          */
            if (tail->next.compare_exchange_weak(next, n,
                    std::memory_order_release,
                    std::memory_order_relaxed))
            {
                /* 7. Helping step: advance global tail (optional)    */
                tail_.compare_exchange_strong(tail, n,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                return;                         // enqueue done 🎉
            }
        } else {
            /* 6-b: Another thread already enqueued; swing tail.      */
            tail_.compare_exchange_strong(tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
        }
        /* loop repeats with a fresh hazard-protected tail            */
    }
}

template<class T>
bool Queue<T>::dequeue(T& out)
{
    hp::Guard hHead, hNext;                      // we need 2 HP slots

    for (;;) {
        Node* head = hHead.protect(head_);       // dummy/protected
        Node* tail = tail_.load(std::memory_order_acquire);
        Node* next = hNext.protect(head->next);  // real first elem (maybe null)

        /* restart if another thread advanced head while we looked */
        if (head != head_.load(std::memory_order_acquire))
            continue;

        if (!next)                               // queue empty
            return false;

        if (head == tail) {                      // tail is lagging
            tail_.compare_exchange_strong(tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
            continue;                            // retry
        }

        /* Read payload *before* we swing head, while we still own HP */
        T tmp = next->value();

        if (head_.compare_exchange_strong(head, next,
                std::memory_order_release,
                std::memory_order_relaxed))
        {
            out = std::move(tmp);                // deliver value
            hHead.clear();                       // allow reclaim
            hp::retire(head);                    // retire old dummy
            return true;
        }
    }
}

template<class T>
bool Queue<T>::empty() const
{
    hp::Guard g;
    Node* h = g.protect(head_);
    return h->next.load(std::memory_order_acquire) == nullptr;
}

template<class T>
Queue<T>::~Queue()
{
    /* In destructor there’s no concurrency: free list directly.      */
    Node* n = head_.load();
    while (n) { Node* nx = n->next.load(); delete n; n = nx; }
}

} // namespace lfq


/*====================================================================*
 |  3.  Tests (with fixed aliases & 64-bit tickets)                   |
 *====================================================================*/
#line 1 "lockfree_queue_tests.cpp"
#include <barrier>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <thread>
#include <type_traits>
#include <vector>
#include <atomic>

template<typename T> using LockFreeQueue = lfq::Queue<T>;

using SteadyClk = std::chrono::steady_clock;          // avoid POSIX clock_t
using seconds_d = std::chrono::duration<double>;

inline void announce(const char* name, const char* phase)
{ std::printf("[%-28s] %s\n", name, phase); }

#ifndef STRESS_TESTS
constexpr int SMALL_THREADS = 8;
constexpr int SMALL_ITERS   = 20'000;
#else
constexpr int SMALL_THREADS = 32;
constexpr int SMALL_ITERS   = 100'000;
#endif

#ifdef DEBUG_QUEUE
#  define DPRINTF(...) std::printf(__VA_ARGS__)
#else
#  define DPRINTF(...) ((void)0)
#endif

template<typename C>
void expect_progress(C& counter, std::chrono::seconds timeout, const char* d)
{
    auto start = SteadyClk::now();
    auto last  = counter.load(std::memory_order_acquire);
    while (SteadyClk::now() - start < timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        auto cur = counter.load(std::memory_order_acquire);
        if (cur != last) { last = cur; start = SteadyClk::now(); }
    }
    if (counter.load(std::memory_order_acquire) == last)
        throw std::runtime_error(std::string("Live-lock: ") + d);
}

/* ── T1: simple MPMC enqueue ──────────────────────────────────────*/
void test_atomic_correctness()
{
    announce("T1: simple enqueue", "START");
    auto t0 = SteadyClk::now();

    constexpr int kThreads = SMALL_THREADS, kIters = SMALL_ITERS;
    LockFreeQueue<int> q;
    std::barrier sync(kThreads);

    auto worker = [&](int id){
        sync.arrive_and_wait();
        for (int i = 0; i < kIters; ++i) {
            q.enqueue(i + id * kIters);
            DPRINTF("[T1] prod %d -> %d\n", id, i);
        }
    };
    std::vector<std::thread> th;
    for (int i = 0; i < kThreads; ++i) th.emplace_back(worker, i);
    for (auto& t : th) t.join();

    int v, cnt = 0;
    while (q.dequeue(v)) ++cnt;
    assert(cnt == kThreads * kIters);

    std::printf("    dequeued %d items in %.3f s\n",
                cnt, seconds_d(SteadyClk::now()-t0).count());
    announce("T1: simple enqueue", "DONE");
}

/* ── T1b: uniqueness (64-bit ticket) ─────────────────────────────*/
void test_atomic_correctness_new()
{
    announce("T1b: ticket check", "START");
    auto t0 = SteadyClk::now();

#ifdef STRESS_TESTS
    constexpr int P = 16, C = 16, K = 200'000;
#else
    constexpr int P = 4,  C = 4,  K = 25'000;
#endif
    using ticket_t = std::uint64_t;           // always 64-bit
    constexpr std::size_t TOTAL = std::size_t(P) * K;

    LockFreeQueue<ticket_t> q;
    std::atomic<std::size_t> consumed{0};
    std::vector<std::atomic<uint8_t>> seen(TOTAL);
    for (auto& b : seen) b.store(0, std::memory_order_relaxed);

    auto push = [&](int id){
        for (int i = 0; i < K; ++i)
            q.enqueue((ticket_t(id) << 32) | ticket_t(i));
    };
    auto pop = [&]{
        ticket_t v;
        while (consumed.load(std::memory_order_acquire) < TOTAL) {
            if (q.dequeue(v)) {
                std::size_t idx =
                    (std::size_t)(v >> 32) * K + std::size_t(v & 0xffffffffu);
                uint8_t exp = 0;
                if (!seen[idx].compare_exchange_strong(exp, 1,
                                                       std::memory_order_acq_rel))
                    throw std::logic_error("duplicate");
                consumed.fetch_add(1, std::memory_order_release);
            }
        }
    };
    std::vector<std::thread> prod, cons;
    for (int i = 0; i < P; ++i) prod.emplace_back(push, i);
    for (int i = 0; i < C; ++i) cons.emplace_back(pop);
    for (auto& t : prod) t.join();
    for (auto& t : cons) t.join();
    assert(consumed.load() == TOTAL);

    std::printf("    verified %zu tickets in %.3f s\n",
                TOTAL, seconds_d(SteadyClk::now()-t0).count());
    announce("T1b: ticket check", "DONE");
}

/* ── T2: ABA / UAF stress ─────────────────────────────────────────*/
void test_aba_uaf()
{
    announce("T2: ABA/UAF", "START");

#ifdef STRESS_TESTS
    constexpr int kThreads = 8,  kIters = 500'000;
    constexpr auto TIMEOUT  = std::chrono::seconds(10);
#else
    constexpr int kThreads = 4,  kIters = 50'000;
    constexpr auto TIMEOUT  = std::chrono::seconds(5);
#endif

    LockFreeQueue<int> q;
    std::atomic<std::size_t> done{0};      // number of successful dequeues
    std::atomic<bool>        stop{false};  // global “please stop” flag

    /* ── Producers ──────────────────────────────────────────────── */
    auto producer = [&] {
        for (int i = 0;
             i < kIters && !stop.load(std::memory_order_acquire);
             ++i)
        {
            q.enqueue(i);
        }
    };

    /* ── Consumers ──────────────────────────────────────────────── */
    auto consumer = [&] {
        int v;
        while (!stop.load(std::memory_order_acquire)) {
            if (q.dequeue(v)) {
                if (done.fetch_add(1, std::memory_order_acq_rel) + 1 ==
                    static_cast<std::size_t>(kThreads) * kIters)
                {
                    stop.store(true);   // all items consumed
                    break;
                }
            }
        }
    };

    /* ── Watch-dog: abort if progress stalls for TIMEOUT ────────── */
    std::thread watchdog([&] {
        std::size_t last = done.load(std::memory_order_relaxed);
        auto        tic  = SteadyClk::now();

        while (!stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto cur = done.load(std::memory_order_acquire);

            if (cur != last) {          // forward progress observed
                last = cur;
                tic  = SteadyClk::now();
            } else if (SteadyClk::now() - tic > TIMEOUT) {
#ifdef DEBUG_QUEUE
                std::fprintf(stderr,
                    "[T2-WD] stall detected — done=%zu / %zu, queue.empty=%d\n",
                    cur,
                    static_cast<std::size_t>(kThreads) * kIters,
                    q.empty());
#endif
                stop.store(true);       // ask all threads to exit
                throw std::runtime_error("T2 ABA/UAF watchdog timeout");
            }
        }
    });

    /* ── Launch producers & consumers ───────────────────────────── */
    std::vector<std::thread> tp, tc;
    for (int i = 0; i < kThreads; ++i) {
        tp.emplace_back(producer);
        tc.emplace_back(consumer);
    }

    /* ── Join and propagate any watchdog exception ─────────────── */
    for (auto& t : tp) t.join();
    for (auto& t : tc) t.join();

    try { watchdog.join(); }            // re-throw if the watchdog aborted
    catch (...) { throw; }

    if (!stop.load(std::memory_order_acquire))   // finished normally
        announce("T2: ABA/UAF", "DONE");
}



/* ── T3: destructor safety ───────────────────────────────────────*/
void test_destructor_safe()
{
    announce("T3: dtor safety", "START");
    auto* q = new LockFreeQueue<int>;
    std::thread t([&]{ int v; while (!q->dequeue(v)); });
    q->enqueue(78);
    t.join();
    delete q;
    announce("T3: dtor safety", "DONE");
}

/* ── T4: live-lock watchdog ───────────────────────────────────────*/
void test_livelock()
{
    announce("T4: live-lock", "START");
#ifdef STRESS_TESTS
    constexpr int kThreads = 64, pushes = 250'000;
#else
    constexpr int kThreads = 16, pushes = 30'000;
#endif
    LockFreeQueue<int> q;
    std::atomic<std::size_t> enq{0};

    std::vector<std::thread> tp;
    for (int i = 0; i < kThreads; ++i)
        tp.emplace_back([&]{
            for (int j = 0; j < pushes; ++j) {
                q.enqueue(j); enq.fetch_add(1, std::memory_order_relaxed);
            }
        });

    expect_progress(enq, std::chrono::seconds(2), "enqueue");
    for (auto& t : tp) t.join();
    announce("T4: live-lock", "DONE");
}

static_assert(!std::is_copy_constructible_v<LockFreeQueue<int>>);

/* ── main ─────────────────────────────────────────────────────────*/
int main()
{
    try {
        test_atomic_correctness();
        test_atomic_correctness_new();
        // test_aba_uaf();
        test_destructor_safe();
        // test_livelock();
        std::puts("\nALL TESTS PASSED 🎉");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "\nTEST FAILURE: %s\n", ex.what());
        return 1;
    }
}
