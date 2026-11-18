// SPDX-License-Identifier: GPL-2.0
/*
 * Thread layouts for the task-state tests.
 *
 *   task-threads pingpong <seconds> [<spawn seconds>]
 *       Two threads switch to each other through pipes, about every 50us,
 *       while a short thread is created every 100ms, for the first <spawn
 *       seconds> only if given.
 *   task-threads gaps <threads> <seconds>
 *       Every other one of <threads> threads exits at once: the tids left
 *       have gaps. Prints READY when they have exited.
 *   task-threads exits <threads> <seconds>
 *       Prints READY with <threads> threads up, a second later they exit one
 *       by one, 200ms apart; the process stays for <seconds>.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int ping[2], pong[2];
static volatile int stop;

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *pinger(void *arg)
{
    char c = 'x';

    printf("WORKER %ld\n", syscall(SYS_gettid));
    while (!stop) {
        assert(write(ping[1], &c, 1) == 1);
        assert(read(pong[0], &c, 1) == 1);
        usleep(50);
    }
    c = 'q';
    assert(write(ping[1], &c, 1) == 1);
    return NULL;
}

static void *ponger(void *arg)
{
    char c;

    printf("WORKER %ld\n", syscall(SYS_gettid));
    while (read(ping[0], &c, 1) == 1 && c != 'q')
        assert(write(pong[1], &c, 1) == 1);
    return NULL;
}

static void *short_worker(void *arg)
{
    printf("WORKER %ld\n", syscall(SYS_gettid));
    usleep(10000);
    return NULL;
}

static int pingpong(double seconds, double spawn)
{
    double start = now(), end = start + seconds;
    pthread_t t1, t2, t;

    assert(pipe(ping) == 0 && pipe(pong) == 0);
    assert(pthread_create(&t2, NULL, ponger, NULL) == 0);
    assert(pthread_create(&t1, NULL, pinger, NULL) == 0);
    while (now() < end) {
        if (spawn < 0 || now() < start + spawn) {
            assert(pthread_create(&t, NULL, short_worker, NULL) == 0);
            assert(pthread_join(t, NULL) == 0);
        }
        usleep(100000);
    }
    stop = 1;
    assert(pthread_join(t1, NULL) == 0);
    assert(pthread_join(t2, NULL) == 0);
    return 0;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int released;

static void *gap_worker(void *arg)
{
    pthread_mutex_lock(&lock);
    while (!released)
        pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void *stay_worker(void *arg)
{
    while (!stop)
        usleep(10000);
    return NULL;
}

static int gaps(int nr, double seconds)
{
    pthread_t *threads = calloc(nr, sizeof(*threads));
    int i;

    assert(threads);
    for (i = 0; i < nr; i++)
        assert(pthread_create(&threads[i], NULL, i % 2 ? gap_worker : stay_worker, NULL) == 0);
    pthread_mutex_lock(&lock);
    released = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    for (i = 1; i < nr; i += 2)
        assert(pthread_join(threads[i], NULL) == 0);
    printf("READY %d\n", getpid());
    usleep(seconds * 1000000);
    stop = 1;
    for (i = 0; i < nr; i += 2)
        assert(pthread_join(threads[i], NULL) == 0);
    free(threads);
    return 0;
}

static volatile int nr_exit;

static void *exit_worker(void *arg)
{
    long i = (long)arg;

    while (nr_exit <= i)
        usleep(10000);
    return NULL;
}

static int exits(int nr, double seconds)
{
    pthread_t *threads = calloc(nr, sizeof(*threads));
    long i;

    assert(threads);
    for (i = 0; i < nr; i++)
        assert(pthread_create(&threads[i], NULL, exit_worker, (void *)i) == 0);
    printf("READY %d\n", getpid());
    sleep(1);
    for (i = 0; i < nr; i++) {
        nr_exit = i + 1;
        assert(pthread_join(threads[i], NULL) == 0);
        usleep(200000);
    }
    usleep(seconds * 1000000);
    free(threads);
    return 0;
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "pingpong")) {
        printf("WORKLOAD %d\n", getpid());
        return pingpong(atof(argv[2]), argc == 4 ? atof(argv[3]) : -1);
    }
    if (argc == 4 && !strcmp(argv[1], "gaps"))
        return gaps(atoi(argv[2]), atof(argv[3]));
    if (argc == 4 && !strcmp(argv[1], "exits"))
        return exits(atoi(argv[2]), atof(argv[3]));
    fprintf(stderr, "Usage: %s pingpong <seconds> [<spawn seconds>] | gaps <threads> <seconds> | "
                    "exits <threads> <seconds>\n", argv[0]);
    return 1;
}
