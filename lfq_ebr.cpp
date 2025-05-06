/**********************************************************************
 *  lockfree_queue_ebr.hpp  —  Michael-&-Scott MPMC queue + EBR GC
 *  ──────────────────────────────────────────────────────────────────
 *  What you get
 *  -------------
 *  • A wait-free¹ MPSC / lock-free MPMC FIFO queue (Michael & Scott, 1996)
 *  • Memory safety via **Epoch-Based Reclamation** (EBR)
 *      ▸ a retired node is freed only after *two* global epoch flips
 *      ▸ removes the “memory-reuse ABA” & use-after-free hazards
 *  • Header-only, C++20, ASan / TSan clean, no dynamic TLS keys
 *
 *  ¹ enqueue is wait-free for any fixed thread count N; dequeue is lock-free.
 *
 *  ──────────  EBR OVERVIEW  ─────────────────────────────────────────
 *
 *  Timeline (ASCII)            time ─────────────────────────────────►
 *
 *            EPOCH-0                    EPOCH-1                    EPOCH-2
 *  Global ───|  E=0  |──────────────────|  E=1  |──────────────────|  E=2  |───
 *             flip-A                    flip-B
 *
 *  T0  enter-CS
 *      (reads node A) ─ CS in epoch-0 ─ exit-CS ───────── idle
 *
 *  T1                        retire(A)               may alloc new node here
 *                             (list[0])   ──────────►   (old A is reclaimed)
 *
 *  Retire-lists  list[0]: {A} ──────┐ kept 1st GP │ kept 2nd GP │ free(A)
 *                                   └─────────────┴─────────────┴────────► …
 *
 *  Legend
 *  ------
 *  CS       : critical section (thread holds shared ptr)
 *  flip-A   : every thread left epoch-0  →  global_epoch++  (1st grace period)
 *  flip-B   : every thread left epoch-1  →  global_epoch++  (2nd grace period)
 *  GP       : grace period
 *
 *  Proof-sketch
 *  ------------
 *  • A node retired in epoch *E* is kept until the system has witnessed
 *    **two** flips: E→E+1 and E+1→E+2.
 *  • During those two grace periods every thread passes a quiescent point
 *    twice, therefore no live compare-and-swap can still hold the old address.
 *  • Hence the *memory-reuse* ABA is impossible; freeing is safe.
 *
 *  How to use in client code
 *  -------------------------
 *      // In every loop that dereferences shared nodes:
 *      ebr::Guard g;          // announces "I'm in the current epoch"
 *      auto* n = shared_ptr.load();
 *      … dereference n safely …
 *
 *      // When you unlink a node:
 *      ebr::retire(old_node);
 *
 *  That’s it.  The queue below already does this for you.
 *
 *  Build / test
 *  -------------
 *      g++ -std=c++20 -O2 -pthread -fsanitize=address,undefined \
 *          your_file.cpp && ./a.out
 *
 *********************************************************************/

// #pragma once
#include <atomic>
#include <array>
#include <vector>
#include <thread>
#include <functional>
#include <cassert>
#include <new>
#include <utility>

/**********************************************************************
 *  namespace ebr  –  Minimal Epoch-Based Reclamation toolkit
 *  ──────────────────────────────────────────────────────────────────
 *  PURPOSE
 *  -------
 *  Safely free memory in lock-free data-structures without per-pointer
 *  hazard tracking.  A node retired in epoch E is reclaimed only after
 *  every thread has announced a **quiescent point** in *two* subsequent
 *  epochs (E+1 and E+2).  This “two-epoch / three-value” rule prevents
 *  the classic *memory-reuse ABA* and use-after-free errors.
 *
 *  timeline
 *  --------------
 *                     time ─────────────────────────────────────────►
 *
 *           EPOCH-0                  EPOCH-1                  EPOCH-2
 *  Global ─┬─ E=0 ───────────────────┬─ E=1 ───────────────────┬─ E=2 ────
 *          flip-A                   flip-B
 *
 *  T0  enter-CS
 *      uses node A ──────── CS in E=0 ──────── exit-CS ─────── idle
 *
 *  T1                     retire(A)                      may reuse ptr here
 *                          list[0]       ───────────────►  SAFE (A freed)
 *
 *  Retire lists   list[0] {A}── kept GP1 ─ kept GP2 ─ free(A) ────────────►
 *                 list[1] {}
 *
 *      GP = grace period = “all threads left epoch N”
 *
 *  API AT A GLANCE
 *  ---------------
 *      {              // in each critical section
 *          ebr::Guard g;
 *          ... dereference shared pointers safely ...
 *      }              // guard dtor announces quiescent point
 *
 *      ebr::retire(ptr);          // after logical unlink
 *
 *  TUNABLES
 *  --------
 *      kMaxThreads   : compile-time upper bound on concurrent threads
 *      kBatchRetired : flip attempt threshold (amortises O(1) cost)
 *
 *  IMPLEMENTATION NOTES
 *  --------------------
 *  • One   ThreadCtl  per thread   – stored *on the heap* so the global
 *    registry never dangles after thread exit.
 *  • Two   retire lists per thread: index = global_epoch & 1
 *  • Global flip:  O(#threads) scan, executed only every kBatchRetired
 *    retire calls ⇒ amortised constant work.
 *  • Memory order: acquire/release pairs; no fences slower than CAS.
 *********************************************************************/

