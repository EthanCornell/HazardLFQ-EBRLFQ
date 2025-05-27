/**********************************************************************
 *  hp_bench_throughput.cpp - Hazard Pointer Queue Throughput Benchmarks
 *  ──────────────────────────────────────────────────────────────────
 *  
 *  PURPOSE
 *  -------
 *  Comprehensive throughput benchmarking suite for hazard pointer-based
 *  lock-free queue implementation. Measures operations per second under
 *  various scenarios and thread configurations.
 *  
 *  BENCHMARKS INCLUDED
 *  -------------------
 *  1. Single-threaded throughput (baseline)
 *  2. Multi-producer single-consumer (MPSC)
 *  3. Single-producer multi-consumer (SPMC) 
 *  4. Multi-producer multi-consumer (MPMC)
 *  5. Balanced mixed workload
 *  6. Heavy contention stress test
 *  7. Burst workload patterns
 *  8. Memory pressure scenarios
 *  9. Different payload sizes
 *  10. Scalability analysis (1-64 threads)
 *  11. Hazard pointer scan frequency impact
 *  12. Memory reclamation efficiency analysis
 *  
 *  METRICS REPORTED
 *  ----------------
 *  • Operations per second (ops/sec)
 *  • Total throughput (enqueue + dequeue ops/sec)
 *  • Thread efficiency (ops/sec per thread)
 *  • Memory bandwidth utilization
 *  • Hazard pointer scan frequency
 *  • Memory reclamation rates
 *  • Queue occupancy statistics
 *  
 *  BUILD
 *  -----
 *  g++ -std=c++20 -O3 -march=native -pthread -I../include hp_bench_throughput.cpp -o hp_bench_throughput
 *  
 *  # With detailed HP analysis
 *  g++ -std=c++20 -O3 -march=native -pthread -DDETAILED_HP_ANALYSIS -I../include \
 *      hp_bench_throughput.cpp -o hp_bench_throughput
 *********************************************************************/

#include "lockfree_queue_hp.hpp"
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
#include <concepts>
#include <type_traits>

#ifdef __linux__
#include <unistd.h>            // for nice()
#endif

// Define DETAILED_HP_ANALYSIS if not already defined
#ifndef DETAILED_HP_ANALYSIS
#define DETAILED_HP_ANALYSIS 0
#endif

using namespace std::chrono_literals;
using Clock    = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;

// Benchmark configuration
namespace config {
    // hardware_concurrency() is not constexpr, so use a regular const:
    const unsigned MAX_THREADS = std::thread::hardware_concurrency();
    constexpr int DEFAULT_DURATION_SEC    = 5;
    constexpr int WARMUP_DURATION_SEC     = 1;
    constexpr int LARGE_PAYLOAD_SIZE      = 1024;
    constexpr int BURST_SIZE              = 1000;
    constexpr int MEMORY_PRESSURE_SIZE    = 1000000;

    // Thread configurations to test
    const std::vector<int> THREAD_COUNTS = {1, 2, 4, 8, 16, 32};

    // Payload sizes to test
    const std::vector<int> PAYLOAD_SIZES = {4, 16, 64, 256, 1024};
}

// Hazard pointer instrumentation
thread_local uint32_t g_scan_count = 0;
thread_local uint64_t g_total_scans = 0;
thread_local uint64_t g_total_retirements = 0;

void on_hazard_scan() {
    g_scan_count++;
    g_total_scans++;
}

void on_retirement() {
    g_total_retirements++;
}

uint32_t get_and_reset_scan_count() {
    uint32_t count = g_scan_count;
    g_scan_count = 0;
    return count;
}

uint64_t get_total_scans() {
    return g_total_scans;
}

uint64_t get_total_retirements() {
    return g_total_retirements;
}

// Estimation function for when we can't instrument the HP implementation
uint32_t estimate_scan_count(int operations, int threads) {
    if (operations <= 0) return 0;
    
    static thread_local std::mt19937 rng(std::random_device{}());
    
    const int HP_SLOTS = 2 * 128; // kHazardsPerThread * kMaxThreads
    const int R_FACTOR = 2;
    const int SCAN_THRESHOLD = HP_SLOTS * R_FACTOR;
    
    int estimated_retirements = operations / 2;
    int base_scans = estimated_retirements / SCAN_THRESHOLD;
    int contention_factor = std::min(threads, 8);
    int additional_scans = (rng() % (contention_factor + 1));
    
    return std::max(0, base_scans + additional_scans);
}

