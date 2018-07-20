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
template<typename MsgType>
class MsgNetwork: public ConnPool {
    public:
    class Conn: public ConnPool::Conn {
        enum MsgState {
            HEADER,
            PAYLOAD
        };
        MsgType msg;
        MsgState msg_state;
        MsgNetwork *mn;

        protected:
#ifdef SALTICIDAE_MSG_STAT
        mutable size_t nsent;
        mutable size_t nrecv;
#endif

        public:
        friend MsgNetwork;
        Conn(MsgNetwork *mn):
            msg_state(HEADER), mn(mn)
#ifdef SALTICIDAE_MSG_STAT
            , nsent(0), nrecv(0)
#endif
        {}

#ifdef SALTICIDAE_MSG_STAT
        size_t get_nsent() const { return nsent; }
        size_t get_nrecv() const { return nrecv; }
        void clear_nsent() const { nsent = 0; }
        void clear_nrecv() const { nrecv = 0; }
#endif

        protected:
        void on_read() override;
        void on_setup() override {}
        void on_teardown() override {}
    };

    using conn_t = RcObj<Conn>;
    using msg_callback_t = std::function<void(const MsgType &msg, conn_t conn)>;
#ifdef SALTICIDAE_MSG_STAT
    class msg_stat_by_opcode_t:
            public std::unordered_map<typename MsgType::opcode_t,
                                        std::pair<uint32_t, size_t>> {
        public:
        void add(const MsgType &msg) {
            auto &p = this->operator[](msg.get_opcode());
            p.first++;
            p.second += msg.get_length();
        }
    };
#endif

    private:
    std::unordered_map<typename MsgType::opcode_t,
                        msg_callback_t> handler_map;

    protected:
#ifdef SALTICIDAE_MSG_STAT
    mutable msg_stat_by_opcode_t sent_by_opcode;
    mutable msg_stat_by_opcode_t recv_by_opcode;
#endif

    ConnPool::conn_t create_conn() override { return (new Conn(this))->self(); }

    public:
    MsgNetwork(const EventContext &eb,
            int max_listen_backlog,
            double conn_server_timeout,
            size_t seg_buff_size):
        ConnPool(eb, max_listen_backlog,
                    conn_server_timeout,
                    seg_buff_size) {}

    void reg_handler(typename MsgType::opcode_t opcode, msg_callback_t handler);
    void send_msg(const MsgType &msg, conn_t conn);
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
template<typename MsgType>
class ClientNetwork: public MsgNetwork<MsgType> {
    using MsgNet = MsgNetwork<MsgType>;
    std::unordered_map<NetAddr, typename MsgNet::conn_t> addr2conn;

    public:
    class Conn: public MsgNet::Conn {
        ClientNetwork *cn;

        public:
        Conn(ClientNetwork *cn):
            MsgNet::Conn(static_cast<MsgNet *>(cn)),
            cn(cn) {}

        protected:
        void on_setup() override;
        void on_teardown() override;
    };

    using conn_t = RcObj<Conn>;

    protected:
    ConnPool::conn_t create_conn() override { return (new Conn(this))->self(); }

    public:
    ClientNetwork(const EventContext &eb,
                int max_listen_backlog = 10,
                double conn_server_timeout = 0,
                size_t seg_buff_size = 4096):
        MsgNet(eb, max_listen_backlog,
                conn_server_timeout,
                seg_buff_size) {}

    void send_msg(const MsgType &msg, const NetAddr &addr);
    conn_t connect(const NetAddr &addr) = delete;
};

class PeerNetworkError: public SalticidaeError {
    using SalticidaeError::SalticidaeError;
};

/** Peer-to-peer network where any two nodes could hold a bi-diretional message
 * channel, established by either side. */
template<typename MsgType>
class PeerNetwork: public MsgNetwork<MsgType> {
    using MsgNet= MsgNetwork<MsgType>;
    public:
    enum IdentityMode {
        IP_BASED,
        IP_PORT_BASED
    };

    class Conn: public MsgNet::Conn {
        NetAddr peer_id;
        Event ev_timeout;
        PeerNetwork *pn;

