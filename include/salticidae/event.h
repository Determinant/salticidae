#ifndef _SALTICIDAE_EVENT_H
#define _SALTICIDAE_EVENT_H

namespace salticidae {

struct _event_context_deleter {
    constexpr _event_context_deleter() = default;
    void operator()(struct event_base *ptr) {
        event_base_free(ptr);
    }
};

using _event_context_ot = RcObj<struct event_base, _event_context_deleter>;

class EventContext: public _event_context_ot {
    public:
    EventContext(): _event_context_ot(event_base_new()) {}
    EventContext(const EventContext &) = default;
    EventContext(EventContext &&) = default;
    EventContext &operator=(const EventContext &) = default;
    EventContext &operator=(EventContext &&) = default;
    void dispatch() { event_base_dispatch(get()); }
};

class Event {
    public:
    using callback_t = std::function<void(evutil_socket_t fd, short events)>;

    private:
    EventContext eb;
    evutil_socket_t fd;
    short events;
    struct event *ev;
    callback_t callback;
    static inline void _then(evutil_socket_t fd, short events, void *arg) {
        (static_cast<Event *>(arg))->callback(fd, events);
    }

    public:
    Event(): ev(nullptr) {}
    Event(const EventContext &eb,
        evutil_socket_t fd,
        short events,
        callback_t callback):
            eb(eb), fd(fd), events(events),
            ev(event_new(eb.get(), fd, events, Event::_then, this)),
            callback(callback) {}

    Event(Event &&other):
            eb(std::move(other.eb)), fd(other.fd), events(other.events),
            callback(std::move(other.callback)) {
        other.clear();
        ev = event_new(eb.get(), fd, events, Event::_then, this);
    }

    Event &operator=(Event &&other) {
        clear();
        other.clear();
        eb = std::move(other.eb);
        fd = other.fd;
        events = other.events;
        ev = event_new(eb.get(), fd, events, Event::_then, this);
        callback = std::move(other.callback);
        return *this;
    }

    ~Event() { clear(); }

    void clear() {
        if (ev != nullptr)
        {
            event_del(ev);
            event_free(ev);
            ev = nullptr;
        }
    }

    void add() { event_add(ev, nullptr); }
    void del() { event_del(ev); }
    void add_with_timeout(double timeout) {
        event_add_with_timeout(ev, timeout);
    }

    operator bool() const { return ev != nullptr; }
};

}

#endif