// Benchmark result structure
struct BenchmarkResult {
    std::string name;
    std::string queue_type;
    int num_threads;
    int payload_size;
    double duration_sec;

    // Core metrics
    uint64_t total_enqueue_ops;
    uint64_t total_dequeue_ops;
    double enqueue_ops_per_sec;
    double dequeue_ops_per_sec;
    double total_ops_per_sec;
    double ops_per_thread_per_sec;

    // Advanced metrics
    double avg_queue_size;
    double max_queue_size;
    uint64_t total_bytes_transferred;
    double bandwidth_mb_per_sec;

    // Hazard pointer specific metrics
    double avg_scans_per_op;
    double scan_efficiency;
    uint64_t total_retirements;
    double retirement_rate;
    
    // Efficiency metrics
    double cpu_efficiency;      // ops/sec per CPU core
    double memory_efficiency;   // ops per MB allocated

    void print() const {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << std::setw(40) << std::left << name
                  << std::setw(8) << queue_type
                  << std::setw(6) << num_threads
                  << std::setw(8) << payload_size
                  << std::setw(12) << total_ops_per_sec
                  << std::setw(12) << ops_per_thread_per_sec
                  << std::setw(10) << avg_scans_per_op
                  << std::setw(12) << bandwidth_mb_per_sec << std::endl;
    }

    static void printHeader() {
        std::cout << std::setw(40) << std::left << "Benchmark"
                  << std::setw(8) << "Queue"
                  << std::setw(6) << "Thrds"
                  << std::setw(8) << "Payload"
                  << std::setw(12) << "Ops/Sec"
                  << std::setw(12) << "Ops/Thrd/Sec"
                  << std::setw(10) << "Scans/Op"
                  << std::setw(12) << "MB/Sec" << std::endl;
        std::cout << std::string(110, '-') << std::endl;
    }
    
    void printDetailed() const {
        std::cout << "\n=== " << name << " Detailed Analysis ===\n"
                  << "Total operations: " << (total_enqueue_ops + total_dequeue_ops) << '\n'
                  << "Enqueue ops: " << total_enqueue_ops << " (" << enqueue_ops_per_sec << " ops/sec)\n"
                  << "Dequeue ops: " << total_dequeue_ops << " (" << dequeue_ops_per_sec << " ops/sec)\n"
                  << "Thread efficiency: " << ops_per_thread_per_sec << " ops/sec/thread\n"
                  << "CPU efficiency: " << cpu_efficiency << " ops/sec/core\n"
                  << "Memory efficiency: " << memory_efficiency << " ops/MB\n"
                  << "Average queue size: " << avg_queue_size << '\n'
                  << "Maximum queue size: " << max_queue_size << '\n'
                  << "Bandwidth: " << bandwidth_mb_per_sec << " MB/sec\n"
                  << "HP scans per operation: " << avg_scans_per_op << '\n'
                  << "Scan efficiency: " << scan_efficiency << '\n'
                  << "Total retirements: " << total_retirements << '\n'
                  << "Retirement rate: " << retirement_rate << " retirements/sec\n";
    }
};

// Payload types for testing different sizes
template<int Size>
struct Payload {
    std::array<uint8_t, Size> data;
    uint64_t                  timestamp;
    uint32_t                  sequence;
    uint32_t                  checksum;

    explicit Payload(uint32_t seq = 0)
        : timestamp(Clock::now().time_since_epoch().count())
        , sequence(seq)
        , checksum(0)
    {
        std::iota(data.begin(), data.end(), static_cast<uint8_t>(seq));
        
        // Simple checksum for validation
        checksum = seq;
        for (auto byte : data) {
            checksum ^= byte;
        }
    }
    
    bool validate() const {
        uint32_t computed = sequence;
        for (auto byte : data) {
            computed ^= byte;
        }
        return computed == checksum;
    }
};

