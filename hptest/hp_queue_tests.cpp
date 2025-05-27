/**********************************************************************
 *  hp_queue_tests.cpp - Comprehensive Test Suite for Hazard Pointer Queue
 *  ──────────────────────────────────────────────────────────────────
 *  
 *  Tests the lock-free queue implementation with hazard pointer memory
 *  reclamation for correctness, performance, and memory safety.
 *  
 *  Build commands:
 *    # Basic build
 *    g++ -std=c++20 -O2 -pthread -I../include hp_queue_tests.cpp -o hp_test
 *    
 *    # ThreadSanitizer build
 *    g++ -std=c++20 -O3 -g -fsanitize=thread -pthread -I../include hp_queue_tests.cpp -o hp_test_tsan
 *    
 *    # AddressSanitizer build  
 *    g++ -std=c++20 -O3 -g -fsanitize=address -fno-omit-frame-pointer -pthread -I../include hp_queue_tests.cpp -o hp_test_asan
 *    
 *    # Stress tests (larger workloads)
 *    g++ -std=c++20 -O2 -pthread -DSTRESS_TESTS -I../include hp_queue_tests.cpp -o hp_test_stress
 *    
 *  Usage:
 *    ./hp_test           # Run all tests
 *    ./hp_test_tsan      # Run with thread sanitizer
 *    ./hp_test_asan      # Run with address sanitizer
 *********************************************************************/

#include "lockfree_queue_hp.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cassert>
#include <random>
#include <iomanip>
#include <algorithm>
#include <functional>
#include <string>
#include <sstream>
#include <barrier>

// Test configuration
#ifndef STRESS_TESTS
constexpr int SMALL_THREADS = 8;
constexpr int SMALL_ITERS   = 20'000;
constexpr int MEDIUM_THREADS = 16;
constexpr int MEDIUM_ITERS  = 50'000;
#else
constexpr int SMALL_THREADS = 16;
constexpr int SMALL_ITERS   = 100'000;
constexpr int MEDIUM_THREADS = 32;
constexpr int MEDIUM_ITERS  = 200'000;
#endif

// Timing utilities
using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;
using TimePoint = Clock::time_point;

// Test result formatting
class TestResults {
public:
    void printHeader(const std::string& testName) {
        std::cout << "====================================================\n";
        std::cout << "  " << testName << "\n";
        std::cout << "====================================================\n";
    }

    void printResult(const std::string& operation, double opsPerSec, double timeInSec) {
        std::cout << std::left << std::setw(20) << operation 
                  << std::setw(15) << std::fixed << std::setprecision(2) << opsPerSec 
                  << " ops/sec  "
                  << std::setw(10) << std::fixed << std::setprecision(6) << timeInSec 
                  << " sec\n";
    }

    void printSuccess(const std::string& message) {
        std::cout << "✓ " << message << "\n";
    }

    void printFailure(const std::string& message) {
        std::cerr << "✗ " << message << "\n";
    }
};

inline void announce(const char* name, const char* phase) {
    std::printf("[%-28s] %s\n", name, phase);
}

// Progress monitoring utility (detects live-locks)
template<typename Counter>
void expectProgress(Counter& counter, 
                   std::size_t goal,
                   std::chrono::milliseconds idleWindow,
                   std::chrono::seconds hardTimeout)
{
    auto startTime = Clock::now();
    auto lastValue = counter.load(std::memory_order_acquire);

    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));

        auto currentValue = counter.load(std::memory_order_acquire);
        if (currentValue >= goal) return;  // Goal reached

        if (currentValue != lastValue) {
            // Progress detected - reset idle timer
            lastValue = currentValue;
            startTime = Clock::now();
        } else if (Clock::now() - startTime > idleWindow) {
            throw std::runtime_error("Live-lock detected: no progress for " + 
                                   std::to_string(idleWindow.count()) + "ms");
        }
        
        if (Clock::now() - startTime > hardTimeout) {
            throw std::runtime_error("Hard timeout reached after " + 
                                   std::to_string(hardTimeout.count()) + "s");
        }
    }
}

