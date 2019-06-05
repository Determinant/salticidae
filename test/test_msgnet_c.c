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
#include <stdlib.h>
#include <signal.h>

#include "salticidae/event.h"
#include "salticidae/network.h"
#include "salticidae/stream.h"

/** Hello Message. */
const uint8_t MSG_OPCODE_HELLO = 0x0;
const uint8_t MSG_OPCODE_ACK = 0x1;
typedef struct MsgHello {
    const char *name;
    const char *text;
} MsgHello;
/** Defines how to serialize the msg. */
msg_t *msg_hello_serialize(const char *name, const char *text) {
    datastream_t *serialized = datastream_new();
    size_t name_len = strlen(name);
    datastream_put_i32(serialized, (uint32_t)htole32(name_len));
    datastream_put_data(serialized, name, name + name_len);
    datastream_put_data(serialized, text, text + strlen(text));
    msg_t *msg = msg_new(MSG_OPCODE_HELLO, datastream_to_bytearray(serialized));
    return msg;
}

/** Defines how to parse the msg. */
MsgHello msg_hello_unserialize(const msg_t *msg) {
    datastream_t *s = msg_get_payload(msg);
    MsgHello res;
    uint32_t len;
    len = datastream_get_u32(s);
    len = le32toh(len);

    char *name = (char *)malloc(len + 1);
    memmove(name, datastream_get_data_inplace(s, len), len);
    name[len] = 0;

    len = datastream_size(s);
    char *text = (char *)malloc(len + 1);
    memmove(text, datastream_get_data_inplace(s, len), len);
    text[len] = 0;

    res.name = name;
    res.text = text;
    datastream_free(s);
    return res;
}

bytearray_t *msg_ack_serialize() { return bytearray_new(); }

typedef struct MyNet {
    msgnetwork_t *net;
    const char *name;
} MyNet;
MyNet alice, bob;

void on_receive_hello(const msg_t *_msg, const msgnetwork_conn_t *conn) {
    msgnetwork_t *net = msgnetwork_conn_get_net(conn);
    const char *name = net == alice.net ? alice.name : bob.name;
    MsgHello msg = msg_hello_unserialize(_msg);
    printf("[%s] %s says %s\n", name, msg.name, msg.text);
    msg_t *ack = msg_new(MSG_OPCODE_ACK, msg_ack_serialize());
    /* send acknowledgement */
    msgnetwork_send_msg(net, ack, conn);
    msg_free(ack);
}

void on_receive_ack(const msg_t *msg, const msgnetwork_conn_t *conn) {
    msgnetwork_t *net = msgnetwork_conn_get_net(conn);
    const char *name = net == alice.net ? alice.name : bob.name;
    printf("[%s] the peer knows\n", name);
}

void conn_handler(const msgnetwork_conn_t *conn, bool connected) {
    msgnetwork_t *net = msgnetwork_conn_get_net(conn);
    MyNet *n = net == alice.net ? &alice: &bob;
    const char *name = n->name;
    if (connected)
    {
        if (msgnetwork_conn_get_mode(conn) == CONN_MODE_ACTIVE)
        {
            printf("[%s] Connected, sending hello.", name);
            /* send the first message through this connection */
            msgnetwork_send_msg(n->net,
                msg_hello_serialize(name, "Hello there!"), conn);
        }
        else
            printf("[%s] Accepted, waiting for greetings.\n", name);
    }
    else
    {
        printf("[%s] Disconnected, retrying.\n", name);
        /* try to reconnect to the same address */
        msgnetwork_connect(net, msgnetwork_conn_get_addr(conn));
    }
}

MyNet gen_mynet(const eventcontext_t *ec,
                const char *name) {
    MyNet res;
    const msgnetwork_config_t *netconfig = msgnetwork_config_new();
    res.net = msgnetwork_new(ec, netconfig);
    res.name = name;
};

static eventcontext_t *ec;

void on_term_signal(int sig) {
    eventcontext_stop(ec);
}

int main() {
    ec = eventcontext_new();
    netaddr_t *alice_addr = netaddr_new_from_sipport("127.0.0.1:12345");
    netaddr_t *bob_addr = netaddr_new_from_sipport("127.0.0.1:12346");

    /* test two nodes in the same main loop */
    alice = gen_mynet(ec, "Alice");
    bob = gen_mynet(ec, "Bob");

    msgnetwork_reg_handler(alice.net, MSG_OPCODE_HELLO, on_receive_hello);
    msgnetwork_reg_handler(alice.net, MSG_OPCODE_ACK, on_receive_ack);
    msgnetwork_reg_handler(bob.net, MSG_OPCODE_HELLO, on_receive_hello);
    msgnetwork_reg_handler(bob.net, MSG_OPCODE_ACK, on_receive_ack);

    /* start all threads */
    msgnetwork_start(alice.net);
    msgnetwork_start(bob.net);

    /* accept incoming connections */
    msgnetwork_listen(alice.net, alice_addr);
    msgnetwork_listen(bob.net, bob_addr);

    /* try to connect once */
    msgnetwork_connect(alice.net, bob_addr);
    msgnetwork_connect(bob.net, alice_addr);

    /* the main loop can be shutdown by ctrl-c or kill */
    sigev_t *ev_sigint = sigev_new(ec, on_term_signal);
    sigev_t *ev_sigterm = sigev_new(ec, on_term_signal);
    sigev_add(ev_sigint, SIGINT);
    sigev_add(ev_sigterm, SIGTERM);

    /* enter the main loop */
    eventcontext_dispatch(ec);
    return 0;
}