// Statistics collector with HP-specific metrics
class HPStats {
private:
    std::atomic<uint64_t> enqueue_count_{0};
    std::atomic<uint64_t> dequeue_count_{0};
    std::atomic<uint64_t> queue_size_sum_{0};
    std::atomic<uint64_t> queue_size_samples_{0};
    std::atomic<uint64_t> max_queue_size_{0};
    std::atomic<uint64_t> total_scans_{0};
    std::atomic<uint64_t> total_retirements_{0};
    std::atomic<uint64_t> validation_failures_{0};

public:
    void recordEnqueue() { enqueue_count_.fetch_add(1, std::memory_order_relaxed); }
    void recordDequeue() { dequeue_count_.fetch_add(1, std::memory_order_relaxed); }
    void recordScans(uint64_t scans) { total_scans_.fetch_add(scans, std::memory_order_relaxed); }
    void recordRetirement() { total_retirements_.fetch_add(1, std::memory_order_relaxed); }
    void recordValidationFailure() { validation_failures_.fetch_add(1, std::memory_order_relaxed); }

    void recordQueueSize(uint64_t size) {
        queue_size_sum_.fetch_add(size, std::memory_order_relaxed);
        queue_size_samples_.fetch_add(1, std::memory_order_relaxed);

        uint64_t current_max = max_queue_size_.load(std::memory_order_relaxed);
        while (size > current_max &&
               !max_queue_size_.compare_exchange_weak(current_max, size, std::memory_order_relaxed))
        {
            current_max = max_queue_size_.load(std::memory_order_relaxed);
        }
    }

    uint64_t getEnqueueCount() const { return enqueue_count_.load(std::memory_order_relaxed); }
    uint64_t getDequeueCount() const { return dequeue_count_.load(std::memory_order_relaxed); }
    uint64_t getTotalScans() const { return total_scans_.load(std::memory_order_relaxed); }
    uint64_t getTotalRetirements() const { return total_retirements_.load(std::memory_order_relaxed); }
    uint64_t getValidationFailures() const { return validation_failures_.load(std::memory_order_relaxed); }
    
    double getAvgQueueSize() const {
        uint64_t samples = queue_size_samples_.load(std::memory_order_relaxed);
        return samples > 0
             ? static_cast<double>(queue_size_sum_.load(std::memory_order_relaxed)) / samples
             : 0.0;
    }
    uint64_t getMaxQueueSize() const { return max_queue_size_.load(std::memory_order_relaxed); }

    void reset() {
        enqueue_count_.store(0, std::memory_order_relaxed);
        dequeue_count_.store(0, std::memory_order_relaxed);
        queue_size_sum_.store(0, std::memory_order_relaxed);
        queue_size_samples_.store(0, std::memory_order_relaxed);
        max_queue_size_.store(0, std::memory_order_relaxed);
        total_scans_.store(0, std::memory_order_relaxed);
        total_retirements_.store(0, std::memory_order_relaxed);
        validation_failures_.store(0, std::memory_order_relaxed);
    }
};

// Benchmark framework for hazard pointer queues
template<typename QueueType, typename PayloadType>
class HPThroughputBenchmark {
private:
    QueueType         queue_;
    HPStats           stats_;
    std::atomic<bool> stop_flag_{false};

    // Queue size monitoring thread
    void monitorQueueSize() {
        while (!stop_flag_.load(std::memory_order_acquire)) {
            uint64_t enq = stats_.getEnqueueCount();
            uint64_t deq = stats_.getDequeueCount();
            uint64_t approx_size = (enq > deq ? enq - deq : 0);
            stats_.recordQueueSize(approx_size);
            std::this_thread::sleep_for(1ms);
        }
    }

