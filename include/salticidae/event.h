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

static void _on_uv_handle_close(uv_handle_t *h) { delete h; }

class FdEvent {
    public:
    using callback_t = std::function<void(int fd, int events)>;
    static const int READ = UV_READABLE;
    static const int WRITE = UV_WRITABLE;
    static const int ERROR = 1 << 30;

    protected:
    EventContext ec;
    int fd;
    uv_poll_t *ev_fd;
    callback_t callback;

    static inline void fd_then(uv_poll_t *h, int status, int events) {
        if (status != 0)
            events |= ERROR;
        auto event = static_cast<FdEvent *>(h->data);
        event->callback(event->fd, events);
    }

    public:
    FdEvent(): ec(nullptr), ev_fd(nullptr) {}
    FdEvent(const EventContext &ec, int fd, callback_t callback):
            ec(ec), fd(fd), ev_fd(new uv_poll_t()),
            callback(std::move(callback)) {
        uv_poll_init(ec.get(), ev_fd, fd);
        ev_fd->data = this;
    }

    FdEvent(const FdEvent &) = delete;
    FdEvent(FdEvent &&other):
            ec(std::move(other.ec)), fd(other.fd), ev_fd(other.ev_fd),
            callback(std::move(other.callback)) {
        other.ev_fd = nullptr;
        if (ev_fd != nullptr)
            ev_fd->data = this;
    }
    
    void swap(FdEvent &other) {
        std::swap(ec, other.ec);
        std::swap(fd, other.fd);
        std::swap(ev_fd, other.ev_fd);
        std::swap(callback, other.callback);
        if (ev_fd != nullptr)
            ev_fd->data = this;
        if (other.ev_fd != nullptr)
            other.ev_fd->data = &other;
    }

    FdEvent &operator=(FdEvent &&other) {
        if (this != &other)
        {
            FdEvent tmp(std::move(other));
            tmp.swap(*this);
        }
        return *this;
    }

    ~FdEvent() { clear(); }

    void clear() {
        if (ev_fd != nullptr)
        {
            uv_poll_stop(ev_fd);
            uv_close((uv_handle_t *)ev_fd, _on_uv_handle_close);
            ev_fd = nullptr;
        }
        callback = nullptr;
    }

    void set_callback(callback_t _callback) {
        callback = _callback;
    }

    void add(int events) {
        assert(ev_fd != nullptr);
        uv_poll_start(ev_fd, events, FdEvent::fd_then);
    }

    void del() {
        if (ev_fd != nullptr) uv_poll_stop(ev_fd);
    }

    operator bool() const { return ev_fd != nullptr; }
};


class TimerEvent {
    public:
    using callback_t = std::function<void(TimerEvent &)>;

    protected:
    EventContext ec;
    uv_timer_t *ev_timer;
    callback_t callback;

    static inline void timer_then(uv_timer_t *h) {
        auto event = static_cast<TimerEvent *>(h->data);
        event->callback(*event);
    }

    public:
    TimerEvent(): ec(nullptr), ev_timer(nullptr) {}
    TimerEvent(const EventContext &ec, callback_t callback):
            ec(ec), ev_timer(new uv_timer_t()),
            callback(std::move(callback)) {
        uv_timer_init(ec.get(), ev_timer);
        ev_timer->data = this;
    }

    TimerEvent(const TimerEvent &) = delete;
    TimerEvent(TimerEvent &&other):
            ec(std::move(other.ec)), ev_timer(other.ev_timer),
            callback(std::move(other.callback)) {
        other.ev_timer = nullptr;
        if (ev_timer != nullptr)
            ev_timer->data = this;
    }

    void swap(TimerEvent &other) {
        std::swap(ec, other.ec);
        std::swap(ev_timer, other.ev_timer);
        std::swap(callback, other.callback);
        if (ev_timer != nullptr)
            ev_timer->data = this;
        if (other.ev_timer != nullptr)
            other.ev_timer->data = &other;
    }

    TimerEvent &operator=(TimerEvent &&other) {
        if (this != &other)
        {
            TimerEvent tmp(std::move(other));
            tmp.swap(*this);
        }
        return *this;
    }

    ~TimerEvent() { clear(); }

