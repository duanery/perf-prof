#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#define __USE_GNU
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <linux/kernel.h>
#include <linux/circ_buf.h>
#include <linux/list.h>
#include <linux/thread_map.h>
#include <monitor.h>
#include <tep.h>
#include <net.h>

static struct prof_dev *perf_clock_dev = NULL;
static profiler perf_clock;

static int perf_clock_ref(void)
{
    if (!perf_clock_dev) {
        struct env *env = zalloc(sizeof(*env));
        if (!env) return -1;
        perf_clock_dev = prof_dev_open(&perf_clock, env);
        if (!perf_clock_dev) return -1;
    }
    prof_dev_use(perf_clock_dev);
    return 0;
}

static void perf_clock_unref(void)
{
    if (!perf_clock_dev) return;
    if (prof_dev_unuse(perf_clock_dev))
        perf_clock_dev = NULL;
}


#define BUF_LEN (1 << 16)

enum block_type {
    TYPE_TCP,
    TYPE_CDEV,
    TYPE_FILE,
};

struct cdev_block { // chardev
    int fd;
    bool connected;
    const char *filename;

    // EPOLLOUT
    struct perf_record_lost lost_event;
    // struct circ_buf circ;
    char *buf; // BUF_LEN
    int head;  // write event
    int tail;  // read and write to cdev

    // EPOLLIN
    char *event_copy;
    int read;
};

struct event_block {
    struct list_head link;
    struct event_block_list *eb_list;
    const char *block_def;
    enum block_type type;
    union {
        struct tcp_block {
            void *tcp; // broadcast or receive
            struct tcp_socket_ops ops;
            const char *ip;
            const char *port;
        } tcp;
        struct cdev_block cdev;
        struct file_block {
            FILE *file; // broadcast or receive
            size_t pos;
            const char *filename;
            int notifyfd;
        } file;
    } u;
    // order
    char *event_buf, *event;
    int size;
    // receive
    int remote_id;
    int pid_pos;
    int cpu_pos;
    int id_pos;
    int stream_id_pos;
    int common_type_pos;
};

struct event_block_list {
    struct list_head link_to; // broadcast_block_list
    struct list_head block_list;
    struct tp *tp;
    u64 last_event_time;
    int time_pos;
    bool broadcast;
    bool freeing;
    bool ins_oncpu;
};

struct list_head broadcast_block_list = LIST_HEAD_INIT(broadcast_block_list);

static inline void block_free(struct event_block *block);
static inline void block_broadcast(struct event_block *block, const void *buf, size_t len, int flags);
static void handle_cdev_event(int fd, unsigned int revents, void *ptr);


