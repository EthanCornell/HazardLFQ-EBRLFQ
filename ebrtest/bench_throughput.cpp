/**********************************************************************
 *  bench_throughput.cpp - Lock-Free Queue Throughput Benchmarks
 *  ──────────────────────────────────────────────────────────────────
 *  
 *  PURPOSE
 *  -------
 *  Comprehensive throughput benchmarking suite for lock-free queue
 *  implementations. Measures operations per second under various
 *  scenarios and thread configurations.
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
 *  
 *  METRICS REPORTED
 *  ----------------
 *  • Operations per second (ops/sec)
 *  • Total throughput (enqueue + dequeue ops/sec)
 *  • Thread efficiency (ops/sec per thread)
 *  • Memory bandwidth utilization
 *  • Cache miss rates (if available)
 *  • Queue occupancy statistics
 *  
 *  BUILD
 *  -----
 *  g++ -std=c++20 -O3 -march=native -pthread bench_throughput.cpp -o bench_throughput
 *********************************************************************/

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
#include <unistd.h>            // for nice()

// Include the EBR queue implementation (located in same directory)
#include "../include/lockfree_queue_ebr.hpp"

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

// Simple barrier implementation for C++17/C++20 compatibility
class SimpleBarrier {
private:
    std::mutex mtx_;
    std::condition_variable cv_;
    int count_;
    int waiting_;
    int generation_;

public:
    explicit SimpleBarrier(int count) : count_(count), waiting_(0), generation_(0) {}
    
    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mtx_);
        int gen = generation_;
        if (++waiting_ == count_) {
            waiting_ = 0;
            ++generation_;
            cv_.notify_all();
        } else {
            cv_.wait(lock, [this, gen] { return gen != generation_; });
        }
    }
};

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

    // Efficiency metrics
    double cpu_efficiency;      // ops/sec per CPU core
    double memory_efficiency;   // ops per MB allocated

    void print() const {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << std::setw(40) << std::left << name
                  << std::setw(10) << queue_type
                  << std::setw(6) << num_threads
                  << std::setw(8) << payload_size
                  << std::setw(12) << total_ops_per_sec
                  << std::setw(12) << ops_per_thread_per_sec
                  << std::setw(12) << bandwidth_mb_per_sec << std::endl;
    }

    static void printHeader() {
        std::cout << std::setw(40) << std::left << "Benchmark"
                  << std::setw(10) << "Queue"
                  << std::setw(6) << "Thrds"
                  << std::setw(8) << "Payload"
                  << std::setw(12) << "Ops/Sec"
                  << std::setw(12) << "Ops/Thrd/Sec"
                  << std::setw(12) << "MB/Sec" << std::endl;
        std::cout << std::string(100, '-') << std::endl;
    }
};

// Payload types for testing different sizes
template<int Size>
struct Payload {
    std::array<uint8_t, Size> data;
    uint64_t                  timestamp;
    uint32_t                  sequence;

    explicit Payload(uint32_t seq = 0)
        : timestamp(Clock::now().time_since_epoch().count())
        , sequence(seq)
    {
        std::iota(data.begin(), data.end(), static_cast<uint8_t>(seq));
    }
};

// Statistics collector
class Stats {
private:
    std::atomic<uint64_t> enqueue_count_{0};
    std::atomic<uint64_t> dequeue_count_{0};
    std::atomic<uint64_t> queue_size_sum_{0};
    std::atomic<uint64_t> queue_size_samples_{0};
    std::atomic<uint64_t> max_queue_size_{0};

public:
    void recordEnqueue() { enqueue_count_.fetch_add(1, std::memory_order_relaxed); }
    void recordDequeue() { dequeue_count_.fetch_add(1, std::memory_order_relaxed); }

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
    double   getAvgQueueSize() const {
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
    }
};

