#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/refcount.h>
#include <linux/rblist.h>
#include <linux/time64.h>

#include <monitor.h>
#include <tp_struct.h>

#define TASK_COMM_LEN 16

struct pid_comm_node {
    struct rb_node rbnode;
    u64 update_time;
    char comm[TASK_COMM_LEN];
    int pid;
    bool flush;
    struct rb_node exit_node;
};

struct comm_ctx {
    struct prof_dev *comm_dev;
    refcount_t ref;
    struct rblist pid_comm;
    struct rblist exited;
    int task_newtask, task_rename;
    int sched_process_free;
    struct pid_comm_node *cached_node;
};
static struct comm_ctx *global_comm_ctx = NULL;
static struct list_head global_comm_notify_list = LIST_HEAD_INIT(global_comm_notify_list);


static int pid_comm_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct pid_comm_node *b = container_of(rbn, struct pid_comm_node, rbnode);
    const struct pid_comm_node *e = entry;

    return b->pid - e->pid;
}
static struct rb_node *pid_comm_node_new(struct rblist *rlist, const void *new_entry)
{
    struct pid_comm_node *e = (void *)new_entry;
    struct pid_comm_node *b = malloc(sizeof(*b));
    if (b) {
        b->pid = e->pid;
        b->flush = false;
        b->update_time = 0;
        RB_CLEAR_NODE(&b->rbnode);
        RB_CLEAR_NODE(&b->exit_node);
        return &b->rbnode;
    } else
        return NULL;
}
static void pid_comm_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct comm_ctx *ctx = container_of(rblist, struct comm_ctx, pid_comm);
    struct pid_comm_node *b = container_of(rb_node, struct pid_comm_node, rbnode);
    if (ctx->cached_node == b)
        ctx->cached_node = NULL;
    free(b);
}

static int pid_comm_exit_cmp(struct rb_node *rbn, const void *entry)
{
    struct pid_comm_node *b = container_of(rbn, struct pid_comm_node, exit_node);
    const struct pid_comm_node *e = entry;

    if (b->update_time > e->update_time)
        return 1;
    else if (b->update_time < e->update_time)
        return -1;
    else
        return b->pid - e->pid;
}
static struct rb_node *pid_comm_exit_new(struct rblist *rlist, const void *new_entry)
{
    struct pid_comm_node *e = (void *)new_entry;
    RB_CLEAR_NODE(&e->exit_node);
    return &e->exit_node;
}
static void pid_comm_exit_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct pid_comm_node *b = container_of(rb_node, struct pid_comm_node, exit_node);

    // notify
    if (!list_empty(&global_comm_notify_list)) {
        struct comm_notify *node, *n;
        // The notify callback may remove itself from the list.
        list_for_each_entry_safe(node, n, &global_comm_notify_list, link) {
            node->notify(node, b->pid, NOTIFY_COMM_DELETE, b->update_time);
        }
    }
}

static void comm_deinit(struct prof_dev *dev);
static int comm_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct comm_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TIME | PERF_SAMPLE_RAW,
        .pinned        = 1,
        .disabled      = 0,
        .watermark     = 1,
    };
    struct perf_evsel *evsel;

    ctx = zalloc(sizeof(*ctx));
    if (!ctx) return -1;
    dev->private = ctx;
    dev->type = PROF_DEV_TYPE_SERVICE;
    dev->silent = true;
    global_comm_ctx = ctx;

    ctx->comm_dev = dev;
    refcount_set(&ctx->ref, 1);

    rblist__init(&ctx->pid_comm);
    ctx->pid_comm.node_cmp = pid_comm_node_cmp;
    ctx->pid_comm.node_new = pid_comm_node_new;
    ctx->pid_comm.node_delete = pid_comm_node_delete;
    rblist__init(&ctx->exited);
    ctx->exited.node_cmp = pid_comm_exit_cmp;
    ctx->exited.node_new = pid_comm_exit_new;
    ctx->exited.node_delete = pid_comm_exit_delete;

    reduce_wakeup_times(dev, &attr);

    ctx->task_newtask = tep__event_id("task", "task_newtask");
    if (ctx->task_newtask < 0) goto failed;
    attr.config = ctx->task_newtask;
    evsel = perf_evsel__new(&attr);
    if (!evsel)  goto failed;
    perf_evlist__add(evlist, evsel);

    ctx->task_rename = tep__event_id("task", "task_rename");
    if (ctx->task_rename < 0) goto failed;
    attr.config = ctx->task_rename;
    evsel = perf_evsel__new(&attr);
    if (!evsel)  goto failed;
    perf_evlist__add(evlist, evsel);

    ctx->sched_process_free = tep__event_id("sched", "sched_process_free");
    if (ctx->sched_process_free < 0) goto failed;
    attr.config = ctx->sched_process_free;
    evsel = perf_evsel__new(&attr);
    if (!evsel)  goto failed;
    perf_evlist__add(evlist, evsel);

    return 0;

