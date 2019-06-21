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

msgnetwork_t *msgnetwork_new(const eventcontext_t *ec, const msgnetwork_config_t *config, SalticidaeCError *cerror) {
    SALTICIDAE_CERROR_TRY(cerror)
    return new msgnetwork_t(*ec, *config);
    SALTICIDAE_CERROR_CATCH(cerror)
    return nullptr;
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

void msgnetwork_config_enable_tls(msgnetwork_config_t *self, bool enabled) {
    self->enable_tls(enabled);
}

void msgnetwork_config_tls_key_file(msgnetwork_config_t *self, const char *pem_fname) {
    self->tls_key_file(pem_fname);
}

void msgnetwork_config_tls_cert_file(msgnetwork_config_t *self, const char *pem_fname) {
    self->tls_cert_file(pem_fname);
}

void msgnetwork_config_tls_key_by_move(msgnetwork_config_t *self, pkey_t *key) {
    self->tls_key(new pkey_t(std::move(*key)));
}

void msgnetwork_config_tls_cert_by_move(msgnetwork_config_t *self, x509_t *cert) {
    self->tls_cert(new x509_t(std::move(*cert)));
}

void msgnetwork_send_msg(msgnetwork_t *self, const msg_t *msg, const msgnetwork_conn_t *conn) {
    self->_send_msg(*msg, *conn);
}

void msgnetwork_send_msg_deferred_by_move(msgnetwork_t *self,
                        msg_t *_moved_msg, const msgnetwork_conn_t *conn) {
    self->_send_msg_deferred(std::move(*_moved_msg), *conn);
}

msgnetwork_conn_t *msgnetwork_connect(msgnetwork_t *self, const netaddr_t *addr, SalticidaeCError *cerror) {
    SALTICIDAE_CERROR_TRY(cerror)
    return new msgnetwork_conn_t(self->connect(*addr));
    SALTICIDAE_CERROR_CATCH(cerror)
    return nullptr;
}

msgnetwork_conn_t *msgnetwork_conn_copy(const msgnetwork_conn_t *self) {
    return new msgnetwork_conn_t(*self);
}


void msgnetwork_conn_free(const msgnetwork_conn_t *self) { delete self; }

void msgnetwork_listen(msgnetwork_t *self, const netaddr_t *listen_addr, SalticidaeCError *cerror) {
    SALTICIDAE_CERROR_TRY(cerror)
    self->listen(*listen_addr);
    SALTICIDAE_CERROR_CATCH(cerror)
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
        return cb(&conn, connected, userdata);
    });
}

void msgnetwork_reg_error_handler(msgnetwork_t *self,
                                msgnetwork_error_callback_t cb,
                                void *userdata) {
    self->reg_error_handler([=](const std::exception_ptr _err, bool fatal) {
        SalticidaeCError cerror;
        try {
            std::rethrow_exception(_err);
        } catch (SalticidaeError &err) {
            cerror = err.get_cerr();
        } catch (...) {
            cerror = salticidae_cerror_unknown();
        }
        cb(&cerror, fatal, userdata);
    });
}

void msgnetwork_start(msgnetwork_t *self) { self->start(); }

void msgnetwork_stop(msgnetwork_t *self) { self->stop(); }

void msgnetwork_terminate(msgnetwork_t *self, const msgnetwork_conn_t *conn) {
    self->terminate(*conn);
}

msgnetwork_t *msgnetwork_conn_get_net(const msgnetwork_conn_t *conn) {
    return (*conn)->get_net();
}

msgnetwork_conn_mode_t msgnetwork_conn_get_mode(const msgnetwork_conn_t *conn) {
    return (msgnetwork_conn_mode_t)(*conn)->get_mode();
}

const netaddr_t *msgnetwork_conn_get_addr(const msgnetwork_conn_t *conn) {
    return &(*conn)->get_addr();
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

peernetwork_t *msgnetwork_as_peernetwork_unsafe(msgnetwork_t *self) {
    return static_cast<peernetwork_t *>(self);
}

peernetwork_t *peernetwork_new(const eventcontext_t *ec, const peernetwork_config_t *config, SalticidaeCError *cerror) {
    SALTICIDAE_CERROR_TRY(cerror)
    return new peernetwork_t(*ec, *config);
    SALTICIDAE_CERROR_CATCH(cerror)
    return nullptr;
}

void peernetwork_free(const peernetwork_t *self) { delete self; }

void peernetwork_add_peer(peernetwork_t *self, const netaddr_t *paddr) {
    self->add_peer(*paddr);
}

bool peernetwork_has_peer(const peernetwork_t *self, const netaddr_t *paddr) {
    return self->has_peer(*paddr);
}

const peernetwork_conn_t *peernetwork_get_peer_conn(const peernetwork_t *self,
                                                    const netaddr_t *paddr,
                                                    SalticidaeCError *cerror) {
    SALTICIDAE_CERROR_TRY(cerror)
    return new peernetwork_conn_t(self->get_peer_conn(*paddr));
    SALTICIDAE_CERROR_CATCH(cerror)
    return nullptr;
}

msgnetwork_t *peernetwork_as_msgnetwork(peernetwork_t *self) { return self; }

msgnetwork_conn_t *msgnetwork_conn_new_from_peernetwork_conn(const peernetwork_conn_t *conn) {
    return new msgnetwork_conn_t(*conn);
}

peernetwork_conn_t *peernetwork_conn_new_from_msgnetwork_conn_unsafe(const msgnetwork_conn_t *conn) {
    return new peernetwork_conn_t(salticidae::static_pointer_cast<peernetwork_t::Conn>(*conn));
}

peernetwork_conn_t *peernetwork_conn_copy(const peernetwork_conn_t *self) {
    return new peernetwork_conn_t(*self);
}

void peernetwork_conn_free(const peernetwork_conn_t *self) { delete self; }

void peernetwork_send_msg(peernetwork_t *self,
                        const msg_t * msg, const netaddr_t *paddr) {
    self->_send_msg(*msg, *paddr);
}

void peernetwork_send_msg_deferred_by_move(peernetwork_t *self,
                                        msg_t * _moved_msg, const netaddr_t *paddr) {
    self->_send_msg_deferred(std::move(*_moved_msg), *paddr);
}

void peernetwork_multicast_msg_by_move(peernetwork_t *self,
                                    msg_t *_moved_msg,
                                    const netaddr_array_t *paddrs) {
    self->_multicast_msg(std::move(*_moved_msg), *paddrs);
}

void peernetwork_listen(peernetwork_t *self, const netaddr_t *listen_addr, SalticidaeCError *cerror) {
    SALTICIDAE_CERROR_TRY(cerror)
    self->listen(*listen_addr);
    SALTICIDAE_CERROR_CATCH(cerror)
}

void peernetwork_stop(peernetwork_t *self) { self->stop(); }

void peernetwork_reg_unknown_peer_handler(peernetwork_t *self,
                                        msgnetwork_unknown_peer_callback_t cb,
                                        void *userdata) {
    self->reg_unknown_peer_handler([=](const NetAddr &addr) {
        cb(&addr, userdata);
    });
}

}

#endif
