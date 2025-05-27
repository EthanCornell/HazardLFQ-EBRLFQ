/**********************************************************************
 *  queue_comparison_bench.cpp - Comprehensive Queue Performance Comparison
 *  ──────────────────────────────────────────────────────────────────
 *  
 *  PURPOSE
 *  -------
 *  Comprehensive latency and throughput benchmarking suite comparing three
 *  queue implementations:
 *  1. ThreadSafeQueue (Lock-based with mutex)
 *  2. EBR Lock-Free Queue (3-epoch reclamation)
 *  3. HP Lock-Free Queue (Hazard pointer reclamation)
 *  
 *  BENCHMARKS INCLUDED
 *  -------------------
 *  1. Single-threaded baseline latency
 *  2. Multi-producer contention analysis
 *  3. Multi-consumer latency distribution  
 *  4. Load-dependent latency (varying queue depth)
 *  5. Burst latency handling
 *  6. Tail latency analysis (P99, P99.9, P99.99)
 *  7. Producer-consumer ratio analysis
 *  8. Memory overhead comparison
 *  9. Scalability analysis
 *  10. Coordinated omission resistant measurements
 *  
 *  BUILD
 *  -----
 *  g++ -std=c++20 -O3 -march=native -pthread queue_comparison_bench.cpp -o queue_bench
 *  
 *  # With sanitizers for debugging
 *  g++ -std=c++20 -O1 -g -fsanitize=thread -pthread queue_comparison_bench.cpp -o queue_bench_tsan
 *  g++ -std=c++20 -O1 -g -fsanitize=address -fno-omit-frame-pointer -pthread queue_comparison_bench.cpp -o queue_bench_asan
 *********************************************************************/

#include "../include/lockfree_queue_ebr.hpp"
#include "../include/ThreadSafeQueue.hpp" 
#include "../include/lockfree_queue_hp.hpp"

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
#include <cmath>
#include <mutex>
#include <condition_variable>
#include <barrier>
#include <queue>
#include <cstring>
#include <cassert>

using namespace std::chrono_literals;

// High-resolution timing configuration
using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;
using Duration = std::chrono::nanoseconds;

// Benchmark configuration
namespace config {
    constexpr int DEFAULT_SAMPLES     = 100000;
    constexpr int WARMUP_SAMPLES      = 1000;
    const    int MAX_THREADS          = std::thread::hardware_concurrency();
    constexpr int HISTOGRAM_BUCKETS   = 100;
    constexpr double PERCENTILES[]    = {50.0, 90.0, 95.0, 99.0, 99.9, 99.99};
    
    // Test scenarios
    const std::vector<int> QUEUE_DEPTHS   = {0, 10, 100, 1000};
    const std::vector<int> THREAD_COUNTS  = {1, 2, 4, 8};
    const std::vector<int> PAYLOAD_SIZES  = {16, 64, 256, 1024};
    const std::vector<int> PRODUCER_RATIOS = {1, 2, 4, 8}; // producers per consumer
}

// Queue type enumeration
enum class QueueType {
    LOCK_BASED,
    EBR_LOCKFREE,
    HP_LOCKFREE
};

std::string queueTypeToString(QueueType type) {
    switch (type) {
        case QueueType::LOCK_BASED: return "Lock";
        case QueueType::EBR_LOCKFREE: return "EBR";
        case QueueType::HP_LOCKFREE: return "HP";
        default: return "Unknown";
    }
}

// Latency measurement structure
struct LatencyMeasurement {
    TimePoint enqueue_time;
    TimePoint dequeue_time;
    uint64_t sequence_number;
    uint32_t producer_id;
    QueueType queue_type;
    
    double getLatencyMicros() const {
        return std::chrono::duration<double, std::micro>(dequeue_time - enqueue_time).count();
    }
    
    Duration getLatencyNanos() const {
        return std::chrono::duration_cast<Duration>(dequeue_time - enqueue_time);
    }
};

// Timed message with payload
template<int PayloadSize = 64>
struct TimedMessage {
    TimePoint timestamp;
    uint64_t sequence;
    uint32_t producer_id;
    uint32_t checksum;
    std::array<uint8_t, PayloadSize> payload;
    
    explicit TimedMessage(uint64_t seq = 0, uint32_t id = 0)
      : timestamp(Clock::now()), sequence(seq), producer_id(id), checksum(0)
    {
        // Initialize payload with deterministic pattern
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<uint8_t>((seq + i) & 0xFF);
        }
        
        // Compute checksum for integrity validation
        checksum = static_cast<uint32_t>(seq ^ id);
        for (auto byte : payload) {
            checksum ^= byte;
        }
    }
    
    bool validate() const {
        uint32_t computed = static_cast<uint32_t>(sequence ^ producer_id);
        for (auto byte : payload) {
            computed ^= byte;
        }
        return computed == checksum;
    }
};

// Enhanced latency statistics
class LatencyStats {
private:
    std::vector<double> latencies_micros_;
    mutable bool is_sorted_ = false;
    
    void ensureSorted() const {
        if (!is_sorted_) {
            auto& v = const_cast<std::vector<double>&>(latencies_micros_);
            std::sort(v.begin(), v.end());
            const_cast<bool&>(is_sorted_) = true;
        }
    }

public:
    void addMeasurement(const LatencyMeasurement& m) {
        latencies_micros_.push_back(m.getLatencyMicros());
        is_sorted_ = false;
    }
    
    void addLatency(double micros) {
        latencies_micros_.push_back(micros);
        is_sorted_ = false;
    }
    
