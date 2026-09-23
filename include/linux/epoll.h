#ifndef __LINUX_EPOLL__
#define __LINUX_EPOLL__

#include <linux/list.h>
#include <linux/rbtree.h>
#include <sys/epoll.h>

typedef void (*handle_event)(int fd, unsigned int revents, void *ptr);

struct event_poll_data {
    int fd;
    unsigned int dead;
    void *ptr;
    unsigned int events;
    handle_event handle;
};

struct event_poll {
    int epfd;
    int i, cnt;
    int maxevents;
    struct epoll_event *events;
    int nr;
    int fd_alloc;
    struct event_poll_data **by_fd;
};

struct event_poll *event_poll__alloc(int maxevents);
void event_poll__free(struct event_poll *ep);
int event_poll__add(struct event_poll *ep, int fd, unsigned int events, void *ptr, handle_event handle);
int event_poll__del(struct event_poll *ep, int fd);
int event_poll__poll(struct event_poll *ep, int timeout);

#endif
