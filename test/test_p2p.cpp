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
#include <cstdint>
#include <string>
#include <functional>
#include <unordered_map>

#include "salticidae/msg.h"
#include "salticidae/event.h"
#include "salticidae/network.h"
#include "salticidae/stream.h"

using salticidae::NetAddr;
using salticidae::DataStream;
using salticidae::PeerNetwork;
using salticidae::htole;
using salticidae::letoh;
using salticidae::EventContext;
using salticidae::ThreadCall;
using std::placeholders::_1;
using std::placeholders::_2;

struct Net {
    uint64_t id;
    EventContext ec;
    ThreadCall tc;
    std::thread th;
    PeerNetwork<uint8_t> *net;

    Net(uint64_t id, const std::string &listen_addr): id(id), tc(ec) {
        net = new salticidae::PeerNetwork<uint8_t>(
            ec,
            salticidae::PeerNetwork<uint8_t>::Config().conn_timeout(5).ping_period(2));
        net->reg_error_handler([this](const std::exception &err, bool fatal) {
            SALTICIDAE_LOG_WARN("net %lu: captured %s error: %s", this->id, fatal ? "fatal" : "recoverable", err.what());
        });
        th = std::thread([=](){
            try {
                net->start();
                net->listen(NetAddr(listen_addr));
                SALTICIDAE_LOG_INFO("net %lu: listen to %s\n", id, listen_addr.c_str());
                ec.dispatch();
            } catch (std::exception &err) {
                SALTICIDAE_LOG_WARN("net %lu: got error during a sync call: %s", id, err.what());
            }
            SALTICIDAE_LOG_INFO("net %lu: main loop ended\n", id);
        });
    }

    void add_peer(const std::string &listen_addr) {
        try {
            net->add_peer(NetAddr(listen_addr));
        } catch (std::exception &err) {
            fprintf(stderr, "net %lu: got error during a sync call: %s\n", id, err.what());
        }
    }

    void stop_join() {
        tc.async_call([ec=this->ec](ThreadCall::Handle &) { ec.stop(); });
        th.join();
    }

    ~Net() { delete net; }
};

std::unordered_map<uint64_t, Net *> nets;
std::unordered_map<std::string, std::function<void(char *)> > cmd_map;

int read_int(char *buff) {
    scanf("%64s", buff);
    try {
        int t = std::stoi(buff);
        if (t < 0) throw std::invalid_argument("negative");
        return t;
    } catch (std::invalid_argument) {
        fprintf(stderr, "expect a non-negative integer\n");
        return -1;
    }
}

int main(int argc, char **argv) {
    int i;
    fprintf(stderr, "p2p network library playground (type h for help)\n");
    fprintf(stderr, "================================================\n");

    auto cmd_exit = [](char *) {
        for (auto &p: nets)
            p.second->stop_join();
        exit(0);
    };

    auto cmd_net = [](char *buff) {
        int id = read_int(buff);
        if (id < 0) return;
        if (nets.count(id))
        {
            fprintf(stderr, "net id already exists");
            return;
        }
        scanf("%64s", buff);
        nets.insert(std::make_pair(id, new Net(id, buff)));
    };

    auto cmd_ls = [](char *) {
        for (auto &p: nets)
            fprintf(stderr, "%d\n", p.first);
    };

    auto cmd_rm = [](char *buff) {
        int id = read_int(buff);
        if (id < 0) return;
        auto it = nets.find(id);
        if (it == nets.end())
        {
            fprintf(stderr, "net id does not exist\n");
            return;
        }
        it->second->stop_join();
        delete it->second;
        nets.erase(it);
    };

    auto cmd_addpeer = [](char *buff) {
        int id = read_int(buff);
        if (id < 0) return;
        auto it = nets.find(id);
        if (it == nets.end())
        {
            fprintf(stderr, "net id does not exist\n");
            return;
        }
        scanf("%64s", buff);
        it->second->add_peer(buff);
    };

    cmd_map.insert(std::make_pair("exit", cmd_exit));
    cmd_map.insert(std::make_pair("net", cmd_net));
    cmd_map.insert(std::make_pair("ls", cmd_ls));
    cmd_map.insert(std::make_pair("rm", cmd_rm));
    cmd_map.insert(std::make_pair("addpeer", cmd_addpeer));

    for (;;)
    {
        fprintf(stderr, "> ");
        char buff[128];
        if (scanf("%64s", buff) == EOF) break;
        auto it = cmd_map.find(buff);
        if (it == cmd_map.end())
            fprintf(stderr, "invalid comand \"%s\"\n", buff);
        else
            (it->second)(buff);
    }

    return 0;
}