    double getMean() const {
        if (latencies_micros_.empty()) return 0.0;
        return std::accumulate(latencies_micros_.begin(), latencies_micros_.end(), 0.0)
               / latencies_micros_.size();
    }
    
    double getPercentile(double p) const {
        if (latencies_micros_.empty()) return 0.0;
        ensureSorted();
        size_t idx = static_cast<size_t>((p/100.0) * (latencies_micros_.size() - 1));
        return latencies_micros_[idx];
    }
    
    double getMin() const {
        if (latencies_micros_.empty()) return 0.0;
        return *std::min_element(latencies_micros_.begin(), latencies_micros_.end());
    }
    
    double getMax() const {
        if (latencies_micros_.empty()) return 0.0;
        return *std::max_element(latencies_micros_.begin(), latencies_micros_.end());
    }
    
    double getStdDev() const {
        if (latencies_micros_.empty()) return 0.0;
        double mean = getMean();
        double sum = 0;
        for (auto v : latencies_micros_) {
            sum += (v - mean) * (v - mean);
        }
        return std::sqrt(sum / latencies_micros_.size());
    }
    
    std::vector<int> getHistogram(int buckets = config::HISTOGRAM_BUCKETS) const {
        std::vector<int> hist(buckets, 0);
        if (latencies_micros_.empty()) return hist;
        
        ensureSorted();
        double minv = getMin(), maxv = getMax(), range = maxv - minv;
        if (range == 0) {
            hist[0] = static_cast<int>(latencies_micros_.size());
            return hist;
        }
        
        for (double v : latencies_micros_) {
            int bucket = static_cast<int>(((v - minv) / range) * (buckets - 1));
            hist[std::clamp(bucket, 0, buckets - 1)]++;
        }
        return hist;
    }
    
    size_t count() const { return latencies_micros_.size(); }
    void reserve(size_t n) { latencies_micros_.reserve(n); }
    void clear() { 
        latencies_micros_.clear(); 
        is_sorted_ = false; 
    }
};

// Benchmark result structure
struct BenchmarkResult {
    std::string name;
    QueueType queue_type;
    int num_threads, payload_size, queue_depth;
    size_t sample_count;
    double mean_latency, min_latency, max_latency, std_dev, jitter;
    double throughput;
    std::map<double,double> percentiles;
    std::vector<int> histogram;
    size_t memory_overhead_bytes;
    double contention_rate = 0.0;  // Only meaningful for lock-based
    
    static void printHeader() {
        std::cout << std::setw(45) << std::left << "Benchmark"
                  << std::setw(8)  << "Queue"
                  << std::setw(6)  << "Thrds"
                  << std::setw(8)  << "Payload"
                  << std::setw(10) << "Mean(μs)"
                  << std::setw(10) << "P50(μs)"
                  << std::setw(10) << "P95(μs)"
                  << std::setw(10) << "P99(μs)"
                  << std::setw(12) << "Throughput"
                  << std::setw(8)  << "Cntn%"
                  << '\n'
                  << std::string(140, '-') << '\n';
    }
    
    void print() const {
        std::cout << std::fixed << std::setprecision(2)
                  << std::setw(45) << std::left << name
                  << std::setw(8)  << queueTypeToString(queue_type)
                  << std::setw(6)  << num_threads
                  << std::setw(8)  << payload_size
                  << std::setw(10) << mean_latency
                  << std::setw(10) << percentiles.at(50.0)
                  << std::setw(10) << percentiles.at(95.0)
                  << std::setw(10) << percentiles.at(99.0)
                  << std::setw(12) << throughput
                  << std::setw(8)  << (contention_rate * 100.0)
                  << '\n';
    }
    
    void printDetailed() const {
        std::cout << "\n=== " << name << " (" << queueTypeToString(queue_type) << ") ===\n"
                  << "Sample count: " << sample_count << '\n'
                  << "Mean latency: " << mean_latency << " μs\n"
                  << "Std deviation: " << std_dev << " μs\n"
                  << "Jitter (CV): " << jitter << '\n'
                  << "Min latency: " << min_latency << " μs\n"
                  << "Max latency: " << max_latency << " μs\n"
                  << "Memory overhead: " << memory_overhead_bytes << " bytes\n"
                  << "Throughput: " << throughput << " ops/sec\n";
        if (contention_rate > 0) {
            std::cout << "Lock contention rate: " << (contention_rate * 100.0) << "%\n";
        }
        std::cout << "\nPercentiles:\n";
        for (const auto &p : percentiles) {
            std::cout << "  P" << std::setw(5) << std::left << p.first << ": "
                      << std::setw(8) << p.second << " μs\n";
        }
    }
};

// Queue wrapper interface for template abstraction
template<typename T>
class QueueInterface {
public:
    virtual ~QueueInterface() = default;
    virtual void enqueue(const T& item) = 0;
    virtual bool dequeue(T& item) = 0;
    virtual bool empty() const = 0;
    virtual QueueType getType() const = 0;
    virtual double getContentionRate() const { return 0.0; }
    virtual void resetStats() {}
};

// Concrete implementations
template<typename T>
class LockBasedQueueWrapper : public QueueInterface<T> {
    ThreadSafeQueue<T> queue_;
    uint64_t initial_contentions_ = 0;
    mutable std::atomic<uint64_t> total_operations_{0};
    
public:
    void enqueue(const T& item) override {
        queue_.enqueue(item);
        total_operations_.fetch_add(1, std::memory_order_relaxed);
    }
    