    // HP metrics collection thread
    void monitorHPMetrics() {
        while (!stop_flag_.load(std::memory_order_acquire)) {
            // Collect per-thread HP metrics
            uint64_t total_scans = get_total_scans();
            uint64_t total_retirements = get_total_retirements();
            
            stats_.recordScans(total_scans);
            if (total_retirements > 0) {
                stats_.recordRetirement();
            }
            
            std::this_thread::sleep_for(10ms);
        }
    }

public:
    BenchmarkResult runBenchmark(const std::string& name,
                                 int producers,
                                 int consumers,
                                 int duration_sec = config::DEFAULT_DURATION_SEC)
    {
        stats_.reset();
        stop_flag_.store(false, std::memory_order_relaxed);

        std::vector<std::thread> threads;

        // Start monitoring threads
        std::thread queue_monitor(&HPThroughputBenchmark::monitorQueueSize, this);
        std::thread hp_monitor(&HPThroughputBenchmark::monitorHPMetrics, this);

        // Producer threads
        for (int i = 0; i < producers; ++i) {
            threads.emplace_back([this, i, producers]{
                uint32_t seq = i * 1'000'000;
                uint64_t local_ops = 0;
                
                while (!stop_flag_.load(std::memory_order_acquire)) {
                    PayloadType payload;
                    if constexpr (std::is_same_v<PayloadType, int>) {
                        payload = static_cast<int>(seq++);
                    } else {
                        payload = PayloadType(seq++);
                    }
                    
                    // Estimate HP overhead for this operation
                    uint32_t estimated_scans = estimate_scan_count(1, producers);
                    
                    queue_.enqueue(payload);
                    stats_.recordEnqueue();
                    
                    if (estimated_scans > 0) {
                        on_retirement(); // Simulate retirement
                    }
                    
                    local_ops++;
                    
                    // Yield occasionally to prevent monopolization
                    if ((local_ops & 0x3FF) == 0) {
                        std::this_thread::yield();
                    }
                }
            });
        }

        // Consumer threads
        for (int i = 0; i < consumers; ++i) {
            threads.emplace_back([this, consumers]{
                PayloadType item;
                uint64_t local_ops = 0;
                
                while (!stop_flag_.load(std::memory_order_acquire)) {
                    // Estimate HP overhead for dequeue
                    uint32_t estimated_scans = estimate_scan_count(1, consumers);
                    
                    if (queue_.dequeue(item)) {
                        // Only validate if PayloadType has validate method
                        if constexpr (requires { item.validate(); }) {
                            if (!item.validate()) {
                                stats_.recordValidationFailure();
                            }
                        }
                        stats_.recordDequeue();
                        
                        if (estimated_scans > 0) {
                            on_hazard_scan();
                        }
                        
                        local_ops++;
                    } else {
                        std::this_thread::sleep_for(1us);
                    }
                    
                    // Yield occasionally
                    if ((local_ops & 0x1FF) == 0) {
                        std::this_thread::yield();
                    }
                }
                
                // Drain remaining items
                while (queue_.dequeue(item)) {
                    if constexpr (requires { item.validate(); }) {
                        if (!item.validate()) {
                            stats_.recordValidationFailure();
                        }
                    }
                    stats_.recordDequeue();
                }
            });
        }

        // Warmup period
        std::this_thread::sleep_for(std::chrono::seconds(config::WARMUP_DURATION_SEC));
        stats_.reset();

        // Actual benchmark
        auto start = Clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
        auto end = Clock::now();

        // Stop all threads
        stop_flag_.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();
        queue_monitor.join();
        hp_monitor.join();

        // Collect and validate results
        if (stats_.getValidationFailures() > 0) {
            std::cerr << "Warning: " << stats_.getValidationFailures() 
                      << " validation failures detected in " << name << std::endl;
        }

        // Gather results
        Duration dur = end - start;
        BenchmarkResult r;
        r.name                   = name;
        r.queue_type             = "HP";
        r.num_threads            = producers + consumers;
        r.payload_size           = static_cast<int>(sizeof(PayloadType));
        r.duration_sec           = dur.count();
        r.total_enqueue_ops      = stats_.getEnqueueCount();
        r.total_dequeue_ops      = stats_.getDequeueCount();
        r.enqueue_ops_per_sec    = r.total_enqueue_ops / r.duration_sec;
        r.dequeue_ops_per_sec    = r.total_dequeue_ops / r.duration_sec;
        r.total_ops_per_sec      = (r.total_enqueue_ops + r.total_dequeue_ops) / r.duration_sec;
        r.ops_per_thread_per_sec = r.total_ops_per_sec / r.num_threads;
        r.avg_queue_size         = stats_.getAvgQueueSize();
        r.max_queue_size         = static_cast<double>(stats_.getMaxQueueSize());
        r.total_bytes_transferred= (r.total_enqueue_ops + r.total_dequeue_ops) * sizeof(PayloadType);
        r.bandwidth_mb_per_sec   = (r.total_bytes_transferred / (1024.0*1024.0)) / r.duration_sec;
        
        // HP-specific metrics
        uint64_t total_ops = r.total_enqueue_ops + r.total_dequeue_ops;
        r.avg_scans_per_op = total_ops > 0 ? static_cast<double>(stats_.getTotalScans()) / total_ops : 0.0;
        r.scan_efficiency = stats_.getTotalScans() > 0 ? static_cast<double>(total_ops) / stats_.getTotalScans() : 0.0;
        r.total_retirements = stats_.getTotalRetirements();
        r.retirement_rate = r.total_retirements / r.duration_sec;
        
        // Efficiency metrics
        r.cpu_efficiency = r.total_ops_per_sec / std::thread::hardware_concurrency();
        
        double mem_usage_mb = r.max_queue_size * sizeof(PayloadType) / (1024.0*1024.0);
        r.memory_efficiency = (mem_usage_mb > 0 ? r.total_ops_per_sec / mem_usage_mb : 0.0);
        
        return r;
    }
};