    void clear() {
        if (ev_timer != nullptr)
        {
            uv_timer_stop(ev_timer);
            uv_close((uv_handle_t *)ev_timer, _on_uv_handle_close);
            ev_timer = nullptr;
        }
        callback = nullptr;
    }

    void set_callback(callback_t _callback) {
        callback = _callback;
    }

    void add(double t_sec) {
        assert(ev_timer != nullptr);
        uv_timer_start(ev_timer, TimerEvent::timer_then, uint64_t(t_sec * 1000), 0);
    }

    void del() {
        if (ev_timer != nullptr) uv_timer_stop(ev_timer);
    }

    operator bool() const { return ev_timer != nullptr; }
};

class TimedFdEvent: public FdEvent, public TimerEvent {
    public:
    static const int TIMEOUT = 1 << 29;

    private:
    FdEvent::callback_t callback;
    uint64_t timeout;

    static inline void timer_then(uv_timer_t *h) {
        auto event = static_cast<TimedFdEvent *>(h->data);
        event->FdEvent::del();
        event->callback(event->fd, TIMEOUT);
    }

    static inline void fd_then(uv_poll_t *h, int status, int events) {
        if (status != 0)
            events |= ERROR;
        auto event = static_cast<TimedFdEvent *>(h->data);
        event->TimerEvent::del();
        uv_timer_start(event->ev_timer, TimedFdEvent::timer_then,
                        event->timeout, 0);
        event->callback(event->fd, events);
    }

    public:
    TimedFdEvent() = default;
    TimedFdEvent(const EventContext &ec, int fd, FdEvent::callback_t callback):
            FdEvent(ec, fd, FdEvent::callback_t()),
            TimerEvent(ec, TimerEvent::callback_t()),
            callback(std::move(callback)) {
        ev_fd->data = this;
        ev_timer->data = this;
    }

    TimedFdEvent(TimedFdEvent &&other):
            FdEvent(static_cast<FdEvent &&>(other)),
            TimerEvent(static_cast<TimerEvent &&>(other)),
            callback(std::move(other.callback)),
            timeout(other.timeout) {
        if (ev_fd != nullptr)
        {
            ev_timer->data = this;
            ev_fd->data = this;
        }
    }

    void swap(TimedFdEvent &other) {
        std::swap(static_cast<FdEvent &>(*this), static_cast<FdEvent &>(other));
        std::swap(static_cast<TimerEvent &>(*this), static_cast<TimerEvent &>(other));
        std::swap(callback, other.callback);
        std::swap(timeout, other.timeout);
        if (ev_fd != nullptr)
            ev_fd->data = ev_timer->data = this;
        if (other.ev_fd != nullptr)
            other.ev_fd->data = other.ev_timer->data = &other;
    }

    TimedFdEvent &operator=(TimedFdEvent &&other) {
        if (this != &other)
        {
            TimedFdEvent tmp(std::move(other));
            tmp.swap(*this);
        }
        return *this;
    }

    void clear() {
        TimerEvent::clear();
        FdEvent::clear();
    }

    using FdEvent::set_callback;

    void add(int events) = delete;
    void add(double t_sec) = delete;

    void set_callback(FdEvent::callback_t _callback) {
        callback = _callback;
    }

    void add(int events, double t_sec) {
        assert(ev_fd != nullptr && ev_timer != nullptr);
        uv_timer_start(ev_timer, TimedFdEvent::timer_then,
                        timeout = uint64_t(t_sec * 1000), 0);
        uv_poll_start(ev_fd, events, TimedFdEvent::fd_then);
    }

    void del() {
        TimerEvent::del();
        FdEvent::del();
    }

    operator bool() const { return ev_fd != nullptr; }
};

class SigEvent {
    public:
    using callback_t = std::function<void(int signum)>;
    private:
    EventContext ec;
    uv_signal_t *ev_sig;
    callback_t callback;
    static inline void sig_then(uv_signal_t *h, int signum) {
        auto event = static_cast<SigEvent *>(h->data);
        event->callback(signum);
    }

    public:
    SigEvent(): ec(nullptr), ev_sig(nullptr) {}
    SigEvent(const EventContext &ec, callback_t callback):
            ec(ec), ev_sig(new uv_signal_t()),
            callback(std::move(callback)) {
        uv_signal_init(ec.get(), ev_sig);
        ev_sig->data = this;
    }