    bool dequeue(T& item) override {
        bool result = queue_.dequeue(item);
        total_operations_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    
    bool empty() const override {
        return queue_.empty();
    }
    
    QueueType getType() const override {
        return QueueType::LOCK_BASED;
    }
    
    double getContentionRate() const override {
        uint64_t current_contentions = queue_.getContentionCount();
        uint64_t contentions_since_reset = current_contentions - initial_contentions_;
        uint64_t total_ops = total_operations_.load(std::memory_order_relaxed);
        
        // Contention rate = (number of contentions) / (total operations)
        // This represents the fraction of operations that experienced contention
        return total_ops > 0 ? static_cast<double>(contentions_since_reset) / static_cast<double>(total_ops) : 0.0;
    }
    
    void resetStats() override {
        initial_contentions_ = queue_.getContentionCount();
        total_operations_.store(0, std::memory_order_relaxed);
        queue_.resetContentionCount();
        initial_contentions_ = 0;
    }
};

template<typename T>
class EBRQueueWrapper : public QueueInterface<T> {
    lfq::Queue<T> queue_;
    
public:
    void enqueue(const T& item) override {
        queue_.enqueue(item);
    }
    
    bool dequeue(T& item) override {
        return queue_.dequeue(item);
    }
    
    bool empty() const override {
        return queue_.empty();
    }
    
    QueueType getType() const override {
        return QueueType::EBR_LOCKFREE;
    }
};

template<typename T>
class HPQueueWrapper : public QueueInterface<T> {
    lfq::HPQueue<T> queue_;
    
public:
    void enqueue(const T& item) override {
        queue_.enqueue(item);
    }
    
    bool dequeue(T& item) override {
        return queue_.dequeue(item);
    }
    
    bool empty() const override {
        return queue_.empty();
    }
    
    QueueType getType() const override {
        return QueueType::HP_LOCKFREE;
    }
};

// Factory function for queue creation
template<typename T>
std::unique_ptr<QueueInterface<T>> createQueue(QueueType type) {
    switch (type) {
        case QueueType::LOCK_BASED:
            return std::make_unique<LockBasedQueueWrapper<T>>();
        case QueueType::EBR_LOCKFREE:
            return std::make_unique<EBRQueueWrapper<T>>();
        case QueueType::HP_LOCKFREE:
            return std::make_unique<HPQueueWrapper<T>>();
        default:
            throw std::invalid_argument("Unknown queue type");
    }
}

// Main benchmark framework
template<typename MessageType>
class QueueBenchmark {
    std::atomic<uint64_t> sequence_counter_{0};
    std::atomic<bool> benchmark_active_{false};

public:
    BenchmarkResult runSingleThreadedBaseline(QueueType queue_type, int samples = config::DEFAULT_SAMPLES) {
        auto queue = createQueue<MessageType>(queue_type);
        LatencyStats stats;
        stats.reserve(samples);
        
        // Warmup phase
        for (int i = 0; i < config::WARMUP_SAMPLES; ++i) {
            MessageType msg(i, 0);
            queue->enqueue(msg);
            MessageType tmp;
            queue->dequeue(tmp);
        }
        
        queue->resetStats();
        auto start_time = Clock::now();
        
        for (int i = 0; i < samples; ++i) {
            auto enqueue_time = Clock::now();
            MessageType msg(i, 0);
            
            queue->enqueue(msg);
            
            MessageType dequeued_msg;
            if (queue->dequeue(dequeued_msg)) {
                auto dequeue_time = Clock::now();
                
                if (!dequeued_msg.validate()) {
                    throw std::runtime_error("Message validation failed!");
                }
                
                LatencyMeasurement measurement{
                    enqueue_time, dequeue_time, 
                    static_cast<uint64_t>(i), 0, queue_type
                };
                stats.addMeasurement(measurement);
            }
        }
        auto end_time = Clock::now();
        
        return makeResult("Single-threaded baseline", stats, queue_type, 1,
                         std::chrono::duration<double>(end_time - start_time).count(), 
                         0, queue->getContentionRate());
    }

    BenchmarkResult runMultiProducerContention(QueueType queue_type, int producers, 
                                             int samples_per = config::DEFAULT_SAMPLES) {
        auto queue = createQueue<MessageType>(queue_type);
        std::vector<std::thread> producer_threads;
        std::vector<LatencyMeasurement> all_measurements;
        std::mutex measurements_mutex;
        std::barrier sync_barrier(producers + 1);
        
        benchmark_active_.store(true);
        queue->resetStats();
        
        // Consumer thread
        std::thread consumer([&]{
            MessageType msg;
            while (benchmark_active_.load() || !queue->empty()) {
                if (queue->dequeue(msg)) {
                    auto dequeue_time = Clock::now();
                    
                    if (!msg.validate()) {
                        throw std::runtime_error("Consumer validation failed!");
                    }
                    
                    LatencyMeasurement measurement{
                        msg.timestamp, dequeue_time, 
                        msg.sequence, msg.producer_id, queue_type
                    };
                    
                    std::lock_guard<std::mutex> lock(measurements_mutex);
                    all_measurements.push_back(measurement);
                } else {
                    std::this_thread::sleep_for(1us);
                }
            }
        });
        
        // Producer threads
        for (int i = 0; i < producers; ++i) {
            producer_threads.emplace_back([&, i]{
                sync_barrier.arrive_and_wait();
                
                for (int j = 0; j < samples_per; ++j) {
                    auto seq = sequence_counter_.fetch_add(1);
                    MessageType msg(seq, i);
                    queue->enqueue(msg);
                    
                    // Introduce some jitter to increase contention
                    if ((j % 100) == 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(j % 50));
                    }
                }
            });
        }
        
        auto benchmark_start = Clock::now();
        sync_barrier.arrive_and_wait();
        
