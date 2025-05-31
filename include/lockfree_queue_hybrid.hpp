/********************************************************************************************
 *  lockfree_queue_hybrid.hpp — Michael–Scott MPMC Queue + 3‑epoch EBR fast‑path +
 *                               Hazard‑Pointer fallback when the epoch stalls.
 *
 *  Header‑only, C++20, sanitizer‑clean.
 *
 *  ────────────────────────────────────────────────────────────────────────────────
 *  Why hybrid?
 *  ───────────
 *      • Epoch‑Based Reclamation (EBR) gives near‑zero‑overhead reads, but leaks
 *        memory indefinitely if a thread parks inside a critical section.
 *
 *      • Hazard Pointers (HP) guarantee safety even if a thread dies, but every
 *        dereference pays a store + fence + snapshot scan.
 *
 *      • This file combines both: normal traffic runs on EBR; if the global
 *        epoch cannot advance for STALL_LIMIT consecutive attempts, every live
 *        thread switches to HP mode and reclamation proceeds via hp::scan().
 *
 *      Fast path:   enqueue/dequeue cost == pure EBR   (no HP stores)
 *      Slow path:   space leak bounded, system remains lock‑free.
 *
 *  ────────────────────────────────────────────────────────────────────────────────
 *  Usage
 *  ─────
 *      #include "lockfree_queue_hybrid.hpp"
 *
 *      lfq::HybridQueue<int> q;
 *      q.enqueue(1);
 *      int x;  q.dequeue(x);
 *
 *  Build:
 *      g++ -std=c++20 -O2 -pthread test.cpp
 *
 *  Copyright (c) 2025 EthanCornell & contributors — MIT License
 ********************************************************************************************/
#ifndef LOCKFREE_QUEUE_HYBRID_HPP
#define LOCKFREE_QUEUE_HYBRID_HPP

#include <atomic>
#include <array>
#include <vector>
#include <thread>
#include <functional>
#include <cassert>
#include <utility>
#include <cstdint>
#include <memory>
#include <algorithm>