// Custom test value class for leak detection
class TestValue {
private:
    int value_;
    static std::atomic<int> constructCount_;
    static std::atomic<int> destroyCount_;
    
public:
    explicit TestValue(int val = 0) : value_(val) {
        constructCount_.fetch_add(1, std::memory_order_relaxed);
    }
    
    TestValue(const TestValue& other) : value_(other.value_) {
        constructCount_.fetch_add(1, std::memory_order_relaxed);
    }
    
    TestValue(TestValue&& other) noexcept : value_(other.value_) {
        constructCount_.fetch_add(1, std::memory_order_relaxed);
    }
    
    TestValue& operator=(const TestValue& other) {
        value_ = other.value_;
        return *this;
    }
    
    TestValue& operator=(TestValue&& other) noexcept {
        value_ = other.value_;
        return *this;
    }
    
    ~TestValue() {
        destroyCount_.fetch_add(1, std::memory_order_relaxed);
    }
    
    int value() const { return value_; }
    
    static void resetCounts() {
        constructCount_.store(0, std::memory_order_relaxed);
        destroyCount_.store(0, std::memory_order_relaxed);
    }
    
    static int getConstructCount() {
        return constructCount_.load(std::memory_order_relaxed);
    }
    
    static int getDestroyCount() {
        return destroyCount_.load(std::memory_order_relaxed);
    }
};

std::atomic<int> TestValue::constructCount_{0};
std::atomic<int> TestValue::destroyCount_{0};

//==============================================================================
// Test 1: Basic Functionality
//==============================================================================
void testBasicFunctionality() {
    TestResults results;
    results.printHeader("Basic Functionality Test");
    
    lfq::HPQueue<int> queue;
    
    // Test empty queue
    int value;
    bool success = queue.dequeue(value);
    assert(!success && "Dequeue from empty queue should fail");
    results.printSuccess("Empty queue check passed");
    
    // Test single enqueue/dequeue
    queue.enqueue(42);
    success = queue.dequeue(value);
    assert(success && value == 42 && "Single enqueue/dequeue failed");
    results.printSuccess("Single enqueue/dequeue passed");
    
    // Test FIFO ordering
    for (int i = 0; i < 10; i++) {
        queue.enqueue(i);
    }
    
    for (int i = 0; i < 10; i++) {
        success = queue.dequeue(value);
        assert(success && value == i && "FIFO ordering violated");
    }
    results.printSuccess("FIFO ordering verified");
    
    // Test empty after all dequeues
    success = queue.dequeue(value);
    assert(!success && "Queue should be empty after all items dequeued");
    results.printSuccess("Empty state after dequeue verified");
}

//==============================================================================
// Test 2: Single-threaded Performance
//==============================================================================
void testSingleThreadedPerformance() {
    TestResults results;
    results.printHeader("Single-Threaded Performance Test");
    
    lfq::HPQueue<int> queue;
    constexpr int COUNT = 100000;
    
    // Measure enqueue performance
    auto startTime = Clock::now();
    for (int i = 0; i < COUNT; i++) {
        queue.enqueue(i);
    }
    auto endTime = Clock::now();
    Duration enqueueTime = endTime - startTime;
    double enqueueOpsPerSec = COUNT / enqueueTime.count();
    results.printResult("Enqueue", enqueueOpsPerSec, enqueueTime.count());
    
    // Measure dequeue performance
    int value;
    int dequeueCount = 0;
    
    startTime = Clock::now();
    while (queue.dequeue(value)) {
        assert(value == dequeueCount && "Dequeue order violation");
        dequeueCount++;
    }
    endTime = Clock::now();
    Duration dequeueTime = endTime - startTime;
    double dequeueOpsPerSec = COUNT / dequeueTime.count();
    results.printResult("Dequeue", dequeueOpsPerSec, dequeueTime.count());
    
    assert(dequeueCount == COUNT && "Incorrect number of items dequeued");
    results.printSuccess("All items correctly processed");
}