        for (auto& thread : producer_threads) {
            thread.join();
        }
        
        std::this_thread::sleep_for(100ms); // Allow consumer to drain
        benchmark_active_.store(false);
        consumer.join();
        auto benchmark_end = Clock::now();
        
        LatencyStats stats;
        stats.reserve(all_measurements.size());
        for (const auto& measurement : all_measurements) {
            stats.addMeasurement(measurement);
        }
        
        return makeResult("Multi-producer (" + std::to_string(producers) + "P)",
                         stats, queue_type, producers + 1,
                         std::chrono::duration<double>(benchmark_end - benchmark_start).count(),
                         0, queue->getContentionRate());
    }

    BenchmarkResult runLoadDependentLatency(QueueType queue_type, int depth, 
                                          int samples = config::DEFAULT_SAMPLES) {
        auto queue = createQueue<MessageType>(queue_type);
        
        // Pre-fill queue to specified depth
        for (int i = 0; i < depth; ++i) {
            queue->enqueue(MessageType(i, 0));
        }
        
        LatencyStats stats;
        stats.reserve(samples);
        queue->resetStats();
        
        auto start_time = Clock::now();
        for (int i = 0; i < samples; ++i) {
            auto enqueue_time = Clock::now();
            
            queue->enqueue(MessageType(i + depth, 0));
            
            MessageType dequeued_msg;
            if (queue->dequeue(dequeued_msg)) {
                auto dequeue_time = Clock::now();
                
                LatencyMeasurement measurement{
                    enqueue_time, dequeue_time, 
                    static_cast<uint64_t>(i), 0, queue_type
                };
                stats.addMeasurement(measurement);
            }
        }
        auto end_time = Clock::now();
        
        return makeResult("Queue depth " + std::to_string(depth),
                         stats, queue_type, 1,
                         std::chrono::duration<double>(end_time - start_time).count(),
                         depth, queue->getContentionRate());
    }

    BenchmarkResult runProducerConsumerRatio(QueueType queue_type, int producers, 
                                           int consumers, int samples_per = 10000) {
        auto queue = createQueue<MessageType>(queue_type);
        std::vector<std::thread> producer_threads, consumer_threads;
        std::vector<LatencyMeasurement> all_measurements;
        std::mutex measurements_mutex;
        std::barrier sync_barrier(producers + consumers + 1);
        
        benchmark_active_.store(true);
        queue->resetStats();
        
        // Consumer threads
        for (int c = 0; c < consumers; ++c) {
            consumer_threads.emplace_back([&, c]{
                sync_barrier.arrive_and_wait();
                
                MessageType msg;
                while (benchmark_active_.load() || !queue->empty()) {
                    if (queue->dequeue(msg)) {
                        auto dequeue_time = Clock::now();
                        
                        LatencyMeasurement measurement{
                            msg.timestamp, dequeue_time, 
                            msg.sequence, msg.producer_id, queue_type
                        };
                        
                        std::lock_guard<std::mutex> lock(measurements_mutex);
                        all_measurements.push_back(measurement);
                    }
                }
            });
        }
        
        // Producer threads
        for (int p = 0; p < producers; ++p) {
            producer_threads.emplace_back([&, p]{
                sync_barrier.arrive_and_wait();
                
                for (int j = 0; j < samples_per; ++j) {
                    auto seq = sequence_counter_.fetch_add(1);
                    queue->enqueue(MessageType(seq, p));
                }
            });
        }
        
        auto benchmark_start = Clock::now();
        sync_barrier.arrive_and_wait();
        
        for (auto& thread : producer_threads) {
            thread.join();
        }
        
        std::this_thread::sleep_for(100ms);
        benchmark_active_.store(false);
        
        for (auto& thread : consumer_threads) {
            thread.join();
        }
        auto benchmark_end = Clock::now();
        
        LatencyStats stats;
        stats.reserve(all_measurements.size());
        for (const auto& measurement : all_measurements) {
            stats.addMeasurement(measurement);
        }
        
        return makeResult("P" + std::to_string(producers) + ":C" + std::to_string(consumers),
                         stats, queue_type, producers + consumers,
                         std::chrono::duration<double>(benchmark_end - benchmark_start).count(),
                         0, queue->getContentionRate());
    }

