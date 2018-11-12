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
#include <mutex>
#include <thread>
#include <fcntl.h>

#include "salticidae/type.h"
#include "salticidae/ref.h"
#include "salticidae/event.h"
#include "salticidae/util.h"
#include "salticidae/netaddr.h"
#include "salticidae/msg.h"
#include "salticidae/buffer.h"

namespace salticidae {

struct ConnPoolError: public SalticidaeError {
    using SalticidaeError::SalticidaeError;
};

/** Abstraction for connection management. */
class ConnPool {
    public:
    class Conn;
    /** The handle to a bi-directional connection. */
    using conn_t = ArcObj<Conn>;
    /** The type of callback invoked when connection status is changed. */
    using conn_callback_t = std::function<void(Conn &, bool)>;

    /** Abstraction for a bi-directional connection. */
    class Conn {
        friend ConnPool;
        public:
        enum ConnMode {
            ACTIVE, /**< the connection is established by connect() */
            PASSIVE, /**< the connection is established by accept() */
        };
    
        protected:
        size_t seg_buff_size;
        conn_t self_ref;
        int fd;
        ConnPool *cpool;
        ConnMode mode;
        NetAddr addr;

        // TODO: send_buffer should be a thread-safe mpsc queue
        MPSCWriteBuffer send_buffer;
        SegBuffer recv_buffer;

        Event ev_read;
        Event ev_write;
        Event ev_connect;
        /** does not need to wait if true */
        bool ready_send;
    
        void recv_data(evutil_socket_t, short);
        void send_data(evutil_socket_t, short);
        void conn_server(evutil_socket_t, short);

        /** Terminate the connection. */
        void terminate();

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
        const NetAddr &get_addr() const { return addr; }
        ConnMode get_mode() const { return mode; }
        ConnPool *get_pool() const { return cpool; }
        MPSCWriteBuffer &get_send_buffer() { return send_buffer; }

        /** Write data to the connection (non-blocking). The data will be sent
         * whenever I/O is available. */
        void write(bytearray_t &&data) {
            send_buffer.push(std::move(data));
        }

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
        virtual void on_read() {}
        /** Called when the underlying connection is established. */
        virtual void on_setup() {}
        /** Called when the underlying connection breaks. */
        virtual void on_teardown() {}
    };

    private:
    const int max_listen_backlog;
    const double conn_server_timeout;
    const size_t seg_buff_size;

    /* owned by user loop */
    int mlisten_fd[2]; /**< for connection events sent to the user loop */
    Event ev_mlisten;
    conn_callback_t conn_cb; 

    /* owned by the dispatcher */
    std::unordered_map<int, conn_t> pool;
    int listen_fd;  /**< for accepting new network connections */
    Event ev_listen;
    Event ev_dlisten;
    std::mutex cp_mlock;

    void update_conn(const conn_t &conn, bool connected) {
        auto dcmd = new UserConn(conn, connected);
        write(mlisten_fd[1], &dcmd, sizeof(dcmd));
    }

    struct Worker;
    class WorkerFeed;

    class WorkerCmd {
        public:
        virtual ~WorkerCmd() = default;
        virtual void exec(Worker *worker) = 0;
    };

    class Worker {
        EventContext ec;
        Event ev_ctl;
        int ctl_fd[2]; /**< for control messages from dispatcher */
        std::thread handle;

        public:
        Worker() {
            if (pipe2(ctl_fd, O_NONBLOCK))
                throw ConnPoolError(std::string("failed to create worker pipe"));
            ev_ctl = Event(ec, ctl_fd[0], EV_READ | EV_PERSIST, [this](int fd, short) {
                WorkerCmd *dcmd;
                read(fd, &dcmd, sizeof(dcmd));
                dcmd->exec(this);
                delete dcmd;
            });
            ev_ctl.add();
        }

        ~Worker() {
            close(ctl_fd[0]);
            close(ctl_fd[1]);
        }

        /* the following functions are called by the dispatcher */
        void start() {
            handle = std::thread([this]() { ec.dispatch(); });
        }

        void feed(const conn_t &conn, int client_fd) {
            auto dcmd = new WorkerFeed(conn, client_fd);
            write(ctl_fd[1], &dcmd, sizeof(dcmd));
        }

        void stop() {
            auto dcmd = new WorkerStop();
            write(ctl_fd[1], &dcmd, sizeof(dcmd));
        }

        std::thread &get_handle() { return handle; }
        const EventContext &get_ec() { return ec; }
    };

    class WorkerFeed: public WorkerCmd {
        conn_t conn;
        int client_fd;

        public:
        WorkerFeed(const conn_t &conn, int client_fd):
            conn(conn), client_fd(client_fd) {}
        void exec(Worker *worker) override {
            SALTICIDAE_LOG_INFO("worker %x got %s",
                    std::this_thread::get_id(),
                    std::string(*conn).c_str());
            auto &ec = worker->get_ec();
            conn->get_send_buffer()
                    .get_queue()
                    .reg_handler(ec, [conn=this->conn,
                                    client_fd=this->client_fd](MPSCWriteBuffer::queue_t &) {
                if (conn->ready_send)
                    conn->send_data(client_fd, EV_WRITE);
                return false;
            });
            auto conn_ptr = conn.get();
            conn->ev_read = Event(ec, client_fd, EV_READ,
                                    std::bind(&Conn::recv_data, conn_ptr, _1, _2));
            conn->ev_write = Event(ec, client_fd, EV_WRITE,
                                    std::bind(&Conn::send_data, conn_ptr, _1, _2));
            conn->ev_read.add();
            conn->ev_write.add();
        }
    };

