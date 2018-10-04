#ifndef _SALTICIDAE_QUEUE_H
#define _SALTICIDAE_QUEUE_H

#include <atomic>
#include <vector>
#include <cassert>

namespace salticidae {

static size_t const cacheline_size = 64;

class FreeList {
    public:
    struct Node {
        std::atomic<Node *> next;
        std::atomic<bool> addback;
        std::atomic<size_t> refcnt;
        Node(): next(nullptr), addback(false), refcnt(1) {}
    };

    private:
    alignas(cacheline_size) std::atomic<Node *> top;

    public:
    FreeList(): top(nullptr) {}
    FreeList(const FreeList &) = delete;
    FreeList(FreeList &&) = delete;

    void release_ref(Node *u) {
        if (u->refcnt.fetch_sub(1, std::memory_order_relaxed) != 1) return;
        u->addback.store(false, std::memory_order_relaxed);
        //if (!u->addback.exchange(false, std::memory_order_relaxed)) return;
        for (;;)
        {
            auto t = top.load(std::memory_order_acquire);
            // repair the next pointer before CAS, otherwise u->next == nullptr
            // could lead to skipping elements
            u->next.store(t, std::memory_order_consume);
            // the replacement is ok even if ABA happens
            if (top.compare_exchange_weak(t, u, std::memory_order_consume))
            {
                u->refcnt.store(1, std::memory_order_relaxed);
                break;
            }
        }
    }

    bool push(Node *u) {
        assert(u->addback.load() == false);
        // attempt to push it
        u->addback.store(true, std::memory_order_release);
        release_ref(u);
        return true;
    }

    bool pop(Node *&r) {
        bool loop = true;
        while (loop)
        {
            auto u = top.load(std::memory_order_acquire);
            /* the list is now empty */
            if (u == nullptr) return false;
            auto t = u->refcnt.load(std::memory_order_acquire);
            /* let's wait for another round if u is a ghost (already popped) */
            if (!t) continue;
            /* otherwise t > 0, so with CAS, the invariant that zero refcnt can
             * never be increased is guaranteed */
            if (u->refcnt.compare_exchange_weak(t, t + 1, std::memory_order_consume))
            {
                /* here, nobody is able to change v->next (because v->next is
                 * only changed when pushed) even when ABA happens */
                auto v = u;
                auto nv = u->next.load(std::memory_order_acquire);
                if (top.compare_exchange_weak(v, nv, std::memory_order_consume))
                {
                    /* manage to pop the head */
                    r = u;
                    loop = false;
                    /* do not need to try cas_push here because the current
                     * thread is the only one who can push u back */
                }
                /* release the refcnt and execute the delayed push call if
                 * necessary */
                release_ref(u);
            }
        }
        return true;
    }
};

template<typename T>
class MPMCQueue {
    struct Block: public FreeList::Node {
        T elem;
        std::atomic<Block *> next;
    };

    FreeList blks;

    alignas(cacheline_size) std::atomic<Block *> head;
    alignas(cacheline_size) std::atomic<Block *> tail;

    template<typename U>
    void _enqueue(Block *nblk, U &&e) {
        new (&(nblk->elem)) T(std::forward<U>(e));
        nblk->next.store(nullptr, std::memory_order_release);
        auto prev = tail.exchange(nblk, std::memory_order_relaxed);
        prev->next.store(nblk, std::memory_order_relaxed);
    }

    public:
    MPMCQueue(const MPMCQueue &) = delete;
    MPMCQueue(MPMCQueue &&) = delete;

    MPMCQueue(size_t capacity = 65536): head(new Block()), tail(head.load()) {
        head.load()->next = nullptr;
        while (capacity--)
            blks.push(new Block());
    }

    ~MPMCQueue() {
        for (FreeList::Node *ptr; blks.pop(ptr); ) delete ptr;
        for (Block *ptr = head.load(), *nptr; ptr; ptr = nptr)
        {
            nptr = ptr->next;
            delete ptr;
        }
    }

    template<typename U>
    bool enqueue(U &&e) {
        FreeList::Node * _nblk;
        if (!blks.pop(_nblk)) _nblk = new Block();
        _enqueue(static_cast<Block *>(_nblk), std::forward<U>(e));
        return true;
    }

    template<typename U>
    bool try_enqueue(U &&e) {
        FreeList::Node * _nblk;
        if (!blks.pop(_nblk)) return false;
        _enqueue(static_cast<Block *>(_nblk), std::forward<U>(e));
        return true;
    }

    bool try_dequeue(T &e) {
        for (;;)
        {
            auto h = head.load(std::memory_order_acquire);
            auto t = h->refcnt.load(std::memory_order_relaxed);
            if (!t) continue;
            if (h->refcnt.compare_exchange_weak(t, t + 1, std::memory_order_consume))
            {
                auto nh = h->next.load(std::memory_order_relaxed);
                if (nh == nullptr)
                {
                    blks.release_ref(h);
                    return false;
                }
                e = std::move(nh->elem);
                auto hh = h;
                if (head.compare_exchange_weak(hh, nh, std::memory_order_consume))
                {
                    blks.release_ref(h);
                    blks.push(h);
                    return true;
                }
                blks.release_ref(h);
            }
        }
    }
};

}

#endif
