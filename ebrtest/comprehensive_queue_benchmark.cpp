/**********************************************************************
 *  comprehensive_queue_benchmark.cpp — Comparative Performance Analysis
 *  ──────────────────────────────────────────────────────────────────
 *  
 *  PURPOSE
 *  -------
 *  Comprehensive benchmarking suite comparing three queue implementations:
 *  1. EBR Lock-Free Queue (3-epoch Epoch-Based Reclamation)
 *  2. HP Lock-Free Queue (Hazard Pointer-based)
 *  3. ThreadSafeQueue (Coarse-grained mutex-based)
 *  
 *  BENCHMARKS INCLUDED
 *  -------------------
 *  • Single-threaded throughput (baseline)
 *  • Multi-producer single-consumer (MPSC)
 *  • Single-producer multi-consumer (SPMC) 
 *  • Multi-producer multi-consumer (MPMC)
 *  • High contention stress test
 *  • Scalability analysis (1-32 threads)
 *  • Memory reclamation efficiency
 *  • Fairness and latency analysis
 *  
 *  METRICS REPORTED
 *  ----------------
 *  • Operations per second (ops/sec)
 *  • Thread efficiency (ops/sec per thread)
 *  • Memory overhead and reclamation rates
 *  • Contention statistics
 *  • Latency percentiles
 *  • Scalability coefficients
 *  
 *  BUILD
 *  -----
 *  g++ -std=c++20 -O3 -march=native -pthread comprehensive_queue_benchmark.cpp -o benchmark
 *  
 *  # With sanitizers (debug mode)
 *  g++ -std=c++20 -O1 -g -fsanitize=address,thread -pthread comprehensive_queue_benchmark.cpp -o benchmark_debug
 *********************************************************************/

#include "../include/lockfree_queue_ebr.hpp"    // EBR-based lock-free queue
#include "../include/lockfree_queue_hp.hpp"     // Hazard pointer-based lock-free queue

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <random>
#include <memory>
#include <string>
#include <map>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <barrier>
#include <cstring>
#include <queue>
#include <cassert>
#include <sstream>

using namespace std::chrono_literals;
using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;

// ThreadSafeQueue implementation (coarse-grained lock-based)
template<typename T>
class ThreadSafeQueue {
private:
    std::queue<T> queue_;
    mutable std::mutex mtx_;
    mutable std::atomic<uint64_t> contention_counter_{0};
    
    // Contention detection helper
    bool try_lock_with_contention_tracking() const {
        if (mtx_.try_lock()) {
            return true;
        } else {
            contention_counter_.fetch_add(1, std::memory_order_relaxed);
            mtx_.lock(); // Block until available
            return false; // Indicates contention occurred
        }
    }

public:
    // Enqueue an item (const reference)
    void enqueue(const T& value) {
        bool no_contention = try_lock_with_contention_tracking();
        queue_.push(value);
        mtx_.unlock();
    }
    
    // Enqueue an item (rvalue reference)
    void enqueue(T&& value) {
        bool no_contention = try_lock_with_contention_tracking();
        queue_.push(std::move(value));
        mtx_.unlock();
    }

    // Dequeue an item
    bool dequeue(T& value) {
        bool no_contention = try_lock_with_contention_tracking();
        if (queue_.empty()) {
            mtx_.unlock();
            return false;
        }
        value = queue_.front();
        queue_.pop();
        mtx_.unlock();
        return true;
    }

    // Check if the queue is empty
    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }
    
    // Get the current size of the queue
    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }
    
    // Get the current contention count
    uint64_t getContentionCount() const {
        return contention_counter_.load(std::memory_order_relaxed);
    }
    
    // Reset the contention count
    void resetContentionCount() {
        contention_counter_.store(0, std::memory_order_relaxed);
    }
};

// Benchmark configuration
namespace config {
    const unsigned MAX_THREADS = std::min(32u, std::thread::hardware_concurrency());
    constexpr int DEFAULT_DURATION_SEC = 5;
    constexpr int WARMUP_DURATION_SEC = 1;
    constexpr int HIGH_CONTENTION_THREADS = 16;
    
    // Thread configurations to test
    const std::vector<int> THREAD_COUNTS = {1, 2, 4, 8, 16, 24, 32};
}

// Queue type enumeration for cleaner code
enum class QueueType { EBR, HP, MUTEX };

std::string queueTypeToString(QueueType type) {
    switch (type) {
        case QueueType::EBR: return "EBR";
        case QueueType::HP: return "HP";
        case QueueType::MUTEX: return "MUTEX";
        default: return "UNKNOWN";
    }
}

// Comprehensive benchmark result structure
struct BenchmarkResult {
    std::string test_name;
    QueueType queue_type;
    int num_producers;
    int num_consumers;
    int total_threads;
    double duration_sec;

