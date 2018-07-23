Salticidae: a Minimal C++ asynchronous network library.
=======================================================

.. image:: https://img.shields.io/badge/License-MIT-yellow.svg
   :target: https://opensource.org/licenses/MIT


Features
--------

- Simplicity. The library is self-contained, small in code base, and only
  relies on libevent and libcrypo (OpenSSL, for SHA256 purpose).

- Clarity. With moderate use of C++ template and new features, the vast
  majority of the code is self-documented.

- Layered design. You can use network abstraction from the lowest socket
  connection level to the highest P2P network level.

- Performance. The implementation strives to incur very little in processing
  network I/O, and avoid unnecessary memory copies thanks to the move semantics.

- Utilities. The libray also provies with some useful gadgets, such as command
  line parser, libevent abstraction, etc.

Dependencies
------------

- CMake >= 3.9
- C++14
- libevent
- libcrypto

Example
-------

.. code-block:: cpp

   #include <cstdio>
   #include <string>
   
   #include "salticidae/msg.h"
   #include "salticidae/event.h"
   #include "salticidae/network.h"
   #include "salticidae/stream.h"
   
   using salticidae::NetAddr;
   using salticidae::DataStream;
   using salticidae::MsgNetwork;
   using salticidae::htole;
   using salticidae::letoh;
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
   
   using MsgNetworkByteOp = MsgNetwork<opcode_t>;
   
   struct MyNet: public MsgNetworkByteOp {
       const std::string name;
       const NetAddr peer;
   
       MyNet(const salticidae::EventContext &ec,
               const std::string name,
               const NetAddr &peer):
           MsgNetwork<opcode_t>(ec, 10, 1.0, 4096),
           name(name),
           peer(peer) {}
   
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
   };
   
   
   void on_receive_hello(MsgHello &&msg, MyNet::conn_t conn) {
       auto net = conn->get_net();
       printf("[%s] %s says %s\n",
               net->name.c_str(),
               msg.name.c_str(), msg.text.c_str());
       /* send acknowledgement */
       net->send_msg(MsgAck(), conn);
   }
   
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
   
       /* register the message handler */
       alice.reg_handler(on_receive_hello);
       alice.reg_handler(on_receive_ack);
       bob.reg_handler(on_receive_hello);
       bob.reg_handler(on_receive_ack);
   
       alice.listen(alice_addr);
       bob.listen(bob_addr);
   
       /* first attempt */
       alice.connect(bob_addr);
       bob.connect(alice_addr);
   
       ec.dispatch();
       return 0;
   }