    class WorkerStop: public WorkerCmd {
        public:
        void exec(Worker *worker) override { worker->get_ec().stop(); }
    };

    /* related to workers */
    size_t nworker;
    salticidae::BoxObj<Worker[]> workers;

    void accept_client(evutil_socket_t, short);
    conn_t add_conn(const conn_t &conn);
    conn_t _connect(const NetAddr &addr);
    void _post_terminate(int fd);

    protected:
    class DispatchCmd {
        public:
        virtual ~DispatchCmd() = default;
        virtual void exec(ConnPool *cpool) = 0;
    };

    private:
    class DspConnect: public DispatchCmd {
        const NetAddr addr;
        public:
        DspConnect(const NetAddr &addr): addr(addr) {}
        void exec(ConnPool *cpool) override {
            cpool->update_conn(cpool->_connect(addr), true);
        }
    };

    class DspPostTerm: public DispatchCmd {
        int fd;
        public:
        DspPostTerm(int fd): fd(fd) {}
        void exec(ConnPool *cpool) override {
            cpool->_post_terminate(fd);
        }
    };

    class DspMulticast: public DispatchCmd {
        std::vector<conn_t> receivers;
        bytearray_t data;
        public:
        DspMulticast(std::vector<conn_t> &&receivers, bytearray_t &&data):
            receivers(std::move(receivers)),
            data(std::move(data)) {}
        void exec(ConnPool *) override {
            for (auto &r: receivers) r->write(bytearray_t(data));
        }
    };

    class UserConn: public DispatchCmd {
        conn_t conn;
        bool connected;
        public:
        UserConn(const conn_t &conn, bool connected):
            conn(conn), connected(connected) {}
        void exec(ConnPool *cpool) override {
            if (cpool->conn_cb)
                cpool->conn_cb(*conn, connected);
        }
    };

    void post_terminate(int fd) {
        auto dcmd = new DspPostTerm(fd);
        write(dlisten_fd[1], &dcmd, sizeof(dcmd));
    }

    Worker &select_worker() {
        return workers[1];
    }

    protected:
    EventContext ec;
    EventContext dispatcher_ec;
    int dlisten_fd[2]; /**< for control command sent to the dispatcher */
    std::mutex dsp_ec_mlock;
    /** Should be implemented by derived class to return a new Conn object. */
    virtual Conn *create_conn() = 0;

    public:
    ConnPool(const EventContext &ec,
            int max_listen_backlog = 10,
            double conn_server_timeout = 2,
            size_t seg_buff_size = 4096,
            size_t nworker = 2):
            max_listen_backlog(max_listen_backlog),
            conn_server_timeout(conn_server_timeout),
            seg_buff_size(seg_buff_size),
            listen_fd(-1),
            nworker(std::min((size_t)1, nworker)),
            ec(ec) {
        if (pipe2(mlisten_fd, O_NONBLOCK))
            throw ConnPoolError(std::string("failed to create main pipe"));
        if (pipe2(dlisten_fd, O_NONBLOCK))
            throw ConnPoolError(std::string("failed to create dispatcher pipe"));

        ev_mlisten = Event(ec, mlisten_fd[0], EV_READ | EV_PERSIST, [this](int fd, short) {
            DispatchCmd *dcmd;
            read(fd, &dcmd, sizeof(dcmd));
            dcmd->exec(this);
            delete dcmd;
        });
        ev_mlisten.add();

        workers = new Worker[nworker];
        dispatcher_ec = workers[0].get_ec();

        ev_dlisten = Event(dispatcher_ec, dlisten_fd[0], EV_READ | EV_PERSIST, [this](int fd, short) {
            DispatchCmd *dcmd;
            read(fd, &dcmd, sizeof(dcmd));
            dcmd->exec(this);
            delete dcmd;
        });
        ev_dlisten.add();

        SALTICIDAE_LOG_INFO("starting all threads...");
        for (size_t i = 0; i < nworker; i++)
            workers[i].start();
    }

    ~ConnPool() {
        /* stop all workers */
        for (size_t i = 0; i < nworker; i++)
            workers[i].stop();
        /* join all worker threads */
        for (size_t i = 0; i < nworker; i++)
            workers[i].get_handle().join();
        for (auto it: pool)
        {
            conn_t conn = it.second;
            conn->on_close();
        }
        if (listen_fd != -1) close(listen_fd);
        for (int i = 0; i < 2; i++)
        {
            close(mlisten_fd[i]);
            close(dlisten_fd[i]);
        }
    }

    ConnPool(const ConnPool &) = delete;
    ConnPool(ConnPool &&) = delete;

    /** Actively connect to remote addr. */
    conn_t connect(const NetAddr &addr, bool blocking = true) {
        if (blocking)
            return _connect(addr);
        else
        {
            auto dcmd = new DspConnect(addr);
            write(dlisten_fd[1], &dcmd, sizeof(dcmd));
            return nullptr;
        }
    }

    /** Listen for passive connections (connection initiated from remote).
     * Does not need to be called if do not want to accept any passive
     * connections. */
    void listen(NetAddr listen_addr);

    template<typename Func>
    void reg_conn_handler(Func cb) { conn_cb = cb; }
};

}

#endif