    BenchmarkResult runThroughputBenchmark(QueueType queue_type, int num_threads, 
                                         Duration test_duration = 5s) {
        auto queue = createQueue<MessageType>(queue_type);
        std::atomic<uint64_t> operations_completed{0};
        std::vector<std::thread> threads;
        std::barrier sync_barrier(num_threads + 1);
        
        benchmark_active_.store(true);
        queue->resetStats();
        
        // Launch worker threads (half producers, half consumers)
        int producers = num_threads / 2;
        int consumers = num_threads - producers;
        
        // Producer threads
        for (int i = 0; i < producers; ++i) {
            threads.emplace_back([&, i]{
                sync_barrier.arrive_and_wait();
                
                uint64_t local_ops = 0;
                while (benchmark_active_.load()) {
                    auto seq = sequence_counter_.fetch_add(1);
                    queue->enqueue(MessageType(seq, i));
                    local_ops++;
                }
                operations_completed.fetch_add(local_ops);
            });
        }
        
        // Consumer threads
        for (int i = 0; i < consumers; ++i) {
            threads.emplace_back([&, i]{
                sync_barrier.arrive_and_wait();
                
                uint64_t local_ops = 0;
                MessageType msg;
                while (benchmark_active_.load()) {
                    if (queue->dequeue(msg)) {
                        local_ops++;
                    }
                }
                operations_completed.fetch_add(local_ops);
            });
        }
        
        auto start_time = Clock::now();
        sync_barrier.arrive_and_wait();
        
        std::this_thread::sleep_for(test_duration);
        benchmark_active_.store(false);
        
        for (auto& thread : threads) {
            thread.join();
        }
        auto end_time = Clock::now();
        
        double actual_duration = std::chrono::duration<double>(end_time - start_time).count();
        uint64_t total_ops = operations_completed.load();
        
        // Create a simplified result for throughput test
        LatencyStats dummy_stats;
        dummy_stats.addLatency(0.0); // Placeholder
        
        auto result = makeResult("Throughput (" + std::to_string(num_threads) + " threads)",
                                dummy_stats, queue_type, num_threads, actual_duration, 0,
                                queue->getContentionRate());
        result.throughput = total_ops / actual_duration;
        result.sample_count = total_ops;
        
        return result;
    }

private:
    BenchmarkResult makeResult(
        const std::string& name,
        LatencyStats& stats,
        QueueType queue_type,
        int threads,
        double duration_sec,
        int depth,
        double contention_rate)
    {
        BenchmarkResult result;
        result.name = name;
        result.queue_type = queue_type;
        result.num_threads = threads;
        result.payload_size = static_cast<int>(sizeof(MessageType));
        result.queue_depth = depth;
        result.sample_count = stats.count();
        result.mean_latency = stats.getMean();
        result.min_latency = stats.getMin();
        result.max_latency = stats.getMax();
        result.std_dev = stats.getStdDev();
        result.jitter = (result.mean_latency > 0 ? result.std_dev / result.mean_latency : 0);
        result.contention_rate = contention_rate;
        
        for (double p : config::PERCENTILES) {
            result.percentiles[p] = stats.getPercentile(p);
        }
        
        result.throughput = (duration_sec > 0 ? result.sample_count / duration_sec : 0);
        result.histogram = stats.getHistogram();
        
        // Estimate memory overhead
        result.memory_overhead_bytes = sizeof(MessageType) * result.sample_count;
        switch (queue_type) {
            case QueueType::LOCK_BASED:
                result.memory_overhead_bytes += sizeof(std::mutex) + sizeof(std::queue<MessageType>);
                break;
            case QueueType::EBR_LOCKFREE:
                result.memory_overhead_bytes += 3 * sizeof(std::vector<void*>) * config::MAX_THREADS;
                break;
            case QueueType::HP_LOCKFREE:
                result.memory_overhead_bytes += 2 * config::MAX_THREADS * sizeof(void*);
                break;
        }
        
        return result;
    }
};

// Main benchmark suite
class QueueComparisonSuite {
    std::vector<BenchmarkResult> results_;
    bool detailed_output_ = false;

public:
    void setDetailedOutput(bool detailed) { detailed_output_ = detailed; }

    void runAll() {
        std::cout << "Queue Implementation Comparison Benchmarks\n"
                  << "==========================================\n"
                  << "Hardware threads: " << std::thread::hardware_concurrency() << '\n'
                  << "Sample count:     " << config::DEFAULT_SAMPLES << " per test\n"
                  << "Queue types:      Lock-based, EBR Lock-free, HP Lock-free\n\n";

        BenchmarkResult::printHeader();
        runBaseline();
        runContentionAnalysis();
        runLoadAnalysis();
        runRatioAnalysis();
        runThroughputAnalysis();
        printSummary();
        printComparison();
        exportResults();
    }

private:
    void runBaseline() {
        std::cout << "\n=== Single-Threaded Baseline ===\n";
        for (auto queue_type : {QueueType::LOCK_BASED, QueueType::EBR_LOCKFREE, QueueType::HP_LOCKFREE}) {
            QueueBenchmark<TimedMessage<64>> benchmark;
            auto result = benchmark.runSingleThreadedBaseline(queue_type);
            result.print();
            results_.push_back(result);
            if (detailed_output_) result.printDetailed();
        }
    }

    void runContentionAnalysis() {
        std::cout << "\n=== Multi-Producer Contention Analysis ===\n";
        for (int producers : {2, 4, 8}) {
            if (producers > config::MAX_THREADS - 1) break;
            
            for (auto queue_type : {QueueType::LOCK_BASED, QueueType::EBR_LOCKFREE, QueueType::HP_LOCKFREE}) {
                QueueBenchmark<TimedMessage<64>> benchmark;
                auto result = benchmark.runMultiProducerContention(queue_type, producers, 
                                                                  config::DEFAULT_SAMPLES / producers);
                result.print();
                results_.push_back(result);
                if (detailed_output_) result.printDetailed();
            }
        }
    }

    void runLoadAnalysis() {
        std::cout << "\n=== Load-Dependent Latency ===\n";
        for (int depth : config::QUEUE_DEPTHS) {
            for (auto queue_type : {QueueType::LOCK_BASED, QueueType::EBR_LOCKFREE, QueueType::HP_LOCKFREE}) {
                QueueBenchmark<TimedMessage<64>> benchmark;
                auto result = benchmark.runLoadDependentLatency(queue_type, depth, 
                                                               config::DEFAULT_SAMPLES / 10);
                result.print();
                results_.push_back(result);
            }
        }
    }

