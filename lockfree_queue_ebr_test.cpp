/**********************************************************************
 *  lockfree_queue_test.cpp  —  Test suite for Michael-&-Scott MPMC queue + EBR GC
 *  ──────────────────────────────────────────────────────────────────
 *  This test suite verifies the lock-free queue implementation with
 *  epoch-based reclamation (EBR) garbage collection.
 *
 *  Tests include:
 *  - Basic functionality (enqueue/dequeue)
 *  - Single-threaded stress test
 *  - Multi-producer multi-consumer concurrent test
 *  - Memory safety under high contention
 *  - Empty queue handling
 *  - Performance benchmarks
 *
 *  Build & run:
 *      g++ -std=c++20 -O2 -pthread -fsanitize=address,undefined \
 *          lockfree_queue_test.cpp && ./a.out
 *********************************************************************/

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

// Include the lock-free queue header
// Assume the code from paste.txt is in this header
#include "lockfree_queue_ebr.cpp"

// For timing measurements
using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;
using TimePoint = Clock::time_point;

// Global parameters for tests
constexpr int NUM_THREADS = 8;
constexpr int ITEMS_PER_THREAD = 10000;
constexpr int TOTAL_ITEMS = NUM_THREADS * ITEMS_PER_THREAD;

// Utility for printing test results
class TestResults {
public:
    void printHeader(const std::string& testName) {
        std::cout << "====================================================\n";
        std::cout << "  " << testName << "\n";
        std::cout << "====================================================\n";
    }

    void printResult(const std::string& operation, double opPerSec, double timeInSec) {
        std::cout << std::left << std::setw(20) << operation 
                  << std::setw(15) << std::fixed << std::setprecision(2) << opPerSec << " ops/sec  "
                  << std::setw(10) << std::fixed << std::setprecision(6) << timeInSec << " sec\n";
    }

    void printSuccess(const std::string& message) {
        std::cout << "✓ " << message << "\n";
    }

    void printFailure(const std::string& message) {
        std::cerr << "✗ " << message << "\n";
    }
};

// Custom test value class to detect memory issues
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

// Helper struct for validating results in producer-consumer tests
struct TestStats {
    std::atomic<int> totalEnqueued{0};
    std::atomic<int> totalDequeued{0};
    std::atomic<bool> producersDone{false};
    std::atomic<bool> consumersDone{false};
    
    // For verification
    std::vector<std::atomic<int>> valueFrequency;
    
    TestStats(int maxValue) : valueFrequency(maxValue + 1) {
        for (auto& freq : valueFrequency) {
            freq.store(0, std::memory_order_relaxed);
        }
    }
    
    void reset() {
        totalEnqueued.store(0, std::memory_order_relaxed);
        totalDequeued.store(0, std::memory_order_relaxed);
        producersDone.store(false, std::memory_order_relaxed);
        consumersDone.store(false, std::memory_order_relaxed);
        
        for (auto& freq : valueFrequency) {
            freq.store(0, std::memory_order_relaxed);
        }
    }
};

// Test 1: Basic functionality test
void testBasicFunctionality() {
    TestResults results;
    results.printHeader("Basic Functionality Test");
    
    lfq::Queue<int> queue;
    
    // Test empty queue
    int value;
    bool success = queue.dequeue(value);
    assert(!success && "Dequeue from empty queue should fail");
    results.printSuccess("Empty queue check passed");
    
    // Test enqueue and dequeue
    queue.enqueue(42);
    success = queue.dequeue(value);
    assert(success && value == 42 && "Dequeue should return enqueued value");
    results.printSuccess("Single enqueue/dequeue passed");
    
    // Test multiple enqueue/dequeue
    for (int i = 0; i < 10; i++) {
        queue.enqueue(i);
    }
    
    for (int i = 0; i < 10; i++) {
        success = queue.dequeue(value);
        assert(success && value == i && "Values should be dequeued in FIFO order");
    }
    results.printSuccess("Multiple enqueue/dequeue passed");
    
    // Test empty after dequeues
    success = queue.dequeue(value);
    assert(!success && "Queue should be empty after all dequeues");
    results.printSuccess("Empty after dequeue check passed");
}

