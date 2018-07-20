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

#include <cstring>
#include <cassert>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>

#include "salticidae/util.h"
#include "salticidae/conn.h"

namespace salticidae {

ConnPool::Conn::operator std::string() const {
    DataStream s;
    s << "<Conn "
      << "fd=" << std::to_string(fd) << " "
      << "addr=" << std::string(addr) << " "
      << "mode=" << ((mode == Conn::ACTIVE) ? "active" : "passive") << ">";
    return std::move(s);
}

void ConnPool::Conn::send_data(evutil_socket_t fd, short events) {
    if (!(events & EV_WRITE)) return;
    auto conn = self(); /* pin the connection */
    ssize_t ret = seg_buff_size;
    while (!send_buffer.empty() && ret == (ssize_t)seg_buff_size)
    {
        bytearray_t buff_seg = send_buffer.pop(seg_buff_size);
        ssize_t size = buff_seg.size();
        ret = send(fd, buff_seg.data(), size, MSG_NOSIGNAL);
        SALTICIDAE_LOG_DEBUG("socket sent %zd bytes", ret);
        size -= ret;
        if (size > 0)
        {
            if (ret < 1) /* nothing is sent */
            {
                /* rewind the whole buff_seg */
                send_buffer.push(std::move(buff_seg));
                if (ret < 0 && errno != EWOULDBLOCK)
                {
                    SALTICIDAE_LOG_INFO("reason: %s", strerror(errno));
                    terminate();
                    return;
                }
            }
            else
            {
                /* rewind the leftover */
                bytearray_t left_over;
                left_over.resize(size);
                memmove(left_over.data(), buff_seg.data() + ret, size);
                send_buffer.push(std::move(left_over));
            }
            /* wait for the next write callback */
            ready_send = false;
            ev_write.add();
            return;
        }
    }
    /* consumed the buffer but endpoint still seems to be writable */
    ready_send = true;
}

void ConnPool::Conn::recv_data(evutil_socket_t fd, short events) {
    if (!(events & EV_READ)) return;
    auto conn = self(); /* pin the connection */
    ssize_t ret = seg_buff_size;
    while (ret == (ssize_t)seg_buff_size)
    {
        bytearray_t buff_seg;
        buff_seg.resize(seg_buff_size);
        ret = recv(fd, buff_seg.data(), seg_buff_size, 0);
        SALTICIDAE_LOG_DEBUG("socket read %zd bytes", ret);
        if (ret < 0 && errno != EWOULDBLOCK)
        {
            SALTICIDAE_LOG_INFO("reason: %s", strerror(errno));
            /* connection err or half-opened connection */
            terminate();
            return;
        }
        if (ret == 0)
        {
            terminate();
            return;
        }
        buff_seg.resize(ret);
        recv_buffer.push(std::move(buff_seg));
    }
    ev_read.add();
    on_read();
}

void ConnPool::accept_client(evutil_socket_t fd, short) {
    int client_fd;
    struct sockaddr client_addr;
    socklen_t addr_size = sizeof(struct sockaddr_in);
    if ((client_fd = accept(fd, &client_addr, &addr_size)) < 0)
        SALTICIDAE_LOG_ERROR("error while accepting the connection");
    else
    {
        int one = 1;
        if (setsockopt(client_fd, SOL_TCP, TCP_NODELAY, (const char *)&one, sizeof(one)) < 0)
            throw ConnPoolError(std::string("setsockopt failed"));
        if (fcntl(client_fd, F_SETFL, O_NONBLOCK) == -1)
            throw ConnPoolError(std::string("unable to set nonblocking socket"));

        NetAddr addr((struct sockaddr_in *)&client_addr);
        conn_t conn = create_conn();
        Conn *conn_ptr = conn.get();
        conn->seg_buff_size = seg_buff_size;
        conn->fd = client_fd;
        conn->cpool = this;
        conn->mode = Conn::PASSIVE;
        conn->addr = addr;
        conn->ev_read = Event(eb, client_fd, EV_READ,
                                std::bind(&Conn::recv_data, conn_ptr, _1, _2));
        conn->ev_write = Event(eb, client_fd, EV_WRITE,
                                std::bind(&Conn::send_data, conn_ptr, _1, _2));
        conn->ev_read.add();
        conn->ev_write.add();
        add_conn(conn);
        SALTICIDAE_LOG_INFO("created %s", std::string(*conn).c_str());
        conn->on_setup();
    }
    ev_listen.add();
}

void ConnPool::Conn::conn_server(evutil_socket_t fd, short events) {
    auto conn = self(); /* pin the connection */
    if (send(fd, "", 0, MSG_NOSIGNAL) == 0)
    {
        ev_read = Event(cpool->eb, fd, EV_READ,
                std::bind(&Conn::recv_data, this, _1, _2));
        ev_write = Event(cpool->eb, fd, EV_WRITE,
                std::bind(&Conn::send_data, this, _1, _2));
        ev_read.add();
        ev_write.add();
        ev_connect.clear();
        SALTICIDAE_LOG_INFO("connected to peer %s", std::string(*this).c_str());
        on_setup();
    }
    else
    {
        if (events & EV_TIMEOUT)
            SALTICIDAE_LOG_INFO("%s connect timeout", std::string(*this).c_str());
        terminate();
        return;
    }
}

void ConnPool::listen(NetAddr listen_addr) {
    int one = 1;
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        throw ConnPoolError(std::string("cannot create socket for listening"));
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one)) < 0 ||
        setsockopt(listen_fd, SOL_TCP, TCP_NODELAY, (const char *)&one, sizeof(one)) < 0)
        throw ConnPoolError(std::string("setsockopt failed"));
    if (fcntl(listen_fd, F_SETFL, O_NONBLOCK) == -1)
        throw ConnPoolError(std::string("unable to set nonblocking socket"));

    struct sockaddr_in sockin;
    memset(&sockin, 0, sizeof(struct sockaddr_in));
    sockin.sin_family = AF_INET;
    sockin.sin_addr.s_addr = INADDR_ANY;
    sockin.sin_port = listen_addr.port;

    if (bind(listen_fd, (struct sockaddr *)&sockin, sizeof(sockin)) < 0)
        throw ConnPoolError(std::string("binding error"));
    if (::listen(listen_fd, max_listen_backlog) < 0)
        throw ConnPoolError(std::string("listen error"));
    ev_listen = Event(eb, listen_fd, EV_READ,
            std::bind(&ConnPool::accept_client, this, _1, _2));
    ev_listen.add();
    SALTICIDAE_LOG_INFO("listening to %u", ntohs(listen_addr.port));
}

