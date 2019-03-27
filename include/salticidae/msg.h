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

template<typename OpcodeType>
class MsgBase {
    public:
    using opcode_t = OpcodeType;
    static const size_t header_size;

    private:
    /* header */
    /* all integers are encoded in little endian in the protocol */
    uint32_t magic;
    opcode_t opcode;
    uint32_t length;
#ifndef SALTICIDAE_NOCHECKSUM
    uint32_t checksum;
#endif

    mutable bytearray_t payload;
    mutable bool no_payload;

    public:
    MsgBase(): magic(0x0), opcode(0xff), no_payload(true) {}

    template<typename MsgType,
            typename = typename std::enable_if<
                !std::is_same<MsgType, MsgBase>::value &&
                !std::is_same<MsgType, bytearray_t>::value &&
                !std::is_same<MsgType, DataStream>::value>::type>
    MsgBase(const MsgType &msg): magic(0x0) {
        set_opcode(MsgType::opcode);
        set_payload(std::move(msg.serialized));
        set_checksum();
    }

    MsgBase(const MsgBase &other):
            magic(other.magic),
            opcode(other.opcode),
            length(other.length),
#ifndef SALTICIDAE_NOCHECKSUM
            checksum(other.checksum),
#endif
            payload(other.payload),
            no_payload(other.no_payload) {}

    MsgBase(MsgBase &&other):
            magic(other.magic),
            opcode(std::move(other.opcode)),
            length(other.length),
#ifndef SALTICIDAE_NOCHECKSUM
            checksum(other.checksum),
#endif
            payload(std::move(other.payload)),
            no_payload(other.no_payload) {}

    MsgBase(DataStream &&s): no_payload(true) {
        uint32_t _magic;
        opcode_t _opcode;
        uint32_t _length;
#ifndef SALTICIDAE_NOCHECKSUM
        uint32_t _checksum;
#endif
        s >> _magic
          >> _opcode
          >> _length
#ifndef SALTICIDAE_NOCHECKSUM
          >> _checksum
#endif
          ;
        magic = letoh(_magic);
        opcode = _opcode;
        length = letoh(_length);
#ifndef SALTICIDAE_NOCHECKSUM
        checksum = letoh(_checksum);
#endif
    }

    void swap(MsgBase &other) {
        std::swap(magic, other.magic);
        std::swap(opcode, other.opcode);
        std::swap(length, other.length);
#ifndef SALTICIDAE_NOCHECKSUM
        std::swap(checksum, other.checksum);
#endif
        std::swap(payload, other.payload);
        std::swap(no_payload, other.no_payload);
    }

    MsgBase &operator=(const MsgBase &other) {
        if (this != &other)
        {
            MsgBase tmp(other);
            tmp.swap(*this);
        }
        return *this;
    }

    MsgBase &operator=(MsgBase &&other) {
        if (this != &other)
        {
            MsgBase tmp(std::move(other));
            tmp.swap(*this);
        }
        return *this;
    }

    ~MsgBase() {}

    size_t get_length() const { return length; }

    const opcode_t &get_opcode() const { return opcode; }

    void set_opcode(const opcode_t &_opcode) {
        opcode = _opcode;
    }

    DataStream get_payload() const {
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
    }

    void set_checksum() {
#ifndef SALTICIDAE_NOCHECKSUM
        checksum = get_checksum();
#endif
    }

    operator std::string() const {
        DataStream s;
        s << "<msg "
          << "magic=" << get_hex(magic) << " "
          << "opcode=" << get_hex(opcode) << " "
          << "length=" << std::to_string(length) << " "
#ifndef SALTICIDAE_NOCHECKSUM
          << "checksum=" << get_hex(checksum) << " "
#endif
          << "payload=" << get_hex(payload) << ">";
        return std::move(s);
    }

#ifndef SALTICIDAE_NOCHECKSUM
    uint32_t get_checksum() const {
        static thread_local class SHA256 sha256;
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
#endif

    bytearray_t serialize() const {
        DataStream s;
        s << htole(magic)
          << opcode
          << htole(length)
#ifndef SALTICIDAE_NOCHECKSUM
          << htole(checksum)
#endif
          << payload;
        return std::move(s);
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

template<typename OpcodeType>
const size_t MsgBase<OpcodeType>::header_size =
    sizeof(MsgBase<OpcodeType>::magic) +
    sizeof(MsgBase<OpcodeType>::opcode) +
    sizeof(MsgBase<OpcodeType>::length) +
#ifndef SALTICIDAE_NOCHECKSUM
    sizeof(MsgBase<OpcodeType>::checksum) +
#endif
    0;
}

#endif