// Test 2: Single-threaded stress test
void testSingleThreadedStress() {
    TestResults results;
    results.printHeader("Single-Threaded Stress Test");
    
    lfq::Queue<int> queue;
    constexpr int COUNT = 100000;
    
    // Time enqueue operations
    auto startEnqueue = Clock::now();
    for (int i = 0; i < COUNT; i++) {
        queue.enqueue(i);
    }
    auto endEnqueue = Clock::now();
    Duration enqueueTime = endEnqueue - startEnqueue;
    double enqueueOpsPerSec = COUNT / enqueueTime.count();
    
    results.printResult("Enqueue", enqueueOpsPerSec, enqueueTime.count());
    
    // Time dequeue operations
    int value;
    bool success;
    int dequeueCount = 0;
    
    auto startDequeue = Clock::now();
    while (queue.dequeue(value)) {
        assert(value == dequeueCount && "Values should be dequeued in order");
        dequeueCount++;
    }
    auto endDequeue = Clock::now();
    Duration dequeueTime = endDequeue - startDequeue;
    double dequeueOpsPerSec = COUNT / dequeueTime.count();
    
    results.printResult("Dequeue", dequeueOpsPerSec, dequeueTime.count());
    
    assert(dequeueCount == COUNT && "Should dequeue exactly COUNT items");
    results.printSuccess("All values correctly enqueued and dequeued");
}

// Test 3: Memory leak detection with TestValue objects
void testMemoryManagement()
{
    TestResults r;
    r.printHeader("Memory-Leak Test (EBR aware)");

    TestValue::resetCounts();
    {
        lfq::Queue<TestValue> q;

        // 1) fill queue
        constexpr int N = 10'000;
        for (int i = 0; i < N; ++i) q.enqueue(TestValue(i));

        // 2) completely drain it (forces N dummy-node retires)
        TestValue tmp;
        while (q.dequeue(tmp)) { /* nothing */ }

        // 3) retire a few extra objects to trigger ≥2 epoch-flips
        //    so that every previously-retired node is reclaimed *now*.
        for (int i = 0; i < ebr::kBatchRetired * 3; ++i)
            ebr::retire(new int(i));      // cheap dummy allocations
    } // <- queue dtor

    // 4) give EBR a chance to run its deleters on our thread
    {
        ebr::Guard g1;   // epoch (cur)
        ebr::Guard g2;   // epoch (cur+1)
    }

    int c = TestValue::getConstructCount();
    int d = TestValue::getDestroyCount();
    std::cout << "constructed=" << c << "  destroyed=" << d << '\n';
    assert(c == d && "EBR must eventually destroy every TestValue");
    r.printSuccess("No leaks — counts match, EBR ran to completion");
}


// Test 4: Multi-producer multi-consumer test
void testMultiThreaded() {
    TestResults results;
    results.printHeader("Multi-Producer Multi-Consumer Test");
    
    lfq::Queue<int> queue;
    TestStats stats(TOTAL_ITEMS);
    
    auto producer = [&queue, &stats](int tid) {
        std::mt19937 rng(tid); // Use thread ID as seed
        
        for (int i = 0; i < ITEMS_PER_THREAD; i++) {
            int value = tid * ITEMS_PER_THREAD + i;
            queue.enqueue(value);
            stats.valueFrequency[value].fetch_add(1, std::memory_order_relaxed);
            stats.totalEnqueued.fetch_add(1, std::memory_order_relaxed);
            
            // Random delay to increase contention
            if (i % 100 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 10));
            }
        }
    };
    
    auto consumer = [&queue, &stats]() {
        int value;
        while (!stats.producersDone || stats.totalDequeued < stats.totalEnqueued) {
            if (queue.dequeue(value)) {
                stats.valueFrequency[value].fetch_sub(1, std::memory_order_relaxed);
                stats.totalDequeued.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Short delay when queue is empty but producers might still be running
                std::this_thread::yield();
            }
        }
    };
    
    // Start timing
    auto startTime = Clock::now();
    
    // Launch producer threads
    std::vector<std::thread> producerThreads;
    for (int i = 0; i < NUM_THREADS; i++) {
        producerThreads.emplace_back(producer, i);
    }
    
    // Launch consumer threads
    std::vector<std::thread> consumerThreads;
    for (int i = 0; i < NUM_THREADS; i++) {
        consumerThreads.emplace_back(consumer);
    }
    
    // Wait for producers to finish
    for (auto& t : producerThreads) {
        t.join();
    }
    stats.producersDone.store(true, std::memory_order_release);
    
    // Wait for consumers to finish
    for (auto& t : consumerThreads) {
        t.join();
    }
    stats.consumersDone.store(true, std::memory_order_release);
    
    // End timing
    auto endTime = Clock::now();
    Duration elapsed = endTime - startTime;
    
    // Verify results
    assert(stats.totalEnqueued == TOTAL_ITEMS && "All items should be enqueued");
    assert(stats.totalDequeued == TOTAL_ITEMS && "All items should be dequeued");
    
    bool frequencyCorrect = true;
    for (size_t i = 0; i < stats.valueFrequency.size(); i++) {
        if (stats.valueFrequency[i].load(std::memory_order_relaxed) != 0) {
            results.printFailure("Value frequency mismatch for " + std::to_string(i));
            frequencyCorrect = false;
        }
    }
    
    if (frequencyCorrect) {
        results.printSuccess("All values were correctly enqueued and dequeued exactly once");
    }
    
    // Print throughput
    double opsPerSec = TOTAL_ITEMS * 2.0 / elapsed.count(); // Both enqueue and dequeue
    results.printResult("Throughput", opsPerSec, elapsed.count());
}

