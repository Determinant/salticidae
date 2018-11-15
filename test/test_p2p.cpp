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

using PeerNetworkByteOp = salticidae::PeerNetwork<uint8_t>;

struct MyNet: public PeerNetworkByteOp {
    const std::string name;
    const NetAddr peer;

    MyNet(const salticidae::EventContext &ec,
            const std::string name,
            const NetAddr &peer):
            PeerNetwork<uint8_t>(ec, 10, 2, 2, 4096, 2, 10),
            name(name),
            peer(peer) {
        /* message handler could be a bound method */
        reg_handler(salticidae::generic_bind(
            &MyNet::on_receive_hello, this, _1, _2));

        reg_conn_handler([this](ConnPool::Conn &conn, bool connected) {
            if (connected)
            {
                if (conn.get_mode() == ConnPool::Conn::ACTIVE)
                {
                    printf("[%s] Connected, sending hello.\n",
                            this->name.c_str());
                    /* send the first message through this connection */
                    MsgNet::send_msg(MsgHello(this->name, "Hello there!"),
                            static_cast<MsgNet::Conn &>(conn));
                }
                else
                    printf("[%s] Accepted, waiting for greetings.\n",
                            this->name.c_str());
            }
            else
            {
                printf("[%s] Disconnected, retrying.\n", this->name.c_str());
            }
        });
    }

    void on_receive_hello(MsgHello &&msg, MyNet::Conn &conn) {
        printf("[%s] %s says %s\n",
                name.c_str(),
                msg.name.c_str(), msg.text.c_str());
        /* send acknowledgement */
        MsgNet::send_msg(MsgAck(), conn);
    }
};


void on_receive_ack(MsgAck &&msg, MyNet::Conn &conn) {
    auto net = static_cast<MyNet *>(conn.get_net());
    printf("[%s] the peer knows\n", net->name.c_str());
}

salticidae::EventContext ec;
NetAddr alice_addr("127.0.0.1:12345");
NetAddr bob_addr("127.0.0.1:12346");

void signal_handler(int) {
    throw salticidae::SalticidaeError("got termination signal");
}

int main() {
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    /* test two nodes */
    MyNet alice(ec, "Alice", bob_addr);
    MyNet bob(ec, "Bob", alice_addr);

    /* message handler could be a normal function */
    alice.reg_handler(on_receive_ack);
    bob.reg_handler(on_receive_ack);

    try {
        alice.start();
        bob.start();

        alice.listen(alice_addr);
        bob.listen(bob_addr);

        /* first attempt */
        alice.add_peer(bob_addr);
        bob.add_peer(alice_addr);

        ec.dispatch();
    } catch (salticidae::SalticidaeError &e) {}
    return 0;
}