        public:
        friend PeerNetwork;
        const NetAddr &get_peer() { return peer_id; }
        Conn(PeerNetwork *pn):
            MsgNet::Conn(static_cast<MsgNet *>(pn)),
            pn(pn) {}

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
        PeerNetwork *pn;
        Event ev_ping_timer;
        Event ev_retry_timer;
        bool ping_timer_ok;
        bool pong_msg_ok;
        bool connected;

        Peer() = delete;
        Peer(NetAddr addr, conn_t conn, PeerNetwork *pn, const EventContext &eb):
            addr(addr), conn(conn), pn(pn),
            ev_ping_timer(
                Event(eb, -1, 0, std::bind(&Peer::ping_timer, this, _1, _2))),
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

    void msg_ping(const MsgType &msg, ConnPool::conn_t conn);
    void msg_pong(const MsgType &msg, ConnPool::conn_t conn);
    void reset_conn_timeout(conn_t conn);
    bool check_new_conn(conn_t conn, uint16_t port);
    void start_active_conn(const NetAddr &paddr);

    protected:
    ConnPool::conn_t create_conn() override { return (new Conn(this))->self(); }
    virtual double gen_conn_timeout() {
        return gen_rand_timeout(retry_conn_delay);
    }

    public:
    PeerNetwork(const EventContext &eb,
                int max_listen_backlog = 10,
                double retry_conn_delay = 2,
                double conn_server_timeout = 2,
                size_t seg_buff_size = 4096,
                double ping_period = 30,
                double conn_timeout = 180,
                IdentityMode id_mode = IP_PORT_BASED):
        MsgNet(eb, max_listen_backlog,
                    conn_server_timeout,
                    seg_buff_size),
        id_mode(id_mode),
        retry_conn_delay(retry_conn_delay),
        ping_period(ping_period),
        conn_timeout(conn_timeout) {}