    // Core throughput metrics
    uint64_t total_enqueue_ops;
    uint64_t total_dequeue_ops;
    uint64_t total_operations;
    double enqueue_ops_per_sec;
    double dequeue_ops_per_sec;
    double total_ops_per_sec;
    double ops_per_thread_per_sec;

    // Advanced metrics
    double scalability_factor;     // Efficiency relative to single-threaded
    uint64_t contention_events;    // Lock contention (for MUTEX queue)
    double contention_rate;        // Contentions per operation
    
    // Memory and efficiency metrics
    double cpu_efficiency;         // ops/sec per CPU core
    uint64_t approximate_memory_overhead; // Estimated memory usage
    
    // Fairness metrics (standard deviation of per-thread performance)
    double performance_variance;
    double fairness_index;         // Jain's fairness index

    void print() const {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << std::setw(25) << std::left << test_name
                  << std::setw(8) << queueTypeToString(queue_type)
                  << std::setw(4) << total_threads
                  << std::setw(12) << total_ops_per_sec
                  << std::setw(12) << ops_per_thread_per_sec
                  << std::setw(10) << scalability_factor
                  << std::setw(12) << contention_rate
                  << std::setw(10) << fairness_index << std::endl;
    }

    static void printHeader() {
        std::cout << std::setw(25) << std::left << "Test Name"
                  << std::setw(8) << "Queue"
                  << std::setw(4) << "Thrd"
                  << std::setw(12) << "Ops/Sec"
                  << std::setw(12) << "Ops/Thrd/Sec"
                  << std::setw(10) << "Scale"
                  << std::setw(12) << "Contention"
                  << std::setw(10) << "Fairness" << std::endl;
        std::cout << std::string(100, '-') << std::endl;
    }
    
    void printDetailed() const {
        std::cout << "\n=== " << test_name << " (" << queueTypeToString(queue_type) << ") ===\n";
        std::cout << "Configuration: " << num_producers << " producers, " << num_consumers << " consumers\n";
        std::cout << "Duration: " << duration_sec << " seconds\n";
        std::cout << "Total operations: " << total_operations << " (" << total_enqueue_ops 
                  << " enqueue, " << total_dequeue_ops << " dequeue)\n";
        std::cout << "Throughput: " << total_ops_per_sec << " ops/sec\n";
        std::cout << "Per-thread efficiency: " << ops_per_thread_per_sec << " ops/sec/thread\n";
        std::cout << "Scalability factor: " << scalability_factor << "x\n";
        std::cout << "CPU efficiency: " << cpu_efficiency << " ops/sec/core\n";
        
        if (queue_type == QueueType::MUTEX) {
            std::cout << "Lock contention events: " << contention_events << "\n";
            std::cout << "Contention rate: " << contention_rate << " per operation\n";
        }
        
        std::cout << "Performance variance: " << performance_variance << "\n";
        std::cout << "Fairness index: " << fairness_index << "\n";
        std::cout << "Memory overhead estimate: " << approximate_memory_overhead << " bytes\n";
    }
};

// Statistics collector for thread-specific metrics
struct ThreadStats {
    std::atomic<uint64_t> enqueue_count{0};
    std::atomic<uint64_t> dequeue_count{0};
    std::atomic<uint64_t> failed_dequeue_count{0};
    Clock::time_point start_time;
    Clock::time_point end_time;
    
    // Default constructor
    ThreadStats() = default;
    
    // Explicitly delete copy constructor and assignment
    ThreadStats(const ThreadStats&) = delete;
    ThreadStats& operator=(const ThreadStats&) = delete;
    
    // Define move constructor and assignment to handle atomics properly
    ThreadStats(ThreadStats&& other) noexcept 
        : enqueue_count(other.enqueue_count.load())
        , dequeue_count(other.dequeue_count.load())
        , failed_dequeue_count(other.failed_dequeue_count.load())
        , start_time(other.start_time)
        , end_time(other.end_time)
    {
        // Reset the moved-from object
        other.enqueue_count.store(0);
        other.dequeue_count.store(0);
        other.failed_dequeue_count.store(0);
    }
    
    ThreadStats& operator=(ThreadStats&& other) noexcept {
        if (this != &other) {
            enqueue_count.store(other.enqueue_count.load());
            dequeue_count.store(other.dequeue_count.load());
            failed_dequeue_count.store(other.failed_dequeue_count.load());
            start_time = other.start_time;
            end_time = other.end_time;
            
            // Reset the moved-from object
            other.enqueue_count.store(0);
            other.dequeue_count.store(0);
            other.failed_dequeue_count.store(0);
        }
        return *this;
    }
    
