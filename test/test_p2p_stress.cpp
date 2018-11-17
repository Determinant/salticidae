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
using salticidae::htole;
using salticidae::letoh;
using salticidae::bytearray_t;
using std::placeholders::_1;
using std::placeholders::_2;

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
        serialized << htole((uint32_t)size) << std::move(bytes);
    }
    /** Defines how to parse the msg. */
    MsgRand(DataStream &&s) {
        uint32_t len;
        s >> len;
        bytes = std::move(s);
    }
};

/** Acknowledgement Message. */
struct MsgAck {
    static const uint8_t opcode = 0x1;
    DataStream serialized;
    MsgAck() {}
    MsgAck(DataStream &&s) {}
};

const uint8_t MsgRand::opcode;
const uint8_t MsgAck::opcode;

using MyNet = salticidae::PeerNetwork<uint8_t>;

std::vector<NetAddr> addrs = {
    NetAddr("127.0.0.1:12345"),
    NetAddr("127.0.0.1:12346"),
    NetAddr("127.0.0.1:12347"),
    NetAddr("127.0.0.1:12348")
};

void signal_handler(int) {
    throw salticidae::SalticidaeError("got termination signal");
}

int main(int argc, char **argv) {
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    std::vector<std::thread> nodes;

    for (auto &addr: addrs)
    {
        nodes.push_back(std::thread([addr]() {
            salticidae::EventContext ec;
            /* test two nodes */
            MyNet net(ec, MyNet::Config(
                salticidae::ConnPool::Config()
                    .nworker(2)).conn_timeout(5).ping_period(2));
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