static int block_event_convert(struct event_block *block, union perf_event *event)
{
    struct tp *tp = block->eb_list->tp;
    struct perf_event_attr *attr;
    void *data;
    u64 sample_type;
    int cpuidx = 0;
    int threadidx = 0;
    int pos = 0;
    int common_type_pos = 0;
    int cpu = -1;
    int tid = -1;
    bool oncpu = block->eb_list->ins_oncpu;

    if (unlikely(!tp->evsel))
        return -1;

    attr = perf_evsel__attr(tp->evsel);
    if (unlikely(attr->sample_period == 0))
        return -1;

   /*
    *  { u64           id;   } && PERF_SAMPLE_IDENTIFIER
    *  { u64           ip;   } && PERF_SAMPLE_IP
    *  { u32           pid, tid; } && PERF_SAMPLE_TID
    *  { u64           time;     } && PERF_SAMPLE_TIME
    *  { u64           addr;     } && PERF_SAMPLE_ADDR
    *  { u64           id;   } && PERF_SAMPLE_ID
    *  { u64           stream_id;} && PERF_SAMPLE_STREAM_ID
    *  { u32           cpu, res; } && PERF_SAMPLE_CPU
    *  { u64           period;   } && PERF_SAMPLE_PERIOD
    *  { struct read_format    values;   } && PERF_SAMPLE_READ
    *  { u64           nr,
    *    u64           ips[nr];  } && PERF_SAMPLE_CALLCHAIN
    *  { u32			size;
    *    char                  data[size];}&& PERF_SAMPLE_RAW
    */
    data = (void *)event->sample.array;
    sample_type = attr->sample_type;

    if (block->pid_pos == -1 && block->cpu_pos == -1 && block->id_pos == -1 && block->stream_id_pos == -1 && block->common_type_pos == -1) {
        if (sample_type & PERF_SAMPLE_IDENTIFIER)
            pos += sizeof(u64);
        if (sample_type & PERF_SAMPLE_IP)
            pos += sizeof(u64);
        if (sample_type & PERF_SAMPLE_TID) {
            block->pid_pos = pos;
            pos += sizeof(u32) + sizeof(u32);
        }
        if (sample_type & PERF_SAMPLE_TIME)
            pos += sizeof(u64);
        if (sample_type & PERF_SAMPLE_ADDR)
            pos += sizeof(u64);
        if (sample_type & PERF_SAMPLE_ID) {
            block->id_pos = pos;
            pos += sizeof(u64);
        }
        if (sample_type & PERF_SAMPLE_STREAM_ID) {
            block->stream_id_pos = pos;
            pos += sizeof(u64);
        }
        if (sample_type & PERF_SAMPLE_CPU) {
            block->cpu_pos = pos;
            pos += sizeof(u32) + sizeof(u32);
        }
        if (sample_type & PERF_SAMPLE_PERIOD)
            pos += sizeof(u64);
        if (sample_type & PERF_SAMPLE_READ)
            pos += perf_evsel__read_size(tp->evsel);
        block->common_type_pos = pos;
    }

    if (block->cpu_pos != -1) {
        cpu = *(u32 *)(data + block->cpu_pos);

        if (!tp->vcpu) {
            if (oncpu)
                cpuidx = perf_cpu_map__idx(tp->dev->cpus, cpu);
        } else {
            if (oncpu)
                cpuidx = perf_cpu_map__idx(tp->dev->cpus, tp->vcpu->vcpu[cpu].host_cpu);
            else
                threadidx = perf_thread_map__idx(tp->dev->threads, tp->vcpu->vcpu[cpu].thread_id);

            // Guest vcpu => Host cpu
            if (perf_cpu_map__nr(tp->vcpu->host_cpus[cpu]) > 1)
                *(u32 *)(data + block->cpu_pos) = -1;
            else
                *(u32 *)(data + block->cpu_pos) = tp->vcpu->vcpu[cpu].host_cpu;

            // Guest vcpu => Host tid
            if (block->pid_pos != -1) {
                // u32           pid, tid;
                *(u32 *)(data + block->pid_pos) = tp->vcpu->vcpu[cpu].thread_id;
                *(u32 *)(data + block->pid_pos + sizeof(u32)) = tp->vcpu->vcpu[cpu].thread_id;
            }
        }
    }

    if (block->pid_pos != -1) {
        tid = *(u32 *)(data + block->pid_pos + sizeof(u32));
        if (!tp->vcpu) {
            if (!oncpu)
                threadidx = perf_thread_map__idx(tp->dev->threads, tid);
        }
    }

    if (sample_type & PERF_SAMPLE_ID) {
        //u64           id;
        *(u64 *)(data + block->id_pos) = perf_evsel__get_id(tp->evsel, cpuidx < 0 ? 0 : cpuidx,
                                         threadidx < 0 ? 0 : threadidx);
    }

    if (sample_type & PERF_SAMPLE_STREAM_ID) {
        //u64           stream_id;
        *(u64 *)(data + block->stream_id_pos) = perf_evsel__get_id(tp->evsel, cpuidx < 0 ? 0 : cpuidx,
                                                threadidx < 0 ? 0 : threadidx);
    }

    if (tp->id != block->remote_id) {
        common_type_pos = block->common_type_pos;
        if (sample_type & PERF_SAMPLE_CALLCHAIN) {
            struct {
                u64 nr;
                u64 ips[];
            } *callchain = data + common_type_pos;
            common_type_pos += (callchain->nr + 1) * sizeof(u64);
        }
        if (sample_type & PERF_SAMPLE_RAW) {
            common_type_pos += sizeof(u32);
            //unsigned short common_type;
            //unsigned char common_flags;
            //unsigned char common_preempt_count;
            //int common_pid;
            *(unsigned short *)(data + common_type_pos) = tp->id;
            if (tp->vcpu && cpu >= 0)
                *(int *)(data + common_type_pos + sizeof(u16) + sizeof(u8) + sizeof(u8)) = tp->vcpu->vcpu[cpu].thread_id;
        }
    }

    if (oncpu ? cpuidx < 0 : threadidx < 0) {
        static int once = 0;
        if (once == 0) {
            once = 1;
            printf("The partial events pulled by %s:%s//pull=\"%s\"/ cannot be switched to the cpus/threads of '%s'.\n",
                    tp->sys, tp->name, block->block_def, tp->dev->prof->name);
        }
    }

    return oncpu ? cpuidx : threadidx;
}

