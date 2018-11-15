/**
 * Copyright (c) 2018 Cornell University.
 *
 * Author: Ted Yin <tederminant@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _SALTICIDAE_EVENT_H
#define _SALTICIDAE_EVENT_H

#include <condition_variable>
#include <unistd.h>
#include <uv.h>
#include <sys/eventfd.h>

#include "salticidae/type.h"
#include "salticidae/queue.h"
#include "salticidae/util.h"
#include "salticidae/ref.h"

namespace salticidae {

struct _event_context_deleter {
    constexpr _event_context_deleter() = default;
    void operator()(uv_loop_t *ptr) {
        if (ptr != nullptr)
        {
            while (uv_loop_close(ptr) == UV_EBUSY)
                uv_run(ptr, UV_RUN_NOWAIT);
            delete ptr;
        }
    }
};

using _event_context_ot = ArcObj<uv_loop_t, _event_context_deleter>;

class EventContext: public _event_context_ot {
    public:
    EventContext(): _event_context_ot(new uv_loop_t()) {
        uv_loop_init(get());
    }
    EventContext(uv_loop_t *eb): _event_context_ot(eb) {}
    EventContext(const EventContext &) = default;
    EventContext(EventContext &&) = default;
    EventContext &operator=(const EventContext &) = default;
    EventContext &operator=(EventContext &&) = default;
    void dispatch() const {
        // TODO: improve this loop
        uv_run(get(), UV_RUN_DEFAULT);
    }
    void stop() const { uv_stop(get()); }
};

class Event {
    public:
    using callback_t = std::function<void(int fd, short events)>;
    static const int READ = UV_READABLE;
    static const int WRITE = UV_WRITABLE;
    static const int TIMEOUT = ~(UV_READABLE | UV_WRITABLE |
                                UV_DISCONNECT | UV_PRIORITIZED);

    private:
    EventContext eb;
    int fd;
    uv_poll_t *ev_fd;
    uv_timer_t *ev_timer;
    callback_t callback;
    static inline void fd_then(uv_poll_t *h, int status, int events) {
        if (status != 0)
        {
            SALTICIDAE_LOG_WARN("%s", uv_strerror(status));
            return;
        }
        auto event = static_cast<Event *>(h->data);
        event->callback(event->fd, events);
    }

    static inline void timer_then(uv_timer_t *h) {
        auto event = static_cast<Event *>(h->data);
        if (event->ev_fd) uv_poll_stop(event->ev_fd);
        event->callback(event->fd, TIMEOUT);
    }

    static void _on_handle_close(uv_handle_t *h) {
        delete h;
    }

    public:
    Event(): eb(nullptr), ev_fd(nullptr), ev_timer(nullptr) {}
    Event(const EventContext &eb, int fd, callback_t callback):
            eb(eb), fd(fd),
            ev_fd(nullptr),
            ev_timer(new uv_timer_t()),
            callback(callback) {
        if (fd != -1)
        {
            ev_fd = new uv_poll_t();
            uv_poll_init(eb.get(), ev_fd, fd);
            ev_fd->data = this;
        }
        uv_timer_init(eb.get(), ev_timer);
        ev_timer->data = this;
    }

    Event(const Event &) = delete;
    Event(Event &&other):
            eb(std::move(other.eb)), fd(other.fd),
            ev_fd(other.ev_fd), ev_timer(other.ev_timer),
            callback(std::move(other.callback)) {
        other.del();
        if (fd != -1)
        {
            other.ev_fd = nullptr;
            ev_fd->data = this;
        }
        other.ev_timer = nullptr;
        ev_timer->data = this;
    }

    Event &operator=(Event &&other) {
        clear();
        other.del();
        eb = std::move(other.eb);
        fd = other.fd;
        ev_fd = other.ev_fd;
        ev_timer = other.ev_timer;
        callback = std::move(other.callback);

        if (fd != -1)
        {
            other.ev_fd = nullptr;
            ev_fd->data = this;
        }
        other.ev_timer = nullptr;
        ev_timer->data = this;
        return *this;
    }

    ~Event() { clear(); }

    void clear() {
        if (ev_fd != nullptr)
        {
            uv_poll_stop(ev_fd);
            uv_close((uv_handle_t *)ev_fd, Event::_on_handle_close);
            ev_fd = nullptr;
        }
        if (ev_timer != nullptr)
        {
            uv_timer_stop(ev_timer);
            uv_close((uv_handle_t *)ev_timer, Event::_on_handle_close);
            ev_timer = nullptr;
        }
        callback = nullptr;
    }

    void set_callback(callback_t _callback) {
        callback = _callback;
    }

    void add(int events) {
        if (ev_fd) uv_poll_start(ev_fd, events, Event::fd_then);
    }
    void del() {
        if (ev_fd) uv_poll_stop(ev_fd);
        if (ev_timer == nullptr)
        assert(ev_timer);
        uv_timer_stop(ev_timer);
    }
    void add_with_timeout(double t_sec, int events) {
        add(events);
        uv_timer_start(ev_timer, Event::timer_then, uint64_t(t_sec * 1000), 0);
    }

    operator bool() const { return ev_fd != nullptr || ev_timer != nullptr; }
};

class ThreadNotifier {
    std::condition_variable cv;
    std::mutex mlock;
    mutex_ul_t ul;
    bool ready;
    void *data;
    public:
    ThreadNotifier(): ul(mlock), ready(false) {}
    void *wait() {
        cv.wait(ul, [this]{ return ready; });
        return data;
    }
    void notify(void *_data) { 
        {
            mutex_lg_t _(mlock);
            ready = true;
            data = _data;
        }
        cv.notify_all();
    }
};

class ThreadCall {
    int ctl_fd[2];
    EventContext ec;
    Event ev_listen;

    public:
    class Handle {
        std::function<void(Handle &)> callback;
        std::function<void(void *)> deleter;
        ThreadNotifier* notifier;
        void *result;
        friend ThreadCall;
        public:
        Handle(): notifier(nullptr), result(nullptr) {}
        void exec() {
            callback(*this);
            if (notifier) notifier->notify(result);
        }
        void set_result(void *data) { result = data; }
        template<typename Func>
        void set_deleter(Func _deleter) { deleter = _deleter; }
    };

    ThreadCall() = default;
    ThreadCall(const ThreadCall &) = delete;
    ThreadCall(ThreadCall &&) = delete;
    ThreadCall(EventContext ec): ec(ec) {
        if (pipe2(ctl_fd, O_NONBLOCK))
            throw SalticidaeError(std::string("ThreadCall: failed to create pipe"));
        ev_listen = Event(ec, ctl_fd[0], [this](int fd, int) {
            Handle *h;
            read(fd, &h, sizeof(h));
            h->exec();
            delete h;
        });
        ev_listen.add(Event::READ);
    }

    ~ThreadCall() {
        ev_listen.clear();
        Handle *h;
        while (read(ctl_fd[0], &h, sizeof(h)) == sizeof(h))
        {
            if (h->result && h->deleter)
                h->deleter(h->result);
            delete h;
        }
        close(ctl_fd[0]);
        close(ctl_fd[1]);
    }

    template<typename Func>
    void *call(Func callback, bool blocking = false) {
        auto h = new Handle();
        h->callback = callback;
        if (blocking)
        {
            ThreadNotifier notifier;
            h->notifier = &notifier;
            std::atomic_thread_fence(std::memory_order_release);
            write(ctl_fd[1], &h, sizeof(h));
            return notifier.wait();
        }
        else
        {
            std::atomic_thread_fence(std::memory_order_release);
            write(ctl_fd[1], &h, sizeof(h));
            return nullptr;
        }
    }
};


template<typename T>
class MPSCQueueEventDriven: public MPSCQueue<T> {
    private:
    const uint64_t dummy = 1;
    std::atomic<bool> wait_sig;
    int fd;
    Event ev;

    public:
    MPSCQueueEventDriven(size_t capacity = 65536):
        MPSCQueue<T>(capacity),
        wait_sig(true),
        fd(eventfd(0, EFD_NONBLOCK)) {}

    ~MPSCQueueEventDriven() {
        ev.clear();
        close(fd);
    }

    template<typename Func>
    void reg_handler(const EventContext &ec, Func &&func) {
        ev = Event(ec, fd,
                    [this, func=std::forward<Func>(func)](int, short) {
                    //fprintf(stderr, "%x\n", std::this_thread::get_id());
                    uint64_t t;
                    read(fd, &t, 8);
                    // the only undesirable case is there are some new items
                    // enqueued before recovering wait_sig to true, so the consumer
                    // won't be notified. In this case, no enqueuing thread will
                    // get to write(fd). Then store(true) must happen after all exchange(false),
                    // since all enqueue operations are finalized, the dequeue should be able
                    // to see those enqueued values in func()
                    wait_sig.store(true, std::memory_order_release);
                    if (func(*this))
                        write(fd, &dummy, 8);
                });
        ev.add(Event::READ);
    }

    void unreg_handler() { ev.clear(); }

    template<typename U>
    bool enqueue(U &&e) {
        static const uint64_t dummy = 1;
        bool ret = MPSCQueue<T>::enqueue(std::forward<U>(e));
        // memory barrier here, so any load/store in enqueue must be finialized
        if (wait_sig.exchange(false, std::memory_order_acq_rel))
        {
            SALTICIDAE_LOG_DEBUG("mpsc notify");
            write(fd, &dummy, 8);
        }
        return ret;
    }
};



// TODO: incorrect MPMCQueueEventDriven impl
/*
template<typename T>
class MPMCQueueEventDriven: public MPMCQueue<T> {
    private:
    const uint64_t dummy = 1;
    std::atomic<bool> wait_sig;
    std::vector<std::pair<BoxObj<Event>, int>> evs;

    public:
    MPMCQueueEventDriven(size_t capacity = 65536):
        MPMCQueue<T>(capacity),
        wait_sig(true) {}

    template<typename Func>
    void listen(const EventContext &ec, Func &&func, size_t burst_size=128) {
        int fd = eventfd(0, EFD_NONBLOCK);
        evs.emplace(evs.end(), std::make_pair(new Event(ec, fd, EV_READ | EV_PERSIST,
            [this, func=std::forward<Func>(func), burst_size](int fd, short) {
            uint64_t t;
            read(fd, &t, 8);
            //fprintf(stderr, "%x\n", std::this_thread::get_id());
            T elem;
            size_t cnt = burst_size;
            while (MPMCQueue<T>::try_dequeue(elem))
            {
                func(std::move(elem));
                if (!--cnt)
                {
                    write(fd, &dummy, 8);
                    return;
                }
            }
            wait_sig.store(true, std::memory_order_relaxed);
        }), fd));
        evs.rbegin()->first->add();
    }

    ~MPMCQueueEventDriven() {
        for (const auto &p: evs)
            close(p.second);
    }

    template<typename U>
    bool enqueue(U &&e) {
        bool ret = MPMCQueue<T>::enqueue(std::forward<U>(e));
        if (wait_sig.exchange(false, std::memory_order_relaxed))
        {
            SALTICIDAE_LOG_DEBUG("mpmc notify");
            for (const auto &p: evs)
                write(p.second, &dummy, 8);
        }
        return ret;
    }
};
*/

}

#endif
