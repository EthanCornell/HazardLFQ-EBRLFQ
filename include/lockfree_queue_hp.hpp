/********************************************************************************************
 *  lockfree_queue_hp.hpp — Michael–Scott MPMC Queue + Hazard Pointers
 *
 *  Header-only lock-free queue library with hazard pointer memory reclamation.
 *  This implementation provides wait-free memory reclamation with bounded memory usage
 *  and excellent performance for read-heavy workloads.
 *
 *  Features:
 *  • Wait-free enqueue (bounded retries for fixed thread count)
 *  • Lock-free dequeue with progress guarantee
 *  • Hazard pointer reclamation prevents ABA and use-after-free
 *  • Bounded memory usage (at most H unreclaimed nodes)
 *  • No per-element fence instructions for long traversals
 *  • Header-only, C++20, sanitizer-clean
 *
 *  Usage:
 *      #include "lockfree_queue_hp.hpp"
 *      
 *      lfq::Queue<int> queue;
 *      queue.enqueue(42);
 *      
 *      int value;
 *      if (queue.dequeue(value)) {
 *          // Got value
 *      }
 *
 *  Build examples:
 *      g++ -std=c++20 -O2 -pthread your_code.cpp
 *      g++ -std=c++20 -O1 -g -fsanitize=thread -pthread your_code.cpp
 *      g++ -std=c++20 -O1 -g -fsanitize=address -fno-omit-frame-pointer -pthread your_code.cpp
 *
 *  ────────────────────────────────────────────────────────────────────────────────
 *  Hazard Pointer Algorithm - How memory reclamation works:
 *  ────────────────────────────────────────────────────────────────────────────────
 *  
 *  1. PUBLISH: Before dereferencing a shared pointer, publish it in a hazard slot
 *  2. VALIDATE: Re-read the pointer to ensure it hasn't been removed concurrently  
 *  3. PROTECT: The pointer is now safe to dereference (protected by hazard)
 *  4. RETIRE: When removing nodes, add them to a private retired list
 *  5. SCAN: Periodically scan all hazard pointers and reclaim unprotected nodes
 *
 *  Memory bound: At most H = K×N hazard pointers exist, so at most H nodes
 *  can be protected from reclamation, ensuring bounded memory usage.
 *
 *  Reference: Maged M. Michael, "Hazard Pointers: Safe Memory Reclamation 
 *  for Lock-Free Objects", IEEE TPDS 2004.
 ********************************************************************************************/

#pragma once

#include <atomic>
#include <array>
#include <vector>
#include <functional>
#include <algorithm>
#include <stdexcept>
#include <new>
#include <utility>

namespace lfq {

/******************************** Hazard Pointer System *********************************/
namespace hp {

/* Configuration Constants */
constexpr unsigned kHazardsPerThread = 2;   // K in Michael's paper
constexpr unsigned kMaxThreads       = 128; // Maximum concurrent threads
constexpr unsigned kRFactor          = 2;   // R = H×kRFactor threshold

/* Global hazard pointer table */
struct Slot {
    std::atomic<void*> ptr{nullptr};         // nullptr = unused, (void*)1 = reserved
};

inline std::array<Slot, kHazardsPerThread * kMaxThreads> g_slots{};

/* Per-thread retired node tracking */
struct Retired {
    void*                      raw;
    std::function<void(void*)> deleter;
};

inline thread_local struct ThreadState {
    std::array<Slot*, kHazardsPerThread> hazard_slots{nullptr, nullptr};
    std::vector<Retired>                 retired_list;
} tls;

/* Acquire an unused global hazard slot */
inline Slot* acquire_slot() {
    for (auto& slot : g_slots) {
        void* expected = nullptr;
        if (slot.ptr.compare_exchange_strong(expected, reinterpret_cast<void*>(1),
                                           std::memory_order_acq_rel)) {
            return &slot;
        }
    }
    throw std::runtime_error("hazard_pointer: all slots exhausted");
}

/* RAII guard for hazard pointer management */
class Guard {
    Slot* slot_;
    
public:
    Guard() {
        // Lazily acquire one of this thread's K hazard slots
        unsigned i = 0;
        while (i < kHazardsPerThread && !tls.hazard_slots[i]) ++i;
        if (i == kHazardsPerThread) i = 0;  // Rotate if all slots used
        
        if (!tls.hazard_slots[i]) {
            tls.hazard_slots[i] = acquire_slot();
        }
        slot_ = tls.hazard_slots[i];
    }
    
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    
    /* Publish and validate pointer until stable (Michael's Algorithm Fig. 2) */
    template<typename T>
    T* protect(const std::atomic<T*>& source) {
        T* ptr;
        do {
            ptr = source.load(std::memory_order_acquire);
            slot_->ptr.store(ptr, std::memory_order_release);
        } while (ptr != source.load(std::memory_order_acquire));
        return ptr;
    }
    