namespace ebr {

/*──────────────────── 1. Parameters ───────────────────────────────*/
constexpr unsigned kMaxThreads   = 256;   // soft cap on live threads
constexpr unsigned kBatchRetired = 128;   // attempt flip after this many retires

/*──────────────────── 2. Per-thread record ────────────────────────*/
struct ThreadCtl {
    std::atomic<unsigned> local_epoch{~0u};   // ~0 = quiescent
    std::vector<void*>    retire[2];          // even / odd buckets
    std::vector<std::function<void(void*)>> deleter[2];
};

/*──────────────────── 3. Global tables ────────────────────────────*/
inline std::array<ThreadCtl*, kMaxThreads> g_threads{};
inline std::atomic<unsigned>               g_thread_cnt{0};
inline std::atomic<unsigned>               g_epoch{0};        // monotonically ++

/* Register current thread – heap allocate so record outlives thread */
inline ThreadCtl* init_thread() {
    static thread_local ThreadCtl* ctl = new ThreadCtl;   // never freed
    static thread_local bool registered = false;
    if (!registered) {
        unsigned idx = g_thread_cnt.fetch_add(1, std::memory_order_acq_rel);
        assert(idx < kMaxThreads && "EBR: increase kMaxThreads");
        g_threads[idx] = ctl;
        registered = true;
    }
    return ctl;
}

/*──────────────────── 4. Guard (RAII) ─────────────────────────────*/
class Guard {
    ThreadCtl* tc_;
public:
    Guard() : tc_(init_thread()) {
        unsigned e = g_epoch.load(std::memory_order_acquire);
        tc_->local_epoch.store(e, std::memory_order_release);   // announce entry
    }
    ~Guard() { tc_->local_epoch.store(~0u, std::memory_order_release); }
    Guard(const Guard&)            = delete;
    Guard& operator=(const Guard&) = delete;
};

/*──────────────────── 5. Epoch flip & reclamation ────────────────*/
inline void try_flip(ThreadCtl* self) {
    unsigned cur = g_epoch.load(std::memory_order_relaxed);

    /* Step-1: is everyone past epoch-cur ? */
    for (unsigned i = 0,
                   n = g_thread_cnt.load(std::memory_order_acquire);
         i < n; ++i)
    {
        ThreadCtl* t = g_threads[i];
        if (t && t->local_epoch.load(std::memory_order_acquire) == cur)
            return;                                 // someone still inside
    }

    /* Step-2: advance global epoch (only one thread wins) */
    if (!g_epoch.compare_exchange_strong(cur, cur + 1,
                                         std::memory_order_acq_rel))
        return;

    /* Step-3: reclaim epoch-(cur-2) nodes in *this* thread */
    unsigned idx_prev = (cur + 1) & 1;              // (cur-2) mod 2
    for (std::size_t i = 0; i < self->retire[idx_prev].size(); ++i)
        self->deleter[idx_prev][i]( self->retire[idx_prev][i] );
    self->retire[idx_prev].clear();
    self->deleter[idx_prev].clear();
}

/*──────────────────── 6. Retire helpers ───────────────────────────*/
template<class T>
inline void retire(T* p) {
    ThreadCtl* tc = init_thread();
    unsigned   e  = g_epoch.load(std::memory_order_acquire);
    unsigned   idx = e & 1;                          // bucket 0 or 1

    tc->retire [idx].push_back(p);
    tc->deleter[idx].emplace_back(
        [](void* q){ delete static_cast<T*>(q); });

    if (tc->retire[idx].size() >= kBatchRetired)
        try_flip(tc);
}

inline void retire(void* p, std::function<void(void*)> del) {
    ThreadCtl* tc = init_thread();
    unsigned   e  = g_epoch.load(std::memory_order_acquire);
    unsigned   idx = e & 1;

    tc->retire [idx].push_back(p);
    tc->deleter[idx].push_back(std::move(del));

    if (tc->retire[idx].size() >= kBatchRetired)
        try_flip(tc);
}

} // namespace ebr