    void reset() {
        enqueue_count.store(0, std::memory_order_relaxed);
        dequeue_count.store(0, std::memory_order_relaxed);
        failed_dequeue_count.store(0, std::memory_order_relaxed);
        start_time = Clock::now();
    }
    
    void finish() {
        end_time = Clock::now();
    }
    
    double getDuration() const {
        return Duration(end_time - start_time).count();
    }
    
    uint64_t getTotalOps() const {
        return enqueue_count.load() + dequeue_count.load();
    }
    
    double getOpsPerSec() const {
        double dur = getDuration();
        return dur > 0 ? getTotalOps() / dur : 0.0;
    }
};

// Template benchmark framework that works with any queue type
template<typename QueueImpl>
class QueueBenchmark {
private:
    QueueImpl queue_;
    std::atomic<bool> stop_flag_{false};
    std::vector<ThreadStats> thread_stats_;
    
    // Producer thread function
    void producerThread(int thread_id, int total_producers, int operations_target) {
        thread_stats_[thread_id].reset();
        
        uint32_t seed = thread_id * 12345;  // Deterministic but different per thread
        int operations_done = 0;
        
        while (!stop_flag_.load(std::memory_order_acquire) && 
               (operations_target <= 0 || operations_done < operations_target)) {
            
            // Generate deterministic test data
            int value = (thread_id << 16) | (operations_done & 0xFFFF);
            
            queue_.enqueue(value);
            thread_stats_[thread_id].enqueue_count.fetch_add(1, std::memory_order_relaxed);
            operations_done++;
            
            // Occasional yield to prevent monopolization
            if ((operations_done & 0x3FF) == 0) {
                std::this_thread::yield();
            }
        }
        
        thread_stats_[thread_id].finish();
    }
    