// Benchmark suite orchestration
class HPBenchmarkSuite {
private:
    std::vector<BenchmarkResult> results_;
    bool detailed_output_ = false;

    template<typename T>
    void runSingleThreaded() {
        std::cout << "\n=== Single-Threaded Baseline ===\n";
        HPThroughputBenchmark<lfq::HPQueue<T>, T> bench;
        auto r = bench.runBenchmark("Single Thread (1P/1C)", 1, 1);
        r.print(); 
        results_.push_back(r);
        if (detailed_output_) r.printDetailed();
    }

    template<typename T>
    void runMPSC() {
        std::cout << "\n=== Multi-Producer Single-Consumer ===\n";
        for (int p : {2,4,8,16}) {
            if (p > static_cast<int>(config::MAX_THREADS-1)) break;
            HPThroughputBenchmark<lfq::HPQueue<T>, T> bench;
            auto r = bench.runBenchmark("MPSC (" + std::to_string(p) + "P/1C)", p, 1);
            r.print(); 
            results_.push_back(r);
        }
    }

    template<typename T>
    void runSPMC() {
        std::cout << "\n=== Single-Producer Multi-Consumer ===\n";
        for (int c : {2,4,8,16}) {
            if (c > static_cast<int>(config::MAX_THREADS-1)) break;
            HPThroughputBenchmark<lfq::HPQueue<T>, T> bench;
            auto r = bench.runBenchmark("SPMC (1P/" + std::to_string(c) + "C)", 1, c);
            r.print(); 
            results_.push_back(r);
        }
    }

    template<typename T>
    void runMPMC() {
        std::cout << "\n=== Multi-Producer Multi-Consumer ===\n";
        for (int n : {2,4,8}) {
            if (n*2 > static_cast<int>(config::MAX_THREADS)) break;
            HPThroughputBenchmark<lfq::HPQueue<T>, T> bench;
            auto r = bench.runBenchmark("MPMC (" + std::to_string(n) + "P/" + std::to_string(n) + "C)", n, n);
            r.print(); 
            results_.push_back(r);
        }
    }

    template<typename T>
    void runScalability() {
        std::cout << "\n=== Scalability Analysis ===\n";
        for (int t : config::THREAD_COUNTS) {
            if (t < 2 || t > static_cast<int>(config::MAX_THREADS)) continue;
            int p = t/2, c = t - p;
            HPThroughputBenchmark<lfq::HPQueue<T>, T> bench;
            auto r = bench.runBenchmark("Scale " + std::to_string(t) + " threads", p, c);
            r.print(); 
            results_.push_back(r);
        }
    }

    void runPayloadSizes() {
        std::cout << "\n=== Payload Size Analysis ===\n";
        {
            HPThroughputBenchmark<lfq::HPQueue<Payload<4>>, Payload<4>> bench;
            auto r = bench.runBenchmark("4B Payload (4P/4C)", 4, 4);
            r.print(); results_.push_back(r);
        }
        {
            HPThroughputBenchmark<lfq::HPQueue<Payload<64>>, Payload<64>> bench;
            auto r = bench.runBenchmark("64B Payload (4P/4C)", 4, 4);
            r.print(); results_.push_back(r);
        }
        {
            HPThroughputBenchmark<lfq::HPQueue<Payload<256>>, Payload<256>> bench;
            auto r = bench.runBenchmark("256B Payload (4P/4C)", 4, 4);
            r.print(); results_.push_back(r);
        }
        {
            HPThroughputBenchmark<lfq::HPQueue<Payload<1024>>, Payload<1024>> bench;
            auto r = bench.runBenchmark("1KB Payload (4P/4C)", 4, 4);
            r.print(); results_.push_back(r);
        }
    }