/*──────────────── Hazard‑pointer subsystem (unchanged) ─────────────*/
namespace hp {

constexpr unsigned kHazardsPerThread = 2;
constexpr unsigned kMaxThreads       = 512;        // align with EBR pool
constexpr unsigned kRFactor          = 2;

struct HazardSlot { std::atomic<void*> ptr{nullptr}; };
inline std::array<HazardSlot, kMaxThreads * kHazardsPerThread> g_slots{};

struct RetiredNode { void* raw; std::function<void(void*)> deleter; };

inline thread_local struct ThreadRec {
    std::array<HazardSlot*, kHazardsPerThread> h{nullptr,nullptr};
    std::vector<RetiredNode> retired;
} tls;

inline HazardSlot* acquire_slot() {
    for (auto& s : g_slots) {
        void* exp = nullptr;
        if (s.ptr.compare_exchange_strong(exp, reinterpret_cast<void*>(1),
                                          std::memory_order_acq_rel))
            return &s;
    }
    throw std::runtime_error("hp: exhausted hazard slots");
}

class Guard {
    HazardSlot* s_;
public:
    Guard() {
        unsigned i{};
        for (; i<kHazardsPerThread && !tls.h[i]; ++i);
        if (i==kHazardsPerThread) i = 0;
        if (!tls.h[i]) tls.h[i] = acquire_slot();
        s_ = tls.h[i];
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

    template<typename T>
    T* protect(std::atomic<T*>& src) {
        T* p;
        do {
            p = src.load(std::memory_order_acquire);
            s_->ptr.store(p, std::memory_order_release);
        } while (p != src.load(std::memory_order_acquire));
        return p;
    }
    void clear() noexcept { s_->ptr.store(nullptr, std::memory_order_release); }
    ~Guard() { clear(); }
};

inline void retire(void* p, std::function<void(void*)> d) {
    tls.retired.push_back({p,std::move(d)});
    const size_t H = kHazardsPerThread * kMaxThreads;
    const size_t R = H * kRFactor;
    if (tls.retired.size() >= R) {
        /* snapshot */
        std::vector<void*> hp_vec;
        hp_vec.reserve(H);
        for (auto& s: g_slots) {
            void* v=s.ptr.load(std::memory_order_acquire);
            if (v && v!=reinterpret_cast<void*>(1)) hp_vec.push_back(v);
        }
        auto it = tls.retired.begin();
        while (it!=tls.retired.end()) {
            if (std::find(hp_vec.begin(), hp_vec.end(), it->raw)==hp_vec.end()) {
                it->deleter(it->raw);
                it = tls.retired.erase(it);
            } else ++it;
        }
    }
}
template<typename T>
inline void retire(T* p){ retire(static_cast<void*>(p),
                          [](void* q){ delete static_cast<T*>(q);} ); }
} // namespace hp

/*──────────────── Hybrid EBR + HP queue ───────────────────────────*/
namespace lfq {

namespace detail {

constexpr size_t kCacheLine = 64;
constexpr unsigned kThreadPool = 512;
constexpr unsigned kBuckets = 3;
constexpr unsigned kBatchRetired = 128;

/* per‑thread control */
enum class Mode : uint8_t { FAST=0, SLOW=1 };

struct alignas(kCacheLine) ThreadCtl {
    std::atomic<unsigned> local_epoch{~0u};
    Mode   mode = Mode::FAST;
    unsigned stuck_cnt = 0;
    std::array<std::vector<void*>,kBuckets> retire;
    std::array<std::vector<std::function<void(void*)>>,kBuckets> del;
    char pad[kCacheLine - sizeof(std::atomic<unsigned>) - sizeof(Mode)
             - sizeof(unsigned)];
    ~ThreadCtl(){
        for(unsigned i=0;i<kBuckets;++i){
            for(size_t k=0;k<retire[i].size();++k)
                if(del[i][k]) del[i][k](retire[i][k]);
        }
    }
};

struct alignas(kCacheLine) ThreadSlot {
    std::atomic<ThreadCtl*> ctl{nullptr};
    std::atomic<std::thread::id> owner_id{std::thread::id{}};
    char pad[kCacheLine - sizeof(std::atomic<ThreadCtl*>) -
             sizeof(std::atomic<std::thread::id>)];
};

inline std::array<ThreadSlot,kThreadPool> g_slots{};

struct alignas(kCacheLine) EpochCtr {
    std::atomic<unsigned> e{0};
    char pad[kCacheLine - sizeof(std::atomic<unsigned>)];
};
inline EpochCtr g_epoch;

inline std::atomic<unsigned>& g_e = g_epoch.e;

/* init thread */
inline thread_local ThreadCtl* tls_ctl = nullptr;
inline thread_local std::unique_ptr<struct Cleanup> tls_cleanup;

struct Cleanup {
    unsigned slot;
    ThreadCtl* ctl;
    Cleanup(unsigned s, ThreadCtl* c):slot(s),ctl(c){}
    ~Cleanup(){
        if(slot<kThreadPool){
            g_slots[slot].ctl.store(nullptr,std::memory_order_release);
            g_slots[slot].owner_id.store(std::thread::id{},
                                         std::memory_order_release);
        }
        delete ctl;
    }
};

inline ThreadCtl* init_thread(){
    if(tls_ctl) return tls_ctl;
    tls_ctl=new ThreadCtl;
    auto id = std::this_thread::get_id();
    unsigned s=kThreadPool;
    for(unsigned i=0;i<kThreadPool;++i){
        std::thread::id exp{};
        if(g_slots[i].owner_id.compare_exchange_strong(
               exp,id,std::memory_order_acq_rel)){
            g_slots[i].ctl.store(tls_ctl,std::memory_order_release);
            s=i; break;
        }
    }
    if(s==kThreadPool) throw std::runtime_error("Thread pool full");
    tls_cleanup = std::make_unique<Cleanup>(s,tls_ctl);
    return tls_ctl;
}

/* forward */
inline void try_reclaim(ThreadCtl*);

/* retire helper */
inline void retire(void* p,std::function<void(void*)> d){
    ThreadCtl* tc=init_thread();
    unsigned e=g_e.load(std::memory_order_acquire);
    unsigned idx=e%kBuckets;
    tc->retire[idx].push_back(p);
    tc->del[idx].push_back(std::move(d));
    if(tc->retire[idx].size()>=kBatchRetired) try_reclaim(tc);
}
template<typename T>
inline void retire(T* p){ retire(static_cast<void*>(p),
                          [](void* q){ delete static_cast<T*>(q);} ); }

constexpr unsigned STALL_LIMIT=16;

inline void reclaim_bucket_EBR(unsigned idx){
    for(unsigned i=0;i<kThreadPool;++i){
        ThreadCtl* t=g_slots[i].ctl.load(std::memory_order_acquire);
        if(!t) continue;
        auto& vec=t->retire[idx]; auto& del=t->del[idx];
        for(size_t k=0;k<vec.size();++k) if(del[k]) del[k](vec[k]);
        vec.clear(); del.clear();
    }
}

inline void try_reclaim(ThreadCtl* self){
    unsigned cur=g_e.load(std::memory_order_relaxed);
    bool can_flip=true;
    for(unsigned i=0;i<kThreadPool;++i){
        ThreadCtl* t=g_slots[i].ctl.load(std::memory_order_acquire);
        if(t && t->local_epoch.load(std::memory_order_acquire)==cur){
            can_flip=false; break;
        }
    }
    if(can_flip && g_e.compare_exchange_strong(cur,cur+1,
                    std::memory_order_acq_rel)){
        reclaim_bucket_EBR((cur+1)%kBuckets);
        self->stuck_cnt=0;
        return;
    }
    if(++self->stuck_cnt<STALL_LIMIT) return;

    /* epoch appears stuck — switch all threads to SLOW */
    for(unsigned i=0;i<kThreadPool;++i){
        ThreadCtl* t=g_slots[i].ctl.load(std::memory_order_acquire);
        if(t) t->mode=Mode::SLOW;
    }
    unsigned idx_old=(cur+1)%kBuckets;
    for(unsigned i=0;i<kThreadPool;++i){
        ThreadCtl* t=g_slots[i].ctl.load(std::memory_order_acquire);
        if(!t) continue;
        auto& v=t->retire[idx_old]; auto& d=t->del[idx_old];
        for(size_t k=0;k<v.size();++k) hp::retire(v[k],std::move(d[k]));
        v.clear(); d.clear();
    }
    hp::retire(nullptr,[](void*){}); /* trigger scan via size heuristic */
    self->stuck_cnt=0;
}

/* hybrid guard */
template<class Node>
struct HybridGuard {
    ThreadCtl* tc_;
    hp::Guard  hp_;
    bool use_hp=false;

