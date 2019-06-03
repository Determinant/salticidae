#include "salticidae/netaddr.h"

using namespace salticidae;

#ifdef __cplusplus

extern "C" {

netaddr_t *netaddr_new() { return new NetAddr(); }
netaddr_t *netaddr_new_from_ip_port(uint32_t ip, uint16_t port) {
    return new NetAddr(ip, port);
}

netaddr_t *netaddr_new_from_sip_port(const char *ip, uint16_t port) {
    return new NetAddr(ip, port);
}

netaddr_t *netaddr_new_from_sipport(const char *ip_port_addr) {
    return new NetAddr(ip_port_addr);
}

bool netaddr_is_eq(const netaddr_t *a, const netaddr_t *b) {
    return *a == *b;
}

bool netaddr_is_null(const netaddr_t *self) { return self->is_null(); }

}

#endif
