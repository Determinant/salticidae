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
#include <thread>
#include <signal.h>

/* disable SHA256 checksum */
#define SALTICIDAE_NOCHECKSUM

#include "salticidae/msg.h"
#include "salticidae/event.h"
#include "salticidae/network.h"
#include "salticidae/stream.h"

using salticidae::NetAddr;
using salticidae::DataStream;
using salticidae::MsgNetwork;
using salticidae::htole;
using salticidae::letoh;
using salticidae::bytearray_t;
using salticidae::TimerEvent;
using std::placeholders::_1;
using std::placeholders::_2;
using opcode_t = uint8_t;

struct MsgBytes {
    static const opcode_t opcode = 0xa;
    DataStream serialized;
    bytearray_t bytes;
    MsgBytes(size_t size) {
        bytes.resize(size);
        serialized << htole((uint32_t)size) << bytes;
    }
    MsgBytes(DataStream &&s) {
        uint32_t len;
        s >> len;
        len = letoh(len);
        auto base = s.get_data_inplace(len);
        bytes = bytearray_t(base, base + len);
    }
};

const opcode_t MsgBytes::opcode;

using MsgNetworkByteOp = MsgNetwork<opcode_t>;

struct MyNet: public MsgNetworkByteOp {
    const std::string name;
    const NetAddr peer;
    TimerEvent ev_period_send;
    TimerEvent ev_period_stat;
    size_t nrecv;

    MyNet(const salticidae::EventContext &ec,
            const std::string name,
            const NetAddr &peer,
            double stat_timeout = -1):
            MsgNetworkByteOp(ec, MsgNetworkByteOp::Config().burst_size(1000).queue_capacity(65536)),
            name(name),
            peer(peer),
            ev_period_stat(ec, [this, stat_timeout](TimerEvent &) {
                SALTICIDAE_LOG_INFO("%.2f mps", nrecv / (double)stat_timeout);
                fflush(stderr);
                nrecv = 0;
                ev_period_stat.add(stat_timeout);
            }),
            nrecv(0) {
        /* message handler could be a bound method */
        reg_handler(salticidae::generic_bind(
            &MyNet::on_receive_bytes, this, _1, _2));
        if (stat_timeout > 0)
            ev_period_stat.add(0);
    }

    struct Conn: public MsgNetworkByteOp::Conn {
        MyNet *get_net() { return static_cast<MyNet *>(get_pool()); }
        salticidae::ArcObj<Conn> self() {
            return salticidae::static_pointer_cast<Conn>(
                MsgNetworkByteOp::Conn::self());
        }

        void on_setup() override {
            auto net = get_net();
            if (get_mode() == ACTIVE)
            {
                printf("[%s] Connected, sending hello.\n",
                        net->name.c_str());
                /* send the first message through this connection */
                net->ev_period_send = TimerEvent(net->ec,
                                            [net, conn = self()](TimerEvent &) {
                    net->send_msg(MsgBytes(256), conn);
                    net->ev_period_send.add(0);
                });
                net->ev_period_send.add(0);
            }
            else
                printf("[%s] Passively connected, waiting for greetings.\n",
                        net->name.c_str());
        }
        void on_teardown() override {
            auto net = get_net();
            net->ev_period_send.clear();
            printf("[%s] Disconnected, retrying.\n", net->name.c_str());
            /* try to reconnect to the same address */
            net->connect(get_addr());
        }
    };

    salticidae::ConnPool::Conn *create_conn() override {
        return new Conn();
    }

    void on_receive_bytes(MsgBytes &&msg, const conn_t &conn) {
        nrecv++;
    }
};

salticidae::EventContext ec;
NetAddr alice_addr("127.0.0.1:1234");
NetAddr bob_addr("127.0.0.1:1235");

int main() {
    salticidae::BoxObj<MyNet> alice = new MyNet(ec, "Alice", bob_addr, 10);
    alice->start();
    alice->listen(alice_addr);
    salticidae::EventContext tec;
    salticidae::BoxObj<salticidae::ThreadCall> tcall = new salticidae::ThreadCall(tec);
    std::thread bob_thread([&tec]() {
        MyNet bob(tec, "Bob", alice_addr);
        bob.start();
        bob.connect(alice_addr);
        try {
            tec.dispatch();
        } catch (std::exception &) {}
        SALTICIDAE_LOG_INFO("thread exiting");
    });
    auto shutdown = [&](int) {
        tcall->async_call([&](salticidae::ThreadCall::Handle &) {
            tec.stop();
        });
        alice = nullptr;
        //ec.stop();
        //bob_thread.join();
    };
    salticidae::SigEvent ev_sigint(ec, shutdown);
    salticidae::SigEvent ev_sigterm(ec, shutdown);
    ev_sigint.add(SIGINT);
    ev_sigterm.add(SIGTERM);
    ec.dispatch();
    return 0;
}
