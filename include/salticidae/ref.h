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

#ifndef _SALTICIDAE_REF_H
#define _SALTICIDAE_REF_H

#include <atomic>
#include <functional>

namespace salticidae {

struct _RCCtl {
    size_t ref_cnt;
    size_t weak_cnt;
    void add_ref() { ref_cnt++; }
    void add_weak() { weak_cnt++; }
    bool release_ref() {
        if (--ref_cnt) return false;
        if (weak_cnt) return true;
        delete this;
        return true;
    }
    void release_weak() {
        if (--weak_cnt) return;
        if (ref_cnt) return;
        delete this;
    }
    size_t get_cnt() { return ref_cnt; }
    _RCCtl(): ref_cnt(1), weak_cnt(0) {}
    ~_RCCtl() {}
};

struct _ARCCtl {
    std::atomic_size_t ref_cnt;
    std::atomic_size_t weak_cnt;
    std::atomic_uint8_t dcnt;
    void add_ref() { ref_cnt++; }
    void add_weak() { weak_cnt++; }
    bool release_ref() {
        dcnt++;
        if (--ref_cnt) { dcnt--; return false; }
        if (weak_cnt) { dcnt--; return true; }
        if (!--dcnt) delete this;
        return true;
    }
    void release_weak() {
        dcnt++;
        if (--weak_cnt) { dcnt--; return; }
        if (ref_cnt) { dcnt--; return; }
        if (!--dcnt) delete this;
    }
    size_t get_cnt() { return ref_cnt.load(); }
    _ARCCtl(): ref_cnt(1), weak_cnt(0), dcnt(0) {}
    ~_ARCCtl() {}
};

template<typename T, typename R> class RcObjBase;

template<typename T, typename R>
class WeakObjBase {
    R *ctl;
    void release() { if (ctl) ctl->release_weak(); }
    public:
    friend RcObjBase<T, R>;
    friend std::hash<WeakObjBase<T, R>>;
    WeakObjBase(): ctl(nullptr) {}
    WeakObjBase &operator=(const WeakObjBase &other) {
        release();
        ctl = other.ctl;
        ctl->add_weak();
        return *this;
    }

    WeakObjBase(const WeakObjBase &other): ctl(other.ctl) {
        ctl->add_weak();
    }

    WeakObjBase(WeakObjBase &&other): ctl(other.ctl) {
        other.ctl = nullptr;
    }

    WeakObjBase(const RcObjBase<T, R> &other);

    ~WeakObjBase() { release(); }
};

template<typename T, typename R>
class RcObjBase {
    T *obj;
    R *ctl;
    void release() {
        if (ctl && ctl->release_ref())
            delete obj;
    }
    public:
    friend WeakObjBase<T, R>;
    friend std::hash<RcObjBase<T, R>>;
    template<typename T__, typename T_, typename R_>
    friend RcObjBase<T__, R_> static_pointer_cast(const RcObjBase<T_, R_> &other);
    template<typename T_, typename R_> friend class RcObjBase;

    operator T*() const { return obj; }
    T *operator->() const { return obj; }
    RcObjBase(): obj(nullptr), ctl(nullptr) {}
    RcObjBase(T *obj): obj(obj), ctl(new R()) {}
    RcObjBase &operator=(const RcObjBase &other) {
        release();
        obj = other.obj;
        ctl = other.ctl;
        ctl->add_ref();
        return *this;
    }

    RcObjBase(const RcObjBase &other):
            obj(other.obj), ctl(other.ctl) {
        ctl->add_ref();
    }

    template<typename T_>
    RcObjBase(const RcObjBase<T_, R> &other):
            obj(other.obj), ctl(other.ctl) {
        ctl->add_ref();
    }

    RcObjBase(RcObjBase &&other):
            obj(other.obj), ctl(other.ctl) {
        other.ctl = nullptr;
    }

    RcObjBase(const WeakObjBase<T, R> &other) {
        if (other.ctl && other.ctl->ref_cnt)
        {
            obj = other.obj;
            ctl = other.ctl;
            ctl->add_ref();
        }
        else
        {
            obj = nullptr;
            ctl = nullptr;
        }
    }

    ~RcObjBase() { release(); }

    size_t get_cnt() const { return ctl ? ctl->get_cnt() : 0; }
};

template<typename T, typename T_, typename R>
RcObjBase<T, R> static_pointer_cast(const RcObjBase<T_, R> &other) {
    RcObjBase<T, R> rc{};
    rc.obj = static_cast<T *>(other.obj);
    rc.ctl = other.ctl;
    rc.ctl->add_ref();
    return std::move(rc);
}

template<typename T, typename R>
inline WeakObjBase<T, R>::WeakObjBase(const RcObjBase<T, R> &other):
        ctl(other.ctl) {
    ctl->add_weak();
}

template<typename T> using RcObj = RcObjBase<T, _RCCtl>;
template<typename T> using WeakObj = WeakObjBase<T, _RCCtl>;

template<typename T> using ArcObj = RcObjBase<T, _ARCCtl>;
template<typename T> using AweakObj = WeakObjBase<T, _ARCCtl>;

}

namespace std {
    template<typename T, typename R>
    struct hash<salticidae::RcObjBase<T, R>> {
        size_t operator()(const salticidae::RcObjBase<T, R> &k) const {
            return (size_t)k.obj;
        }
    };
}

#endif
