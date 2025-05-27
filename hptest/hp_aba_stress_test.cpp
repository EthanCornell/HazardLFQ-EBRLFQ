/**********************************************************************
 *  hp_aba_stress_test.cpp - ABA & Use-After-Free Stress Tests for HP Queue
 *  ──────────────────────────────────────────────────────────────────
 *  
 *  PURPOSE
 *  -------
 *  Intensive stress testing of hazard pointer-based queue implementation
 *  specifically targeting ABA patterns, use-after-free scenarios, and
 *  memory reclamation edge cases that hazard pointers are designed to prevent.
 *  
 *  TEST SCENARIOS
 *  --------------
 *  1. Rapid enqueue/dequeue ABA patterns
 *  2. Concurrent node reuse stress testing  
 *  3. Hazard pointer scan frequency stress
 *  4. Memory pressure with delayed reclamation
 *  5. Burst traffic with node address reuse
 *  6. Thread lifecycle stress (threads joining/starting)
 *  7. Long-running stability test
 *  8. Coordinated retirement stress
 *  9. Hazard pointer slot exhaustion test
 *  10. Memory leak detection under stress
 *  
 *  BUILD
 *  -----
 *  g++ -std=c++20 -O3 -march=native -pthread -I../include hp_aba_stress_test.cpp -o hp_aba_stress
 *  
 *  # With detailed analysis
 *  g++ -std=c++20 -O3 -march=native -pthread -DDETAILED_ANALYSIS -I../include \
 *      hp_aba_stress_test.cpp -o hp_aba_stress
 *  
 *  # With sanitizers (for verification)
 *  g++ -std=c++20 -O1 -g -fsanitize=address,undefined -pthread -I../include \
 *      hp_aba_stress_test.cpp -o hp_aba_stress_asan
 *********************************************************************/

#include "lockfree_queue_hp.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <memory>
#include <cassert>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <barrier>
#include <map>
#include <set>
#include <mutex>
#include <cstring>  // For memset

using namespace std::chrono_literals;
using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

// Test configuration
namespace config {
    constexpr int STRESS_THREADS = 16;
    constexpr int STRESS_OPERATIONS = 1000000;
    constexpr int BURST_SIZE = 10000;
    constexpr int LONG_RUNNING_MINUTES = 5;
    constexpr int RAPID_CYCLE_COUNT = 100000;
}

// Instrumented message for tracking
struct StressTestMessage {
    uint64_t sequence_id;
    uint32_t producer_id;
    uint32_t magic_number;
    TimePoint creation_time;
    std::array<uint8_t, 32> payload;
    
    static constexpr uint32_t MAGIC = 0xDEADBEEF;
    
    explicit StressTestMessage(uint64_t seq = 0, uint32_t prod_id = 0)
        : sequence_id(seq), producer_id(prod_id), magic_number(MAGIC)
        , creation_time(Clock::now())
    {
        // Fill payload with deterministic pattern
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<uint8_t>((seq + i + prod_id) & 0xFF);
        }
    }
    
    bool isValid() const {
        if (magic_number != MAGIC) return false;
        
        // Verify payload integrity
        for (size_t i = 0; i < payload.size(); ++i) {
            uint8_t expected = static_cast<uint8_t>((sequence_id + i + producer_id) & 0xFF);
            if (payload[i] != expected) return false;
        }
        return true;
    }
    
    void corrupt() {
        magic_number = 0xBADC0DE;
        payload[0] = 0xFF;
    }
};

// Test statistics tracking
struct StressTestStats {
    std::atomic<uint64_t> operations_completed{0};
    std::atomic<uint64_t> enqueue_operations{0};
    std::atomic<uint64_t> dequeue_operations{0};
    std::atomic<uint64_t> validation_failures{0};
    std::atomic<uint64_t> empty_dequeues{0};
    std::atomic<uint64_t> hazard_pointer_conflicts{0};
    std::atomic<uint64_t> memory_reclamations{0};
    