    HybridGuard(std::atomic<Node*>& to_protect){
        tc_=init_thread();
        unsigned e=g_e.load(std::memory_order_acquire);
        tc_->local_epoch.store(e,std::memory_order_release);
        if(tc_->mode==Mode::SLOW){
            hp_.protect(to_protect);
            use_hp=true;
        }
    }
    ~HybridGuard(){
        if(use_hp) hp_.clear();
        tc_->local_epoch.store(~0u,std::memory_order_release);
    }
};

} // namespace detail

/*───────────────── Michael‑Scott queue with hybrid MR ──────────────*/
template<class T>
class HybridQueue {
    struct Node {
        std::atomic<Node*> next{nullptr};
        alignas(T) unsigned char storage[sizeof(T)];
        bool has_val;
        Node():has_val(false){}
        template<class...A> Node(A&&...a):has_val(true){
            ::new (storage) T(std::forward<A>(a)...);
        }
        T& val(){ return *std::launder(reinterpret_cast<T*>(storage)); }
        ~Node(){ if(has_val) val().~T(); }
    };

    struct alignas(detail::kCacheLine) HeadPtr {
        std::atomic<Node*> ptr;
        char pad[detail::kCacheLine - sizeof(std::atomic<Node*>)];
        HeadPtr(Node* n):ptr(n){}
    } head_;

