// lockfree_queue_tests.cpp
//
// Build (pick one):
//   ─ ThreadSanitizer ────────────────────────────────────────────────
//   g++ -std=c++20 -O1 -g -fsanitize=thread   -pthread lockfree_queue_tests.cpp
//
//   ─ AddressSanitizer ───────────────────────────────────────────────
//   g++ -std=c++20 -O1 -g -fsanitize=address -fno-omit-frame-pointer \
//       -pthread lockfree_queue_tests.cpp
//
//   (Both should print “All tests PASSED 🎉” and exit 0.)
//
// NOTE:  **Do NOT** also compile/​link the queue on the command line;
//        this TU includes the full implementation via the header.

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
