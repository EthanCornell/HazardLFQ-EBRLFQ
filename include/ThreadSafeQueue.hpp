// This ThreadSafeQueue is a coarse-grained, lock-based synchronization solution
#ifndef THREAD_SAFE_QUEUE_HPP
#define THREAD_SAFE_QUEUE_HPP

#include <queue>
#include <mutex>
#include <atomic>
#include <iostream>
#include <condition_variable>

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
    // Enqueue an item (non-const reference)
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

#endif // THREAD_SAFE_QUEUE_HPP
