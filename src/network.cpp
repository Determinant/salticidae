#include "salticidae/network.h"
#ifdef SALTICIDAE_CBINDINGS

using namespace salticidae;

extern "C" {

msg_t _test_create_msg() {
    return msg_t(0x0, bytearray_t());
}


msgnetwork_t *msgnetwork_new(const EventContext *ec, const msgnetwork_config_t *config) {
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

}

#endif
