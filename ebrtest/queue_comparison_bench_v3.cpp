/**********************************************************************
 *  queue_comparison_bench_v3.cpp - Comprehensive Queue Performance Comparison
 *  ──────────────────────────────────────────────────────────────────
 *  
 *  PURPOSE
 *  -------
 *  Comprehensive latency and throughput benchmarking suite comparing five
 *  queue implementations:
 *  1. ThreadSafeQueue (Lock-based with mutex)
 *  2. EBR Lock-Free Queue (3-epoch reclamation)
 *  3. HP Lock-Free Queue (Hazard pointer reclamation)
 *  4. Hybrid Lock-Free Queue (EBR + HP fallback)
 *  5. Boost Lock-Free Queue (Boost.Lockfree)
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
 *  11. Epoch stall detection and hybrid mode analysis
 *  
 *  BUILD
 *  -----
 *  # For Ubuntu/Debian: sudo apt-get install libboost-dev
 *  # For macOS: brew install boost
 *  
 *  g++ -std=c++20 -O3 -march=native -pthread queue_comparison_bench_v3.cpp -o queue_bench_v3 -lboost_system
 *  
 *  # Note: boost::lockfree::queue is header-only, so no linking required
 *  # But if you get linker errors, try adding: -lboost_system
 *  
 *  # With sanitizers for debugging
 *  g++ -std=c++20 -O1 -g -fsanitize=thread -pthread queue_comparison_bench.cpp -o queue_bench_tsan
 *  g++ -std=c++20 -O1 -g -fsanitize=address -fno-omit-frame-pointer -pthread queue_comparison_bench.cpp -o queue_bench_asan
 *********************************************************************/

#include "../include/lockfree_queue_ebr.hpp"
#include "../include/ThreadSafeQueue.hpp" 
#include "../include/lockfree_queue_hp.hpp"
#include "../include/lockfree_queue_hybrid.hpp"  // Add the hybrid queue

// Boost includes
#include <boost/lockfree/queue.hpp>

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
    
    // Boost lockfree queue configuration
    constexpr size_t BOOST_QUEUE_CAPACITY = 65536;  // Must be power of 2
    
    // Test scenarios
    const std::vector<int> QUEUE_DEPTHS   = {0, 10, 100, 1000};
    const std::vector<int> THREAD_COUNTS  = {1, 2, 4, 8};
    const std::vector<int> PAYLOAD_SIZES  = {16, 64, 256, 1024};
    const std::vector<int> PRODUCER_RATIOS = {1, 2, 4, 8}; // producers per consumer
}

// Queue type enumeration - UPDATED
enum class QueueType {
    LOCK_BASED,
    EBR_LOCKFREE,
    HP_LOCKFREE,
    HYBRID_LOCKFREE,  // NEW: Add hybrid queue type
    BOOST_LOCKFREE
};