    SigEvent(const SigEvent &) = delete;
    SigEvent(SigEvent &&other):
            ec(std::move(other.ec)), ev_sig(other.ev_sig),
            callback(std::move(other.callback)) {
        other.ev_sig = nullptr;
        if (ev_sig != nullptr)
            ev_sig->data = this;
    }

    void swap(SigEvent &other) {
        std::swap(ec, other.ec);
        std::swap(ev_sig, other.ev_sig);
        std::swap(callback, other.callback);
        if (ev_sig != nullptr)
            ev_sig->data = this;
        if (other.ev_sig != nullptr)
            other.ev_sig->data = &other;
    }

    SigEvent &operator=(SigEvent &&other) {
        if (this != &other)
        {
            SigEvent tmp(std::move(other));
            tmp.swap(*this);
        }
        return *this;
    }

    ~SigEvent() { clear(); }

    void clear() {
        if (ev_sig != nullptr)
        {
            uv_signal_stop(ev_sig);
            uv_close((uv_handle_t *)ev_sig, _on_uv_handle_close);
            ev_sig = nullptr;
        }
        callback = nullptr;
    }

    void set_callback(callback_t _callback) {
        callback = _callback;
    }

    void add(int signum) {
        assert(ev_sig != nullptr);
        uv_signal_start(ev_sig, SigEvent::sig_then, signum);
    }

    void add_once(int signum) {
        assert(ev_sig != nullptr);
        uv_signal_start_oneshot(ev_sig, SigEvent::sig_then, signum);
    }

    void del() {
        if (ev_sig != nullptr) uv_signal_stop(ev_sig);
    }

    operator bool() const { return ev_sig != nullptr; }
};

template<typename T>
class ThreadNotifier {
    std::condition_variable cv;
    std::mutex mlock;
    mutex_ul_t ul;
    bool ready;
    T data;
    public:
    ThreadNotifier(): ul(mlock), ready(false) {}
    T wait() {
        cv.wait(ul, [this]{ return ready; });
        return std::move(data);
    }
    void notify(T &&_data) {
        mutex_lg_t _(mlock);
        ready = true;
        data = std::move(_data);
        cv.notify_all();
    }
};

template<typename T>
class MPSCQueueEventDriven: public MPSCQueue<T> {
    private:
    const uint64_t dummy = 1;
    std::atomic<bool> wait_sig;
    int fd;
    FdEvent ev;

    public:
    MPSCQueueEventDriven():
        wait_sig(true),
        fd(eventfd(0, EFD_NONBLOCK)) {}

    ~MPSCQueueEventDriven() {
        ev.clear();
        close(fd);
    }

    template<typename Func>
    void reg_handler(const EventContext &ec, Func &&func) {
        ev = FdEvent(ec, fd,
                    [this, func=std::forward<Func>(func)](int, int) {
                    //fprintf(stderr, "%x\n", std::this_thread::get_id());
                    uint64_t t;
                    read(fd, &t, 8);
                    // the only undesirable case is there are some new items
                    // enqueued before recovering wait_sig to true, so the consumer
                    // won't be notified. In this case, no enqueuing thread will
                    // get to write(fd). Then store(true) must happen after all exchange(false),
                    // since all enqueue operations are finalized, the dequeue should be able
                    // to see those enqueued values in func()
                    wait_sig.exchange(true, std::memory_order_acq_rel);
                    if (func(*this))
                        write(fd, &dummy, 8);
                });
        ev.add(FdEvent::READ);
    }

    void unreg_handler() { ev.clear(); }

    template<typename U>
    bool enqueue(U &&e, bool unbounded = true) {
        static const uint64_t dummy = 1;
        if (!MPSCQueue<T>::enqueue(std::forward<U>(e), unbounded))
            return false;
        // memory barrier here, so any load/store in enqueue must be finialized
        if (wait_sig.exchange(false, std::memory_order_acq_rel))
        {
            SALTICIDAE_LOG_DEBUG("mpsc notify");
            write(fd, &dummy, 8);
        }
        return true;
    }

    template<typename U> bool try_enqueue(U &&e) = delete;
};

template<typename T>
class MPMCQueueEventDriven: public MPMCQueue<T> {
    private:
    const uint64_t dummy = 1;
    std::atomic<bool> wait_sig;
    int fd;
    std::vector<FdEvent> evs;

