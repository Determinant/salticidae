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

#include <stdio.h>
#include <string.h>

#include "salticidae/event.h"
#include "salticidae/network.h"
#include "salticidae/stream.h"

/** Hello Message. */
const uint8_t msg_hello_opcode = 0x0;
typedef struct MsgHello {
    const char *name;
    const char *text;
} MsgHello;
/** Defines how to serialize the msg. */
datastream_t *msg_hello_serialize(const char *name, const char *text) {
    datastream_t *serialized = datastream_new();
    serialized << htole((uint32_t)name.length());
    serialized << name << text;
    return serialized;
}

/** Defines how to parse the msg. */
MsgHello msg_hello_unserialize(datastream_t *s) {
    MsgHello res;
    uint32_t len;
    len = datastream_get_u32(s);
    len = letoh(len);

    const char *name = (const char *)malloc(len + 1);
    memmove(name, datastream_get_data_inplace(s, len), len);
    name[len] = 0;

    len = datastream_size(s);
    const char *text = (const char *)malloc(len + 1);
    memmove(text, datastream_get_data_inplace(s, len), len);
    text[len] = 0;

    res.name = name;
    res.text = text;
    return res;
}

datastream_t *msg_ack_serialize() { return datastream_new(); }

struct MyNet {
    msgnetwork_t *net;
    const char *name;
    const NetAddr peer;
} alice, bob;

void on_receive_hello(const msg_t *msg, const msgnetwork_conn_t *conn) {
    msgnetwork_t *net = msgnetwork_conn_get_net(conn);

    printf("[%s] %s says %s\n", name, msg.name, msg.text);
    msg_t *msg = msg_new(0x1, msg_ack_serialize());
    /* send acknowledgement */
    send_msg(msg, conn);
    msg_free(msg);
}

void on_receive_ack(const msg_t *msg, const msgnetwork_conn_t *conn) {
    auto net = static_cast<MyNet *>(conn->get_net());
    printf("[%s] the peer knows\n", net->name.c_str());
}

void conn_handler(const msgnetwork_conn_t *conn, bool connected) {
    msgnetwork_t *net = msgnetwork_conn_get_net(conn);
    const char *name = net == alice.net ? alice.name : bob.name;
    if (connected)
    {
        if (conn->get_mode() == ConnPool::Conn::ACTIVE)
        {
            puts("[%s] Connected, sending hello.", name);
            /* send the first message through this connection */
            msgnetwork_send_msg(alice,
                msg_hello_serialize("alice", "Hello there!"), conn);
        }
        else
            printf("[%s] Accepted, waiting for greetings.\n", name);
    }
    else
    {
        printf("[%s] Disconnected, retrying.\n", name);
        /* try to reconnect to the same address */
        connect(conn->get_addr());
    }
}

MyNet gen_mynet(const eventcontext_t *ec,
                const char *name,
                const netaddr_t *peer) {
    MyNet res;
    const msgnetwork_config_t *netconfig = msgnetwork_config_new();
    res.net = msgnetwork_new(ec, netconfig);
    res.name = name;
    res.peer = peer;
};


void on_term_signal(int) {
    ec.stop();
}

int main() {
    eventcontext_t *ec = eventcontext_new();
    netaddr_t *alice_addr = netaddr_new_from_sipport("127.0.0.1:12345");
    netaddr_t *bob_addr = netaddr_new_from_sipport("127.0.0.1:12346");

    /* test two nodes in the same main loop */
    alice = gen_mynet(ec, "Alice", bob_addr);
    bob = gen_mynet(ec, "Bob", alice_addr);

    msgnetwork_reg_handler(alice.net, MSG_OPCODE_HELLO, on_receive_hello);
    msgnetwork_reg_handler(alice.net, MSG_OPCODE_HELLO, on_receive_hello);
    msgnetwork_reg_handler(bob.net, MSG_OPCODE_HELLO, on_receive_hello);
    msgnetwork_reg_handler(bob.net, MSG_OPCODE_HELLO, on_receive_hello);

    /* start all threads */
    msgnetwork_start(alice.net);
    msgnetwork_start(bob.net);

    /* accept incoming connections */
    msgnetwork_listen(alice_addr);
    msgnetwork_listen(bob_addr);

    /* try to connect once */
    msgnetwork_connect(bob_addr);
    msgnetwork_connect(alice_addr);

    /* the main loop can be shutdown by ctrl-c or kill */
    sigev_t *ev_sigint = sigev_new(ec, on_term_signal);
    sigev_t *ev_sigterm = sigev_new(ec, on_term_signal);
    sigev_add(ev_sigint, SIGINT);
    sigev_add(ev_sigterm, SIGTERM);

    /* enter the main loop */
    eventcontext_dispatch(ec);
    return 0;
}
