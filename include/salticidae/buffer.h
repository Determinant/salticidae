#ifndef _SALTICIDAE_BUFFER_H
#define _SALTICIDAE_BUFFER_H

#include <list>

namespace salticidae {

class SegBuffer {
    public:
    struct buffer_entry_t {
        bytearray_t data;
        bytearray_t::iterator offset;
        buffer_entry_t(): offset(data.begin()) {}
        buffer_entry_t(bytearray_t &&_data):
            data(std::move(_data)), offset(data.begin()) {}

        buffer_entry_t(buffer_entry_t &&other) {
            size_t _offset = other.offset - other.data.begin();
            data = std::move(other.data);
            offset = data.begin() + _offset;
        }

        buffer_entry_t &operator=(buffer_entry_t &&other) {
            size_t _offset = other.offset - other.data.begin();
            data = std::move(other.data);
            offset = data.begin() + _offset;
            return *this;
        }

        buffer_entry_t(const buffer_entry_t &other): data(other.data) {
            offset = data.begin() + (other.offset - other.data.begin());
        }

        size_t length() const { return data.end() - offset; }
    };

    private:
    std::list<buffer_entry_t> buffer;
    size_t _size;

    public:
    SegBuffer(): _size(0) {}
    ~SegBuffer() { clear(); }

    void swap(SegBuffer &other) {
        std::swap(buffer, other.buffer);
        std::swap(_size, other._size);
    }

    SegBuffer(const SegBuffer &other):
        buffer(other.buffer), _size(other._size) {}

    SegBuffer(SegBuffer &&other):
        buffer(std::move(other.buffer)), _size(other._size) {
        other._size = 0;
    }

    SegBuffer &operator=(SegBuffer &&other) {
        if (this != &other)
        {
            SegBuffer tmp(std::move(other));
            tmp.swap(*this);
        }
        return *this;
    }
 
    SegBuffer &operator=(const SegBuffer &other) {
        if (this != &other)
        {
            SegBuffer tmp(other);
            tmp.swap(*this);
        }
        return *this;
    }

    void rewind(bytearray_t &&data) {
        _size += data.size();
        buffer.push_front(buffer_entry_t(std::move(data)));
    }
  
    void push(bytearray_t &&data) {
        _size += data.size();
        buffer.push_back(buffer_entry_t(std::move(data)));
    }

    bytearray_t move_pop() {
        auto res = std::move(buffer.front().data);
        buffer.pop_front();
        _size -= res.size();
        return std::move(res);
    }
    
    bytearray_t pop(size_t len) {
        bytearray_t res;
        auto i = buffer.begin();
        while (len && i != buffer.end())
        {
            size_t copy_len = std::min(i->length(), len);
            res.insert(res.end(), i->offset, i->offset + copy_len);
            i->offset += copy_len;
            len -= copy_len;
            if (i->offset == i->data.end())
                i++;
        }
        buffer.erase(buffer.begin(), i);
        _size -= res.size();
        return std::move(res);
    }
    
    size_t size() const { return _size; }
    bool empty() const { return buffer.empty(); }
    
    void clear() {
        buffer.clear();
        _size = 0;
    }
};

struct MPSCWriteBuffer {
    using buffer_entry_t = SegBuffer::buffer_entry_t;
    using queue_t = MPSCQueueEventDriven<buffer_entry_t>;
    queue_t buffer;

    MPSCWriteBuffer() {}

    MPSCWriteBuffer(const SegBuffer &other) = delete;
    MPSCWriteBuffer(SegBuffer &&other) = delete;

    void rewind(bytearray_t &&data) {
        buffer.rewind(buffer_entry_t(std::move(data)));
    }
  
    void push(bytearray_t &&data) {
        buffer.enqueue(buffer_entry_t(std::move(data)));
    }

    bytearray_t move_pop() {
        buffer_entry_t res;
        buffer.try_dequeue(res);
        return std::move(res.data);
    }
    
    queue_t &get_queue() { return buffer; }
};

}

#endif
