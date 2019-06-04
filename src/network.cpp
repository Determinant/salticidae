#ifdef SALTICIDAE_CBINDINGS
#include "salticidae/network.h"

using namespace salticidae;

extern "C" {

msgnetwork_t *msgnetwork_new(const eventcontext_t *ec, const msgnetwork_config_t *config) {
    return new msgnetwork_t(*ec, *config);
}

bool msgnetwork_send_msg(msgnetwork_t *self,
                        const msg_t *msg, const msgnetwork_conn_t *conn) {
    return self->send_msg(*msg,  *conn);
}

msgnetwork_conn_t *msgnetwork_connect(msgnetwork_t *self, const netaddr_t *addr) {
    return new msgnetwork_t::conn_t(self->connect(*addr));
}

void msgnetwork_listen(msgnetwork_t *self, const netaddr_t *listen_addr) {
    self->listen(*listen_addr);
}

#ifdef SALTICIDAE_CBINDINGS_STR_OP
void msgnetwork_reg_handler(msgnetwork_t *self,
                            const char *opcode,
                            msgnetwork_msg_callback_t cb) {
    self->set_handler(std::string(opcode),
        [cb](const msgnetwork_t::Msg &msg, const msgnetwork_t::conn_t &conn) {
            cb(&msg, &conn);
        });
}
#else
void msgnetwork_reg_handler(msgnetwork_t *self,
                            uint8_t opcode,
                            msgnetwork_msg_callback_t cb) {
    self->set_handler(opcode,
        [cb](const msgnetwork_t::Msg &msg, const msgnetwork_t::conn_t &conn) {
            cb(&msg, &conn);
        });
}
#endif

void msgnetwork_reg_conn_handler(msgnetwork_t *self, msgnetwork_conn_callback_t cb) {
    self->reg_conn_handler([cb](const ConnPool::conn_t &_conn, bool connected) {
        auto conn = salticidae::static_pointer_cast<msgnetwork_t::Conn>(_conn);
        cb(&conn, connected);
    });
}

msgnetwork_t *msgnetwork_conn_get_net(const msgnetwork_conn_t *conn) {
    return (*conn)->get_net();
}

void msgnetwork_start(msgnetwork_t *self) { self->start(); }

}

#endif