// Test 5: High contention test
void testHighContention() {
    TestResults results;
    results.printHeader("High Contention Test");
    
    lfq::Queue<int> queue;
    std::atomic<int> dequeueSum{0};
    std::atomic<int> expectedSum{0};
    
    // Pre-fill queue with some items
    constexpr int PREFILL = 1000;
    for (int i = 0; i < PREFILL; i++) {
        queue.enqueue(i);
        expectedSum.fetch_add(i, std::memory_order_relaxed);
    }
    
    // All threads will both enqueue and dequeue simultaneously
    auto worker = [&queue, &dequeueSum, &expectedSum](int tid) {
        std::mt19937 rng(tid);
        
        for (int i = 0; i < ITEMS_PER_THREAD; i++) {
            int value = PREFILL + tid * ITEMS_PER_THREAD + i;
            
            // Randomly decide between enqueue and dequeue with 50% probability
            if (rng() % 2 == 0) {
                queue.enqueue(value);
                expectedSum.fetch_add(value, std::memory_order_relaxed);
            } else {
                int dequeuedValue;
                if (queue.dequeue(dequeuedValue)) {
                    dequeueSum.fetch_add(dequeuedValue, std::memory_order_relaxed);
                }
            }
            
            // Random slight delay
            if (i % 50 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(rng() % 5));
            }
        }
    };
    
    // Start timing
    auto startTime = Clock::now();
    
    // Launch worker threads
    std::vector<std::thread> workerThreads;
    for (int i = 0; i < NUM_THREADS; i++) {
        workerThreads.emplace_back(worker, i);
    }
    
    // Wait for all threads to finish
    for (auto& t : workerThreads) {
        t.join();
    }
    
    // End timing
    auto endTime = Clock::now();
    Duration elapsed = endTime - startTime;
    
    // Drain the queue to get final sum
    int dequeuedValue;
    while (queue.dequeue(dequeuedValue)) {
        dequeueSum.fetch_add(dequeuedValue, std::memory_order_relaxed);
    }
    
    // Verify results
    if (dequeueSum.load() == expectedSum.load()) {
        results.printSuccess("Sum of dequeued values matches expected sum");
    } else {
        results.printFailure("Sum mismatch: expected " + 
                           std::to_string(expectedSum.load()) + 
                           " but got " + 
                           std::to_string(dequeueSum.load()));
    }
    
    // Print throughput
    double opsPerSec = NUM_THREADS * ITEMS_PER_THREAD / elapsed.count();
    results.printResult("Mixed Operations", opsPerSec, elapsed.count());
}

// Test 6: Empty check race condition test
void testEmptyCheck() {
    TestResults results;
    results.printHeader("Empty Check Race Condition Test");
    
    lfq::Queue<int> queue;
    std::atomic<bool> stop{false};
    std::atomic<int> enqueueCount{0};
    std::atomic<int> dequeueCount{0};
    std::atomic<int> emptyCheckCount{0};
    
    // Thread constantly checking empty() status
    auto emptyChecker = [&queue, &stop, &emptyCheckCount]() {
        while (!stop.load(std::memory_order_relaxed)) {
            bool isEmpty = queue.empty();
            emptyCheckCount.fetch_add(1, std::memory_order_relaxed);
            
            // Very short yield to give other threads a chance
            std::this_thread::yield();
        }
    };
    
    // Thread constantly enqueuing
    auto enqueuer = [&queue, &stop, &enqueueCount]() {
        int value = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            queue.enqueue(value++);
            enqueueCount.fetch_add(1, std::memory_order_relaxed);
            
            // Short delay to regulate speed
            if (value % 100 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        }
    };
    
    // Thread constantly dequeuing
    auto dequeuer = [&queue, &stop, &dequeueCount]() {
        int value;
        while (!stop.load(std::memory_order_relaxed)) {
            if (queue.dequeue(value)) {
                dequeueCount.fetch_add(1, std::memory_order_relaxed);
            }
            
            // Very short yield to give other threads a chance
            std::this_thread::yield();
        }
    };
    
    // Start threads
    std::thread checkerThread(emptyChecker);
    std::thread enqueuerThread(enqueuer);
    std::thread dequeuerThread(dequeuer);
    
    // Run for a short time
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Stop threads
    stop.store(true, std::memory_order_relaxed);
    checkerThread.join();
    enqueuerThread.join();
    dequeuerThread.join();
    
    // Print statistics
    std::cout << "Empty checks performed: " << emptyCheckCount.load() << "\n";
    std::cout << "Items enqueued: " << enqueueCount.load() << "\n";
    std::cout << "Items dequeued: " << dequeueCount.load() << "\n";
    
    // Drain remaining items
    int remainingItems = 0;
    int value;
    while (queue.dequeue(value)) {
        remainingItems++;
    }
    
    std::cout << "Remaining items in queue: " << remainingItems << "\n";
    std::cout << "Total accounted for: " << (dequeueCount.load() + remainingItems) << "\n";
    
    // Verify that all enqueued items are accounted for
    assert(enqueueCount.load() == dequeueCount.load() + remainingItems);
    results.printSuccess("All operations completed without crashes or hangs");
}

