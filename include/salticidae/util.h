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

#ifndef _SALTICIDAE_COMMON_H
#define _SALTICIDAE_COMMON_H

#include <string>
#include <exception>
#include <cstdarg>
#include <vector>
#include <unordered_map>
#include <functional>
#include <getopt.h>
#include <event2/event.h>

namespace salticidae {

void sec2tv(double t, struct timeval &tv);
void event_add_with_timeout(struct event *ev, double timeout);

class SalticidaeError: public std::exception {
    std::string msg;
    public:
    SalticidaeError();
    SalticidaeError(const std::string &fmt, ...);
    operator std::string() const;
};

class Logger {
    protected:
    FILE *output;
    bool opened;
    void write(const char *tag, const char *fmt, va_list ap);

    public:
    Logger() : output(stderr), opened(false) {}
    Logger(FILE *f) : output(f) {}
    Logger(const char *filename): opened(true) {
        if ((output = fopen(filename, "w")) == nullptr)
            throw SalticidaeError("logger cannot open file");
    }

    ~Logger() {
        if (opened) fclose(output);
    }

    void debug(const char *fmt, ...);
    void info(const char *fmt, ...);
    void warning(const char *fmt, ...);
    void error(const char *fmt, ...);
};

extern Logger logger;

#ifdef SALTICIDAE_DEBUG_LOG
#define SALTICIDAE_NORMAL_LOG
#define SALTICIDAE_ENABLE_LOG_DEBUG
#endif

#ifdef SALTICIDAE_NORMAL_LOG
#define SALTICIDAE_ENABLE_LOG_INFO
#define SALTICIDAE_ENABLE_LOG_WARN
#endif

#ifdef SALTICIDAE_ENABLE_LOG_INFO
#define SALTICIDAE_LOG_INFO(...) salticidae::logger.info(__VA_ARGS__)
#else
#define SALTICIDAE_LOG_INFO(...) ((void)0)
#endif

#ifdef SALTICIDAE_ENABLE_LOG_DEBUG
#define SALTICIDAE_LOG_DEBUG(...) salticidae::logger.debug(__VA_ARGS__)
#else
#define SALTICIDAE_LOG_DEBUG(...) ((void)0)
#endif

#ifdef SALTICIDAE_ENABLE_LOG_WARN
#define SALTICIDAE_LOG_WARN(...) salticidae::logger.warning(__VA_ARGS__)
#else
#define SALTICIDAE_LOG_WARN(...) ((void)0)
#endif

#define SALTICIDAE_LOG_ERROR(...) salticidae::logger.error(__VA_ARGS__)

class ElapsedTime {
    struct timeval t0;
    clock_t cpu_t0;
    public:
    double elapsed_sec;
    double cpu_elapsed_sec;
    void start();
    void stop(bool show_info = false);
};

class Config {
    public:
    enum Action {
        SWITCH_ON,
        SET_VAL,
        APPEND
    };

    class OptVal {
        public:
        virtual void switch_on() {
            throw SalticidaeError("undefined OptVal behavior: set_val");
        }
        
        virtual void set_val(const std::string &) {
            throw SalticidaeError("undefined OptVal behavior: set_val");
        }
        
        virtual void append(const std::string &) {
            throw SalticidaeError("undefined OptVal behavior: append");
        }
        virtual ~OptVal() = default;
    };

    class OptValFlag: public OptVal {
        bool &val;
        public:
        OptValFlag(bool &val): val(val) {}
        void switch_on() override { val = true; }
    };

    class OptValStr: public OptVal {
        std::string &val;
        public:
        OptValStr(std::string &val): val(val) {}
        void set_val(const std::string &strval) override {
            val = strval;
        }
    };

    class OptValInt: public OptVal {
        int &val;
        public:
        OptValInt(int &val): val(val) {}
        void set_val(const std::string &strval) override {
            size_t idx;
            try {
                val = stoi(strval, &idx);
            } catch (std::invalid_argument) {
                throw SalticidaeError("invalid integer");
            }
        }
    };

    class OptValDouble: public OptVal {
        double &val;
        public:
        OptValDouble(double &val): val(val) {}
        void set_val(const std::string &strval) override {
            size_t idx;
            try {
                val = stod(strval, &idx);
            } catch (std::invalid_argument) {
                throw SalticidaeError("invalid double");
            }
        }
    };

    class OptValStrVec: public OptVal {
        std::vector<std::string> &val;
        public:
        OptValStrVec(std::vector<std::string> &val): val(val) {}
        void append(const std::string &strval) override {
            val.push_back(strval);
        }
    };

    private:
    struct Opt {
        std::string optname;
        OptVal *optval;
        Action action;
        struct option opt;
        Opt(const std::string &optname, OptVal *optval, Action action, int idx);
        Opt(Opt &&other):
                optname(std::move(other.optname)),
                optval(other.optval),
                action(other.action),
                opt(other.opt) { opt.name = this->optname.c_str(); }
    };

    std::unordered_map<std::string, Opt> conf;
    std::vector<Opt *> getopt_order;
    std::string conf_fname;
    OptValStr opt_val_conf;
    int conf_idx;
    void update(const std::string &optname, const char *optval);
    void update(Opt &opt, const char *optval);

    public:
    Config(const std::string &conf_fname):
            conf_fname(conf_fname),
            opt_val_conf(this->conf_fname) {
        conf_idx = getopt_order.size();
        add_opt("conf", &opt_val_conf, SET_VAL);
    }
    
    ~Config() {}

    void add_opt(const std::string &optname, OptVal *optval, Action action);
    bool load(const std::string &fname);
    size_t parse(int argc, char **argv);
};

class Event {
    public:
    using callback_t = std::function<void(evutil_socket_t fd, short events)>;

    private:
    struct event_base *eb;
    evutil_socket_t fd;
    short events;
    struct event *ev;
    callback_t callback;
    static inline void _then(evutil_socket_t fd, short events, void *arg) {
        (static_cast<Event *>(arg))->callback(fd, events);
    }

    public:
    Event(): ev(nullptr) {}
    Event(struct event_base *eb,
        evutil_socket_t fd,
        short events,
        callback_t callback):
            eb(eb), fd(fd), events(events),
            ev(event_new(eb, fd, events, Event::_then, this)),
            callback(callback) {}
    Event(Event &&other):
            eb(other.eb), fd(other.fd), events(other.events),
            callback(std::move(other.callback)) {
        other.clear();
        ev = event_new(eb, fd, events, Event::_then, this);
    }
    Event &operator=(Event &&other) {
        clear();
        other.clear();
        eb = other.eb;
        fd = other.fd;
        events = other.events;
        ev = event_new(eb, fd, events, Event::_then, this);
        callback = std::move(other.callback);
        return *this;
    }

    ~Event() { clear(); }

    void clear() {
        if (ev != nullptr)
        {
            event_del(ev);
            event_free(ev);
            ev = nullptr;
        }
    }

    void add() { event_add(ev, nullptr); }
    void del() { event_del(ev); }
    void add_with_timeout(double timeout) {
        event_add_with_timeout(ev, timeout);
    }

    operator bool() const { return ev != nullptr; }
};

}

#endif
