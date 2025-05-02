// lockfree_queue.hpp — header-only, C++17
#pragma once
#include "hazard_pointer.hpp"
#include <atomic>
#include <memory>
#include <new>

namespace lfq {

template<typename T>
class Queue {
    struct Node {
        std::atomic<Node*> next{nullptr};
        alignas(T) unsigned char storage[sizeof(T)];
        bool has_value;                     // false for the dummy node

        template<typename... Args>
        explicit Node(bool dummy, Args&&... args) : has_value(!dummy) {
            if (!dummy)
                ::new (storage) T(std::forward<Args>(args)...);
        }
        T& value()       { return *std::launder(reinterpret_cast<T*>(storage)); }
        const T& value() const { return *std::launder(reinterpret_cast<const T*>(storage)); }
        ~Node() {
            if (has_value) value().~T();
        }
    };

    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;

public:
    Queue() {
        Node* dummy = new Node(true);   // single sentinel
        head_.store(dummy, std::memory_order_relaxed);
        tail_.store(dummy, std::memory_order_relaxed);
    }

    Queue(const Queue&)            = delete;
    Queue& operator=(const Queue&) = delete;

    /* ───────────── enqueue (push) ─────────────────────────────── */
    template<typename... Args>
    void enqueue(Args&&... args) {
        Node* n = new Node(false, std::forward<Args>(args)...);

        for (;;) {
            Node* tail = tail_.load(std::memory_order_acquire);
            Node* next = tail->next.load(std::memory_order_acquire);

            if (tail == tail_.load(std::memory_order_acquire)) {
                if (next == nullptr) {
                    // queue appears in quiescent state; try to link new node
                    if (tail->next.compare_exchange_weak(
                            next, n, std::memory_order_release, std::memory_order_relaxed))
                    {
                        // swing tail to new node (optional)
                        tail_.compare_exchange_strong(tail, n,
                                                      std::memory_order_release,
                                                      std::memory_order_relaxed);
                        return;
                    }
                } else {
                    // tail is falling behind; help by advancing it
                    tail_.compare_exchange_strong(tail, next,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed);
                }
            }
        }
    }

    /* ───────────── dequeue (pop) ──────────────────────────────── */
    bool dequeue(T& out) {
        hp::Guard hHead, hNext;             // two hazard slots required

        for (;;) {
            Node* head  = hHead.protect(head_);
            Node* tail  = tail_.load(std::memory_order_acquire);
            Node* next  = hNext.protect(head->next);

            if (head != head_.load(std::memory_order_acquire))
                continue;                   // retry if head changed

            if (next == nullptr)            // empty?
                return false;

            if (head == tail) {
                // tail is behind; help it
                tail_.compare_exchange_strong(tail, next,
                                              std::memory_order_release,
                                              std::memory_order_relaxed);
                continue;
            }

            // read value *before* CAS; safe because we still hold hazard for next
            T value_copy = next->value();

            if (head_.compare_exchange_strong(head, next,
                                              std::memory_order_release,
                                              std::memory_order_relaxed))
            {
                out = std::move(value_copy);
                hHead.clear();              // make old dummy reclaimable
                hp::retire(head);           // reclamation through HP
                return true;
            }
        }
    }

    /* ───────────── convenience helpers ────────────────────────── */
    bool empty() const {
        hp::Guard g;
        Node* head = g.protect(head_);
        Node* next = head->next.load(std::memory_order_acquire);
        return next == nullptr;
    }

    /* ───────────── teardown ───────────────────────────────────── */
    ~Queue() {
        // Drain the list without concurrency; safe to delete directly
        Node* n = head_.load(std::memory_order_relaxed);
        while (n) {
            Node* nxt = n->next.load(std::memory_order_relaxed);
            delete n;
            n = nxt;
        }
    }
};

} // namespace lfq
// lockfree_queue.hpp — header-only, C++17
// g++ -std=c++20 -O3 -g -fsanitize=thread -pthread lockfree_queue.hpp -o lfq_tsan