static int block_process_event(struct event_block *block, union perf_event *event)
{
    struct tp *tp = block->eb_list->tp;
    int cpu, tid;
    int ins = 0;

    switch (event->header.type) {
        case PERF_RECORD_TP: {
                struct perf_record_tp *record = (void *)event;
                struct perf_event_attr *attr = tp->evsel ? perf_evsel__attr(tp->evsel) : NULL;

                if (strcmp((char *)record + record->sys_offset, tp->sys) ||
                    strcmp((char *)record + record->name_offset, tp->name)) {
                    fprintf(stderr, "tp sys:name does not match %s:%s, unable to receive pull-events.\n",
                                    (char *)record + record->sys_offset, (char *)record + record->name_offset);
                    goto failed;
                }
                if (!attr || attr->sample_period == 0 || record->sample_period == 0) {
                    fprintf(stderr, "tp is non-sampling, unable to receive pull-events.\n");
                    goto failed;
                }
                if (attr->sample_type != record->sample_type) {
                    fprintf(stderr, "tp sample_type(%llu) mismatch, unable to receive pull-events.\n",
                                    attr->sample_type ^ record->sample_type);
                    goto failed;
                }
                if (record->sample_type & PERF_SAMPLE_CALLCHAIN) {
                    fprintf(stderr, "tp has PERF_SAMPLE_CALLCHAIN enabled, unable to receive pull-events.\n");
                    goto failed;
                }
                if (tep__event_size(tp->id) != record->event_size) {
                    fprintf(stderr, "tp event_size mismatch, unable to receive pull-events.\n");
                    goto failed;
                }

                block->remote_id = record->id;
                return 0;

            failed:
                block_free(block);
                return -1;
            }
        case PERF_RECORD_SAMPLE:
            /*
             * -e sched:sched_switch//pull=9900/vm=$uuid/ --kvmclock $uuid
             * --kvmclock, when waiting for pvclock update, do not process the pulled events in advance.
             */
            if (!prof_dev_enabled(tp->dev))
                return 0;
            ins = block_event_convert(block, event);
            if (ins < 0) return 0;
            else break;
        default:
            if (!prof_dev_enabled(tp->dev))
                return 0;
            break;
    }

    prof_dev_ins_pair(tp->dev, ins, &cpu, &tid);
    perf_event_process_record(tp->dev, event, cpu, tid, true, true);
    return 0;
}

static __always_inline char *event_buf_alloc(void)
{
    return malloc(BUF_LEN);
}

static union perf_event *block_read_event(void *stream, bool init,
                int (*read_init)(struct event_block *block, void *buf, size_t len),
                int *ins, bool *writable, bool *converted)
{
    struct event_block *block = stream;
    union perf_event *event = (void *)block->event;
    int ret;

tcp_retry:
    // consume
    if (block->size > sizeof(struct perf_event_header) &&
        block->size >= event->header.size) {
        block->size -= event->header.size;
        block->event += event->header.size;
        event = (void *)block->event;
    }

    if (init && (block->size <= sizeof(struct perf_event_header) ||
                 block->size < event->header.size)) {
        if (block->size && block->event_buf != block->event)
            memcpy(block->event_buf, block->event, block->size);

        ret = read_init(block, block->event_buf+block->size, BUF_LEN-block->size);
        if (ret < 0)
            return NULL;
        block->size += ret;
        block->event = block->event_buf;
        event = (void *)block->event;
    }

    // read event
    if (block->size > sizeof(struct perf_event_header) &&
        block->size >= event->header.size) {
        if (unlikely(event->header.type == PERF_RECORD_TP)) {
            if (block_process_event(block, event) < 0)
                return NULL;
            goto tcp_retry;
        }

        /*
         * -e sched:sched_switch//pull=9900/vm=$uuid/ --kvmclock $uuid
         * --kvmclock, when waiting for pvclock update, do not process the pulled events in advance.
         */
        if (!prof_dev_enabled(block->eb_list->tp->dev))
            goto tcp_retry;

        // converted
        if (event->header.type == PERF_RECORD_SAMPLE) {
            *ins = block_event_convert(block, event);
            if (*ins < 0)
                goto tcp_retry;
        } else
            *ins = 0;

        *writable = 1;
        *converted = 1;
        return event;
    }

    return NULL;
}

static int tcp_read_init(struct event_block *block, void *buf, size_t len)
{
    return tcp_recv(block->u.tcp.tcp, buf, len, 0);
}

static union perf_event *tcp_read_event(void *stream, bool init, int *ins, bool *writable, bool *converted)
{
    return block_read_event(stream, init, tcp_read_init, ins, writable, converted);
}

static void tcp_notify(struct tcp_socket_ops *ops)
{
    /* Compatible with accept client and connect client. */
    struct event_block *block = container_of(ops->server_ops ?: ops, struct event_block, u.tcp.ops);
    struct prof_dev *dev = block->eb_list->tp->dev;
    order_stream(dev);
}