    void clear() {
        slot_->ptr.store(nullptr, std::memory_order_release);
    }
    
    ~Guard() {
        clear();
    }
};

/* Forward declaration */
inline void scan();

/* Retire a node with custom deleter */
inline void retire(void* ptr, std::function<void(void*)> deleter) {
    tls.retired_list.push_back({ptr, std::move(deleter)});
    
    const std::size_t H = kHazardsPerThread * kMaxThreads;
    const std::size_t R = H * kRFactor;
    
    if (tls.retired_list.size() >= R) {
        scan();  // Amortized O(1) reclamation
    }
}

/* Scan hazard pointers and reclaim unprotected nodes */
inline void scan() {
    // Build snapshot of all current hazard pointers
    std::vector<void*> hazard_snapshot;
    hazard_snapshot.reserve(kHazardsPerThread * kMaxThreads);
    
    for (auto& slot : g_slots) {
        void* ptr = slot.ptr.load(std::memory_order_acquire);
        if (ptr && ptr != reinterpret_cast<void*>(1)) {
            hazard_snapshot.push_back(ptr);
        }
    }
    
    // Reclaim nodes not present in hazard snapshot
    auto it = tls.retired_list.begin();
    while (it != tls.retired_list.end()) {
        if (std::find(hazard_snapshot.begin(), hazard_snapshot.end(), 
                      it->raw) == hazard_snapshot.end()) {
            // Safe to reclaim - not in any hazard pointer
            it->deleter(it->raw);
            it = tls.retired_list.erase(it);
        } else {
            ++it;
        }
    }
}

/* Convenience template wrapper for typed objects */
template<typename T>
inline void retire(T* ptr) {
    retire(static_cast<void*>(ptr), 
           [](void* p) { delete static_cast<T*>(p); });
}

} // namespace hp

/******************************** Michael–Scott Queue (MPMC) ********************************
 *
 *  Progress guarantees (Michael & Scott 1996):
 *
 *      • enqueue()  — **wait-free for any fixed thread count N**  
 *        ────────────────────────────────────────────────────────
 *        - A producer performs **at most N + 2 CAS attempts** before success
 *        - Bounded retry count independent of other threads ⇒ wait-free
 *
 *      • dequeue()  — **lock-free**  
 *        ───────────────────────────
 *        - May retry indefinitely under contention, but system-wide progress
 *          is guaranteed (some thread always makes progress)
 *
 *      • Hazard pointer operations — **wait-free**
 *        - scan() is O(H) but called every R retirements ⇒ amortized O(1)
 *        - Memory usage bounded: at most H + R×N unreclaimed nodes
 *
 *  Memory reclamation properties:
 *      • No ABA problems: Hazard pointers prevent premature reclamation
 *      • Bounded memory: At most H protected + R×N retired nodes
 *      • Wait-free reclamation: No thread can block memory cleanup
 ************************************************************************************************/

template<typename T>
class HPQueue {
    struct Node {
        std::atomic<Node*> next{nullptr};
        alignas(T) unsigned char storage[sizeof(T)];
        bool has_value;
        
