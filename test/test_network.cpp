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
using opcode_t = uint8_t;

/** Hello Message. */
struct MsgHello {
    static const opcode_t opcode = 0x0;
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
    static const opcode_t opcode = 0x1;
    DataStream serialized;
    MsgAck() {}
    MsgAck(DataStream &&s) {}
};

const opcode_t MsgHello::opcode;
const opcode_t MsgAck::opcode;

using MsgNetworkByteOp = MsgNetwork<opcode_t>;

struct MyNet: public MsgNetworkByteOp {
    const std::string name;
    const NetAddr peer;

    MyNet(const salticidae::EventContext &ec,
            const std::string name,
            const NetAddr &peer):
            MsgNetwork<opcode_t>(ec, 10, 1.0, 4096),
            name(name),
            peer(peer) {
        /* message handler could be a bound method */
        reg_handler(salticidae::handler_bind(
            &MyNet::on_receive_hello, this, _1, _2));
    }

    struct Conn: public MsgNetworkByteOp::Conn {
        MyNet *get_net() { return static_cast<MyNet *>(get_pool()); }
        salticidae::RcObj<Conn> self() {
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
                net->send_msg(MsgHello(net->name, "Hello there!"), self());
            }
            else
                printf("[%s] Passively connected, waiting for greetings.\n",
                        net->name.c_str());
        }
        void on_teardown() override {
            auto net = get_net();
            printf("[%s] Disconnected, retrying.\n", net->name.c_str());
            /* try to reconnect to the same address */
            net->connect(get_addr());
        }
    };
    using conn_t = salticidae::RcObj<Conn>;

    salticidae::ConnPool::Conn *create_conn() override {
        return new Conn();
    }

    void on_receive_hello(MsgHello &&msg, conn_t conn) {
        printf("[%s] %s says %s\n",
                name.c_str(),
                msg.name.c_str(), msg.text.c_str());
        /* send acknowledgement */
        send_msg(MsgAck(), conn);
    }
};

    
void on_receive_ack(MsgAck &&msg, MyNet::conn_t conn) {
    auto net = conn->get_net();
    printf("[%s] the peer knows\n", net->name.c_str());
}

salticidae::EventContext ec;
NetAddr alice_addr("127.0.0.1:1234");
NetAddr bob_addr("127.0.0.1:1235");

int main() {
    /* test two nodes */
    MyNet alice(ec, "Alice", bob_addr);
    MyNet bob(ec, "Bob", alice_addr);

    /* message handler could be a normal function */
    alice.reg_handler(on_receive_ack);
    bob.reg_handler(on_receive_ack);

    alice.listen(alice_addr);
    bob.listen(bob_addr);

    /* first attempt */
    alice.connect(bob_addr);
    bob.connect(alice_addr);

    ec.dispatch();
    return 0;
}
