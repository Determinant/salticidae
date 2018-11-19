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
      << "mode=";
    switch (mode)
    {
        case Conn::ACTIVE: s << "active"; break;
        case Conn::PASSIVE: s << "passive"; break;
        case Conn::DEAD: s << "dead"; break;
    }
    s << ">";
    return std::move(s);
}

/* the following two functions are executed by exactly one worker per Conn object */

void ConnPool::Conn::send_data(int fd, int events) {
    if (events & Event::ERROR)
    {
        worker_terminate();
        return;
    }
    auto conn = self(); /* pin the connection */
    ssize_t ret = seg_buff_size;
    for (;;)
    {
        bytearray_t buff_seg = send_buffer.move_pop();
        ssize_t size = buff_seg.size();
        if (!size) break;
        ret = send(fd, buff_seg.data(), size, MSG_NOSIGNAL);
        SALTICIDAE_LOG_DEBUG("socket sent %zd bytes", ret);
        size -= ret;
        if (size > 0)
        {
            if (ret < 1) /* nothing is sent */
            {
                /* rewind the whole buff_seg */
                send_buffer.rewind(std::move(buff_seg));
                if (ret < 0 && errno != EWOULDBLOCK)
                {
                    SALTICIDAE_LOG_INFO("send(%d) failure: %s", fd, strerror(errno));
                    worker_terminate();
                    return;
                }
            }
            else
                /* rewind the leftover */
                send_buffer.rewind(
                    bytearray_t(buff_seg.begin() + ret, buff_seg.end()));
            /* wait for the next write callback */
            ready_send = false;
            //ev_write.add();
            return;
        }
    }
    ev_socket.del();
    ev_socket.add(Event::READ);
    /* consumed the buffer but endpoint still seems to be writable */
    ready_send = true;
}

void ConnPool::Conn::recv_data(int fd, int events) {
    if (events & Event::ERROR)
    {
        worker_terminate();
        return;
    }
    auto conn = self(); /* pin the connection */
    ssize_t ret = seg_buff_size;
    while (ret == (ssize_t)seg_buff_size)
    {
        bytearray_t buff_seg;
        buff_seg.resize(seg_buff_size);
        ret = recv(fd, buff_seg.data(), seg_buff_size, 0);
        SALTICIDAE_LOG_DEBUG("socket read %zd bytes", ret);
        if (ret < 0)
        {
            if (errno == EWOULDBLOCK) break;
            SALTICIDAE_LOG_INFO("recv(%d) failure: %s", fd, strerror(errno));
            /* connection err or half-opened connection */
            worker_terminate();
            return;
        }
        if (ret == 0)
        {
            //SALTICIDAE_LOG_INFO("recv(%d) terminates", fd, strerror(errno));
            worker_terminate();
            return;
        }
        buff_seg.resize(ret);
        recv_buffer.push(std::move(buff_seg));
    }
    //ev_read.add();
    on_read();
}

void ConnPool::Conn::stop() {
    if (mode != ConnMode::DEAD)
    {
        if (worker) worker->unfeed();
        ev_connect.clear();
        ev_socket.clear();
        send_buffer.get_queue().unreg_handler();
        mode = ConnMode::DEAD;
    }
}

void ConnPool::Conn::worker_terminate() {
    auto conn = self();
    if (!conn) return;
    stop();
    if (!worker->is_dispatcher())
        cpool->disp_tcall->async_call(
                [cpool=this->cpool, conn](ThreadCall::Handle &) {
            cpool->del_conn(conn);
        });
    else cpool->del_conn(conn);
}

void ConnPool::Conn::disp_terminate() {
    auto conn = self();
    if (!conn) return;
    if (worker && !worker->is_dispatcher())
        worker->get_tcall()->call([conn](ThreadCall::Handle &) {
            conn->stop();
        });
    else stop();
    cpool->del_conn(conn);
}

void ConnPool::accept_client(int fd, int) {
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
        conn->self_ref = conn;
        conn->seg_buff_size = seg_buff_size;
        conn->fd = client_fd;
        conn->cpool = this;
        conn->mode = Conn::PASSIVE;
        conn->addr = addr;
        add_conn(conn);
        SALTICIDAE_LOG_INFO("accepted %s", std::string(*conn).c_str());
        auto &worker = select_worker();
        conn->worker = &worker;
        conn->on_setup();
        update_conn(conn, true);
        worker.feed(conn, client_fd);
    }
}

void ConnPool::Conn::conn_server(int fd, int events) {
    auto conn = self(); /* pin the connection */
    if (!conn) return;
    if (send(fd, "", 0, MSG_NOSIGNAL) == 0)
    {
        ev_connect.clear();
        SALTICIDAE_LOG_INFO("connected to remote %s", std::string(*this).c_str());
        worker = &(cpool->select_worker());
        on_setup();
        cpool->update_conn(conn, true);
        worker->feed(conn, fd);
    }
    else
    {
        if (events & Event::TIMEOUT)
            SALTICIDAE_LOG_INFO("%s connect timeout", std::string(*this).c_str());
        conn->disp_terminate();
        return;
    }
}

void ConnPool::_listen(NetAddr listen_addr) {
    int one = 1;
    if (listen_fd != -1)
    { /* reset the previous listen() */
        ev_listen.clear();
        close(listen_fd);
    }
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
    ev_listen = Event(disp_ec, listen_fd,
                    std::bind(&ConnPool::accept_client, this, _1, _2));
    ev_listen.add(Event::READ);
    SALTICIDAE_LOG_INFO("listening to %u", ntohs(listen_addr.port));
}

ConnPool::conn_t ConnPool::_connect(const NetAddr &addr) {
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
    conn->self_ref = conn;
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
        conn->disp_terminate();
    }
    else
    {
        conn->ev_connect = Event(disp_ec, conn->fd, std::bind(&Conn::conn_server, conn.get(), _1, _2));
        conn->ev_connect.add_with_timeout(conn_server_timeout, Event::WRITE);
        add_conn(conn);
        SALTICIDAE_LOG_INFO("created %s", std::string(*conn).c_str());
    }
    return conn;
}

void ConnPool::del_conn(const conn_t &conn) {
    auto it = pool.find(conn->fd);
    if (it != pool.end())
    {
        /* temporarily pin the conn before it dies */
        auto conn = it->second;
        //assert(conn->fd == fd);
        pool.erase(it);
        /* inform the upper layer the connection will be destroyed */
        conn->on_teardown();
        update_conn(conn, false);
        conn->release_self(); /* remove the self-cycle */
        ::close(conn->fd);
        SALTICIDAE_LOG_INFO("remove_conn: %s", std::string(*conn).c_str());
        conn->fd = -1;
    }
}

ConnPool::conn_t ConnPool::add_conn(const conn_t &conn) {
    //assert(pool.find(conn->fd) == pool.end());
    return pool.insert(std::make_pair(conn->fd, conn)).first->second;
}

}