failed:
    comm_deinit(dev);
    return -1;
}

static void comm_gc(struct prof_dev *dev, u64 time_before)
{
    struct comm_ctx *ctx = dev->private;
    while (1) {
        struct rb_node *rbn = rb_first_cached(&ctx->exited.entries);
        struct pid_comm_node *node = rb_entry_safe(rbn, struct pid_comm_node, exit_node);
        if (!node || node->update_time >= time_before)
            break;
        rblist__remove_node(&ctx->exited, rbn);
        rblist__remove_node(&ctx->pid_comm, &node->rbnode);
    }
}

static void comm_update(struct prof_dev *dev, u64 time, bool exit, int pid, char *comm)
{
    struct comm_ctx *ctx = dev->private;
    struct pid_comm_node new, *node;
    struct rb_node *rbn;

    new.pid = pid;

    rbn = rblist__findnew(&ctx->pid_comm, &new);
    node = rb_entry_safe(rbn, struct pid_comm_node, rbnode);
    if (node) {
        if (node->update_time < time) {
            if (unlikely(!RB_EMPTY_NODE(&node->exit_node))) {
                rblist__remove_node(&ctx->exited, &node->exit_node);
                RB_CLEAR_NODE(&node->exit_node);
            }
            node->update_time = time;
            node->flush = false;
            *(u64 *)(node->comm) = *(u64 *)(comm);
            *(u64 *)(node->comm+8) = *(u64 *)(comm+8);
        }
        if (exit) {
            rblist__add_node(&ctx->exited, node);
        }
    }
}

/* Skip "." and ".." directories */
static int filter(const struct dirent *dir)
{
    if (dir->d_name[0] == '.')
        return 0;
    else
        return 1;
}

static char *get_comm(int pid, char *comm)
{
    char path[64];
    int fd, len;

    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    len = (int)read(fd, comm, TASK_COMM_LEN);
    close(fd);
    if (len <= 0) return NULL;

    len--;
    if (comm[len] == '\n' || len == TASK_COMM_LEN-1)
        comm[len] = '\0';
    return comm;
}

static void comm_enabled(struct prof_dev *dev)
{
    DIR *proc;
    int items, i;
    char path[NAME_MAX + 1 + 6];
    struct dirent *dirent, **namelist = NULL;
    char buff[TASK_COMM_LEN] = {0};

    // pid 0, swapper
    strcpy(buff, "swapper");
    comm_update(dev, 1, false, 0, buff);

    proc = opendir("/proc");
    if (proc == NULL)
        return;

    while ((dirent = readdir(proc)) != NULL) {
        char *end;
        pid_t pid = strtol(dirent->d_name, &end, 10);

        if (*end) /* only interested in proper numerical dirents */
            continue;

        snprintf(path, sizeof(path), "/proc/%d/task", pid);
        items = scandir(path, &namelist, filter, NULL);
        if (items <= 0)
            continue;

        for (i = 0; i < items; i++) {
            int tid = atoi(namelist[i]->d_name);
            char *comm = get_comm(tid, buff);
            if (comm)
                comm_update(dev, 1, false, tid, comm);
        }

        for (i = 0; i < items; i++)
            zfree(&namelist[i]);
        free(namelist);
    }

    closedir(proc);
    return;
}

