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
#include "salticidae/event.h"
#include "salticidae/util.h"
#include "salticidae/netaddr.h"
#include "salticidae/msg.h"

namespace salticidae {

class SegBuffer {
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

    std::list<buffer_entry_t> buffer;
    size_t _size;

    public:
    SegBuffer(): _size(0) {}
    ~SegBuffer() { clear(); }

    void swap(SegBuffer &other) {
        std::swap(buffer, other.buffer);
        std::swap(_size, other._size);
    }

    SegBuffer(const SegBuffer &other):
        buffer(other.buffer), _size(other._size) {}

    SegBuffer(SegBuffer &&other):
        buffer(std::move(other.buffer)), _size(other._size) {
        other._size = 0;
    }

    SegBuffer &operator=(SegBuffer &&other) {
        if (this != &other)
        {
            SegBuffer tmp(std::move(other));
            tmp.swap(*this);
        }
        return *this;
    }
 
    SegBuffer &operator=(const SegBuffer &other) {
        if (this != &other)
        {
            SegBuffer tmp(other);
            tmp.swap(*this);
        }
        return *this;
    }
   
    void push(bytearray_t &&data) {
        _size += data.size();
        buffer.push_back(buffer_entry_t(std::move(data)));
    }
    
    bytearray_t pop(size_t len) {
        bytearray_t res;
        auto i = buffer.begin();
        while (len && i != buffer.end())
        {
            size_t copy_len = std::min(i->length(), len);
            res.insert(res.end(), i->offset, i->offset + copy_len);
            i->offset += copy_len;
            len -= copy_len;
            if (i->offset == i->data.end())
                i++;
        }
        buffer.erase(buffer.begin(), i);
        _size -= res.size();
        return std::move(res);
    }
    
    size_t size() const { return _size; }
    bool empty() const { return buffer.empty(); }
    
    void clear() {
        buffer.clear();
        _size = 0;
    }
};

class ConnPoolError: public SalticidaeError {
    using SalticidaeError::SalticidaeError;
};

/** Abstraction for connection management. */
class ConnPool {
    public:
    class Conn;
    /** The handle to a bi-directional connection. */
    using conn_t = RcObj<Conn>;
    /** The type of callback invoked when connection status is changed. */
    using conn_callback_t = std::function<void(Conn &)>;

    /** Abstraction for a bi-directional connection. */
    class Conn {
        friend ConnPool;
        public:
        enum ConnMode {
            ACTIVE, /**< the connection is established by connect() */
            PASSIVE, /**< the connection is established by accept() */
        };
    
        private:
        size_t seg_buff_size;
        conn_t self_ref;
        int fd;
        ConnPool *cpool;
        ConnMode mode;
        NetAddr addr;

        SegBuffer send_buffer;
        SegBuffer recv_buffer;

        Event ev_read;
        Event ev_write;
        Event ev_connect;
        /** does not need to wait if true */
        bool ready_send;
    
        void recv_data(evutil_socket_t, short);
        void send_data(evutil_socket_t, short);
        void conn_server(evutil_socket_t, short);

        public:
        Conn(): ready_send(false) {}
        Conn(const Conn &) = delete;
        Conn(Conn &&other) = delete;
    
        virtual ~Conn() {
            SALTICIDAE_LOG_INFO("destroyed %s", std::string(*this).c_str());
        }

        /** Get the handle to itself. */
        conn_t self() { return self_ref; }
        operator std::string() const;
        int get_fd() const { return fd; }
        const NetAddr &get_addr() const { return addr; }
        ConnMode get_mode() const { return mode; }
        ConnPool *get_pool() const { return cpool; }
        SegBuffer &read() { return recv_buffer; }
        /** Set the buffer size used for send/receive data. */
        void set_seg_buff_size(size_t size) { seg_buff_size = size; }

        /** Write data to the connection (non-blocking). The data will be sent
         * whenever I/O is available. */
        void write(bytearray_t &&data) {
            send_buffer.push(std::move(data));
            if (ready_send)
                send_data(fd, EV_WRITE);
        }

        /** Move the send buffer from the other (old) connection. */
        void move_send_buffer(conn_t other) {
            send_buffer = std::move(other->send_buffer);
        }

        /** Terminate the connection. */
        void terminate();

        protected:
        /** Close the IO and clear all on-going or planned events. */
        virtual void on_close() {
            ev_read.clear();
            ev_write.clear();
            ev_connect.clear();
            ::close(fd);
            fd = -1;
            self_ref = nullptr; /* remove the self-cycle */
        }

        /** Called when new data is available. */
        virtual void on_read() {
            if (cpool->read_cb) cpool->read_cb(*this);
        }
        /** Called when the underlying connection is established. */
        virtual void on_setup() {
            if (cpool->conn_cb) cpool->conn_cb(*this);
        }
        /** Called when the underlying connection breaks. */
        virtual void on_teardown() {
            if (cpool->conn_cb) cpool->conn_cb(*this);
        }
    };
    
    private:
    int max_listen_backlog;
    double conn_server_timeout;
    size_t seg_buff_size;
    conn_callback_t read_cb;
    conn_callback_t conn_cb;

    std::unordered_map<int, conn_t> pool;
    int listen_fd;
    Event ev_listen;

    void accept_client(evutil_socket_t, short);
    conn_t add_conn(conn_t conn);

    protected:
    EventContext ec;
    /** Should be implemented by derived class to return a new Conn object. */
    virtual Conn *create_conn() = 0;

    public:
    ConnPool(const EventContext &ec,
            int max_listen_backlog = 10,
            double conn_server_timeout = 2,
            size_t seg_buff_size = 4096):
        max_listen_backlog(max_listen_backlog),
        conn_server_timeout(conn_server_timeout),
        seg_buff_size(seg_buff_size),
        ec(ec) {}

    ~ConnPool() {
        for (auto it: pool)
        {
            conn_t conn = it.second;
            conn->on_close();
        }
    }

    ConnPool(const ConnPool &) = delete;
    ConnPool(ConnPool &&) = delete;

    /** Actively connect to remote addr. */
    conn_t connect(const NetAddr &addr);
    /** Listen for passive connections (connection initiated from remote).
     * Does not need to be called if do not want to accept any passive
     * connections. */
    void listen(NetAddr listen_addr);

    template<typename Func>
    void reg_conn_handler(Func cb) { conn_cb = cb; }
};

}

#endif