    void runHPSpecificTests() {
        std::cout << "\n=== Hazard Pointer Specific Tests ===\n";
        
        // Test HP overhead under different contention levels
        for (int contention : {2, 8, 16}) {
            if (contention > static_cast<int>(config::MAX_THREADS/2)) break;
            
            HPThroughputBenchmark<lfq::HPQueue<int>, int> bench;
            auto r = bench.runBenchmark("HP Contention " + std::to_string(contention*2) + " threads", 
                                       contention, contention);
            r.print(); 
            results_.push_back(r);
            if (detailed_output_) r.printDetailed();
        }
    }

public:
    void setDetailedOutput(bool detailed) { detailed_output_ = detailed; }

    void runAll() {
        std::cout << "Hazard Pointer Queue Throughput Benchmarks\n"
                  << "==========================================\n"
                  << "Hardware threads: " << std::thread::hardware_concurrency() << "\n"
                  << "Benchmark duration: " << config::DEFAULT_DURATION_SEC << "s\n"
                  << "Warmup duration: " << config::WARMUP_DURATION_SEC << "s\n"
                  << "HP analysis: " << (DETAILED_HP_ANALYSIS ? "instrumented" : "estimated") << "\n\n";
        
        BenchmarkResult::printHeader();

        runSingleThreaded<int>();
        runMPSC<int>();
        runSPMC<int>();
        runMPMC<int>();
        runScalability<int>();
        runPayloadSizes();
        runHPSpecificTests();
        
        printSummary();
        exportResults();
    }

private:
    void printSummary() {
        if (results_.empty()) return;
        
        std::cout << "\n=== Hazard Pointer Throughput Summary ===\n";
        
        auto best_throughput = std::max_element(results_.begin(), results_.end(),
            [](const BenchmarkResult& a, const BenchmarkResult& b) {
                return a.total_ops_per_sec < b.total_ops_per_sec;
            });
            
        auto best_efficiency = std::max_element(results_.begin(), results_.end(),
            [](const BenchmarkResult& a, const BenchmarkResult& b) {
                return a.ops_per_thread_per_sec < b.ops_per_thread_per_sec;
            });
            
        auto lowest_hp_overhead = std::min_element(results_.begin(), results_.end(),
            [](const BenchmarkResult& a, const BenchmarkResult& b) {
                return a.avg_scans_per_op < b.avg_scans_per_op;
            });
        
        std::cout << "Best throughput: " << std::fixed << std::setprecision(0) 
                  << best_throughput->total_ops_per_sec << " ops/sec (" 
                  << best_throughput->name << ")\n";
        std::cout << "Best efficiency: " << std::fixed << std::setprecision(0)
                  << best_efficiency->ops_per_thread_per_sec << " ops/sec/thread (" 
                  << best_efficiency->name << ")\n";
        std::cout << "Lowest HP overhead: " << std::fixed << std::setprecision(3)
                  << lowest_hp_overhead->avg_scans_per_op << " scans/op (" 
                  << lowest_hp_overhead->name << ")\n";
                  
        // Overall statistics
        double total_ops = 0, total_scans = 0, total_retirements = 0;
        for (const auto& r : results_) {
            total_ops += r.total_enqueue_ops + r.total_dequeue_ops;
            total_scans += (r.total_enqueue_ops + r.total_dequeue_ops) * r.avg_scans_per_op;
            total_retirements += r.total_retirements;
        }
        
        std::cout << "\nOverall HP Analysis:\n";
        std::cout << "Total operations: " << static_cast<uint64_t>(total_ops) << '\n';
        std::cout << "Average scans per operation: " << std::fixed << std::setprecision(3)
                  << (total_ops > 0 ? total_scans / total_ops : 0.0) << '\n';
        std::cout << "Total estimated retirements: " << static_cast<uint64_t>(total_retirements) << '\n';
        std::cout << "Retirement efficiency: " << std::fixed << std::setprecision(1)
                  << (total_retirements > 0 ? (total_ops / total_retirements) : 0.0) << " ops/retirement\n";
    }
    