static void comm_deinit(struct prof_dev *dev)
{
    struct comm_ctx *ctx = dev->private;
    global_comm_ctx = NULL;
    rblist__exit(&ctx->exited);
    rblist__exit(&ctx->pid_comm);
    free(ctx);
}

static void comm_interval(struct prof_dev *dev)
{
    u64 time_before = prof_dev_list_minevtime();
    comm_gc(dev, time_before);
}

static void comm_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    struct comm_ctx *ctx = dev->private;
    // PERF_SAMPLE_TIME | PERF_SAMPLE_RAW
    struct sample_type_header {
        __u64   time;
        struct {
            __u32   size;
            union {
                __u8    data[0];
                unsigned short common_type;
                struct task_newtask task_newtask;
                struct task_rename task_rename;
                struct sched_process_free process_free;
            };
        } __packed raw;
    } *data = (void *)event->sample.array;
    unsigned short common_type = data->raw.common_type;

    if (common_type == ctx->task_newtask)
        comm_update(dev, data->time, false, data->raw.task_newtask.pid, data->raw.task_newtask.comm);
    else if (common_type == ctx->task_rename)
        comm_update(dev, data->time, false, data->raw.task_rename.pid, data->raw.task_rename.newcomm);
    else if (common_type == ctx->sched_process_free)
        comm_update(dev, data->time, true, data->raw.process_free.pid, data->raw.process_free.comm);
}

static profiler comm = {
    .name = "comm",
    .pages = 16,
    .init = comm_init,
    .enabled = comm_enabled,
    .deinit = comm_deinit,
    .interval = comm_interval,
    .sample = comm_sample,
};

int global_comm_ref(void)
{
    if (global_comm_ctx == NULL) {
        struct env *env = zalloc(sizeof(*env));
        if (!env)
            return -1;
        env->interval = 1000;
        if (!prof_dev_open(&comm, env))
            return -1;
    } else
        refcount_inc(&global_comm_ctx->ref);

    return 0;
}

void global_comm_unref(void)
{
    if (!global_comm_ctx)
        return;

    if (refcount_dec_and_test(&global_comm_ctx->ref))
        prof_dev_close(global_comm_ctx->comm_dev);
}

char *global_comm_get(int pid)
{
    struct pid_comm_node find, *node;
    struct rb_node *rbn;

    if (!global_comm_ctx)
        return NULL;

    if (global_comm_ctx->cached_node &&
        global_comm_ctx->cached_node->pid == pid)
        return global_comm_ctx->cached_node->comm;

    find.pid = pid;
    rbn = rblist__find(&global_comm_ctx->pid_comm, &find);
    node = rb_entry_safe(rbn, struct pid_comm_node, rbnode);

    if (!node || node->flush) {
        prof_dev_flush(global_comm_ctx->comm_dev, PROF_DEV_FLUSH_NORMAL);
        rbn = rblist__find(&global_comm_ctx->pid_comm, &find);
        node = rb_entry_safe(rbn, struct pid_comm_node, rbnode);
    }

    if (node)
        global_comm_ctx->cached_node = node;

    return node ? node->comm : NULL;
}

void global_comm_flush(int pid)
{
    struct pid_comm_node find, *node;
    struct rb_node *rbn;

    if (!global_comm_ctx)
        return;
    if (pid == 0)
        return;

    find.pid = pid;
    rbn = rblist__find(&global_comm_ctx->pid_comm, &find);
    node = rb_entry_safe(rbn, struct pid_comm_node, rbnode);
    if (node)
        node->flush = true;
}

void global_comm_register_notify(struct comm_notify *node)
{
    INIT_LIST_HEAD(&node->link);
    list_add(&node->link, &global_comm_notify_list);
}

void global_comm_unregister_notify(struct comm_notify *node)
{
    list_del_init(&node->link);
}

