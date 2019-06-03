#include "salticidae/stream.h"

using namespace salticidae;

#ifdef __cplusplus

extern "C" {

uint256_t *uint256_new() { return new uint256_t(); }
uint256_t *uint256_new_from_bytes(const uint8_t *arr) {
    return new uint256_t(arr);
}

bool uint256_is_null(const uint256_t *self) { return self->is_null(); }
bool uint256_is_eq(const uint256_t *a, const uint256_t *b) {
    return *a == *b;
}

void uint256_serialize(const uint256_t *self, datastream_t *s) {
    self->serialize(*s);
}

void uint256_unserialize(uint256_t *self, datastream_t *s) {
    self->unserialize(*s);
}

datastream_t *datastream_new() { return new DataStream(); }
datastream_t *datastream_new_from_bytes(const uint8_t *begin, const uint8_t *end) {
    return new DataStream(begin, end);
}

void datastream_assign_by_copy(datastream_t *dst, const datastream_t *src) {
    *dst = *src;
}

void datastream_assign_by_move(datastream_t *dst, datastream_t *src) {
    *dst = std::move(*src);
}

uint8_t *datastream_data(datastream_t *self) { return self->data(); }

void datastream_clear(datastream_t *self) { self->clear(); }

size_t datastream_size(const datastream_t *self) { return self->size(); }

void datastream_put_u8(datastream_t *self, uint8_t val) { *self << val; }
void datastream_put_u16(datastream_t *self, uint16_t val) { *self << val; }
void datastream_put_u32(datastream_t *self, uint32_t val) { *self << val; }

void datastream_put_i8(datastream_t *self, int8_t val) { *self << val; }
void datastream_put_i16(datastream_t *self, int16_t val) { *self << val; }
void datastream_put_i32(datastream_t *self, int32_t val) { *self << val; }

void datastream_put_data(datastream_t *self,
                        uint8_t *begin, uint8_t *end) {
    self->put_data(begin, end);
}

const uint8_t *datastream_get_data_inplace(datastream_t *self, size_t len) {
    return self->get_data_inplace(len);
}

uint256_t *datastream_get_hash(const datastream_t *self) {
    return new uint256_t(self->get_hash());
}

}

#endif
