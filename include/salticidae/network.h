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

#ifndef _SALTICIDAE_NETWORK_H
#define _SALTICIDAE_NETWORK_H

#include "salticidae/event.h"
#include "salticidae/netaddr.h"
#include "salticidae/msg.h"
#include "salticidae/conn.h"

namespace salticidae {

/** Network of nodes who can send async messages.  */
template<typename OpcodeType>
class MsgNetwork: public ConnPool {
    public:
    using Msg = MsgBase<OpcodeType>;
    /* match lambdas */
    template<typename T>
    struct callback_traits:
        public callback_traits<decltype(&T::operator())> {};
    
    /* match plain functions */
    template<typename ReturnType, typename MsgType, typename ConnType>
    struct callback_traits<ReturnType(MsgType, ConnType)> {
        using ret_type = ReturnType;
        using conn_type = typename std::remove_reference<ConnType>::type::type;
        using msg_type = typename std::remove_reference<MsgType>::type;
    };
    
    /* match function pointers */
    template<typename ReturnType, typename... Args>
    struct callback_traits<ReturnType(*)(Args...)>:
        public callback_traits<ReturnType(Args...)> {};
    
    /* match const member functions */
    template<typename ClassType, typename ReturnType, typename... Args>
    struct callback_traits<ReturnType(ClassType::*)(Args...) const>:
        public callback_traits<ReturnType(Args...)> {};
    
    /* match member functions */
    template<typename ClassType, typename ReturnType, typename... Args>
    struct callback_traits<ReturnType(ClassType::*)(Args...)>:
        public callback_traits<ReturnType(Args...)> {};

    class Conn: public ConnPool::Conn {
        friend MsgNetwork;
        enum MsgState {
            HEADER,
            PAYLOAD
        };

        Msg msg;
        MsgState msg_state;

        protected:
#ifdef SALTICIDAE_MSG_STAT
        mutable std::atomic<size_t> nsent;
        mutable std::atomic<size_t> nrecv;
#endif

        public:
        Conn(): msg_state(HEADER)
#ifdef SALTICIDAE_MSG_STAT
            , nsent(0), nrecv(0)
#endif
        {}

        MsgNetwork *get_net() {
            return static_cast<MsgNetwork *>(get_pool());
        }

#ifdef SALTICIDAE_MSG_STAT
        size_t get_nsent() const { return nsent; }
        size_t get_nrecv() const { return nrecv; }
        void clear_nsent() const { nsent = 0; }
        void clear_nrecv() const { nrecv = 0; }
#endif

        protected:
        void on_read() override;
    };

    using conn_t = ArcObj<Conn>;
#ifdef SALTICIDAE_MSG_STAT
    // TODO: a lock-free, thread-safe, fine-grained stat
#endif

    private:
    std::unordered_map<
        typename Msg::opcode_t,
        std::function<void(const Msg &msg, const conn_t &)>> handler_map;
    using queue_t = MPSCQueueEventDriven<std::pair<Msg, conn_t>>;
    queue_t incoming_msgs;

    protected:
#ifdef SALTICIDAE_MSG_STAT
#endif

    ConnPool::Conn *create_conn() override { return new Conn(); }

    public:

    class Config: public ConnPool::Config {
        friend MsgNetwork;
        size_t _burst_size;

        public:
        Config(): Config(ConnPool::Config()) {}
        Config(const ConnPool::Config &config):
            ConnPool::Config(config), _burst_size(1000) {}

        Config &burst_size(size_t x) {
            _burst_size = x;
            return *this;
        }
    };

    MsgNetwork(const EventContext &ec, const Config &config):
            ConnPool(ec, config) {
        incoming_msgs.reg_handler(ec, [this, burst_size=config._burst_size](queue_t &q) {
            std::pair<Msg, conn_t> item;
            size_t cnt = 0;
            while (q.try_dequeue(item))
            {
                auto &msg = item.first;
                auto &conn = item.second;
                auto it = handler_map.find(msg.get_opcode());
                if (it == handler_map.end())
                    SALTICIDAE_LOG_WARN("unknown opcode: %s",
                                        get_hex(msg.get_opcode()).c_str());
                else /* call the handler */
                {
                    SALTICIDAE_LOG_DEBUG("got message %s from %s",
                            std::string(msg).c_str(),
                            std::string(*conn).c_str());
                    it->second(msg, conn);
#ifdef SALTICIDAE_MSG_STAT
                    conn->nrecv++;
#endif
                }
                if (++cnt == burst_size) return true;
            }
            return false;
        });
    }

