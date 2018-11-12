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
        using conn_type = ConnType;
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
    class msg_stat_by_opcode_t:
            public std::unordered_map<typename Msg::opcode_t,
                        std::pair<std::atomic<uint32_t>,
                                std::atomic<size_t>>> {
        public:
        void add(const Msg &msg) {
            auto &p = this->operator[](msg.get_opcode());
            p.first++;
            p.second += msg.get_length();
        }
    };
#endif

    private:
    std::unordered_map<
        typename Msg::opcode_t,
        std::function<void(const Msg &msg, Conn &)>> handler_map;
    using queue_t = MPSCQueueEventDriven<std::pair<Msg, conn_t>>;
    queue_t incoming_msgs;

    protected:
#ifdef SALTICIDAE_MSG_STAT
    mutable msg_stat_by_opcode_t sent_by_opcode;
    mutable msg_stat_by_opcode_t recv_by_opcode;
#endif

    ConnPool::Conn *create_conn() override { return new Conn(); }

    public:
    MsgNetwork(const EventContext &ec,
            int max_listen_backlog,
            double conn_server_timeout,
            size_t seg_buff_size,
            size_t burst_size = 1000):
        ConnPool(ec, max_listen_backlog,
                    conn_server_timeout,
                    seg_buff_size) {
        incoming_msgs.reg_handler(ec, [this, burst_size](queue_t &q) {
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
                    it->second(msg, *conn);
#ifdef SALTICIDAE_MSG_STAT
                    conn->nrecv++;
                    recv_by_opcode.add(msg);
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
        handler_map[callback_t::msg_type::opcode] = [handler](const Msg &msg, Conn &conn) {
            handler(typename callback_t::msg_type(msg.get_payload()),
                    static_cast<typename callback_t::conn_type>(conn));
        };
    }

    template<typename MsgType>
    void send_msg(const MsgType &msg, Conn &conn);
    using ConnPool::listen;
#ifdef SALTICIDAE_MSG_STAT
    msg_stat_by_opcode_t &get_sent_by_opcode() const {
        return sent_by_opcode;
    }
    msg_stat_by_opcode_t &get_recv_by_opcode() const {
        return recv_by_opcode;
    }
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
    std::mutex cn_mlock;

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
    ClientNetwork(const EventContext &ec,
                int max_listen_backlog = 10,
                double conn_server_timeout = 0,
                size_t seg_buff_size = 4096):
        MsgNet(ec, max_listen_backlog,
                conn_server_timeout,
                seg_buff_size) {}

    template<typename MsgType>
    void send_msg(const MsgType &msg, const NetAddr &addr);
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
        Event ev_timeout;
        void reset_timeout(double timeout);

        public:
        Conn() = default;

        PeerNetwork *get_net() {
            return static_cast<PeerNetwork *>(ConnPool::Conn::get_pool());
        }

        const NetAddr &get_peer() { return peer_id; }

        protected:
        void on_close() override {
            ev_timeout.clear();
            MsgNet::Conn::on_close();
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
        Event ev_ping_timer;
        Event ev_retry_timer;
        bool ping_timer_ok;
        bool pong_msg_ok;
        bool connected;
        std::mutex mlock;

        Peer() = delete;
        Peer(NetAddr addr, conn_t conn, const EventContext &ec):
            addr(addr), conn(conn),
            ev_ping_timer(
                Event(ec, -1, 0, std::bind(&Peer::ping_timer, this, _1, _2))),
            connected(false) {}
        ~Peer() {}
        Peer &operator=(const Peer &) = delete;
        Peer(const Peer &) = delete;

        void ping_timer(evutil_socket_t, short);
        void reset_ping_timer();
        void send_ping();
        void clear_all_events() {
            if (ev_ping_timer)
                ev_ping_timer.del();
        }
        void reset_conn(conn_t conn);
    };

    std::unordered_map <NetAddr, BoxObj<Peer>> id2peer;
    std::mutex pn_mlock;

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

    struct PingCmd: public ConnPool::DispatchCmd {
        conn_t conn;
        uint16_t port;
        PingCmd(const conn_t &conn, uint16_t port):
            conn(conn), port(port) {}
        void exec(ConnPool *cpool) override {
            auto pn = static_cast<PeerNetwork *>(cpool);
            pn->_ping_msg_cb(conn, port);
        }
    };
    
    struct PongCmd: public PingCmd {
        using PingCmd::PingCmd;
        void exec(ConnPool *cpool) override {
            auto pn = static_cast<PeerNetwork *>(cpool);
            pn->_pong_msg_cb(this->conn, this->port);
        }
    };

    void msg_ping(MsgPing &&msg, Conn &conn);
    void msg_pong(MsgPong &&msg, Conn &conn);
    void _ping_msg_cb(const conn_t &conn, uint16_t port);
    void _pong_msg_cb(const conn_t &conn, uint16_t port);
    bool check_new_conn(Conn &conn, uint16_t port);
    void start_active_conn(const NetAddr &paddr);

    protected:
    ConnPool::Conn *create_conn() override { return new Conn(); }
    virtual double gen_conn_timeout() {
        return gen_rand_timeout(retry_conn_delay);
    }

    public:
    PeerNetwork(const EventContext &ec,
                int max_listen_backlog = 10,
                double retry_conn_delay = 2,
                double conn_server_timeout = 2,
                size_t seg_buff_size = 4096,
                double ping_period = 30,
                double conn_timeout = 180,
                IdentityMode id_mode = IP_PORT_BASED):
        MsgNet(ec, max_listen_backlog,
                    conn_server_timeout,
                    seg_buff_size),
        id_mode(id_mode),
        retry_conn_delay(retry_conn_delay),
        ping_period(ping_period),
        conn_timeout(conn_timeout) {}

    void add_peer(const NetAddr &paddr);
    const conn_t get_peer_conn(const NetAddr &paddr) const;
    template<typename MsgType>
    void _send_msg(const MsgType &msg, const Peer *peer);
    template<typename MsgType>
    void send_msg(const MsgType &msg, const NetAddr &paddr);
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
    while (fd != -1)
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
void MsgNetwork<OpcodeType>::send_msg(const MsgType &_msg, Conn &conn) {
    Msg msg(_msg);
    bytearray_t msg_data = msg.serialize();
    SALTICIDAE_LOG_DEBUG("wrote message %s to %s",
                std::string(msg).c_str(),
                std::string(conn).c_str());
    conn.write(std::move(msg_data));
#ifdef SALTICIDAE_MSG_STAT
    conn.nsent++;
    sent_by_opcode.add(msg);
#endif
}

/* begin: functions invoked by the dispatcher */
template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Conn::on_setup() {
    MsgNet::Conn::on_setup();
    auto pn = get_net();
    assert(!ev_timeout);
    ev_timeout = Event(pn->ec, -1, 0, [this](evutil_socket_t, short) {
        SALTICIDAE_LOG_INFO("peer ping-pong timeout");
        this->terminate();
    });
    if (this->get_mode() == Conn::ConnMode::ACTIVE)
    {
        peer_id = this->get_addr();
        if (pn->id_mode == IP_BASED) peer_id.port = 0;
    }
    /* the initial ping-pong to set up the connection */
    auto &conn = static_cast<Conn &>(*this);
    reset_timeout(pn->conn_timeout);
    pn->MsgNet::send_msg(MsgPing(pn->listen_port), conn);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Conn::on_teardown() {
    MsgNet::Conn::on_teardown();
    auto pn = get_net();
    mutex_lg_t _pn_lg(pn->pn_mlock);
    auto it = pn->id2peer.find(peer_id);
    if (it == pn->id2peer.end()) return;
    auto p = it->second.get();
    mutex_lg_t _p_lg(p->mlock);
    if (this != p->conn.get()) return;
    p->ev_ping_timer.del();
    p->connected = false;
    p->conn = nullptr;
    SALTICIDAE_LOG_INFO("connection lost %s for %s",
            std::string(*this).c_str(),
            std::string(peer_id).c_str());
    // try to reconnect
    p->ev_retry_timer = Event(pn->dispatcher_ec, -1, 0,
            [pn, peer_id = this->peer_id](evutil_socket_t, short) {
        mutex_lg_t _pn_lg(pn->pn_mlock);
        pn->start_active_conn(peer_id);
    });
    p->ev_retry_timer.add_with_timeout(pn->gen_conn_timeout());
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
            conn->terminate();
        }
        addr = new_conn->get_addr();
        conn = new_conn;
    }
    clear_all_events();
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Conn::reset_timeout(double timeout) {
    assert(ev_timeout);
    ev_timeout.del();
    ev_timeout.add_with_timeout(timeout);
    SALTICIDAE_LOG_INFO("reset timeout %.2f", timeout);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::reset_ping_timer() {
    assert(ev_ping_timer);
    ev_ping_timer.del();
    ev_ping_timer.add_with_timeout(
        gen_rand_timeout(conn->get_net()->ping_period));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::send_ping() {
    auto pn = conn->get_net();
    ping_timer_ok = false;
    pong_msg_ok = false;
    conn->reset_timeout(pn->conn_timeout);
    pn->_send_msg(MsgPing(pn->listen_port), this);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::ping_timer(evutil_socket_t, short) {
    mutex_lg_t _p_lg(mlock);
    ping_timer_ok = true;
    if (pong_msg_ok)
    {
        reset_ping_timer();
        send_ping();
    }
}

template<typename O, O _, O __>
bool PeerNetwork<O, _, __>::check_new_conn(Conn &conn, uint16_t port) {
    if (conn.peer_id.is_null())
    {   /* passive connections can eventually have ids after getting the port
           number in IP_BASED_PORT mode */
        assert(id_mode == IP_PORT_BASED);
        conn.peer_id.ip = conn.get_addr().ip;
        conn.peer_id.port = port;
    }
    auto p = id2peer.find(conn.peer_id)->second.get();
    mutex_lg_t _p_lg(p->mlock);
    if (p->connected)
    {
        if (conn.self() != p->conn)
        {
            conn.terminate();
            return true;
        }
        return false;
    }
    p->reset_conn(static_pointer_cast<Conn>(conn.self()));
    p->connected = true;
    p->reset_ping_timer();
    p->send_ping();
    if (p->connected)
        SALTICIDAE_LOG_INFO("PeerNetwork: established connection with %s via %s",
            std::string(conn.peer_id).c_str(), std::string(conn).c_str());
    return false;
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::_ping_msg_cb(const conn_t &conn, uint16_t port) {
    mutex_lg_t _pn_lg(pn_mlock);
    if (check_new_conn(*conn, port)) return;
    auto p = id2peer.find(conn->peer_id)->second.get();
    mutex_lg_t _p_lg(p->mlock);
    _send_msg(MsgPong(this->listen_port), p);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::_pong_msg_cb(const conn_t &conn, uint16_t port) {
    mutex_lg_t _pn_lg(pn_mlock);
    auto it = id2peer.find(conn->peer_id);
    if (it == id2peer.end())
    {
        SALTICIDAE_LOG_WARN("pong message discarded");
        return;
    }
    if (check_new_conn(*conn, port)) return;
    auto p = it->second.get();
    mutex_lg_t _p_lg(p->mlock);
    p->pong_msg_ok = true;
    if (p->ping_timer_ok)
    {
        p->reset_ping_timer();
        p->send_ping();
    }
}

/* end: functions invoked by the dispatcher */

/* this function could be both invoked by the dispatcher and the user loop */
template<typename O, O _, O __>
void PeerNetwork<O, _, __>::start_active_conn(const NetAddr &addr) {
    auto p = id2peer.find(addr)->second.get();
    mutex_lg_t _p_lg(p->mlock);
    if (p->connected) return;
    auto conn = static_pointer_cast<Conn>(MsgNet::connect(addr));
    assert(p->conn == nullptr);
    p->conn = conn;
}

/* begin: functions invoked by the user loop */
template<typename O, O _, O __>
void PeerNetwork<O, _, __>::msg_ping(MsgPing &&msg, Conn &conn) {
    uint16_t port = msg.port;
    SALTICIDAE_LOG_INFO("ping from %s, port %u", std::string(conn).c_str(), ntohs(port));
    auto dcmd = new PingCmd(static_pointer_cast<Conn>(conn.self()), port);
    write(this->dlisten_fd[1], &dcmd, sizeof(dcmd));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::msg_pong(MsgPong &&msg, Conn &conn) {
    auto dcmd = new PongCmd(static_pointer_cast<Conn>(conn.self()), msg.port);
    write(this->dlisten_fd[1], &dcmd, sizeof(dcmd));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::listen(NetAddr listen_addr) {
    MsgNet::listen(listen_addr);
    listen_port = listen_addr.port;
    this->reg_handler(generic_bind(&PeerNetwork::msg_ping, this, _1, _2));
    this->reg_handler(generic_bind(&PeerNetwork::msg_pong, this, _1, _2));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::add_peer(const NetAddr &addr) {
    mutex_lg_t _pn_lg(pn_mlock);
    auto it = id2peer.find(addr);
    if (it != id2peer.end())
        throw PeerNetworkError("peer already exists");
    id2peer.insert(std::make_pair(addr, new Peer(addr, nullptr, this->dispatcher_ec)));
    start_active_conn(addr);
}

template<typename O, O _, O __>
const typename PeerNetwork<O, _, __>::conn_t
PeerNetwork<O, _, __>::get_peer_conn(const NetAddr &paddr) const {
    mutex_lg_t _pn_lg(pn_mlock);
    auto it = id2peer.find(paddr);
    if (it == id2peer.end())
        throw PeerNetworkError("peer does not exist");
    return it->second->conn;
}

template<typename O, O _, O __>
bool PeerNetwork<O, _, __>::has_peer(const NetAddr &paddr) const {
    mutex_lg_t _pn_lg(pn_mlock);
    return id2peer.count(paddr);
}

template<typename O, O _, O __>
template<typename MsgType>
void PeerNetwork<O, _, __>::_send_msg(const MsgType &msg, const Peer *peer) {
    if (peer->connected)
        MsgNet::send_msg(msg, *(peer->conn));
    else
        SALTICIDAE_LOG_DEBUG("dropped");
}

template<typename O, O _, O __>
template<typename MsgType>
void PeerNetwork<O, _, __>::send_msg(const MsgType &msg, const NetAddr &addr) {
    mutex_lg_t _pn_lg(pn_mlock);
    auto it = id2peer.find(addr);
    if (it == id2peer.end())
    {
        SALTICIDAE_LOG_ERROR("sending to non-existing peer: %s",
                            std::string(addr).c_str());
        throw PeerNetworkError("peer does not exist");
    }
    auto p = it->second.get();
    mutex_lg_t _p_lg(p->mlock);
    _send_msg(msg, p);
}
/* end: functions invoked by the user loop */

template<typename OpcodeType>
void ClientNetwork<OpcodeType>::Conn::on_setup() {
    MsgNet::Conn::on_setup();
    assert(this->get_mode() == Conn::PASSIVE);
    const auto &addr = this->get_addr();
    mutex_lg_t _cn_lg(cn_mlock);
    auto cn = get_net();
    cn->addr2conn.erase(addr);
    cn->addr2conn.insert(
        std::make_pair(addr,
                        static_pointer_cast<Conn>(this->self())));
}

template<typename OpcodeType>
void ClientNetwork<OpcodeType>::Conn::on_teardown() {
    MsgNet::Conn::on_teardown();
    assert(this->get_mode() == Conn::PASSIVE);
    mutex_lg_t _cn_lg(cn_mlock);
    get_net()->addr2conn.erase(this->get_addr());
}

template<typename OpcodeType>
template<typename MsgType>
void ClientNetwork<OpcodeType>::send_msg(const MsgType &msg, const NetAddr &addr) {
    mutex_lg_t _cn_lg(cn_mlock);
    auto it = addr2conn.find(addr);
    if (it == addr2conn.end()) return;
    MsgNet::send_msg(msg, *(it->second));
}

template<typename O, O OPCODE_PING, O _>
const O PeerNetwork<O, OPCODE_PING, _>::MsgPing::opcode = OPCODE_PING;

template<typename O, O _, O OPCODE_PONG>
const O PeerNetwork<O, _, OPCODE_PONG>::MsgPong::opcode = OPCODE_PONG;

}

#endif
