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

#include <cstdarg>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <ctime>
#include <sys/time.h>
#include <cmath>
#include <event2/event.h>

#include "salticidae/util.h"

namespace salticidae {

void sec2tv(double t, struct timeval &tv) {
    tv.tv_sec = trunc(t);
    tv.tv_usec = trunc((t - tv.tv_sec) * 1e6);
}

void event_add_with_timeout(struct event *ev, double timeout) {
    struct timeval tv;
    tv.tv_sec = trunc(timeout);
    tv.tv_usec = trunc((timeout - tv.tv_sec) * 1e6);
    event_add(ev, &tv);
}

const std::string get_current_datetime() {
    /* credit: http://stackoverflow.com/a/41381479/544806 */
    char fmt[64], buf[64];
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm *tmp = localtime(&tv.tv_sec);
    strftime(fmt, sizeof fmt, "%Y-%m-%d %H:%M:%S.%%06u", tmp);
    snprintf(buf, sizeof buf, fmt, tv.tv_usec);
    return std::string(buf);
}

SalticidaeError::SalticidaeError() : msg("unknown") {}

SalticidaeError::SalticidaeError(const std::string &fmt, ...) {
    size_t guessed_size = 128;
    std::string buff;
    va_list ap;
    for (;;)
    {
        buff.resize(guessed_size);
        va_start(ap, fmt);
        int nwrote = vsnprintf((char *)buff.data(), guessed_size, fmt.c_str(), ap);
        if (nwrote < 0 || nwrote == guessed_size)
        {
            guessed_size <<= 1;
            continue;
        }
        buff.resize(nwrote);
        msg = std::move(buff);
        break;
    }
}

SalticidaeError::operator std::string() const {
    return msg;
}

void Logger::write(const char *tag, const char *fmt, va_list ap) {
    fprintf(output, "%s [%s] ", get_current_datetime().c_str(), tag);
    vfprintf(output, fmt, ap);
    fprintf(output, "\n");
}

void Logger::debug(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    write("debug", fmt, ap);
    va_end(ap);
}

void Logger::info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    write("salticidae info", fmt, ap);
    va_end(ap);
}

void Logger::warning(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    write("salticidae warn", fmt, ap);
    va_end(ap);
}
void Logger::error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    write("salticida error", fmt, ap);
    va_end(ap);
}

Logger logger;

void ElapsedTime::start() {
    struct timezone tz;
    gettimeofday(&t0, &tz);
    cpu_t0 = clock();
}

void ElapsedTime::stop(bool show_info) {
    struct timeval t1;
    struct timezone tz;
    gettimeofday(&t1, &tz);
    cpu_elapsed_sec = (float)clock() / CLOCKS_PER_SEC -
                        (float)cpu_t0 / CLOCKS_PER_SEC;
    elapsed_sec = (t1.tv_sec + t1.tv_usec * 1e-6) -
                    (t0.tv_sec + t0.tv_usec * 1e-6);
    if (show_info)
        SALTICIDAE_LOG_INFO("elapsed: %.3f (wall) %.3f (cpu)",
                    elapsed_sec, cpu_elapsed_sec);
}

Config::Opt::Opt(const std::string &optname, OptVal *optval, Action action, int idx): \
    optname(optname), optval(optval), action(action) {
    opt.name = this->optname.c_str();
    opt.has_arg = action == SWITCH_ON ? no_argument : required_argument;
    opt.flag = nullptr;
    opt.val = idx;
}

void Config::add_opt(const std::string &optname, OptVal *optval, Action action) {
    if (conf.count(optname))
        throw SalticidaeError("option name already exists");
    auto it = conf.insert(
        std::make_pair(optname,
                        Opt(optname, optval, action, getopt_order.size()))).first;
    getopt_order.push_back(&it->second);
}

std::string trim(const std::string &str,
                const std::string &space = "\t\r\n ") {
    const auto new_begin = str.find_first_not_of(space);
    if (new_begin == std::string::npos)
        return "";
    const auto new_end = str.find_last_not_of(space);
    return str.substr(new_begin, new_end - new_begin + 1);
}

void Config::update(Opt &p, const char *optval) {
    switch (p.action)
    {
        case SWITCH_ON: p.optval->switch_on(); break;
        case SET_VAL: p.optval->set_val(optval); break;
        case APPEND: p.optval->append(optval); break;
        default:
            throw SalticidaeError("unknown action");
    }
}

void Config::update(const std::string &optname, const char *optval) {
    assert(conf.count(optname));
    update(conf.find(optname)->second, optval);
}

bool Config::load(const std::string &fname) {
    static const size_t BUFF_SIZE = 1024;
    FILE *conf_f = fopen(fname.c_str(), "r");
    char buff[BUFF_SIZE];
    /* load configuration from file */
    if (conf_f)
    {
        while (fgets(buff, BUFF_SIZE, conf_f))
        {
            if (strlen(buff) == BUFF_SIZE - 1)
            {
                fclose(conf_f);
                throw SalticidaeError("configuration file line too long");
            }
            std::string line(buff);
            size_t pos = line.find("=");
            if (pos == std::string::npos)
                continue;
            std::string optname = trim(line.substr(0, pos));
            std::string optval = trim(line.substr(pos + 1));
            if (!conf.count(optname))
            {
                SALTICIDAE_LOG_WARN("ignoring option name in conf file: %s",
                            optname.c_str());
                continue;
            }
            update(optname, optval.c_str());
        }
        return true;
    }
    else
        return false;
}

size_t Config::parse(int argc, char **argv) {
    if (load(conf_fname))
        SALTICIDAE_LOG_INFO("loaded configuration from %s", conf_fname.c_str());

    size_t nopts = getopt_order.size();
    struct option *longopts = (struct option *)malloc(
                                sizeof(struct option) * (nopts + 1));
    int ind;
    for (size_t i = 0; i < nopts; i++)
        longopts[i] = getopt_order[i]->opt;
    longopts[nopts] = {0, 0, 0, 0};
    for (;;)
    {
        int id = getopt_long(argc, argv, "", longopts, &ind);
        if (id == -1 || id == '?') break;
        update(*getopt_order[id], optarg);
        if (id == conf_idx)
        {
            if (load(conf_fname))
                SALTICIDAE_LOG_INFO("load configuration from %s", conf_fname.c_str());
            else
                SALTICIDAE_LOG_INFO("configuration file %s not found", conf_fname.c_str());
        }
    }
    return optind;
}

}