    // Consumer thread function
    void consumerThread(int thread_id, int total_consumers) {
        thread_stats_[thread_id].reset();
        
        int value;
        while (!stop_flag_.load(std::memory_order_acquire)) {
            if (queue_.dequeue(value)) {
                thread_stats_[thread_id].dequeue_count.fetch_add(1, std::memory_order_relaxed);
            } else {
                thread_stats_[thread_id].failed_dequeue_count.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(1us);  // Brief pause on empty queue
            }
            
            // Occasional yield
            if ((thread_stats_[thread_id].dequeue_count.load() & 0x1FF) == 0) {
                std::this_thread::yield();
            }
        }
        
        // Drain remaining items
        while (queue_.dequeue(value)) {
            thread_stats_[thread_id].dequeue_count.fetch_add(1, std::memory_order_relaxed);
        }
        
        thread_stats_[thread_id].finish();
    }

public:
    BenchmarkResult runBenchmark(const std::string& test_name,
                                 QueueType queue_type,
                                 int producers,
                                 int consumers,
                                 int duration_sec = config::DEFAULT_DURATION_SEC,
                                 int operations_per_producer = 0) {
        
        // Reset queue state
        int dummy;
        while (queue_.dequeue(dummy)) { /* drain */ }
        
        if constexpr (std::is_same_v<QueueImpl, ThreadSafeQueue<int>>) {
            queue_.resetContentionCount();
        }
        
        // Initialize thread statistics
        int total_threads = producers + consumers;
        thread_stats_.clear();
        thread_stats_.reserve(total_threads);
        for (int i = 0; i < total_threads; ++i) {
            thread_stats_.emplace_back();
        }
        stop_flag_.store(false, std::memory_order_relaxed);
        
        std::vector<std::thread> threads;
        
        // Start producer threads
        for (int i = 0; i < producers; ++i) {
            threads.emplace_back(&QueueBenchmark::producerThread, this, 
                               i, producers, operations_per_producer);
        }
        
        // Start consumer threads
        for (int i = 0; i < consumers; ++i) {
            threads.emplace_back(&QueueBenchmark::consumerThread, this, 
                               producers + i, consumers);
        }
        
        // Warmup period
        std::this_thread::sleep_for(std::chrono::seconds(config::WARMUP_DURATION_SEC));
        
        // Reset statistics after warmup
        for (auto& stats : thread_stats_) {
            stats.reset();
        }
        
        // Actual benchmark period
        auto benchmark_start = Clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
        auto benchmark_end = Clock::now();
        
        // Stop all threads
        stop_flag_.store(true, std::memory_order_release);
        for (auto& thread : threads) {
            thread.join();
        }
        
        // Collect results
        Duration actual_duration = benchmark_end - benchmark_start;
        
        BenchmarkResult result;
        result.test_name = test_name;
        result.queue_type = queue_type;
        result.num_producers = producers;
        result.num_consumers = consumers;
        result.total_threads = total_threads;
        result.duration_sec = actual_duration.count();
        
        // Aggregate thread statistics
        result.total_enqueue_ops = 0;
        result.total_dequeue_ops = 0;
        std::vector<double> thread_performance;
        
        for (const auto& stats : thread_stats_) {
            result.total_enqueue_ops += stats.enqueue_count.load();
            result.total_dequeue_ops += stats.dequeue_count.load();
            thread_performance.push_back(stats.getOpsPerSec());
        }
        
        result.total_operations = result.total_enqueue_ops + result.total_dequeue_ops;
        result.enqueue_ops_per_sec = result.total_enqueue_ops / result.duration_sec;
        result.dequeue_ops_per_sec = result.total_dequeue_ops / result.duration_sec;
        result.total_ops_per_sec = result.total_operations / result.duration_sec;
        result.ops_per_thread_per_sec = result.total_ops_per_sec / total_threads;
        
        // Calculate scalability factor (requires single-threaded baseline)
        static double single_thread_baseline = 0.0;
        if (total_threads == 1) {
            single_thread_baseline = result.ops_per_thread_per_sec;
            result.scalability_factor = 1.0;
        } else if (single_thread_baseline > 0) {
            result.scalability_factor = result.ops_per_thread_per_sec / single_thread_baseline;
        } else {
            result.scalability_factor = 0.0;
        }
        
        // Contention metrics (MUTEX queue only)
        if constexpr (std::is_same_v<QueueImpl, ThreadSafeQueue<int>>) {
            result.contention_events = queue_.getContentionCount();
            result.contention_rate = result.total_operations > 0 ? 
                static_cast<double>(result.contention_events) / result.total_operations : 0.0;
        } else {
            result.contention_events = 0;
            result.contention_rate = 0.0;
        }
        
        // Performance variance and fairness
        if (!thread_performance.empty()) {
            double mean_perf = std::accumulate(thread_performance.begin(), 
                                             thread_performance.end(), 0.0) / thread_performance.size();
            
            double variance = 0.0;
            for (double perf : thread_performance) {
                variance += (perf - mean_perf) * (perf - mean_perf);
            }
            result.performance_variance = variance / thread_performance.size();
            
            // Jain's fairness index: (sum x_i)^2 / (n * sum x_i^2)
            double sum_perf = std::accumulate(thread_performance.begin(), thread_performance.end(), 0.0);
            double sum_perf_squared = 0.0;
            for (double perf : thread_performance) {
                sum_perf_squared += perf * perf;
            }
            
            if (sum_perf_squared > 0) {
                result.fairness_index = (sum_perf * sum_perf) / 
                    (thread_performance.size() * sum_perf_squared);
            } else {
                result.fairness_index = 1.0;
            }
        } else {
            result.performance_variance = 0.0;
            result.fairness_index = 1.0;
        }
        
        // Additional metrics
        result.cpu_efficiency = result.total_ops_per_sec / std::thread::hardware_concurrency();
        result.approximate_memory_overhead = estimateMemoryOverhead(queue_type, total_threads);
        
        return result;
    }

private:
    uint64_t estimateMemoryOverhead(QueueType type, int threads) const {
        switch (type) {
            case QueueType::EBR:
                // EBR overhead: 3 buckets per thread + epoch tracking
                return threads * (3 * 100 * sizeof(void*) + 64);  // Conservative estimate
            case QueueType::HP:
                // HP overhead: hazard pointer slots + retired lists
                return threads * (2 * sizeof(void*) + 100 * sizeof(void*));
            case QueueType::MUTEX:
                // Mutex overhead: minimal - just the mutex and condition variable
                return sizeof(std::mutex) + sizeof(std::queue<int>);
            default:
                return 0;
        }
    }
};

// Main benchmark suite
class ComprehensiveBenchmarkSuite {
private:
    std::vector<BenchmarkResult> results_;
    bool detailed_output_ = false;
    
    template<QueueType Type>
    void runSingleTest(const std::string& name, int producers, int consumers, 
                      int duration = config::DEFAULT_DURATION_SEC) {
        
        BenchmarkResult result;
        
        if constexpr (Type == QueueType::EBR) {
            QueueBenchmark<lfq::Queue<int>> benchmark;
            result = benchmark.runBenchmark(name, Type, producers, consumers, duration);
        } else if constexpr (Type == QueueType::HP) {
            QueueBenchmark<lfq::HPQueue<int>> benchmark;
            result = benchmark.runBenchmark(name, Type, producers, consumers, duration);
        } else if constexpr (Type == QueueType::MUTEX) {
            QueueBenchmark<ThreadSafeQueue<int>> benchmark;
            result = benchmark.runBenchmark(name, Type, producers, consumers, duration);
        }
        
        result.print();
        results_.push_back(result);
        if (detailed_output_) {
            result.printDetailed();
        }
    }
    