static int tcp_process_event(char *event_buf, int size, struct tcp_socket_ops *ops)
{
    /* Compatible with accept client and connect client. */
    struct event_block *block = container_of(ops->server_ops ?: ops, struct event_block, u.tcp.ops);
    struct prof_dev *dev = block->eb_list->tp->dev;
    union perf_event *event;
    int total = size;

    prof_dev_get(dev);
    event = (void *)event_buf;
    while (size > sizeof(struct perf_event_header) &&
        size >= event->header.size) {
        size -= event->header.size;
        if (block_process_event(block, event) < 0) {
            prof_dev_put(dev);
            return total;
        }
        event = (void *)event + event->header.size;
    }
    prof_dev_put(dev);
    return total - size;
}

static int tcp_disconnect(struct tcp_socket_ops *ops)
{
    struct event_block *block = container_of(ops, struct event_block, u.tcp.ops);
    block_free(block);
    return 0;
}

static int perf_record_tp_init(struct tp *tp, struct perf_record_tp *record)
{
    struct perf_event_attr *attr;

    if (!tp->evsel)
        return -1;

    attr = perf_evsel__attr(tp->evsel);

    // send sys:name, perf_event_attr, /FILTER/ATTR/

    record->header.size = sizeof(*record) + strlen(tp->sys) + strlen(tp->name) + 2;
    record->header.type = PERF_RECORD_TP;
    record->header.misc = 0;

    record->id = tp->id;
    record->sys_offset = sizeof(*record);
    record->name_offset = sizeof(*record) + strlen(tp->sys) + 1;
    record->sample_period = attr->sample_period;
    record->sample_type = attr->sample_type;
    record->event_size = tep__event_size(tp->id);
    record->unused = 0;

    return 0;
}

static int tcp_new_client(struct tcp_socket_ops *ops)
{
    struct event_block *block = NULL;
    struct tp *tp = NULL;
    struct perf_record_tp record;
    int ret = -1;

    if (!ops->server_ops)
        goto err;

    block = container_of(ops->server_ops, struct event_block, u.tcp.ops);
    tp = block->eb_list->tp;
    if (perf_record_tp_init(tp, &record) < 0)
        goto err;

    if (tcp_send(ops->client, &record, sizeof(record), MSG_MORE) == 0 &&
        tcp_send(ops->client, tp->sys, strlen(tp->sys)+1, MSG_MORE) == 0 &&
        tcp_send(ops->client, tp->name, strlen(tp->name)+1, 0) == 0)
        ret = 0;
    else
        goto err;

    prof_dev_flush(tp->dev, PROF_DEV_FLUSH_NORMAL);
err:
    return ret;
}

static int cdev_read_init(struct event_block *block, void *buf, size_t len)
{
    struct cdev_block *cdev = &block->u.cdev;
    int ret = read(cdev->fd, buf, len);
    if (unlikely(ret <= 0)) {
        // The return value will be 0 when the host is disconnected.
        if (ret == 0)
            return 0;
        if (errno == EAGAIN)
            return 0;
        else
            fprintf(stderr, "Unable read from %s: %s\n", cdev->filename, strerror(errno));
    }
    return ret;
}

static union perf_event *cdev_read_event(void *stream, bool init, int *ins, bool *writable, bool *converted)
{
    return block_read_event(stream, init, cdev_read_init, ins, writable, converted);
}

static int cdev_write_header(struct event_block *block)
{
    struct tp *tp = block->eb_list->tp;
    struct perf_record_tp record;

    if (perf_record_tp_init(tp, &record) < 0)
        return -1;

    block_broadcast(block, &record, sizeof(record), 0);
    block_broadcast(block, tp->sys, strlen(tp->sys)+1, 0);
    block_broadcast(block, tp->name, strlen(tp->name)+1, 0);

    prof_dev_flush(tp->dev, PROF_DEV_FLUSH_NORMAL);
    return 0;
}