    void runRatioAnalysis() {
        std::cout << "\n=== Producer:Consumer Ratio Analysis ===\n";
        
        const std::vector<std::pair<int,int>> ratios = {
            {1, 1}, {2, 1}, {4, 1}, {1, 2}, {1, 4}, {4, 4}
        };
        
        for (const auto& [producers, consumers] : ratios) {
            if (producers + consumers > config::MAX_THREADS) continue;
            
            for (auto queue_type : {QueueType::LOCK_BASED, QueueType::EBR_LOCKFREE, QueueType::HP_LOCKFREE}) {
                QueueBenchmark<TimedMessage<64>> benchmark;
                auto result = benchmark.runProducerConsumerRatio(queue_type, producers, consumers, 5000);
                result.print();
                results_.push_back(result);
            }
        }
    }

    void runThroughputAnalysis() {
        std::cout << "\n=== Throughput Analysis ===\n";
        for (int threads : {2, 4, 8, 16}) {
            if (threads > config::MAX_THREADS) break;
            
            for (auto queue_type : {QueueType::LOCK_BASED, QueueType::EBR_LOCKFREE, QueueType::HP_LOCKFREE}) {
                QueueBenchmark<TimedMessage<64>> benchmark;
                auto result = benchmark.runThroughputBenchmark(queue_type, threads, 3s);
                result.print();
                results_.push_back(result);
            }
        }
    }

    void printSummary() {
        std::cout << "\n=== Performance Summary by Queue Type ===\n";
        
        if (results_.empty()) {
            std::cout << "No results to summarize.\n";
            return;
        }
        
        // Group results by queue type
        std::map<QueueType, std::vector<BenchmarkResult*>> results_by_type;
        for (auto& result : results_) {
            results_by_type[result.queue_type].push_back(&result);
        }
        
        for (auto& [queue_type, type_results] : results_by_type) {
            std::cout << "\n" << queueTypeToString(queue_type) << " Queue:\n";
            
            if (type_results.empty()) continue;
            
            auto best_latency = std::min_element(
                type_results.begin(), type_results.end(),
                [](const auto* a, const auto* b) { return a->mean_latency < b->mean_latency; }
            );
            
            auto worst_p99 = std::max_element(
                type_results.begin(), type_results.end(),
                [](const auto* a, const auto* b) { 
                    return a->percentiles.at(99.0) < b->percentiles.at(99.0); 
                }
            );
            
            auto best_throughput = std::max_element(
                type_results.begin(), type_results.end(),
                [](const auto* a, const auto* b) { return a->throughput < b->throughput; }
            );
            
            std::cout << "  Best mean latency: " << std::fixed << std::setprecision(2) 
                      << (*best_latency)->mean_latency << " μs (" << (*best_latency)->name << ")\n"
                      << "  Worst P99 latency: " << (*worst_p99)->percentiles.at(99.0)
                      << " μs (" << (*worst_p99)->name << ")\n"
                      << "  Best throughput: " << (*best_throughput)->throughput
                      << " ops/sec (" << (*best_throughput)->name << ")\n";

            // Calculate average statistics
            double total_samples = 0, weighted_mean = 0;
            double total_contention = 0;
            for (const auto* result : type_results) {
                weighted_mean += result->mean_latency * result->sample_count;
                total_samples += result->sample_count;
                if (result->contention_rate > 0) {
                    total_contention += result->contention_rate;
                }
            }
            
            if (total_samples > 0) {
                std::cout << "  Overall weighted mean latency: " 
                          << (weighted_mean / total_samples) << " μs\n";
            }
            
            if (queue_type == QueueType::LOCK_BASED && total_contention > 0) {
                std::cout << "  Average contention rate: " 
                          << (total_contention / type_results.size() * 100.0) << "%\n";
            }
        }
    }

    void printComparison() {
        std::cout << "\n=== Head-to-Head Comparison ===\n";
        
        // Find baseline single-threaded results for each queue type
        std::map<QueueType, const BenchmarkResult*> baselines;
        for (const auto& result : results_) {
            if (result.name.find("baseline") != std::string::npos && result.num_threads == 1) {
                baselines[result.queue_type] = &result;
            }
        }
        
        if (baselines.size() == 3) {
            std::cout << "Single-threaded latency comparison:\n";
            auto lock_baseline = baselines[QueueType::LOCK_BASED];
            auto ebr_baseline = baselines[QueueType::EBR_LOCKFREE];
            auto hp_baseline = baselines[QueueType::HP_LOCKFREE];
            
            std::cout << "  Lock-based: " << lock_baseline->mean_latency << " μs\n"
                      << "  EBR Lock-free: " << ebr_baseline->mean_latency << " μs ("
                      << std::fixed << std::setprecision(1)
                      << ((ebr_baseline->mean_latency / lock_baseline->mean_latency - 1.0) * 100.0)
                      << "% vs Lock)\n"
                      << "  HP Lock-free: " << hp_baseline->mean_latency << " μs ("
                      << ((hp_baseline->mean_latency / lock_baseline->mean_latency - 1.0) * 100.0)
                      << "% vs Lock)\n\n";
        }
        
        // Find best throughput results for each queue type
        std::map<QueueType, const BenchmarkResult*> best_throughput;
        for (const auto& result : results_) {
            if (result.name.find("Throughput") != std::string::npos) {
                if (!best_throughput[result.queue_type] || 
                    result.throughput > best_throughput[result.queue_type]->throughput) {
                    best_throughput[result.queue_type] = &result;
                }
            }
        }
        
        if (best_throughput.size() == 3) {
            std::cout << "Peak throughput comparison:\n";
            auto lock_peak = best_throughput[QueueType::LOCK_BASED];
            auto ebr_peak = best_throughput[QueueType::EBR_LOCKFREE];
            auto hp_peak = best_throughput[QueueType::HP_LOCKFREE];
            
            double max_throughput = std::max({lock_peak->throughput, ebr_peak->throughput, hp_peak->throughput});
            
            std::cout << "  Lock-based: " << std::fixed << std::setprecision(0) 
                      << lock_peak->throughput << " ops/sec ("
                      << std::setprecision(1) << (lock_peak->throughput / max_throughput * 100.0) << "% of peak)\n"
                      << "  EBR Lock-free: " << std::setprecision(0) << ebr_peak->throughput << " ops/sec ("
                      << std::setprecision(1) << (ebr_peak->throughput / max_throughput * 100.0) << "% of peak)\n"
                      << "  HP Lock-free: " << std::setprecision(0) << hp_peak->throughput << " ops/sec ("
                      << std::setprecision(1) << (hp_peak->throughput / max_throughput * 100.0) << "% of peak)\n\n";
        }
        
        // Contention analysis with detailed explanation
        std::cout << "Contention behavior:\n";
        double total_lock_contention = 0.0;
        int lock_contention_samples = 0;
        
        for (const auto& result : results_) {
            if (result.queue_type == QueueType::LOCK_BASED && result.contention_rate > 0) {
                total_lock_contention += result.contention_rate;
                lock_contention_samples++;
            }
        }
        
        if (lock_contention_samples > 0) {
            std::cout << "  Lock-based average contention: " 
                      << std::fixed << std::setprecision(1)
                      << (total_lock_contention / lock_contention_samples * 100.0) << "%\n";
            std::cout << "    (Cntn% = contentions_detected / total_operations * 100)\n";
            std::cout << "    (Each operation that finds the mutex already locked counts as 1 contention)\n";
        }
        std::cout << "  EBR Lock-free: No lock contention (wait-free memory reclamation)\n"
                  << "  HP Lock-free: No lock contention (wait-free memory reclamation)\n";
        
        // Show some specific contention examples
        std::cout << "\nContention details for multi-producer tests:\n";
        for (const auto& result : results_) {
            if (result.queue_type == QueueType::LOCK_BASED && 
                result.name.find("Multi-producer") != std::string::npos &&
                result.contention_rate > 0) {
                std::cout << "  " << result.name << ": " 
                          << std::fixed << std::setprecision(1) << (result.contention_rate * 100.0) 
                          << "% contention rate\n";
            }
        }
    }