//==============================================================================
// Test 3: Memory Management (Hazard Pointer effectiveness)
//==============================================================================
void testMemoryManagement() {
    TestResults results;
    results.printHeader("Memory Management Test (Hazard Pointers)");
    
    TestValue::resetCounts();
    
    {
        lfq::HPQueue<TestValue> queue;
        constexpr int N = 10000;
        
        // Fill queue
        for (int i = 0; i < N; i++) {
            queue.enqueue(TestValue(i));
        }
        
        // Drain queue (forces hazard pointer reclamation)
        TestValue tmp;
        while (queue.dequeue(tmp)) {
            // Process item
        }
        
        // Force hazard pointer scans by creating retirement pressure
        for (int i = 0; i < 1000; i++) {
            queue.enqueue(TestValue(i));
            queue.dequeue(tmp);
        }
    } // Queue destructor
    
    // Allow some time for cleanup
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    int constructed = TestValue::getConstructCount();
    int destroyed = TestValue::getDestroyCount();
    
    std::cout << "Constructed: " << constructed << ", Destroyed: " << destroyed << "\n";
    
    // Note: With hazard pointers, some objects might still be in retired lists
    // but the counts should be reasonably close
    double destructionRate = static_cast<double>(destroyed) / constructed;
    assert(destructionRate > 0.8 && "Too many objects not reclaimed");
    
    results.printSuccess("Memory management working (≥80% objects reclaimed)");
}

//==============================================================================
// Test 4: Multi-Producer Multi-Consumer
//==============================================================================
void testMultiProducerMultiConsumer() {
    TestResults results;
    results.printHeader("Multi-Producer Multi-Consumer Test");
    
    lfq::HPQueue<std::uint64_t> queue;
    constexpr int NUM_PRODUCERS = SMALL_THREADS;
    constexpr int NUM_CONSUMERS = SMALL_THREADS;
    constexpr int ITEMS_PER_PRODUCER = SMALL_ITERS / NUM_PRODUCERS;
    constexpr std::size_t TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
    
    std::atomic<std::size_t> totalProduced{0};
    std::atomic<std::size_t> totalConsumed{0};
    std::atomic<bool> producersDone{false};
    
    // Track uniqueness using 64-bit tickets
    std::vector<std::atomic<uint8_t>> seen(TOTAL_ITEMS);
    for (auto& flag : seen) {
        flag.store(0, std::memory_order_relaxed);
    }
    
    // Producer function
    auto producer = [&](int producerId) {
        for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
            std::uint64_t ticket = (static_cast<std::uint64_t>(producerId) << 32) | i;
            queue.enqueue(ticket);
            totalProduced.fetch_add(1, std::memory_order_acq_rel);
        }
    };
    
    // Consumer function
    auto consumer = [&]() {
        std::uint64_t ticket;
        while (!producersDone.load(std::memory_order_acquire) || 
               totalConsumed.load(std::memory_order_acquire) < TOTAL_ITEMS) {
            if (queue.dequeue(ticket)) {
                // Decode ticket
                std::size_t producerId = ticket >> 32;
                std::size_t itemId = ticket & 0xFFFFFFFF;
                std::size_t index = producerId * ITEMS_PER_PRODUCER + itemId;
                
                // Check uniqueness
                uint8_t expected = 0;
                if (!seen[index].compare_exchange_strong(expected, 1, 
                                                        std::memory_order_acq_rel)) {
                    throw std::logic_error("Duplicate item detected!");
                }
                
                totalConsumed.fetch_add(1, std::memory_order_acq_rel);
            } else {
                std::this_thread::yield();
            }
        }
    };
    
    auto startTime = Clock::now();
    
    // Launch producers
    std::vector<std::thread> producers;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        producers.emplace_back(producer, i);
    }
    
    // Launch consumers
    std::vector<std::thread> consumers;
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        consumers.emplace_back(consumer);
    }
    
    // Wait for producers
    for (auto& t : producers) {
        t.join();
    }
    producersDone.store(true, std::memory_order_release);
    
    // Wait for consumers
    for (auto& t : consumers) {
        t.join();
    }
    
    auto endTime = Clock::now();
    Duration elapsed = endTime - startTime;
    
    // Verify results
    assert(totalProduced.load() == TOTAL_ITEMS);
    assert(totalConsumed.load() == TOTAL_ITEMS);
    
    // Check all items were seen exactly once
    for (std::size_t i = 0; i < TOTAL_ITEMS; i++) {
        assert(seen[i].load() == 1 && "Item not consumed exactly once");
    }
    
    double throughput = (TOTAL_ITEMS * 2.0) / elapsed.count(); // Both enqueue and dequeue
    results.printResult("Total Throughput", throughput, elapsed.count());
    results.printSuccess("All items produced and consumed exactly once");
}

