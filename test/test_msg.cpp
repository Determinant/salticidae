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

#include "salticidae/msg.h"
#include "salticidae/network.h"

using salticidae::uint256_t;
using salticidae::DataStream;
using salticidae::get_hash;
using salticidae::get_hex;
/*
struct MsgTest: public salticidae::MsgBase<> {
    using MsgBase::MsgBase;

    void gen_testhashes(int cnt) {
        DataStream s;
        set_opcode(0x01);
        s << (uint32_t)cnt;
        for (int i = 0; i < cnt; i++)
        {
            uint256_t hash = get_hash(i);
            printf("adding hash %s\n", get_hex(hash).c_str());
            s << hash;
        }
        set_payload(std::move(s));
    }

    void parse_testhashes() {
        DataStream s(get_payload());
        uint32_t cnt;
        s >> cnt;
        printf("got %d hashes\n", cnt);
        for (int i = 0; i < cnt; i++)
        {
            uint256_t hash;
            s >> hash;
            printf("got hash %s\n", get_hex(hash).c_str());
        }
    }
};
*/

int main() {
    /*
    MsgTest msg;
    msg.gen_ping(1234);
    printf("%s\n", std::string(msg).c_str());
    msg.gen_testhashes(5);
    printf("%s\n", std::string(msg).c_str());
    msg.parse_testhashes();
    try
    {
        msg.parse_testhashes();
    } catch (std::runtime_error &e) {
        printf("caught: %s\n", e.what());
    }
    */
    salticidae::PeerNetwork<> pn(salticidae::EventContext());
    return 0;
}