static void handle_cdev_event(int fd, unsigned int revents, void *ptr)
{
    struct event_block *block = ptr;
    struct cdev_block *cdev = &block->u.cdev;
    struct event_block_list *eb_list = block->eb_list;

    // Handle EPOLLIN first to fully read out the received events, then EPOLLHUP.
    if (revents & EPOLLIN) {
        struct prof_dev *dev = eb_list->tp->dev;
        union perf_event *event;
        int ret;

        if (!eb_list->broadcast && using_order(dev))
            order_stream(dev);
        else
        while (true) {
            ret = read(cdev->fd, cdev->event_copy+cdev->read, BUF_LEN-cdev->read);
            if (unlikely(ret <= 0)) {
                // The return value will be 0 when the host is disconnected.
                if (ret == 0)
                    break;
                if (errno != EAGAIN)
                    fprintf(stderr, "Unable read from %s: %s\n", cdev->filename, strerror(errno));
                break;
            }

            cdev->connected = 1;
            cdev->read += ret;
            event = (void *)cdev->event_copy;
            while (cdev->read >= sizeof(struct perf_event_header) &&
                cdev->read >= event->header.size) {
                cdev->read -= event->header.size;
                if (unlikely(block_process_event(block, event) < 0))
                    return;
                event = (void *)event + event->header.size;
            }
            if (cdev->read) {
                memcpy(cdev->event_copy, event, cdev->read);
            }
        }
    }

    if (revents & EPOLLHUP) {
        // EPOLLET: Edge Triggered.
        // Avoid EPOLLHUP loops.
        // Wait until the host of virtio-ports is connected and return EPOLLOUT.
        unsigned int events = (eb_list->broadcast ? EPOLLOUT : EPOLLIN) | EPOLLET | EPOLLHUP;

        // reset circ_buf
        cdev->head = 0;
        cdev->tail = 0;
        memset(&cdev->lost_event, 0, sizeof(struct perf_record_lost));
        cdev->read = 0;

        // reopen.
        // There may be some dirty data in cdev, which can be refreshed by reopening.
        if (cdev->connected == 1) {
            cdev->connected = 0;
            printf("Cdev %s has hang up\n", cdev->filename);

            main_epoll_del(cdev->fd);
            close(cdev->fd);
            cdev->fd = open(cdev->filename, (eb_list->broadcast ? O_WRONLY : O_RDONLY) | O_NONBLOCK);
            if (cdev->fd < 0 ||
                main_epoll_add(cdev->fd, events, block, handle_cdev_event) < 0)
                block_free(block);
        }
        return ;
    }

    if (revents & EPOLLOUT) {
        int cnt;
        ssize_t wr;

        if (unlikely(cdev->connected == 0)) {
            cdev->connected = 1;
            printf("Cdev %s is connected\n", cdev->filename);
            cdev_write_header(block);
        }
    retry:
        cnt = CIRC_CNT_TO_END(cdev->head, cdev->tail, BUF_LEN);
        if (cnt == 0) {
            main_epoll_add(fd, EPOLLHUP, block, handle_cdev_event);
            return ;
        }
        wr = write(cdev->fd, cdev->buf + (cdev->tail & (BUF_LEN-1)), cnt);
        if (wr <= 0) {
            if (wr < 0 && errno != EAGAIN)
                fprintf(stderr, "Unable write to %s: %s\n", cdev->filename, strerror(errno));
            return ;
        }
        cdev->tail = (cdev->tail + wr) & (BUF_LEN-1);
        if (cdev->lost_event.lost > 0 && wr >= sizeof(struct perf_record_lost)) {
            block_broadcast(block, &cdev->lost_event, sizeof(struct perf_record_lost), 0);
            cdev->lost_event.lost = 0;
        }
        goto retry;
    }
}

static int file_write_header(struct event_block *block)
{
    struct tp *tp = block->eb_list->tp;
    struct perf_record_tp record;
    FILE *file = block->u.file.file;
    size_t pos = 0;

    if (perf_record_tp_init(tp, &record) < 0)
        return -1;

    pos += fwrite(&record, 1, sizeof(record), file);
    pos += fwrite(tp->sys, 1, strlen(tp->sys)+1, file);
    pos += fwrite(tp->name, 1, strlen(tp->name)+1, file);
    block->u.file.pos = pos;
    fflush(file);
    return 0;
}

static void handle_file_event(int fd, unsigned int revents, void *ptr)
{
    struct event_block *block = ptr;
    FILE *file = block->u.file.file;
    char event_copy[PERF_SAMPLE_MAX_SIZE];
    union perf_event *event = (void *)event_copy;
    int len, i;

    for (i = 0; i < 64; i++) {
        len = fread(event_copy, 1, sizeof(struct perf_event_header), file);
        if (len == sizeof(struct perf_event_header))
            len += fread(event_copy+len, 1, event->header.size-len, file);
        if (len != event->header.size)
            goto err;
        if (block_process_event(block, event) < 0)
            break;
    }
    return ;

err:
    block_free(block);
    return;
}

