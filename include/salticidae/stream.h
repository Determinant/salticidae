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

#ifndef _SALTICIDAE_STREAM_H
#define _SALTICIDAE_STREAM_H

#include "salticidae/type.h"
#include "salticidae/crypto.h"

namespace salticidae {

template<size_t N, typename T> class Blob;
using uint256_t = Blob<256, uint64_t>;

class DataStream {
    bytearray_t buffer;
    size_t offset;

    public:
    DataStream(): offset(0) {}
    DataStream(const uint8_t *begin, const uint8_t *end): buffer(begin, end), offset(0) {}
    DataStream(bytearray_t &&data): buffer(std::move(data)), offset(0) {}
    DataStream(const bytearray_t &data): buffer(data), offset(0) {}

    DataStream(DataStream &&other):
            buffer(std::move(other.buffer)),
            offset(other.offset) {}

    DataStream(const DataStream &other):
        buffer(other.buffer),
        offset(other.offset) {}

    DataStream &operator=(const DataStream &other) {
        buffer = other.buffer;
        offset = other.offset;
        return *this;
    }

    DataStream &operator=(DataStream &&other) {
        buffer = std::move(other.buffer);
        offset = other.offset;
        return *this;
    }

    uint8_t *data() { return &buffer[offset]; }

    void clear() {
        buffer.clear();
        offset = 0;
    }

    size_t size() const {
        return buffer.size() - offset;
    }

    template<typename T>
    typename std::enable_if<std::is_integral<T>::value, DataStream &>::type
    operator<<(T d) {
        buffer.resize(buffer.size() + sizeof(T));
        *(reinterpret_cast<T *>(&*buffer.end() - sizeof(T))) = d;
        return *this;
    }

    template<typename T>
    typename std::enable_if<is_ranged<T>::value, DataStream &>::type
    operator<<(const T &d) {
        buffer.insert(buffer.end(), d.begin(), d.end());
        return *this;
    }

    void put_data(const uint8_t *begin, const uint8_t *end) {
        size_t len = end - begin;
        buffer.resize(buffer.size() + len);
        memmove(&*buffer.end() - len, begin, len);
    }

    const uint8_t *get_data_inplace(size_t len) {
        auto res = (uint8_t *)&*(buffer.begin() + offset);
#ifndef SALTICIDAE_NOCHECK
        if (offset + len > buffer.size())
            throw std::ios_base::failure("insufficient buffer");
#endif
        offset += len;
        return res;
    }

    template<typename T>
    typename std::enable_if<!is_ranged<T>::value &&
                            !std::is_integral<T>::value, DataStream &>::type
    operator<<(const T &obj) {
        obj.serialize(*this);
        return *this;
    }

    DataStream &operator<<(const char *cstr) {
        put_data((uint8_t *)cstr, (uint8_t *)cstr + strlen(cstr));
        return *this;
    }

    template<typename T>
    typename std::enable_if<std::is_integral<T>::value, DataStream &>::type
    operator>>(T &d) {
#ifndef SALTICIDAE_NOCHECK
        if (offset + sizeof(T) > buffer.size())
            throw std::ios_base::failure("insufficient buffer");
#endif
        d = *(reinterpret_cast<T *>(&buffer[offset]));
        offset += sizeof(T);
        return *this;
    }

    template<typename T>
    typename std::enable_if<!std::is_integral<T>::value, DataStream &>::type
    operator>>(T &obj) {
        obj.unserialize(*this);
        return *this;
    }

    std::string get_hex() const {
        char buf[3];
        DataStream s;
        for (auto it = buffer.begin() + offset; it != buffer.end(); it++)
        {
            sprintf(buf, "%02x", *it);
            s.put_data((uint8_t *)buf, (uint8_t *)buf + 2);
        }
        return std::string(s.buffer.begin(), s.buffer.end());
    }

    void load_hex(const std::string &hexstr) {
        size_t len = hexstr.size();
        const char *p;
        uint8_t *bp;
        unsigned int tmp;
        if (len & 1)
            throw std::invalid_argument("not a valid hex string");
        buffer.resize(len >> 1);
        offset = 0;
        for (p = hexstr.data(), bp = &*buffer.begin();
            p < hexstr.data() + len; p += 2, bp++)
        {
            if (sscanf(p, "%02x", &tmp) != 1)
                throw std::invalid_argument("not a valid hex string");
            *bp = tmp;
        }
    }

    operator bytearray_t () const & {
        return bytearray_t(buffer.begin() + offset, buffer.end());
    }

    operator bytearray_t () const && {
        return std::move(buffer);
    }

    operator std::string () const & {
        return std::string(buffer.begin() + offset, buffer.end());
    }

    inline uint256_t get_hash() const;
};

template<size_t N, typename T = uint64_t>
class Blob {
    using _impl_type = T;
    static const size_t bit_per_datum = sizeof(_impl_type) * 8;
    static_assert(!(N % bit_per_datum), "N must be divisible by bit_per_datum");
    static const auto _len = N / bit_per_datum;
    _impl_type data[_len];
    bool loaded;

    public:

    Blob(): loaded(false) {}
    Blob(const bytearray_t &arr) {
        if (arr.size() != N / 8)
            throw std::invalid_argument("incorrect Blob size");
        load(&*arr.begin());
    }

    Blob(const uint8_t *arr) { load(arr); }

    void load(const uint8_t *arr) {
        arr += N / 8;
        for (_impl_type *ptr = data + _len; ptr > data;)
        {
            _impl_type x = 0;
            for (unsigned j = 0; j < sizeof(_impl_type); j++)
                x = (x << 8) | *(--arr);
            *(--ptr) = x;
        }
        loaded = true;
    }

    bool is_null() const { return !loaded; }

    bool operator==(const Blob<N> &other) const {
        for (size_t i = 0; i < _len; i++)
            if (data[i] != other.data[i])
                return false;
        return true;
    }

    bool operator!=(const Blob<N> &other) const {
        return !(data == other);
    }

    size_t cheap_hash() const { return *data; }

    void serialize(DataStream &s) const {
        if (loaded)
        {
            for (const _impl_type *ptr = data; ptr < data + _len; ptr++)
                s << htole(*ptr);
        }
        else
        {
            for (const _impl_type *ptr = data; ptr < data + _len; ptr++)
                s << htole((_impl_type)0);
        }
    }

    void unserialize(DataStream &s) {
        for (_impl_type *ptr = data; ptr < data + _len; ptr++)
        {
            _impl_type x;
            s >> x;
            *ptr = letoh(x);
        }
        loaded = true;
    }
};

const size_t ENT_HASH_LENGTH = 256 / 8;

uint256_t DataStream::get_hash() const {
    class SHA256 d;
    d.update(buffer.begin() + offset, size());
    return d.digest();
}

template<typename T> inline uint256_t get_hash(const T &x) {
    DataStream s;
    s << x;
    return s.get_hash();
}

template<typename T> inline std::string get_hex(const T &x) {
    DataStream s;
    s << x;
    return s.get_hex();
}

}

namespace std {
    template <>
    struct hash<salticidae::uint256_t> {
        size_t operator()(const salticidae::uint256_t &k) const {
            return (size_t)k.cheap_hash();
        }
    };

    template <>
    struct hash<const salticidae::uint256_t> {
        size_t operator()(const salticidae::uint256_t &k) const {
            return (size_t)k.cheap_hash();
        }
    };
}

#endif