    void add_peer(const NetAddr &paddr);
    const conn_t get_peer_conn(const NetAddr &paddr) const;
    void send_msg(const MsgType &msg, const Peer *peer);
    void send_msg(const MsgType &msg, const NetAddr &paddr);
    void listen(NetAddr listen_addr);
    bool has_peer(const NetAddr &paddr) const;
    const std::vector<NetAddr> &all_peers() const;
    conn_t connect(const NetAddr &addr) {
        return static_pointer_cast<Conn>(ConnPool::connect(addr));
    }
};

template<typename MsgType>
void MsgNetwork<MsgType>::Conn::on_read() {
    auto &recv_buffer = read();
    auto conn = static_pointer_cast<Conn>(self());
    while (get_fd() != -1)
    {
        if (msg_state == Conn::HEADER)
        {
            if (recv_buffer.size() < MsgType::header_size) break;
            /* new header available */
            bytearray_t data = recv_buffer.pop(MsgType::header_size);
            msg = MsgType(data.data());
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
            if (!msg.verify_checksum())
            {
                SALTICIDAE_LOG_WARN("checksums do not match, dropping the message");
                return;
            }
            auto it = mn->handler_map.find(msg.get_opcode());
            if (it == mn->handler_map.end())
                SALTICIDAE_LOG_WARN("unknown opcode: %s",
                                    get_hex(msg.get_opcode()).c_str());
            else /* call the handler */
            {
                SALTICIDAE_LOG_DEBUG("got message %s from %s",
                        std::string(msg).c_str(),
                        std::string(*this).c_str());
                it->second(msg, conn);
#ifdef SALTICIDAE_MSG_STAT
                nrecv++;
                mn->recv_by_opcode.add(msg);
#endif
            }
        }
    }
}

template<typename MsgType>
void PeerNetwork<MsgType>::Peer::reset_conn(conn_t new_conn) {
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

template<typename MsgType>
void PeerNetwork<MsgType>::Conn::on_setup() {
    assert(!ev_timeout);
    ev_timeout = Event(pn->eb, -1, 0, [this](evutil_socket_t, short) {
        SALTICIDAE_LOG_INFO("peer ping-pong timeout");
        this->terminate();
    });
    /* the initial ping-pong to set up the connection */
    MsgType ping;
    ping.gen_ping(pn->listen_port);
    auto conn = static_pointer_cast<Conn>(this->self());
    pn->reset_conn_timeout(conn);
    pn->MsgNet::send_msg(ping, conn);
}

template<typename MsgType>
void PeerNetwork<MsgType>::Conn::on_teardown() {
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
    p->ev_retry_timer = Event(pn->eb, -1, 0,
                            [pn = this->pn,
                            peer_id = this->peer_id](evutil_socket_t, short) {
        pn->start_active_conn(peer_id);
    });
    p->ev_retry_timer.add_with_timeout(pn->gen_conn_timeout());
}

template<typename MsgType>
bool PeerNetwork<MsgType>::check_new_conn(conn_t conn, uint16_t port) {
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
            conn->terminate();
            return true;
        }
        return false;
    }
    p->reset_conn(conn);
    p->connected = true;
    p->reset_ping_timer();
    p->send_ping();
    if (p->connected)
        SALTICIDAE_LOG_INFO("PeerNetwork: established connection with %s via %s",
            std::string(conn->peer_id).c_str(), std::string(*conn).c_str());
    return false;
}

template<typename MsgType>
void PeerNetwork<MsgType>::msg_ping(const MsgType &msg, ConnPool::conn_t conn_) {
    auto conn = static_pointer_cast<Conn>(conn_);
    uint16_t port;
    msg.parse_ping(port);
    SALTICIDAE_LOG_INFO("ping from %s, port %u", std::string(*conn).c_str(), ntohs(port));
    if (check_new_conn(conn, port)) return;
    auto p = id2peer.find(conn->peer_id)->second.get();
    MsgType pong;
    pong.gen_pong(this->listen_port);
    send_msg(pong, p);
}

template<typename MsgType>
void PeerNetwork<MsgType>::msg_pong(const MsgType &msg, ConnPool::conn_t conn_) {
    auto conn = static_pointer_cast<Conn>(conn_);
    auto it = id2peer.find(conn->peer_id);
    if (it == id2peer.end())
    {
        SALTICIDAE_LOG_WARN("pong message discarded");
        return;
    }
    auto p = it->second.get();
    uint16_t port;
    msg.parse_pong(port);
    if (check_new_conn(conn, port)) return;
    p->pong_msg_ok = true;
    if (p->ping_timer_ok)
    {
        p->reset_ping_timer();
        p->send_ping();
    }
}

template<typename MsgType>
void PeerNetwork<MsgType>::listen(NetAddr listen_addr) {
    MsgNet::listen(listen_addr);
    listen_port = listen_addr.port;
    this->reg_handler(MsgType::OPCODE_PING,
                        std::bind(&PeerNetwork::msg_ping, this, _1, _2));
    this->reg_handler(MsgType::OPCODE_PONG,
                        std::bind(&PeerNetwork::msg_pong, this, _1, _2));
}

template<typename MsgType>
void PeerNetwork<MsgType>::start_active_conn(const NetAddr &addr) {
    auto p = id2peer.find(addr)->second.get();
    if (p->connected) return;
    auto conn = static_pointer_cast<Conn>(connect(addr));
    assert(p->conn == nullptr);
    p->conn = conn;
    conn->peer_id = addr;
    if (id_mode == IP_BASED)
        conn->peer_id.port = 0;
}

template<typename MsgType>
void PeerNetwork<MsgType>::add_peer(const NetAddr &addr) {
    auto it = id2peer.find(addr);
    if (it != id2peer.end())
        throw PeerNetworkError("peer already exists");
    id2peer.insert(std::make_pair(addr, new Peer(addr, nullptr, this, this->eb)));
    peer_list.push_back(addr);
    start_active_conn(addr);
}

template<typename MsgType>
const typename PeerNetwork<MsgType>::conn_t
PeerNetwork<MsgType>::get_peer_conn(const NetAddr &paddr) const {
    auto it = id2peer.find(paddr);
    if (it == id2peer.end())
        throw PeerNetworkError("peer does not exist");
    return it->second->conn;
}

template<typename MsgType>
bool PeerNetwork<MsgType>::has_peer(const NetAddr &paddr) const {
    return id2peer.count(paddr);
}

template<typename MsgType>
void MsgNetwork<MsgType>::reg_handler(typename MsgType::opcode_t opcode,
                                        msg_callback_t handler) {
    handler_map[opcode] = handler;
}

template<typename MsgType>
void MsgNetwork<MsgType>::send_msg(const MsgType &msg, conn_t conn) {
    bytearray_t msg_data = msg.serialize();
    SALTICIDAE_LOG_DEBUG("wrote message %s to %s",
                std::string(msg).c_str(),
                std::string(*conn).c_str());
    conn->write(std::move(msg_data));
#ifdef SALTICIDAE_MSG_STAT
    conn->nsent++;
    sent_by_opcode.add(msg);
#endif
}

template<typename MsgType>
void PeerNetwork<MsgType>::send_msg(const MsgType &msg, const Peer *peer) {
    bytearray_t msg_data = msg.serialize();
    SALTICIDAE_LOG_DEBUG("wrote message %s to %s",
                std::string(msg).c_str(),
                std::string(peer->addr).c_str());
    if (peer->connected)
    {
        SALTICIDAE_LOG_DEBUG("wrote to ConnPool");
        peer->conn->write(std::move(msg_data));
    }
    else
    {
        SALTICIDAE_LOG_DEBUG("dropped");
    }
#ifdef SALTICIDAE_MSG_STAT
    peer->conn->nsent++;
    this->sent_by_opcode.add(msg);
#endif
}

template<typename MsgType>
void PeerNetwork<MsgType>::send_msg(const MsgType &msg, const NetAddr &addr) {
    auto it = id2peer.find(addr);
    if (it == id2peer.end())
    {
        SALTICIDAE_LOG_ERROR("sending to non-existing peer: %s", std::string(addr).c_str());
        throw PeerNetworkError("peer does not exist");
    }
    send_msg(msg, it->second.get());
}

template<typename MsgType>
void PeerNetwork<MsgType>::Peer::reset_ping_timer() {
    assert(ev_ping_timer);
    ev_ping_timer.del();
    ev_ping_timer.add_with_timeout(gen_rand_timeout(pn->ping_period));
}

template<typename MsgType>
void PeerNetwork<MsgType>::reset_conn_timeout(conn_t conn) {
    assert(conn->ev_timeout);
    conn->ev_timeout.del();
    conn->ev_timeout.add_with_timeout(conn_timeout);
    SALTICIDAE_LOG_INFO("reset timeout %.2f", conn_timeout);
}

template<typename MsgType>
void PeerNetwork<MsgType>::Peer::send_ping() {
    ping_timer_ok = false;
    pong_msg_ok = false;
    MsgType ping;
    ping.gen_ping(pn->listen_port);
    pn->reset_conn_timeout(conn);
    pn->send_msg(ping, this);
}

template<typename MsgType>
void PeerNetwork<MsgType>::Peer::ping_timer(evutil_socket_t, short) {
    ping_timer_ok = true;
    if (pong_msg_ok)
    {
        reset_ping_timer();
        send_ping();
    }
}

template<typename MsgType>
const std::vector<NetAddr> &PeerNetwork<MsgType>::all_peers() const {
    return peer_list;
}

template<typename MsgType>
void ClientNetwork<MsgType>::Conn::on_setup() {
    assert(this->get_mode() == Conn::PASSIVE);
    const auto &addr = this->get_addr();
    cn->addr2conn.erase(addr);
    cn->addr2conn.insert(
        std::make_pair(addr,
                        static_pointer_cast<Conn>(this->self())));
}

template<typename MsgType>
void ClientNetwork<MsgType>::Conn::on_teardown() {
    assert(this->get_mode() == Conn::PASSIVE);
    cn->addr2conn.erase(this->get_addr());
}

template<typename MsgType>
void ClientNetwork<MsgType>::send_msg(const MsgType &msg, const NetAddr &addr) {
    auto it = addr2conn.find(addr);
    if (it == addr2conn.end()) return;
    MsgNet::send_msg(msg, it->second);
}

}

#endif
