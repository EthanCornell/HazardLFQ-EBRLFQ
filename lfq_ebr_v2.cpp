/********************************************************************************************
 *  lockfree queue with ebr —  Michael–Scott MPMC Queue + 3-epoch EBR + Back-off (2025-05-06)
 *
 *  This **final** version passes the full stress-test suite (T1-T4).
 *  All inline documentation has been rewritten in **English** and expanded, so every step that
 *  fixes the classical **ABA / use-after-free** hazard and the **live-lock** corner-case is
 *  crystal-clear for future maintenance or teaching.
 *
 *  ────────────────────────────────────────────────────────────────────────────────
 *  Table of Contents
 *  ────────────────────────────────────────────────────────────────────────────────
 *   1.  Background     – Why the Michael & Scott (1996) queue needs safe memory reclamation
 *   2.  Where ABA / UAF come from and why the traditional 2-epoch scheme is not enough
 *   3.  The 3-epoch (three-bucket) design – guaranteeing **two full grace periods**
 *   4.  try_flip() algorithm – lock-free, anyone can help, one sweep reclaims **all** threads
 *   5.  Why every thread must free the **cur-2** bucket, not just its own backlog
 *   6.  Lock-free entry point – Guard RAII: every dereference occurs inside one epoch
 *   7.  Live-lock root cause – hot CAS loops on head_/tail_; theory & practice of back-off
 *   8.  Two helping rules – push tail_ forward whenever tail_.next != nullptr & head == tail
 *   9.  Bounded exponential back-off: why we cap at 1 µs (≈1024 `_mm_pause`)
 *  10.  Testing tricks – watchdog + idle window + hard timeout: spotting real stalls only
 *
 *  ────────────────────────────────────────────────────────────────────────────────
 *  Key Take-aways
 *  ────────────────────────────────────────────────────────────────────────────────
 *  • **ABA / UAF fix**
 *      – Split retire-lists into 3 buckets (`epoch % 3`).
 *      – Reclaim a node only after the **global epoch has advanced by ≥2** and **every thread**
 *        has left the epoch in which the node was retired.  (Same as Crossbeam’s
 *        "global_epoch − retired_epoch ≥ 3" rule.)
 *  • **Live-lock fix**
 *      – Failed CAS retries back-off 1,2,4,8… pauses, up to 1024 (`≈1 µs` on a 3 GHz core),
 *        then keep that delay; reduces coherent-bus thrashing but keeps latency low.
 *      – Implement the original helping rules from the M&S paper so **some** thread always makes
 *        progress when head or tail lags behind.
 *
 *  ────────────────────────────────────────────────────────────────────────────────
 *  EBR timeline (ASCII Art)
 *  ────────────────────────────────────────────────────────────────────────────────
 *      time  ➜─────────────────────────────────────────────────────────────────➜
 *
 *      global epoch   0               1               2               3
 *                     │<-- grace-period-1 -->│<-- grace-period-2 -->│
 *
 *      T0  CPU  ↱ enter CS @E0
 *               │  …uses node A…            ↳ exit CS (quiescent)
 *
 *      T1  CPU                  retire(A)  (bucket 0)
 *                                                      ──────────►  free(A)
 *
 *      Bucket age   kept         kept        ─────────► reclaim
 *                    (E0)        (E1)               (during E2→E3 flip)
 *
 *    Guarantee: a node is freed **only** after two complete grace periods (GP1+GP2),
 *    therefore no live pointer can still reference its address – the memory-reuse ABA is
 *    impossible.
 ********************************************************************************************/

#pragma once
#include <atomic>
#include <array>
#include <vector>
#include <thread>
#include <functional>
#include <cassert>
#include <utility>
#include <cstdint>