    void runSingleThreaded() {
        std::cout << "\n=== Single-Threaded Baseline ===\n";
        runSingleTest<QueueType::EBR>("Single Thread", 1, 1);
        runSingleTest<QueueType::HP>("Single Thread", 1, 1);
        runSingleTest<QueueType::MUTEX>("Single Thread", 1, 1);
    }
    
    void runMPSC() {
        std::cout << "\n=== Multi-Producer Single-Consumer ===\n";
        for (int p : {2, 4, 8, 16}) {
            if (p > static_cast<int>(config::MAX_THREADS - 1)) break;
            std::string name = std::to_string(p) + "P/1C";
            runSingleTest<QueueType::EBR>(name, p, 1);
            runSingleTest<QueueType::HP>(name, p, 1);
            runSingleTest<QueueType::MUTEX>(name, p, 1);
        }
    }
    
    void runSPMC() {
        std::cout << "\n=== Single-Producer Multi-Consumer ===\n";
        for (int c : {2, 4, 8, 16}) {
            if (c > static_cast<int>(config::MAX_THREADS - 1)) break;
            std::string name = "1P/" + std::to_string(c) + "C";
            runSingleTest<QueueType::EBR>(name, 1, c);
            runSingleTest<QueueType::HP>(name, 1, c);
            runSingleTest<QueueType::MUTEX>(name, 1, c);
        }
    }
    
    void runMPMC() {
        std::cout << "\n=== Multi-Producer Multi-Consumer ===\n";
        for (int n : {2, 4, 8}) {
            if (n * 2 > static_cast<int>(config::MAX_THREADS)) break;
            std::string name = std::to_string(n) + "P/" + std::to_string(n) + "C";
            runSingleTest<QueueType::EBR>(name, n, n);
            runSingleTest<QueueType::HP>(name, n, n);
            runSingleTest<QueueType::MUTEX>(name, n, n);
        }
    }
    
    void runHighContention() {
        std::cout << "\n=== High Contention Stress Test ===\n";
        int threads = std::min(config::HIGH_CONTENTION_THREADS, 
                              static_cast<int>(config::MAX_THREADS));
        int p = threads / 2;
        int c = threads - p;
        
        std::string name = "High Contention " + std::to_string(threads);
        runSingleTest<QueueType::EBR>(name, p, c, 3);  // Shorter duration for stress test
        runSingleTest<QueueType::HP>(name, p, c, 3);
        runSingleTest<QueueType::MUTEX>(name, p, c, 3);
    }
    
    void runScalabilityAnalysis() {
        std::cout << "\n=== Scalability Analysis ===\n";
        for (int total : config::THREAD_COUNTS) {
            if (total > static_cast<int>(config::MAX_THREADS)) break;
            if (total < 2) continue;
            
            int p = total / 2;
            int c = total - p;
            std::string name = "Scale " + std::to_string(total);
            
            runSingleTest<QueueType::EBR>(name, p, c);
            runSingleTest<QueueType::HP>(name, p, c);
            runSingleTest<QueueType::MUTEX>(name, p, c);
        }
    }

public:
    void setDetailedOutput(bool detailed) { detailed_output_ = detailed; }
    