void ConnPool::Conn::terminate() {
    auto &pool = cpool->pool;
    auto it = pool.find(fd);
    if (it != pool.end())
    {
        /* temporarily pin the conn before it dies */
        auto conn = it->second;
        assert(conn.get() == this);
        pool.erase(it);
        on_close();
        /* inform the upper layer the connection will be destroyed */
        on_teardown();
    }
}

ConnPool::conn_t ConnPool::connect(const NetAddr &addr) {
    int fd;
    int one = 1;
    if ((fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        throw ConnPoolError(std::string("cannot create socket for remote"));
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one)) < 0 ||
        setsockopt(fd, SOL_TCP, TCP_NODELAY, (const char *)&one, sizeof(one)) < 0)
        throw ConnPoolError(std::string("setsockopt failed"));
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1)
        throw ConnPoolError(std::string("unable to set nonblocking socket"));
    conn_t conn = create_conn();
    conn->seg_buff_size = seg_buff_size;
    conn->fd = fd;
    conn->cpool = this;
    conn->mode = Conn::ACTIVE;
    conn->addr = addr;

    struct sockaddr_in sockin;
    memset(&sockin, 0, sizeof(struct sockaddr_in));
    sockin.sin_family = AF_INET;
    sockin.sin_addr.s_addr = addr.ip;
    sockin.sin_port = addr.port;

    if (::connect(fd, (struct sockaddr *)&sockin,
                sizeof(struct sockaddr_in)) < 0 && errno != EINPROGRESS)
    {
            SALTICIDAE_LOG_INFO("cannot connect to %s", std::string(addr).c_str());
            conn->terminate();
    }
    else
    {
        conn->ev_connect = Event(eb, fd, EV_WRITE,
                    std::bind(&Conn::conn_server, conn.get(), _1, _2));
        conn->ev_connect.add_with_timeout(conn_server_timeout);

        add_conn(conn);
        SALTICIDAE_LOG_INFO("created %s", std::string(*conn).c_str());
    }
    return conn;
}

ConnPool::conn_t ConnPool::add_conn(conn_t conn) {
    auto it = pool.find(conn->fd);
    if (it != pool.end())
    {
        auto old_conn = it->second;
        old_conn->terminate();
    }
    return pool.insert(std::make_pair(conn->fd, conn)).first->second;
}

}
