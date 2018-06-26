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

#ifndef _SALTICIDAE_CONN_H
#define _SALTICIDAE_CONN_H

#include <cassert>
#include <cstdint>
#include <event2/event.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <string>
#include <unordered_map>
#include <list>
#include <algorithm>
#include <exception>

#include "salticidae/type.h"
#include "salticidae/ref.h"
#include "salticidae/util.h"
#include "salticidae/netaddr.h"
#include "salticidae/msg.h"

const int MAX_LISTEN_BACKLOG = 10;
const size_t BUFF_SEG_SIZE = 4096;
const size_t MAX_MSG_HANDLER = 64;
const double TRY_CONN_DELAY = 2;
const double CONN_SERVER_TIMEOUT = 2;

namespace salticidae {

inline double gen_rand_timeout(double base_timeout) {
    return base_timeout + rand() / (double)RAND_MAX * 0.5 * base_timeout;
}

class RingBuffer {
    struct buffer_entry_t {
        bytearray_t data;
        bytearray_t::iterator offset;
        buffer_entry_t(bytearray_t &&_data): data(std::move(_data)) {
            offset = data.begin();
        }

        buffer_entry_t(buffer_entry_t &&other) {
            size_t _offset = other.offset - other.data.begin();
            data = std::move(other.data);
            offset = data.begin() + _offset;
        }

        buffer_entry_t(const buffer_entry_t &other): data(other.data) {
            offset = data.begin() + (other.offset - other.data.begin());
        }

        size_t length() const { return data.end() - offset; }
    };
    std::list<buffer_entry_t> ring;
    size_t _size;

    public:
    RingBuffer(): _size(0) {}
    ~RingBuffer() { clear(); }
    RingBuffer &operator=(const RingBuffer &other) = delete;
    RingBuffer(const RingBuffer &other) = delete;
    RingBuffer &operator=(RingBuffer &&other) {
        ring = std::move(other.ring);
        _size = other._size;
        other._size = 0;
        return *this;
    }
    
    void push(bytearray_t &&data) {
        _size += data.size();
        ring.push_back(buffer_entry_t(std::move(data)));
    }
    
    bytearray_t pop(size_t len) {
        bytearray_t res;
        auto i = ring.begin();
        while (len && i != ring.end())
        {
            size_t copy_len = std::min(i->length(), len);
            res.insert(res.end(), i->offset, i->offset + copy_len);
            i->offset += copy_len;
            len -= copy_len;
            if (i->offset == i->data.end())
                i++;
        }
        ring.erase(ring.begin(), i);
        _size -= res.size();
        return std::move(res);
    }
    
    size_t size() const { return _size; }
    
    void clear() {
        ring.clear();
        _size = 0;
    }
};

class ConnPoolError: public SalticidaeError {
    using SalticidaeError::SalticidaeError;
};

/** The connection pool. */
class ConnPool {
    public:
    class Conn;
    using conn_t = RcObj<Conn>;
    /** The abstraction for a bi-directional connection. */
    class Conn {
        public:
        enum ConnMode {
            ACTIVE, /**< the connection is established by connect() */
            PASSIVE, /**< the connection is established by accept() */
        };
    
        private:
        conn_t self_ref;
        int fd;
        ConnPool *cpool;
        ConnMode mode;
        NetAddr addr;

        RingBuffer send_buffer;
        RingBuffer recv_buffer;

        Event ev_read;
        Event ev_write;
        Event ev_connect;
        /** does not need to wait if true */
        bool ready_send;
    
        void recv_data(evutil_socket_t, short);
        void send_data(evutil_socket_t, short);
        void conn_server(evutil_socket_t, short);
        void try_conn(evutil_socket_t, short);

        public:
        friend ConnPool;
        Conn(): self_ref(this) {}
    
        virtual ~Conn() {
            SALTICIDAE_LOG_INFO("destroyed connection %s", std::string(*this).c_str());
        }

        conn_t self() { return self_ref; }
        operator std::string() const;
        int get_fd() const { return fd; }
        const NetAddr &get_addr() const { return addr; }
        ConnMode get_mode() const { return mode; }
        RingBuffer &read() { return recv_buffer; }

        void write(bytearray_t &&data) {
            send_buffer.push(std::move(data));
            if (ready_send)
                send_data(fd, EV_WRITE);
        }

        void move_send_buffer(conn_t other) {
            send_buffer = std::move(other->send_buffer);
        }

        void terminate();

        protected:
        /** close the connection and free all on-going or planned events. */
        virtual void close() {
            ev_read.clear();
            ev_write.clear();
            ev_connect.clear();
            ::close(fd);
            fd = -1;
        }

        virtual void on_read() = 0;
        virtual void on_setup() = 0;
        virtual void on_teardown() = 0;
    };
    
    private:
    std::unordered_map<int, conn_t> pool;
    int listen_fd;
    Event ev_listen;

    void accept_client(evutil_socket_t, short);
    conn_t add_conn(conn_t conn);

    protected:
    struct event_base *eb;
    virtual conn_t create_conn() = 0;

    public:
    friend Conn;
    ConnPool(struct event_base *eb): eb(eb) {}

    ~ConnPool() {
        for (auto it: pool)
        {
            conn_t conn = it.second;
            conn->close();
        }
    }

    /** create an active mode connection to addr */
    conn_t create_conn(const NetAddr &addr);
    /** setup and start listening */
    void init(NetAddr listen_addr);
};

}

#endif
