#include <cstdio>
#include <thread>
#include <atomic>

#include "salticidae/event.h"

void test_mpsc(int nproducers = 16, int nops = 100000, size_t burst_size = 128) {
    size_t total = nproducers * nops;
    salticidae::EventContext ec;
    std::atomic<size_t> collected(0);
    using queue_t = salticidae::MPSCQueueEventDriven<int>;
    queue_t q;
    q.reg_handler(ec, [&collected, burst_size](queue_t &q) {
        size_t cnt = burst_size;
        int x;
        while (q.try_dequeue(x))
        {
            printf("%d\n", x);
            collected.fetch_add(1);
            if (!--cnt) return true;
        }
        return false;
    });
    std::vector<std::thread> producers;
    std::thread consumer([&collected, total, &ec]() {
        salticidae::Event timer(ec, -1, EV_TIMEOUT | EV_PERSIST,
            [&ec, &collected, total](int, short) {
            if (collected.load() == total) ec.stop();
        });
        timer.add_with_timeout(1);
        ec.dispatch();
    });
    for (int i = 0; i < nproducers; i++)
    {
        producers.emplace(producers.end(), std::thread([&q, nops, i, nproducers]() {
            int x = i;
            for (int j = 0; j < nops; j++)
            {
                //usleep(rand() / double(RAND_MAX) * 100);
                q.enqueue(x);
                x += nproducers;
            }
        }));
    }
    for (auto &t: producers) t.join();
    fprintf(stderr, "producers terminate\n");
    consumer.join();
    fprintf(stderr, "consumers terminate\n");
}

/*
void test_mpmc(int nproducers = 16, int nconsumers = 4, int nops = 100000) {
    size_t total = nproducers * nops;
    salticidae::MPMCQueueEventDriven<int> q;
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;
    std::vector<salticidae::EventContext> ecs;
    std::atomic<size_t> collected(0);
    ecs.resize(nconsumers);
    for (int i = 0; i < nconsumers; i++)
    {
        q.listen(ecs[i], [&collected](int x) {
            //usleep(10);
            printf("%d\n", x);
            collected.fetch_add(1);
        });
    }
    for (int i = 0; i < nconsumers; i++)
    {
        consumers.emplace(consumers.end(), std::thread(
                [&collected, total, &ec = ecs[i]]() {
            salticidae::Event timer(ec, -1, EV_TIMEOUT | EV_PERSIST,
                [&ec, &collected, total](int, short) {
                if (collected.load() == total) ec.stop();
            });
            timer.add_with_timeout(1);
            ec.dispatch();
        }));
    }
    for (int i = 0; i < nproducers; i++)
    {
        producers.emplace(producers.end(), std::thread([&q, nops, i, nproducers]() {
            int x = i;
            for (int j = 0; j < nops; j++)
            {
                //usleep(rand() / double(RAND_MAX) * 100);
                q.enqueue(x);
                x += nproducers;
            }
        }));
    }
    for (auto &t: producers) t.join();
    fprintf(stderr, "producers terminate\n");
    for (auto &t: consumers) t.join();
    fprintf(stderr, "consumers terminate\n");
}
*/

int main() {
    test_mpsc();
    //test_mpmc();
    return 0;
}
