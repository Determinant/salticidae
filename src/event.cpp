#include "salticidae/config.h"
#ifdef SALTICIDAE_CBINDINGS
#include "salticidae/event.h"

extern "C" {

eventcontext_t *eventcontext_new() { return new eventcontext_t(); }

void eventcontext_dispatch(eventcontext_t *self) { return self->dispatch(); }

void eventcontext_stop(eventcontext_t *self) { return self->stop(); }

void eventcontext_free(eventcontext_t *self) { delete self; }

sigev_t *sigev_new(const eventcontext_t *self, sigev_callback_t cb) {
    return new sigev_t(*self, cb);
}

void sigev_add(sigev_t *self, int sig) { self->add(sig); }

void sigev_delete(sigev_t *self) { delete self; }

}

#endif