    template<typename Func>
    typename std::enable_if<std::is_constructible<
        typename callback_traits<Func>::msg_type, DataStream &&>::value>::type
    reg_handler(Func handler) {
        using callback_t = callback_traits<Func>;
        handler_map[callback_t::msg_type::opcode] = [handler](const Msg &msg, const conn_t &conn) {
            handler(typename callback_t::msg_type(msg.get_payload()),
                    static_pointer_cast<typename callback_t::conn_type>(conn));
        };
    }

    template<typename MsgType>
    void send_msg(MsgType &&msg, const conn_t &conn);
    using ConnPool::listen;
#ifdef SALTICIDAE_MSG_STAT
#endif
    conn_t connect(const NetAddr &addr) {
        return static_pointer_cast<Conn>(ConnPool::connect(addr));
    }
};

/** Simple network that handles client-server requests. */
template<typename OpcodeType>
class ClientNetwork: public MsgNetwork<OpcodeType> {
    public:
    using MsgNet = MsgNetwork<OpcodeType>;
    using Msg = typename MsgNet::Msg;

    private:
    std::unordered_map<NetAddr, typename MsgNet::conn_t> addr2conn;

    public:
    class Conn: public MsgNet::Conn {
        friend ClientNetwork;

        public:
        Conn() = default;

        ClientNetwork *get_net() {
            return static_cast<ClientNetwork *>(ConnPool::Conn::get_pool());
        }

        protected:
        void on_setup() override;
        void on_teardown() override;
    };

    using conn_t = ArcObj<Conn>;

    protected:
    ConnPool::Conn *create_conn() override { return new Conn(); }

    public:
    using Config = typename MsgNet::Config;
    ClientNetwork(const EventContext &ec, const Config &config):
        MsgNet(ec, config) {}

    using MsgNet::send_msg;
    template<typename MsgType>
    void send_msg(MsgType &&msg, const NetAddr &addr);
};

class PeerNetworkError: public ConnPoolError {
    using ConnPoolError::ConnPoolError;
};

/** Peer-to-peer network where any two nodes could hold a bi-diretional message
 * channel, established by either side. */
template<typename OpcodeType = uint8_t,
        OpcodeType OPCODE_PING = 0xf0,
        OpcodeType OPCODE_PONG = 0xf1>
class PeerNetwork: public MsgNetwork<OpcodeType> {
    public:
    using MsgNet = MsgNetwork<OpcodeType>;
    using Msg = typename MsgNet::Msg;
    enum IdentityMode {
        IP_BASED,
        IP_PORT_BASED
    };

    class Conn: public MsgNet::Conn {
        friend PeerNetwork;
        NetAddr peer_id;
        TimerEvent ev_timeout;
        void reset_timeout(double timeout);

        public:
        Conn() = default;

        PeerNetwork *get_net() {
            return static_cast<PeerNetwork *>(ConnPool::Conn::get_pool());
        }

        const NetAddr &get_peer() { return peer_id; }

        protected:
        void stop() override {
            ev_timeout.clear();
            MsgNet::Conn::stop();
        }

        void on_setup() override;
        void on_teardown() override;
    };

    using conn_t = ArcObj<Conn>;

    private:
    struct Peer {
        /** connection addr, may be different due to passive mode */
        NetAddr addr;
        /** the underlying connection, may be invalid when connected = false */
        conn_t conn;
        TimerEvent ev_ping_timer;
        TimerEvent ev_retry_timer;
        bool ping_timer_ok;
        bool pong_msg_ok;
        bool connected;

        Peer() = delete;
        Peer(NetAddr addr, conn_t conn, const EventContext &ec):
            addr(addr), conn(conn),
            ev_ping_timer(
                TimerEvent(ec, std::bind(&Peer::ping_timer, this, _1))),
            connected(false) {}
        ~Peer() {}
        Peer &operator=(const Peer &) = delete;
        Peer(const Peer &) = delete;

        void ping_timer(TimerEvent &);
        void reset_ping_timer();
        void send_ping();
        void clear_all_events() {
            if (ev_ping_timer)
                ev_ping_timer.del();
        }
        void reset_conn(conn_t conn);
    };

    std::unordered_map <NetAddr, BoxObj<Peer>> id2peer;

    const IdentityMode id_mode;
    double retry_conn_delay;
    double ping_period;
    double conn_timeout;
    uint16_t listen_port;

    struct MsgPing {
        static const OpcodeType opcode;
        DataStream serialized;
        uint16_t port;
        MsgPing(uint16_t port) {
            serialized << htole(port);
        }
        MsgPing(DataStream &&s) {
            s >> port;
            port = letoh(port);
        }
    };

