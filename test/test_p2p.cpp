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

#include "salticidae/msg.h"
#include "salticidae/event.h"
#include "salticidae/network.h"
#include "salticidae/stream.h"

using salticidae::NetAddr;
using salticidae::DataStream;
using salticidae::MsgNetwork;
using salticidae::htole;
using salticidae::letoh;
using std::placeholders::_1;
using std::placeholders::_2;

/** Hello Message. */
struct MsgHello {
    static const uint8_t opcode = 0x0;
    DataStream serialized;
    std::string name;
    std::string text;
    /** Defines how to serialize the msg. */
    MsgHello(const std::string &name,
            const std::string &text) {
        serialized << htole((uint32_t)name.length());
        serialized << name << text;
    }
    /** Defines how to parse the msg. */
    MsgHello(DataStream &&s) {
        uint32_t len;
        s >> len;
        len = letoh(len);
        name = std::string((const char *)s.get_data_inplace(len), len);
        len = s.size();
        text = std::string((const char *)s.get_data_inplace(len), len);
    }
};

/** Acknowledgement Message. */
struct MsgAck {
    static const uint8_t opcode = 0x1;
    DataStream serialized;
    MsgAck() {}
    MsgAck(DataStream &&s) {}
};

const uint8_t MsgHello::opcode;
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

    salticidae::EventContext ec;
    /* test two nodes */
    MyNet net(ec, 10, 2, 2, 4096, 3, 5);

    try {
        int i;
        net.start();
        net.listen(addrs[i = atoi(argv[1])]);
        for (int j = 0; j < addrs.size(); j++)
            if (i != j) net.add_peer(addrs[j]);
        ec.dispatch();
    } catch (salticidae::SalticidaeError &e) {}
    return 0;
}