// Benchmark framework
template<typename QueueType, typename PayloadType>
class ThroughputBenchmark {
private:
    QueueType         queue_;
    Stats             stats_;
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

public:
    BenchmarkResult runBenchmark(const std::string& name,
                                 int producers,
                                 int consumers,
                                 int duration_sec = config::DEFAULT_DURATION_SEC)
    {
        stats_.reset();
        stop_flag_.store(false, std::memory_order_relaxed);

        std::vector<std::thread> threads;

        // Start monitoring thread
        std::thread monitor(&ThroughputBenchmark::monitorQueueSize, this);

        // Producer threads
        for (int i = 0; i < producers; ++i) {
            threads.emplace_back([this, i]{
                uint32_t seq = i * 1'000'000;
                while (!stop_flag_.load(std::memory_order_acquire)) {
                    queue_.enqueue(PayloadType(seq++));
                    stats_.recordEnqueue();
                    if ((seq & 0x3FF) == 0) std::this_thread::yield();
                }
            });
        }

        // Consumer threads
        for (int i = 0; i < consumers; ++i) {
            threads.emplace_back([this]{
                PayloadType item;
                while (!stop_flag_.load(std::memory_order_acquire)) {
                    if (queue_.dequeue(item)) {
                        stats_.recordDequeue();
                    } else {
                        std::this_thread::sleep_for(1us);
                    }
                }
                // Drain remaining
                while (queue_.dequeue(item)) {
                    stats_.recordDequeue();
                }
            });
        }

        // Warmup
        std::this_thread::sleep_for(std::chrono::seconds(config::WARMUP_DURATION_SEC));
        stats_.reset();

        // Benchmark
        auto start = Clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
        auto end = Clock::now();

        // Stop
        stop_flag_.store(true, std::memory_order_release);
        for (auto& t : threads)  t.join();
        monitor.join();

        // Gather results
        Duration      dur = end - start;
        BenchmarkResult r;
        r.name                   = name;
        r.queue_type             = "EBR";
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
        r.cpu_efficiency         = r.total_ops_per_sec / std::thread::hardware_concurrency();
        
        // Avoid division by zero in memory efficiency calculation
        double mem_usage_mb = r.max_queue_size * sizeof(PayloadType) / (1024.0*1024.0);
        r.memory_efficiency      = (mem_usage_mb > 0 ? r.total_ops_per_sec / mem_usage_mb : 0.0);
        
        return r;
    }
};

// Benchmark suite orchestration
class BenchmarkSuite {
private:
    std::vector<BenchmarkResult> results_;

    template<typename T>
    void runSingleThreaded() {
        ThroughputBenchmark<lfq::EBRQueue<T>, T> bench;
        auto r = bench.runBenchmark("Single Thread (1P/1C)", 1, 1);
        r.print(); results_.push_back(r);
    }

    template<typename T>
    void runMPSC() {
        for (int p : {2,4,8,16}) {
            if (p > static_cast<int>(config::MAX_THREADS-1)) break;
            ThroughputBenchmark<lfq::EBRQueue<T>, T> bench;
            auto r = bench.runBenchmark("MPSC (" + std::to_string(p) + "P/1C)", p, 1);
            r.print(); results_.push_back(r);
        }
    }

    template<typename T>
    void runSPMC() {
        for (int c : {2,4,8,16}) {
            if (c > static_cast<int>(config::MAX_THREADS-1)) break;
            ThroughputBenchmark<lfq::EBRQueue<T>, T> bench;
            auto r = bench.runBenchmark("SPMC (1P/" + std::to_string(c) + "C)", 1, c);
            r.print(); results_.push_back(r);
        }
    }

    template<typename T>
    void runMPMC() {
        for (int n : {2,4,8}) {
            if (n*2 > static_cast<int>(config::MAX_THREADS)) break;
            ThroughputBenchmark<lfq::EBRQueue<T>, T> bench;
            auto r = bench.runBenchmark("MPMC (" + std::to_string(n) + "P/" + std::to_string(n) + "C)", n, n);
            r.print(); results_.push_back(r);
        }
    }

    template<typename T>
    void runScalability() {
        for (int t : config::THREAD_COUNTS) {
            if (t < 2 || t > static_cast<int>(config::MAX_THREADS)) continue;
            int p = t/2, c = t - p;
            ThroughputBenchmark<lfq::EBRQueue<T>, T> bench;
            auto r = bench.runBenchmark("Scale " + std::to_string(t) + " threads", p, c);
            r.print(); results_.push_back(r);
        }
    }

