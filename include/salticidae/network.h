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
#include "salticidae/crypto.h"
#include "salticidae/netaddr.h"
#include "salticidae/msg.h"
#include "salticidae/conn.h"

#ifdef __cplusplus
#include <unordered_set>
#include <shared_mutex>
#include <openssl/rand.h>
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
        mutable std::atomic<size_t> nsentb;
        mutable std::atomic<size_t> nrecvb;
#endif

        public:
        Conn(): msg_state(HEADER)
#ifdef SALTICIDAE_MSG_STAT
            , nsent(0), nrecv(0), nsentb(0), nrecvb(0)
#endif
        {}

        MsgNetwork *get_net() {
            return static_cast<MsgNetwork *>(get_pool());
        }

#ifdef SALTICIDAE_MSG_STAT
        size_t get_nsent() const { return nsent; }
        size_t get_nrecv() const { return nrecv; }
        size_t get_nsentb() const { return nsentb; }
        size_t get_nrecvb() const { return nrecvb; }
        void clear_msgstat() const {
            nsent.store(0, std::memory_order_relaxed);
            nrecv.store(0, std::memory_order_relaxed);
            nsentb.store(0, std::memory_order_relaxed);
            nrecvb.store(0, std::memory_order_relaxed);
        }
#endif

        protected:
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

    ConnPool::Conn *create_conn() override { return new Conn(); }
    void on_read(const ConnPool::conn_t &) override;

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

    virtual ~MsgNetwork() { stop(); }

    MsgNetwork(const EventContext &ec, const Config &config):
            ConnPool(ec, config) {
        incoming_msgs.set_capacity(65536);
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
#ifdef SALTICIDAE_MSG_STAT
                    conn->nrecv++;
                    conn->nrecvb += msg.get_length();
#endif
                    it->second(msg, conn);
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
        set_handler(callback_t::msg_type::opcode,
            [handler](const Msg &msg, const conn_t &conn) {
            handler(typename callback_t::msg_type(msg.get_payload()),
                    static_pointer_cast<typename callback_t::conn_type>(conn));
        });
    }

    template<typename Func>
    inline void set_handler(OpcodeType opcode, Func handler) {
        handler_map[opcode] = handler;
    }

    template<typename MsgType>
    inline void send_msg(const MsgType &msg, const conn_t &conn);
    inline void _send_msg(const Msg &msg, const conn_t &conn);
    template<typename MsgType>
    inline void send_msg_deferred(MsgType &&msg, const conn_t &conn);
    inline void _send_msg_deferred(Msg &&msg, const conn_t &conn);

    void stop() { stop_workers(); }
    using ConnPool::listen;
    conn_t connect(const NetAddr &addr, bool blocking = true) {
        return static_pointer_cast<Conn>(ConnPool::connect(addr, blocking));
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
    };

    using conn_t = ArcObj<Conn>;

    protected:
    ConnPool::Conn *create_conn() override { return new Conn(); }
    void on_setup(const ConnPool::conn_t &) override;
    void on_teardown(const ConnPool::conn_t &) override;

    public:
    using Config = typename MsgNet::Config;
    ClientNetwork(const EventContext &ec, const Config &config):
        MsgNet(ec, config) {}

    using MsgNet::send_msg;
    template<typename MsgType>
    inline void send_msg(const MsgType &msg, const NetAddr &addr);
    inline void _send_msg(const Msg &msg, const NetAddr &addr);
    template<typename MsgType>
    inline void send_msg_deferred(MsgType &&msg, const NetAddr &addr);
    inline void _send_msg_deferred(Msg &&msg, const NetAddr &addr);
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
        ADDR_BASED,
        CERT_BASED
    };

    private:
    struct Peer;

    public:
    class Conn: public MsgNet::Conn {
        friend PeerNetwork;
        Peer *peer;
        TimerEvent ev_timeout;

        void reset_timeout(double timeout);

        public:
        Conn(): MsgNet::Conn(), peer(nullptr) {}
        NetAddr get_peer_addr() {
            auto ret = *(static_cast<NetAddr *>(
                get_net()->disp_tcall->call([this](ThreadCall::Handle &h) {
                    h.set_result(peer ? NetAddr(peer->peer_addr) : NetAddr());
                }).get()));
            return ret;
        }

        PeerNetwork *get_net() {
            return static_cast<PeerNetwork *>(ConnPool::Conn::get_pool());
        }

        protected:
        void stop() override {
            ev_timeout.clear();
            MsgNet::Conn::stop();
        }
    };

    using conn_t = ArcObj<Conn>;
    using peer_callback_t = std::function<void(const conn_t &peer_conn, bool connected)>;
    using unknown_peer_callback_t = std::function<void(const NetAddr &claimed_addr, const X509 *cert)>;

    private:
    class Peer {
        friend PeerNetwork;
        /** connection addr, may be different due to passive mode */
        uint256_t peer_id;
        NetAddr peer_addr;
        /** the underlying connection, may be invalid when connected = false */
        conn_t conn;
        conn_t inbound_conn;
        conn_t outbound_conn;

        TimerEvent ev_ping_timer;
        bool ping_timer_ok;
        bool pong_msg_ok;
        bool connected;
        bool outbound_handshake;
        bool inbound_handshake;
        double ping_period;

        Peer() = delete;
        Peer(const uint256_t &peer_id,
            conn_t conn, conn_t inbound_conn, conn_t outbound_conn,
            const PeerNetwork *pn):
                peer_id(peer_id),
                conn(conn),
                inbound_conn(inbound_conn),
                outbound_conn(outbound_conn),
                ev_ping_timer(
                    TimerEvent(pn->disp_ec, std::bind(&Peer::ping_timer, this, _1))),
                connected(false),
                outbound_handshake(false),
                inbound_handshake(false),
                ping_period(pn->ping_period) {}
        Peer &operator=(const Peer &) = delete;
        Peer(const Peer &) = delete;

        void reset_ping_timer();
        void send_ping();
        void ping_timer(TimerEvent &);
        void clear_all_events() {
            if (ev_ping_timer)
                ev_ping_timer.del();
        }
        public:
        ~Peer() {
            if (inbound_conn) inbound_conn->peer = nullptr;
            if (outbound_conn) outbound_conn->peer = nullptr;
        }
    };

    std::unordered_map<NetAddr, conn_t> pending_peers;
    std::unordered_map<NetAddr, std::pair<uint256_t, TimerEvent>> known_peers;
    std::unordered_map<uint256_t, BoxObj<Peer>> pid2peer;

    using pinfo_slock_t = std::shared_lock<std::shared_timed_mutex>;
    using pinfo_ulock_t = std::shared_lock<std::shared_timed_mutex>;

    mutable std::shared_timed_mutex known_peers_lock;
    mutable std::shared_timed_mutex pid2peer_lock;

    peer_callback_t peer_cb;
    unknown_peer_callback_t unknown_peer_cb;

    const IdentityMode id_mode;
    double retry_conn_delay;
    double ping_period;
    double conn_timeout;
    NetAddr listen_addr;
    bool allow_unknown_peer;
    uint256_t my_nonce;

    struct MsgPing {
        static const OpcodeType opcode;
        DataStream serialized;
        NetAddr claimed_addr;
        uint256_t nonce;
        MsgPing() { serialized << (uint8_t)false; }
        MsgPing(const NetAddr &_claimed_addr, const uint256_t &_nonce) {
            serialized << (uint8_t)true << _claimed_addr << _nonce;
        }
        MsgPing(DataStream &&s) {
            uint8_t flag;
            s >> flag;
            if (flag)
                s >> claimed_addr >> nonce;
        }
    };

    struct MsgPong: public MsgPing {
        static const OpcodeType opcode;
        MsgPong(): MsgPing() {}
        MsgPong(const NetAddr &_claimed_addr, const uint256_t _nonce):
            MsgPing(_claimed_addr, _nonce) {}
        MsgPong(DataStream &&s): MsgPing(std::move(s)) {}
    };

    void ping_handler(MsgPing &&msg, const conn_t &conn);
    void pong_handler(MsgPong &&msg, const conn_t &conn);
    void _ping_msg_cb(const conn_t &conn, uint16_t port);
    void _pong_msg_cb(const conn_t &conn, uint16_t port);
    bool check_handshake(Peer *peer);
    void replace_conn(const conn_t &conn);
    void start_active_conn(const NetAddr &addr);
    static void tcall_reset_timeout(ConnPool::Worker *worker,
                                    const conn_t &conn, double timeout);
    inline conn_t _get_peer_conn(const NetAddr &addr) const;

    protected:
    ConnPool::Conn *create_conn() override { return new Conn(); }
    virtual double gen_conn_timeout() {
        return gen_rand_timeout(retry_conn_delay);
    }
    void on_setup(const ConnPool::conn_t &) override;
    void on_teardown(const ConnPool::conn_t &) override;
    uint256_t gen_peer_id(const conn_t &conn, const NetAddr &claimed_addr, const uint256_t &nonce) {
        DataStream tmp;
        if (!this->enable_tls || id_mode == ADDR_BASED)
            tmp << nonce << claimed_addr;
        else
            tmp << conn->get_peer_cert()->get_der();
        return tmp.get_hash();
    }

    public:

    class Config: public MsgNet::Config {
        friend PeerNetwork;
        double _retry_conn_delay;
        double _ping_period;
        double _conn_timeout;
        bool _allow_unknown_peer;
        IdentityMode _id_mode;

        public:
        Config(): Config(typename MsgNet::Config()) {}

        Config(const typename MsgNet::Config &config):
            MsgNet::Config(config),
            _retry_conn_delay(2),
            _ping_period(30),
            _conn_timeout(180),
            _allow_unknown_peer(false),
            _id_mode(CERT_BASED) {}


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

        Config &allow_unknown_peer(bool x) {
            _allow_unknown_peer = x;
            return *this;
        }
    };

    PeerNetwork(const EventContext &ec, const Config &config):
            MsgNet(ec, config),
            id_mode(config._id_mode),
            retry_conn_delay(config._retry_conn_delay),
            ping_period(config._ping_period),
            conn_timeout(config._conn_timeout),
            allow_unknown_peer(config._allow_unknown_peer) {
        this->reg_handler(generic_bind(&PeerNetwork::ping_handler, this, _1, _2));
        this->reg_handler(generic_bind(&PeerNetwork::pong_handler, this, _1, _2));
    }

    virtual ~PeerNetwork() { this->stop(); }

    void add_peer(const NetAddr &addr);
    void del_peer(const NetAddr &addr);
    bool has_peer(const NetAddr &addr) const;
    size_t get_npending() const;
    conn_t get_peer_conn(const NetAddr &addr) const;
    using MsgNet::send_msg;
    template<typename MsgType>
    inline void send_msg(const MsgType &msg, const NetAddr &addr);
    inline void _send_msg(const Msg &msg, const NetAddr &addr);
    template<typename MsgType>
    inline void send_msg_deferred(MsgType &&msg, const NetAddr &addr);
    inline void _send_msg_deferred(Msg &&msg, const NetAddr &addr);
    template<typename MsgType>
    void multicast_msg(MsgType &&msg, const std::vector<NetAddr> &addrs);
    inline void _multicast_msg(Msg &&msg, const std::vector<NetAddr> &addrs);

    void listen(NetAddr listen_addr);
    conn_t connect(const NetAddr &addr) = delete;
    template<typename Func>
    void reg_unknown_peer_handler(Func cb) { unknown_peer_cb = cb; }
    template<typename Func>
    void reg_peer_handler(Func cb) { peer_cb = cb; }
};

