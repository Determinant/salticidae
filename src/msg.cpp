#include "salticidae/config.h"
#ifdef SALTICIDAE_CBINDINGS
#include "salticidae/msg.h"

extern "C" {

msg_t *msg_new(_opcode_t opcode, bytearray_t *_moved_payload) {
    auto res = new msg_t(opcode, std::move(*_moved_payload));
    bytearray_free(_moved_payload);
    return res;
}

void msg_free(msg_t *msg) { delete msg; }

datastream_t *msg_get_payload(const msg_t *msg) {
    return new datastream_t(msg->get_payload());
}

_opcode_t msg_get_opcode(const msg_t *msg) {
    return msg->get_opcode();
}

}

#endif
