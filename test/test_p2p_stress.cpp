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

#include <cstdio>
#include <string>
#include <functional>
#include <openssl/rand.h>

#include "salticidae/msg.h"
#include "salticidae/event.h"
#include "salticidae/network.h"
#include "salticidae/stream.h"

using salticidae::NetAddr;
using salticidae::DataStream;
using salticidae::MsgNetwork;
using salticidae::ConnPool;
using salticidae::TimerEvent;
using salticidae::EventContext;
using salticidae::htole;
using salticidae::letoh;
using salticidae::bytearray_t;
using salticidae::uint256_t;
using salticidae::static_pointer_cast;
using salticidae::Config;
using salticidae::ThreadCall;
using salticidae::BoxObj;
using std::placeholders::_1;
using std::placeholders::_2;

struct MsgRand {
    static const uint8_t opcode = 0x0;
    DataStream serialized;
    bytearray_t bytes;
    MsgRand(size_t size) {
        bytearray_t bytes;
        bytes.resize(size);
        RAND_bytes(&bytes[0], size);
        serialized << std::move(bytes);
    }
    MsgRand(DataStream &&s) { bytes = s; }
};

struct MsgAck {
    static const uint8_t opcode = 0x1;
    uint256_t hash;
    DataStream serialized;
    MsgAck(const uint256_t &hash) { serialized << hash; }
    MsgAck(DataStream &&s) { s >> hash; }
};

const uint8_t MsgRand::opcode;
const uint8_t MsgAck::opcode;

using MyNet = salticidae::PeerNetwork<uint8_t>;

std::vector<NetAddr> addrs;

struct TestContext {
    TimerEvent timer;
    int state;
    uint256_t hash;
    size_t ncompleted;
    TestContext(): ncompleted(0) {}
};

struct AppContext {
    NetAddr addr;
    EventContext ec;
    BoxObj<MyNet> net;
    BoxObj<ThreadCall> tcall;
    std::unordered_map<NetAddr, TestContext> tc;
};

void install_proto(AppContext &app, const size_t &seg_buff_size) {
    auto &ec = app.ec;
    auto &net = *app.net;
    auto send_rand = [&](int size, const MyNet::conn_t &conn) {
        auto &tc = app.tc[conn->get_addr()];
        MsgRand msg(size);
        tc.hash = msg.serialized.get_hash();
        net.send_msg(std::move(msg), conn);
    };
    net.reg_conn_handler([&, send_rand](const ConnPool::conn_t &conn, bool connected) {
        if (connected)
        {
            if (conn->get_mode() == ConnPool::Conn::ACTIVE)
            {
                auto &tc = app.tc[conn->get_addr()];
                tc.state = 1;
                SALTICIDAE_LOG_INFO("increasing phase");
                send_rand(tc.state, static_pointer_cast<MyNet::Conn>(conn));
            }
        }
    });
    net.reg_error_handler([ec](const std::exception &err, bool fatal) {
        SALTICIDAE_LOG_WARN("main thread captured %s error: %s",
            fatal ? "fatal" : "recoverable", err.what());
    });
    net.reg_handler([&](MsgRand &&msg, const MyNet::conn_t &conn) {
        uint256_t hash = salticidae::get_hash(msg.bytes);
        net.send_msg(MsgAck(hash), conn);
    });
    net.reg_handler([&, send_rand](MsgAck &&msg, const MyNet::conn_t &conn) {
        auto &tc = app.tc[conn->get_addr()];
        if (msg.hash != tc.hash)
        {
            SALTICIDAE_LOG_ERROR("corrupted I/O!");
            exit(1);
        }

        if (tc.state == seg_buff_size * 2)
        {
            send_rand(tc.state, conn);
            tc.state = -1;
            tc.timer = TimerEvent(ec, [&, conn](TimerEvent &) {
                tc.ncompleted++;
                net.terminate(conn);
                std::string s;
                for (const auto &p: app.tc)
                    s += salticidae::stringprintf(" %d(%d)", ntohs(p.first.port), p.second.ncompleted);
                SALTICIDAE_LOG_INFO("%d completed:%s", ntohs(app.addr.port), s.c_str());
            });
            double t = salticidae::gen_rand_timeout(10);
            tc.timer.add(t);
            SALTICIDAE_LOG_INFO("rand-bomboard phase, ending in %.2f secs", t);
        }
        else if (tc.state == -1)
            send_rand(rand() % (seg_buff_size * 10), conn);
        else
            send_rand(++tc.state, conn);
    });
}

int main(int argc, char **argv) {
    Config config;
    auto opt_no_msg = Config::OptValFlag::create(false);
    auto opt_npeers = Config::OptValInt::create(5);
    auto opt_seg_buff_size = Config::OptValInt::create(4096);
    auto opt_nworker = Config::OptValInt::create(2);
    auto opt_conn_timeout = Config::OptValDouble::create(5);
    auto opt_ping_peroid = Config::OptValDouble::create(2);
    auto opt_help = Config::OptValFlag::create(false);
    config.add_opt("no-msg", opt_no_msg, Config::SWITCH_ON);
    config.add_opt("npeers", opt_npeers, Config::SET_VAL);
    config.add_opt("seg-buff-size", opt_seg_buff_size, Config::SET_VAL);
    config.add_opt("nworker", opt_nworker, Config::SET_VAL);
    config.add_opt("conn-timeout", opt_conn_timeout, Config::SET_VAL);
    config.add_opt("ping-period", opt_ping_peroid, Config::SET_VAL);
    config.add_opt("help", opt_help, Config::SWITCH_ON, 'h', "show this help info");
    config.parse(argc, argv);
    if (opt_help->get())
    {
        config.print_help();
        exit(0);
    }
    size_t seg_buff_size = opt_seg_buff_size->get();
    for (int i = 0; i < opt_npeers->get(); i++)
        addrs.push_back(NetAddr("127.0.0.1:" + std::to_string(12345 + i)));
    std::vector<AppContext> apps;
    std::vector<std::thread> threads;
    apps.resize(addrs.size());
    for (size_t i = 0; i < apps.size(); i++)
    {
        auto &a = apps[i];
        a.addr = addrs[i];
        a.net = new MyNet(a.ec, MyNet::Config(
                salticidae::ConnPool::Config()
                    .nworker(opt_nworker->get())
                    .seg_buff_size(seg_buff_size))
                        .conn_timeout(opt_conn_timeout->get())
                        .ping_period(opt_ping_peroid->get()));
        a.tcall = new ThreadCall(a.ec);
        if (!opt_no_msg->get())
            install_proto(a, seg_buff_size);
        a.net->start();
    }

    for (auto &a: apps)
        threads.push_back(std::thread([&]() {
            a.net->listen(a.addr);
            for (auto &paddr: addrs)
                if (paddr != a.addr) a.net->add_peer(paddr);
            a.ec.dispatch();}));
    
    EventContext ec;
    auto shutdown = [&](int) {
        for (auto &a: apps)
        {
            auto &tc = a.tcall;
            tc->async_call([ec=a.ec](ThreadCall::Handle &) {
                ec.stop();
            });
        }
        for (auto &t: threads) t.join();
        ec.stop();
    };
    salticidae::SigEvent ev_sigint(ec, shutdown);
    salticidae::SigEvent ev_sigterm(ec, shutdown);
    ev_sigint.add(SIGINT);
    ev_sigterm.add(SIGTERM);
    ec.dispatch();
    return 0;
}