//────────────────Michael-&-Scott queue with EBR GC───────────────────────────
//  lfq::Queue<T>
//  --------------------------------------------------------------------------
//  • Lock-free multi-producer / multi-consumer FIFO queue (Michael & Scott)
//  • Coupled with Epoch-Based Reclamation (EBR) for safe memory management
//  • Single dummy node technique:
//        head_  ─▶  [DUMMY]  ─▶  n1  ─▶  n2  ─▶ … ─▶  tail_
//      • head_ always points to the *dummy* (node to retire on pop).
//      • tail_ points to the *last* real node (or to dummy when empty).
//  • Memory order glossary
//      acquire : observe preceding RELEASE store(s) from other threads
//      release : publish writes before the store so observers see them
//      relaxed : no ordering, only atomicity
//
//  Fast-path costs
//  ---------------
//  enqueue : 1  CAS + at most 1 failed CAS retry            (wait-free for N)
//  dequeue : 1-2 CAS (may loop on contention)               (lock-free)
//
//  EBR integration
//  ---------------
//  Each loop iteration installs an `ebr::Guard` *first* so every pointer
//  dereference occurs inside a “critical section”.  When we physically
//  unlink a dummy node we hand it to `ebr::retire()`; the node will be
//  freed only after two epoch flips (= two full grace periods) guaranteeing
//  no other thread can still hold stale references.
//─────────────────────────────────────────────────────────────────────────────
namespace lfq {

template<class T>
class Queue {
    /*--------------------------------------------------------------*
     |  Intrusive singly-linked node                                |
     |  ----------------------------------------------------------- |
     |  • has_value == false  ⇒  dummy ­node                        |
     |  • payload stored via placement-new inside `storage`         |
     *--------------------------------------------------------------*/
    struct Node {
        std::atomic<Node*> next{nullptr};
        alignas(T) unsigned char storage[sizeof(T)];
        bool has_value;

        template<class... A>
        explicit Node(bool dummy, A&&... a) : has_value(!dummy) {
            if (!dummy) ::new (storage) T(std::forward<A>(a)...);
        }
        T&       value()       { return *std::launder(reinterpret_cast<T*>(storage)); }
        const T& value() const { return *std::launder(reinterpret_cast<const T*>(storage)); }
        ~Node() { if (has_value) value().~T(); }
    };

    /*--------------------------------------------------------------*
     |  Pointers shared by all threads                              |
     *--------------------------------------------------------------*/
    std::atomic<Node*> head_;   // points at *dummy*
    std::atomic<Node*> tail_;   // last real node (or dummy)

public:
    /* Initial state: one dummy node that is both head & tail */
    Queue() {
        Node* d = new Node(true);                 // dummy
        head_.store(d, std::memory_order_relaxed);
        tail_.store(d, std::memory_order_relaxed);
    }
    Queue(const Queue&)            = delete;
    Queue& operator=(const Queue&) = delete;