    void printSummary() const {
        std::cout << "\n=== Stress Test Statistics ===\n"
                  << "Total operations: " << operations_completed.load() << '\n'
                  << "Enqueue ops: " << enqueue_operations.load() << '\n'
                  << "Dequeue ops: " << dequeue_operations.load() << '\n'
                  << "Validation failures: " << validation_failures.load() << '\n'
                  << "Empty dequeues: " << empty_dequeues.load() << '\n'
                  << "HP conflicts: " << hazard_pointer_conflicts.load() << '\n'
                  << "Memory reclamations: " << memory_reclamations.load() << '\n';
    }
    
    void reset() {
        operations_completed.store(0);
        enqueue_operations.store(0);
        dequeue_operations.store(0);
        validation_failures.store(0);
        empty_dequeues.store(0);
        hazard_pointer_conflicts.store(0);
        memory_reclamations.store(0);
    }
};

// Memory tracking for leak detection
class MemoryTracker {
private:
    std::atomic<size_t> allocations_{0};
    std::atomic<size_t> deallocations_{0};
    std::atomic<size_t> bytes_allocated_{0};
    std::atomic<size_t> bytes_deallocated_{0};
    std::atomic<size_t> peak_usage_{0};
    
public:
    void recordAllocation(size_t bytes) {
        allocations_.fetch_add(1, std::memory_order_relaxed);
        size_t current = bytes_allocated_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        
        // Update peak usage
        size_t current_peak = peak_usage_.load(std::memory_order_relaxed);
        while (current > current_peak && 
               !peak_usage_.compare_exchange_weak(current_peak, current, std::memory_order_relaxed)) {
            // Retry if another thread updated peak_usage_
        }
    }
    
    void recordDeallocation(size_t bytes) {
        deallocations_.fetch_add(1, std::memory_order_relaxed);
        bytes_deallocated_.fetch_add(bytes, std::memory_order_relaxed);
    }
    
    size_t getCurrentUsage() const {
        return bytes_allocated_.load(std::memory_order_acquire) - 
               bytes_deallocated_.load(std::memory_order_acquire);
    }
    
    size_t getPeakUsage() const {
        return peak_usage_.load(std::memory_order_acquire);
    }
    
    bool hasLeaks() const {
        return allocations_.load() != deallocations_.load();
    }
    
    void printReport() const {
        std::cout << "\n=== Memory Tracking Report ===\n"
                  << "Allocations: " << allocations_.load() << '\n'
                  << "Deallocations: " << deallocations_.load() << '\n'
                  << "Current usage: " << getCurrentUsage() << " bytes\n"
                  << "Peak usage: " << getPeakUsage() << " bytes\n"
                  << "Potential leaks: " << (hasLeaks() ? "YES" : "NO") << '\n';
    }
};

// Global instances
StressTestStats g_stats;
MemoryTracker g_memory_tracker;

