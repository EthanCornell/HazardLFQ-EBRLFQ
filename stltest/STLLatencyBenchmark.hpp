#ifndef STLLATENCYBENCHMARK_HPP
#define STLLATENCYBENCHMARK_HPP

#include "../include/ThreadSafeQueue.hpp"  // Include the thread-safe queue header

// Define the LatencyBenchmarkResult structure for storing benchmark results
struct LatencyBenchmarkResult {
    std::string name, queue_type;
    int num_threads, payload_size, queue_depth;
    size_t sample_count;
    double mean_latency, min_latency, max_latency, std_dev, jitter, throughput;
    double avg_lock_contentions, lock_efficiency;
    std::map<double, double> percentiles;
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
                  << std::setw(10) << "AvgLocks"
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
                  << std::setw(10) << avg_lock_contentions
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
                  << "Avg lock contentions: "  << avg_lock_contentions << '\n'
                  << "Lock efficiency: " << lock_efficiency << " ops/contention\n"
                  << "Memory overhead: " << memory_overhead_bytes << " bytes\n\n"
                  << "Percentiles:\n";
        for (const auto& p : percentiles) {
            std::cout << "  P" << std::setw(5) << std::left << p.first << ": "
                      << std::setw(8) << p.second << " μs\n";
        }
        std::cout << "\nThroughput: " << throughput << " ops/sec\n";
    }
};

// Define the STLLatencyBenchmark class
template <typename QueueType, typename MessageType>
class STLLatencyBenchmark {
private:
    QueueType queue_;
    std::atomic<uint64_t> sequence_counter_{0};
    std::atomic<bool> benchmark_active_{false};

public:
    // Single-threaded latency benchmark
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

        queue_.resetContentionCount();
        auto start_time = Clock::now();
        
        for (int i = 0; i < samples; ++i) {
            auto enqueue_time = Clock::now();
            MessageType msg(i, 0);
            queue_.enqueue(msg);
            
            MessageType dequeued_msg;
            if (queue_.dequeue(dequeued_msg)) {
                auto dequeue_time = Clock::now();
                
                // Estimate lock contentions (should be 0 for single-threaded)
                uint32_t contentions = estimate_lock_contentions(1, 2);
                
                // Validate message integrity
                if (!dequeued_msg.validate()) {
                    throw std::runtime_error("Message validation failed!");
                }
                
                LatencyMeasurement measurement{
                    enqueue_time, dequeue_time, 
                    static_cast<uint64_t>(i), 0, contentions
                };
                stats.addMeasurement(measurement);
            }
        }
        auto end_time = Clock::now();

        return makeResult("Single-threaded latency", stats, 1,
                          std::chrono::duration<double>(end_time - start_time).count(), 0);
    }

    // Multi-threaded latency benchmark
    LatencyBenchmarkResult runMultiProducerLatency(int producers, int samples_per = config::DEFAULT_SAMPLES) {
        std::vector<std::thread> producer_threads;
        std::vector<LatencyMeasurement> all_measurements;
        std::mutex measurements_mutex;
        std::barrier sync_barrier(producers + 1);

        benchmark_active_.store(true);
        queue_.resetContentionCount();
        
        // Single consumer thread
        std::thread consumer([&]{
            MessageType msg;
            while (benchmark_active_.load() || !queue_.empty()) {
                if (queue_.dequeue(msg)) {
                    auto dequeue_time = Clock::now();
                    
                    // Estimate lock contentions for this dequeue in multi-threaded context
                    uint32_t contentions = estimate_lock_contentions(producers + 1, 1);
                    
                    if (!msg.validate()) {
                        throw std::runtime_error("Message validation failed in consumer!");
                    }
                    
                    LatencyMeasurement measurement{
                        msg.timestamp, dequeue_time, 
                        msg.sequence, msg.producer_id, contentions
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

    // Add additional benchmarking functions as required
    // (e.g., Load-Dependent, Burst Latency, Payload Size Impact, etc.)

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
        result.queue_type     = "STL";
        result.num_threads    = threads;
        result.payload_size   = static_cast<int>(sizeof(MessageType));
        result.queue_depth    = depth;
        result.sample_count   = stats.count();
        result.mean_latency   = stats.getMean();
        result.min_latency    = stats.getMin();
        result.max_latency    = stats.getMax();
        result.std_dev        = stats.getStdDev();
        result.jitter         = (result.mean_latency > 0 ? result.std_dev / result.mean_latency : 0);
        result.avg_lock_contentions = stats.getAverageContentions();
        result.lock_efficiency = (result.avg_lock_contentions > 0 ? 
                                 result.sample_count / result.avg_lock_contentions : 0);
        
        for (double p : config::PERCENTILES) {
            result.percentiles[p] = stats.getPercentile(p);
        }
        
        result.throughput = (duration_sec > 0 ? result.sample_count / duration_sec : 0);
        result.histogram = stats.getHistogram();
        
        // Estimate memory overhead
        result.memory_overhead_bytes = sizeof(MessageType) * result.sample_count + 
                                      sizeof(std::mutex) + sizeof(std::queue<MessageType>);
        
        return result;
    }
};

#endif // STLLATENCYBENCHMARK_HPP