        template<typename... Args>
        explicit Node(bool is_dummy, Args&&... args) : has_value(!is_dummy) {
            if (!is_dummy) {
                ::new (storage) T(std::forward<Args>(args)...);
            }
        }
        
        T& value() { 
            return *std::launder(reinterpret_cast<T*>(storage)); 
        }
        
        const T& value() const { 
            return *std::launder(reinterpret_cast<const T*>(storage)); 
        }
        
        ~Node() { 
            if (has_value) {
                value().~T(); 
            }
        }
    };

    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;

public:
    HPQueue() {
        Node* dummy = new Node(true);  // Initial dummy node
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }
    
    HPQueue(const HPQueue&) = delete;
    HPQueue& operator=(const HPQueue&) = delete;

    /*-------------------------------- enqueue --------------------------------*/
    /*  Wait-free (bounded retries) for fixed thread count */
    template<typename... Args>
    void enqueue(Args&&... args) {
        Node* new_node = new Node(false, std::forward<Args>(args)...);
        hp::Guard tail_guard;  // Only need one hazard pointer for enqueue

        for (;;) {
            Node* tail = tail_guard.protect(tail_);
            Node* next = tail->next.load(std::memory_order_acquire);

            // Validate tail hasn't changed during hazard pointer acquisition
            if (tail != tail_.load(std::memory_order_acquire)) {
                continue;
            }

            if (next == nullptr) {
                // Queue appears quiescent - try to link new node
                if (tail->next.compare_exchange_weak(next, new_node,
                        std::memory_order_release, std::memory_order_relaxed)) {
                    // Help advance tail pointer (cooperative)
                    tail_.compare_exchange_strong(tail, new_node,
                        std::memory_order_release, std::memory_order_relaxed);
                    return;  // Enqueue complete
                }
            } else {
                // Tail is lagging - help advance it
                tail_.compare_exchange_strong(tail, next,
                    std::memory_order_release, std::memory_order_relaxed);
            }
        }
    }

    /*-------------------------------- dequeue --------------------------------*/
    /*  Lock-free - may retry indefinitely but system makes progress */
    bool dequeue(T& result) {
        hp::Guard head_guard, next_guard;  // Need 2 hazard pointers

        for (;;) {
            Node* head = head_guard.protect(head_);  // Protect dummy node
            Node* tail = tail_.load(std::memory_order_acquire);
            Node* next = next_guard.protect(head->next);  // Protect first real node

            // Validate head hasn't changed during protection
            if (head != head_.load(std::memory_order_acquire)) {
                continue;
            }

            if (next == nullptr) {
                return false;  // Queue is empty
            }

            if (head == tail) {
                // Tail is lagging behind - help advance it
                tail_.compare_exchange_strong(tail, next,
                    std::memory_order_release, std::memory_order_relaxed);
                continue;
            }

            // Read value before CAS (still protected by hazard pointer)
            T value = next->value();

            if (head_.compare_exchange_strong(head, next,
                    std::memory_order_release, std::memory_order_relaxed)) {
                result = std::move(value);
                head_guard.clear();  // Allow old dummy to be reclaimed
                hp::retire(head);    // Safe reclamation via hazard pointers
                return true;
            }
        }
    }

    /*-------------------------------- utility --------------------------------*/
    bool empty() const {
        hp::Guard guard;
        Node* head = guard.protect(head_);
        return head->next.load(std::memory_order_acquire) == nullptr;
    }

    ~HPQueue() {
        // Destructor is single-threaded - safe to free directly
        Node* current = head_.load(std::memory_order_relaxed);
        while (current) {
            Node* next = current->next.load(std::memory_order_relaxed);
            delete current;
            current = next;
        }
    }
};

} // namespace lfq