    struct MsgPong {
        static const OpcodeType opcode;
        DataStream serialized;
        uint16_t port;
        MsgPong(uint16_t port) {
            serialized << htole(port);
        }
        MsgPong(DataStream &&s) {
            s >> port;
            port = letoh(port);
        }
    };

    void msg_ping(MsgPing &&msg, const conn_t &conn);
    void msg_pong(MsgPong &&msg, const conn_t &conn);
    void _ping_msg_cb(const conn_t &conn, uint16_t port);
    void _pong_msg_cb(const conn_t &conn, uint16_t port);
    bool check_new_conn(const conn_t &conn, uint16_t port);
    void start_active_conn(const NetAddr &paddr);
    static void tcall_reset_timeout(ConnPool::Worker *worker,
                                    const conn_t &conn, double timeout);

    protected:
    ConnPool::Conn *create_conn() override { return new Conn(); }
    virtual double gen_conn_timeout() {
        return gen_rand_timeout(retry_conn_delay);
    }

    public:

    class Config: public MsgNet::Config {
        friend PeerNetwork;
        double _retry_conn_delay;
        double _ping_period;
        double _conn_timeout;
        IdentityMode _id_mode;

        public:
        Config(): Config(typename MsgNet::Config()) {}

        Config(const typename MsgNet::Config &config):
            MsgNet::Config(config),
            _retry_conn_delay(2),
            _ping_period(30),
            _conn_timeout(180),
            _id_mode(IP_PORT_BASED) {}


        Config &retry_conn_delay(double x) {
            _retry_conn_delay = x;
            return *this;
        }

        Config &ping_period(double x) {
            _ping_period = x;
            return *this;
        }

        Config &conn_timeout(double x) {
            _conn_timeout = x;
            return *this;
        }

        Config &id_mode(IdentityMode x) {
            _id_mode = x;
            return *this;
        }
    };

    PeerNetwork(const EventContext &ec, const Config &config):
        MsgNet(ec, config),
        id_mode(config._id_mode),
        retry_conn_delay(config._retry_conn_delay),
        ping_period(config._ping_period),
        conn_timeout(config._conn_timeout) {
        this->reg_handler(generic_bind(&PeerNetwork::msg_ping, this, _1, _2));
        this->reg_handler(generic_bind(&PeerNetwork::msg_pong, this, _1, _2));
    }

    ~PeerNetwork() { this->stop_workers(); }