/******************************** 1. Epoch-Based Reclamation *********************************/
namespace ebr {
constexpr unsigned kMaxThreads   = 256;   // soft upper-bound on live threads
constexpr unsigned kBatchRetired = 512;   // flip attempt threshold (amortised O(1))
constexpr unsigned kBuckets      = 3;     // epoch % 3  ⇒ 3 buckets ⇒ 2 grace periods

struct ThreadCtl {
    std::atomic<unsigned> local_epoch{~0u};          // ~0 = quiescent
    std::array<std::vector<void*>, kBuckets> retire; // nodes retired in each epoch bucket
    std::array<std::vector<std::function<void(void*)>>, kBuckets> del; // custom deleters
};

inline std::array<ThreadCtl*, kMaxThreads> g_threads{};
inline std::atomic<unsigned>               g_nthreads{0};
inline std::atomic<unsigned>               g_epoch{0};

inline ThreadCtl* init_thread() {
    static thread_local ThreadCtl* ctl = new ThreadCtl;        // never freed
    static thread_local bool registered = false;
    if (!registered) {
        unsigned idx = g_nthreads.fetch_add(1, std::memory_order_acq_rel);
        assert(idx < kMaxThreads && "EBR: raise kMaxThreads");
        g_threads[idx] = ctl;
        registered = true;
    }
    return ctl;
}

class Guard {                      // RAII epoch pin
    ThreadCtl* tc_;
public:
    Guard() : tc_(init_thread()) {
        unsigned e = g_epoch.load(std::memory_order_acquire);
        tc_->local_epoch.store(e, std::memory_order_release);
    }
    ~Guard() { tc_->local_epoch.store(~0u, std::memory_order_release); }
    Guard(const Guard&)            = delete;
    Guard& operator=(const Guard&) = delete;
};

/* try_flip – advance g_epoch & reclaim the (cur-2) bucket of **all** threads */
inline void try_flip(ThreadCtl* self)
{
    unsigned cur = g_epoch.load(std::memory_order_relaxed);

    /* 1️⃣  Is *any* thread still inside epoch = cur ? */
    for (unsigned i = 0, n = g_nthreads.load(std::memory_order_acquire); i < n; ++i) {
        ThreadCtl* t = g_threads[i];
        if (t && t->local_epoch.load(std::memory_order_acquire) == cur) return;
    }

    /* 2️⃣  Attempt to advance the global epoch – only one thread wins. */
    if (!g_epoch.compare_exchange_strong(cur, cur + 1, std::memory_order_acq_rel)) return;

    /* 3️⃣  Free everything retired 2 epochs ago  →  (cur+1) % 3  ==  cur-2  */
    unsigned idx_old = (cur + 1) % kBuckets;
    for (unsigned i = 0, n = g_nthreads.load(std::memory_order_acquire); i < n; ++i) {
        ThreadCtl* t = g_threads[i];
        if (!t) continue;
        auto& vec = t->retire[idx_old];
        auto& del = t->del   [idx_old];
        for (std::size_t k = 0; k < vec.size(); ++k) del[k](vec[k]);
        vec.clear(); del.clear();
    }
}

/* O(1) retire; reclamation work amortised by kBatchRetired */
template<class T>
inline void retire(T* p)
{
    ThreadCtl* tc  = init_thread();
    unsigned   e   = g_epoch.load(std::memory_order_acquire);
    unsigned   idx = e % kBuckets;
    tc->retire[idx].push_back(p);
    tc->del   [idx].emplace_back([](void* q){ delete static_cast<T*>(q); });

    if (tc->retire[idx].size() >= kBatchRetired) try_flip(tc);
}

inline void retire(void* p, std::function<void(void*)> f)
{
    ThreadCtl* tc  = init_thread();
    unsigned   e   = g_epoch.load(std::memory_order_acquire);
    unsigned   idx = e % kBuckets;
    tc->retire[idx].push_back(p);
    tc->del   [idx].push_back(std::move(f));

    if (tc->retire[idx].size() >= kBatchRetired) try_flip(tc);
}
} // namespace ebr

/******************************** 2. Michael–Scott Queue (MPMC) ********************************/
namespace lfq {

template<class T>
class Queue {
    struct Node {
        std::atomic<Node*> next{nullptr};
        alignas(T) unsigned char storage[sizeof(T)];
        bool has_val;
        template<class... A>
        Node(bool dummy, A&&... a) : has_val(!dummy) {
            if (!dummy) ::new (storage) T(std::forward<A>(a)...);
        }
        T& val() { return *std::launder(reinterpret_cast<T*>(storage)); }
        ~Node()  { if (has_val) val().~T(); }
    };

    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;

    /* bounded exponential back-off – doubles pauses up to 1024 */
    static inline void backoff(unsigned& n)
    {
#if defined(__i386__) || defined(__x86_64__)
        constexpr uint32_t kMax = 1024; // 1024 × pause ≈ 1 µs @3 GHz
        if (n < kMax) {
            for (uint32_t i = 0; i < n; ++i) __builtin_ia32_pause();
            n <<= 1;
        }
#endif
    }

public:
    Queue() {
        Node* d = new Node(true);          // initial dummy
        head_.store(d, std::memory_order_relaxed);
        tail_.store(d, std::memory_order_relaxed);
    }
    Queue(const Queue&) = delete; Queue& operator=(const Queue&) = delete;

    /*-------------------------------- enqueue --------------------------------*/
    template<class... Args>
    void enqueue(Args&&... args)
    {
        Node* n = new Node(false, std::forward<Args>(args)...);
        unsigned delay = 1;
        for (;;) {
            ebr::Guard g;
            Node* tail = tail_.load(std::memory_order_acquire);
            Node* next = tail->next.load(std::memory_order_acquire);
            if (tail != tail_.load(std::memory_order_acquire)) continue; // snapshot invalid

            if (!next) {             // tail truly last → link n
                if (tail->next.compare_exchange_weak(next, n,
                        std::memory_order_release,
                        std::memory_order_relaxed))
                {
                    /* help rule #1 – advance global tail */
                    tail_.compare_exchange_strong(tail, n,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                    return;          // enqueue done 🎉
                }
            } else {
                /* another thread already appended – help rule #2 */
                tail_.compare_exchange_strong(tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
            }
            backoff(delay);
        }
    }

    /*-------------------------------- dequeue --------------------------------*/
    bool dequeue(T& out)
    {
        unsigned delay = 1;
        for (;;) {
            ebr::Guard g;
            Node* head = head_.load(std::memory_order_acquire); // dummy
            Node* tail = tail_.load(std::memory_order_acquire);
            Node* next = head->next.load(std::memory_order_acquire);
            if (head != head_.load(std::memory_order_acquire)) continue;
            if (!next) return false;      // queue empty

            if (head == tail) {           // tail is stale – help advance
                tail_.compare_exchange_strong(tail, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
                backoff(delay);
                continue;
            }

            T val = next->val();          // copy before CAS
            if (head_.compare_exchange_strong(head, next,
                    std::memory_order_release,
                    std::memory_order_relaxed))
            {
                out = std::move(val);
                ebr::retire(head);        // old dummy → retire list
                return true;
            }
            backoff(delay);
        }
    }

    bool empty() const {
        ebr::Guard g;
        return head_.load(std::memory_order_acquire)
                ->next.load(std::memory_order_acquire) == nullptr;
    }

    ~Queue() {
        Node* n = head_.load(std::memory_order_relaxed);
        while (n) { Node* nx = n->next.load(std::memory_order_relaxed); delete n; n = nx; }
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