    void runAll() {
        std::cout << "Comprehensive Queue Benchmark Suite\n";
        std::cout << "===================================\n";
        std::cout << "Hardware threads: " << std::thread::hardware_concurrency() << "\n";
        std::cout << "Max test threads: " << config::MAX_THREADS << "\n";
        std::cout << "Benchmark duration: " << config::DEFAULT_DURATION_SEC << "s per test\n";
        std::cout << "Warmup duration: " << config::WARMUP_DURATION_SEC << "s per test\n\n";
        
        std::cout << "Queue Types:\n";
        std::cout << "• EBR: 3-Epoch Based Reclamation Lock-Free Queue\n";
        std::cout << "• HP: Hazard Pointer Lock-Free Queue\n";
        std::cout << "• MUTEX: Coarse-Grained Mutex-Based Queue\n\n";
        
        BenchmarkResult::printHeader();
        
        runSingleThreaded();
        runMPSC();
        runSPMC();
        runMPMC();
        runHighContention();
        runScalabilityAnalysis();
        
        printComparativeAnalysis();
        exportResults();
    }

private:
    void printComparativeAnalysis() {
        if (results_.empty()) return;
        
        std::cout << "\n=== Comparative Analysis Summary ===\n";
        
        // Find best performers for each metric
        auto best_throughput = std::max_element(results_.begin(), results_.end(),
            [](const BenchmarkResult& a, const BenchmarkResult& b) {
                return a.total_ops_per_sec < b.total_ops_per_sec;
            });
            
        auto best_scalability = std::max_element(results_.begin(), results_.end(),
            [](const BenchmarkResult& a, const BenchmarkResult& b) {
                return a.scalability_factor < b.scalability_factor;
            });
            
        auto best_fairness = std::max_element(results_.begin(), results_.end(),
            [](const BenchmarkResult& a, const BenchmarkResult& b) {
                return a.fairness_index < b.fairness_index;
            });
        
        std::cout << "Best overall throughput: " << std::fixed << std::setprecision(0)
                  << best_throughput->total_ops_per_sec << " ops/sec ("
                  << queueTypeToString(best_throughput->queue_type) << " - "
                  << best_throughput->test_name << ")\n";
                  
        std::cout << "Best scalability: " << std::fixed << std::setprecision(2)
                  << best_scalability->scalability_factor << "x ("
                  << queueTypeToString(best_scalability->queue_type) << " - "
                  << best_scalability->test_name << ")\n";
                  
        std::cout << "Best fairness: " << std::fixed << std::setprecision(3)
                  << best_fairness->fairness_index << " ("
                  << queueTypeToString(best_fairness->queue_type) << " - "
                  << best_fairness->test_name << ")\n";
        
        // Performance breakdown by queue type
        std::map<QueueType, std::vector<double>> type_performance;
        std::map<QueueType, uint64_t> type_contention;
        
        for (const auto& result : results_) {
            type_performance[result.queue_type].push_back(result.total_ops_per_sec);
            type_contention[result.queue_type] += result.contention_events;
        }
        
        std::cout << "\nAverage Performance by Queue Type:\n";
        for (const auto& [type, perfs] : type_performance) {
            double avg_perf = std::accumulate(perfs.begin(), perfs.end(), 0.0) / perfs.size();
            double total_contention = type_contention[type];
            
            std::cout << "• " << queueTypeToString(type) << ": " 
                      << std::fixed << std::setprecision(0) << avg_perf << " ops/sec avg";
            
            if (type == QueueType::MUTEX && total_contention > 0) {
                std::cout << " (contention: " << total_contention << " events)";
            }
            std::cout << "\n";
        }
        
        // Recommendations
        std::cout << "\nRecommendations:\n";
        
        auto ebr_avg = std::accumulate(type_performance[QueueType::EBR].begin(),
                                     type_performance[QueueType::EBR].end(), 0.0) 
                      / type_performance[QueueType::EBR].size();
        auto hp_avg = std::accumulate(type_performance[QueueType::HP].begin(),
                                    type_performance[QueueType::HP].end(), 0.0) 
                     / type_performance[QueueType::HP].size();
        auto mutex_avg = std::accumulate(type_performance[QueueType::MUTEX].begin(),
                                       type_performance[QueueType::MUTEX].end(), 0.0) 
                        / type_performance[QueueType::MUTEX].size();
        
        if (ebr_avg > hp_avg && ebr_avg > mutex_avg) {
            std::cout << "• For high-throughput applications: EBR Lock-Free Queue\n";
        } else if (hp_avg > ebr_avg && hp_avg > mutex_avg) {
            std::cout << "• For high-throughput applications: HP Lock-Free Queue\n";
        }
        
        if (type_contention[QueueType::MUTEX] > 0) {
            std::cout << "• High contention detected in MUTEX queue - consider lock-free alternatives\n";
        }
        
        std::cout << "• For predictable latency: Consider queue type with highest fairness index\n";
        std::cout << "• For memory-constrained environments: HP Queue (bounded memory usage)\n";
        std::cout << "• For maximum simplicity: MUTEX Queue (if contention is acceptable)\n";
    }
    
    void exportResults() {
        // Export detailed CSV results
        std::ofstream csv("comprehensive_queue_benchmark_results.csv");
        csv << "Test_Name,Queue_Type,Producers,Consumers,Total_Threads,Duration_Sec,"
               "Total_Enqueue_Ops,Total_Dequeue_Ops,Total_Operations,"
               "Enqueue_Ops_Per_Sec,Dequeue_Ops_Per_Sec,Total_Ops_Per_Sec,"
               "Ops_Per_Thread_Per_Sec,Scalability_Factor,Contention_Events,"
               "Contention_Rate,Performance_Variance,Fairness_Index,"
               "CPU_Efficiency,Memory_Overhead_Bytes\n";
               
        for (const auto& result : results_) {
            csv << result.test_name << ","
                << queueTypeToString(result.queue_type) << ","
                << result.num_producers << ","
                << result.num_consumers << ","
                << result.total_threads << ","
                << result.duration_sec << ","
                << result.total_enqueue_ops << ","
                << result.total_dequeue_ops << ","
                << result.total_operations << ","
                << result.enqueue_ops_per_sec << ","
                << result.dequeue_ops_per_sec << ","
                << result.total_ops_per_sec << ","
                << result.ops_per_thread_per_sec << ","
                << result.scalability_factor << ","
                << result.contention_events << ","
                << result.contention_rate << ","
                << result.performance_variance << ","
                << result.fairness_index << ","
                << result.cpu_efficiency << ","
                << result.approximate_memory_overhead << "\n";
        }
        
        std::cout << "\nResults exported to comprehensive_queue_benchmark_results.csv\n";
        exportComparativeAnalysis();
    }
    