    void add_peer(const NetAddr &paddr);
    const conn_t get_peer_conn(const NetAddr &paddr) const;
    using MsgNet::send_msg;
    template<typename MsgType>
    void send_msg(MsgType &&msg, const NetAddr &paddr);
    void listen(NetAddr listen_addr);
    bool has_peer(const NetAddr &paddr) const;
    conn_t connect(const NetAddr &addr) = delete;
};

/* this callback is run by a worker */
template<typename OpcodeType>
void MsgNetwork<OpcodeType>::Conn::on_read() {
    ConnPool::Conn::on_read();
    auto &recv_buffer = this->recv_buffer;
    auto mn = get_net();
    while (self_ref)
    {
        if (msg_state == Conn::HEADER)
        {
            if (recv_buffer.size() < Msg::header_size) break;
            /* new header available */
            msg = Msg(recv_buffer.pop(Msg::header_size));
            msg_state = Conn::PAYLOAD;
        }
        if (msg_state == Conn::PAYLOAD)
        {
            size_t len = msg.get_length();
            if (recv_buffer.size() < len) break;
            /* new payload available */
            msg.set_payload(recv_buffer.pop(len));
            msg_state = Conn::HEADER;
#ifndef SALTICIDAE_NOCHECKSUM
            if (!msg.verify_checksum())
            {
                SALTICIDAE_LOG_WARN("checksums do not match, dropping the message");
                return;
            }
#endif
            mn->incoming_msgs.enqueue(
                std::make_pair(std::move(msg), static_pointer_cast<Conn>(self())));
        }
    }
}

template<typename OpcodeType>
template<typename MsgType>
void MsgNetwork<OpcodeType>::send_msg(MsgType &&_msg, const conn_t &conn) {
    Msg msg(std::forward<MsgType>(_msg));
    bytearray_t msg_data = msg.serialize();
    SALTICIDAE_LOG_DEBUG("wrote message %s to %s",
                std::string(msg).c_str(),
                std::string(*conn).c_str());
    conn->write(std::move(msg_data));
#ifdef SALTICIDAE_MSG_STAT
    conn->nsent++;
#endif
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::tcall_reset_timeout(ConnPool::Worker *worker,
                                    const conn_t &conn, double timeout) {
    worker->get_tcall()->async_call([conn, t=timeout](ThreadCall::Handle &) {
        if (!conn->ev_timeout) return;
        conn->ev_timeout.del();
        conn->ev_timeout.add(t);
        SALTICIDAE_LOG_DEBUG("reset connection timeout %.2f", t);
    });
}

/* begin: functions invoked by the dispatcher */
template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Conn::on_setup() {
    MsgNet::Conn::on_setup();
    auto pn = get_net();
    auto conn = static_pointer_cast<Conn>(this->self());
    auto worker = this->worker;
    assert(!ev_timeout);
    ev_timeout = TimerEvent(worker->get_ec(), [conn](TimerEvent &) {
        SALTICIDAE_LOG_INFO("peer ping-pong timeout");
        conn->worker_terminate();
    });
    /* the initial ping-pong to set up the connection */
    tcall_reset_timeout(worker, conn, pn->conn_timeout);
    pn->send_msg(MsgPing(pn->listen_port), conn);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Conn::on_teardown() {
    MsgNet::Conn::on_teardown();
    auto pn = get_net();
    auto it = pn->id2peer.find(peer_id);
    if (it == pn->id2peer.end()) return;
    auto p = it->second.get();
    if (this != p->conn.get()) return;
    p->ev_ping_timer.del();
    p->connected = false;
    //p->conn = nullptr;
    SALTICIDAE_LOG_INFO("connection lost: %s", std::string(*this).c_str());
    // try to reconnect
    p->ev_retry_timer = TimerEvent(pn->disp_ec,
            [pn, peer_id = this->peer_id](TimerEvent &) {
        pn->start_active_conn(peer_id);
    });
    p->ev_retry_timer.add(pn->gen_conn_timeout());
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::reset_conn(conn_t new_conn) {
    if (conn != new_conn)
    {
        if (conn)
        {
            //SALTICIDAE_LOG_DEBUG("moving send buffer");
            //new_conn->move_send_buffer(conn);
            SALTICIDAE_LOG_INFO("terminating old connection %s", std::string(*conn).c_str());
            conn->disp_terminate();
        }
        addr = new_conn->get_addr();
        conn = new_conn;
    }
    clear_all_events();
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::reset_ping_timer() {
    assert(ev_ping_timer);
    ev_ping_timer.del();
    ev_ping_timer.add(gen_rand_timeout(conn->get_net()->ping_period));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::send_ping() {
    auto pn = conn->get_net();
    ping_timer_ok = false;
    pong_msg_ok = false;
    tcall_reset_timeout(conn->worker, conn, pn->conn_timeout);
    pn->send_msg(MsgPing(pn->listen_port), conn);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::ping_timer(TimerEvent &) {
    ping_timer_ok = true;
    if (pong_msg_ok)
    {
        reset_ping_timer();
        send_ping();
    }
}

template<typename O, O _, O __>
bool PeerNetwork<O, _, __>::check_new_conn(const conn_t &conn, uint16_t port) {
    if (conn->peer_id.is_null())
    {   /* passive connections can eventually have ids after getting the port
           number in IP_BASED_PORT mode */
        assert(id_mode == IP_PORT_BASED);
        conn->peer_id.ip = conn->get_addr().ip;
        conn->peer_id.port = port;
    }
    auto p = id2peer.find(conn->peer_id)->second.get();
    if (p->connected)
    {
        if (conn != p->conn)
        {
            conn->disp_terminate();
            return true;
        }
        return false;
    }
    p->reset_conn(conn);
    p->connected = true;
    p->reset_ping_timer();
    p->send_ping();
    if (p->connected)
    {
        auto color_begin = "";
        auto color_end = "";
        if (logger.is_tty())
        {
            color_begin = TTY_COLOR_BLUE;
            color_end = TTY_COLOR_RESET;
        }
        SALTICIDAE_LOG_INFO("%sPeerNetwork: established connection with %s via %s%s",
            color_begin,
            std::string(conn->peer_id).c_str(), std::string(*conn).c_str(),
            color_end);
    }
    return false;
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::start_active_conn(const NetAddr &addr) {
    auto p = id2peer.find(addr)->second.get();
    if (p->connected) return;
    auto conn = static_pointer_cast<Conn>(MsgNet::_connect(addr));
    //assert(p->conn == nullptr);
    p->conn = conn;
    conn->peer_id = addr;
    if (id_mode == IP_BASED)
        conn->peer_id.port = 0;
}
/* end: functions invoked by the dispatcher */

/* begin: functions invoked by the user loop */
template<typename O, O _, O __>
void PeerNetwork<O, _, __>::msg_ping(MsgPing &&msg, const conn_t &conn) {
    uint16_t port = msg.port;
    this->disp_tcall->async_call([this, conn, port](ThreadCall::Handle &) {
        if (conn->get_mode() == ConnPool::Conn::DEAD) return;
        SALTICIDAE_LOG_INFO("ping from %s, port %u",
                            std::string(*conn).c_str(), ntohs(port));
        if (check_new_conn(conn, port)) return;
        send_msg(MsgPong(this->listen_port), conn);
    });
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::msg_pong(MsgPong &&msg, const conn_t &conn) {
    uint16_t port = msg.port;
    this->disp_tcall->async_call([this, conn, port](ThreadCall::Handle &) {
        if (conn->get_mode() == ConnPool::Conn::DEAD) return;
        auto it = id2peer.find(conn->peer_id);
        if (it == id2peer.end())
        {
            SALTICIDAE_LOG_WARN("pong message discarded");
            return;
        }
        if (check_new_conn(conn, port)) return;
        auto p = it->second.get();
        p->pong_msg_ok = true;
        if (p->ping_timer_ok)
        {
            p->reset_ping_timer();
            p->send_ping();
        }
    });
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::listen(NetAddr listen_addr) {
    this->disp_tcall->call([this, listen_addr](ThreadCall::Handle &) {
        MsgNet::_listen(listen_addr);
        listen_port = listen_addr.port;
    });
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::add_peer(const NetAddr &addr) {
    this->disp_tcall->call([this, addr](ThreadCall::Handle &) {
        auto it = id2peer.find(addr);
        if (it != id2peer.end())
            throw PeerNetworkError("peer already exists");
        id2peer.insert(std::make_pair(addr, new Peer(addr, nullptr, this->disp_ec)));
        start_active_conn(addr);
    });
}

template<typename O, O _, O __>
const typename PeerNetwork<O, _, __>::conn_t
PeerNetwork<O, _, __>::get_peer_conn(const NetAddr &paddr) const {
    auto ret = *(static_cast<conn_t *>(this->disp_tcall->call(
                [this, paddr](ThreadCall::Handle &h) {
        auto it = id2peer.find(paddr);
        if (it == id2peer.end())
            throw PeerNetworkError("peer does not exist");
        h.set_result(it->second->conn);
    }).get()));
    return std::move(ret);
}

template<typename O, O _, O __>
bool PeerNetwork<O, _, __>::has_peer(const NetAddr &paddr) const {
    return *(static_cast<bool *>(this->disp_tcall->call(
                [this, paddr](ThreadCall::Handle &h) {
        h.set_result(id2peer.count(paddr));
    }).get()));
}

template<typename O, O _, O __>
template<typename MsgType>
void PeerNetwork<O, _, __>::send_msg(MsgType &&msg, const NetAddr &paddr) {
    this->disp_tcall->async_call(
                [this, msg=std::forward<MsgType>(msg), paddr](ThreadCall::Handle &) {
        auto it = id2peer.find(paddr);
        if (it == id2peer.end())
            throw PeerNetworkError("peer does not exist");
        send_msg(msg, it->second->conn);
    });
}
/* end: functions invoked by the user loop */

template<typename OpcodeType>
void ClientNetwork<OpcodeType>::Conn::on_setup() {
    MsgNet::Conn::on_setup();
    assert(this->get_mode() == Conn::PASSIVE);
    const auto &addr = this->get_addr();
    auto cn = get_net();
    cn->addr2conn.erase(addr);
    cn->addr2conn.insert(
        std::make_pair(addr,
                        static_pointer_cast<Conn>(this->self())));
}

template<typename OpcodeType>
void ClientNetwork<OpcodeType>::Conn::on_teardown() {
    MsgNet::Conn::on_teardown();
    get_net()->addr2conn.erase(this->get_addr());
}

template<typename OpcodeType>
template<typename MsgType>
void ClientNetwork<OpcodeType>::send_msg(MsgType &&msg, const NetAddr &addr) {
    this->disp_tcall->async_call(
            [this, addr, msg=std::forward<MsgType>(msg)](ThreadCall::Handle &) {
        auto it = addr2conn.find(addr);
        if (it != addr2conn.end())
            send_msg(msg, it->second);
    });
}

template<typename O, O OPCODE_PING, O _>
const O PeerNetwork<O, OPCODE_PING, _>::MsgPing::opcode = OPCODE_PING;

template<typename O, O _, O OPCODE_PONG>
const O PeerNetwork<O, _, OPCODE_PONG>::MsgPong::opcode = OPCODE_PONG;

}

#endif