    public:
    MPMCQueueEventDriven():
        wait_sig(true),
        fd(eventfd(0, EFD_NONBLOCK)) {}

    ~MPMCQueueEventDriven() {
        evs.clear();
        close(fd);
    }

    // this function is *NOT* thread-safe
    template<typename Func>
    void reg_handler(const EventContext &ec, Func &&func) {
        FdEvent ev(ec, fd, [this, func=std::forward<Func>(func)](int, int) {
            //fprintf(stderr, "%x\n", std::this_thread::get_id());
            uint64_t t;
            if (read(fd, &t, 8) != 8) return;
            // only one consumer should be here a a time
            wait_sig.exchange(true, std::memory_order_acq_rel);
            if (func(*this))
                write(fd, &dummy, 8);
        });
        ev.add(FdEvent::READ);
        evs.push_back(std::move(ev));
    }

    void unreg_handlers() { evs.clear(); }

    template<typename U>
    bool enqueue(U &&e, bool unbounded = true) {
        static const uint64_t dummy = 1;
        if (!MPMCQueue<T>::enqueue(std::forward<U>(e), unbounded))
            return false;
        // memory barrier here, so any load/store in enqueue must be finialized
        if (wait_sig.exchange(false, std::memory_order_acq_rel))
        {
            SALTICIDAE_LOG_DEBUG("mpsc notify");
            write(fd, &dummy, 8);
        }
        return true;
    }

    template<typename U> bool try_enqueue(U &&e) = delete;
};

class ThreadCall {
    public: class Handle;
    private:
    int ctl_fd[2];
    EventContext ec;
    const size_t burst_size;
    using queue_t = MPSCQueueEventDriven<Handle *>;
    queue_t q;

    public:
    struct Result {
        void *data;
        std::function<void(void *)> deleter;
        Result(): data(nullptr) {}
        Result(void *data, std::function<void(void *)> &&deleter):
            data(data), deleter(std::move(deleter)) {}
        ~Result() { if (data != nullptr) deleter(data); }
        Result(const Result &) = delete;
        Result(Result &&other):
                data(other.data), deleter(std::move(other.deleter)) {
            other.data = nullptr;
        }
        void swap(Result &other) {
            std::swap(data, other.data);
            std::swap(deleter, other.deleter);
        }
        Result &operator=(const Result &other) = delete;
        Result &operator=(Result &&other) {
            if (this != &other)
            {
                Result tmp(std::move(other));
                tmp.swap(*this);
            }
            return *this;
        }
        void *get() { return data; }
    };
    class Handle {
        std::function<void(Handle &)> callback;
        ThreadNotifier<Result> * notifier;
        Result result;
        friend ThreadCall;
        public:
        Handle(): notifier(nullptr) {}
        void exec() {
            callback(*this);
            if (notifier)
                notifier->notify(std::move(result));
        }
        template<typename T>
        void set_result(T &&data) {
            using _T = std::remove_reference_t<T>;
            result = Result(new _T(std::forward<T>(data)),
                            [](void *ptr) {delete static_cast<_T *>(ptr);});
        }
    };

    ThreadCall(size_t burst_size): burst_size(burst_size) {}
    ThreadCall(const ThreadCall &) = delete;
    ThreadCall(ThreadCall &&) = delete;
    ThreadCall(EventContext ec, size_t burst_size = 128): ec(ec), burst_size(burst_size) {
        q.reg_handler(ec, [this, burst_size=burst_size](queue_t &q) {
            size_t cnt = 0;
            Handle *h;
            while (q.try_dequeue(h))
            {
                h->exec();
                delete h;
                if (++cnt == burst_size) return true;
            }
            return false;
        });
    }

    ~ThreadCall() {
        Handle *h;
        while (q.try_dequeue(h)) delete h;
        close(ctl_fd[0]);
        close(ctl_fd[1]);
    }

    template<typename Func>
    void async_call(Func callback) {
        auto h = new Handle();
        h->callback = callback;
        q.enqueue(h);
    }

    template<typename Func>
    Result call(Func callback) {
        auto h = new Handle();
        h->callback = callback;
        ThreadNotifier<Result> notifier;
        h->notifier = &notifier;
        q.enqueue(h);
        return notifier.wait();
    }

    const EventContext &get_ec() const { return ec; }
};

}

#endif