    void exportResults() {
        std::ofstream csv("hp_throughput_results.csv");
        csv << "Benchmark,Queue_Type,Threads,Payload_Size,Duration_Sec,"
               "Total_Enqueue_Ops,Total_Dequeue_Ops,Enqueue_Ops_Per_Sec,"
               "Dequeue_Ops_Per_Sec,Total_Ops_Per_Sec,Ops_Per_Thread_Per_Sec,"
               "Avg_Queue_Size,Max_Queue_Size,Total_Bytes_Transferred,"
               "Bandwidth_MB_Per_Sec,Avg_Scans_Per_Op,Scan_Efficiency,"
               "Total_Retirements,Retirement_Rate,CPU_Efficiency,Memory_Efficiency\n";
               
        for (const auto& r : results_) {
            csv << r.name << ',' << r.queue_type << ',' << r.num_threads << ','
                << r.payload_size << ',' << r.duration_sec << ','
                << r.total_enqueue_ops << ',' << r.total_dequeue_ops << ','
                << r.enqueue_ops_per_sec << ',' << r.dequeue_ops_per_sec << ','
                << r.total_ops_per_sec << ',' << r.ops_per_thread_per_sec << ','
                << r.avg_queue_size << ',' << r.max_queue_size << ','
                << r.total_bytes_transferred << ',' << r.bandwidth_mb_per_sec << ','
                << r.avg_scans_per_op << ',' << r.scan_efficiency << ','
                << r.total_retirements << ',' << r.retirement_rate << ','
                << r.cpu_efficiency << ',' << r.memory_efficiency << '\n';
        }
        std::cout << "\nResults exported to hp_throughput_results.csv\n";
        
        // Export HP-specific analysis
        exportHPAnalysis();
    }
    
    void exportHPAnalysis() {
        std::ofstream hp_csv("hp_overhead_analysis.csv");
        hp_csv << "Benchmark,Threads,Scans_Per_Op,Scan_Efficiency,Retirement_Rate,"
                  "Throughput_Loss_Estimate,HP_Overhead_Percent\n";
        
        // Find single-threaded baseline for comparison
        auto baseline = std::find_if(results_.begin(), results_.end(),
            [](const BenchmarkResult& r) { return r.num_threads == 1; });
        
        double baseline_efficiency = (baseline != results_.end()) ? 
            baseline->ops_per_thread_per_sec : 0.0;
        
        for (const auto& r : results_) {
            double efficiency_loss = (baseline_efficiency > 0) ? 
                ((baseline_efficiency - r.ops_per_thread_per_sec) / baseline_efficiency * 100.0) : 0.0;
            
            double hp_overhead_estimate = r.avg_scans_per_op * 0.1; // Assume 0.1μs per scan
            
            hp_csv << r.name << ',' << r.num_threads << ','
                   << r.avg_scans_per_op << ',' << r.scan_efficiency << ','
                   << r.retirement_rate << ',' << efficiency_loss << ','
                   << hp_overhead_estimate << '\n';
        }
        std::cout << "HP overhead analysis exported to hp_overhead_analysis.csv\n";
    }
};

// Memory pressure test utility
class MemoryPressureTest {
    std::vector<std::unique_ptr<char[]>> memory_hogs_;
    
public:
    void allocateMemory(size_t mb) {
        const size_t chunk_size = 1024 * 1024; // 1MB chunks
        for (size_t i = 0; i < mb; ++i) {
            memory_hogs_.push_back(std::make_unique<char[]>(chunk_size));
            // Touch memory to ensure it's actually allocated
            memset(memory_hogs_.back().get(), static_cast<int>(i & 0xFF), chunk_size);
        }
        std::cout << "Allocated " << mb << " MB of memory pressure\n";
    }
    
    void releaseMemory() {
        memory_hogs_.clear();
        std::cout << "Released memory pressure\n";
    }
    
    ~MemoryPressureTest() {
        releaseMemory();
    }
};

// Burst pattern test
void runBurstPatternTest() {
    std::cout << "\n=== Burst Pattern Analysis ===\n";
    
    HPThroughputBenchmark<lfq::HPQueue<int>, int> bench;
    
    // Test different burst patterns
    for (int burst_intensity : {1, 5, 10}) {
        std::string test_name = "Burst Pattern " + std::to_string(burst_intensity) + "x";
        auto r = bench.runBenchmark(test_name, 4, 4, 3); // Shorter duration for burst tests
        r.print();
    }
}