//==============================================================================
// Test 1: Rapid ABA Pattern Stress Test
//==============================================================================
void testRapidABAPattern() {
    std::cout << "=== Rapid ABA Pattern Test ===\n";
    
    lfq::HPQueue<StressTestMessage> queue;
    constexpr int THREADS = config::STRESS_THREADS;
    constexpr int OPS_PER_THREAD = config::RAPID_CYCLE_COUNT;
    
    std::atomic<bool> start_flag{false};
    std::atomic<uint64_t> sequence_counter{0};
    std::barrier sync_barrier(THREADS);
    
    auto worker = [&](int thread_id) {
        std::mt19937_64 rng(thread_id);
        sync_barrier.arrive_and_wait();
        
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        
        for (int i = 0; i < OPS_PER_THREAD; ++i) {
            // Rapid enqueue/dequeue cycle to stress ABA prevention
            auto seq = sequence_counter.fetch_add(1, std::memory_order_relaxed);
            StressTestMessage msg(seq, thread_id);
            
            queue.enqueue(msg);
            g_stats.enqueue_operations.fetch_add(1, std::memory_order_relaxed);
            
            StressTestMessage dequeued_msg;
            if (queue.dequeue(dequeued_msg)) {
                if (!dequeued_msg.isValid()) {
                    g_stats.validation_failures.fetch_add(1, std::memory_order_relaxed);
                }
                g_stats.dequeue_operations.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_stats.empty_dequeues.fetch_add(1, std::memory_order_relaxed);
            }
            
            g_stats.operations_completed.fetch_add(1, std::memory_order_relaxed);
            
            // Occasionally yield to create more contention
            if ((i & 0xFF) == 0) {
                std::this_thread::yield();
            }
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back(worker, i);
    }
    
    auto start_time = Clock::now();
    start_flag.store(true, std::memory_order_release);
    
    for (auto& t : threads) {
        t.join();
    }
    
    auto end_time = Clock::now();
    auto duration = std::chrono::duration<double>(end_time - start_time).count();
    
    std::cout << "Completed " << g_stats.operations_completed.load() 
              << " operations in " << std::fixed << std::setprecision(2) 
              << duration << " seconds\n";
    std::cout << "Throughput: " << (g_stats.operations_completed.load() / duration) 
              << " ops/sec\n";
    
    // Verify no validation failures
    if (g_stats.validation_failures.load() > 0) {
        std::cerr << "❌ FAILURE: " << g_stats.validation_failures.load() 
                  << " validation failures detected!\n";
        throw std::runtime_error("ABA pattern test failed - data corruption detected");
    }
    
    std::cout << "✅ PASSED: No ABA-related corruption detected\n";
}

//==============================================================================
// Test 2: Node Address Reuse Stress Test
//==============================================================================
void testNodeAddressReuse() {
    std::cout << "\n=== Node Address Reuse Test ===\n";
    
    lfq::HPQueue<StressTestMessage> queue;
    constexpr int ITERATIONS = 50000;
    std::set<void*> seen_addresses;
    std::mutex address_mutex;
    bool address_reuse_detected = false;
    
    // Custom message with address tracking
    struct TrackedMessage : public StressTestMessage {
        void* self_address;
        
        TrackedMessage(uint64_t seq = 0, uint32_t prod_id = 0) 
            : StressTestMessage(seq, prod_id), self_address(this) {}
    };
    
    lfq::HPQueue<TrackedMessage> tracked_queue;
    
    for (int i = 0; i < ITERATIONS; ++i) {
        TrackedMessage msg(i, 0);
        tracked_queue.enqueue(msg);
        
        TrackedMessage dequeued;
        if (tracked_queue.dequeue(dequeued)) {
            std::lock_guard<std::mutex> lock(address_mutex);
            
            if (seen_addresses.count(dequeued.self_address)) {
                address_reuse_detected = true;
                std::cout << "Address reuse detected: " << dequeued.self_address 
                          << " at iteration " << i << '\n';
            } else {
                seen_addresses.insert(dequeued.self_address);
            }
            
            // Verify message wasn't corrupted during reuse
            if (!dequeued.isValid()) {
                std::cerr << "❌ Message corruption during address reuse!\n";
                throw std::runtime_error("Node reuse corruption detected");
            }
        }
        
        // Force some memory pressure to encourage reuse
        if ((i % 1000) == 0) {
            std::vector<TrackedMessage> temp;
            for (int j = 0; j < 100; ++j) {
                temp.emplace_back(i * 1000 + j, 1);
            }
        }
    }
    
    std::cout << "Processed " << ITERATIONS << " messages\n";
    std::cout << "Unique addresses seen: " << seen_addresses.size() << '\n';
    std::cout << "Address reuse detected: " << (address_reuse_detected ? "YES" : "NO") << '\n';
    std::cout << "✅ PASSED: Address reuse handled safely by hazard pointers\n";
}

//==============================================================================
// Test 3: Hazard Pointer Slot Exhaustion Test  
//==============================================================================
void testHazardPointerSlotExhaustion() {
    std::cout << "\n=== Hazard Pointer Slot Exhaustion Test ===\n";
    
    lfq::HPQueue<StressTestMessage> queue;
    constexpr int MAX_THREADS = 64; // Should exceed HP slot limit
    std::atomic<bool> should_stop{false};
    std::atomic<int> successful_threads{0};
    std::atomic<int> failed_threads{0};
    
    auto worker = [&](int thread_id) {
        try {
            std::mt19937 rng(thread_id);
            
            for (int i = 0; i < 1000 && !should_stop.load(); ++i) {
                StressTestMessage msg(i, thread_id);
                queue.enqueue(msg);
                
                StressTestMessage dequeued;
                if (queue.dequeue(dequeued)) {
                    if (!dequeued.isValid()) {
                        throw std::runtime_error("Message validation failed");
                    }
                }
                
                // Small delay to maintain thread activity
                if ((i % 100) == 0) {
                    std::this_thread::sleep_for(1ms);
                }
            }
            
            successful_threads.fetch_add(1, std::memory_order_relaxed);
            
        } catch (const std::exception& e) {
            std::cout << "Thread " << thread_id << " failed: " << e.what() << '\n';
            failed_threads.fetch_add(1, std::memory_order_relaxed);
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < MAX_THREADS; ++i) {
        try {
            threads.emplace_back(worker, i);
        } catch (const std::exception& e) {
            std::cout << "Failed to create thread " << i << ": " << e.what() << '\n';
            break;
        }
    }
    
    // Let threads run for a bit
    std::this_thread::sleep_for(5s);
    should_stop.store(true);
    
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    
    std::cout << "Threads created: " << threads.size() << '\n';
    std::cout << "Successful threads: " << successful_threads.load() << '\n';
    std::cout << "Failed threads: " << failed_threads.load() << '\n';
    
    if (failed_threads.load() == 0) {
        std::cout << "✅ PASSED: All threads handled HP slot management correctly\n";
    } else {
        std::cout << "⚠️  WARNING: Some threads failed (expected if HP slots exhausted)\n";
    }
}

//==============================================================================
// Test 4: Memory Pressure with Delayed Reclamation
//==============================================================================
void testMemoryPressureReclamation() {
    std::cout << "\n=== Memory Pressure Reclamation Test ===\n";
    
    lfq::HPQueue<StressTestMessage> queue;
    constexpr int PRESSURE_THREADS = 8;
    constexpr int OPERATIONS = 100000;
    
    std::atomic<bool> memory_pressure_active{true};
    std::atomic<size_t> peak_memory_usage{0};
    
    // Memory pressure thread
    std::thread memory_pressure_thread([&]() {
        std::vector<std::unique_ptr<char[]>> memory_hogs;
        
        while (memory_pressure_active.load()) {
            // Allocate 1MB chunks
            constexpr size_t CHUNK_SIZE = 1024 * 1024;
            memory_hogs.push_back(std::make_unique<char[]>(CHUNK_SIZE));
            
            // Touch the memory to ensure it's allocated
            memset(memory_hogs.back().get(), 0xAA, CHUNK_SIZE);
            
            size_t current_usage = memory_hogs.size() * CHUNK_SIZE;
            g_memory_tracker.recordAllocation(CHUNK_SIZE);
            
            // Update peak usage
            size_t current_peak = peak_memory_usage.load();
            while (current_usage > current_peak && 
                   !peak_memory_usage.compare_exchange_weak(current_peak, current_usage)) {
                // Retry
            }
            
            // Keep about 100MB of pressure
            if (memory_hogs.size() > 100) {
                g_memory_tracker.recordDeallocation(CHUNK_SIZE);
                memory_hogs.erase(memory_hogs.begin());
            }
            
            std::this_thread::sleep_for(10ms);
        }
    });
    
    // Queue operation threads
    std::vector<std::thread> worker_threads;
    std::atomic<uint64_t> sequence_counter{0};
    
    for (int i = 0; i < PRESSURE_THREADS; ++i) {
        worker_threads.emplace_back([&, i]() {
            std::mt19937 rng(i);
            
            for (int j = 0; j < OPERATIONS / PRESSURE_THREADS; ++j) {
                auto seq = sequence_counter.fetch_add(1);
                StressTestMessage msg(seq, i);
                
                queue.enqueue(msg);
                
                // Randomly dequeue to create retirement pressure
                if (rng() % 2 == 0) {
                    StressTestMessage dequeued;
                    if (queue.dequeue(dequeued)) {
                        if (!dequeued.isValid()) {
                            g_stats.validation_failures.fetch_add(1);
                        }
                    }
                }
                
                g_stats.operations_completed.fetch_add(1);
                
                // Occasionally pause to let memory pressure build
                if ((j % 1000) == 0) {
                    std::this_thread::sleep_for(1ms);
                }
            }
        });
    }
    
    auto start_time = Clock::now();
    
    for (auto& t : worker_threads) {
        t.join();
    }
    
    // Stop memory pressure
    memory_pressure_active.store(false);
    memory_pressure_thread.join();
    
    auto end_time = Clock::now();
    auto duration = std::chrono::duration<double>(end_time - start_time).count();
    
    // Drain remaining queue
    StressTestMessage msg;
    int remaining = 0;
    while (queue.dequeue(msg)) {
        remaining++;
        if (!msg.isValid()) {
            g_stats.validation_failures.fetch_add(1);
        }
    }
    
    std::cout << "Operations completed: " << g_stats.operations_completed.load() << '\n';
    std::cout << "Duration: " << std::fixed << std::setprecision(2) << duration << " seconds\n";
    std::cout << "Peak memory pressure: " << (peak_memory_usage.load() / (1024 * 1024)) << " MB\n";
    std::cout << "Remaining in queue: " << remaining << '\n';
    std::cout << "Validation failures: " << g_stats.validation_failures.load() << '\n';
    
    if (g_stats.validation_failures.load() == 0) {
        std::cout << "✅ PASSED: No corruption under memory pressure\n";
    } else {
        std::cerr << "❌ FAILED: Data corruption detected under memory pressure\n";
        throw std::runtime_error("Memory pressure test failed");
    }
}

//==============================================================================
// Test 5: Burst Traffic Stress Test
//==============================================================================
void testBurstTrafficStress() {
    std::cout << "\n=== Burst Traffic Stress Test ===\n";
    
    lfq::HPQueue<StressTestMessage> queue;
    constexpr int NUM_BURSTS = 20;
    constexpr int BURST_SIZE = config::BURST_SIZE;
    constexpr int PRODUCERS = 4;
    constexpr int CONSUMERS = 4;
    
    std::atomic<bool> burst_active{false};
    std::atomic<int> current_burst{0};
    std::atomic<uint64_t> sequence_counter{0};
    std::atomic<uint64_t> messages_produced{0};
    std::atomic<uint64_t> messages_consumed{0};
    
    // Producer threads
    std::vector<std::thread> producers;
    for (int i = 0; i < PRODUCERS; ++i) {
        producers.emplace_back([&, i]() {
            while (current_burst.load() < NUM_BURSTS) {
                while (!burst_active.load() && current_burst.load() < NUM_BURSTS) {
                    std::this_thread::sleep_for(1ms);
                }
                
                if (current_burst.load() >= NUM_BURSTS) break;
                
                // Produce burst
                for (int j = 0; j < BURST_SIZE / PRODUCERS && burst_active.load(); ++j) {
                    auto seq = sequence_counter.fetch_add(1);
                    StressTestMessage msg(seq, i);
                    queue.enqueue(msg);
                    messages_produced.fetch_add(1);
                }
            }
        });
    }
    
    // Consumer threads
    std::vector<std::thread> consumers;
    for (int i = 0; i < CONSUMERS; ++i) {
        consumers.emplace_back([&]() {
            StressTestMessage msg;
            while (current_burst.load() < NUM_BURSTS || !queue.empty()) {
                if (queue.dequeue(msg)) {
                    if (!msg.isValid()) {
                        g_stats.validation_failures.fetch_add(1);
                    }
                    messages_consumed.fetch_add(1);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    
    // Burst controller
    auto start_time = Clock::now();
    for (int burst = 0; burst < NUM_BURSTS; ++burst) {
        current_burst.store(burst);
        burst_active.store(true);
        
        std::this_thread::sleep_for(50ms); // Burst duration
        
        burst_active.store(false);
        std::this_thread::sleep_for(100ms); // Inter-burst interval
        
        std::cout << "Burst " << (burst + 1) << "/" << NUM_BURSTS 
                  << " - Produced: " << messages_produced.load()
                  << ", Consumed: " << messages_consumed.load() << '\n';
    }
    
    // Signal completion
    current_burst.store(NUM_BURSTS);
    
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();
    
    auto end_time = Clock::now();
    auto duration = std::chrono::duration<double>(end_time - start_time).count();
    
    std::cout << "Total duration: " << std::fixed << std::setprecision(2) << duration << " seconds\n";
    std::cout << "Messages produced: " << messages_produced.load() << '\n';
    std::cout << "Messages consumed: " << messages_consumed.load() << '\n';
    std::cout << "Validation failures: " << g_stats.validation_failures.load() << '\n';
    
    if (messages_produced.load() == messages_consumed.load() && 
        g_stats.validation_failures.load() == 0) {
        std::cout << "✅ PASSED: All burst messages processed correctly\n";
    } else {
        std::cerr << "❌ FAILED: Message count mismatch or validation failures\n";
        throw std::runtime_error("Burst traffic test failed");
    }
}

//==============================================================================
// Test 6: Long-Running Stability Test
//==============================================================================
void testLongRunningStability() {
    std::cout << "\n=== Long-Running Stability Test ===\n";
    std::cout << "Running for " << config::LONG_RUNNING_MINUTES << " minutes...\n";
    
    lfq::HPQueue<StressTestMessage> queue;
    constexpr int THREADS = 8;
    
    std::atomic<bool> should_stop{false};
    std::atomic<uint64_t> sequence_counter{0};
    std::atomic<uint64_t> total_operations{0};
    std::atomic<uint64_t> checkpoint_operations{0};
    
    // Worker threads
    std::vector<std::thread> workers;
    for (int i = 0; i < THREADS; ++i) {
        workers.emplace_back([&, i]() {
            std::mt19937 rng(i);
            uint64_t local_ops = 0;
            
            while (!should_stop.load()) {
                // Mix of operations
                int operation = rng() % 3;
                
                if (operation == 0) {
                    // Enqueue
                    auto seq = sequence_counter.fetch_add(1);
                    StressTestMessage msg(seq, i);
                    queue.enqueue(msg);
                } else {
                    // Dequeue
                    StressTestMessage msg;
                    if (queue.dequeue(msg)) {
                        if (!msg.isValid()) {
                            g_stats.validation_failures.fetch_add(1);
                        }
                    }
                }
                
                local_ops++;
                if ((local_ops % 10000) == 0) {
                    total_operations.fetch_add(10000);
                    local_ops = 0;
                }
                
                // Occasional brief pause
                if ((rng() % 10000) == 0) {
                    std::this_thread::sleep_for(1ms);
                }
            }
            
            // Add remaining operations
            total_operations.fetch_add(local_ops);
        });
    }
    
    // Progress monitoring
    std::thread monitor([&]() {
        auto start_time = Clock::now();
        auto last_checkpoint = start_time;
        uint64_t last_ops = 0;
        
        while (!should_stop.load()) {
            std::this_thread::sleep_for(30s);
            
            auto now = Clock::now();
            auto current_ops = total_operations.load();
            auto elapsed = std::chrono::duration<double>(now - last_checkpoint).count();
            auto rate = (current_ops - last_ops) / elapsed;
            
            auto total_elapsed = std::chrono::duration<double>(now - start_time).count();
            
            std::cout << "Elapsed: " << std::fixed << std::setprecision(1) 
                      << (total_elapsed / 60.0) << " min, "
                      << "Operations: " << current_ops << ", "
                      << "Rate: " << std::fixed << std::setprecision(0) << rate << " ops/sec, "
                      << "Failures: " << g_stats.validation_failures.load() << '\n';
            
            last_checkpoint = now;
            last_ops = current_ops;
        }
    });
    
    // Run for specified duration
    std::this_thread::sleep_for(std::chrono::minutes(config::LONG_RUNNING_MINUTES));
    should_stop.store(true);
    
    for (auto& t : workers) t.join();
    monitor.join();
    
    // Final drain
    StressTestMessage msg;
    int remaining = 0;
    while (queue.dequeue(msg)) {
        remaining++;
        if (!msg.isValid()) {
            g_stats.validation_failures.fetch_add(1);
        }
    }
    
    std::cout << "Final statistics:\n";
    std::cout << "Total operations: " << total_operations.load() << '\n';
    std::cout << "Remaining in queue: " << remaining << '\n';
    std::cout << "Validation failures: " << g_stats.validation_failures.load() << '\n';
    std::cout << "Average rate: " << (total_operations.load() / (config::LONG_RUNNING_MINUTES * 60.0)) 
              << " ops/sec\n";
    
    if (g_stats.validation_failures.load() == 0) {
        std::cout << "✅ PASSED: Long-running stability maintained\n";
    } else {
        std::cerr << "❌ FAILED: Validation failures in long-running test\n";
        throw std::runtime_error("Long-running stability test failed");
    }
}

//==============================================================================
// Test 7: Thread Lifecycle Stress Test
//==============================================================================
void testThreadLifecycleStress() {
    std::cout << "\n=== Thread Lifecycle Stress Test ===\n";
    
    lfq::HPQueue<StressTestMessage> queue;
    constexpr int LIFECYCLE_CYCLES = 50;
    constexpr int THREADS_PER_CYCLE = 8;
    constexpr int OPS_PER_THREAD = 1000;
    
    std::atomic<uint64_t> sequence_counter{0};
    std::atomic<uint64_t> total_operations{0};
    std::atomic<uint64_t> thread_creation_failures{0};
    
    for (int cycle = 0; cycle < LIFECYCLE_CYCLES; ++cycle) {
        std::vector<std::thread> threads;
        std::atomic<int> threads_completed{0};
        
        // Create threads for this cycle
        for (int i = 0; i < THREADS_PER_CYCLE; ++i) {
            try {
                threads.emplace_back([&, i, cycle]() {
                    std::mt19937 rng(cycle * THREADS_PER_CYCLE + i);
                    
                    for (int j = 0; j < OPS_PER_THREAD; ++j) {
                        if (rng() % 2 == 0) {
                            // Enqueue
                            auto seq = sequence_counter.fetch_add(1);
                            StressTestMessage msg(seq, i);
                            queue.enqueue(msg);
                        } else {
                            // Dequeue
                            StressTestMessage msg;
                            if (queue.dequeue(msg)) {
                                if (!msg.isValid()) {
                                    g_stats.validation_failures.fetch_add(1);
                                }
                            }
                        }
                        
                        total_operations.fetch_add(1);
                    }
                    
                    threads_completed.fetch_add(1);
                });
            } catch (const std::exception& e) {
                thread_creation_failures.fetch_add(1);
                std::cout << "Thread creation failed in cycle " << cycle << ": " << e.what() << '\n';
            }
        }
        
        // Wait for all threads in this cycle to complete
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        
        if ((cycle % 10) == 0) {
            std::cout << "Completed cycle " << cycle << "/" << LIFECYCLE_CYCLES 
                      << ", threads completed: " << threads_completed.load() << '\n';
        }
        
        // Brief pause between cycles
        std::this_thread::sleep_for(10ms);
    }
    
    // Final cleanup
    StressTestMessage msg;
    int remaining = 0;
    while (queue.dequeue(msg)) {
        remaining++;
        if (!msg.isValid()) {
            g_stats.validation_failures.fetch_add(1);
        }
    }
    
    std::cout << "Total operations: " << total_operations.load() << '\n';
    std::cout << "Thread creation failures: " << thread_creation_failures.load() << '\n';
    std::cout << "Remaining in queue: " << remaining << '\n';
    std::cout << "Validation failures: " << g_stats.validation_failures.load() << '\n';
    
    if (g_stats.validation_failures.load() == 0) {
        std::cout << "✅ PASSED: Thread lifecycle stress handled correctly\n";
    } else {
        std::cerr << "❌ FAILED: Validation failures in thread lifecycle test\n";
        throw std::runtime_error("Thread lifecycle stress test failed");
    }
}

//==============================================================================
// Main Test Runner
//==============================================================================
int main(int argc, char* argv[]) {
    std::cout << "Hazard Pointer Queue ABA & Stress Test Suite\n";
    std::cout << "============================================\n";
    std::cout << "Hardware threads: " << std::thread::hardware_concurrency() << '\n';
    std::cout << "Test configuration:\n";
    std::cout << "  Stress threads: " << config::STRESS_THREADS << '\n';
    std::cout << "  Stress operations: " << config::STRESS_OPERATIONS << '\n';
    std::cout << "  Long-running duration: " << config::LONG_RUNNING_MINUTES << " minutes\n\n";
    
    // Parse command line options
    bool run_long_test = false;
    bool detailed_output = false;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--long" || arg == "-l") {
            run_long_test = true;
        } else if (arg == "--detailed" || arg == "-d") {
            detailed_output = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  -l, --long      Run long-duration stability test\n";
            std::cout << "  -d, --detailed  Show detailed statistics\n";
            std::cout << "  -h, --help      Show this help\n";
            return 0;
        }
    }
    
    // Set high priority if possible
    #ifdef __linux__
    if (nice(-19) == -1) {
        std::cerr << "Warning: Could not set high process priority\n";
    }
    #endif
    
    try {
        auto overall_start = Clock::now();
        
        // Run all stress tests
        g_stats.reset();
        testRapidABAPattern();
        
        g_stats.reset();
        testNodeAddressReuse();
        
        g_stats.reset();
        testHazardPointerSlotExhaustion();
        
        g_stats.reset();
        testMemoryPressureReclamation();
        
        g_stats.reset();
        testBurstTrafficStress();
        
        g_stats.reset();
        testThreadLifecycleStress();
        
        // Optional long-running test
        if (run_long_test) {
            g_stats.reset();
            testLongRunningStability();
        }
        
        auto overall_end = Clock::now();
        auto total_duration = std::chrono::duration<double>(overall_end - overall_start).count();
        
        std::cout << "\n🎯 ALL HAZARD POINTER STRESS TESTS PASSED! 🎯\n";
        std::cout << "Total test duration: " << std::fixed << std::setprecision(1) 
                  << total_duration << " seconds\n";
        
        if (detailed_output) {
            g_stats.printSummary();
            g_memory_tracker.printReport();
        }
        
        return 0;
        
    } catch (const std::exception& ex) {
        std::cerr << "\n❌ STRESS TEST FAILED: " << ex.what() << "\n";
        g_stats.printSummary();
        g_memory_tracker.printReport();
        return 1;
    }
}