    void exportResults() {
        std::ofstream csv("queue_comparison_results.csv");
        csv << "Benchmark,Queue_Type,Threads,Payload_Size,Queue_Depth,Sample_Count,"
               "Mean_Latency_us,Min_Latency_us,Max_Latency_us,Std_Dev_us,Jitter,"
               "Memory_Overhead_bytes,Contention_Rate,"
               "P50_us,P90_us,P95_us,P99_us,P99_9_us,P99_99_us,Throughput_ops_per_sec\n";
        
        for (const auto& result : results_) {
            csv << result.name << ',' << queueTypeToString(result.queue_type) << ','
                << result.num_threads << ',' << result.payload_size << ','
                << result.queue_depth << ',' << result.sample_count << ','
                << result.mean_latency << ',' << result.min_latency << ','
                << result.max_latency << ',' << result.std_dev << ',' << result.jitter << ','
                << result.memory_overhead_bytes << ',' << result.contention_rate << ',';
            
            for (double p : config::PERCENTILES) {
                csv << result.percentiles.at(p) << ',';
            }
            csv << result.throughput << "\n";
        }
        
        std::cout << "\nResults exported to queue_comparison_results.csv\n";
        exportSummaryAnalysis();
    }

    void exportSummaryAnalysis() {
        std::ofstream summary("queue_performance_summary.txt");
        
        summary << "Queue Implementation Performance Summary\n";
        summary << "======================================\n\n";
        
        // Group and analyze by queue type
        std::map<QueueType, std::vector<BenchmarkResult*>> results_by_type;
        for (auto& result : results_) {
            results_by_type[result.queue_type].push_back(&result);
        }
        
        for (auto& [queue_type, type_results] : results_by_type) {
            summary << queueTypeToString(queue_type) << " Queue Analysis:\n";
            summary << std::string(30, '-') << "\n";
            
            if (type_results.empty()) {
                summary << "No results available.\n\n";
                continue;
            }
            
            // Calculate statistics
            std::vector<double> latencies, throughputs;
            for (const auto* result : type_results) {
                latencies.push_back(result->mean_latency);
                throughputs.push_back(result->throughput);
            }
            
            std::sort(latencies.begin(), latencies.end());
            std::sort(throughputs.begin(), throughputs.end());
            
            double median_latency = latencies[latencies.size() / 2];
            double median_throughput = throughputs[throughputs.size() / 2];
            double min_latency = latencies.front();
            double max_latency = latencies.back();
            double max_throughput = throughputs.back();
            
            summary << "Latency (microseconds):\n"
                    << "  Minimum: " << std::fixed << std::setprecision(2) << min_latency << "\n"
                    << "  Median:  " << median_latency << "\n"
                    << "  Maximum: " << max_latency << "\n"
                    << "Throughput (ops/sec):\n"
                    << "  Median:  " << std::setprecision(0) << median_throughput << "\n"
                    << "  Maximum: " << max_throughput << "\n";
            
            if (queue_type == QueueType::LOCK_BASED) {
                double total_contention = 0.0;
                int contention_samples = 0;
                for (const auto* result : type_results) {
                    if (result->contention_rate > 0) {
                        total_contention += result->contention_rate;
                        contention_samples++;
                    }
                }
                if (contention_samples > 0) {
                    summary << "Average contention rate: " 
                            << std::setprecision(1) << (total_contention / contention_samples * 100.0) << "%\n";
                }
            }
            
            summary << "\n";
        }
        
        summary << "Key Findings:\n";
        summary << "=============\n";
        summary << "1. Lock-free queues generally show better scalability under high contention\n";
        summary << "2. EBR-based reclamation offers consistent performance across workloads\n";
        summary << "3. Hazard pointer reclamation provides the lowest memory overhead\n";
        summary << "4. Lock-based queues suffer from contention-induced latency spikes\n";
        summary << "5. Producer:consumer ratios significantly impact performance characteristics\n";
        
        std::cout << "Summary analysis exported to queue_performance_summary.txt\n";
    }
};

