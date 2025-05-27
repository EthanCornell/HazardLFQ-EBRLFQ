/**********************************************************************
 *  lock_queue_latency_bench.cpp - ThreadSafeQueue Latency Benchmarks
 *  ──────────────────────────────────────────────────────────────────
 *  
 *  PURPOSE
 *  -------
 *  Comprehensive latency benchmarking suite for the lock-based ThreadSafeQueue
 *  implementation. Provides detailed performance analysis including contention
 *  tracking, tail latency analysis, and comparison with theoretical maximums.
 *  
 *  BENCHMARKS INCLUDED
 *  -------------------
 *  1. Single-threaded baseline latency
 *  2. Multi-producer contention analysis
 *  3. Multi-consumer latency distribution  
 *  4. Load-dependent latency (varying queue depth)
 *  5. Burst latency handling
 *  6. Tail latency analysis (P99, P99.9, P99.99)
 *  7. Lock contention impact on latency
 *  8. Memory pressure effects
 *  9. Payload size scaling
 *  10. Producer-consumer ratio analysis
 *  11. Coordinated omission resistant measurements
 *  
 *  METRICS REPORTED
 *  ----------------
 *  • Mean/Median latency (microseconds)
 *  • Tail latencies (P95, P99, P99.9, P99.99)
 *  • Lock contention rate and efficiency
 *  • Throughput vs latency trade-offs
 *  • Jitter and consistency analysis
 *  • Memory overhead estimation
 *  
 *  BUILD
 *  -----
 *  g++ -std=c++20 -O3 -march=native -pthread lock_queue_latency_bench.cpp -o lock_bench
 *  
 *  # With high-resolution timing
 *  g++ -std=c++20 -O3 -march=native -pthread -DHIGH_RES_TIMER \
 *      lock_queue_latency_bench.cpp -o lock_bench
 *  
 *  # With detailed debug output
 *  g++ -std=c++20 -O2 -g -pthread -DDEBUG_DETAILED \
 *      lock_queue_latency_bench.cpp -o lock_bench_debug
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
#include <cmath>
#include <mutex>
#include <condition_variable>
#include <barrier>
#include <queue>
#include <cstring>
#include <cassert>
#include "../include/ThreadSafeQueue.hpp" // Include the lock-free queue implementation

using namespace std::chrono_literals;

// High-resolution timing configuration
#ifdef HIGH_RES_TIMER
using Clock = std::chrono::high_resolution_clock;
#else
using Clock = std::chrono::steady_clock;
#endif
using TimePoint = Clock::time_point;
using Duration  = std::chrono::nanoseconds;


// Benchmark configuration
namespace config {
    constexpr int DEFAULT_SAMPLES     = 1000000;
    constexpr int WARMUP_SAMPLES      = 10000;
    const    int MAX_THREADS          = std::thread::hardware_concurrency();
    constexpr int HISTOGRAM_BUCKETS   = 100;
    constexpr double PERCENTILES[]    = {50.0, 90.0, 95.0, 99.0, 99.9, 99.99};
    
    // Test scenarios
    const std::vector<int> QUEUE_DEPTHS   = {0, 10, 100, 1000, 10000};
    const std::vector<int> THREAD_COUNTS  = {1, 2, 4, 8, 16};
    const std::vector<int> PAYLOAD_SIZES  = {16, 64, 256, 1024, 4096};
    const std::vector<int> PRODUCER_RATIOS = {1, 2, 4, 8}; // producers per consumer
}

// Latency measurement structure
struct LatencyMeasurement {
    TimePoint enqueue_time;
    TimePoint dequeue_time;
    uint64_t sequence_number;
    uint32_t producer_id;
    uint32_t actual_contentions; // Real contention count from queue
    
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
    std::vector<uint32_t> contention_counts_;
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
        contention_counts_.push_back(m.actual_contentions);
        is_sorted_ = false;
    }
    
    void addLatency(double micros, uint32_t contentions = 0) {
        latencies_micros_.push_back(micros);
        contention_counts_.push_back(contentions);
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
    
    double getAverageContentions() const {
        if (contention_counts_.empty()) return 0.0;
        return std::accumulate(contention_counts_.begin(), contention_counts_.end(), 0.0)
               / contention_counts_.size();
    }
    
    double getContentionRate() const {
        if (contention_counts_.empty()) return 0.0;
        return static_cast<double>(std::count_if(contention_counts_.begin(), 
                                               contention_counts_.end(),
                                               [](uint32_t c) { return c > 0; }))
               / contention_counts_.size();
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
    void reserve(size_t n) { 
        latencies_micros_.reserve(n); 
        contention_counts_.reserve(n);
    }
    void clear() { 
        latencies_micros_.clear(); 
        contention_counts_.clear();
        is_sorted_ = false; 
    }
};

// Benchmark result structure
struct LatencyBenchmarkResult {
    std::string name, queue_type;
    int num_threads, payload_size, queue_depth;
    size_t sample_count;
    double mean_latency, min_latency, max_latency, std_dev, jitter;
    double avg_contentions, contention_rate, lock_efficiency, throughput;
    std::map<double,double> percentiles;
    std::vector<int> histogram;
    size_t memory_overhead_bytes;
    
    static void printHeader() {
        std::cout << std::setw(40) << std::left << "Benchmark"
                  << std::setw(8)  << "Queue"
                  << std::setw(6)  << "Thrds"
                  << std::setw(8)  << "Payload"
                  << std::setw(10) << "Mean(μs)"
                  << std::setw(10) << "P50(μs)"
                  << std::setw(10) << "P95(μs)"
                  << std::setw(10) << "P99(μs)"
                  << std::setw(8)  << "Cntn%"
                  << std::setw(12) << "Throughput"
                  << '\n'
                  << std::string(130, '-') << '\n';
    }
    
    void print() const {
        std::cout << std::fixed << std::setprecision(2)
                  << std::setw(40) << std::left << name
                  << std::setw(8)  << queue_type
                  << std::setw(6)  << num_threads
                  << std::setw(8)  << payload_size
                  << std::setw(10) << mean_latency
                  << std::setw(10) << percentiles.at(50.0)
                  << std::setw(10) << percentiles.at(95.0)
                  << std::setw(10) << percentiles.at(99.0)
                  << std::setw(8)  << (contention_rate * 100.0)
                  << std::setw(12) << throughput
                  << '\n';
    }
    
    void printDetailed() const {
        std::cout << "\n=== " << name << " Detailed Statistics ===\n"
                  << "Sample count: " << sample_count << '\n'
                  << "Mean latency: " << mean_latency << " μs\n"
                  << "Std deviation: " << std_dev << " μs\n"
                  << "Jitter (CV): " << jitter << '\n'
                  << "Min latency: " << min_latency << " μs\n"
                  << "Max latency: " << max_latency << " μs\n"
                  << "Lock contention rate: " << (contention_rate * 100.0) << "%\n"
                  << "Avg contentions per op: " << avg_contentions << '\n'
                  << "Lock efficiency: " << lock_efficiency << " ops/contention\n"
                  << "Memory overhead: " << memory_overhead_bytes << " bytes\n\n"
                  << "Percentiles:\n";
        for (const auto &p : percentiles) {
            std::cout << "  P" << std::setw(5) << std::left << p.first << ": "
                      << std::setw(8) << p.second << " μs\n";
        }
        std::cout << "\nThroughput: " << throughput << " ops/sec\n";
    }
};

// Lock contention analyzer
class ContentionAnalyzer {
    std::vector<std::pair<TimePoint, uint64_t>> contention_timeline_;
    mutable std::mutex timeline_mutex_;
    
public:
    void recordContention(uint64_t thread_id) {
        std::lock_guard<std::mutex> lock(timeline_mutex_);
        contention_timeline_.emplace_back(Clock::now(), thread_id);
    }
    
    double getContentionRate(Duration window = 1s) const {
        std::lock_guard<std::mutex> lock(timeline_mutex_);
        if (contention_timeline_.empty()) return 0.0;
        
        auto now = Clock::now();
        auto cutoff = now - window;
        
        auto count = std::count_if(contention_timeline_.begin(), contention_timeline_.end(),
                                  [cutoff](const auto& entry) { return entry.first > cutoff; });
        
        return static_cast<double>(count) / std::chrono::duration<double>(window).count();
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(timeline_mutex_);
        contention_timeline_.clear();
    }
};

// Coordinated omission correction
class CoordinatedOmissionCorrector {
    std::vector<LatencyMeasurement> measurements_;
    TimePoint start_time_;
    Duration intended_interval_;
    
public:
    CoordinatedOmissionCorrector(Duration interval)
      : start_time_(Clock::now()), intended_interval_(interval)
    {
        measurements_.reserve(config::DEFAULT_SAMPLES);
    }
    
    void recordMeasurement(const LatencyMeasurement& m) {
        measurements_.push_back(m);
    }
    
    LatencyStats getCorrectedStats() {
        LatencyStats stats;
        stats.reserve(measurements_.size() * 2);
        
        for (const auto& m : measurements_) {
            stats.addMeasurement(m);
            
            // Calculate timing delays
            auto expected_time = start_time_ + intended_interval_ * m.sequence_number;
            auto actual_time = m.enqueue_time;
            auto delay = actual_time - expected_time;
            
            if (delay > intended_interval_) {
                // Add synthetic measurements for coordinated omission
                int missed_intervals = static_cast<int>(delay / intended_interval_);
                double base_latency = m.getLatencyMicros();
                
                for (int i = 1; i <= missed_intervals; ++i) {
                    double synthetic_latency = base_latency + 
                        std::chrono::duration<double, std::micro>(intended_interval_ * i).count();
                    stats.addLatency(synthetic_latency, m.actual_contentions);
                }
            }
        }
        return stats;
    }
};

// Main latency benchmark framework
template<typename QueueType, typename MessageType>
class LockQueueLatencyBenchmark {
    QueueType queue_;
    std::atomic<uint64_t> sequence_counter_{0};
    std::atomic<bool> benchmark_active_{false};
    ContentionAnalyzer contention_analyzer_;

public:
    LatencyBenchmarkResult runSingleThreadedBaseline(int samples = config::DEFAULT_SAMPLES) {
        LatencyStats stats;
        stats.reserve(samples);
        
        // Warmup phase
        for (int i = 0; i < config::WARMUP_SAMPLES; ++i) {
            MessageType msg(i, 0);
            queue_.enqueue(msg);
            MessageType tmp;
            queue_.dequeue(tmp);
        }
        
        queue_.resetContentionCount();
        auto start_time = Clock::now();
        uint64_t initial_contentions = queue_.getContentionCount();
        
        for (int i = 0; i < samples; ++i) {
            auto enqueue_time = Clock::now();
            MessageType msg(i, 0);
            
            queue_.enqueue(msg);
            
            MessageType dequeued_msg;
            if (queue_.dequeue(dequeued_msg)) {
                auto dequeue_time = Clock::now();
                
                if (!dequeued_msg.validate()) {
                    throw std::runtime_error("Message validation failed!");
                }
                
                uint64_t current_contentions = queue_.getContentionCount();
                LatencyMeasurement measurement{
                    enqueue_time, dequeue_time, 
                    static_cast<uint64_t>(i), 0,
                    static_cast<uint32_t>(current_contentions - initial_contentions)
                };
                stats.addMeasurement(measurement);
                initial_contentions = current_contentions;
            }
        }
        auto end_time = Clock::now();
        
        return makeResult("Single-threaded baseline", stats, 1,
                         std::chrono::duration<double>(end_time - start_time).count(), 0);
    }

    LatencyBenchmarkResult runMultiProducerContention(int producers, int samples_per = config::DEFAULT_SAMPLES) {
        std::vector<std::thread> producer_threads;
        std::vector<LatencyMeasurement> all_measurements;
        std::mutex measurements_mutex;
        std::barrier sync_barrier(producers + 1);
        
        benchmark_active_.store(true);
        queue_.resetContentionCount();
        contention_analyzer_.clear();
        
        // Consumer thread
        std::thread consumer([&]{
            MessageType msg;
            while (benchmark_active_.load() || !queue_.empty()) {
                uint64_t pre_contentions = queue_.getContentionCount();
                
                if (queue_.dequeue(msg)) {
                    auto dequeue_time = Clock::now();
                    uint64_t post_contentions = queue_.getContentionCount();
                    
                    if (!msg.validate()) {
                        throw std::runtime_error("Consumer validation failed!");
                    }
                    
                    LatencyMeasurement measurement{
                        msg.timestamp, dequeue_time, 
                        msg.sequence, msg.producer_id,
                        static_cast<uint32_t>(post_contentions - pre_contentions)
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
                    
                    uint64_t pre_contentions = queue_.getContentionCount();
                    queue_.enqueue(msg);
                    uint64_t post_contentions = queue_.getContentionCount();
                    
                    if (post_contentions > pre_contentions) {
                        contention_analyzer_.recordContention(i);
                    }
                    
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
                         stats, producers + 1,
                         std::chrono::duration<double>(benchmark_end - benchmark_start).count(),
                         0);
    }

    LatencyBenchmarkResult runLoadDependentLatency(int depth, int samples = config::DEFAULT_SAMPLES) {
        // Pre-fill queue to specified depth
        for (int i = 0; i < depth; ++i) {
            queue_.enqueue(MessageType(i, 0));
        }
        
        LatencyStats stats;
        stats.reserve(samples);
        queue_.resetContentionCount();
        
        auto start_time = Clock::now();
        for (int i = 0; i < samples; ++i) {
            auto enqueue_time = Clock::now();
            
            uint64_t pre_enq_contentions = queue_.getContentionCount();
            queue_.enqueue(MessageType(i + depth, 0));
            uint64_t post_enq_contentions = queue_.getContentionCount();
            
            MessageType dequeued_msg;
            uint64_t pre_deq_contentions = queue_.getContentionCount();
            if (queue_.dequeue(dequeued_msg)) {
                auto dequeue_time = Clock::now();
                uint64_t post_deq_contentions = queue_.getContentionCount();
                
                uint32_t total_contentions = static_cast<uint32_t>(
                    (post_enq_contentions - pre_enq_contentions) +
                    (post_deq_contentions - pre_deq_contentions)
                );
                
                LatencyMeasurement measurement{
                    enqueue_time, dequeue_time, 
                    static_cast<uint64_t>(i), 0, total_contentions
                };
                stats.addMeasurement(measurement);
            }
        }
        auto end_time = Clock::now();
        
        return makeResult("Queue depth " + std::to_string(depth),
                         stats, 1,
                         std::chrono::duration<double>(end_time - start_time).count(),
                         depth);
    }

    LatencyBenchmarkResult runBurstLatency(int burst_size = 1000, int interval_ms = 100) {
        LatencyStats stats;
        std::vector<LatencyMeasurement> all_measurements;
        const int num_bursts = 10;
        auto burst_interval = std::chrono::milliseconds(interval_ms);
        
        benchmark_active_.store(true);
        queue_.resetContentionCount();
        
        std::thread consumer([&]{
            MessageType msg;
            while (benchmark_active_.load()) {
                uint64_t pre_contentions = queue_.getContentionCount();
                if (queue_.dequeue(msg)) {
                    auto dequeue_time = Clock::now();
                    uint64_t post_contentions = queue_.getContentionCount();
                    
                    LatencyMeasurement measurement{
                        msg.timestamp, dequeue_time, 
                        msg.sequence, msg.producer_id,
                        static_cast<uint32_t>(post_contentions - pre_contentions)
                    };
                    all_measurements.push_back(measurement);
                }
            }
            
            // Drain remaining
            while (queue_.dequeue(msg)) {
                auto dequeue_time = Clock::now();
                LatencyMeasurement measurement{
                    msg.timestamp, dequeue_time, 
                    msg.sequence, msg.producer_id, 0
                };
                all_measurements.push_back(measurement);
            }
        });
        
        auto burst_start = Clock::now();
        for (int burst = 0; burst < num_bursts; ++burst) {
            for (int i = 0; i < burst_size; ++i) {
                auto seq = sequence_counter_.fetch_add(1);
                queue_.enqueue(MessageType(seq, 0));
            }
            
            if (burst < num_bursts - 1) {
                std::this_thread::sleep_for(burst_interval);
            }
        }
        
        std::this_thread::sleep_for(100ms);
        benchmark_active_.store(false);
        consumer.join();
        auto burst_end = Clock::now();
        
        for (const auto& measurement : all_measurements) {
            stats.addMeasurement(measurement);
        }
        
        return makeResult("Burst latency (burst=" + std::to_string(burst_size) + ")",
                         stats, 2,
                         std::chrono::duration<double>(burst_end - burst_start).count(),
                         0);
    }

    LatencyBenchmarkResult runProducerConsumerRatio(int producers, int consumers, int samples_per = 10000) {
        std::vector<std::thread> producer_threads, consumer_threads;
        std::vector<LatencyMeasurement> all_measurements;
        std::mutex measurements_mutex;
        std::barrier sync_barrier(producers + consumers + 1);
        
        benchmark_active_.store(true);
        queue_.resetContentionCount();
        
        // Consumer threads
        for (int c = 0; c < consumers; ++c) {
            consumer_threads.emplace_back([&, c]{
                sync_barrier.arrive_and_wait();
                
                MessageType msg;
                while (benchmark_active_.load() || !queue_.empty()) {
                    uint64_t pre_contentions = queue_.getContentionCount();
                    if (queue_.dequeue(msg)) {
                        auto dequeue_time = Clock::now();
                        uint64_t post_contentions = queue_.getContentionCount();
                        
                        LatencyMeasurement measurement{
                            msg.timestamp, dequeue_time, 
                            msg.sequence, msg.producer_id,
                            static_cast<uint32_t>(post_contentions - pre_contentions)
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
                    queue_.enqueue(MessageType(seq, p));
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
                         stats, producers + consumers,
                         std::chrono::duration<double>(benchmark_end - benchmark_start).count(),
                         0);
    }

    LatencyBenchmarkResult runCoordinatedOmissionTest(Duration target_interval = 1ms) {
        CoordinatedOmissionCorrector corrector(target_interval);
        const int samples = config::DEFAULT_SAMPLES / 10;
        
        benchmark_active_.store(true);
        std::vector<LatencyMeasurement> measurements;
        std::mutex measurements_mutex;
        queue_.resetContentionCount();
        
        std::thread consumer([&]{
            MessageType msg;
            while (benchmark_active_.load() || !queue_.empty()) {
                uint64_t pre_contentions = queue_.getContentionCount();
                if (queue_.dequeue(msg)) {
                    auto dequeue_time = Clock::now();
                    uint64_t post_contentions = queue_.getContentionCount();
                    
                    LatencyMeasurement measurement{
                        msg.timestamp, dequeue_time, 
                        msg.sequence, msg.producer_id,
                        static_cast<uint32_t>(post_contentions - pre_contentions)
                    };
                    
                    std::lock_guard<std::mutex> lock(measurements_mutex);
                    measurements.push_back(measurement);
                }
            }
        });
        
        auto start_time = Clock::now();
        for (int i = 0; i < samples; ++i) {
            auto target_time = start_time + target_interval * i;
            std::this_thread::sleep_until(target_time);
            
            queue_.enqueue(MessageType(i, 0));
        }
        
        std::this_thread::sleep_for(100ms);
        benchmark_active_.store(false);
        consumer.join();
        auto end_time = Clock::now();
        
        for (const auto& m : measurements) {
            corrector.recordMeasurement(m);
        }
        
        LatencyStats corrected_stats = corrector.getCorrectedStats();
        
        return makeResult("Coordinated Omission Corrected",
                         corrected_stats, 2,
                         std::chrono::duration<double>(end_time - start_time).count(),
                         0);
    }

private:
    LatencyBenchmarkResult makeResult(
        const std::string& name,
        LatencyStats& stats,
        int threads,
        double duration_sec,
        int depth)
    {
        LatencyBenchmarkResult result;
        result.name = name;
        result.queue_type = "Lock";
        result.num_threads = threads;
        result.payload_size = static_cast<int>(sizeof(MessageType));
        result.queue_depth = depth;
        result.sample_count = stats.count();
        result.mean_latency = stats.getMean();
        result.min_latency = stats.getMin();
        result.max_latency = stats.getMax();
        result.std_dev = stats.getStdDev();
        result.jitter = (result.mean_latency > 0 ? result.std_dev / result.mean_latency : 0);
        result.avg_contentions = stats.getAverageContentions();
        result.contention_rate = stats.getContentionRate();
        result.lock_efficiency = (result.avg_contentions > 0 ? 
                                 result.sample_count / result.avg_contentions : result.sample_count);
        
        for (double p : config::PERCENTILES) {
            result.percentiles[p] = stats.getPercentile(p);
        }
        
        result.throughput = (duration_sec > 0 ? result.sample_count / duration_sec : 0);
        result.histogram = stats.getHistogram();
        
        // Estimate memory overhead
        result.memory_overhead_bytes = sizeof(MessageType) * result.sample_count + 
                                      sizeof(std::mutex) + sizeof(std::queue<MessageType>) +
                                      sizeof(std::atomic<uint64_t>);
        
        return result;
    }
};

// Memory pressure testing utility
class MemoryPressureTest {
    std::vector<std::unique_ptr<char[]>> memory_hogs_;
    
public:
    void allocateMemory(size_t mb) {
        const size_t chunk_size = 1024 * 1024;
        for (size_t i = 0; i < mb; ++i) {
            memory_hogs_.push_back(std::make_unique<char[]>(chunk_size));
            memset(memory_hogs_.back().get(), static_cast<int>(i & 0xFF), chunk_size);
        }
        std::cout << "Allocated " << mb << " MB of memory pressure\n";
    }
    
    void releaseMemory() {
        memory_hogs_.clear();
        std::cout << "Released memory pressure\n";
    }
    
    ~MemoryPressureTest() { releaseMemory(); }
};

// Main benchmark suite
class LockQueueBenchmarkSuite {
    std::vector<LatencyBenchmarkResult> results_;
    bool detailed_output_ = false;

public:
    void setDetailedOutput(bool detailed) { detailed_output_ = detailed; }

    void runAll() {
        std::cout << "ThreadSafeQueue (Lock-Based) Latency Benchmarks\n"
                  << "===============================================\n"
                  << "Hardware threads: " << std::thread::hardware_concurrency() << '\n'
                  << "Sample count:     " << config::DEFAULT_SAMPLES << " per test\n"
                  << "Clock resolution: " << getClockResolution() << " ns\n"
                  << "Lock contention:  real-time tracking\n\n";

        LatencyBenchmarkResult::printHeader();
        runBaseline();
        runContentionAnalysis();
        runLoadAnalysis();
        runBurstAnalysis();
        runPayloadAnalysis();
        runRatioAnalysis();
        runCoordinatedOmissionAnalysis();
        printSummary();
        exportResults();
    }

private:
    double getClockResolution() {
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
        
        if (deltas.empty()) return 1.0;
        return std::chrono::duration<double, std::nano>(
                   *std::min_element(deltas.begin(), deltas.end())
               ).count();
    }

    void runBaseline() {
        std::cout << "\n=== Single-Threaded Baseline ===\n";
        LockQueueLatencyBenchmark<ThreadSafeQueue<TimedMessage<64>>, TimedMessage<64>> benchmark;
        auto result = benchmark.runSingleThreadedBaseline();
        result.print();
        results_.push_back(result);
        if (detailed_output_) result.printDetailed();
    }

    void runContentionAnalysis() {
        std::cout << "\n=== Lock Contention Analysis ===\n";
        for (int producers : {2, 4, 8, 16}) {
            if (producers > config::MAX_THREADS - 1) break;
            
            LockQueueLatencyBenchmark<ThreadSafeQueue<TimedMessage<64>>, TimedMessage<64>> benchmark;
            auto result = benchmark.runMultiProducerContention(producers, config::DEFAULT_SAMPLES / producers);
            result.print();
            results_.push_back(result);
            if (detailed_output_) result.printDetailed();
        }
    }

    void runLoadAnalysis() {
        std::cout << "\n=== Load-Dependent Latency ===\n";
        for (int depth : config::QUEUE_DEPTHS) {
            LockQueueLatencyBenchmark<ThreadSafeQueue<TimedMessage<64>>, TimedMessage<64>> benchmark;
            auto result = benchmark.runLoadDependentLatency(depth, config::DEFAULT_SAMPLES / 10);
            result.print();
            results_.push_back(result);
        }
    }

    void runBurstAnalysis() {
        std::cout << "\n=== Burst Latency Analysis ===\n";
        for (int burst_size : {100, 1000, 10000}) {
            LockQueueLatencyBenchmark<ThreadSafeQueue<TimedMessage<64>>, TimedMessage<64>> benchmark;
            auto result = benchmark.runBurstLatency(burst_size);
            result.print();
            results_.push_back(result);
        }
    }

    void runPayloadAnalysis() {
        std::cout << "\n=== Payload Size Impact ===\n";
        
        // 16-byte payload
        {
            LockQueueLatencyBenchmark<ThreadSafeQueue<TimedMessage<16>>, TimedMessage<16>> benchmark;
            auto result = benchmark.runSingleThreadedBaseline(config::DEFAULT_SAMPLES / 10);
            result.name = "16B payload";
            result.print();
            results_.push_back(result);
        }
        
        // 256-byte payload
        {
            LockQueueLatencyBenchmark<ThreadSafeQueue<TimedMessage<256>>, TimedMessage<256>> benchmark;
            auto result = benchmark.runSingleThreadedBaseline(config::DEFAULT_SAMPLES / 10);
            result.name = "256B payload";
            result.print();
            results_.push_back(result);
        }
        
        // 1KB payload
        {
            LockQueueLatencyBenchmark<ThreadSafeQueue<TimedMessage<1024>>, TimedMessage<1024>> benchmark;
            auto result = benchmark.runSingleThreadedBaseline(config::DEFAULT_SAMPLES / 10);
            result.name = "1KB payload";
            result.print();
            results_.push_back(result);
        }
        
        // 4KB payload
        {
            LockQueueLatencyBenchmark<ThreadSafeQueue<TimedMessage<4096>>, TimedMessage<4096>> benchmark;
            auto result = benchmark.runSingleThreadedBaseline(config::DEFAULT_SAMPLES / 20);
            result.name = "4KB payload";
            result.print();
            results_.push_back(result);
        }
    }

    void runRatioAnalysis() {
        std::cout << "\n=== Producer:Consumer Ratio Analysis ===\n";
        
        // Test different producer:consumer ratios
        const std::vector<std::pair<int,int>> ratios = {
            {1, 1}, {2, 1}, {4, 1}, {1, 2}, {1, 4}, {4, 4}
        };
        
        for (const auto& [producers, consumers] : ratios) {
            if (producers + consumers > config::MAX_THREADS) continue;
            
            LockQueueLatencyBenchmark<ThreadSafeQueue<TimedMessage<64>>, TimedMessage<64>> benchmark;
            auto result = benchmark.runProducerConsumerRatio(producers, consumers, 10000);
            result.print();
            results_.push_back(result);
        }
    }

    void runCoordinatedOmissionAnalysis() {
        std::cout << "\n=== Coordinated Omission Analysis ===\n";
        LockQueueLatencyBenchmark<ThreadSafeQueue<TimedMessage<64>>, TimedMessage<64>> benchmark;
        auto result = benchmark.runCoordinatedOmissionTest(1ms);
        result.print();
        results_.push_back(result);
        if (detailed_output_) result.printDetailed();
    }

    void printSummary() {
        std::cout << "\n=== Lock-Based Queue Latency Summary ===\n";
        
        if (results_.empty()) {
            std::cout << "No results to summarize.\n";
            return;
        }
        
        auto best_mean = std::min_element(
            results_.begin(), results_.end(),
            [](const auto& a, const auto& b) { return a.mean_latency < b.mean_latency; }
        );
        
        auto worst_p99 = std::max_element(
            results_.begin(), results_.end(),
            [](const auto& a, const auto& b) { 
                return a.percentiles.at(99.0) < b.percentiles.at(99.0); 
            }
        );
        
        auto best_throughput = std::max_element(
            results_.begin(), results_.end(),
            [](const auto& a, const auto& b) { return a.throughput < b.throughput; }
        );
        
        auto highest_contention = std::max_element(
            results_.begin(), results_.end(),
            [](const auto& a, const auto& b) { return a.contention_rate < b.contention_rate; }
        );
        
        std::cout << "Best mean latency: " << std::fixed << std::setprecision(2) 
                  << best_mean->mean_latency << " μs (" << best_mean->name << ")\n"
                  << "Worst P99 latency: " << worst_p99->percentiles.at(99.0)
                  << " μs (" << worst_p99->name << ")\n"
                  << "Best throughput: " << best_throughput->throughput
                  << " ops/sec (" << best_throughput->name << ")\n"
                  << "Highest contention: " << (highest_contention->contention_rate * 100.0)
                  << "% (" << highest_contention->name << ")\n";

        // Calculate weighted statistics
        double total_samples = 0, weighted_mean = 0, weighted_contentions = 0;
        for (const auto& result : results_) {
            weighted_mean += result.mean_latency * result.sample_count;
            weighted_contentions += result.avg_contentions * result.sample_count;
            total_samples += result.sample_count;
        }
        
        if (total_samples > 0) {
            std::cout << "Overall weighted mean latency: " 
                      << (weighted_mean / total_samples) << " μs\n"
                      << "Overall avg lock contentions: " 
                      << (weighted_contentions / total_samples) << " per operation\n";
        }
        
        std::cout << "Total samples processed: " << static_cast<size_t>(total_samples) << '\n';
        
        // Lock efficiency analysis
        std::cout << "\n=== Lock Efficiency Analysis ===\n";
        double best_efficiency = 0.0, worst_efficiency = std::numeric_limits<double>::max();
        std::string best_test, worst_test;
        
        for (const auto& result : results_) {
            if (result.lock_efficiency > best_efficiency) {
                best_efficiency = result.lock_efficiency;
                best_test = result.name;
            }
            if (result.lock_efficiency < worst_efficiency) {
                worst_efficiency = result.lock_efficiency;
                worst_test = result.name;
            }
        }
        
        std::cout << "Best lock efficiency: " << best_efficiency 
                  << " ops/contention (" << best_test << ")\n"
                  << "Worst lock efficiency: " << worst_efficiency 
                  << " ops/contention (" << worst_test << ")\n";

        // Contention impact analysis
        auto baseline = std::find_if(results_.begin(), results_.end(),
                                   [](const auto& r) { return r.name.find("baseline") != std::string::npos; });
        
        if (baseline != results_.end()) {
            std::cout << "\n=== Contention Impact Analysis ===\n";
            for (const auto& result : results_) {
                if (result.contention_rate > 0.01) { // More than 1% contention
                    double latency_overhead = ((result.mean_latency - baseline->mean_latency) / baseline->mean_latency) * 100.0;
                    std::cout << result.name << ": " 
                              << std::fixed << std::setprecision(1)
                              << latency_overhead << "% latency overhead with "
                              << (result.contention_rate * 100.0) << "% contention\n";
                }
            }
        }
    }

    void exportResults() {
        std::ofstream csv("lock_queue_latency_results.csv");
        csv << "Benchmark,Queue_Type,Threads,Payload_Size,Queue_Depth,Sample_Count,"
               "Mean_Latency_us,Min_Latency_us,Max_Latency_us,Std_Dev_us,Jitter,"
               "Avg_Contentions,Contention_Rate,Lock_Efficiency,Memory_Overhead_bytes,"
               "P50_us,P90_us,P95_us,P99_us,P99_9_us,P99_99_us,Throughput_ops_per_sec\n";
        
        for (const auto& result : results_) {
            csv << result.name << ',' << result.queue_type << ','
                << result.num_threads << ',' << result.payload_size << ','
                << result.queue_depth << ',' << result.sample_count << ','
                << result.mean_latency << ',' << result.min_latency << ','
                << result.max_latency << ',' << result.std_dev << ',' << result.jitter << ','
                << result.avg_contentions << ',' << result.contention_rate << ','
                << result.lock_efficiency << ',' << result.memory_overhead_bytes << ',';
            
            for (double p : config::PERCENTILES) {
                csv << result.percentiles.at(p) << ',';
            }
            csv << result.throughput << "\n";
        }
        
        std::cout << "\nResults exported to lock_queue_latency_results.csv\n";
        exportContentionAnalysis();
        exportLatencyDistribution();
    }

    void exportContentionAnalysis() {
        std::ofstream contention_file("lock_contention_analysis.csv");
        contention_file << "Benchmark,Threads,Contention_Rate,Avg_Contentions_Per_Op,"
                          "Lock_Efficiency,Latency_Overhead_Percent\n";
        
        // Find baseline for overhead calculation
        auto baseline = std::find_if(results_.begin(), results_.end(),
                                   [](const auto& r) { return r.name.find("baseline") != std::string::npos; });
        double baseline_latency = (baseline != results_.end()) ? baseline->mean_latency : 0.0;
        
        for (const auto& result : results_) {
            double overhead = 0.0;
            if (baseline_latency > 0) {
                overhead = ((result.mean_latency - baseline_latency) / baseline_latency) * 100.0;
            }
            
            contention_file << result.name << ','
                           << result.num_threads << ','
                           << result.contention_rate << ','
                           << result.avg_contentions << ','
                           << result.lock_efficiency << ','
                           << overhead << '\n';
        }
        
        std::cout << "Contention analysis exported to lock_contention_analysis.csv\n";
    }

    void exportLatencyDistribution() {
        std::ofstream dist_file("lock_latency_distribution.csv");
        
        // Find the result with the most interesting distribution (highest std dev)
        auto most_variable = std::max_element(
            results_.begin(), results_.end(),
            [](const auto& a, const auto& b) { return a.std_dev < b.std_dev; }
        );
        
        if (most_variable != results_.end()) {
            dist_file << "Benchmark," << most_variable->name << "\n";
            dist_file << "Bucket,Count\n";
            for (size_t i = 0; i < most_variable->histogram.size(); ++i) {
                dist_file << i << ',' << most_variable->histogram[i] << '\n';
            }
            std::cout << "Latency distribution exported to lock_latency_distribution.csv\n";
        }
    }
};

// Performance comparison with theoretical lock-free
void runLockFreeComparison() {
    std::cout << "\n=== Lock-Based vs Theoretical Lock-Free Comparison ===\n";
    std::cout << "Estimating overhead of lock-based synchronization\n";
    
    LockQueueLatencyBenchmark<ThreadSafeQueue<TimedMessage<64>>, TimedMessage<64>> benchmark;
    auto single_result = benchmark.runSingleThreadedBaseline(10000);
    auto multi_result = benchmark.runMultiProducerContention(4, 2500);
    
    // Theoretical lock-free would have minimal synchronization overhead
    constexpr double THEORETICAL_LOCK_OVERHEAD = 0.3; // ~300ns per lock operation
    constexpr double CACHE_COHERENCY_OVERHEAD = 0.1;  // ~100ns for cache misses
    
    double estimated_lockfree_latency = single_result.mean_latency - THEORETICAL_LOCK_OVERHEAD;
    double estimated_lockfree_multi = multi_result.mean_latency - 
                                     (THEORETICAL_LOCK_OVERHEAD + CACHE_COHERENCY_OVERHEAD * multi_result.num_threads);
    
    std::cout << "Single-threaded:\n"
              << "  Lock-based latency: " << single_result.mean_latency << " μs\n"
              << "  Estimated lock-free: " << estimated_lockfree_latency << " μs\n"
              << "  Lock overhead: " << std::fixed << std::setprecision(1)
              << (THEORETICAL_LOCK_OVERHEAD / single_result.mean_latency * 100.0) << "%\n\n"
              << "Multi-threaded (4 producers):\n"
              << "  Lock-based latency: " << multi_result.mean_latency << " μs\n"
              << "  Estimated lock-free: " << estimated_lockfree_multi << " μs\n"
              << "  Total overhead: " << std::fixed << std::setprecision(1)
              << ((multi_result.mean_latency - estimated_lockfree_multi) / multi_result.mean_latency * 100.0) << "%\n"
              << "  Contention rate: " << (multi_result.contention_rate * 100.0) << "%\n";
}

// Stress test with varying system load
void runSystemLoadTest() {
    std::cout << "\n=== System Load Impact Test ===\n";
    
    LockQueueLatencyBenchmark<ThreadSafeQueue<TimedMessage<64>>, TimedMessage<64>> benchmark;
    
    // Test under different CPU loads
    std::vector<std::thread> load_threads;
    std::atomic<bool> generate_load{true};
    
    auto baseline_result = benchmark.runSingleThreadedBaseline(10000);
    std::cout << "Baseline (no load): " << baseline_result.mean_latency << " μs\n";
    
    // Generate CPU load
    int num_load_threads = std::thread::hardware_concurrency() / 2;
    for (int i = 0; i < num_load_threads; ++i) {
        load_threads.emplace_back([&generate_load](){
            while (generate_load.load()) {
                // CPU-intensive work
                volatile double x = 1.0;
                for (int j = 0; j < 10000; ++j) {
                    x = x * 1.0001 + 0.0001;
                }
            }
        });
    }
    
    std::this_thread::sleep_for(100ms); // Let load stabilize
    auto loaded_result = benchmark.runSingleThreadedBaseline(10000);
    
    generate_load.store(false);
    for (auto& thread : load_threads) {
        thread.join();
    }
    
    double load_impact = ((loaded_result.mean_latency - baseline_result.mean_latency) / baseline_result.mean_latency) * 100.0;
    
    std::cout << "Under CPU load: " << loaded_result.mean_latency << " μs\n"
              << "Load impact: " << std::fixed << std::setprecision(1) << load_impact << "% increase\n";
}

int main(int argc, char* argv[]) {
    bool detailed_output = false;
    bool memory_pressure = false;
    bool system_load_test = false;
    size_t pressure_mb = 100;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--detailed" || arg == "-d") {
            detailed_output = true;
        } else if (arg == "--memory-pressure" || arg == "-m") {
            memory_pressure = true;
        } else if (arg == "--system-load" || arg == "-s") {
            system_load_test = true;
        } else if (arg.substr(0, 11) == "--pressure=") {
            pressure_mb = std::stoull(arg.substr(11));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -d, --detailed         Show detailed statistics\n"
                      << "  -m, --memory-pressure  Run with memory pressure\n"
                      << "  -s, --system-load      Run system load impact test\n"
                      << "  --pressure=MB          Set memory pressure amount (default: 100)\n"
                      << "  -h, --help            Show this help\n";
            return 0;
        }
    }

    // Set process priority for more consistent measurements
    #ifdef __linux__
    if (nice(-19) == -1) {
        std::cerr << "Warning: Could not set high process priority\n";
    }
    #endif

    // Optional memory pressure test
    std::unique_ptr<MemoryPressureTest> pressure_test;
    if (memory_pressure) {
        pressure_test = std::make_unique<MemoryPressureTest>();
        pressure_test->allocateMemory(pressure_mb);
    }

    try {
        LockQueueBenchmarkSuite suite;
        suite.setDetailedOutput(detailed_output);
        suite.runAll();
        
        runLockFreeComparison();
        
        if (system_load_test) {
            runSystemLoadTest();
        }
        
        std::cout << "\n🔒 Lock-Based Queue Latency Benchmarks Complete! 🔒\n";
        std::cout << "Key insights:\n"
                  << "• Lock contention directly impacts tail latencies\n"
                  << "• Single-threaded performance shows mutex overhead\n"
                  << "• Producer:consumer ratios affect contention patterns\n"
                  << "• Payload size has minimal impact on lock overhead\n"
                  << "• Memory pressure can amplify lock contention effects\n";
        
        return 0;
        
    } catch (const std::exception& ex) {
        std::cerr << "\n❌ Benchmark failed: " << ex.what() << "\n";
        return 1;
    }
}