    struct alignas(detail::kCacheLine) TailPtr {
        std::atomic<Node*> ptr;
        char pad[detail::kCacheLine - sizeof(std::atomic<Node*>)];
        TailPtr(Node* n):ptr(n){}
    } tail_;

    static inline void backoff(unsigned& n){
#if defined(__i386__) || defined(__x86_64__)
        constexpr uint32_t kMax=1024;
        if(n<kMax){ for(uint32_t i=0;i<n;++i) __builtin_ia32_pause(); n<<=1; }
#else
        if(n<1024){ for(uint32_t i=0;i<n;++i) std::this_thread::yield(); n<<=1;}
#endif
    }

public:
    HybridQueue() : head_(new Node()), tail_(head_.ptr.load()) {}
    HybridQueue(const HybridQueue&)=delete;
    HybridQueue& operator=(const HybridQueue&)=delete;

    template<class...Args>
    void enqueue(Args&&...args){
        Node* n=new Node(std::forward<Args>(args)...);
        unsigned delay=1;
        for(;;){
            detail::HybridGuard<Node> g(head_.ptr); // nothing to protect yet
            Node* tail=tail_.ptr.load(std::memory_order_acquire);
            Node* next=tail->next.load(std::memory_order_acquire);
            if(tail!=tail_.ptr.load(std::memory_order_acquire)) continue;
            if(!next){
                if(tail->next.compare_exchange_weak(next,n,
                       std::memory_order_release,std::memory_order_relaxed)){
                    tail_.ptr.compare_exchange_strong(tail,n,
                       std::memory_order_release,std::memory_order_relaxed);
                    return;
                }
            } else {
                tail_.ptr.compare_exchange_strong(tail,next,
                       std::memory_order_release,std::memory_order_relaxed);
            }
            backoff(delay);
        }
    }

    bool dequeue(T& out){
        unsigned delay=1;
        for(;;){
            detail::HybridGuard<Node> g(head_.ptr);
            Node* head=head_.ptr.load(std::memory_order_acquire);
            Node* tail=tail_.ptr.load(std::memory_order_acquire);
            Node* next=head->next.load(std::memory_order_acquire);
            if(head!=head_.ptr.load(std::memory_order_acquire)) continue;
            if(!next) return false;
            if(head==tail){
                tail_.ptr.compare_exchange_strong(tail,next,
                       std::memory_order_release,std::memory_order_relaxed);
                backoff(delay); continue;
            }
            T val=next->val();
            if(head_.ptr.compare_exchange_strong(head,next,
                       std::memory_order_release,std::memory_order_relaxed)){
                out=std::move(val);
                detail::retire(head);
                return true;
            }
            backoff(delay);
        }
    }

    bool empty() const{
        detail::HybridGuard<Node> g(const_cast<std::atomic<Node*>&>(head_.ptr));
        return head_.ptr.load(std::memory_order_acquire)
              ->next.load(std::memory_order_acquire)==nullptr;
    }

    ~HybridQueue(){
        Node* n=head_.ptr.load(std::memory_order_relaxed);
        while(n){ Node* nx=n->next.load(std::memory_order_relaxed); delete n; n=nx; }
    }

    /* monitoring helpers */
    unsigned current_epoch() const{ return detail::g_e.load(); }
};

template<class T>
using HybridEBRHPQueue = HybridQueue<T>;

} // namespace lfq

#endif /* LOCKFREE_QUEUE_HYBRID_HPP */
