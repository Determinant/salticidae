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

#ifndef _SALTICIDAE_TYPE_H
#define _SALTICIDAE_TYPE_H

#include <vector>
#include <string>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ios>
#include <functional>
#include <mutex>

namespace salticidae {

const auto _1 = std::placeholders::_1;
const auto _2 = std::placeholders::_2;

using bytearray_t = std::vector<uint8_t>;
using mutex_lg_t = std::lock_guard<std::mutex>;
using mutex_ul_t = std::unique_lock<std::mutex>;

template<typename T> T htole(T) = delete;
template<> inline uint16_t htole<uint16_t>(uint16_t x) { return htole16(x); }
template<> inline uint32_t htole<uint32_t>(uint32_t x) { return htole32(x); }
template<> inline uint64_t htole<uint64_t>(uint64_t x) { return htole64(x); }

template<typename T> T letoh(T) = delete;
template<> inline uint16_t letoh<uint16_t>(uint16_t x) { return le16toh(x); }
template<> inline uint32_t letoh<uint32_t>(uint32_t x) { return le32toh(x); }
template<> inline uint64_t letoh<uint64_t>(uint64_t x) { return le64toh(x); }

template <typename T, typename = void>
struct is_ranged : std::false_type {};

template <typename T>
struct is_ranged<T,
    std::void_t<decltype(std::declval<T>().begin()),
                decltype(std::declval<T>().end())>> : std::true_type {};

}

#endif