/* this callback is run by a worker */
template<typename OpcodeType>
void MsgNetwork<OpcodeType>::on_read(const ConnPool::conn_t &_conn) {
    ConnPool::on_read(_conn);
    auto conn = static_pointer_cast<Conn>(_conn);
    auto &recv_buffer = conn->recv_buffer;
    auto &msg = conn->msg;
    auto &msg_state = conn->msg_state;
    while (true)
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
            while (!incoming_msgs.enqueue(std::make_pair(msg, conn), false))
                std::this_thread::yield();
        }
    }
}

template<typename OpcodeType>
template<typename MsgType>
inline void MsgNetwork<OpcodeType>::send_msg_deferred(MsgType &&msg, const conn_t &conn) {
    return _send_msg_deferred(std::move(msg), conn);
}

template<typename OpcodeType>
inline void MsgNetwork<OpcodeType>::_send_msg_deferred(Msg &&msg, const conn_t &conn) {
    this->disp_tcall->async_call(
            [this, msg=std::move(msg), conn](ThreadCall::Handle &) {
        try {
            this->_send_msg(msg, conn);
        } catch (...) { this->recoverable_error(std::current_exception()); }
    });
}

template<typename OpcodeType>
template<typename MsgType>
inline void MsgNetwork<OpcodeType>::send_msg(const MsgType &msg, const conn_t &conn) {
    return _send_msg(msg, conn);
}