// Test 7: EBR Epoch Advancement Test
void testEbrEpochAdvancement() {
    TestResults results;
    results.printHeader("EBR Epoch Advancement Test");
    
    lfq::Queue<int> queue;
    std::atomic<bool> stop{false};
    std::atomic<int> itemsProcessed{0};
    
    // This test stresses the EBR system by rapidly creating and retiring nodes
    auto worker = [&queue, &stop, &itemsProcessed](int id) {
        // Each thread performs rapid enqueue/dequeue cycles
        for (int i = 0; i < 50000 && !stop.load(std::memory_order_relaxed); i++) {
            // Enqueue batch
            for (int j = 0; j < 10; j++) {
                queue.enqueue(id * 1000000 + i * 1000 + j);
            }
            
            // Dequeue batch
            int value;
            for (int j = 0; j < 10; j++) {
                if (queue.dequeue(value)) {
                    itemsProcessed.fetch_add(1, std::memory_order_relaxed);
                }
            }
            
            // Deliberately create pressure on the EBR system by
            // having threads frequently enter/exit critical sections
            {
                ebr::Guard g1;
                // Force compiler not to optimize away the guard
                volatile int dummy = 0;
                dummy++;
            }
            
            {
                ebr::Guard g2;
                // Another empty critical section
                volatile int dummy = 0;
                dummy++;
            }
            
            // Check if we should stop early (for ASAN runs)
            if (i % 1000 == 0 && itemsProcessed.load(std::memory_order_relaxed) > 100000) {
                break;
            }
        }
    };
    
    // Start timing
    auto startTime = Clock::now();
    
    // Launch worker threads
    std::vector<std::thread> workerThreads;
    for (int i = 0; i < NUM_THREADS; i++) {
        workerThreads.emplace_back(worker, i);
    }
    
    // Wait for threads to finish
    for (auto& t : workerThreads) {
        t.join();
    }
    
    // End timing
    auto endTime = Clock::now();
    Duration elapsed = endTime - startTime;
    
    // Print statistics
    int processed = itemsProcessed.load();
    std::cout << "Total items processed: " << processed << "\n";
    double opsPerSec = processed / elapsed.count();
    results.printResult("Operations", opsPerSec, elapsed.count());
    
    // Drain remaining items
    int remainingItems = 0;
    int value;
    while (queue.dequeue(value)) {
        remainingItems++;
    }
    
    std::cout << "Remaining items in queue: " << remainingItems << "\n";
    
    results.printSuccess("EBR system handled high node turnover without issues");
}

// Test 8: Custom deleter test
void testCustomDeleter() {
    TestResults results;
    results.printHeader("Custom Deleter Test");
    
    // Allocate an object on the heap
    int* heapInt = new int(42);
    
    // Track if the deleter was called
    std::atomic<bool> deleterCalled{false};
    
    // Create a custom deleter
    auto customDeleter = [&deleterCalled](void* ptr) {
        delete static_cast<int*>(ptr);
        deleterCalled.store(true, std::memory_order_release);
    };
    
    // Use the retire function directly with custom deleter
    {
        ebr::Guard g; // Enter a critical section
        ebr::retire(heapInt, customDeleter);
    }
    
    // Force epoch advancement
    for (int i = 0; i < 3; i++) {
        {
            ebr::Guard g1;
            // Just enter and exit critical section
        }
        
        // Create and retire something to force epoch checks
        for (int j = 0; j < ebr::kBatchRetired + 1; j++) {
            int* dummy = new int(j);
            ebr::retire(dummy);
        }
        
        {
            ebr::Guard g2;
            // Another critical section
        }
    }
    
    // Check if our deleter was called
    if (deleterCalled.load()) {
        results.printSuccess("Custom deleter was called");
    } else {
        results.printFailure("Custom deleter was not called");
    }
}

// Main function: run all tests
int main() {
    std::cout << "Running comprehensive test suite for lock-free queue with EBR...\n\n";
    
    testBasicFunctionality();
    testSingleThreadedStress();
    testMemoryManagement();
    testMultiThreaded();
    testHighContention();
    testEmptyCheck();
    testEbrEpochAdvancement();
    testCustomDeleter();
    
    std::cout << "\nAll tests completed successfully!\n";
    return 0;
}