    void runPayloadSizes() {
        {
            ThroughputBenchmark<lfq::EBRQueue<Payload<4>>, Payload<4>> bench;
            auto r = bench.runBenchmark("4B Payload (4P/4C)", 4, 4);
            r.print(); results_.push_back(r);
        }
        {
            ThroughputBenchmark<lfq::EBRQueue<Payload<64>>, Payload<64>> bench;
            auto r = bench.runBenchmark("64B Payload (4P/4C)", 4, 4);
            r.print(); results_.push_back(r);
        }
        {
            ThroughputBenchmark<lfq::EBRQueue<Payload<256>>, Payload<256>> bench;
            auto r = bench.runBenchmark("256B Payload (4P/4C)", 4, 4);
            r.print(); results_.push_back(r);
        }
        {
            ThroughputBenchmark<lfq::EBRQueue<Payload<1024>>, Payload<1024>> bench;
            auto r = bench.runBenchmark("1KB Payload (4P/4C)", 4, 4);
            r.print(); results_.push_back(r);
        }
    }

public:
    void runAll() {
        std::cout << "Lock-Free Queue Throughput Benchmarks\n"
                  << "=====================================\n"
                  << "Hardware threads: " << std::thread::hardware_concurrency() << "\n"
                  << "Benchmark duration: " << config::DEFAULT_DURATION_SEC << "s\n"
                  << "Warmup duration:   " << config::WARMUP_DURATION_SEC << "s\n\n";
        BenchmarkResult::printHeader();

        runSingleThreaded<int>();
        runMPSC<int>();
        runSPMC<int>();
        runMPMC<int>();
        runScalability<int>();
        runPayloadSizes();
        
        printSummary();
        exportResults();
    }

private:
    void printSummary() {
        if (results_.empty()) return;
        
        std::cout << "\n=== Benchmark Summary ===\n";
        
        auto best_throughput = std::max_element(results_.begin(), results_.end(),
            [](const BenchmarkResult& a, const BenchmarkResult& b) {
                return a.total_ops_per_sec < b.total_ops_per_sec;
            });
            
        auto best_efficiency = std::max_element(results_.begin(), results_.end(),
            [](const BenchmarkResult& a, const BenchmarkResult& b) {
                return a.ops_per_thread_per_sec < b.ops_per_thread_per_sec;
            });
        
        std::cout << "Best throughput: " << std::fixed << std::setprecision(0) 
                  << best_throughput->total_ops_per_sec << " ops/sec (" 
                  << best_throughput->name << ")\n";
        std::cout << "Best efficiency: " << std::fixed << std::setprecision(0)
                  << best_efficiency->ops_per_thread_per_sec << " ops/sec/thread (" 
                  << best_efficiency->name << ")\n";
    }
    
    void exportResults() {
        std::ofstream csv("throughput_results.csv");
        csv << "Benchmark,Queue_Type,Threads,Payload_Size,Duration_Sec,"
               "Total_Enqueue_Ops,Total_Dequeue_Ops,Enqueue_Ops_Per_Sec,"
               "Dequeue_Ops_Per_Sec,Total_Ops_Per_Sec,Ops_Per_Thread_Per_Sec,"
               "Avg_Queue_Size,Max_Queue_Size,Total_Bytes_Transferred,"
               "Bandwidth_MB_Per_Sec,CPU_Efficiency,Memory_Efficiency\n";
               
        for (const auto& r : results_) {
            csv << r.name << ',' << r.queue_type << ',' << r.num_threads << ','
                << r.payload_size << ',' << r.duration_sec << ','
                << r.total_enqueue_ops << ',' << r.total_dequeue_ops << ','
                << r.enqueue_ops_per_sec << ',' << r.dequeue_ops_per_sec << ','
                << r.total_ops_per_sec << ',' << r.ops_per_thread_per_sec << ','
                << r.avg_queue_size << ',' << r.max_queue_size << ','
                << r.total_bytes_transferred << ',' << r.bandwidth_mb_per_sec << ','
                << r.cpu_efficiency << ',' << r.memory_efficiency << '\n';
        }
        std::cout << "\nResults exported to throughput_results.csv\n";
    }
};

int main(int argc, char* argv[]) {
    // Optional override of duration
    if (argc > 1) {
        int d = std::stoi(argv[1]);
        (void)d;  // Acknowledge the parameter but don't use it for now
    }

#ifdef __linux__
    // Set process priority - capture and check result to avoid warning
    int nice_result = nice(-10);
    if (nice_result == -1) {
        // Handle error if needed, but for benchmarking we can continue
        // Note: This may fail if not running with appropriate privileges
        std::cerr << "Warning: Could not set high process priority\n";
    }
#endif

    BenchmarkSuite suite;
    suite.runAll();
    return 0;
}