template<typename OpcodeType>
inline void MsgNetwork<OpcodeType>::_send_msg(const Msg &msg, const conn_t &conn) {
    bytearray_t msg_data = msg.serialize();
    SALTICIDAE_LOG_DEBUG("wrote message %s to %s",
                std::string(msg).c_str(),
                std::string(*conn).c_str());
#ifdef SALTICIDAE_MSG_STAT
    conn->nsent++;
    conn->nsentb += msg.get_length();
#endif
    conn->write(std::move(msg_data));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::tcall_reset_timeout(ConnPool::Worker *worker,
                                    const conn_t &conn, double timeout) {
    worker->get_tcall()->async_call([worker, conn, t=timeout](ThreadCall::Handle &) {
        try {
            if (!conn->ev_timeout) return;
            conn->ev_timeout.del();
            conn->ev_timeout.add(t);
            SALTICIDAE_LOG_DEBUG("reset connection timeout %.2f", t);
        } catch (...) { worker->error_callback(std::current_exception()); }
    });
}

/* begin: functions invoked by the dispatcher */
template<typename O, O _, O __>
void PeerNetwork<O, _, __>::on_setup(const ConnPool::conn_t &_conn) {
    MsgNet::on_setup(_conn);
    auto conn = static_pointer_cast<Conn>(_conn);
    auto worker = conn->worker;
    SALTICIDAE_LOG_INFO("connection: %s", std::string(*conn).c_str());
    worker->get_tcall()->async_call([this, conn, worker](ThreadCall::Handle &) {
        auto &ev_timeout = conn->ev_timeout;
        assert(!ev_timeout);
        ev_timeout = TimerEvent(worker->get_ec(), [=](TimerEvent &) {
            try {
                SALTICIDAE_LOG_INFO("peer ping-pong timeout");
                this->worker_terminate(conn);
            } catch (...) { worker->error_callback(std::current_exception()); }
        });
    });
    /* the initial ping-pong to set up the connection */
    tcall_reset_timeout(worker, conn, conn_timeout);
    replace_conn(conn);
    if (conn->get_mode() == Conn::ConnMode::ACTIVE)
        send_msg(MsgPing(listen_addr, my_nonce), conn);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::on_teardown(const ConnPool::conn_t &_conn) {
    MsgNet::on_teardown(_conn);
    auto conn = static_pointer_cast<Conn>(_conn);
    auto addr = conn->get_addr();
    pending_peers.erase(addr);
    SALTICIDAE_LOG_INFO("connection lost: %s", std::string(*conn).c_str());
    auto p = conn->peer;
    if (p) addr = p->peer_addr;
    TimerEvent retry_timer(this->disp_ec, [this, addr](TimerEvent &) {
        try {
            start_active_conn(addr);
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
    pinfo_ulock_t _g(known_peers_lock);
    auto it = known_peers.find(addr);
    if (it == known_peers.end()) return;
    if (p)
    {
        if (conn != p->conn) return;
        p->inbound_conn = nullptr;
        p->outbound_conn = nullptr;
        p->ev_ping_timer.del();
        p->connected = false;
        p->outbound_handshake = false;
        p->inbound_handshake = false;
        known_peers[p->peer_addr] = std::make_pair(uint256_t(), TimerEvent());
        {
            pinfo_ulock_t __g(pid2peer_lock);
            p->conn->peer = nullptr;
            pid2peer.erase(p->peer_id);
        }
        this->user_tcall->async_call([this, conn](ThreadCall::Handle &) {
            if (peer_cb) peer_cb(conn, false);
        });
    }
    else
    {
        if (!it->second.first.is_null()) return;
    }
    auto &ev_retry_timer = it->second.second;
    ev_retry_timer = std::move(retry_timer);
    ev_retry_timer.add(gen_conn_timeout());
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::reset_ping_timer() {
    assert(ev_ping_timer);
    ev_ping_timer.del();
    ev_ping_timer.add(gen_rand_timeout(ping_period));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::send_ping() {
    auto pn = conn->get_net();
    ping_timer_ok = false;
    pong_msg_ok = false;
    tcall_reset_timeout(conn->worker, conn, pn->conn_timeout);
    pn->send_msg(MsgPing(), conn);
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
bool PeerNetwork<O, _, __>::check_handshake(Peer *p) {
    if (!(p->inbound_handshake && p->outbound_handshake) ||
            p->connected)
        return false;
    p->clear_all_events();
    if (p->inbound_conn && p->inbound_conn != p->conn)
        p->inbound_conn->peer = nullptr;
    if (p->outbound_conn && p->outbound_conn != p->conn)
        p->outbound_conn->peer = nullptr;
    p->conn->peer = p;
    p->connected = true;
    p->reset_ping_timer();
    p->send_ping();
    {
        pinfo_ulock_t _g(known_peers_lock);
        known_peers[p->peer_addr] = std::make_pair(p->peer_id, TimerEvent());
    }
    pending_peers.erase(p->conn->get_addr());
    if (p->connected)
    {
        auto color_begin = "";
        auto color_end = "";
        if (logger.is_tty())
        {
            color_begin = TTY_COLOR_BLUE;
            color_end = TTY_COLOR_RESET;
        }
        SALTICIDAE_LOG_INFO("%sPeerNetwork: established connection %s <-> %s via %s",
            color_begin,
            std::string(listen_addr).c_str(),
            std::string(p->peer_addr).c_str(),
            std::string(*(p->conn)).c_str(),
            color_end);
    }
    this->user_tcall->async_call([this, conn=p->conn](ThreadCall::Handle &) {
        if (peer_cb) peer_cb(conn, true);
    });
    return true;
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::replace_conn(const conn_t &conn) {
    const auto &addr = conn->get_addr();
    auto it = pending_peers.find(addr);
    if (it != pending_peers.end())
    {
        auto &old_conn = it->second;
        if (old_conn != conn)
        {
            this->disp_terminate(old_conn);
            pending_peers.erase(it);
        }
    }
    pending_peers.insert(std::make_pair(addr, conn));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::start_active_conn(const NetAddr &addr) {
    auto conn = static_pointer_cast<Conn>(MsgNet::_connect(addr));
    replace_conn(conn);
}

template<typename O, O _, O __>
inline typename PeerNetwork<O, _, __>::conn_t PeerNetwork<O, _, __>::_get_peer_conn(const NetAddr &addr) const {
    auto it = known_peers.find(addr);
    if (it == known_peers.end())
        throw PeerNetworkError(SALTI_ERROR_PEER_NOT_EXIST);
    const auto &peer_id = it->second.first;
    if (peer_id.is_null())
        throw PeerNetworkError(SALTI_ERROR_PEER_NOT_READY);
    auto it2 = pid2peer.find(peer_id);
    assert(it2 != pid2peer.end());
    return it2->second->conn;
}
/* end: functions invoked by the dispatcher */

/* begin: functions invoked by the user loop */
template<typename O, O _, O __>
void PeerNetwork<O, _, __>::ping_handler(MsgPing &&msg, const conn_t &conn) {
    this->disp_tcall->async_call([this, conn, msg=std::move(msg)](ThreadCall::Handle &) {
        try {
            if (conn->is_terminated()) return;
            if (!msg.claimed_addr.is_null())
            {
                auto peer_id = gen_peer_id(conn, msg.claimed_addr, msg.nonce);
                if (conn->get_mode() == Conn::ConnMode::PASSIVE)
                {
                    pinfo_slock_t _g(known_peers_lock);
                    pinfo_ulock_t __g(pid2peer_lock);
                    if (!known_peers.count(msg.claimed_addr))
                    {
                        this->user_tcall->async_call([this, addr=msg.claimed_addr, conn](ThreadCall::Handle &) {
                            if (unknown_peer_cb) unknown_peer_cb(addr, conn->get_peer_cert());
                        });
                        this->disp_terminate(conn);
                        return;
                    }
                    SALTICIDAE_LOG_INFO("%s inbound handshake from %s",
                        std::string(listen_addr).c_str(),
                        std::string(*conn).c_str());
                    send_msg(MsgPong(listen_addr, my_nonce), conn);
                    auto it = pid2peer.find(peer_id);
                    if (it != pid2peer.end())
                    {
                        auto p = it->second.get();
                        if (p->connected)
                        {
                            //conn->get_net()->disp_terminate(conn);
                            return;
                        }
                        auto &old_conn = p->inbound_conn;
                        if (old_conn && !old_conn->is_terminated())
                        {
                            SALTICIDAE_LOG_DEBUG("%s terminating old connection %s",
                                    std::string(listen_addr).c_str(),
                                    std::string(*old_conn).c_str());
                            old_conn->peer = nullptr;
                            old_conn->get_net()->disp_terminate(old_conn);
                        }
                        old_conn = conn;
                        if (msg.nonce < my_nonce)
                        {
                            p->conn = conn;
                        }
                    }
                    else
                    {
                        it = pid2peer.insert(std::make_pair(peer_id,
                            new Peer(peer_id, conn, conn, nullptr, this))).first;
                    }
                    auto p = it->second.get();
                    p->inbound_handshake = true;
                    check_handshake(p);
                }
                else
                    SALTICIDAE_LOG_WARN("unexpected inbound handshake from %s",
                        std::string(*conn).c_str());
            }
            else
            {
                SALTICIDAE_LOG_INFO("ping from %s", std::string(*conn).c_str());
                send_msg(MsgPong(), conn);
            }
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::pong_handler(MsgPong &&msg, const conn_t &conn) {
    this->disp_tcall->async_call([this, conn, msg=std::move(msg)](ThreadCall::Handle &) {
        try {
            if (conn->is_terminated()) return;
            if (!msg.claimed_addr.is_null())
            {
                auto peer_id = gen_peer_id(conn, msg.claimed_addr, msg.nonce);
                if (conn->get_mode() == Conn::ConnMode::ACTIVE)
                {
                    pinfo_ulock_t _g(known_peers_lock);
                    pinfo_ulock_t __g(pid2peer_lock);
                    SALTICIDAE_LOG_INFO("%s outbound handshake to %s",
                        std::string(listen_addr).c_str(),
                        std::string(*conn).c_str());
                    auto it = pid2peer.find(peer_id);
                    if (it != pid2peer.end())
                    {
                        auto p = it->second.get();
                        if (p->connected)
                        {
                            conn->get_net()->disp_terminate(conn);
                            return;
                        }
                        auto &old_conn = p->outbound_conn;
                        if (old_conn && !old_conn->is_terminated())
                        {
                            SALTICIDAE_LOG_DEBUG("%s terminating old connection %s",
                                    std::string(listen_addr).c_str(),
                                    std::string(*old_conn).c_str());
                            old_conn->peer = nullptr;
                            old_conn->get_net()->disp_terminate(old_conn);
                        }
                        old_conn = conn;
                        if (my_nonce < msg.nonce)
                        {
                            p->conn = conn;
                        }
                        else
                        {
                            SALTICIDAE_LOG_DEBUG("%s terminating low connection %s",
                                std::string(listen_addr).c_str(),
                                std::string(*conn).c_str());
                            conn->get_net()->disp_terminate(conn);
                        }
                    }
                    else
                    {
                        it = pid2peer.insert(std::make_pair(peer_id,
                            new Peer(peer_id, conn, nullptr, conn, this))).first;
                    }
                    auto p = it->second.get();
                    p->outbound_handshake = true;
                    auto &peer_addr = conn->get_addr();
                    auto &old_peer_addr = p->peer_addr;
                    if (!old_peer_addr.is_null() && old_peer_addr != peer_addr)
                    {
                        SALTICIDAE_LOG_WARN("multiple peer addresses share the same identity");
                        known_peers.erase(old_peer_addr);
                        if (p->conn && !p->conn->is_terminated())
                            this->disp_terminate(p->conn);
                    }
                    old_peer_addr = peer_addr;
                    p->reset_ping_timer();
                    check_handshake(p);
                }
                else
                    SALTICIDAE_LOG_WARN("unexpected outbound handshake from %s",
                        std::string(*conn).c_str());
            }
            else
            {
                auto p = conn->peer;
                if (!p)
                {
                    SALTICIDAE_LOG_WARN("unexpected pong mesage");
                    return;
                }
                p->pong_msg_ok = true;
                if (p->ping_timer_ok)
                {
                    p->reset_ping_timer();
                    p->send_ping();
                }
            }
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::listen(NetAddr _listen_addr) {
    auto ret = *(static_cast<std::exception_ptr *>(
            this->disp_tcall->call([this, _listen_addr](ThreadCall::Handle &h) {
        std::exception_ptr err = nullptr;
        try {
            MsgNet::_listen(_listen_addr);
            listen_addr = _listen_addr;
            uint8_t rand_bytes[32];
            if (!RAND_bytes(rand_bytes, 32))
                throw PeerNetworkError(SALTI_ERROR_RAND_SOURCE);
            my_nonce.load(rand_bytes);
        } catch (...) {
            err = std::current_exception();
        }
        h.set_result(std::move(err));
    }).get()));
    if (ret) std::rethrow_exception(ret);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::add_peer(const NetAddr &addr) {
    this->disp_tcall->async_call([this, addr](ThreadCall::Handle &) {
        try {
            pinfo_ulock_t _g(known_peers_lock);
            if (!known_peers.insert(std::make_pair(addr,
                    std::make_pair(uint256_t(), TimerEvent()))).second)
                throw PeerNetworkError(SALTI_ERROR_PEER_ALREADY_EXISTS);
            if (!pending_peers.count(addr))
                start_active_conn(addr);
        } catch (const PeerNetworkError &) {
            this->recoverable_error(std::current_exception());
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::del_peer(const NetAddr &addr) {
    this->disp_tcall->async_call([this, addr](ThreadCall::Handle &) {
        try {
            pinfo_ulock_t _g(known_peers_lock);
            pinfo_ulock_t __g(pid2peer_lock);
            auto it = known_peers.find(addr);
            if (it == known_peers.end())
                throw PeerNetworkError(SALTI_ERROR_PEER_NOT_EXIST);
            auto peer_id = it->second.first;
            known_peers.erase(it);
            auto it2 = pending_peers.find(addr);
            if (it2 != pending_peers.end())
            {
                if (!it2->second->peer)
                    this->disp_terminate(it2->second);
                pending_peers.erase(it2);
            }
            auto it3 = pid2peer.find(peer_id);
            if (it3 != pid2peer.end())
            {
                auto p = it3->second.get();
                this->disp_terminate(p->conn);
                pid2peer.erase(it3);
            }
        } catch (const PeerNetworkError &) {
            this->recoverable_error(std::current_exception());
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
}

template<typename O, O _, O __>
typename PeerNetwork<O, _, __>::conn_t
PeerNetwork<O, _, __>::get_peer_conn(const NetAddr &addr) const {
    auto ret = *(static_cast<std::pair<conn_t, std::exception_ptr> *>(
            this->disp_tcall->call([this, addr](ThreadCall::Handle &h) {
        conn_t conn;
        std::exception_ptr err = nullptr;
        try {
            pinfo_slock_t _g(known_peers_lock);
            pinfo_slock_t __g(pid2peer_lock);
            auto it = known_peers.find(addr);
            if (it == known_peers.end())
                throw PeerNetworkError(SALTI_ERROR_PEER_NOT_EXIST);
            if (it->second.first.is_null())
            {
                conn = nullptr;
                return;
            }
            auto it2 = pid2peer.find(it->second.first);
            assert(it2 != pid2peer.end());
            conn = it2->second->conn;
        } catch (const PeerNetworkError &) {
            this->recoverable_error(std::current_exception());
        } catch (...) {
            err = std::current_exception();
        }
        h.set_result(std::make_pair(std::move(conn), err));
    }).get()));
    if (ret.second) std::rethrow_exception(ret.second);
    return std::move(ret.first);
}

template<typename O, O _, O __>
bool PeerNetwork<O, _, __>::has_peer(const NetAddr &addr) const {
    return *(static_cast<bool *>(this->disp_tcall->call(
                [this, addr](ThreadCall::Handle &h) {
        pinfo_slock_t _g(known_peers_lock);
        h.set_result(known_peers.count(addr));
    }).get()));
}

template<typename O, O _, O __>
size_t PeerNetwork<O, _, __>::get_npending() const {
    return *(static_cast<bool *>(this->disp_tcall->call(
                [this](ThreadCall::Handle &h) {
        h.set_result(pending_peers.size());
    }).get()));
}

template<typename O, O _, O __>
template<typename MsgType>
inline void PeerNetwork<O, _, __>::send_msg_deferred(MsgType &&msg, const NetAddr &addr) {
    return _send_msg_deferred(std::move(msg), addr);
}

template<typename O, O _, O __>
inline void PeerNetwork<O, _, __>::_send_msg_deferred(Msg &&msg, const NetAddr &addr) {
    this->disp_tcall->async_call(
            [this, msg=std::move(msg), addr](ThreadCall::Handle &) {
        try {
            _send_msg(msg, addr);
        } catch (...) { this->recoverable_error(std::current_exception()); }
    });
}

template<typename O, O _, O __>
template<typename MsgType>
inline void PeerNetwork<O, _, __>::send_msg(const MsgType &msg, const NetAddr &addr) {
    return _send_msg(msg, addr);
}

template<typename O, O _, O __>
inline void PeerNetwork<O, _, __>::_send_msg(const Msg &msg, const NetAddr &addr) {
    pinfo_slock_t _g(known_peers_lock);
    pinfo_slock_t __g(pid2peer_lock);
    MsgNet::_send_msg(msg, _get_peer_conn(addr));
}

template<typename O, O _, O __>
template<typename MsgType>
inline void PeerNetwork<O, _, __>::multicast_msg(MsgType &&msg, const std::vector<NetAddr> &addrs) {
    return _multicast_msg(MsgType(std::move(msg)), addrs);
}

template<typename O, O _, O __>
inline void PeerNetwork<O, _, __>::_multicast_msg(Msg &&msg, const std::vector<NetAddr> &addrs) {
    this->disp_tcall->async_call(
                [this, msg=std::move(msg), addrs](ThreadCall::Handle &) {
        try {
            for (auto &addr: addrs)
                MsgNet::_send_msg(msg, _get_peer_conn(addr));
        } catch (const PeerNetworkError &) {
            this->recoverable_error(std::current_exception());
        } catch (...) { this->recoverable_error(std::current_exception()); }
    });
}

/* end: functions invoked by the user loop */

template<typename OpcodeType>
void ClientNetwork<OpcodeType>::on_setup(const ConnPool::conn_t &_conn) {
    MsgNet::on_setup(_conn);
    auto conn = static_pointer_cast<Conn>(_conn);
    assert(conn->get_mode() == Conn::PASSIVE);
    const auto &addr = conn->get_addr();
    auto cn = conn->get_net();
    cn->addr2conn.erase(addr);
    cn->addr2conn.insert(std::make_pair(addr, conn));
}

template<typename OpcodeType>
void ClientNetwork<OpcodeType>::on_teardown(const ConnPool::conn_t &_conn) {
    MsgNet::on_teardown(_conn);
    auto conn = static_pointer_cast<Conn>(_conn);
    conn->get_net()->addr2conn.erase(conn->get_addr());
}

template<typename OpcodeType>
template<typename MsgType>
inline void ClientNetwork<OpcodeType>::send_msg_deferred(MsgType &&msg, const NetAddr &addr) {
    return _send_msg_deferred(std::move(msg), addr);
}

template<typename OpcodeType>
inline void ClientNetwork<OpcodeType>::_send_msg_deferred(Msg &&msg, const NetAddr &addr) {
    this->disp_tcall->async_call(
            [this, msg=std::move(msg), addr](ThreadCall::Handle &) {
        try {
            _send_msg(msg, addr);
        } catch (...) { this->recoverable_error(std::current_exception()); }
    });
}

template<typename OpcodeType>
template<typename MsgType>
inline void ClientNetwork<OpcodeType>::send_msg(const MsgType &msg, const NetAddr &addr) {
    return _send_msg(msg, addr);
}

template<typename OpcodeType>
inline void ClientNetwork<OpcodeType>::_send_msg(const Msg &msg, const NetAddr &addr) {
    auto it = addr2conn.find(addr);
    if (it != addr2conn.end())
        MsgNet::_send_msg(msg, it->second);
    else
        throw ClientNetworkError(SALTI_ERROR_PEER_NOT_EXIST);
}

template<typename O, O OPCODE_PING, O _>
const O PeerNetwork<O, OPCODE_PING, _>::MsgPing::opcode = OPCODE_PING;

template<typename O, O _, O OPCODE_PONG>
const O PeerNetwork<O, _, OPCODE_PONG>::MsgPong::opcode = OPCODE_PONG;

}

#ifdef SALTICIDAE_CBINDINGS
using msgnetwork_t = salticidae::MsgNetwork<_opcode_t>;
using msgnetwork_config_t = msgnetwork_t::Config;
using msgnetwork_conn_t = msgnetwork_t::conn_t;

using peernetwork_t = salticidae::PeerNetwork<_opcode_t>;
using peernetwork_config_t = peernetwork_t::Config;
using peernetwork_conn_t = peernetwork_t::conn_t;

using clientnetwork_t = salticidae::ClientNetwork<_opcode_t>;
using clientnetwork_conn_t = clientnetwork_t::conn_t;
#endif

#else

#ifdef SALTICIDAE_CBINDINGS
typedef struct msgnetwork_t msgnetwork_t;
typedef struct msgnetwork_config_t msgnetwork_config_t;
typedef struct msgnetwork_conn_t msgnetwork_conn_t;

typedef struct peernetwork_t peernetwork_t;
typedef struct peernetwork_config_t peernetwork_config_t;
typedef struct peernetwork_conn_t peernetwork_conn_t;

typedef struct clientnetwork_t clientnetwork_t;
typedef struct clientnetwork_conn_t clientnetwork_conn_t;
#endif

#endif

#ifdef SALTICIDAE_CBINDINGS
typedef enum msgnetwork_conn_mode_t {
    CONN_MODE_ACTIVE,
    CONN_MODE_PASSIVE,
} msgnetwork_conn_mode_t;

typedef enum peernetwork_id_mode_t {
    ID_MODE_ADDR_BASED,
    ID_MODE_CERT_BASED
} peernetwork_id_mode_t;

#ifdef __cplusplus
extern "C" {
#endif

void salticidae_injected_msg_callback(const msg_t *msg, msgnetwork_conn_t *conn);

/* MsgNetwork */

msgnetwork_config_t *msgnetwork_config_new();
void msgnetwork_config_free(const msgnetwork_config_t *self);
void msgnetwork_config_burst_size(msgnetwork_config_t *self, size_t burst_size);
void msgnetwork_config_max_listen_backlog(msgnetwork_config_t *self, int backlog);
void msgnetwork_config_conn_server_timeout(msgnetwork_config_t *self, double timeout);
void msgnetwork_config_seg_buff_size(msgnetwork_config_t *self, size_t size);
void msgnetwork_config_nworker(msgnetwork_config_t *self, size_t nworker);
void msgnetwork_config_queue_capacity(msgnetwork_config_t *self, size_t cap);
void msgnetwork_config_enable_tls(msgnetwork_config_t *self, bool enabled);
void msgnetwork_config_tls_key_file(msgnetwork_config_t *self, const char *pem_fname);
void msgnetwork_config_tls_cert_file(msgnetwork_config_t *self, const char *pem_fname);
void msgnetwork_config_tls_key_by_move(msgnetwork_config_t *self, pkey_t *key);
void msgnetwork_config_tls_cert_by_move(msgnetwork_config_t *self, x509_t *cert);

msgnetwork_t *msgnetwork_new(const eventcontext_t *ec, const msgnetwork_config_t *config, SalticidaeCError *err);
void msgnetwork_free(const msgnetwork_t *self);
void msgnetwork_send_msg(msgnetwork_t *self, const msg_t *msg, const msgnetwork_conn_t *conn);
void msgnetwork_send_msg_deferred_by_move(msgnetwork_t *self, msg_t *_moved_msg, const msgnetwork_conn_t *conn);
msgnetwork_conn_t *msgnetwork_connect(msgnetwork_t *self, const netaddr_t *addr, bool blocking, SalticidaeCError *err);
msgnetwork_conn_t *msgnetwork_conn_copy(const msgnetwork_conn_t *self);
void msgnetwork_conn_free(const msgnetwork_conn_t *self);
void msgnetwork_listen(msgnetwork_t *self, const netaddr_t *listen_addr, SalticidaeCError *err);
void msgnetwork_start(msgnetwork_t *self);
void msgnetwork_stop(msgnetwork_t *self);
void msgnetwork_terminate(msgnetwork_t *self, const msgnetwork_conn_t *conn);

typedef void (*msgnetwork_msg_callback_t)(const msg_t *, const msgnetwork_conn_t *, void *userdata);
void msgnetwork_reg_handler(msgnetwork_t *self, _opcode_t opcode, msgnetwork_msg_callback_t cb, void *userdata);

typedef bool (*msgnetwork_conn_callback_t)(const msgnetwork_conn_t *, bool connected, void *userdata);
void msgnetwork_reg_conn_handler(msgnetwork_t *self, msgnetwork_conn_callback_t cb, void *userdata);


typedef void (*msgnetwork_error_callback_t)(const SalticidaeCError *, bool fatal, void *userdata);
void msgnetwork_reg_error_handler(msgnetwork_t *self, msgnetwork_error_callback_t cb, void *userdata);

msgnetwork_t *msgnetwork_conn_get_net(const msgnetwork_conn_t *conn);
msgnetwork_conn_mode_t msgnetwork_conn_get_mode(const msgnetwork_conn_t *conn);
const netaddr_t *msgnetwork_conn_get_addr(const msgnetwork_conn_t *conn);
const x509_t *msgnetwork_conn_get_peer_cert(const msgnetwork_conn_t *conn);

/* PeerNetwork */

peernetwork_config_t *peernetwork_config_new();
void peernetwork_config_free(const peernetwork_config_t *self);
void peernetwork_config_retry_conn_delay(peernetwork_config_t *self, double t);
void peernetwork_config_ping_period(peernetwork_config_t *self, double t);
void peernetwork_config_conn_timeout(peernetwork_config_t *self, double t);
void peernetwork_config_id_mode(peernetwork_config_t *self, peernetwork_id_mode_t mode);
msgnetwork_config_t *peernetwork_config_as_msgnetwork_config(peernetwork_config_t *self);

peernetwork_t *peernetwork_new(const eventcontext_t *ec, const peernetwork_config_t *config, SalticidaeCError *err);
void peernetwork_free(const peernetwork_t *self);
void peernetwork_add_peer(peernetwork_t *self, const netaddr_t *addr);
void peernetwork_del_peer(peernetwork_t *self, const netaddr_t *addr);
bool peernetwork_has_peer(const peernetwork_t *self, const netaddr_t *addr);
const peernetwork_conn_t *peernetwork_get_peer_conn(const peernetwork_t *self, const netaddr_t *addr, SalticidaeCError *cerror);
msgnetwork_t *peernetwork_as_msgnetwork(peernetwork_t *self);
peernetwork_t *msgnetwork_as_peernetwork_unsafe(msgnetwork_t *self);
msgnetwork_conn_t *msgnetwork_conn_new_from_peernetwork_conn(const peernetwork_conn_t *conn);
peernetwork_conn_t *peernetwork_conn_new_from_msgnetwork_conn_unsafe(const msgnetwork_conn_t *conn);
peernetwork_conn_t *peernetwork_conn_copy(const peernetwork_conn_t *self);
netaddr_t *peernetwork_conn_get_peer_addr(const peernetwork_conn_t *self);
void peernetwork_conn_free(const peernetwork_conn_t *self);
bool peernetwork_send_msg(peernetwork_t *self, const msg_t * msg, const netaddr_t *addr);
void peernetwork_send_msg_deferred_by_move(peernetwork_t *self, msg_t * _moved_msg, const netaddr_t *addr);
void peernetwork_multicast_msg_by_move(peernetwork_t *self, msg_t *_moved_msg, const netaddr_array_t *addrs);
void peernetwork_listen(peernetwork_t *self, const netaddr_t *listen_addr, SalticidaeCError *err);

typedef void (*peernetwork_peer_callback_t)(const peernetwork_conn_t *, bool connected, void *userdata);
void peernetwork_reg_peer_handler(peernetwork_t *self, peernetwork_peer_callback_t cb, void *userdata);

typedef void (*peernetwork_unknown_peer_callback_t)(const netaddr_t *, const x509_t *, void *userdata);
void peernetwork_reg_unknown_peer_handler(peernetwork_t *self, peernetwork_unknown_peer_callback_t cb, void *userdata);

/* ClientNetwork */

clientnetwork_t *clientnetwork_new(const eventcontext_t *ec, const msgnetwork_config_t *config, SalticidaeCError *err);
void clientnetwork_free(const clientnetwork_t *self);
msgnetwork_t *clientnetwork_as_msgnetwork(clientnetwork_t *self);
clientnetwork_t *msgnetwork_as_clientnetwork_unsafe(msgnetwork_t *self);
msgnetwork_conn_t *msgnetwork_conn_new_from_clientnetwork_conn(const clientnetwork_conn_t *conn);
clientnetwork_conn_t *clientnetwork_conn_new_from_msgnetwork_conn_unsafe(const msgnetwork_conn_t *conn);
clientnetwork_conn_t *clientnetwork_conn_copy(const clientnetwork_conn_t *self);
void clientnetwork_conn_free(const clientnetwork_conn_t *self);
bool clientnetwork_send_msg(clientnetwork_t *self, const msg_t * msg, const netaddr_t *addr);
void clientnetwork_send_msg_deferred_by_move(clientnetwork_t *self, msg_t * _moved_msg, const netaddr_t *addr);

#ifdef __cplusplus
}
#endif
#endif

#endif
