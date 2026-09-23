#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/zalloc.h>
#include <linux/epoll.h>

static struct event_poll_data *event_poll__find(struct event_poll *ep, int fd)
{
    if (fd < 0 || fd >= ep->fd_alloc)
        return NULL;
    return ep->by_fd[fd];
}

static int event_poll__grow(struct event_poll *ep, int fd)
{
    struct event_poll_data **n;
    int alloc = ep->fd_alloc ? ep->fd_alloc : 64;

    if (fd < ep->fd_alloc)
        return 0;
    while (alloc <= fd)
        alloc *= 2;
    n = realloc(ep->by_fd, (size_t)alloc * sizeof(*n));
    if (!n)
        return -ENOMEM;
    memset(n + ep->fd_alloc, 0, (size_t)(alloc - ep->fd_alloc) * sizeof(*n));
    ep->by_fd = n;
    ep->fd_alloc = alloc;
    return 0;
}

struct event_poll *event_poll__alloc(int maxevents)
{
    struct event_poll *ep;

    ep = malloc(sizeof(*ep));
    if (!ep)
        return NULL;

    memset(ep, 0, sizeof(*ep));

    ep->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ep->epfd < 0)
        goto err;
    ep->maxevents = maxevents;
    ep->events = zalloc(ep->maxevents * sizeof(*ep->events));
    if (ep->events == NULL)
        goto err;

    return ep;
err:
    event_poll__free(ep);
    return NULL;
}

void event_poll__free(struct event_poll *ep)
{
    int fd;

    for (fd = 0; fd < ep->fd_alloc; fd++) {
        if (!ep->by_fd[fd])
            continue;
        epoll_ctl(ep->epfd, EPOLL_CTL_DEL, fd, NULL);
        free(ep->by_fd[fd]);
        ep->by_fd[fd] = NULL;
    }
    free(ep->by_fd);
    if (ep->events)
        free(ep->events);
    if (ep->epfd >= 0)
        close(ep->epfd);
    free(ep);
}

int event_poll__add(struct event_poll *ep, int fd, unsigned int events, void *ptr, handle_event handle)
{
    struct event_poll_data *data;
    struct epoll_event event;
    int mod = 0;

    if (fd < 0)
        return -EINVAL;
    if (event_poll__grow(ep, fd) < 0)
        return -ENOMEM;

    data = ep->by_fd[fd];
    if (data) {
        mod = 1;
        data->dead = 0;
    } else {
        data = zalloc(sizeof(*data));
        if (!data)
            return -ENOMEM;
        data->fd = fd;
        ep->by_fd[fd] = data;
        fcntl(fd, F_SETFL, O_NONBLOCK | fcntl(fd, F_GETFL));
        ep->nr++;
    }

    data->ptr = ptr;
    data->events = events;
    data->handle = handle;

    event.events = events;
    event.data.ptr = data;
    return epoll_ctl(ep->epfd, mod ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &event);
}

int event_poll__del(struct event_poll *ep, int fd)
{
    struct event_poll_data *data;
    int i;

    data = event_poll__find(ep, fd);
    if (!data)
        return -ENOENT;

    for (i = 0; i < ep->cnt; i++) {
        if (ep->events[i].data.ptr == data)
            ep->events[i].data.ptr = NULL;
    }

    ep->by_fd[fd] = NULL;
    ep->nr--;
    epoll_ctl(ep->epfd, EPOLL_CTL_DEL, fd, NULL);
    free(data);
    return 0;
}

int event_poll__poll(struct event_poll *ep, int timeout)
{
    int i, cnt;
    unsigned int revents;
    struct event_poll_data *data;

    cnt = epoll_wait(ep->epfd, ep->events, ep->maxevents, timeout);
    if (cnt < 0)
        return -errno;

    ep->cnt = cnt;
    for (i = 0; i < cnt; i++) {
        revents = ep->events[i].events;
        data = ep->events[i].data.ptr;
        if (data && !data->dead) {
            ep->i = i;
            data->handle(data->fd, revents, data->ptr);
        }
    }
    ep->i = ep->cnt = 0;

    return ep->nr ? cnt : -ENOENT;
}
