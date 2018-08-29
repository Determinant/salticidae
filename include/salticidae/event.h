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

#include <event2/event.h>

#include "salticidae/util.h"
#include "salticidae/ref.h"

namespace salticidae {

struct _event_context_deleter {
    constexpr _event_context_deleter() = default;
    void operator()(struct event_base *ptr) {
        if (ptr != nullptr) event_base_free(ptr);
    }
};

using _event_context_ot = RcObj<struct event_base, _event_context_deleter>;

class EventContext: public _event_context_ot {
    public:
    EventContext(): _event_context_ot(event_base_new()) {}
    EventContext(struct event_base *eb): _event_context_ot(eb) {}
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
    Event(): eb(nullptr), ev(nullptr) {}
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

    void add() { if (ev) event_add(ev, nullptr); }
    void del() { if (ev) event_del(ev); }
    void add_with_timeout(double timeout) {
        if (ev)
            event_add_with_timeout(ev, timeout);
    }

    operator bool() const { return ev != nullptr; }
};

}

#endif