static int block_new(struct event_block_list *eb_list, char *value)
{
    struct prof_dev *dev = eb_list->tp->dev;
    struct event_block *block = NULL;
    char *ip = NULL;
    char *port;
    FILE *file = NULL;
    struct stat st;

    port = strchr(value, ':');
    if (port) {
        *port ++ = '\0';
        if (*value) ip = value;
    } else
        port = value;
    if (!*port) goto err_return;

    if (!(block = malloc(sizeof(*block)))) return -1;
    memset(block, 0, sizeof(*block));

    block->eb_list = eb_list;
    list_add_tail(&block->link, &eb_list->block_list);
    block->block_def = value;

    block->pid_pos = -1;
    block->cpu_pos = -1;
    block->id_pos = -1;
    block->stream_id_pos = -1;
    block->common_type_pos = -1;

    /*
     * Detect whether it is a tcp server or client.
     */
    block->u.tcp.ip = ip;
    block->u.tcp.port = port;
    block->u.tcp.ops.process_event = tcp_process_event;
    block->u.tcp.ops.disconnect = tcp_disconnect;
    block->u.tcp.ops.new_client = tcp_new_client;
    block->u.tcp.tcp = (eb_list->broadcast ? tcp_server : tcp_connect)(ip, port, &block->u.tcp.ops);
    if (block->u.tcp.tcp) {
        block->type = TYPE_TCP;
        if (!eb_list->broadcast && using_order(dev)) {
            block->u.tcp.ops.notify_to_recv = tcp_notify;
            block->event_buf = event_buf_alloc();
            if (!block->event_buf ||
                order_register(dev, tcp_read_event, block) < 0) {
                free(block->event_buf);
                goto failed;
            }
        }
        return 0;
    }

    /*
     * Check if it is a cdev.
     * Currently only virtio_console are supported, /dev/virtio-ports/.
     */
    if (stat(port, &st) == 0 &&
        S_ISCHR(st.st_mode)) {
        int fd = open(port, (eb_list->broadcast ? O_WRONLY : O_RDONLY) | O_NONBLOCK);
        unsigned int events = (eb_list->broadcast ? EPOLLOUT : EPOLLIN) | EPOLLET | EPOLLHUP;
        if (fd >= 0 &&
            main_epoll_add(fd, events, block, handle_cdev_event) == 0) {
            // main_epoll_add < 0 && errno == EPERM, The file fd does not support epoll.
            block->u.cdev.fd = fd;
            block->u.cdev.connected = 0;
            block->u.cdev.filename = port;
            block->u.cdev.head = 0;
            block->u.cdev.tail = 0;
            memset(&block->u.cdev.lost_event, 0, sizeof(struct perf_record_lost));
            block->u.cdev.buf = event_buf_alloc();
            block->u.cdev.event_copy = block->u.cdev.buf;
            block->u.cdev.read = 0;
            block->type = TYPE_CDEV;
            block->event_buf = block->u.cdev.buf;
            if (block->u.cdev.buf && (eb_list->broadcast ||
                !using_order(dev) || order_register(dev, cdev_read_event, block) == 0)) {
                printf("Open cdev %s\n", block->u.cdev.filename);
                return 0;
            } else {
                free(block->u.cdev.buf);
                main_epoll_del(fd);
                close(fd);
                goto failed;
            }
        }
    }

    /*
     * Check if it is a file.
     */
    if ((file = fopen(port, eb_list->broadcast ? "w+" : "r"))) {
        block->u.file.file = file;
        block->u.file.pos = 0;
        block->u.file.filename = port;
        block->type = TYPE_FILE;
        if (!eb_list->broadcast) {
            block->u.file.notifyfd = eventfd(1, EFD_NONBLOCK);
            if (block->u.file.notifyfd < 0)
                goto failed;
            main_epoll_add(block->u.file.notifyfd, EPOLLIN, block, handle_file_event);
        } else
            block->u.file.notifyfd = -1;
        printf("Open file %s\n", block->u.file.filename);
        return 0;
    }

failed:
    // failed
    list_del(&block->link);
    if (file) fclose(file);
    free(block);
err_return:
    fprintf(stderr, "The pull=%s attribute is incorrect.\n", value);
    return -1;
}

static void block_free(struct event_block *block)
{
    struct event_block_list *eb_list = block->eb_list;
    struct prof_dev *dev = eb_list->tp->dev;

    switch (block->type) {
        case TYPE_TCP:
            tcp_close(block->u.tcp.tcp);
            if (!eb_list->broadcast && using_order(dev)) {
                order_unregister(dev, block);
                free(block->event_buf);
            }
            break;
        case TYPE_CDEV:
            if (!eb_list->broadcast && using_order(dev))
                order_unregister(dev, block);
            if (block->u.cdev.fd >= 0) {
                main_epoll_del(block->u.cdev.fd);
                close(block->u.cdev.fd);
            }
            free(block->u.cdev.buf);
            printf("Close cdev %s\n", block->u.cdev.filename);
            break;
        case TYPE_FILE:
            if (block->u.file.notifyfd >= 0)
                main_epoll_del(block->u.file.notifyfd);
            fclose(block->u.file.file);
            printf("Close file %s\n", block->u.file.filename);
            break;
        default:
            break;
    }
    list_del(&block->link);
    free(block);

    if (!eb_list->freeing &&
        !eb_list->broadcast && list_empty(&eb_list->block_list)) {
        struct tp *tp = eb_list->tp;
        printf("%s:%s re-enable kernel events\n", tp->sys, tp->name);
        if (tp->evsel) {
            perf_evsel__keep_disable(tp->evsel, false);
            perf_evsel__enable(tp->evsel);
            tp->kernel = true;
        }
    }
}