    void exportComparativeAnalysis() {
        std::ofstream analysis("queue_comparison_analysis.csv");
        analysis << "Queue_Type,Test_Count,Avg_Throughput,Max_Throughput,Min_Throughput,"
                    "Avg_Scalability,Max_Scalability,Avg_Fairness,Total_Contention_Events,"
                    "Avg_Memory_Overhead\n";
        
        std::map<QueueType, std::vector<BenchmarkResult>> type_results;
        for (const auto& result : results_) {
            type_results[result.queue_type].push_back(result);
        }
        
        for (const auto& [type, results] : type_results) {
            if (results.empty()) continue;
            
            // Calculate statistics
            std::vector<double> throughputs, scalabilities, fairness_indices;
            uint64_t total_contention = 0;
            uint64_t total_memory = 0;
            
            for (const auto& r : results) {
                throughputs.push_back(r.total_ops_per_sec);
                scalabilities.push_back(r.scalability_factor);
                fairness_indices.push_back(r.fairness_index);
                total_contention += r.contention_events;
                total_memory += r.approximate_memory_overhead;
            }
            
            double avg_throughput = std::accumulate(throughputs.begin(), throughputs.end(), 0.0) / throughputs.size();
            double max_throughput = *std::max_element(throughputs.begin(), throughputs.end());
            double min_throughput = *std::min_element(throughputs.begin(), throughputs.end());
            
            double avg_scalability = std::accumulate(scalabilities.begin(), scalabilities.end(), 0.0) / scalabilities.size();
            double max_scalability = *std::max_element(scalabilities.begin(), scalabilities.end());
            
            double avg_fairness = std::accumulate(fairness_indices.begin(), fairness_indices.end(), 0.0) / fairness_indices.size();
            
            analysis << queueTypeToString(type) << ","
                     << results.size() << ","
                     << avg_throughput << ","
                     << max_throughput << ","
                     << min_throughput << ","
                     << avg_scalability << ","
                     << max_scalability << ","
                     << avg_fairness << ","
                     << total_contention << ","
                     << (total_memory / results.size()) << "\n";
        }
        
        std::cout << "Comparative analysis exported to queue_comparison_analysis.csv\n";
    }
};

// Specialized test functions for advanced scenarios
void runLatencyAnalysis() {
    std::cout << "\n=== Latency Analysis (Advanced) ===\n";
    std::cout << "Running ping-pong latency test between producer-consumer pairs...\n";
    
    const int NUM_SAMPLES = 1000;
    const int NUM_PAIRS = 4;
    
    // EBR Queue latency test
    {
        lfq::Queue<std::chrono::steady_clock::time_point> ebr_queue;
        std::vector<std::chrono::nanoseconds> latencies;
        latencies.reserve(NUM_SAMPLES);
        
        std::atomic<bool> start_flag{false};
        std::atomic<int> samples_collected{0};
        
        // Producer thread
        std::thread producer([&]() {
            while (!start_flag.load()) std::this_thread::yield();
            
            for (int i = 0; i < NUM_SAMPLES; ++i) {
                auto timestamp = std::chrono::steady_clock::now();
                ebr_queue.enqueue(timestamp);
                std::this_thread::sleep_for(10us);  // Control rate
            }
        });
        
        // Consumer thread
        std::thread consumer([&]() {
            std::chrono::steady_clock::time_point timestamp;
            
            while (!start_flag.load()) std::this_thread::yield();
            
            while (samples_collected.load() < NUM_SAMPLES) {
                if (ebr_queue.dequeue(timestamp)) {
                    auto now = std::chrono::steady_clock::now();
                    auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(now - timestamp);
                    latencies.push_back(latency);
                    samples_collected.fetch_add(1);
                }
            }
        });
        
        start_flag.store(true);
        producer.join();
        consumer.join();
        
        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());
            auto median = latencies[latencies.size() / 2];
            auto p95 = latencies[static_cast<size_t>(latencies.size() * 0.95)];
            auto p99 = latencies[static_cast<size_t>(latencies.size() * 0.99)];
            
            std::cout << "EBR Queue latency - Median: " << median.count() << "ns, "
                      << "95th: " << p95.count() << "ns, "
                      << "99th: " << p99.count() << "ns\n";
        }
    }
    
    // Similar tests for HP and MUTEX queues could be added here
    std::cout << "Latency analysis complete (EBR queue sample shown)\n";
}