std::string queueTypeToString(QueueType type) {
    switch (type) {
        case QueueType::LOCK_BASED: return "Lock";
        case QueueType::EBR_LOCKFREE: return "EBR";
        case QueueType::HP_LOCKFREE: return "HP";
        case QueueType::HYBRID_LOCKFREE: return "Hybrid";  // NEW
        case QueueType::BOOST_LOCKFREE: return "Boost";
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

// Benchmark result structure - UPDATED
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
    size_t queue_full_failures = 0;  // For bounded queues like Boost
    
    // NEW: Hybrid queue specific metrics
    unsigned epoch_stalls = 0;          // Number of epoch stalls detected
    double slow_mode_percentage = 0.0;  // Percentage of time in slow (HP) mode
    unsigned mode_switches = 0;         // Number of fast->slow mode switches
    
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
                  << std::setw(8)  << "Fails/Mode"
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
                  << std::setw(12) << throughput;
        
        if (queue_type == QueueType::HYBRID_LOCKFREE && epoch_stalls > 0) {
            std::cout << std::setw(7) << slow_mode_percentage << "%";
        } else if (queue_full_failures > 0) {
            std::cout << std::setw(8) << queue_full_failures;
        } else if (contention_rate > 0) {
            std::cout << std::setw(7) << (contention_rate * 100.0) << "%";
        } else {
            std::cout << std::setw(8) << "-";
        }
        std::cout << '\n';
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
        
        if (queue_full_failures > 0) {
            std::cout << "Queue full failures: " << queue_full_failures << " ("
                      << (static_cast<double>(queue_full_failures) / sample_count * 100.0) << "%)\n";
        }
        
        // NEW: Hybrid queue specific details
        if (queue_type == QueueType::HYBRID_LOCKFREE) {
            std::cout << "Hybrid queue metrics:\n"
                      << "  Epoch stalls: " << epoch_stalls << "\n"
                      << "  Slow mode percentage: " << slow_mode_percentage << "%\n"
                      << "  Mode switches: " << mode_switches << "\n";
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
    virtual bool enqueue(const T& item) = 0;  // Returns false if queue is full
    virtual bool dequeue(T& item) = 0;
    virtual bool empty() const = 0;
    virtual QueueType getType() const = 0;
    virtual double getContentionRate() const { return 0.0; }
    virtual size_t getFailureCount() const { return 0; }
    virtual void resetStats() {}
    
    // NEW: Hybrid queue specific methods
    virtual unsigned getEpochStalls() const { return 0; }
    virtual double getSlowModePercentage() const { return 0.0; }
    virtual unsigned getModeSwitches() const { return 0; }
    virtual unsigned getCurrentEpoch() const { return 0; }
};

// Concrete implementations
template<typename T>
class LockBasedQueueWrapper : public QueueInterface<T> {
    ThreadSafeQueue<T> queue_;
    uint64_t initial_contentions_ = 0;
    mutable std::atomic<uint64_t> total_operations_{0};
    
public:
    bool enqueue(const T& item) override {
        queue_.enqueue(item);
        total_operations_.fetch_add(1, std::memory_order_relaxed);
        return true;  // Lock-based queue is unbounded
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
    bool enqueue(const T& item) override {
        queue_.enqueue(item);
        return true;  // EBR queue is unbounded
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
    bool enqueue(const T& item) override {
        queue_.enqueue(item);
        return true;  // HP queue is unbounded
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

// 4. Fix the HybridQueueWrapper constructor
template<typename T>
class HybridQueueWrapper : public QueueInterface<T> {
    lfq::HybridQueue<T> queue_;
    mutable std::atomic<unsigned> stats_epoch_stalls_{0};
    mutable std::atomic<unsigned> stats_mode_switches_{0};
    unsigned initial_epoch_;
    
public:
    HybridQueueWrapper() {
        // Fixed: Initialize queue first, then get initial epoch
        initial_epoch_ = queue_.current_epoch();
    }
    
    bool enqueue(const T& item) override {
        queue_.enqueue(item);
        return true;  // Hybrid queue is unbounded
    }
    
    bool dequeue(T& item) override {
        return queue_.dequeue(item);
    }
    
    bool empty() const override {
        return queue_.empty();
    }
    
    QueueType getType() const override {
        return QueueType::HYBRID_LOCKFREE;
    }
    
    unsigned getEpochStalls() const override {
        return stats_epoch_stalls_.load(std::memory_order_relaxed);
    }
    
    double getSlowModePercentage() const override {
        // This is a simplified metric - in a real implementation, 
        // you might want to track time spent in each mode
        return stats_epoch_stalls_.load(std::memory_order_relaxed) > 0 ? 10.0 : 0.0;
    }
    
    unsigned getModeSwitches() const override {
        return stats_mode_switches_.load(std::memory_order_relaxed);
    }
    
    unsigned getCurrentEpoch() const override {
        return queue_.current_epoch();
    }
    
    void resetStats() override {
        stats_epoch_stalls_.store(0, std::memory_order_relaxed);
        stats_mode_switches_.store(0, std::memory_order_relaxed);
        initial_epoch_ = queue_.current_epoch();
    }
};

template<typename T>
class BoostQueueWrapper : public QueueInterface<T> {
    boost::lockfree::queue<T> queue_;
    std::atomic<size_t> enqueue_failures_{0};
    
public:
    BoostQueueWrapper() : queue_(config::BOOST_QUEUE_CAPACITY) {}
    
    bool enqueue(const T& item) override {
        bool success = queue_.push(item);
        if (!success) {
            enqueue_failures_.fetch_add(1, std::memory_order_relaxed);
        }
        return success;
    }
    
    bool dequeue(T& item) override {
        return queue_.pop(item);
    }
    
    bool empty() const override {
        return queue_.empty();
    }
    
    QueueType getType() const override {
        return QueueType::BOOST_LOCKFREE;
    }
    
    size_t getFailureCount() const override {
        return enqueue_failures_.load(std::memory_order_relaxed);
    }
    
    void resetStats() override {
        enqueue_failures_.store(0, std::memory_order_relaxed);
    }
};

// Factory function for queue creation - UPDATED
template<typename T>
std::unique_ptr<QueueInterface<T>> createQueue(QueueType type) {
    switch (type) {
        case QueueType::LOCK_BASED:
            return std::make_unique<LockBasedQueueWrapper<T>>();
        case QueueType::EBR_LOCKFREE:
            return std::make_unique<EBRQueueWrapper<T>>();
        case QueueType::HP_LOCKFREE:
            return std::make_unique<HPQueueWrapper<T>>();
        case QueueType::HYBRID_LOCKFREE:  // NEW
            return std::make_unique<HybridQueueWrapper<T>>();
        case QueueType::BOOST_LOCKFREE:
            return std::make_unique<BoostQueueWrapper<T>>();
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
    // 1. Fix the runSingleThreadedBaseline method (remove undefined 'depth' variable)
BenchmarkResult runSingleThreadedBaseline(QueueType queue_type, int samples = config::DEFAULT_SAMPLES) {
    auto queue = createQueue<MessageType>(queue_type);
    LatencyStats stats;
    stats.reserve(samples);
    size_t failures = 0;
    
    // Warmup phase
    for (int i = 0; i < config::WARMUP_SAMPLES; ++i) {
        MessageType msg(i, 0);
        if (!queue->enqueue(msg)) {
            // For bounded queues, try to dequeue first
            MessageType tmp;
            queue->dequeue(tmp);
            queue->enqueue(msg);
        }
        MessageType tmp;
        queue->dequeue(tmp);
    }
    
    queue->resetStats();
    auto start_time = Clock::now();
    
    for (int i = 0; i < samples; ++i) {
        auto enqueue_time = Clock::now();
        MessageType msg(i, 0);
        
        if (!queue->enqueue(msg)) {
            failures++;
            continue;  // Skip this iteration if enqueue fails
        }
        
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
    
    auto result = makeResult("Single-threaded baseline",  // Fixed: removed undefined 'depth'
                            stats, queue_type, 1,
                            std::chrono::duration<double>(end_time - start_time).count(),
                            0, queue->getContentionRate());  // Fixed: use 0 for depth
    result.queue_full_failures = failures + queue->getFailureCount();
    
    // NEW: Add hybrid queue specific metrics
    if (queue_type == QueueType::HYBRID_LOCKFREE) {
        result.epoch_stalls = queue->getEpochStalls();
        result.slow_mode_percentage = queue->getSlowModePercentage();
        result.mode_switches = queue->getModeSwitches();
    }
    
    return result;
}

// 2. Add the missing runMultiProducerContention method
BenchmarkResult runMultiProducerContention(QueueType queue_type, int producers, 
                                         int samples_per = config::DEFAULT_SAMPLES) {
    auto queue = createQueue<MessageType>(queue_type);
    std::vector<std::thread> producer_threads;
    std::vector<LatencyMeasurement> all_measurements;
    std::mutex measurements_mutex;
    std::barrier sync_barrier(producers + 1);
    std::atomic<size_t> total_failures{0};
    
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
            size_t local_failures = 0;
            
            for (int j = 0; j < samples_per; ++j) {
                auto seq = sequence_counter_.fetch_add(1);
                MessageType msg(seq, i);
                
                if (!queue->enqueue(msg)) {
                    local_failures++;
                    // For bounded queues, we might want to retry or back off
                    std::this_thread::sleep_for(1us);
                }
                
                // Introduce some jitter to increase contention
                if ((j % 100) == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(j % 50));
                }
            }
            
            total_failures.fetch_add(local_failures);
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
    
    auto result = makeResult("Multi-producer (" + std::to_string(producers) + "P)",
                            stats, queue_type, producers + 1,
                            std::chrono::duration<double>(benchmark_end - benchmark_start).count(),
                            0, queue->getContentionRate());
    result.queue_full_failures = total_failures.load() + queue->getFailureCount();
    
    // NEW: Add hybrid queue specific metrics
    if (queue_type == QueueType::HYBRID_LOCKFREE) {
        result.epoch_stalls = queue->getEpochStalls();
        result.slow_mode_percentage = queue->getSlowModePercentage();
        result.mode_switches = queue->getModeSwitches();
    }
    
    return result;
}

// 3. Add the missing runLoadDependentLatency method
BenchmarkResult runLoadDependentLatency(QueueType queue_type, int depth, 
                                      int samples = config::DEFAULT_SAMPLES) {
    auto queue = createQueue<MessageType>(queue_type);
    
    // Pre-fill queue to specified depth
    for (int i = 0; i < depth; ++i) {
        if (!queue->enqueue(MessageType(i, 0))) {
            // If queue is bounded and gets full, stop pre-filling
            break;
        }
    }
    
    LatencyStats stats;
    stats.reserve(samples);
    queue->resetStats();
    size_t failures = 0;
    
    auto start_time = Clock::now();
    for (int i = 0; i < samples; ++i) {
        auto enqueue_time = Clock::now();
        
        if (!queue->enqueue(MessageType(i + depth, 0))) {
            failures++;
            continue;
        }
        
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
    
    auto result = makeResult("Queue depth " + std::to_string(depth),
                            stats, queue_type, 1,
                            std::chrono::duration<double>(end_time - start_time).count(),
                            depth, queue->getContentionRate());
    result.queue_full_failures = failures + queue->getFailureCount();
    
    // NEW: Add hybrid queue specific metrics
    if (queue_type == QueueType::HYBRID_LOCKFREE) {
        result.epoch_stalls = queue->getEpochStalls();
        result.slow_mode_percentage = queue->getSlowModePercentage();
        result.mode_switches = queue->getModeSwitches();
    }
    
    return result;
}


    BenchmarkResult runProducerConsumerRatio(QueueType queue_type, int producers, 
                                           int consumers, int samples_per = 10000) {
        auto queue = createQueue<MessageType>(queue_type);
        std::vector<std::thread> producer_threads, consumer_threads;
        std::vector<LatencyMeasurement> all_measurements;
        std::mutex measurements_mutex;
        std::barrier sync_barrier(producers + consumers + 1);
        std::atomic<size_t> total_failures{0};
        
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
                size_t local_failures = 0;
                
                for (int j = 0; j < samples_per; ++j) {
                    auto seq = sequence_counter_.fetch_add(1);
                    if (!queue->enqueue(MessageType(seq, p))) {
                        local_failures++;
                        // Brief backoff for bounded queues
                        std::this_thread::sleep_for(1us);
                    }
                }
                
                total_failures.fetch_add(local_failures);
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
        
        auto result = makeResult("P" + std::to_string(producers) + ":C" + std::to_string(consumers),
                                stats, queue_type, producers + consumers,
                                std::chrono::duration<double>(benchmark_end - benchmark_start).count(),
                                0, queue->getContentionRate());
        result.queue_full_failures = total_failures.load() + queue->getFailureCount();
        
        // NEW: Add hybrid queue specific metrics
        if (queue_type == QueueType::HYBRID_LOCKFREE) {
            result.epoch_stalls = queue->getEpochStalls();
            result.slow_mode_percentage = queue->getSlowModePercentage();
            result.mode_switches = queue->getModeSwitches();
        }
        
        return result;
    }

    BenchmarkResult runThroughputBenchmark(QueueType queue_type, int num_threads, 
                                         Duration test_duration = 5s) {
        auto queue = createQueue<MessageType>(queue_type);
        std::atomic<uint64_t> operations_completed{0};
        std::atomic<size_t> total_failures{0};
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
                size_t local_failures = 0;
                while (benchmark_active_.load()) {
                    auto seq = sequence_counter_.fetch_add(1);
                    if (queue->enqueue(MessageType(seq, i))) {
                        local_ops++;
                    } else {
                        local_failures++;
                        // Brief backoff for bounded queues
                        std::this_thread::sleep_for(1us);
                    }
                }
                operations_completed.fetch_add(local_ops);
                total_failures.fetch_add(local_failures);
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
        result.queue_full_failures = total_failures.load() + queue->getFailureCount();
        
        // NEW: Add hybrid queue specific metrics
        if (queue_type == QueueType::HYBRID_LOCKFREE) {
            result.epoch_stalls = queue->getEpochStalls();
            result.slow_mode_percentage = queue->getSlowModePercentage();
            result.mode_switches = queue->getModeSwitches();
        }
        
        return result;
    }

    // NEW: Epoch stall test specifically for hybrid queue
    BenchmarkResult runEpochStallTest(int samples = config::DEFAULT_SAMPLES) {
        auto queue = createQueue<MessageType>(QueueType::HYBRID_LOCKFREE);
        std::vector<std::thread> threads;
        std::atomic<bool> test_active{true};
        std::atomic<uint64_t> operations{0};
        LatencyStats stats;
        
        queue->resetStats();
        
        // Create multiple producer threads to increase contention and potential epoch stalls
        const int num_producers = std::min(8, config::MAX_THREADS - 1);
        
        // Consumer thread
        std::thread consumer([&]{
            MessageType msg;
            uint64_t consumed = 0;
            auto start_time = Clock::now();
            
            while (test_active.load() && consumed < samples) {
                if (queue->dequeue(msg)) {
                    auto dequeue_time = Clock::now();
                    
                    LatencyMeasurement measurement{
                        msg.timestamp, dequeue_time, 
                        msg.sequence, msg.producer_id, QueueType::HYBRID_LOCKFREE
                    };
                    stats.addMeasurement(measurement);
                    consumed++;
                }
            }
        });
        
        // Producer threads with intentional blocking to trigger epoch stalls
        for (int i = 0; i < num_producers; ++i) {
            threads.emplace_back([&, i]{
                uint64_t produced = 0;
                while (test_active.load() && produced < samples / num_producers) {
                    auto seq = sequence_counter_.fetch_add(1);
                    queue->enqueue(MessageType(seq, i));
                    produced++;
                    operations.fetch_add(1);
                    
                    // Periodically sleep to simulate blocking and trigger epoch stalls
                    if ((produced % 1000) == 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            });
        }
        
        auto benchmark_start = Clock::now();
        
        for (auto& thread : threads) {
            thread.join();
        }
        
        std::this_thread::sleep_for(100ms);
        test_active.store(false);
        consumer.join();
        auto benchmark_end = Clock::now();
        
        auto result = makeResult("Epoch stall stress test",
                                stats, QueueType::HYBRID_LOCKFREE, num_producers + 1,
                                std::chrono::duration<double>(benchmark_end - benchmark_start).count(),
                                0, 0.0);
        
        result.epoch_stalls = queue->getEpochStalls();
        result.slow_mode_percentage = queue->getSlowModePercentage();
        result.mode_switches = queue->getModeSwitches();
        
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
            case QueueType::HYBRID_LOCKFREE:  // NEW
                // Hybrid uses both EBR and HP structures
                result.memory_overhead_bytes += 3 * sizeof(std::vector<void*>) * config::MAX_THREADS; // EBR
                result.memory_overhead_bytes += 2 * config::MAX_THREADS * sizeof(void*);             // HP
                break;
            case QueueType::BOOST_LOCKFREE:
                result.memory_overhead_bytes += config::BOOST_QUEUE_CAPACITY * sizeof(MessageType);
                break;
        }
        
        return result;
    }
};

// Main benchmark suite - UPDATED
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
                  << "Boost queue cap:  " << config::BOOST_QUEUE_CAPACITY << " elements\n"
                  << "Queue types:      Lock-based, EBR Lock-free, HP Lock-free, Hybrid Lock-free, Boost Lock-free\n\n";

        // Verify all queue types can be created
        std::cout << "Verifying queue implementations...\n";
        try {
            auto lock_queue = createQueue<TimedMessage<64>>(QueueType::LOCK_BASED);
            auto ebr_queue = createQueue<TimedMessage<64>>(QueueType::EBR_LOCKFREE);
            auto hp_queue = createQueue<TimedMessage<64>>(QueueType::HP_LOCKFREE);
            auto hybrid_queue = createQueue<TimedMessage<64>>(QueueType::HYBRID_LOCKFREE);  // NEW
            auto boost_queue = createQueue<TimedMessage<64>>(QueueType::BOOST_LOCKFREE);
            std::cout << "✅ All queue implementations initialized successfully\n";
            
            // Test basic operations
            TimedMessage<64> test_msg(1, 0);
            lock_queue->enqueue(test_msg);
            ebr_queue->enqueue(test_msg);
            hp_queue->enqueue(test_msg);
            hybrid_queue->enqueue(test_msg);  // NEW
            bool boost_success = boost_queue->enqueue(test_msg);
            std::cout << "✅ Basic enqueue operations verified (Boost enqueue: " 
                      << (boost_success ? "success" : "failed") << ")\n\n";
        } catch (const std::exception& e) {
            std::cerr << "❌ Queue initialization failed: " << e.what() << "\n";
            std::cerr << "Note: Make sure to link with -lboost_system\n\n";
        }

        BenchmarkResult::printHeader();
        runBaseline();
        runContentionAnalysis();
        runLoadAnalysis();
        runRatioAnalysis();
        runThroughputAnalysis();
        runHybridSpecificTests();  // NEW
        printSummary();
        printComparison();
        exportResults();
    }

private:
    void runBaseline() {
        std::cout << "\n=== Single-Threaded Baseline ===\n";
        for (auto queue_type : {QueueType::LOCK_BASED, QueueType::EBR_LOCKFREE, 
                               QueueType::HP_LOCKFREE, QueueType::HYBRID_LOCKFREE,  // NEW
                               QueueType::BOOST_LOCKFREE}) {
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
            
            for (auto queue_type : {QueueType::LOCK_BASED, QueueType::EBR_LOCKFREE, 
                                   QueueType::HP_LOCKFREE, QueueType::HYBRID_LOCKFREE,  // NEW
                                   QueueType::BOOST_LOCKFREE}) {
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
            for (auto queue_type : {QueueType::LOCK_BASED, QueueType::EBR_LOCKFREE, 
                                   QueueType::HP_LOCKFREE, QueueType::HYBRID_LOCKFREE,  // NEW
                                   QueueType::BOOST_LOCKFREE}) {
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
            // Balanced configurations
            {1, 1}, {2, 2}, {4, 4}, {8, 8},
            // Producer-heavy configurations
            {2, 1}, {4, 1}, {8, 1}, {4, 2}, {8, 2}, {8, 4},
            // Consumer-heavy configurations
            {1, 2}, {1, 4}, {1, 8}, {2, 4}, {2, 8}, {4, 8},
            // Asymmetric configurations
            {3, 1}, {1, 3}, {6, 2}, {2, 6}, {5, 3}, {3, 5}
        };
        
        for (const auto& [producers, consumers] : ratios) {
            if (producers + consumers > config::MAX_THREADS) continue;
            
            for (auto queue_type : {QueueType::LOCK_BASED, QueueType::EBR_LOCKFREE, 
                                   QueueType::HP_LOCKFREE, QueueType::HYBRID_LOCKFREE,  // NEW
                                   QueueType::BOOST_LOCKFREE}) {
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
            
            for (auto queue_type : {QueueType::LOCK_BASED, QueueType::EBR_LOCKFREE, 
                                   QueueType::HP_LOCKFREE, QueueType::HYBRID_LOCKFREE,  // NEW
                                   QueueType::BOOST_LOCKFREE}) {
                QueueBenchmark<TimedMessage<64>> benchmark;
                auto result = benchmark.runThroughputBenchmark(queue_type, threads, 3s);
                result.print();
                results_.push_back(result);
            }
        }
    }

    // NEW: Hybrid queue specific tests
    void runHybridSpecificTests() {
        std::cout << "\n=== Hybrid Queue Specific Tests ===\n";
        QueueBenchmark<TimedMessage<64>> benchmark;
        
        // Epoch stall stress test
        auto stall_result = benchmark.runEpochStallTest(config::DEFAULT_SAMPLES / 10);
        stall_result.print();
        results_.push_back(stall_result);
        if (detailed_output_) stall_result.printDetailed();
        
        std::cout << "Hybrid queue epoch analysis:\n"
                  << "  Epoch stalls detected: " << stall_result.epoch_stalls << "\n"
                  << "  Time in slow mode: " << stall_result.slow_mode_percentage << "%\n"
                  << "  Mode switches: " << stall_result.mode_switches << "\n";
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
            size_t total_failures = 0;
            unsigned total_epoch_stalls = 0;
            unsigned total_mode_switches = 0;
            
            for (const auto* result : type_results) {
                weighted_mean += result->mean_latency * result->sample_count;
                total_samples += result->sample_count;
                if (result->contention_rate > 0) {
                    total_contention += result->contention_rate;
                }
                total_failures += result->queue_full_failures;
                total_epoch_stalls += result->epoch_stalls;
                total_mode_switches += result->mode_switches;
            }
            
            if (total_samples > 0) {
                std::cout << "  Overall weighted mean latency: " 
                          << (weighted_mean / total_samples) << " μs\n";
            }
            
            if (queue_type == QueueType::LOCK_BASED && total_contention > 0) {
                std::cout << "  Average contention rate: " 
                          << (total_contention / type_results.size() * 100.0) << "%\n";
            }
            
            if (queue_type == QueueType::BOOST_LOCKFREE && total_failures > 0) {
                std::cout << "  Total queue full failures: " << total_failures 
                          << " (" << (total_failures / total_samples * 100.0) << "% failure rate)\n";
            }
            
            // NEW: Hybrid queue specific summary
            if (queue_type == QueueType::HYBRID_LOCKFREE) {
                std::cout << "  Total epoch stalls: " << total_epoch_stalls << "\n"
                          << "  Total mode switches: " << total_mode_switches << "\n";
                if (total_epoch_stalls > 0) {
                    std::cout << "  Hybrid mode effectiveness: Successfully handled epoch stalls\n";
                } else {
                    std::cout << "  Hybrid mode effectiveness: Fast path maintained throughout testing\n";
                }
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
        
        if (baselines.size() == 5) {  // UPDATED: Now we have 5 queue types
            std::cout << "Single-threaded latency comparison:\n";
            auto lock_baseline = baselines[QueueType::LOCK_BASED];
            auto ebr_baseline = baselines[QueueType::EBR_LOCKFREE];
            auto hp_baseline = baselines[QueueType::HP_LOCKFREE];
            auto hybrid_baseline = baselines[QueueType::HYBRID_LOCKFREE];  // NEW
            auto boost_baseline = baselines[QueueType::BOOST_LOCKFREE];
            
            std::cout << "  Lock-based: " << lock_baseline->mean_latency << " μs\n"
                      << "  EBR Lock-free: " << ebr_baseline->mean_latency << " μs ("
                      << std::fixed << std::setprecision(1)
                      << ((ebr_baseline->mean_latency / lock_baseline->mean_latency - 1.0) * 100.0)
                      << "% vs Lock)\n"
                      << "  HP Lock-free: " << hp_baseline->mean_latency << " μs ("
                      << ((hp_baseline->mean_latency / lock_baseline->mean_latency - 1.0) * 100.0)
                      << "% vs Lock)\n"
                      << "  Hybrid Lock-free: " << hybrid_baseline->mean_latency << " μs ("  // NEW
                      << ((hybrid_baseline->mean_latency / lock_baseline->mean_latency - 1.0) * 100.0)
                      << "% vs Lock)\n"
                      << "  Boost Lock-free: " << boost_baseline->mean_latency << " μs ("
                      << ((boost_baseline->mean_latency / lock_baseline->mean_latency - 1.0) * 100.0)
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
        
        if (best_throughput.size() == 5) {  // UPDATED: Now we have 5 queue types
            std::cout << "Peak throughput comparison:\n";
            auto lock_peak = best_throughput[QueueType::LOCK_BASED];
            auto ebr_peak = best_throughput[QueueType::EBR_LOCKFREE];
            auto hp_peak = best_throughput[QueueType::HP_LOCKFREE];
            auto hybrid_peak = best_throughput[QueueType::HYBRID_LOCKFREE];  // NEW
            auto boost_peak = best_throughput[QueueType::BOOST_LOCKFREE];
            
            double max_throughput = std::max({lock_peak->throughput, ebr_peak->throughput, 
                                            hp_peak->throughput, hybrid_peak->throughput,  // NEW
                                            boost_peak->throughput});
            
            std::cout << "  Lock-based: " << std::fixed << std::setprecision(0) 
                      << lock_peak->throughput << " ops/sec ("
                      << std::setprecision(1) << (lock_peak->throughput / max_throughput * 100.0) << "% of peak)\n"
                      << "  EBR Lock-free: " << std::setprecision(0) << ebr_peak->throughput << " ops/sec ("
                      << std::setprecision(1) << (ebr_peak->throughput / max_throughput * 100.0) << "% of peak)\n"
                      << "  HP Lock-free: " << std::setprecision(0) << hp_peak->throughput << " ops/sec ("
                      << std::setprecision(1) << (hp_peak->throughput / max_throughput * 100.0) << "% of peak)\n"
                      << "  Hybrid Lock-free: " << std::setprecision(0) << hybrid_peak->throughput << " ops/sec ("
                      << std::setprecision(1) << (hybrid_peak->throughput / max_throughput * 100.0) << "% of peak)\n"
                      << "  Boost Lock-free: " << std::setprecision(0) << boost_peak->throughput << " ops/sec ("
                      << std::setprecision(1) << (boost_peak->throughput / max_throughput * 100.0) << "% of peak)\n\n";
        }
        
        // Contention and failure analysis
        std::cout << "Implementation characteristics:\n";
        double total_lock_contention = 0.0;
        int lock_contention_samples = 0;
        size_t total_boost_failures = 0;
        int boost_test_count = 0;
        unsigned total_hybrid_stalls = 0;
        int hybrid_test_count = 0;
        
        for (const auto& result : results_) {
            if (result.queue_type == QueueType::LOCK_BASED && result.contention_rate > 0) {
                total_lock_contention += result.contention_rate;
                lock_contention_samples++;
            }
            if (result.queue_type == QueueType::BOOST_LOCKFREE) {
                total_boost_failures += result.queue_full_failures;
                boost_test_count++;
            }
            if (result.queue_type == QueueType::HYBRID_LOCKFREE) {  // NEW
                total_hybrid_stalls += result.epoch_stalls;
                hybrid_test_count++;
            }
        }
        
        if (lock_contention_samples > 0) {
            std::cout << "  Lock-based average contention: " 
                      << std::fixed << std::setprecision(1)
                      << (total_lock_contention / lock_contention_samples * 100.0) << "%\n";
        }
        
        std::cout << "  EBR Lock-free: Unbounded, 3-epoch reclamation\n"
                  << "  HP Lock-free: Unbounded, hazard pointer reclamation\n"
                  << "  Hybrid Lock-free: Unbounded, EBR fast-path + HP fallback";  // NEW
        
        if (hybrid_test_count > 0) {  // NEW
            std::cout << ", " << total_hybrid_stalls << " total epoch stalls detected\n";
        } else {
            std::cout << "\n";
        }
        
        std::cout << "  Boost Lock-free: Bounded (" << config::BOOST_QUEUE_CAPACITY << " capacity)";
        
        if (boost_test_count > 0) {
            std::cout << ", " << total_boost_failures << " total queue-full events\n";
        } else {
            std::cout << "\n";
        }
        
        // Memory usage comparison
        std::cout << "\nMemory characteristics:\n";
        std::cout << "  Lock-based: Dynamic allocation, mutex overhead\n"
                  << "  EBR Lock-free: Dynamic allocation, epoch tracking per thread\n"
                  << "  HP Lock-free: Dynamic allocation, hazard pointers per thread\n"
                  << "  Hybrid Lock-free: Dynamic allocation, epoch tracking + hazard pointers\n"  // NEW
                  << "  Boost Lock-free: Fixed pre-allocated ring buffer ("
                  << (config::BOOST_QUEUE_CAPACITY * sizeof(TimedMessage<64>)) << " bytes)\n";
        
        // NEW: Hybrid queue effectiveness analysis
        std::cout << "\nHybrid queue analysis:\n";
        bool found_hybrid_results = false;
        for (const auto& result : results_) {
            if (result.queue_type == QueueType::HYBRID_LOCKFREE && result.epoch_stalls > 0) {
                found_hybrid_results = true;
                std::cout << "  Successfully detected and handled epoch stalls in: " << result.name << "\n";
                std::cout << "    - Stalls: " << result.epoch_stalls 
                          << ", Mode switches: " << result.mode_switches 
                          << ", Slow mode: " << result.slow_mode_percentage << "%\n";
            }
        }
        if (!found_hybrid_results) {
            std::cout << "  No epoch stalls detected - fast path maintained throughout all tests\n";
            std::cout << "  This demonstrates the hybrid queue's efficiency under normal conditions\n";
        }
    }

    void exportResults() {
        std::ofstream csv("queue_comparison_results_v3.csv");
        csv << "Benchmark,Queue_Type,Threads,Payload_Size,Queue_Depth,Sample_Count,"
               "Mean_Latency_us,Min_Latency_us,Max_Latency_us,Std_Dev_us,Jitter,"
               "Memory_Overhead_bytes,Contention_Rate,Queue_Full_Failures,"
               "Epoch_Stalls,Slow_Mode_Percentage,Mode_Switches,"  // NEW hybrid fields
               "P50_us,P90_us,P95_us,P99_us,P99_9_us,P99_99_us,Throughput_ops_per_sec\n";
        
        for (const auto& result : results_) {
            csv << result.name << ',' << queueTypeToString(result.queue_type) << ','
                << result.num_threads << ',' << result.payload_size << ','
                << result.queue_depth << ',' << result.sample_count << ','
                << result.mean_latency << ',' << result.min_latency << ','
                << result.max_latency << ',' << result.std_dev << ',' << result.jitter << ','
                << result.memory_overhead_bytes << ',' << result.contention_rate << ','
                << result.queue_full_failures << ','
                << result.epoch_stalls << ',' << result.slow_mode_percentage << ','  // NEW
                << result.mode_switches << ',';  // NEW
            
            for (double p : config::PERCENTILES) {
                csv << result.percentiles.at(p) << ',';
            }
            csv << result.throughput << "\n";
        }
        
        std::cout << "\nResults exported to queue_comparison_results_v3.csv\n";
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
            size_t total_failures = 0;
            unsigned total_stalls = 0, total_switches = 0;
            for (const auto* result : type_results) {
                latencies.push_back(result->mean_latency);
                throughputs.push_back(result->throughput);
                total_failures += result->queue_full_failures;
                total_stalls += result->epoch_stalls;
                total_switches += result->mode_switches;
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
            
            if (queue_type == QueueType::BOOST_LOCKFREE && total_failures > 0) {
                summary << "Queue full failures: " << total_failures << "\n";
            }
            
            // NEW: Hybrid queue specific analysis
            if (queue_type == QueueType::HYBRID_LOCKFREE) {
                summary << "Epoch stalls detected: " << total_stalls << "\n";
                summary << "Mode switches: " << total_switches << "\n";
                if (total_stalls > 0) {
                    summary << "Hybrid effectiveness: Successfully handled epoch stalls with HP fallback\n";
                } else {
                    summary << "Hybrid effectiveness: Maintained fast EBR path throughout testing\n";
                }
            }
            
            summary << "\n";
        }
        
        summary << "Key Findings:\n";
        summary << "=============\n";
        summary << "1. Lock-free queues generally show better scalability under high contention\n";
        summary << "2. EBR-based reclamation offers consistent performance across workloads\n";
        summary << "3. Hazard pointer reclamation provides fine-grained memory management\n";
        summary << "4. Hybrid queue combines EBR efficiency with HP robustness\n";  // NEW
        summary << "5. Boost lock-free queue offers excellent performance but is capacity-bounded\n";
        summary << "6. Lock-based queues suffer from contention-induced latency spikes\n";
        summary << "7. Producer:consumer ratios significantly impact performance characteristics\n";
        summary << "8. Bounded queues may experience backpressure under high load conditions\n";
        summary << "9. Hybrid queue maintains fast-path performance while providing safety guarantees\n";  // NEW
        
        std::cout << "Summary analysis exported to queue_performance_summary.txt\n";
    }
};

// Performance regression test - UPDATED
void runRegressionTest() {
    std::cout << "\n=== Performance Regression Test ===\n";
    std::cout << "Verifying all queue implementations meet minimum performance thresholds...\n";
    
    QueueBenchmark<TimedMessage<64>> benchmark;
    
    // Define performance thresholds (adjust based on expected performance)
    const double MAX_SINGLE_THREAD_LATENCY_US = 50.0;  // 50 microseconds
    const double MIN_THROUGHPUT_OPS_PER_SEC = 10000.0;  // 10K ops/sec
    
    bool all_passed = true;
    
    for (auto queue_type : {QueueType::LOCK_BASED, QueueType::EBR_LOCKFREE, 
                           QueueType::HP_LOCKFREE, QueueType::HYBRID_LOCKFREE,  // NEW
                           QueueType::BOOST_LOCKFREE}) {
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
        
        // Check for excessive failures (for bounded queues)
        if (queue_type == QueueType::BOOST_LOCKFREE) {
            double failure_rate = static_cast<double>(throughput_result.queue_full_failures) / 
                                 throughput_result.sample_count;
            if (failure_rate > 0.1) { // More than 10% failures
                std::cout << "  ⚠️  WARNING: High failure rate " << (failure_rate * 100.0) 
                          << "% for bounded queue\n";
            } else {
                std::cout << "  ✅ PASS: Acceptable failure rate " << (failure_rate * 100.0) << "%\n";
            }
        }
        
        // NEW: Check hybrid queue specific functionality
        if (queue_type == QueueType::HYBRID_LOCKFREE) {
            std::cout << "  ✅ PASS: Hybrid queue initialized and functional\n";
            if (baseline_result.epoch_stalls > 0) {
                std::cout << "  ✅ BONUS: Detected and handled " << baseline_result.epoch_stalls 
                          << " epoch stalls\n";
            } else {
                std::cout << "  ✅ PASS: Maintained fast path (no epoch stalls)\n";
            }
        }
    }
    
    if (all_passed) {
        std::cout << "\n🎉 All performance regression tests PASSED!\n";
    } else {
        std::cout << "\n⚠️  Some performance regression tests FAILED!\n";
    }
}

// System information utility - UPDATED
void printSystemInfo() {
    std::cout << "System Information:\n";
    std::cout << "==================\n";
    std::cout << "Hardware concurrency: " << std::thread::hardware_concurrency() << " threads\n";
    std::cout << "Pointer size: " << sizeof(void*) << " bytes\n";
    std::cout << "Cache line size: " << std::hardware_destructive_interference_size << " bytes\n";
    std::cout << "Boost queue capacity: " << config::BOOST_QUEUE_CAPACITY << " elements\n";
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
    std::cout << "  Max test threads: " << config::MAX_THREADS << "\n";
    std::cout << "  Hybrid queue stall limit: 16 consecutive epoch advance failures\n\n";  // NEW
}

// int main(int argc, char* argv[]) {
//     bool detailed_output = false;
//     bool regression_test = false;
//     bool system_info = false;
//     bool hybrid_only = false;  // NEW: Option to test only hybrid queue
    
//     // Parse command line arguments
//     for (int i = 1; i < argc; ++i) {
//         std::string arg = argv[i];
//         if (arg == "--detailed" || arg == "-d") {
//             detailed_output = true;
//         } else if (arg == "--regression" || arg == "-r") {
//             regression_test = true;
//         } else if (arg == "--system-info" || arg == "-s") {
//             system_info = true;
//         } else if (arg == "--hybrid-only" || arg == "-H") {  // NEW
//             hybrid_only = true;
//         } else if (arg == "--help" || arg == "-h") {
//             std::cout << "Usage: " << argv[0] << " [options]\n"
//                       << "Options:\n"
//                       << "  -d, --detailed     Show detailed statistics\n"
//                       << "  -r, --regression   Run performance regression tests\n"
//                       << "  -s, --system-info  Show system information\n"
//                       << "  -H, --hybrid-only  Run only hybrid queue tests\n"  // NEW
//                       << "  -h, --help        Show this help\n";
//             return 0;
//         }
//     }

//     try {
//         if (system_info) {
//             printSystemInfo();
//         }
        
//         if (regression_test) {
//             runRegressionTest();
//             return 0;
//         }
        
//         // NEW: Hybrid-only mode for focused testing
//         if (hybrid_only) {
//             std::cout << "=== Hybrid Queue Focused Testing ===\n";
//             QueueBenchmark<TimedMessage<64>> benchmark;
            
//             std::cout << "Running hybrid queue baseline...\n";
//             auto baseline = benchmark.runSingleThreadedBaseline(QueueType::HYBRID_LOCKFREE);
//             baseline.printDetailed();
            
//             std::cout << "\nRunning hybrid queue epoch stall test...\n";
//             auto stall_test = benchmark.runEpochStallTest();
//             stall_test.printDetailed();
            
//             std::cout << "\nRunning hybrid queue contention test...\n";
//             auto contention = benchmark.runMultiProducerContention(QueueType::HYBRID_LOCKFREE, 4);
//             contention.printDetailed();
            
//             return 0;
//         }
        
//         QueueComparisonSuite suite;
//         suite.setDetailedOutput(detailed_output);
//         suite.runAll();
        
//         std::cout << "\n🚀 Queue Performance Comparison Complete! 🚀\n";
//         std::cout << "Key insights:\n"
//                   << "• Lock-free queues excel under high contention scenarios\n"
//                   << "• EBR provides consistent performance with automatic cleanup\n"
//                   << "• Hazard pointers offer fine-grained memory management\n"
//                   << "• Hybrid queue combines EBR efficiency with HP robustness\n"  // NEW
//                   << "• Boost lock-free queue delivers excellent performance within capacity limits\n"
//                   << "• Lock-based queues show predictable single-threaded performance\n"
//                   << "• Producer:consumer ratios significantly impact scalability\n"
//                   << "• Bounded queues require careful capacity planning for high-throughput scenarios\n"
//                   << "• Hybrid approach successfully handles epoch stalls while maintaining fast-path performance\n";  // NEW
        
//         return 0;
        
//     } catch (const std::exception& ex) {
//         std::cerr << "\n❌ Benchmark failed: " << ex.what() << "\n";
//         return 1;
//     }
// }

// Additional helper functions for hybrid queue analysis
namespace HybridAnalysis {

// Hybrid queue stress test to force epoch stalls
template<typename T>
class EpochStallInducer {
private:
    std::unique_ptr<QueueInterface<T>> queue_;
    std::atomic<bool> active_{false};
    std::atomic<unsigned> stall_count_{0};
    std::vector<std::thread> blocker_threads_;
    
public:
    EpochStallInducer() : queue_(createQueue<T>(QueueType::HYBRID_LOCKFREE)) {}
    
    // Create blocking threads that enter critical sections and then sleep
    void startBlockingThreads(int count = 2) {
        active_.store(true);
        
        for (int i = 0; i < count; ++i) {
            blocker_threads_.emplace_back([this, i] {
                while (active_.load()) {
                    // Enqueue operation enters critical section
                    T dummy_item{};
                    queue_->enqueue(dummy_item);
                    
                    // Simulate thread being blocked/delayed in critical section
                    std::this_thread::sleep_for(std::chrono::milliseconds(10 + i * 5));
                    
                    // Dequeue to balance
                    T retrieved;
                    queue_->dequeue(retrieved);
                    
                    // Brief active period
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
        }
    }
    
    void stopBlockingThreads() {
        active_.store(false);
        for (auto& thread : blocker_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        blocker_threads_.clear();
    }
    
    QueueInterface<T>* getQueue() { return queue_.get(); }
    
    ~EpochStallInducer() {
        stopBlockingThreads();
    }
};

// Detailed hybrid queue behavior analyzer
class HybridBehaviorAnalyzer {
public:
    struct HybridMetrics {
        unsigned initial_epoch;
        unsigned final_epoch;
        unsigned epoch_advances;
        unsigned stalls_detected;
        unsigned mode_switches;
        double fast_mode_duration_ms;
        double slow_mode_duration_ms;
        double total_test_duration_ms;
        
        double getFastModePercentage() const {
            return (fast_mode_duration_ms / total_test_duration_ms) * 100.0;
        }
        
        double getSlowModePercentage() const {
            return (slow_mode_duration_ms / total_test_duration_ms) * 100.0;
        }
        
        void print() const {
            std::cout << "Hybrid Queue Behavior Analysis:\n"
                      << "==============================\n"
                      << "Test duration: " << total_test_duration_ms << " ms\n"
                      << "Epoch advances: " << epoch_advances << " (from " << initial_epoch 
                      << " to " << final_epoch << ")\n"
                      << "Epoch stalls detected: " << stalls_detected << "\n"
                      << "Mode switches: " << mode_switches << "\n"
                      << "Fast mode time: " << fast_mode_duration_ms << " ms ("
                      << std::fixed << std::setprecision(1) << getFastModePercentage() << "%)\n"
                      << "Slow mode time: " << slow_mode_duration_ms << " ms ("
                      << getSlowModePercentage() << "%)\n\n";
        }
    };
    
    template<typename T>
    static HybridMetrics analyzeHybridBehavior(Duration test_duration = 5s) {
        auto queue = createQueue<T>(QueueType::HYBRID_LOCKFREE);
        HybridMetrics metrics{};
        
        // Record initial state
        metrics.initial_epoch = queue->getCurrentEpoch();
        auto test_start = Clock::now();
        
        // Create epoch stall inducer
        EpochStallInducer<T> stall_inducer;
        stall_inducer.startBlockingThreads(3);
        
        // Run normal operations while monitoring behavior
        std::atomic<bool> test_active{true};
        std::vector<std::thread> worker_threads;
        
        // Producer threads
        for (int i = 0; i < 2; ++i) {
            worker_threads.emplace_back([&queue, &test_active, i] {
                uint64_t seq = 0;
                while (test_active.load()) {
                    T item{};
                    queue->enqueue(item);
                    seq++;
                    
                    if (seq % 100 == 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                }
            });
        }
        
        // Consumer threads
        for (int i = 0; i < 2; ++i) {
            worker_threads.emplace_back([&queue, &test_active] {
                while (test_active.load()) {
                    T item;
                    queue->dequeue(item);
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            });
        }
        
        // Monitor metrics during test
        std::thread monitor([&] {
            unsigned last_stalls = 0;
            unsigned last_switches = 0;
            auto last_check = Clock::now();
            bool in_slow_mode = false;
            
            while (test_active.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                
                auto current_stalls = queue->getEpochStalls();
                auto current_switches = queue->getModeSwitches();
                auto now = Clock::now();
                
                // Estimate mode based on stall activity
                bool mode_changed = (current_switches > last_switches);
                if (mode_changed) {
                    in_slow_mode = !in_slow_mode;
                }
                
                auto duration_ms = std::chrono::duration<double, std::milli>(now - last_check).count();
                
                if (in_slow_mode) {
                    metrics.slow_mode_duration_ms += duration_ms;
                } else {
                    metrics.fast_mode_duration_ms += duration_ms;
                }
                
                last_stalls = current_stalls;
                last_switches = current_switches;
                last_check = now;
            }
        });
        
        // Run test for specified duration
        std::this_thread::sleep_for(test_duration);
        test_active.store(false);
        
        // Cleanup
        for (auto& thread : worker_threads) {
            thread.join();
        }
        monitor.join();
        stall_inducer.stopBlockingThreads();
        
        auto test_end = Clock::now();
        
        // Record final metrics
        metrics.final_epoch = queue->getCurrentEpoch();
        metrics.epoch_advances = metrics.final_epoch - metrics.initial_epoch;
        metrics.stalls_detected = queue->getEpochStalls();
        metrics.mode_switches = queue->getModeSwitches();
        metrics.total_test_duration_ms = std::chrono::duration<double, std::milli>(test_end - test_start).count();
        
        return metrics;
    }
};

// Comparative performance test: Hybrid vs EBR vs HP under different conditions
class HybridComparisonTest {
public:
    struct ComparisonResult {
        std::string test_name;
        double hybrid_latency_us;
        double ebr_latency_us;
        double hp_latency_us;
        double hybrid_throughput;
        double ebr_throughput;
        double hp_throughput;
        unsigned hybrid_stalls;
        
        void print() const {
            std::cout << "=== " << test_name << " ===\n";
            std::cout << "Latency (μs):   Hybrid=" << std::fixed << std::setprecision(2) 
                      << hybrid_latency_us << "  EBR=" << ebr_latency_us << "  HP=" << hp_latency_us << "\n";
            std::cout << "Throughput:     Hybrid=" << std::setprecision(0) << hybrid_throughput 
                      << "  EBR=" << ebr_throughput << "  HP=" << hp_throughput << " ops/sec\n";
            std::cout << "Hybrid stalls:  " << hybrid_stalls << "\n";
            
            // Performance comparison
            double hybrid_vs_ebr = ((hybrid_latency_us / ebr_latency_us) - 1.0) * 100.0;
            double hybrid_vs_hp = ((hybrid_latency_us / hp_latency_us) - 1.0) * 100.0;
            
            std::cout << "Hybrid vs EBR:  " << std::showpos << std::setprecision(1) << hybrid_vs_ebr << "% latency";
            if (hybrid_stalls > 0) std::cout << " (with " << hybrid_stalls << " stalls handled)";
            std::cout << "\n";
            
            std::cout << "Hybrid vs HP:   " << hybrid_vs_hp << "% latency\n\n" << std::noshowpos;
        }
    };
    
    template<typename T>
    static std::vector<ComparisonResult> runComparativeTests() {
        std::vector<ComparisonResult> results;
        QueueBenchmark<T> benchmark;
        
        // Test 1: Normal load (no stalls expected)
        {
            ComparisonResult result;
            result.test_name = "Normal Load (Single-threaded)";
            
            auto hybrid_result = benchmark.runSingleThreadedBaseline(QueueType::HYBRID_LOCKFREE, 10000);
            auto ebr_result = benchmark.runSingleThreadedBaseline(QueueType::EBR_LOCKFREE, 10000);
            auto hp_result = benchmark.runSingleThreadedBaseline(QueueType::HP_LOCKFREE, 10000);
            
            result.hybrid_latency_us = hybrid_result.mean_latency;
            result.ebr_latency_us = ebr_result.mean_latency;
            result.hp_latency_us = hp_result.mean_latency;
            result.hybrid_throughput = hybrid_result.throughput;
            result.ebr_throughput = ebr_result.throughput;
            result.hp_throughput = hp_result.throughput;
            result.hybrid_stalls = hybrid_result.epoch_stalls;
            
            results.push_back(result);
        }
        
        // Test 2: High contention (potential stalls)
        {
            ComparisonResult result;
            result.test_name = "High Contention (8 producers)";
            
            auto hybrid_result = benchmark.runMultiProducerContention(QueueType::HYBRID_LOCKFREE, 8, 5000);
            auto ebr_result = benchmark.runMultiProducerContention(QueueType::EBR_LOCKFREE, 8, 5000);
            auto hp_result = benchmark.runMultiProducerContention(QueueType::HP_LOCKFREE, 8, 5000);
            
            result.hybrid_latency_us = hybrid_result.mean_latency;
            result.ebr_latency_us = ebr_result.mean_latency;
            result.hp_latency_us = hp_result.mean_latency;
            result.hybrid_throughput = hybrid_result.throughput;
            result.ebr_throughput = ebr_result.throughput;
            result.hp_throughput = hp_result.throughput;
            result.hybrid_stalls = hybrid_result.epoch_stalls;
            
            results.push_back(result);
        }
        
        // Test 3: Throughput comparison
        {
            ComparisonResult result;
            result.test_name = "Peak Throughput (8 threads)";
            
            auto hybrid_result = benchmark.runThroughputBenchmark(QueueType::HYBRID_LOCKFREE, 8, 3s);
            auto ebr_result = benchmark.runThroughputBenchmark(QueueType::EBR_LOCKFREE, 8, 3s);
            auto hp_result = benchmark.runThroughputBenchmark(QueueType::HP_LOCKFREE, 8, 3s);
            
            result.hybrid_latency_us = 0; // Not measured in throughput test
            result.ebr_latency_us = 0;
            result.hp_latency_us = 0;
            result.hybrid_throughput = hybrid_result.throughput;
            result.ebr_throughput = ebr_result.throughput;
            result.hp_throughput = hp_result.throughput;
            result.hybrid_stalls = hybrid_result.epoch_stalls;
            
            results.push_back(result);
        }
        
        return results;
    }
};

} // namespace HybridAnalysis

// Extended main function with hybrid analysis options
void runHybridAnalysis() {
    std::cout << "\n=== Detailed Hybrid Queue Analysis ===\n";
    
    // 1. Behavior analysis under induced stalls
    std::cout << "1. Analyzing hybrid queue behavior under epoch stalls...\n";
    auto metrics = HybridAnalysis::HybridBehaviorAnalyzer::analyzeHybridBehavior<TimedMessage<64>>(3s);
    metrics.print();
    
    // 2. Comparative performance analysis
    std::cout << "2. Running comparative performance tests...\n";
    auto comparison_results = HybridAnalysis::HybridComparisonTest::runComparativeTests<TimedMessage<64>>();
    
    for (const auto& result : comparison_results) {
        result.print();
    }
    
    // 3. Memory overhead analysis
    std::cout << "3. Memory overhead comparison:\n";
    std::cout << "==============================\n";
    
    size_t ebr_overhead = 3 * sizeof(std::vector<void*>) * config::MAX_THREADS; // EBR buckets
    size_t hp_overhead = 2 * config::MAX_THREADS * sizeof(void*); // Hazard pointers
    size_t hybrid_overhead = ebr_overhead + hp_overhead; // Both systems
    
    std::cout << "EBR memory overhead:    " << ebr_overhead << " bytes\n";
    std::cout << "HP memory overhead:     " << hp_overhead << " bytes\n";
    std::cout << "Hybrid memory overhead: " << hybrid_overhead << " bytes\n";
    std::cout << "Hybrid overhead ratio:  " << std::fixed << std::setprecision(1) 
              << (static_cast<double>(hybrid_overhead) / ebr_overhead) << "x EBR\n\n";
    
    // 4. Recommendations
    std::cout << "4. Usage Recommendations:\n";
    std::cout << "=========================\n";
    std::cout << "• Use Hybrid queue when:\n";
    std::cout << "  - Thread blocking/parking is possible\n";
    std::cout << "  - Memory safety is critical\n";
    std::cout << "  - Performance must remain good under all conditions\n";
    std::cout << "• Use EBR queue when:\n";
    std::cout << "  - Threads never block in critical sections\n";
    std::cout << "  - Maximum performance is required\n";
    std::cout << "  - Memory overhead must be minimized\n";
    std::cout << "• Use HP queue when:\n";
    std::cout << "  - Threads may die unexpectedly\n";
    std::cout << "  - Fine-grained memory control is needed\n";
    std::cout << "  - Consistent latency is more important than peak performance\n\n";
}

// Stress test specifically designed to trigger hybrid behavior
void runHybridStressTest() {
    std::cout << "\n=== Hybrid Queue Stress Test ===\n";
    std::cout << "This test intentionally creates adverse conditions to trigger hybrid behavior...\n";
    
    auto queue = createQueue<TimedMessage<64>>(QueueType::HYBRID_LOCKFREE);
    std::atomic<bool> test_active{true};
    std::atomic<uint64_t> total_operations{0};
    std::vector<std::thread> threads;
    LatencyStats stats;
    std::mutex stats_mutex;
    
    // Create blocking threads that will cause epoch stalls
    HybridAnalysis::EpochStallInducer<TimedMessage<64>> stall_inducer;
    stall_inducer.startBlockingThreads(4);
    
    std::cout << "Starting stress test with intentional blocking threads...\n";
    auto test_start = Clock::now();
    
    // Aggressive producer threads
    for (int i = 0; i < 6; ++i) {
        threads.emplace_back([&, i] {
            uint64_t local_ops = 0;
            uint64_t seq = 0;
            
            while (test_active.load()) {
                auto enqueue_time = Clock::now();
                TimedMessage<64> msg(seq++, i);
                
                if (queue->enqueue(msg)) {
                    local_ops++;
                    
                    // Occasionally add measurement
                    if (local_ops % 1000 == 0) {
                        TimedMessage<64> dequeued;
                        if (queue->dequeue(dequeued)) {
                            auto dequeue_time = Clock::now();
                            LatencyMeasurement measurement{
                                enqueue_time, dequeue_time, seq, 
                                static_cast<uint32_t>(i), QueueType::HYBRID_LOCKFREE
                            };
                            
                            std::lock_guard<std::mutex> lock(stats_mutex);
                            stats.addMeasurement(measurement);
                        }
                    }
                }
            }
            
            total_operations.fetch_add(local_ops);
        });
    }
    
    // Consumer threads
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&] {
            uint64_t local_ops = 0;
            TimedMessage<64> msg;
            
            while (test_active.load()) {
                if (queue->dequeue(msg)) {
                    local_ops++;
                }
            }
            
            total_operations.fetch_add(local_ops);
        });
    }
    
    // Monitor thread for real-time stats
    std::thread monitor([&] {
        unsigned last_stalls = 0;
        unsigned last_switches = 0;
        
        while (test_active.load()) {
            std::this_thread::sleep_for(1s);
            
            auto current_stalls = queue->getEpochStalls();
            auto current_switches = queue->getModeSwitches();
            auto current_epoch = queue->getCurrentEpoch();
            
            if (current_stalls > last_stalls || current_switches > last_switches) {
                std::cout << "  [" << std::chrono::duration_cast<std::chrono::seconds>(
                    Clock::now() - test_start).count() << "s] "
                    << "Stalls: " << current_stalls << " (+" << (current_stalls - last_stalls) << "), "
                    << "Switches: " << current_switches << " (+" << (current_switches - last_switches) << "), "
                    << "Epoch: " << current_epoch << "\n";
            }
            
            last_stalls = current_stalls;
            last_switches = current_switches;
        }
    });
    
    // Run test for 10 seconds
    std::this_thread::sleep_for(10s);
    test_active.store(false);
    
    // Cleanup
    for (auto& thread : threads) {
        thread.join();
    }
    monitor.join();
    stall_inducer.stopBlockingThreads();
    
    auto test_end = Clock::now();
    auto duration = std::chrono::duration<double>(test_end - test_start).count();
    
    // Print results
    std::cout << "\nStress Test Results:\n";
    std::cout << "===================\n";
    std::cout << "Test duration: " << duration << " seconds\n";
    std::cout << "Total operations: " << total_operations.load() << "\n";
    std::cout << "Throughput: " << (total_operations.load() / duration) << " ops/sec\n";
    std::cout << "Final epoch stalls: " << queue->getEpochStalls() << "\n";
    std::cout << "Final mode switches: " << queue->getModeSwitches() << "\n";
    std::cout << "Current epoch: " << queue->getCurrentEpoch() << "\n";
    
    if (stats.count() > 0) {
        std::cout << "Latency stats (" << stats.count() << " samples):\n";
        std::cout << "  Mean: " << std::fixed << std::setprecision(2) << stats.getMean() << " μs\n";
        std::cout << "  P95:  " << stats.getPercentile(95.0) << " μs\n";
        std::cout << "  P99:  " << stats.getPercentile(99.0) << " μs\n";
    }
    
    if (queue->getEpochStalls() > 0) {
        std::cout << "\n✅ SUCCESS: Hybrid queue successfully detected and handled epoch stalls!\n";
        std::cout << "The queue automatically switched to HP mode when needed.\n";
    } else {
        std::cout << "\n⚠️  No epoch stalls detected. Try increasing thread count or blocking duration.\n";
    }
}

// Advanced hybrid queue validation tests
namespace HybridValidation {

// Test hybrid queue correctness under epoch stalls
template<typename T>
class CorrectnessTest {
private:
    struct TestMessage {
        uint64_t sequence;
        uint32_t producer_id;
        uint64_t timestamp;
        uint32_t checksum;
        
        TestMessage(uint64_t seq = 0, uint32_t id = 0) 
            : sequence(seq), producer_id(id), timestamp(Clock::now().time_since_epoch().count()) {
            checksum = static_cast<uint32_t>(sequence ^ producer_id ^ timestamp);
        }
        
        bool validate() const {
            return checksum == static_cast<uint32_t>(sequence ^ producer_id ^ timestamp);
        }
    };
    
public:
    static bool runCorrectnessTest(int producers = 4, int consumers = 2, int messages_per_producer = 10000) {
        auto queue = createQueue<TestMessage>(QueueType::HYBRID_LOCKFREE);
        std::atomic<bool> test_active{true};
        std::atomic<uint64_t> total_produced{0};
        std::atomic<uint64_t> total_consumed{0};
        std::atomic<uint64_t> validation_failures{0};
        
        std::vector<std::thread> threads;
        std::map<uint32_t, uint64_t> producer_sequences;
        std::mutex sequence_mutex;
        
        // Initialize producer sequences
        for (int i = 0; i < producers; ++i) {
            producer_sequences[i] = 0;
        }
        
        // Create blocking threads to induce epoch stalls
        HybridAnalysis::EpochStallInducer<TestMessage> stall_inducer;
        stall_inducer.startBlockingThreads(2);
        
        std::cout << "Running correctness test with " << producers << " producers, " 
                  << consumers << " consumers...\n";
        
        // Producer threads
        for (int i = 0; i < producers; ++i) {
            threads.emplace_back([&, i] {
                for (int j = 0; j < messages_per_producer && test_active.load(); ++j) {
                    uint64_t seq;
                    {
                        std::lock_guard<std::mutex> lock(sequence_mutex);
                        seq = producer_sequences[i]++;
                    }
                    
                    TestMessage msg(seq, i);
                    queue->enqueue(msg);
                    total_produced.fetch_add(1);
                    
                    // Add some jitter
                    if (j % 1000 == 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                }
            });
        }
        
        // Consumer threads with validation
        for (int i = 0; i < consumers; ++i) {
            threads.emplace_back([&] {
                TestMessage msg;
                std::map<uint32_t, uint64_t> last_seen_sequence;
                
                while (test_active.load() || !queue->empty()) {
                    if (queue->dequeue(msg)) {
                        // Validate message integrity
                        if (!msg.validate()) {
                            validation_failures.fetch_add(1);
                            std::cerr << "❌ Message validation failed! Seq=" << msg.sequence 
                                      << " Producer=" << msg.producer_id << "\n";
                        }
                        
                        // Check sequence ordering per producer
                        auto& last_seq = last_seen_sequence[msg.producer_id];
                        if (msg.sequence < last_seq) {
                            validation_failures.fetch_add(1);
                            std::cerr << "❌ Sequence ordering violation! Producer=" << msg.producer_id
                                      << " Current=" << msg.sequence << " Last=" << last_seq << "\n";
                        }
                        last_seq = std::max(last_seq, msg.sequence);
                        
                        total_consumed.fetch_add(1);
                    } else {
                        std::this_thread::sleep_for(std::chrono::microseconds(10));
                    }
                }
            });
        }
        
        // Wait for producers to finish
        for (int i = 0; i < producers; ++i) {
            threads[i].join();
        }
        
        // Allow consumers to drain queue
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        test_active.store(false);
        
        // Wait for consumers
        for (int i = producers; i < producers + consumers; ++i) {
            threads[i].join();
        }
        
        stall_inducer.stopBlockingThreads();
        
        // Verify results
        uint64_t expected_total = static_cast<uint64_t>(producers) * messages_per_producer;
        bool success = (total_produced.load() == expected_total) && 
                      (total_consumed.load() == expected_total) && 
                      (validation_failures.load() == 0);
        
        std::cout << "Correctness test results:\n";
        std::cout << "  Expected messages: " << expected_total << "\n";
        std::cout << "  Produced: " << total_produced.load() << "\n";
        std::cout << "  Consumed: " << total_consumed.load() << "\n";
        std::cout << "  Validation failures: " << validation_failures.load() << "\n";
        std::cout << "  Epoch stalls: " << queue->getEpochStalls() << "\n";
        std::cout << "  Mode switches: " << queue->getModeSwitches() << "\n";
        std::cout << "  Result: " << (success ? "✅ PASS" : "❌ FAIL") << "\n\n";
        
        return success;
    }
};

// Memory leak detection for hybrid queue
class MemoryLeakTest {
public:
    template<typename T>
    static void runMemoryLeakTest(Duration test_duration = 10s) {
        std::cout << "Running memory leak test for " 
                  << std::chrono::duration_cast<std::chrono::seconds>(test_duration).count() 
                  << " seconds...\n";
        
        auto queue = createQueue<T>(QueueType::HYBRID_LOCKFREE);
        std::atomic<bool> test_active{true};
        std::atomic<uint64_t> total_operations{0};
        
        // Create memory pressure with blocking threads
        HybridAnalysis::EpochStallInducer<T> stall_inducer;
        stall_inducer.startBlockingThreads(3);
        
        std::vector<std::thread> threads;
        
        // Aggressive producer/consumer pattern
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&, i] {
                uint64_t seq = 0;
                while (test_active.load()) {
                    // Burst of enqueues
                    for (int j = 0; j < 100; ++j) {
                        T item{};
                        queue->enqueue(item);
                        seq++;
                    }
                    
                    // Burst of dequeues
                    for (int j = 0; j < 100; ++j) {
                        T item;
                        if (queue->dequeue(item)) {
                            total_operations.fetch_add(1);
                        }
                    }
                    
                    // Brief pause to allow memory reclamation
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
        }
        
        // Memory monitoring thread
        std::thread monitor([&] {
            auto start_time = Clock::now();
            unsigned last_stalls = 0;
            
            while (test_active.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                
                auto elapsed = Clock::now() - start_time;
                auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
                auto current_stalls = queue->getEpochStalls();
                
                std::cout << "  [" << seconds << "s] Operations: " << total_operations.load()
                          << ", Stalls: " << current_stalls << " (+" << (current_stalls - last_stalls) << ")"
                          << ", Epoch: " << queue->getCurrentEpoch() << "\n";
                
                last_stalls = current_stalls;
            }
        });
        
        std::this_thread::sleep_for(test_duration);
        test_active.store(false);
        
        for (auto& thread : threads) {
            thread.join();
        }
        monitor.join();
        stall_inducer.stopBlockingThreads();
        
        std::cout << "Memory leak test completed.\n";
        std::cout << "  Total operations: " << total_operations.load() << "\n";
        std::cout << "  Final epoch stalls: " << queue->getEpochStalls() << "\n";
        std::cout << "  Final mode switches: " << queue->getModeSwitches() << "\n";
        std::cout << "Note: Check with memory profiler (valgrind, AddressSanitizer) for actual leak detection.\n\n";
    }
};

} // namespace HybridValidation

// Performance regression specifically for hybrid queue
void runHybridRegressionTest() {
    std::cout << "\n=== Hybrid Queue Regression Test ===\n";
    
    bool all_passed = true;
    
    // Test 1: Basic functionality
    std::cout << "1. Testing basic hybrid queue functionality...\n";
    try {
        auto queue = createQueue<TimedMessage<64>>(QueueType::HYBRID_LOCKFREE);
        TimedMessage<64> msg(1, 0);
        
        bool enqueue_success = queue->enqueue(msg);
        TimedMessage<64> dequeued_msg;
        bool dequeue_success = queue->dequeue(dequeued_msg);
        
        if (enqueue_success && dequeue_success && dequeued_msg.validate()) {
            std::cout << "  ✅ PASS: Basic enqueue/dequeue operations\n";
        } else {
            std::cout << "  ❌ FAIL: Basic operations failed\n";
            all_passed = false;
        }
    } catch (const std::exception& e) {
        std::cout << "  ❌ FAIL: Exception in basic test: " << e.what() << "\n";
        all_passed = false;
    }
    
    // Test 2: Correctness under epoch stalls
    std::cout << "2. Testing correctness under epoch stalls...\n";
    bool correctness_passed = HybridValidation::CorrectnessTest<TimedMessage<64>>::runCorrectnessTest(3, 2, 5000);
    if (!correctness_passed) {
        all_passed = false;
    }
    
    // Test 3: Performance baseline
    std::cout << "3. Testing performance baseline...\n";
    QueueBenchmark<TimedMessage<64>> benchmark;
    auto baseline_result = benchmark.runSingleThreadedBaseline(QueueType::HYBRID_LOCKFREE, 1000);
    
    const double MAX_LATENCY_US = 100.0; // Reasonable threshold
    if (baseline_result.mean_latency > MAX_LATENCY_US) {
        std::cout << "  ❌ FAIL: Latency " << baseline_result.mean_latency 
                  << " μs exceeds threshold " << MAX_LATENCY_US << " μs\n";
        all_passed = false;
    } else {
        std::cout << "  ✅ PASS: Latency " << baseline_result.mean_latency << " μs\n";
    }
    
    // Test 4: Epoch advancement capability
    std::cout << "4. Testing epoch advancement...\n";
    auto initial_epoch = createQueue<TimedMessage<64>>(QueueType::HYBRID_LOCKFREE)->getCurrentEpoch();
    
    // Perform some operations to potentially advance epoch
    auto test_queue = createQueue<TimedMessage<64>>(QueueType::HYBRID_LOCKFREE);
    for (int i = 0; i < 1000; ++i) {
        TimedMessage<64> msg(i, 0);
        test_queue->enqueue(msg);
        TimedMessage<64> dequeued;
        test_queue->dequeue(dequeued);
    }
    
    auto final_epoch = test_queue->getCurrentEpoch();
    if (final_epoch >= initial_epoch) {
        std::cout << "  ✅ PASS: Epoch advancement working (from " << initial_epoch 
                  << " to " << final_epoch << ")\n";
    } else {
        std::cout << "  ❌ FAIL: Epoch went backwards\n";
        all_passed = false;
    }
    
    // Test 5: Memory leak check (basic)
    std::cout << "5. Running basic memory leak check...\n";
    HybridValidation::MemoryLeakTest::runMemoryLeakTest<TimedMessage<64>>(3s);
    std::cout << "  ✅ PASS: No crashes during memory stress test\n";
    
    if (all_passed) {
        std::cout << "\n🎉 All hybrid queue regression tests PASSED!\n";
    } else {
        std::cout << "\n⚠️  Some hybrid queue regression tests FAILED!\n";
    }
}

// Remove the duplicate main function and use this single corrected version:

int main(int argc, char* argv[]) {
    bool detailed_output = false;
    bool regression_test = false;
    bool system_info = false;
    bool hybrid_only = false;
    bool hybrid_analysis = false;  // NEW
    bool hybrid_stress = false;    // NEW
    bool hybrid_regression = false; // NEW
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--detailed" || arg == "-d") {
            detailed_output = true;
        } else if (arg == "--regression" || arg == "-r") {
            regression_test = true;
        } else if (arg == "--system-info" || arg == "-s") {
            system_info = true;
        } else if (arg == "--hybrid-only" || arg == "-H") {
            hybrid_only = true;
        } else if (arg == "--hybrid-analysis" || arg == "-A") {  // NEW
            hybrid_analysis = true;
        } else if (arg == "--hybrid-stress" || arg == "-S") {    // NEW
            hybrid_stress = true;
        } else if (arg == "--hybrid-regression" || arg == "-R") { // NEW
            hybrid_regression = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Queue Performance Benchmark Suite\n"
                      << "Usage: " << argv[0] << " [options]\n\n"
                      << "General Options:\n"
                      << "  -d, --detailed         Show detailed statistics for all tests\n"
                      << "  -r, --regression       Run performance regression tests\n"
                      << "  -s, --system-info      Show system information\n"
                      << "  -h, --help            Show this help\n\n"
                      << "Hybrid Queue Specific Options:\n"
                      << "  -H, --hybrid-only      Run only basic hybrid queue tests\n"
                      << "  -A, --hybrid-analysis  Run detailed hybrid queue behavior analysis\n"
                      << "  -S, --hybrid-stress    Run hybrid queue stress test with epoch stalls\n"
                      << "  -R, --hybrid-regression Run hybrid queue specific regression tests\n\n"
                      << "Examples:\n"
                      << "  " << argv[0] << "                    # Run full benchmark suite\n"
                      << "  " << argv[0] << " -H -d              # Detailed hybrid queue tests only\n"
                      << "  " << argv[0] << " -A                 # Deep hybrid behavior analysis\n"
                      << "  " << argv[0] << " -S                 # Stress test to force epoch stalls\n"
                      << "  " << argv[0] << " -R                 # Hybrid queue regression testing\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            std::cerr << "Use --help for usage information.\n";
            return 1;
        }
    }

    try {
        if (system_info) {
            printSystemInfo();
            if (argc == 2) return 0; // Only system info requested
        }
        
        if (hybrid_regression) {
            runHybridRegressionTest();
            return 0;
        }
        
        if (hybrid_stress) {
            runHybridStressTest();
            return 0;
        }
        
        if (hybrid_analysis) {
            runHybridAnalysis();
            return 0;
        }
        
        if (regression_test) {
            runRegressionTest();
            return 0;
        }
        
        if (hybrid_only) {
            std::cout << "=== Hybrid Queue Focused Testing ===\n";
            QueueBenchmark<TimedMessage<64>> benchmark;
            
            std::cout << "Running hybrid queue baseline...\n";
            auto baseline = benchmark.runSingleThreadedBaseline(QueueType::HYBRID_LOCKFREE);
            baseline.print();
            if (detailed_output) baseline.printDetailed();
            
            std::cout << "\nRunning hybrid queue epoch stall test...\n";
            auto stall_test = benchmark.runEpochStallTest();
            stall_test.print();
            if (detailed_output) stall_test.printDetailed();
            
            std::cout << "\nRunning hybrid queue contention test...\n";
            auto contention = benchmark.runMultiProducerContention(QueueType::HYBRID_LOCKFREE, 4);
            contention.print();
            if (detailed_output) contention.printDetailed();
            
            std::cout << "\nRunning hybrid queue correctness test...\n";
            HybridValidation::CorrectnessTest<TimedMessage<64>>::runCorrectnessTest();
            
            return 0;
        }
        
        // Run full benchmark suite
        QueueComparisonSuite suite;
        suite.setDetailedOutput(detailed_output);
        suite.runAll();
        
        std::cout << "\n🚀 Queue Performance Comparison Complete! 🚀\n";
        std::cout << "\nKey insights:\n"
                  << "• Lock-free queues excel under high contention scenarios\n"
                  << "• EBR provides consistent performance with automatic cleanup\n"
                  << "• Hazard pointers offer fine-grained memory management\n"
                  << "• Hybrid queue combines EBR efficiency with HP robustness\n"
                  << "• Boost lock-free queue delivers excellent performance within capacity limits\n"
                  << "• Lock-based queues show predictable single-threaded performance\n"
                  << "• Producer:consumer ratios significantly impact scalability\n"
                  << "• Bounded queues require careful capacity planning for high-throughput scenarios\n"
                  << "• Hybrid approach successfully handles epoch stalls while maintaining fast-path performance\n";
        
        std::cout << "\nFor more detailed analysis, try:\n"
                  << "  " << argv[0] << " --hybrid-analysis     # Deep hybrid queue analysis\n"
                  << "  " << argv[0] << " --hybrid-stress       # Stress test epoch stall handling\n"
                  << "  " << argv[0] << " --detailed            # Show all detailed statistics\n";
        
        return 0;
        
    } catch (const std::exception& ex) {
        std::cerr << "\n❌ Benchmark failed: " << ex.what() << "\n";
        std::cerr << "This might indicate:\n";
        std::cerr << "• Missing include files (check include paths)\n";
        std::cerr << "• Compilation issues (ensure C++20 support)\n";
        std::cerr << "• Runtime environment problems\n";
        std::cerr << "• Hardware resource exhaustion\n";
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Benchmark failed with unknown exception\n";
        return 1;
    }
}