//==============================================================================
// Test 5: ABA and Use-After-Free Stress Test
//==============================================================================
void testABAAndUseAfterFree() {
    announce("T5: ABA/UAF stress", "START");
    
    constexpr int NUM_THREADS = 4;
    constexpr int OPERATIONS_PER_THREAD = 50000;
    constexpr auto TIMEOUT = std::chrono::seconds(10);
    
    lfq::HPQueue<int> queue;
    std::atomic<std::size_t> operationsCompleted{0};
    std::atomic<bool> shouldStop{false};
    std::atomic<bool> watchdogTimeout{false};
    
    // Producer threads
    auto producer = [&]() {
        for (int i = 0; i < OPERATIONS_PER_THREAD; i++) {
            queue.enqueue(i);
        }
    };
    
    // Consumer threads
    auto consumer = [&]() {
        int value;
        while (!shouldStop.load(std::memory_order_acquire)) {
            if (queue.dequeue(value)) {
                if (operationsCompleted.fetch_add(1, std::memory_order_acq_rel) + 1 == 
                    NUM_THREADS * OPERATIONS_PER_THREAD) {
                    shouldStop.store(true, std::memory_order_release);
                }
            }
        }
    };
    
    // Watchdog to detect stalls
    std::thread watchdog([&]() {
        std::size_t lastCount = 0;
        auto lastProgressTime = Clock::now();
        
        while (!shouldStop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
            auto currentCount = operationsCompleted.load(std::memory_order_acquire);
            if (currentCount != lastCount) {
                lastCount = currentCount;
                lastProgressTime = Clock::now();
            } else if (Clock::now() - lastProgressTime > TIMEOUT) {
                watchdogTimeout.store(true, std::memory_order_release);
                shouldStop.store(true, std::memory_order_release);
                break;
            }
        }
    });
    
    // Launch test threads
    std::vector<std::thread> producers, consumers;
    for (int i = 0; i < NUM_THREADS; i++) {
        producers.emplace_back(producer);
        consumers.emplace_back(consumer);
    }
    
    // Wait for completion
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();
    watchdog.join();
    
    if (watchdogTimeout.load()) {
        throw std::runtime_error("Watchdog timeout - possible live-lock or ABA issue");
    }
    
    announce("T5: ABA/UAF stress", "DONE");
}

//==============================================================================
// Test 6: High Contention Test
//==============================================================================
void testHighContention() {
    TestResults results;
    results.printHeader("High Contention Test");
    
    lfq::HPQueue<int> queue;
    constexpr int NUM_THREADS = MEDIUM_THREADS;
    constexpr int OPS_PER_THREAD = MEDIUM_ITERS / NUM_THREADS;
    
    std::atomic<int> enqueueSum{0};
    std::atomic<int> dequeueSum{0};
    std::atomic<int> expectedSum{0};
    
    // Pre-fill queue
    constexpr int PREFILL = 1000;
    for (int i = 0; i < PREFILL; i++) {
        queue.enqueue(i);
        expectedSum.fetch_add(i, std::memory_order_relaxed);
    }
    
    // Mixed producer/consumer threads
    auto worker = [&](int threadId) {
        std::mt19937 rng(threadId);
        
        for (int i = 0; i < OPS_PER_THREAD; i++) {
            int value = PREFILL + threadId * OPS_PER_THREAD + i;
            
            if (rng() % 2 == 0) {
                // Enqueue operation
                queue.enqueue(value);
                enqueueSum.fetch_add(value, std::memory_order_relaxed);
            } else {
                // Dequeue operation
                int dequeuedValue;
                if (queue.dequeue(dequeuedValue)) {
                    dequeueSum.fetch_add(dequeuedValue, std::memory_order_relaxed);
                }
            }
        }
    };
    
    auto startTime = Clock::now();
    
    std::vector<std::thread> workers;
    for (int i = 0; i < NUM_THREADS; i++) {
        workers.emplace_back(worker, i);
    }
    
    for (auto& t : workers) {
        t.join();
    }
    
    // Drain remaining items
    int remainingValue;
    while (queue.dequeue(remainingValue)) {
        dequeueSum.fetch_add(remainingValue, std::memory_order_relaxed);
    }
    
    auto endTime = Clock::now();
    Duration elapsed = endTime - startTime;
    
    // Calculate final expected sum
    int finalExpectedSum = expectedSum.load() + enqueueSum.load();
    
    std::cout << "Expected sum: " << finalExpectedSum << ", Actual sum: " << dequeueSum.load() << "\n";
    
    if (dequeueSum.load() == finalExpectedSum) {
        results.printSuccess("Sum verification passed - no data corruption");
    } else {
        results.printFailure("Sum mismatch detected!");
        assert(false);
    }
    
    double opsPerSec = (NUM_THREADS * OPS_PER_THREAD) / elapsed.count();
    results.printResult("Mixed Operations", opsPerSec, elapsed.count());
}

