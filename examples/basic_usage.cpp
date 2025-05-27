/**
 * Basic usage example for Lock-Free Queue
 */

#include <iostream>
#include <thread>
#include <vector>
#include <chrono>

// Include the EBR-based queue implementation
#include "lockfree_queue_ebr.hpp"

int main() {
    std::cout << "Lock-Free Queue Basic Usage Example\n";
    std::cout << "===================================\n\n";
    
    // Create a queue for integers - using EBRQueue instead of Queue
    lfq::EBRQueue<int> queue;
    
    // Simple enqueue/dequeue
    std::cout << "1. Basic operations:\n";
    queue.enqueue(42);
    queue.enqueue(100);
    queue.enqueue(200);
    
    int value;
    while (queue.dequeue(value)) {
        std::cout << "   Dequeued: " << value << std::endl;
    }
    
    // Multi-threaded example
    std::cout << "\n2. Multi-threaded producer-consumer:\n";
    
    constexpr int NUM_PRODUCERS = 2;
    constexpr int NUM_CONSUMERS = 2;
    constexpr int ITEMS_PER_PRODUCER = 5;
    
    std::vector<std::thread> threads;
    
    // Start producers - fixed lambda capture
    for (int i = 0; i < NUM_PRODUCERS; ++i) {
        threads.emplace_back([&queue, i]() {
            for (int j = 0; j < ITEMS_PER_PRODUCER; ++j) {
                int value = i * 1000 + j;
                queue.enqueue(value);
                std::cout << "   Producer " << i << " enqueued: " << value << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }
    
    // Start consumers - fixed lambda capture
    for (int i = 0; i < NUM_CONSUMERS; ++i) {
        threads.emplace_back([&queue, i]() {
            int value;
            int consumed = 0;
            while (consumed < (ITEMS_PER_PRODUCER * NUM_PRODUCERS) / NUM_CONSUMERS) {
                if (queue.dequeue(value)) {
                    std::cout << "   Consumer " << i << " dequeued: " << value << std::endl;
                    ++consumed;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        });
    }
    
    // Wait for all threads
    for (auto& t : threads) {
        t.join();
    }
    
    // Check if queue is empty
    std::cout << "\n3. Final state:\n";
    std::cout << "   Queue is " << (queue.empty() ? "empty" : "not empty") << std::endl;
    
    // Drain any remaining items
    int remaining = 0;
    while (queue.dequeue(value)) {
        ++remaining;
    }
    if (remaining > 0) {
        std::cout << "   Drained " << remaining << " remaining items\n";
    }
    
    std::cout << "\nExample completed successfully!\n";
    return 0;
}