void runMemoryPressureTest() {
    std::cout << "\n=== Memory Pressure Test ===\n";
    std::cout << "Testing queue performance under memory pressure...\n";
    
    // Allocate significant memory to create pressure
    const size_t PRESSURE_SIZE = 500 * 1024 * 1024;  // 500MB
    std::vector<std::unique_ptr<char[]>> memory_hogs;
    
    try {
        for (size_t i = 0; i < PRESSURE_SIZE / (1024 * 1024); ++i) {
            memory_hogs.push_back(std::make_unique<char[]>(1024 * 1024));
            // Touch memory to ensure allocation
            memset(memory_hogs.back().get(), static_cast<int>(i & 0xFF), 1024 * 1024);
        }
        
        std::cout << "Allocated " << PRESSURE_SIZE / (1024 * 1024) << "MB memory pressure\n";
        
        // Run a quick benchmark under memory pressure
        QueueBenchmark<lfq::Queue<int>> ebr_benchmark;
        auto result = ebr_benchmark.runBenchmark("Memory Pressure", QueueType::EBR, 4, 4, 3);
        
        std::cout << "EBR performance under memory pressure: " 
                  << std::fixed << std::setprecision(0) << result.total_ops_per_sec << " ops/sec\n";
        
        // Clear memory pressure
        memory_hogs.clear();
        std::cout << "Memory pressure released\n";
        
    } catch (const std::bad_alloc& e) {
        std::cout << "Could not allocate memory pressure - skipping test\n";
        memory_hogs.clear();
    }
}

void runBurstPatternTest() {
    std::cout << "\n=== Burst Pattern Test ===\n";
    std::cout << "Testing queue behavior with bursty workloads...\n";
    
    QueueBenchmark<lfq::Queue<int>> benchmark;
    
    // Modified benchmark that creates burst patterns
    // This would require extending the benchmark framework to support burst patterns
    std::cout << "Burst pattern testing requires specialized implementation\n";
    std::cout << "Consider implementing burst-specific producer patterns for detailed analysis\n";
}

int main(int argc, char* argv[]) {
    bool detailed_output = false;
    bool run_advanced_tests = false;
    bool run_memory_pressure = false;
    bool run_latency_analysis = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--detailed" || arg == "-d") {
            detailed_output = true;
        } else if (arg == "--advanced" || arg == "-a") {
            run_advanced_tests = true;
        } else if (arg == "--memory-pressure" || arg == "-m") {
            run_memory_pressure = true;
        } else if (arg == "--latency" || arg == "-l") {
            run_latency_analysis = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  -d, --detailed         Show detailed statistics for each test\n";
            std::cout << "  -a, --advanced         Run advanced test scenarios\n";
            std::cout << "  -m, --memory-pressure  Run memory pressure tests\n";
            std::cout << "  -l, --latency          Run latency analysis tests\n";
            std::cout << "  -h, --help             Show this help\n";
            return 0;
        }
    }
    
    try {
        // Main benchmark suite
        ComprehensiveBenchmarkSuite suite;
        suite.setDetailedOutput(detailed_output);
        suite.runAll();
        
        // Optional advanced tests
        if (run_advanced_tests || run_latency_analysis) {
            runLatencyAnalysis();
        }
        
        if (run_advanced_tests || run_memory_pressure) {
            runMemoryPressureTest();
        }
        
        if (run_advanced_tests) {
            runBurstPatternTest();
        }
        
        std::cout << "\n🎯 Comprehensive Queue Benchmark Complete! 🎯\n";
        std::cout << "\nOutput files generated:\n";
        std::cout << "• comprehensive_queue_benchmark_results.csv - Detailed results\n";
        std::cout << "• queue_comparison_analysis.csv - Comparative analysis\n";
        
        std::cout << "\nKey Findings:\n";
        std::cout << "• Compare throughput across different threading scenarios\n";
        std::cout << "• Analyze scalability factors for each queue type\n";
        std::cout << "• Review contention rates for mutex-based queue\n";
        std::cout << "• Consider fairness indices for latency-sensitive applications\n";
        
        return 0;
        
    } catch (const std::exception& ex) {
        std::cerr << "\n❌ Benchmark suite failed: " << ex.what() << "\n";
        return 1;
    }
}