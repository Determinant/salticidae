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

#ifndef _SALTICIDAE_NETADDR_H
#define _SALTICIDAE_NETADDR_H

#include <string>
#include <cstring>
#include <cstdint>
#include <arpa/inet.h>

#include "salticidae/util.h"
#include "salticidae/stream.h"

namespace salticidae {

/* TODO: IPv6 support */

struct NetAddr {
    uint32_t ip;
    uint16_t port;
    /* construct from human-readable format */
    NetAddr(): ip(0), port(0) {}

    NetAddr(uint32_t ip, uint16_t port): ip(ip), port(port) {}
    
    NetAddr(const std::string &_addr, uint16_t _port) {
        set_by_ip_port(_addr, _port);
    }
    
    void set_by_ip_port(const std::string &_addr, uint16_t _port) {
        struct hostent *h;
        if ((h = gethostbyname(_addr.c_str())) == nullptr)
            throw SalticidaeError("gethostbyname failed");
        memmove(&ip, h->h_addr_list[0], sizeof(in_addr_t));
        port = htons(_port);
    }
    
    NetAddr(const std::string &ip_port_addr) {
        size_t pos = ip_port_addr.find(":");
        if (pos == std::string::npos)
            throw SalticidaeError("invalid port format");
        std::string ip_str = ip_port_addr.substr(0, pos);
        std::string port_str = ip_port_addr.substr(pos + 1);
        long port;
        try {
            port = std::stol(port_str.c_str());
        } catch (std::logic_error &) {
            throw SalticidaeError("invalid port format");
        }
        if (port < 0)
            throw SalticidaeError("negative port number");
        if (port > 0xffff)
            throw SalticidaeError("port number greater than 0xffff");
        set_by_ip_port(ip_str, (uint16_t)port);
    }
    /* construct from unix socket format */
    NetAddr(const struct sockaddr_in *addr_sock) {
        ip = addr_sock->sin_addr.s_addr;
        port = addr_sock->sin_port;
    }
    
    bool operator==(const NetAddr &other) const {
        return ip == other.ip && port == other.port;
    }

    bool operator!=(const NetAddr &other) const {
        return ip != other.ip || port != other.port;
    }
   
    operator std::string() const {
        struct in_addr in;
        in.s_addr = ip;
        return "<NetAddr " + std::string(inet_ntoa(in)) +
                ":" + std::to_string(ntohs(port)) + ">";
    }

    bool is_null() const { return ip == 0 && port == 0; }

    void serialize(DataStream &s) const { s << ip << port; }

    void unserialize(DataStream &s) { s >> ip >> port; }
};

}

namespace std {
    template <>
    struct hash<salticidae::NetAddr> {
        size_t operator()(const salticidae::NetAddr &k) const {
            return k.ip ^ k.port;
        }
    };

    template <>
    struct hash<const salticidae::NetAddr> {
        size_t operator()(const salticidae::NetAddr &k) const {
            return k.ip ^ k.port;
        }
    };
}

#endif