static inline void block_broadcast(struct event_block *block, const void *buf, size_t len, int flags)
{
    switch (block->type) {
        case TYPE_TCP:
            tcp_server_broadcast(block->u.tcp.tcp, buf, len, flags);
            break;
        case TYPE_CDEV: {
            struct cdev_block *cdev = &block->u.cdev;
            if (cdev->connected) {
                int add = 0;
                if (CIRC_SPACE(cdev->head, cdev->tail, BUF_LEN) < len) {
                    handle_cdev_event(cdev->fd, EPOLLOUT, block);
                }
                add = CIRC_CNT(cdev->head, cdev->tail, BUF_LEN) == 0;
                if (likely(CIRC_SPACE(cdev->head, cdev->tail, BUF_LEN) >= len)) {
                    int space = CIRC_SPACE_TO_END(cdev->head, cdev->tail, BUF_LEN);
                    space = min((int)len, space);
                    len -= space;
                    memcpy(cdev->buf + (cdev->head & (BUF_LEN-1)), buf, space);
                    if (len)
                        memcpy(cdev->buf, buf + space, len);
                    cdev->head = (cdev->head + space + len) & (BUF_LEN-1);
                    if (add)
                        main_epoll_add(cdev->fd, EPOLLOUT | EPOLLHUP, block, handle_cdev_event);
                } else {
                    // lost event
                    cdev->lost_event.header.size = sizeof(struct perf_record_lost);
                    cdev->lost_event.header.type = PERF_RECORD_LOST;
                    cdev->lost_event.header.misc = 0;
                    cdev->lost_event.id          = 0;
                    cdev->lost_event.lost        ++ ;
                }
            }
            } break;
        case TYPE_FILE:
            if (block->u.file.pos == 0)
                file_write_header(block);
            block->u.file.pos += fwrite(buf, 1, len, block->u.file.file);
            break;
        default:
            break;
    }
}

static void block_list_free(struct tp *tp, bool broadcast)
{
    struct event_block_list *eb_list = (broadcast ? tp->broadcast : tp->receive);
    struct event_block *block, *next;

    if (!eb_list)
        return ;

    eb_list->freeing = 1;
    list_for_each_entry_safe(block, next, &eb_list->block_list, link) {
        block_free(block);
    }
    if (!list_empty(&eb_list->link_to)) {
        perf_clock_unref();
        list_del(&eb_list->link_to);
    }
    free(eb_list);

    if (broadcast) tp->broadcast = NULL;
    else tp->receive = NULL;
}

static int block_list_new(struct tp *tp, char *s, bool broadcast)
{
    struct event_block_list *eb_list = (broadcast ? tp->broadcast : tp->receive);
    char *sep;

    if (eb_list == NULL) {
        if (!(eb_list = malloc(sizeof(*eb_list)))) return -1;
        memset(eb_list, 0, sizeof(*eb_list));

        INIT_LIST_HEAD(&eb_list->link_to);
        INIT_LIST_HEAD(&eb_list->block_list);
        eb_list->tp = tp;
        eb_list->time_pos = -1;
        eb_list->broadcast = broadcast;
        eb_list->freeing = 0;
        eb_list->ins_oncpu = prof_dev_ins_oncpu(tp->dev);

        if (broadcast) tp->broadcast = eb_list;
        else tp->receive = eb_list;
    }

    while ((sep = strchr(s, ',')) != NULL) {
        *sep = '\0';
        if (block_new(eb_list, s) < 0) goto err;
        s = sep + 1;
    }
    if (block_new(eb_list, s) < 0) goto err;

    /*
     * Broadcast events, heap sorting is enabled by default.
     * See the comments in order.c.
     * Just set it to 1, order_init() is called later.
     */
    if (broadcast) {
        tp->dev->env->order = 1;
        if (perf_clock_ref() == 0)
            list_add_tail(&eb_list->link_to, &broadcast_block_list);
    }

    return 0;

err:
    block_list_free(tp, broadcast);
    return -1;
}


