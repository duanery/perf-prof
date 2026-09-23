// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static void *worker(void *arg)
{
    int i;
    pid_t tid = syscall(SYS_gettid);

    printf("WORKER %d\n", tid);
    for (i = 0; i < 4; i++) {
        prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
        usleep(1000);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 24;
    int i;

    setbuf(stdout, NULL);
    printf("WORKLOAD %d\n", getpid());
    for (i = 0; i < rounds; i++) {
        pthread_t thread;
        assert(pthread_create(&thread, NULL, worker, NULL) == 0);
        assert(pthread_join(thread, NULL) == 0);
    }
    return 0;
}