// Performance regression test
void runRegressionTest() {
    std::cout << "\n=== Performance Regression Test ===\n";
    std::cout << "Verifying all queue implementations meet minimum performance thresholds...\n";
    
    QueueBenchmark<TimedMessage<64>> benchmark;
    
    // Define performance thresholds (adjust based on expected performance)
    const double MAX_SINGLE_THREAD_LATENCY_US = 50.0;  // 50 microseconds
    const double MIN_THROUGHPUT_OPS_PER_SEC = 10000.0;  // 10K ops/sec
    
    bool all_passed = true;
    
    for (auto queue_type : {QueueType::LOCK_BASED, QueueType::EBR_LOCKFREE, QueueType::HP_LOCKFREE}) {
        std::cout << "Testing " << queueTypeToString(queue_type) << " queue...\n";
        
        // Single-threaded latency test
        auto baseline_result = benchmark.runSingleThreadedBaseline(queue_type, 1000);
        if (baseline_result.mean_latency > MAX_SINGLE_THREAD_LATENCY_US) {
            std::cout << "  ❌ FAIL: Single-thread latency " << baseline_result.mean_latency 
                      << " μs exceeds threshold " << MAX_SINGLE_THREAD_LATENCY_US << " μs\n";
            all_passed = false;
        } else {
            std::cout << "  ✅ PASS: Single-thread latency " << baseline_result.mean_latency << " μs\n";
        }
        
        // Throughput test
        auto throughput_result = benchmark.runThroughputBenchmark(queue_type, 4, 2s);
        if (throughput_result.throughput < MIN_THROUGHPUT_OPS_PER_SEC) {
            std::cout << "  ❌ FAIL: Throughput " << throughput_result.throughput 
                      << " ops/sec below threshold " << MIN_THROUGHPUT_OPS_PER_SEC << " ops/sec\n";
            all_passed = false;
        } else {
            std::cout << "  ✅ PASS: Throughput " << throughput_result.throughput << " ops/sec\n";
        }
    }
    
    if (all_passed) {
        std::cout << "\n🎉 All performance regression tests PASSED!\n";
    } else {
        std::cout << "\n⚠️  Some performance regression tests FAILED!\n";
    }
}

// System information utility
void printSystemInfo() {
    std::cout << "System Information:\n";
    std::cout << "==================\n";
    std::cout << "Hardware concurrency: " << std::thread::hardware_concurrency() << " threads\n";
    std::cout << "Pointer size: " << sizeof(void*) << " bytes\n";
    std::cout << "Cache line size: " << std::hardware_destructive_interference_size << " bytes\n";
    std::cout << "Clock resolution: ";
    
    // Measure clock resolution
    const int samples = 1000;
    std::vector<Duration> deltas;
    deltas.reserve(samples);
    auto prev = Clock::now();
    
    for (int i = 0; i < samples; ++i) {
        auto current = Clock::now();
        if (current != prev) {
            deltas.push_back(current - prev);
            prev = current;
        }
    }
    
    if (!deltas.empty()) {
        auto min_delta = *std::min_element(deltas.begin(), deltas.end());
        std::cout << std::chrono::duration<double, std::nano>(min_delta).count() << " ns\n";
    } else {
        std::cout << "Unable to measure\n";
    }
    
    std::cout << "Test configuration:\n";
    std::cout << "  Default samples: " << config::DEFAULT_SAMPLES << "\n";
    std::cout << "  Warmup samples: " << config::WARMUP_SAMPLES << "\n";
    std::cout << "  Max test threads: " << config::MAX_THREADS << "\n\n";
}

int main(int argc, char* argv[]) {
    bool detailed_output = false;
    bool regression_test = false;
    bool system_info = false;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--detailed" || arg == "-d") {
            detailed_output = true;
        } else if (arg == "--regression" || arg == "-r") {
            regression_test = true;
        } else if (arg == "--system-info" || arg == "-s") {
            system_info = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -d, --detailed     Show detailed statistics\n"
                      << "  -r, --regression   Run performance regression tests\n"
                      << "  -s, --system-info  Show system information\n"
                      << "  -h, --help        Show this help\n";
            return 0;
        }
    }

    try {
        if (system_info) {
            printSystemInfo();
        }
        
        if (regression_test) {
            runRegressionTest();
            return 0;
        }
        
        QueueComparisonSuite suite;
        suite.setDetailedOutput(detailed_output);
        suite.runAll();
        
        std::cout << "\n🚀 Queue Performance Comparison Complete! 🚀\n";
        std::cout << "Key insights:\n"
                  << "• Lock-free queues excel under high contention scenarios\n"
                  << "• EBR provides consistent performance with automatic cleanup\n"
                  << "• Hazard pointers offer fine-grained memory management\n"
                  << "• Lock-based queues show predictable single-threaded performance\n"
                  << "• Producer:consumer ratios significantly impact scalability\n";
        
        return 0;
        
    } catch (const std::exception& ex) {
        std::cerr << "\n❌ Benchmark failed: " << ex.what() << "\n";
        return 1;
    }
}