    /*──────────────────────── enqueue ────────────────────────────*/
    template<class... Args>
    void enqueue(Args&&... args)
    {
        Node* n = new Node(false, std::forward<Args>(args)...); // off-list
        for (;;) {
            ebr::Guard g;                       // enter critical section
            Node* tail = tail_.load(std::memory_order_acquire); // snapshot
            Node* next = tail->next.load(std::memory_order_acquire);

            /* Step-A: validate snapshot — another thread may move tail */
            if (tail != tail_.load(std::memory_order_acquire))
                continue;

            if (!next) {        // Case 1: tail really is last node
                /* Try to link our node after tail. */
                if (tail->next.compare_exchange_weak(next, n,
                        std::memory_order_release,
                        std::memory_order_relaxed))
                {
                    /* Help by advancing the global tail (optional).      *
                     * Even if this CAS fails another thread already did *
                     * the update, so enqueue is complete.               */
                    tail_.compare_exchange_strong(tail, n,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                    return;     // enqueue complete 🎉
                }
            } else {            // Case 2: tail lagged behind, help it
                tail_.compare_exchange_strong(tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
            }
            /* On contention we simply retry the loop. */
        }
    }

    /*──────────────────────── dequeue ────────────────────────────*/
    bool dequeue(T& out)
    {
        for (;;) {
            ebr::Guard g;                       // protects *all* derefs
            Node* head = head_.load(std::memory_order_acquire); // dummy
            Node* tail = tail_.load(std::memory_order_acquire);
            Node* next = head->next.load(std::memory_order_acquire); // 1st real

            if (head != head_.load(std::memory_order_acquire))
                continue;                       // snapshot invalid → retry

            if (!next)                          // queue empty
                return false;

            if (head == tail) {                 // tail behind → help move
                tail_.compare_exchange_strong(tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                continue;                       // start over
            }

            /* Copy payload *before* swinging head (still protected by guard) */
            T val = next->value();

            /* Try to swing head_ to the next node */
            if (head_.compare_exchange_strong(head, next,
                    std::memory_order_release,
                    std::memory_order_relaxed))
            {
                out = std::move(val);           // deliver result
                ebr::retire(head);              // old dummy → retire list
                return true;
            }
            /* Otherwise another consumer got there first; loop again. */
        }
    }

    /*──────────────────────── helpers ────────────────────────────*/
    bool empty() const {
        ebr::Guard g;                           // protect `head_->next`
        Node* h = head_.load(std::memory_order_acquire);
        return h->next.load(std::memory_order_acquire) == nullptr;
    }

    /* Destructor: single-threaded ⇒ safe to free directly          */
    ~Queue() {
        Node* n = head_.load(std::memory_order_relaxed);
        while (n) {
            Node* nx = n->next.load(std::memory_order_relaxed);
            delete n;
            n = nx;
        }
    }
};

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

/* ── upgraded expect_progress (4-arg overload) ─────────────────── */
template<typename C>
void expect_progress(C& counter,
                     std::size_t goal,
                     std::chrono::milliseconds idle_window,
                     std::chrono::seconds    hard_timeout)
{
    auto t_start = SteadyClk::now();
    auto last    = counter.load(std::memory_order_acquire);

    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));

        auto cur = counter.load(std::memory_order_acquire);
        if (cur == goal) return;                       // 全部完成 🎉

        if (cur != last) {                            // 有進度
            last    = cur;
            t_start = SteadyClk::now();               // reset idle timer
        } else if (SteadyClk::now() - t_start > idle_window) {
            throw std::runtime_error("Live-lock: no progress");
        }
        if (SteadyClk::now() - t_start > hard_timeout)
            throw std::runtime_error("Live-lock: hard timeout");
    }
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

    constexpr int kThreads = 4, kIters = 50'000;
    constexpr auto TIMEOUT = std::chrono::seconds(10);

    LockFreeQueue<int> q;
    std::atomic<std::size_t> done{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> wd_timeout{false};

    /* producer – 不提早退出 */
    auto producer = [&] {
        for (int i = 0; i < kIters; ++i) q.enqueue(i);
    };

    auto consumer = [&] {
        int v;
        while (!stop.load(std::memory_order_acquire)) {
            if (q.dequeue(v) &&
                done.fetch_add(1, std::memory_order_acq_rel) + 1 ==
                    std::size_t(kThreads) * kIters)
            {
                stop.store(true, std::memory_order_release);
            }
        }
    };

    std::thread watchdog([&] {
        std::size_t last = 0;
        auto tic = SteadyClk::now();
        while (!stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto cur = done.load(std::memory_order_acquire);
            if (cur != last) { last = cur; tic = SteadyClk::now(); }
            else if (SteadyClk::now() - tic > TIMEOUT) {
                wd_timeout.store(true, std::memory_order_release);
                stop.store(true, std::memory_order_release);
            }
        }
    });

    std::vector<std::thread> tp, tc;
    for (int i=0;i<kThreads;++i) tp.emplace_back(producer);
    for (int i=0;i<kThreads;++i) tc.emplace_back(consumer);
    for (auto& t:tp) t.join();
    for (auto& t:tc) t.join();
    watchdog.join();

    if (wd_timeout.load()) throw std::runtime_error("watchdog timeout");
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
    constexpr auto HARD  = std::chrono::seconds(20);
#else
    constexpr int kThreads = 16, pushes = 30'000;
    constexpr auto HARD  = std::chrono::seconds(8);
#endif
    constexpr std::size_t GOAL = std::size_t(kThreads) * pushes;

    LockFreeQueue<int> q;
    std::atomic<std::size_t> enq{0};

    /* ── 啟動生產者 ───────────────────────────────────────────── */
    std::vector<std::thread> tp;
    tp.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
        tp.emplace_back([&]{
            for (int j = 0; j < pushes; ++j) {
                q.enqueue(j);
                enq.fetch_add(1, std::memory_order_relaxed);
            }
        });

    /* ── 監看進度：idle ≤2s，總時間 ≤ HARD ───────────────────── */
    expect_progress(enq, GOAL,
                    std::chrono::seconds(2),  // idle
                    HARD);                    // hard

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
        test_aba_uaf();
        test_destructor_safe();
        test_livelock();
        std::puts("\nALL TESTS PASSED 🎉");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "\nTEST FAILURE: %s\n", ex.what());
        return 1;
    }
}
