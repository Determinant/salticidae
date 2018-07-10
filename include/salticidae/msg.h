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

#ifndef _SALTICIDAE_MSG_H
#define _SALTICIDAE_MSG_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "salticidae/type.h"
#include "salticidae/stream.h"
#include "salticidae/netaddr.h"

namespace salticidae {

template<typename OpcodeType = uint8_t,
        const OpcodeType PING = 0xf0,
        const OpcodeType PONG = 0xf1>
class MsgBase {
    public:
    using opcode_t = OpcodeType;
    static const opcode_t OPCODE_PING;
    static const opcode_t OPCODE_PONG;
    static const size_t header_size;

    private:
    /* header */
    /* all integers are encoded in little endian in the protocol */
    uint32_t magic;
    opcode_t opcode;
    uint32_t length;
    uint32_t checksum;

    mutable bytearray_t payload;
    mutable bool no_payload;

    public:
    MsgBase(): magic(0x0), no_payload(true) {}

    MsgBase(const MsgBase &other):
            magic(other.magic),
            opcode(other.opcode),
            length(other.length),
            checksum(other.checksum),
            payload(other.payload),
            no_payload(other.no_payload) {}

    MsgBase(MsgBase &&other):
            magic(other.magic),
            opcode(std::move(other.opcode)),
            length(other.length),
            checksum(other.checksum),
            payload(std::move(other.payload)),
            no_payload(other.no_payload) {}

    MsgBase(const uint8_t *raw_header): no_payload(true) {
        uint32_t _magic;
        opcode_t _opcode;
        uint32_t _length;
        uint32_t _checksum;
        DataStream s(raw_header, raw_header + MsgBase::header_size);

        s >> _magic
          >> _opcode
          >> _length
          >> _checksum;
        magic = letoh(_magic);
        opcode = _opcode;
        length = letoh(_length);
        checksum = letoh(_checksum);
    }

    MsgBase &operator=(const MsgBase &other) {
        magic = other.magic;
        opcode = other.opcode;
        length = other.length;
        checksum = other.checksum;
        payload = other.payload;
        no_payload = other.no_payload;
        return *this;
    }

    MsgBase &operator=(MsgBase &&other) {
        magic = other.magic;
        opcode = std::move(other.opcode);
        length = other.length;
        checksum = other.checksum;
        payload = std::move(other.payload);
        no_payload = other.no_payload;
        return *this;
    }

    ~MsgBase() {}

    size_t get_length() const { return length; }

    const opcode_t &get_opcode() const { return opcode; }

    void set_opcode(const opcode_t &_opcode) {
        opcode = _opcode;
    }

    bytearray_t &&get_payload() const {
#ifndef SALTICIDAE_NOCHECK
        if (no_payload)
            throw std::runtime_error("payload not available");
        no_payload = true;
#endif
        return std::move(payload);
    }

    void set_payload(DataStream &&s) {
        set_payload(bytearray_t(std::move(s)));
    }

    void set_payload(bytearray_t &&_payload) {
        payload = std::move(_payload);
#ifndef SALTICIDAE_NOCHECK
        no_payload = false;
#endif
        length = payload.size();
        checksum = get_checksum();
    }

    operator std::string() const {
        DataStream s;
        s << "<msg "
          << "magic=" << get_hex(magic) << " "
          << "opcode=" << get_hex(opcode) << " "
          << "length=" << std::to_string(length) << " "
          << "checksum=" << get_hex(checksum) << " "
          << "payload=" << get_hex(payload) << ">";
        return std::string(std::move(s));
    }

    uint32_t get_checksum() const {
        static class SHA256 sha256;
        uint32_t res;
        bytearray_t tmp;
#ifndef SALTICIDAE_NOCHECK
        if (no_payload)
            throw std::runtime_error("payload not available");
#endif
        sha256.reset();
        sha256.update(payload);
        sha256.digest(tmp);
        sha256.reset();
        sha256.update(tmp);
        sha256.digest(tmp);
        memmove(&res, &*tmp.begin(), 4);
        return res;
    }

    bool verify_checksum() const {
        return checksum == get_checksum();
    }

    bytearray_t serialize() const {
        DataStream s;
        s << htole(magic)
          << opcode
          << htole(length)
          << htole(checksum)
          << payload;
        return std::move(s);
    }

    void gen_ping(uint16_t port) {
        DataStream s;
        set_opcode(OPCODE_PING);
        s << htole(port);
        set_payload(std::move(s));
    }

    void parse_ping(uint16_t &port) const {
        DataStream s(get_payload());
        s >> port;
        port = letoh(port);
    }

    void gen_pong(uint16_t port) {
        DataStream s;
        set_opcode(OPCODE_PONG);
        s << htole(port);
        set_payload(std::move(s));
    }

    void parse_pong(uint16_t &port) const {
        DataStream s(get_payload());
        s >> port;
        port = letoh(port);
    }

    void gen_hash_list(DataStream &s,
                        const std::vector<uint256_t> &hashes) {
        uint32_t size = htole((uint32_t)hashes.size());
        s << size;
        for (const auto &h: hashes) s << h;
    }

    void parse_hash_list(DataStream &s,
                        std::vector<uint256_t> &hashes) const {
        uint32_t size;
        hashes.clear();

        s >> size;
        size = letoh(size);

        hashes.resize(size);
        for (auto &hash: hashes) s >> hash;
    }

};

template<typename OpcodeType,
        OpcodeType _,
        OpcodeType __>
const size_t MsgBase<OpcodeType, _, __>::header_size =
    sizeof(MsgBase<OpcodeType, _, __>::magic) +
    sizeof(MsgBase<OpcodeType, _, __>::opcode) +
    sizeof(MsgBase<OpcodeType, _, __>::length) +
    sizeof(MsgBase<OpcodeType, _, __>::checksum);

template<typename OpcodeType,
        OpcodeType PING,
        OpcodeType _>
const OpcodeType MsgBase<OpcodeType, PING, _>::OPCODE_PING = PING;

template<typename OpcodeType,
        OpcodeType _,
        OpcodeType PONG>
const OpcodeType MsgBase<OpcodeType, _, PONG>::OPCODE_PONG = PONG;

}

#endif
