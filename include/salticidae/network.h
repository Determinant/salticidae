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

template<typename ClassType, typename ReturnType, typename... Args, typename... FArgs>
inline auto handler_bind(ReturnType(ClassType::* f)(Args...), FArgs&&... fargs) {
    return std::function<ReturnType(Args...)>(std::bind(f, std::forward<FArgs>(fargs)...));
}

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
        mutable size_t nsent;
        mutable size_t nrecv;
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

    using conn_t = RcObj<Conn>;
#ifdef SALTICIDAE_MSG_STAT
    class msg_stat_by_opcode_t:
            public std::unordered_map<typename Msg::opcode_t,
                                        std::pair<uint32_t, size_t>> {
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
            size_t seg_buff_size):
        ConnPool(ec, max_listen_backlog,
                    conn_server_timeout,
                    seg_buff_size) {}

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
    using MsgNet = MsgNetwork<OpcodeType>;
    using Msg = typename MsgNet::Msg;
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

    using conn_t = RcObj<Conn>;

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

class PeerNetworkError: public SalticidaeError {
    using SalticidaeError::SalticidaeError;
};

/** Peer-to-peer network where any two nodes could hold a bi-diretional message
 * channel, established by either side. */
template<typename OpcodeType = uint8_t,
        OpcodeType OPCODE_PING = 0xf0,
        OpcodeType OPCODE_PONG = 0xf1>
class PeerNetwork: public MsgNetwork<OpcodeType> {
    using MsgNet = MsgNetwork<OpcodeType>;
    using Msg = typename MsgNet::Msg;
    public:
    enum IdentityMode {
        IP_BASED,
        IP_PORT_BASED
    };

    class Conn: public MsgNet::Conn {
        friend PeerNetwork;
        NetAddr peer_id;
        Event ev_timeout;

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

    using conn_t = RcObj<Conn>;

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
    std::vector<NetAddr> peer_list;

    IdentityMode id_mode;
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