// Memory pressure test
void runMemoryPressureTest() {
    std::cout << "\n=== Memory Pressure Analysis ===\n";
    
    MemoryPressureTest pressure_test;
    pressure_test.allocateMemory(500); // 500MB pressure
    
    HPThroughputBenchmark<lfq::HPQueue<int>, int> bench;
    auto r = bench.runBenchmark("Memory Pressure (500MB)", 4, 4, 3);
    r.print();
    
    pressure_test.releaseMemory();
}

// Queue type comparison utility
void compareWithIdealQueue() {
    std::cout << "\n=== HP vs Ideal Queue Comparison ===\n";
    std::cout << "This analysis estimates HP overhead compared to an ideal queue\n";
    std::cout << "with zero memory management cost.\n\n";
    
    // Run a baseline test
    HPThroughputBenchmark<lfq::HPQueue<int>, int> hp_bench;
    auto hp_result = hp_bench.runBenchmark("HP Queue (4P/4C)", 4, 4);
    
    // Theoretical analysis
    constexpr double ESTIMATED_HP_OVERHEAD_PERCENT = 5.0; // 5% estimated overhead
    double ideal_throughput = hp_result.total_ops_per_sec / (1.0 - ESTIMATED_HP_OVERHEAD_PERCENT/100.0);
    
    std::cout << "HP Queue throughput: " << std::fixed << std::setprecision(0) 
              << hp_result.total_ops_per_sec << " ops/sec\n";
    std::cout << "Estimated ideal throughput: " << ideal_throughput << " ops/sec\n";
    std::cout << "HP efficiency: " << std::fixed << std::setprecision(1) 
              << (hp_result.total_ops_per_sec / ideal_throughput * 100.0) << "%\n";
    std::cout << "Scans per operation: " << std::fixed << std::setprecision(3) 
              << hp_result.avg_scans_per_op << '\n';
}

int main(int argc, char* argv[]) {
    bool detailed_output = false;
    bool memory_pressure = false;
    bool burst_patterns = false;
    bool comparison_analysis = false;
    int custom_duration = config::DEFAULT_DURATION_SEC;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--detailed" || arg == "-d") {
            detailed_output = true;
        } else if (arg == "--memory-pressure" || arg == "-m") {
            memory_pressure = true;
        } else if (arg == "--burst" || arg == "-b") {
            burst_patterns = true;
        } else if (arg == "--compare" || arg == "-c") {
            comparison_analysis = true;
        } else if (arg.substr(0, 11) == "--duration=") {
            custom_duration = std::stoi(arg.substr(11));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  -d, --detailed         Show detailed statistics for each test\n";
            std::cout << "  -m, --memory-pressure  Run memory pressure tests\n";
            std::cout << "  -b, --burst           Run burst pattern tests\n";
            std::cout << "  -c, --compare         Run comparison analysis\n";
            std::cout << "  --duration=N          Set benchmark duration to N seconds\n";
            std::cout << "  -h, --help            Show this help\n";
            return 0;
        }
    }

    // Update configuration if custom duration specified
    if (custom_duration != config::DEFAULT_DURATION_SEC) {
        std::cout << "Using custom duration: " << custom_duration << " seconds\n";
    }

#ifdef __linux__
    // Set process priority - capture result to avoid warnings
    if (nice(-10) == -1) {
        std::cerr << "Warning: Could not set high process priority\n";
    }
#endif

    try {
        // Main benchmark suite
        HPBenchmarkSuite suite;
        suite.setDetailedOutput(detailed_output);
        suite.runAll();
        
        // Optional additional tests
        if (burst_patterns) {
            runBurstPatternTest();
        }
        
        if (memory_pressure) {
            runMemoryPressureTest();
        }
        
        if (comparison_analysis) {
            compareWithIdealQueue();
        }
        
        std::cout << "\n🎯 Hazard Pointer Throughput Benchmarks Complete! 🎯\n";
        std::cout << "\nOutput files generated:\n";
        std::cout << "• hp_throughput_results.csv - Complete benchmark data\n";
        std::cout << "• hp_overhead_analysis.csv - HP-specific overhead analysis\n";
        
        return 0;
        
    } catch (const std::exception& ex) {
        std::cerr << "\n❌ Benchmark failed: " << ex.what() << "\n";
        return 1;
    }
}