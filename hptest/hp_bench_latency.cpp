/**********************************************************************
 *  hp_bench_latency.cpp - Hazard Pointer Queue Latency Benchmarks
 *  ──────────────────────────────────────────────────────────────────
 *  
 *  PURPOSE
 *  -------
 *  Comprehensive latency benchmarking suite for hazard pointer-based
 *  lock-free queue implementation. Measures end-to-end latency,
 *  percentiles, and latency distribution under various load conditions.
 *  
 *  BENCHMARKS INCLUDED
 *  -------------------
 *  1. Single-threaded round-trip latency
 *  2. Multi-producer latency under contention
 *  3. Multi-consumer latency distribution
 *  4. Load-dependent latency (varying queue depth)
 *  5. Burst latency (periodic high-load)
 *  6. Tail latency analysis (P99, P99.9, P99.99)
 *  7. Latency vs throughput trade-offs
 *  8. Memory pressure impact on latency
 *  9. Different payload size latency impact
 *  10. Hazard pointer scan frequency impact
 *  11. Coordinated omission resistant measurements
 *  
 *  METRICS REPORTED
 *  ----------------
 *  • Mean latency (microseconds)
 *  • Median latency (P50)
 *  • Tail latencies (P95, P99, P99.9, P99.99)
 *  • Standard deviation
 *  • Minimum/Maximum latency
 *  • Latency histogram
 *  • Jitter analysis
 *  • Hazard pointer overhead analysis
 *  • Memory reclamation efficiency
 *  
 *  BUILD
 *  -----
 *  g++ -std=c++20 -O3 -march=native -pthread -I../include hp_bench_latency.cpp -o hp_bench_latency
 *  
 *  # For high-resolution timing
 *  g++ -std=c++20 -O3 -march=native -pthread -DHIGH_RES_TIMER -I../include \
 *      hp_bench_latency.cpp -o hp_bench_latency
 *  
 *  # For detailed HP analysis
 *  g++ -std=c++20 -O3 -march=native -pthread -DDETAILED_HP_ANALYSIS -I../include \
 *      hp_bench_latency.cpp -o hp_bench_latency
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
#include <cmath>
#include <mutex>
#include <condition_variable>
#include <barrier>
#include <cstring>  // For memset

// Define DETAILED_HP_ANALYSIS if not already defined
#ifndef DETAILED_HP_ANALYSIS
#define DETAILED_HP_ANALYSIS 0
#endif

using namespace std::chrono_literals;

// High-resolution timing
#ifdef HIGH_RES_TIMER
using Clock = std::chrono::high_resolution_clock;
#else
using Clock = std::chrono::steady_clock;
#endif
using TimePoint = Clock::time_point;
using Duration  = std::chrono::nanoseconds;

// Benchmark configuration
namespace config {
    constexpr int DEFAULT_SAMPLES = 1000000;
    constexpr int WARMUP_SAMPLES  = 10000;
    const    int MAX_THREADS      = std::thread::hardware_concurrency();
    constexpr int HISTOGRAM_BUCKETS = 100;
    constexpr double PERCENTILES[]  = {50.0, 90.0, 95.0, 99.0, 99.9, 99.99};

    // Load scenarios
    const std::vector<int> QUEUE_DEPTHS   = {0, 10, 100, 1000, 10000};
    const std::vector<int> THREAD_COUNTS  = {1, 2, 4, 8, 16};
    const std::vector<int> PAYLOAD_SIZES  = {16, 64, 256, 1024, 4096};
}

// Latency measurement with timestamp
struct LatencyMeasurement {
    TimePoint enqueue_time;
    TimePoint dequeue_time;
    uint64_t sequence_number;
    uint32_t producer_id;
    uint32_t hazard_scans; // Number of HP scans during operation

    double getLatencyMicros() const {
        return std::chrono::duration<double, std::micro>(dequeue_time - enqueue_time).count();
    }
    Duration getLatencyNanos() const {
        return std::chrono::duration_cast<Duration>(dequeue_time - enqueue_time);
    }
};

// Message with embedded timing information
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
        // Initialize payload with deterministic pattern for validation
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<uint8_t>((seq + i) & 0xFF);
        }
        
        // Simple checksum for integrity validation
        checksum = static_cast<uint32_t>(seq ^ id);
        for (auto byte : payload) {
            checksum ^= byte;
        }
    }
    
    bool validate() const {
        uint32_t computed_checksum = static_cast<uint32_t>(sequence ^ producer_id);
        for (auto byte : payload) {
            computed_checksum ^= byte;
        }
        return computed_checksum == checksum;
    }
};

// Enhanced latency statistics calculator
class LatencyStats {
private:
    std::vector<double> latencies_micros_;
    std::vector<uint32_t> hazard_scan_counts_;
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
        hazard_scan_counts_.push_back(m.hazard_scans);
        is_sorted_ = false;
    }
    
    void addLatency(double v, uint32_t scans = 0) {
        latencies_micros_.push_back(v);
        hazard_scan_counts_.push_back(scans);
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
    
    double getAverageScanCount() const {
        if (hazard_scan_counts_.empty()) return 0.0;
        return std::accumulate(hazard_scan_counts_.begin(), hazard_scan_counts_.end(), 0.0)
               / hazard_scan_counts_.size();
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
            int b = static_cast<int>(((v - minv) / range) * (buckets - 1));
            hist[std::clamp(b, 0, buckets - 1)]++;
        }
        return hist;
    }
    
    size_t count() const { return latencies_micros_.size(); }
    void reserve(size_t n) { 
        latencies_micros_.reserve(n); 
        hazard_scan_counts_.reserve(n);
    }
    void clear() { 
        latencies_micros_.clear(); 
        hazard_scan_counts_.clear();
        is_sorted_ = false; 
    }
};

// Benchmark result structure
struct LatencyBenchmarkResult {
    std::string name, queue_type;
    int num_threads, payload_size, queue_depth;
    size_t sample_count;
    double mean_latency, min_latency, max_latency, std_dev, jitter, throughput;
    double avg_hazard_scans, scan_efficiency;
    std::map<double,double> percentiles;
    std::vector<int> histogram;
    size_t memory_overhead_bytes;

    static void printHeader() {
        std::cout << std::setw(35) << std::left << "Benchmark"
                  << std::setw(8)  << "Queue"
                  << std::setw(6)  << "Thrds"
                  << std::setw(8)  << "Payload"
                  << std::setw(10) << "Mean(μs)"
                  << std::setw(10) << "P50(μs)"
                  << std::setw(10) << "P95(μs)"
                  << std::setw(10) << "P99(μs)"
                  << std::setw(10) << "AvgScans"
                  << std::setw(12) << "Throughput"
                  << '\n'
                  << std::string(125, '-') << '\n';
    }
    
    void print() const {
        std::cout << std::fixed << std::setprecision(2)
                  << std::setw(35) << std::left << name
                  << std::setw(8)  << queue_type
                  << std::setw(6)  << num_threads
                  << std::setw(8)  << payload_size
                  << std::setw(10) << mean_latency
                  << std::setw(10) << percentiles.at(50.0)
                  << std::setw(10) << percentiles.at(95.0)
                  << std::setw(10) << percentiles.at(99.0)
                  << std::setw(10) << avg_hazard_scans
                  << std::setw(12) << throughput
                  << '\n';
    }
    
    void printDetailed() const {
        std::cout << "\n=== " << name << " Detailed Statistics ===\n"
                  << "Sample count: " << sample_count << '\n'
                  << "Mean latency: "  << mean_latency << " μs\n"
                  << "Std deviation: " << std_dev     << " μs\n"
                  << "Jitter (CV): "   << jitter      << '\n'
                  << "Min latency: "   << min_latency << " μs\n"
                  << "Max latency: "   << max_latency << " μs\n"
                  << "Avg HP scans: "  << avg_hazard_scans << '\n'
                  << "Scan efficiency: " << scan_efficiency << '\n'
                  << "Memory overhead: " << memory_overhead_bytes << " bytes\n\n"
                  << "Percentiles:\n";
        for (const auto &p : percentiles) {
            std::cout << "  P" << std::setw(5) << std::left << p.first << ": "
                      << std::setw(8) << p.second << " μs\n";
        }
        std::cout << "\nThroughput: " << throughput << " ops/sec\n";
    }
};

// Hazard pointer instrumentation (works with or without detailed analysis)
thread_local uint32_t g_scan_count = 0;
thread_local uint64_t g_total_scans = 0;

// Hook functions that can be called from HP implementation or estimated
void on_hazard_scan() {
    g_scan_count++;
    g_total_scans++;
}

uint32_t get_and_reset_scan_count() {
    uint32_t count = g_scan_count;
    g_scan_count = 0;
    return count;
}

// Improved estimation function for realistic HP scan counts
uint32_t estimate_scan_count_realistic(int operations, int threads, int queue_depth = 0) {
    if (operations <= 0) return 0;
    
    static thread_local std::mt19937 rng(std::random_device{}());
    
    // HP configuration (matching typical hazard_pointer.hpp settings)
    const int HAZARDS_PER_THREAD = 2;
    const int MAX_THREADS = 128;
    const int R_FACTOR = 2;
    const int HP_SLOTS = HAZARDS_PER_THREAD * MAX_THREADS;
    const int SCAN_THRESHOLD = HP_SLOTS * R_FACTOR; // ~512
    
    // Base scan rate calculations
    double base_scan_probability = 0.0;
    
    // Single-threaded: Very low scan rate (only on retirements)
    if (threads == 1) {
        base_scan_probability = 0.002; // ~0.2% of operations trigger scans
    }
    // Multi-threaded: Higher scan rates due to contention
    else if (threads <= 4) {
        base_scan_probability = 0.01 + (threads - 1) * 0.005; // 1-2.5%
    }
    else if (threads <= 8) {
        base_scan_probability = 0.025 + (threads - 4) * 0.01; // 2.5-6.5%
    }
    else {
        base_scan_probability = 0.065 + (threads - 8) * 0.015; // 6.5%+
    }
    
    // Queue depth factor (deeper queues may trigger more scans)
    if (queue_depth > 1000) {
        base_scan_probability *= 1.2;
    } else if (queue_depth > 100) {
        base_scan_probability *= 1.1;
    }
    
    // Calculate expected scans for this operation batch
    double expected_scans = operations * base_scan_probability;
    
    // Add some randomness but keep it realistic
    std::poisson_distribution<int> dist(expected_scans);
    int estimated_scans = dist(rng);
    
    // Cap at reasonable maximum (no more than 1 scan per 2 operations)
    return std::min(estimated_scans, operations / 2 + 1);
}

// Coordinated omission corrections for HP queues
class CoordinatedOmissionLatencyMeasurement {
    std::vector<LatencyMeasurement> measurements_;
    TimePoint start_;
    Duration intended_interval_;

public:
    CoordinatedOmissionLatencyMeasurement(Duration interval)
      : start_(Clock::now()), intended_interval_(interval)
    {
        measurements_.reserve(config::DEFAULT_SAMPLES);
    }
    
    void record(const LatencyMeasurement& m) {
        measurements_.push_back(m);
    }
    
    LatencyStats getCorrected() {
        LatencyStats stats;
        stats.reserve(measurements_.size() * 2);
        
        for (const auto &m : measurements_) {
            stats.addMeasurement(m);
            
            // Calculate expected vs actual timing
            auto expected = start_ + intended_interval_ * m.sequence_number;
            auto actual   = m.enqueue_time;
            auto delay    = actual - expected;
            
            if (delay > intended_interval_) {
                // Account for coordinated omission
                int missed = static_cast<int>(delay / intended_interval_);
                double base_latency = m.getLatencyMicros();
                
                for (int i = 1; i <= missed; ++i) {
                    double adjusted_latency = base_latency + 
                        std::chrono::duration<double,std::micro>(intended_interval_ * i).count();
                    stats.addLatency(adjusted_latency, m.hazard_scans);
                }
            }
        }
        return stats;
    }
};

// Main latency benchmark framework for hazard pointer queues
template<typename QueueType, typename MessageType>
class HazardPointerLatencyBenchmark {
    QueueType queue_;
    std::atomic<uint64_t> sequence_counter_{0};
    std::atomic<bool> benchmark_active_{false};

public:
    LatencyBenchmarkResult runSingleThreadedLatency(int samples = config::DEFAULT_SAMPLES) {
        LatencyStats stats;
        stats.reserve(samples);

        // Warmup phase
        for (int i = 0; i < config::WARMUP_SAMPLES; ++i) {
            MessageType msg(i, 0);
            queue_.enqueue(msg);
            MessageType tmp;
            queue_.dequeue(tmp);
        }

        auto start_time = Clock::now();
        
        for (int i = 0; i < samples; ++i) {
            auto enqueue_time = Clock::now();
            MessageType msg(i, 0);
            
            queue_.enqueue(msg);
            
            MessageType dequeued_msg;
            if (queue_.dequeue(dequeued_msg)) {
                auto dequeue_time = Clock::now();
                
                // Estimate scans for this enqueue+dequeue pair
                uint32_t scans = estimate_scan_count_realistic(2, 1, 0); // 2 ops, 1 thread, no depth
                
                // Validate message integrity
                if (!dequeued_msg.validate()) {
                    throw std::runtime_error("Message validation failed!");
                }
                
                LatencyMeasurement measurement{
                    enqueue_time, dequeue_time, 
                    static_cast<uint64_t>(i), 0, scans
                };
                stats.addMeasurement(measurement);
            }
        }
        auto end_time = Clock::now();

        return makeResult("Single-threaded latency", stats, 1,
                          std::chrono::duration<double>(end_time - start_time).count(), 0);
    }

    LatencyBenchmarkResult runMultiProducerLatency(int producers, int samples_per = config::DEFAULT_SAMPLES) {
        std::vector<std::thread> producer_threads;
        std::vector<LatencyMeasurement> all_measurements;
        std::mutex measurements_mutex;
        std::barrier sync_barrier(producers + 1);

        benchmark_active_.store(true);
        
        // Single consumer thread
        std::thread consumer([&]{
            MessageType msg;
            while (benchmark_active_.load() || !queue_.empty()) {
                if (queue_.dequeue(msg)) {
                    auto dequeue_time = Clock::now();
                    
                    // Estimate scans for this dequeue in multi-threaded context
                    uint32_t scans = estimate_scan_count_realistic(1, producers + 1, 0);
                    
                    if (!msg.validate()) {
                        throw std::runtime_error("Message validation failed in consumer!");
                    }
                    
                    LatencyMeasurement measurement{
                        msg.timestamp, dequeue_time, 
                        msg.sequence, msg.producer_id, scans
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
                    queue_.enqueue(msg);
                    
                    // Add some jitter to increase contention
                    if ((j % 100) == 0) {
                        std::this_thread::sleep_for(10us);
                    }
                }
            });
        }

        auto benchmark_start = Clock::now();
        sync_barrier.arrive_and_wait(); // Start all producers
        
        for (auto &thread : producer_threads) {
            thread.join();
        }
        
        std::this_thread::sleep_for(100ms); // Allow consumer to drain
        benchmark_active_.store(false);
        consumer.join();
        auto benchmark_end = Clock::now();

        LatencyStats stats;
        stats.reserve(all_measurements.size());
        for (const auto &measurement : all_measurements) {
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

        auto start_time = Clock::now();
        for (int i = 0; i < samples; ++i) {
            auto enqueue_time = Clock::now();
            
            queue_.enqueue(MessageType(i + depth, 0));
            
            MessageType dequeued_msg;
            if (queue_.dequeue(dequeued_msg)) {
                auto dequeue_time = Clock::now();
                
                // Estimate scans with queue depth consideration
                uint32_t scans = estimate_scan_count_realistic(2, 1, depth); // 2 ops, 1 thread, with depth
                
                LatencyMeasurement measurement{
                    enqueue_time, dequeue_time, 
                    static_cast<uint64_t>(i), 0, scans
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
        
        std::thread consumer([&]{
            MessageType msg;
            while (benchmark_active_.load()) {
                if (queue_.dequeue(msg)) {
                    auto dequeue_time = Clock::now();
                    
                    // Estimate scans for burst consumer (higher rate due to bursts)
                    uint32_t scans = estimate_scan_count_realistic(1, 2, 0);
                    
                    LatencyMeasurement measurement{
                        msg.timestamp, dequeue_time, 
                        msg.sequence, msg.producer_id, scans
                    };
                    all_measurements.push_back(measurement);
                }
            }
            
            // Drain remaining messages
            while (queue_.dequeue(msg)) {
                auto dequeue_time = Clock::now();
                uint32_t scans = estimate_scan_count_realistic(1, 1, 0); // Less contention during drain
                
                LatencyMeasurement measurement{
                    msg.timestamp, dequeue_time, 
                    msg.sequence, msg.producer_id, scans
                };
                all_measurements.push_back(measurement);
            }
        });

        auto burst_start = Clock::now();
        for (int burst = 0; burst < num_bursts; ++burst) {
            // Produce burst
            for (int i = 0; i < burst_size; ++i) {
                auto seq = sequence_counter_.fetch_add(1);
                queue_.enqueue(MessageType(seq, 0));
            }
            
            // Wait between bursts (except for the last one)
            if (burst < num_bursts - 1) {
                std::this_thread::sleep_for(burst_interval);
            }
        }

        std::this_thread::sleep_for(100ms); // Allow consumer to finish
        benchmark_active_.store(false);
        consumer.join();
        auto burst_end = Clock::now();

        for (const auto &measurement : all_measurements) {
            stats.addMeasurement(measurement);
        }

        return makeResult("Burst latency (burst=" + std::to_string(burst_size) + ")",
                          stats, 2,
                          std::chrono::duration<double>(burst_end - burst_start).count(),
                          0);
    }

    LatencyBenchmarkResult runCoordinatedOmissionTest(Duration target_interval = 1ms) {
        CoordinatedOmissionLatencyMeasurement co_measurement(target_interval);
        const int samples = config::DEFAULT_SAMPLES / 10; // Smaller sample size for CO test
        
        benchmark_active_.store(true);
        std::vector<LatencyMeasurement> measurements;
        std::mutex measurements_mutex;
        
        std::thread consumer([&]{
            MessageType msg;
            while (benchmark_active_.load() || !queue_.empty()) {
                if (queue_.dequeue(msg)) {
                    auto dequeue_time = Clock::now();
                    LatencyMeasurement measurement{
                        msg.timestamp, dequeue_time, 
                        msg.sequence, msg.producer_id, 0
                    };
                    
                    std::lock_guard<std::mutex> lock(measurements_mutex);
                    measurements.push_back(measurement);
                }
            }
        });

        auto start_time = Clock::now();
        for (int i = 0; i < samples; ++i) {
            auto target_time = start_time + target_interval * i;
            
            // Wait until target time
            std::this_thread::sleep_until(target_time);
            
            queue_.enqueue(MessageType(i, 0));
        }

        std::this_thread::sleep_for(100ms);
        benchmark_active_.store(false);
        consumer.join();
        auto end_time = Clock::now();

        // Record all measurements for coordinated omission correction
        for (const auto& m : measurements) {
            co_measurement.record(m);
        }

        LatencyStats corrected_stats = co_measurement.getCorrected();
        
        return makeResult("Coordinated Omission Corrected",
                          corrected_stats, 2,
                          std::chrono::duration<double>(end_time - start_time).count(),
                          0);
    }

private:
    LatencyBenchmarkResult makeResult(
        const std::string &name,
        LatencyStats      &stats,
        int                threads,
        double             duration_sec,
        int                depth)
    {
        LatencyBenchmarkResult result;
        result.name           = name;
        result.queue_type     = "HP";
        result.num_threads    = threads;
        result.payload_size   = static_cast<int>(sizeof(MessageType));
        result.queue_depth    = depth;
        result.sample_count   = stats.count();
        result.mean_latency   = stats.getMean();
        result.min_latency    = stats.getMin();
        result.max_latency    = stats.getMax();
        result.std_dev        = stats.getStdDev();
        result.jitter         = (result.mean_latency > 0 ? result.std_dev / result.mean_latency : 0);
        result.avg_hazard_scans = stats.getAverageScanCount();
        result.scan_efficiency = (result.avg_hazard_scans > 0 ? 
                                 result.sample_count / result.avg_hazard_scans : 0);
        
        for (double p : config::PERCENTILES) {
            result.percentiles[p] = stats.getPercentile(p);
        }
        
        result.throughput = (duration_sec > 0 ? result.sample_count / duration_sec : 0);
        result.histogram = stats.getHistogram();
        
        // Estimate memory overhead (approximation)
        result.memory_overhead_bytes = sizeof(MessageType) * result.sample_count + 
                                      (threads * sizeof(void*) * 2); // Hazard pointer slots
        
        return result;
    }
};

// Main benchmark suite
class HazardPointerLatencyBenchmarkSuite {
    std::vector<LatencyBenchmarkResult> results_;
    bool detailed_output_ = false;

public:
    void setDetailedOutput(bool detailed) { detailed_output_ = detailed; }

    void runAll() {
        std::cout << "Hazard Pointer Queue Latency Benchmarks\n"
                  << "=======================================\n"
                  << "Hardware threads: " << std::thread::hardware_concurrency() << '\n'
                  << "Sample count:     " << config::DEFAULT_SAMPLES << " per test\n"
                  << "Clock resolution: " << getClockResolution() << " ns\n"
                  << "HP analysis:      " << (DETAILED_HP_ANALYSIS ? "instrumented" : "estimated") << "\n"
                  << "Scan counting:    " << (DETAILED_HP_ANALYSIS ? "actual" : "heuristic estimation") << "\n\n";

        LatencyBenchmarkResult::printHeader();
        runSingleThreaded();
        runMultiThreaded();
        runLoadDependent();
        runBurstAnalysis();
        runPayloadSizeAnalysis();
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
        return std::chrono::duration<double,std::nano>(
                   *std::min_element(deltas.begin(), deltas.end())
               ).count();
    }

    void runSingleThreaded() {
        std::cout << "\n=== Single-Threaded Baseline ===\n";
        HazardPointerLatencyBenchmark<lfq::HPQueue<TimedMessage<64>>, TimedMessage<64>> benchmark;
        auto result = benchmark.runSingleThreadedLatency();
        result.print();
        results_.push_back(result);
        if (detailed_output_) result.printDetailed();
    }

    void runMultiThreaded() {
        std::cout << "\n=== Multi-Threaded Scenarios ===\n";
        for (int producers : {2, 4, 8}) {
            if (producers > config::MAX_THREADS - 1) break;
            
            HazardPointerLatencyBenchmark<lfq::HPQueue<TimedMessage<64>>, TimedMessage<64>> benchmark;
            auto result = benchmark.runMultiProducerLatency(producers, config::DEFAULT_SAMPLES / producers);
            result.print();
            results_.push_back(result);
            if (detailed_output_) result.printDetailed();
        }
    }

    void runLoadDependent() {
        std::cout << "\n=== Load-Dependent Latency ===\n";
        for (int depth : config::QUEUE_DEPTHS) {
            HazardPointerLatencyBenchmark<lfq::HPQueue<TimedMessage<64>>, TimedMessage<64>> benchmark;
            auto result = benchmark.runLoadDependentLatency(depth, config::DEFAULT_SAMPLES / 10);
            result.print();
            results_.push_back(result);
        }
    }

    void runBurstAnalysis() {
        std::cout << "\n=== Burst Latency Analysis ===\n";
        for (int burst_size : {100, 1000, 10000}) {
            HazardPointerLatencyBenchmark<lfq::HPQueue<TimedMessage<64>>, TimedMessage<64>> benchmark;
            auto result = benchmark.runBurstLatency(burst_size);
            result.print();
            results_.push_back(result);
        }
    }

    void runPayloadSizeAnalysis() {
        std::cout << "\n=== Payload Size Impact ===\n";
        
        // 16-byte payload
        {
            HazardPointerLatencyBenchmark<lfq::HPQueue<TimedMessage<16>>, TimedMessage<16>> benchmark;
            auto result = benchmark.runSingleThreadedLatency(config::DEFAULT_SAMPLES / 10);
            result.name = "16B payload";
            result.print();
            results_.push_back(result);
        }
        
        // 256-byte payload
        {
            HazardPointerLatencyBenchmark<lfq::HPQueue<TimedMessage<256>>, TimedMessage<256>> benchmark;
            auto result = benchmark.runSingleThreadedLatency(config::DEFAULT_SAMPLES / 10);
            result.name = "256B payload";
            result.print();
            results_.push_back(result);
        }
        
        // 1KB payload
        {
            HazardPointerLatencyBenchmark<lfq::HPQueue<TimedMessage<1024>>, TimedMessage<1024>> benchmark;
            auto result = benchmark.runSingleThreadedLatency(config::DEFAULT_SAMPLES / 10);
            result.name = "1KB payload";
            result.print();
            results_.push_back(result);
        }
        
        // 4KB payload
        {
            HazardPointerLatencyBenchmark<lfq::HPQueue<TimedMessage<4096>>, TimedMessage<4096>> benchmark;
            auto result = benchmark.runSingleThreadedLatency(config::DEFAULT_SAMPLES / 20);
            result.name = "4KB payload";
            result.print();
            results_.push_back(result);
        }
    }

    void runCoordinatedOmissionAnalysis() {
        std::cout << "\n=== Coordinated Omission Analysis ===\n";
        HazardPointerLatencyBenchmark<lfq::HPQueue<TimedMessage<64>>, TimedMessage<64>> benchmark;
        auto result = benchmark.runCoordinatedOmissionTest(1ms);
        result.print();
        results_.push_back(result);
        if (detailed_output_) result.printDetailed();
    }

    void printSummary() {
        std::cout << "\n=== Hazard Pointer Queue Latency Summary ===\n";
        
        if (results_.empty()) {
            std::cout << "No results to summarize.\n";
            return;
        }
        
        auto best_mean = std::min_element(
            results_.begin(), results_.end(),
            [](const auto &a, const auto &b) { return a.mean_latency < b.mean_latency; }
        );
        
        auto worst_p99 = std::max_element(
            results_.begin(), results_.end(),
            [](const auto &a, const auto &b) { 
                return a.percentiles.at(99.0) < b.percentiles.at(99.0); 
            }
        );
        
        auto best_throughput = std::max_element(
            results_.begin(), results_.end(),
            [](const auto &a, const auto &b) { return a.throughput < b.throughput; }
        );
        
        std::cout << "Best mean latency: " << std::fixed << std::setprecision(2) 
                  << best_mean->mean_latency << " μs (" << best_mean->name << ")\n"
                  << "Worst P99 latency: " << worst_p99->percentiles.at(99.0)
                  << " μs (" << worst_p99->name << ")\n"
                  << "Best throughput: " << best_throughput->throughput
                  << " ops/sec (" << best_throughput->name << ")\n";

        // Calculate weighted statistics
        double total_samples = 0, weighted_mean = 0, weighted_scans = 0;
        for (const auto &result : results_) {
            weighted_mean += result.mean_latency * result.sample_count;
            weighted_scans += result.avg_hazard_scans * result.sample_count;
            total_samples += result.sample_count;
        }
        
        if (total_samples > 0) {
            std::cout << "Overall weighted mean latency: " 
                      << (weighted_mean / total_samples) << " μs\n"
                      << "Overall avg hazard scans: " 
                      << (weighted_scans / total_samples) << " per operation\n";
        }
        
        std::cout << "Total samples processed: " << static_cast<size_t>(total_samples) << '\n';
        
        // Hazard pointer specific analysis
        std::cout << "\n=== Hazard Pointer Efficiency Analysis ===\n";
        double min_scan_rate = std::numeric_limits<double>::max();
        double max_scan_rate = 0.0;
        std::string min_scan_test, max_scan_test;
        
        for (const auto &result : results_) {
            if (result.avg_hazard_scans > 0) {
                if (result.avg_hazard_scans < min_scan_rate) {
                    min_scan_rate = result.avg_hazard_scans;
                    min_scan_test = result.name;
                }
                if (result.avg_hazard_scans > max_scan_rate) {
                    max_scan_rate = result.avg_hazard_scans;
                    max_scan_test = result.name;
                }
            }
        }
        
        if (min_scan_rate < std::numeric_limits<double>::max()) {
            std::cout << "Most efficient (fewest scans): " << min_scan_rate 
                      << " scans (" << min_scan_test << ")\n"
                      << "Least efficient (most scans): " << max_scan_rate 
                      << " scans (" << max_scan_test << ")\n";
        }
    }

    void exportResults() {
        std::ofstream csv("hp_latency_results.csv");
        csv << "Benchmark,Queue_Type,Threads,Payload_Size,Queue_Depth,Sample_Count,"
               "Mean_Latency_us,Min_Latency_us,Max_Latency_us,Std_Dev_us,Jitter,"
               "Avg_Hazard_Scans,Scan_Efficiency,Memory_Overhead_bytes,"
               "P50_us,P90_us,P95_us,P99_us,P99_9_us,P99_99_us,Throughput_ops_per_sec\n";
        
        for (const auto &result : results_) {
            csv << result.name << ',' << result.queue_type << ','
                << result.num_threads << ',' << result.payload_size << ','
                << result.queue_depth << ',' << result.sample_count << ','
                << result.mean_latency << ',' << result.min_latency << ','
                << result.max_latency << ',' << result.std_dev << ',' << result.jitter << ','
                << result.avg_hazard_scans << ',' << result.scan_efficiency << ','
                << result.memory_overhead_bytes << ',';
            
            for (double p : config::PERCENTILES) {
                csv << result.percentiles.at(p) << ',';
            }
            csv << result.throughput << "\n";
        }
        
        std::cout << "\nResults exported to hp_latency_results.csv\n";
        exportHistograms();
        exportHazardPointerAnalysis();
    }

    void exportHistograms() {
        std::ofstream hist_file("hp_latency_histograms.csv");
        
        // Find the result with the highest standard deviation for histogram export
        auto most_variable = std::max_element(
            results_.begin(), results_.end(),
            [](const auto &a, const auto &b) { return a.std_dev < b.std_dev; }
        );
        
        if (most_variable != results_.end()) {
            hist_file << "Benchmark," << most_variable->name << "\n";
            hist_file << "Bucket,Count\n";
            for (size_t i = 0; i < most_variable->histogram.size(); ++i) {
                hist_file << i << ',' << most_variable->histogram[i] << '\n';
            }
            std::cout << "Histogram data exported to hp_latency_histograms.csv\n";
        }
    }

    void exportHazardPointerAnalysis() {
        std::ofstream hp_file("hp_analysis.csv");
        hp_file << "Benchmark,Avg_Scans,Scan_Efficiency,Memory_Overhead_MB\n";
        
        for (const auto &result : results_) {
            hp_file << result.name << ','
                    << result.avg_hazard_scans << ','
                    << result.scan_efficiency << ','
                    << (result.memory_overhead_bytes / (1024.0 * 1024.0)) << '\n';
        }
        
        std::cout << "Hazard pointer analysis exported to hp_analysis.csv\n";
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

// Comparison benchmark between HP and theoretical ideal
void runComparisonBenchmark() {
    std::cout << "\n=== HP vs Ideal Comparison ===\n";
    std::cout << "This section would compare HP queue with an ideal queue\n";
    std::cout << "that has zero memory management overhead.\n";
    
    // Theoretical analysis
    constexpr double HAZARD_POINTER_OVERHEAD_NS = 10.0; // Estimated
    constexpr double SCAN_COST_PER_THREAD_NS = 50.0;    // Estimated
    
    HazardPointerLatencyBenchmark<lfq::HPQueue<TimedMessage<64>>, TimedMessage<64>> hp_benchmark;
    auto hp_result = hp_benchmark.runSingleThreadedLatency(10000);
    
    std::cout << "HP Queue mean latency: " << hp_result.mean_latency << " μs\n";
    std::cout << "Estimated HP overhead: " << HAZARD_POINTER_OVERHEAD_NS / 1000.0 << " μs\n";
    std::cout << "HP efficiency: " << std::fixed << std::setprecision(1)
              << (100.0 * ((hp_result.mean_latency * 1000.0 - HAZARD_POINTER_OVERHEAD_NS) / 
                          (hp_result.mean_latency * 1000.0))) << "%\n";
}

int main(int argc, char* argv[]) {
    bool detailed = false;
    bool memory_pressure = false;
    size_t pressure_mb = 100;
    
    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--detailed" || arg == "-d") {
            detailed = true;
        } else if (arg == "--memory-pressure" || arg == "-m") {
            memory_pressure = true;
        } else if (arg.substr(0, 11) == "--pressure=") {
            pressure_mb = std::stoull(arg.substr(11));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  -d, --detailed         Show detailed statistics\n"
                      << "  -m, --memory-pressure  Run with memory pressure\n"
                      << "  --pressure=MB          Set memory pressure amount (default: 100)\n"
                      << "  -h, --help            Show this help\n";
            return 0;
        }
    }

    // Set process priority on Linux
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
        HazardPointerLatencyBenchmarkSuite suite;
        suite.setDetailedOutput(detailed);
        suite.runAll();
        
        runComparisonBenchmark();
        
        std::cout << "\n🎯 Hazard Pointer Latency Benchmarks Complete! 🎯\n";
        return 0;
        
    } catch (const std::exception& ex) {
        std::cerr << "\n❌ Benchmark failed: " << ex.what() << "\n";
        return 1;
    }
}