    void msg_ping(MsgPing &&msg, Conn &conn);
    void msg_pong(MsgPong &&msg, Conn &conn);
    void reset_conn_timeout(Conn &conn);
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
    void send_msg(const MsgType &msg, const Peer *peer);
    template<typename MsgType>
    void send_msg(const MsgType &msg, const NetAddr &paddr);
    void listen(NetAddr listen_addr);
    bool has_peer(const NetAddr &paddr) const;
    const std::vector<NetAddr> &all_peers() const;
    conn_t connect(const NetAddr &addr) {
        return static_pointer_cast<Conn>(ConnPool::connect(addr));
    }
};

template<typename OpcodeType>
void MsgNetwork<OpcodeType>::Conn::on_read() {
    ConnPool::Conn::on_read();
    auto &recv_buffer = read();
    auto mn = get_net();
    while (get_fd() != -1)
    {
        if (msg_state == Conn::HEADER)
        {
            if (recv_buffer.size() < Msg::header_size) break;
            /* new header available */
            bytearray_t data = recv_buffer.pop(Msg::header_size);
            msg = Msg(data.data());
            msg_state = Conn::PAYLOAD;
        }
        if (msg_state == Conn::PAYLOAD)
        {
            size_t len = msg.get_length();
            if (recv_buffer.size() < len) break;
            /* new payload available */
            bytearray_t data = recv_buffer.pop(len);
            msg.set_payload(std::move(data));
            msg_state = Conn::HEADER;
#ifndef SALTICIDAE_NOCHECKSUM
            if (!msg.verify_checksum())
            {
                SALTICIDAE_LOG_WARN("checksums do not match, dropping the message");
                return;
            }
#endif
            auto it = mn->handler_map.find(msg.get_opcode());
            if (it == mn->handler_map.end())
                SALTICIDAE_LOG_WARN("unknown opcode: %s",
                                    get_hex(msg.get_opcode()).c_str());
            else /* call the handler */
            {
                SALTICIDAE_LOG_DEBUG("got message %s from %s",
                        std::string(msg).c_str(),
                        std::string(*this).c_str());
                it->second(msg, *this);
#ifdef SALTICIDAE_MSG_STAT
                nrecv++;
                mn->recv_by_opcode.add(msg);
#endif
            }
        }
    }
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::reset_conn(conn_t new_conn) {
    if (conn != new_conn)
    {
        if (conn)
        {
            SALTICIDAE_LOG_DEBUG("moving send buffer");
            new_conn->move_send_buffer(conn);
            SALTICIDAE_LOG_INFO("terminating old connection %s", std::string(*conn).c_str());
            conn->terminate();
        }
        addr = new_conn->get_addr();
        conn = new_conn;
    }
    clear_all_events();
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Conn::on_setup() {
    MsgNet::Conn::on_setup();
    auto pn = get_net();
    assert(!ev_timeout);
    ev_timeout = Event(pn->ec, -1, 0, [this](evutil_socket_t, short) {
        SALTICIDAE_LOG_INFO("peer ping-pong timeout");
        this->terminate();
    });
    /* the initial ping-pong to set up the connection */
    auto &conn = static_cast<Conn &>(*this);
    pn->reset_conn_timeout(conn);
    pn->MsgNet::send_msg(MsgPing(pn->listen_port), conn);
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
    p->conn = nullptr;
    SALTICIDAE_LOG_INFO("connection lost %s for %s",
            std::string(*this).c_str(),
            std::string(peer_id).c_str());
    p->ev_retry_timer = Event(pn->ec, -1, 0,
            [pn, peer_id = this->peer_id](evutil_socket_t, short) {
        pn->start_active_conn(peer_id);
    });
    p->ev_retry_timer.add_with_timeout(pn->gen_conn_timeout());
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
void PeerNetwork<O, _, __>::msg_ping(MsgPing &&msg, Conn &conn) {
    uint16_t port = msg.port;
    SALTICIDAE_LOG_INFO("ping from %s, port %u", std::string(conn).c_str(), ntohs(port));
    if (check_new_conn(conn, port)) return;
    auto p = id2peer.find(conn.peer_id)->second.get();
    send_msg(MsgPong(this->listen_port), p);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::msg_pong(MsgPong &&msg, Conn &conn) {
    auto it = id2peer.find(conn.peer_id);
    if (it == id2peer.end())
    {
        SALTICIDAE_LOG_WARN("pong message discarded");
        return;
    }
    auto p = it->second.get();
    uint16_t port = msg.port;
    if (check_new_conn(conn, port)) return;
    p->pong_msg_ok = true;
    if (p->ping_timer_ok)
    {
        p->reset_ping_timer();
        p->send_ping();
    }
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::listen(NetAddr listen_addr) {
    MsgNet::listen(listen_addr);
    listen_port = listen_addr.port;
    this->reg_handler(handler_bind(&PeerNetwork::msg_ping, this, _1, _2));
    this->reg_handler(handler_bind(&PeerNetwork::msg_pong, this, _1, _2));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::start_active_conn(const NetAddr &addr) {
    auto p = id2peer.find(addr)->second.get();
    if (p->connected) return;
    auto conn = static_pointer_cast<Conn>(connect(addr));
    assert(p->conn == nullptr);
    p->conn = conn;
    conn->peer_id = addr;
    if (id_mode == IP_BASED)
        conn->peer_id.port = 0;
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::add_peer(const NetAddr &addr) {
    auto it = id2peer.find(addr);
    if (it != id2peer.end())
        throw PeerNetworkError("peer already exists");
    id2peer.insert(std::make_pair(addr, new Peer(addr, nullptr, this->ec)));
    peer_list.push_back(addr);
    start_active_conn(addr);
}

template<typename O, O _, O __>
const typename PeerNetwork<O, _, __>::conn_t
PeerNetwork<O, _, __>::get_peer_conn(const NetAddr &paddr) const {
    auto it = id2peer.find(paddr);
    if (it == id2peer.end())
        throw PeerNetworkError("peer does not exist");
    return it->second->conn;
}

template<typename O, O _, O __>
bool PeerNetwork<O, _, __>::has_peer(const NetAddr &paddr) const {
    return id2peer.count(paddr);
}

template<typename OpcodeType>
template<typename MsgType>
void MsgNetwork<OpcodeType>::send_msg(const MsgType &_msg, Conn &conn) {
    Msg msg(_msg);
    bytearray_t msg_data = msg.serialize();
    SALTICIDAE_LOG_DEBUG("wrote message %s to %s",
                std::string(msg).c_str(),
                std::string(*conn).c_str());
    conn.write(std::move(msg_data));
#ifdef SALTICIDAE_MSG_STAT
    conn.nsent++;
    sent_by_opcode.add(msg);
#endif
}

template<typename O, O _, O __>
template<typename MsgType>
void PeerNetwork<O, _, __>::send_msg(const MsgType &msg, const Peer *peer) {
    if (peer->connected)
        MsgNet::send_msg(msg, *(peer->conn));
    else
        SALTICIDAE_LOG_DEBUG("dropped");
}

template<typename O, O _, O __>
template<typename MsgType>
void PeerNetwork<O, _, __>::send_msg(const MsgType &msg, const NetAddr &addr) {
    auto it = id2peer.find(addr);
    if (it == id2peer.end())
    {
        SALTICIDAE_LOG_ERROR("sending to non-existing peer: %s",
                            std::string(addr).c_str());
        throw PeerNetworkError("peer does not exist");
    }
    send_msg(msg, it->second.get());
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::reset_ping_timer() {
    assert(ev_ping_timer);
    ev_ping_timer.del();
    ev_ping_timer.add_with_timeout(
        gen_rand_timeout(conn->get_net()->ping_period));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::reset_conn_timeout(Conn &conn) {
    assert(conn.ev_timeout);
    conn.ev_timeout.del();
    conn.ev_timeout.add_with_timeout(conn_timeout);
    SALTICIDAE_LOG_INFO("reset timeout %.2f", conn_timeout);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::send_ping() {
    auto pn = conn->get_net();
    ping_timer_ok = false;
    pong_msg_ok = false;
    pn->reset_conn_timeout(*conn);
    pn->send_msg(MsgPing(pn->listen_port), this);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::ping_timer(evutil_socket_t, short) {
    ping_timer_ok = true;
    if (pong_msg_ok)
    {
        reset_ping_timer();
        send_ping();
    }
}

template<typename O, O _, O __>
const std::vector<NetAddr> &PeerNetwork<O, _, __>::all_peers() const {
    return peer_list;
}

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
    assert(this->get_mode() == Conn::PASSIVE);
    get_net()->addr2conn.erase(this->get_addr());
}

template<typename OpcodeType>
template<typename MsgType>
void ClientNetwork<OpcodeType>::send_msg(const MsgType &msg, const NetAddr &addr) {
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