//==============================================================================
// Test 7: Live-lock Detection Test  
//==============================================================================
void testLiveLockDetection() {
    announce("T7: live-lock detection", "START");
    
#ifdef STRESS_TESTS
    constexpr int NUM_THREADS = 64;
    constexpr int PUSHES_PER_THREAD = 100000;
    constexpr auto HARD_TIMEOUT = std::chrono::seconds(20);
#else
    constexpr int NUM_THREADS = 16;
    constexpr int PUSHES_PER_THREAD = 30000;
    constexpr auto HARD_TIMEOUT = std::chrono::seconds(8);
#endif
    
    constexpr std::size_t GOAL = NUM_THREADS * PUSHES_PER_THREAD;
    
    lfq::HPQueue<int> queue;
    std::atomic<std::size_t> enqueueCounter{0};
    
    // Producer threads only (stress enqueue operations)
    std::vector<std::thread> producers;
    producers.reserve(NUM_THREADS);
    
    for (int i = 0; i < NUM_THREADS; i++) {
        producers.emplace_back([&, i]() {
            for (int j = 0; j < PUSHES_PER_THREAD; j++) {
                queue.enqueue(i * PUSHES_PER_THREAD + j);
                enqueueCounter.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    
    // Monitor progress to detect live-locks
    expectProgress(enqueueCounter, GOAL,
                  std::chrono::seconds(2),    // Idle window
                  HARD_TIMEOUT);              // Hard timeout
    
    for (auto& t : producers) {
        t.join();
    }
    
    announce("T7: live-lock detection", "DONE");
}

//==============================================================================
// Test 8: Destructor Safety Test
//==============================================================================
void testDestructorSafety() {
    announce("T8: destructor safety", "START");
    
    auto* queue = new lfq::HPQueue<int>;
    
    // Thread that waits for an item
    std::thread consumer([&]() {
        int value;
        while (!queue->dequeue(value)) {
            std::this_thread::yield();
        }
        // Got the item, thread completes
    });
    
    // Give the consumer a moment to start waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Enqueue an item to wake up the consumer
    queue->enqueue(42);
    
    // Wait for consumer to complete
    consumer.join();
    
    // Now safe to delete the queue
    delete queue;
    
    announce("T8: destructor safety", "DONE");
}

//==============================================================================
// Main Test Runner
//==============================================================================
int main() {
    std::cout << "Running Hazard Pointer Queue Test Suite...\n\n";
    
    try {
        testBasicFunctionality();
        testSingleThreadedPerformance();
        testMemoryManagement();
        testMultiProducerMultiConsumer();
        testABAAndUseAfterFree();
        testHighContention();
        testLiveLockDetection();
        testDestructorSafety();
        
        std::cout << "\n🎉 ALL HAZARD POINTER QUEUE TESTS PASSED! 🎉\n";
        return 0;
        
    } catch (const std::exception& ex) {
        std::cerr << "\n❌ TEST FAILURE: " << ex.what() << "\n";
        return 1;
    }
}