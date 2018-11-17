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
using salticidae::Event;
using salticidae::htole;
using salticidae::letoh;
using salticidae::bytearray_t;
using salticidae::uint256_t;
using std::placeholders::_1;
using std::placeholders::_2;

const size_t SEG_BUFF_SIZE = 4096;

/** Hello Message. */
struct MsgRand {
    static const uint8_t opcode = 0x0;
    DataStream serialized;
    bytearray_t bytes;
    /** Defines how to serialize the msg. */
    MsgRand(size_t size) {
        bytearray_t bytes;
        bytes.resize(size);
        RAND_bytes(&bytes[0], size);
        serialized << std::move(bytes);
    }
    /** Defines how to parse the msg. */
    MsgRand(DataStream &&s) {
        bytes = s;
    }
};

/** Acknowledgement Message. */
struct MsgAck {
    static const uint8_t opcode = 0x1;
    uint256_t hash;
    DataStream serialized;
    MsgAck(const uint256_t &hash) {
        serialized << hash;
    }
    MsgAck(DataStream &&s) {
        s >> hash;
    }
};

const uint8_t MsgRand::opcode;
const uint8_t MsgAck::opcode;

using MyNet = salticidae::PeerNetwork<uint8_t>;

std::vector<NetAddr> addrs;

void signal_handler(int) {
    throw salticidae::SalticidaeError("got termination signal");
}

int main(int argc, char **argv) {
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    int n = argc > 1 ? atoi(argv[1]) : 5;
    for (int i = 0; i < n; i++)
        addrs.push_back(NetAddr("127.0.0.1:" + std::to_string(12345 + i)));
    std::vector<std::thread> nodes;
    for (auto &addr: addrs)
    {
        nodes.push_back(std::thread([addr]() {
            salticidae::EventContext ec;
            /* test two nodes */
            MyNet net(ec, MyNet::Config(
                salticidae::ConnPool::Config()
                    .nworker(2).seg_buff_size(SEG_BUFF_SIZE))
                        .conn_timeout(5).ping_period(2));
            int state;
            uint256_t hash;
            auto send_rand = [&net, &hash](int size, MyNet::Conn &conn) {
                MsgRand msg(size);
                hash = msg.serialized.get_hash();
                net.send_msg(std::move(msg), conn);
            };
            Event timer;
            net.reg_conn_handler([&state, &net, &send_rand](salticidae::ConnPool::Conn &conn, bool connected) {
                if (connected)
                {
                    if (conn.get_mode() == ConnPool::Conn::ACTIVE)
                    {
                        state = 1;
                        SALTICIDAE_LOG_INFO("increasing phase");
                        send_rand(state, static_cast<MyNet::Conn &>(conn));
                    }
                }
            });
            net.reg_handler([&state, &net](MsgRand &&msg, MyNet::Conn &conn) {
                uint256_t hash = salticidae::get_hash(msg.bytes);
                net.send_msg(MsgAck(hash), conn);
            });
            net.reg_handler([&state, &net, &hash, &send_rand, &ec, &timer](MsgAck &&msg, MyNet::Conn &conn) {
                if (msg.hash != hash)
                {
                    SALTICIDAE_LOG_ERROR("corrupted I/O!");
                    exit(1);
                }

                if (state == SEG_BUFF_SIZE * 2)
                {
                    send_rand(state, conn);
                    state = -1;
                    timer = Event(ec, -1, [&net, conn=conn.self()](int, int) {
                        net.terminate(*conn);
                    });
                    double t = salticidae::gen_rand_timeout(10);
                    timer.add_with_timeout(t, 0);
                    SALTICIDAE_LOG_INFO("rand-bomboard phase, ending in %.2f secs", t);
                }
                else if (state == -1)
                {
                    send_rand(rand() % (SEG_BUFF_SIZE * 10), conn);
                }
                else
                {
                    send_rand(++state, conn);
                }
            });
            try {
                net.start();
                net.listen(addr);
                for (auto &paddr: addrs)
                    if (paddr != addr) net.add_peer(paddr);
                ec.dispatch();
            } catch (salticidae::SalticidaeError &e) {}
        }));
    }
    for (auto &t: nodes) t.join();
    return 0;
}