int tp_broadcast_new(struct tp *tp, char *s)
{
    return block_list_new(tp, s, true);
}

void tp_broadcast_free(struct tp *tp)
{
    block_list_free(tp, true);
}

int tp_broadcast_event(struct tp *tp, union perf_event *event)
{
    struct event_block_list *eb_list = tp->broadcast;
    struct event_block *block;

    if (!eb_list)
        return 0;

    if (event->header.type == PERF_RECORD_SAMPLE) {
        if (unlikely(eb_list->time_pos == -1))
            eb_list->time_pos = eb_list->tp->dev->pos.time_pos;
        eb_list->last_event_time = *(u64 *)((void *)event->sample.array + eb_list->time_pos);
    }

    list_for_each_entry(block, &eb_list->block_list, link) {
        block_broadcast(block, event, event->header.size, 0);
    }
    return 1;
}

int tp_receive_new(struct tp *tp, char *s)
{
    return block_list_new(tp, s, false);
}

void tp_receive_free(struct tp *tp)
{
    block_list_free(tp, false);
}


static struct timer push_timer;
static void perf_clock_timer(struct timer *t)
{
    /*
     * By default, 4 signals are ignored: SIGCHLD, SIGCONT, SIGURG, SIGWINCH.
     * Among them, SIGCHLD and SIGWINCH will be blocked by perf-prof and will
     * no be ignored in the kernel. SIGCONT will disrupt ptrace and cannot be
     * used. Only SIGURG.
     */
    kill(getpid(), SIGURG);
}

static void perf_clock_deinit(struct prof_dev *dev)
{
    timer_destroy(&push_timer);
}

static int perf_clock_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TIME,
        .pinned        = 1,
        .disabled      = 0,
        .watermark     = 0,
        .wakeup_events = 1,
    };
    struct perf_evsel *evsel;
    int id = tep__event_id("signal", "signal_generate");

    if (id < 0) goto failed;

    if (timer_init(&push_timer, 1, perf_clock_timer) < 0)
        goto failed;

    dev->type = PROF_DEV_TYPE_SERVICE;
    dev->silent = true;
    perf_cpu_map__put(dev->cpus);
    perf_thread_map__put(dev->threads);
    dev->cpus = perf_cpu_map__dummy_new();
    dev->threads = thread_map__new_by_tid(getpid());

    attr.config = id;
    evsel = perf_evsel__new(&attr);
    if (!evsel)  goto del_timer;
    perf_evlist__add(evlist, evsel);

    timer_start(&push_timer, 10*NSEC_PER_MSEC, false);
    return 0;

del_timer:
    timer_destroy(&push_timer);
failed:
    return -1;
}

static void perf_clock_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    struct prof_dev *main_dev;
    struct event_block_list *eb_list;
    union {
        union perf_event event;
        struct perf_record_order_time o;
    } order_time;
    // PERF_SAMPLE_TIME
    struct sample_type_header {
        __u64   time;
    } *timer = (void *)event->sample.array;

    order_time.o.header.size = sizeof(order_time);
    order_time.o.header.type = PERF_RECORD_ORDER_TIME;
    order_time.o.header.misc = 0;

    /*
     * For the push ATTR event, its frequency of occurrence will affect the heap
     * sorting on the pull side. Therefore, a periodic timer is used to flush the
     * perf_mmap event and notify the pull side of the flush time.
     *
     * The PERF_RECORD_ORDER_TIME event is used for notification, but it is not a
     * kernel event.
     */
restart:
    list_for_each_entry(eb_list, &broadcast_block_list, link_to) {
        dev = eb_list->tp->dev;

        prof_dev_get(dev);
        if (heapclock_to_perfclock(dev, dev->order.heap_popped_time) < timer->time)
            order_to(dev, timer->time);
        if (prof_dev_put(dev))
            goto restart;

        main_dev = order_main_dev(dev);
        // If the order_to() ends early due to lost, there may be events that have
        // not been flushed before the timer->time timestamp.
        if (main_dev->order.break_reason != ORDER_BREAK_LOST_WAIT &&
            main_dev->order.break_reason != ORDER_BREAK_STREAM_STOP &&
            eb_list->last_event_time < timer->time) {
            // Everything passed to the profiler uses the evclock_t clock.
            order_time.o.order_time = perfclock_to_evclock(dev, timer->time).clock;
            tp_broadcast_event(eb_list->tp, (void *)&order_time);
        }
    }
}

static profiler perf_clock = {
    .name = "perf-clock",
    .pages = 1,
    .init = perf_clock_init,
    .deinit = perf_clock_deinit,
    .sample = perf_clock_sample,
};

