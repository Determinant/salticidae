#include "salticidae/config.h"
#ifdef SALTICIDAE_CBINDINGS
#include "salticidae/event.h"

extern "C" {

eventcontext_t *eventcontext_new() { return new eventcontext_t(); }

void eventcontext_free(eventcontext_t *self) { delete self; }

void eventcontext_dispatch(eventcontext_t *self) { return self->dispatch(); }

void eventcontext_stop(eventcontext_t *self) { return self->stop(); }

sigev_t *sigev_new(const eventcontext_t *self, sigev_callback_t callback) {
    return new sigev_t(*self, callback);
}

threadcall_t *threadcall_new(const eventcontext_t *ec) { return new threadcall_t(*ec); }

void threadcall_free(threadcall_t *self) { delete self; }

void threadcall_async_call(threadcall_t *self, threadcall_callback_t callback, void *userdata) {
    self->async_call([=](salticidae::ThreadCall::Handle &h) {
        callback(&h, userdata);
    });
}

eventcontext_t *threadcall_get_ec(threadcall_t *self) {
    return new eventcontext_t(self->get_ec());
}

void sigev_free(sigev_t *self) { delete self; }

void sigev_add(sigev_t *self, int sig) { self->add(sig); }

timerev_t *timerev_new(const eventcontext_t *ec, timerev_callback_t callback) {
    return new timerev_t(*ec, [callback](salticidae::TimerEvent &ev) {
        callback(&ev);
    });
}

void timerev_set_callback(timerev_t *self, timerev_callback_t callback) {
    self->set_callback([callback](salticidae::TimerEvent &ev) {
        callback(&ev);
    });
}

void timerev_free(timerev_t *self) { delete self; }

void timerev_add(timerev_t *self, double t_sec) { self->add(t_sec); }

void timerev_del(timerev_t *self) { self->del(); }

void timerev_clear(timerev_t *self) { self->clear(); }

}

#endif
