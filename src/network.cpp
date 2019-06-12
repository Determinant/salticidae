#include "salticidae/config.h"
#ifdef SALTICIDAE_CBINDINGS
#include "salticidae/network.h"

using namespace salticidae;

extern "C" {

// MsgNetwork

msgnetwork_config_t *msgnetwork_config_new() {
    return new msgnetwork_config_t();
}

void msgnetwork_config_free(const msgnetwork_config_t *self) { delete self; }

msgnetwork_t *msgnetwork_new(const eventcontext_t *ec, const msgnetwork_config_t *config) {
    return new msgnetwork_t(*ec, *config);
}

void msgnetwork_free(const msgnetwork_t *self) { delete self; }

void msgnetwork_config_burst_size(msgnetwork_config_t *self, size_t burst_size) {
    self->burst_size(burst_size);
}

void msgnetwork_config_max_listen_backlog(msgnetwork_config_t *self, int backlog) {
    self->max_listen_backlog(backlog);
}

void msgnetwork_config_conn_server_timeout(msgnetwork_config_t *self, double timeout) {
    self->conn_server_timeout(timeout);
}

void msgnetwork_config_seg_buff_size(msgnetwork_config_t *self, size_t size) {
    self->seg_buff_size(size);
}

void msgnetwork_config_nworker(msgnetwork_config_t *self, size_t nworker) {
    self->nworker(nworker);
}

void msgnetwork_config_queue_capacity(msgnetwork_config_t *self, size_t cap) {
    self->queue_capacity(cap);
}

void msgnetwork_send_msg_by_move(msgnetwork_t *self,
                        msg_t *_moved_msg, const msgnetwork_conn_t *conn) {
    self->_send_msg(std::move(*_moved_msg), *conn);
    delete _moved_msg;
}

msgnetwork_conn_t *msgnetwork_connect(msgnetwork_t *self, const netaddr_t *addr) {
    return new msgnetwork_conn_t(self->connect(*addr));
}

msgnetwork_conn_t *msgnetwork_conn_copy(const msgnetwork_conn_t *self) {
    return new msgnetwork_conn_t(*self);
}


void msgnetwork_conn_free(const msgnetwork_conn_t *self) { delete self; }

void msgnetwork_listen(msgnetwork_t *self, const netaddr_t *listen_addr) {
    self->listen(*listen_addr);
}

void msgnetwork_reg_handler(msgnetwork_t *self,
                            _opcode_t opcode,
                            msgnetwork_msg_callback_t cb,
                            void *userdata) {
    self->set_handler(opcode,
        [=](const msgnetwork_t::Msg &msg, const msgnetwork_conn_t &conn) {
            cb(&msg, &conn, userdata);
        });
}

void msgnetwork_reg_conn_handler(msgnetwork_t *self,
                                msgnetwork_conn_callback_t cb,
                                void *userdata) {
    self->reg_conn_handler([=](const ConnPool::conn_t &_conn, bool connected) {
        auto conn = salticidae::static_pointer_cast<msgnetwork_t::Conn>(_conn);
        cb(&conn, connected, userdata);
    });
}

void msgnetwork_start(msgnetwork_t *self) { self->start(); }

void msgnetwork_terminate(msgnetwork_t *self, const msgnetwork_conn_t *conn) {
    self->terminate(*conn);
}

msgnetwork_t *msgnetwork_conn_get_net(const msgnetwork_conn_t *conn) {
    return (*conn)->get_net();
}

msgnetwork_conn_mode_t msgnetwork_conn_get_mode(const msgnetwork_conn_t *conn) {
    return (msgnetwork_conn_mode_t)(*conn)->get_mode();
}

netaddr_t *msgnetwork_conn_get_addr(const msgnetwork_conn_t *conn) {
    return new netaddr_t((*conn)->get_addr());
}

// PeerNetwork

peernetwork_config_t *peernetwork_config_new() {
    return new peernetwork_config_t();
}

void peernetwork_config_free(const peernetwork_config_t *self) { delete self; }

void peernetwork_config_retry_conn_delay(peernetwork_config_t *self, double t) {
    self->retry_conn_delay(t);
}

void peernetwork_config_ping_period(peernetwork_config_t *self, double t) {
    self->ping_period(t);
}

void peernetwork_config_conn_timeout(peernetwork_config_t *self, double t) {
    self->conn_timeout(t);
}

void peernetwork_config_id_mode(peernetwork_config_t *self, peernetwork_id_mode_t mode) {
    self->id_mode(peernetwork_t::IdentityMode(mode));
}

msgnetwork_config_t *peernetwork_config_as_msgnetwork_config(peernetwork_config_t *self) { return self; }

peernetwork_t *peernetwork_new(const eventcontext_t *ec, const peernetwork_config_t *config) {
    return new peernetwork_t(*ec, *config);
}

void peernetwork_free(const peernetwork_t *self) { delete self; }

void peernetwork_add_peer(peernetwork_t *self, const netaddr_t *paddr) {
    self->add_peer(*paddr);
}

bool peernetwork_has_peer(const peernetwork_t *self, const netaddr_t *paddr) {
    return self->has_peer(*paddr);
}

const peernetwork_conn_t *peernetwork_get_peer_conn(const peernetwork_t *self,
                                        const netaddr_t *paddr) {
    return new peernetwork_conn_t(self->get_peer_conn(*paddr));
}

msgnetwork_t *peernetwork_as_msgnetwork(peernetwork_t *self) { return self; }

msgnetwork_conn_t *msgnetwork_conn_new_from_peernetwork_conn(const peernetwork_conn_t *conn) {
    return new msgnetwork_conn_t(*conn);
}

peernetwork_conn_t *peernetwork_conn_copy(const peernetwork_conn_t *self) {
    return new peernetwork_conn_t(*self);
}

void peernetwork_conn_free(const peernetwork_conn_t *self) { delete self; }

void peernetwork_send_msg_by_move(peernetwork_t *self,
                                msg_t * _moved_msg, const netaddr_t *paddr) {
    self->_send_msg(std::move(*_moved_msg), *paddr);
    delete _moved_msg;
}

void peernetwork_multicast_msg_by_move(peernetwork_t *self,
                                msg_t *_moved_msg,
                                const netaddr_array_t *paddrs) {
    self->_multicast_msg(std::move(*_moved_msg), *paddrs);
    delete _moved_msg;
}

void peernetwork_listen(peernetwork_t *self, const netaddr_t *listen_addr) {
    self->listen(*listen_addr);
}

}

#endif
