#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <linux/rblist.h>
#include <monitor.h>
#include <stack_helpers.h>

#define PYSTACK_MAX_DEPTH 128

struct pystack_node {
    struct rb_node rbnode;
    int pid;
    int depth;
    const char *stack[PYSTACK_MAX_DEPTH];
};

struct pystack_lost_node {
    struct list_head lost_link;
    int cpu;
    bool reclaim;
    u64 start_time;
    u64 end_time;
};

struct pystack_ctx {
    struct rblist pystack;
    void *fixed_event;
    struct list_head lost_list; // struct pystack_lost_node
    int fun_entry;
    int fun_return;
};

struct function__entry_return {
    COMMON_HEADER
    unsigned long __probe_ip;
    unsigned short filename_offset;
    unsigned short filename_len;
    unsigned short funcname_offset;
    unsigned short funcname_len;
    int lineno;
};

static int pystack_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct pystack_node *b = container_of(rbn, struct pystack_node, rbnode);
    const struct pystack_node *e = entry;

    return b->pid - e->pid;
}
static struct rb_node *pystack_node_new(struct rblist *rlist, const void *new_entry)
{
    struct pystack_node *e = (void *)new_entry;
    struct pystack_node *b = malloc(sizeof(*b));
    if (b) {
        b->pid = e->pid;
        b->depth = 0;
        memset(b->stack, 0, sizeof(b->stack));
        RB_CLEAR_NODE(&b->rbnode);
        return &b->rbnode;
    } else
        return NULL;
}
static void pystack_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct pystack_node *b = container_of(rb_node, struct pystack_node, rbnode);
    free(b);
}

static int python_event_id(const char *sys, const char *name)
{
    event_fields *ev_fileds;
    int id = tep__event_id(sys, name);
    if (id < 0) {
        fprintf(stderr, "Not found, use '"PROGRAME" usdt add' to add %s:%s.\n", sys, name);
        return -1;
    }
    ev_fileds = tep__event_fields(id);
    if (!ev_fileds || !ev_fileds[5].name || !ev_fileds[6].name ||
        !ev_fileds[7].name || !ev_fileds[8].name || !ev_fileds[9].name ||
        strcmp(ev_fileds[5].name, "filename_offset") != 0 ||
        strcmp(ev_fileds[7].name, "funcname_offset") != 0 ||
        strcmp(ev_fileds[9].name, "lineno") != 0) {
        fprintf(stderr, "Error, the args of %s:%s needs to be added in the order: "
                        "filename=+0(%%s):string funcname=+0(%%s):string lineno=%%s:s32.\n", sys, name);
        id = -1;
    }
    free(ev_fileds);
    return id;
}

static void pystack_deinit(struct prof_dev *dev)
{
    struct pystack_ctx *ctx = dev->private;
    struct pystack_lost_node *lost, *next;

    list_for_each_entry_safe(lost, next, &ctx->lost_list, lost_link)
        free(lost);
    rblist__exit(&ctx->pystack);
    free(ctx->fixed_event);
    free(ctx);
}

static int pystack_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct pystack_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TIME | PERF_SAMPLE_RAW,
        .pinned        = 1,
        .disabled      = 1,
        .watermark     = 1,
    };
    struct perf_evsel *evsel;

    ctx = zalloc(sizeof(*ctx));
    if (!ctx) return -1;
    dev->private = ctx;
    dev->silent = true;
    INIT_LIST_HEAD(&ctx->lost_list);

    reduce_wakeup_times(dev, &attr);

    rblist__init(&ctx->pystack);
    ctx->pystack.node_cmp = pystack_node_cmp;
    ctx->pystack.node_new = pystack_node_new;
    ctx->pystack.node_delete = pystack_node_delete;

    ctx->fixed_event = malloc(PERF_SAMPLE_MAX_SIZE);
    if (!ctx->fixed_event)
        goto deinit;

    tep__ref_light();

    ctx->fun_entry = python_event_id("python", "function__entry");
    if (ctx->fun_entry < 0) goto failed;
    attr.config = ctx->fun_entry;
    evsel = perf_evsel__new(&attr);
    if (!evsel)  goto failed;
    perf_evlist__add(evlist, evsel);

    ctx->fun_return = python_event_id("python", "function__return");
    if (ctx->fun_return < 0) goto failed;
    attr.config = ctx->fun_return;
    evsel = perf_evsel__new(&attr);
    if (!evsel)  goto failed;
    perf_evlist__add(evlist, evsel);

    tep__unref();

    return 0;

failed:
    tep__unref();
deinit:
    pystack_deinit(dev);
    return -1;
}

static void pystack_lost(struct prof_dev *dev, union perf_event *event, int cpu, int tid, u64 lost_start, u64 lost_end)
{
    struct pystack_ctx *ctx = dev->private;
    struct pystack_lost_node *pos;
    struct pystack_lost_node *lost;

    print_lost_fn(dev, event, cpu, tid);

    // Thread loss event: just remove the thread itself.
    if (tid != -1) {
        struct pystack_node tmp;
        tmp.pid = tid;
        rblist__find_remove(&ctx->pystack, &tmp);
        return;
    }

    // Order is enabled by default.
    // When order is enabled, event loss will be sensed in advance, but it
    // needs to be processed later.
    lost = malloc(sizeof(*lost));
    if (lost) {
        lost->cpu = cpu;
        lost->reclaim = false;
        lost->start_time = lost_start;
        lost->end_time = lost_end;

        list_for_each_entry(pos, &ctx->lost_list, lost_link) {
            if (pos->start_time > lost_start)
                break;
        }
        list_add_tail(&lost->lost_link, &pos->lost_link);
    }
}

static inline int pystack_event_lost(struct prof_dev *dev, union perf_event *event)
{
    struct pystack_ctx *ctx = dev->private;
    // PERF_SAMPLE_TIME | PERF_SAMPLE_RAW
    struct sample_type_header {
        __u64   time;
    } *data = (void *)event->sample.array;
    struct pystack_lost_node *lost, *next;

    if (likely(list_empty(&ctx->lost_list)))
        return 0;

    list_for_each_entry_safe(lost, next, &ctx->lost_list, lost_link) {
        // Events before lost->start_time are processed normally.
        if (data->time <= lost->start_time)
            return 0;

        /*
         * Not sure which events are lost, we can only delete all process stacks
         * in `ctx->pystack'. Restart collection after lost.
         */
        if (!lost->reclaim) {
            rblist__exit(&ctx->pystack);
            lost->reclaim = true;
        }

        // Within the lost range, new events are also unsafe.
        if (data->time < lost->end_time) {
            return -1;
        } else {
            list_del(&lost->lost_link);
            free(lost);
        }
    }
    return 0;
}

static void pystack_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    struct pystack_ctx *ctx = dev->private;
    // PERF_SAMPLE_TIME | PERF_SAMPLE_RAW
    struct sample_type_header {
        __u64   time;
        struct {
            __u32   size;
            union {
                __u8    data[0];
                unsigned short common_type;
                struct function__entry_return function;
            };
        } __packed raw;
    } *data = (void *)event->sample.array;
    unsigned short common_type = data->raw.common_type;
    struct function__entry_return *function = &data->raw.function;
    const char *filename = (const char *)function + function->filename_offset;
    const char *funcname = (const char *)function + function->funcname_offset;
    int lineno = function->lineno;
    struct pystack_node tmp, *node;
    struct rb_node *rbn;
    char buf[4096];

    if (!function->common_pid)
        return;

    if (unlikely(pystack_event_lost(dev, event) < 0))
        return;

    tmp.pid = function->common_pid;
    rbn = rblist__findnew(&ctx->pystack, &tmp);
    node = rb_entry_safe(rbn, struct pystack_node, rbnode);
    if (node) {
        int n = snprintf(buf, sizeof(buf), "%s (%s:%d)", funcname, filename, lineno);
        if (common_type == ctx->fun_entry) {
            if (node->depth < PYSTACK_MAX_DEPTH) {
                const char *str = unique_string_len(buf, n>sizeof(buf) ? sizeof(buf)-1 : n);
                node->stack[node->depth] = str;
            }
            node->depth++;
        } else if (common_type == ctx->fun_return) {
            int len = function->funcname_len + function->filename_len;
            if (node->depth > 0) {
                node->depth--;
                if (node->depth < PYSTACK_MAX_DEPTH &&
                    strncmp(node->stack[node->depth], buf, len) != 0)
                    printf("BUG pystack: %s %s\n", node->stack[node->depth], buf);
            }
            if (node->depth == 0)
                rblist__remove_node(&ctx->pystack, rbn);
        }
    }
}

static profiler pystack = {
    .name = "pystack",
    .pages = 16,
    .order = 1,
    .init = pystack_init,
    .deinit = pystack_deinit,
    .lost = pystack_lost,
    .sample = pystack_sample,
};

int pystack_link(struct prof_dev *main_dev)
{
    struct prof_dev *pydev;
    struct env *e;

    if (main_dev->prof == &pystack)
        return 0;

    e = clone_env(main_dev->env);
    if (!e)
        return -1;

    // Specifies the parent of pydev so that the real main_dev can be found
    // when heap-sorting pydev events. See order_main_dev().
    pydev = prof_dev_open_cpu_thread_map(&pystack, e, NULL, NULL, main_dev);
    if (!pydev)
        return -1;

    if (order_together(main_dev, pydev) < 0) {
        prof_dev_close(pydev);
        return -1;
    }
    main_dev->links.pystack = pydev;
    return 0;
}

void pystack_unlink(struct prof_dev *main_dev)
{
    if (main_dev->links.pystack) {
        prof_dev_close(main_dev->links.pystack);
        main_dev->links.pystack = NULL;
    }
}

union perf_event *
pystack_perf_event(struct prof_dev *main_dev, union perf_event *event, bool *writable, int reserved)
{
    struct prof_dev *pydev = main_dev->links.pystack;
    struct pystack_ctx *ctx;
    struct pystack_node tmp, *node;
    struct rb_node *rbn;
    void *data;
    int pid;
    bool callchain;

    if (!pydev ||
        main_dev->pos.tid_pos < 0 ||
        main_dev->pos.callchain_pos < 0)
        return event;

    data = (void *)event->sample.array;
    pid = *(u32 *)(data + main_dev->pos.tid_pos + sizeof(u32));
    if (pid == 0)
        return event;

    callchain = main_dev->env->callchain;
    if (!callchain && main_dev->pos.id_pos >= 0) {
        struct perf_evsel *evsel;
        u64 id = *(u64 *)(data + main_dev->pos.id_pos);
        evsel = perf_evlist__id_to_evsel(main_dev->evlist, id, NULL);
        callchain = !!(perf_evsel__attr(evsel)->sample_type & PERF_SAMPLE_CALLCHAIN);
    }
    if (!callchain)
        return event;

    ctx = pydev->private;
    tmp.pid = pid;
    rbn = rblist__find(&ctx->pystack, &tmp);
    node = rb_entry_safe(rbn, struct pystack_node, rbnode);
    if (node) {
        union perf_event *new_event = ctx->fixed_event + reserved;
        struct callchain *cc = data + main_dev->pos.callchain_pos;
        int copy_len = (void *)&cc->ips[cc->nr] - (void *)event;
        int depth = node->depth;
        int d;

        /*
         * Generate new events: put PERF_CONTEXT_PYSTACK at the end of
         * the callchain.
         *
         * { u64   nr,
         *   u64   ips[nr]; } && PERF_SAMPLE_CALLCHAIN
         *
         * Default callchain context order:
         *   PERF_CONTEXT_KERNEL
         *   PERF_CONTEXT_USER
         *   PERF_CONTEXT_PYSTACK => Contains only the unique string:
         *                           "funcname (filename:lineno)"
         */
        memcpy(new_event, event, copy_len);
        data = (void *)new_event->sample.array;
        cc = data + main_dev->pos.callchain_pos;
        cc->ips[cc->nr++] = PERF_CONTEXT_PYSTACK;

        if (depth > PYSTACK_MAX_DEPTH)
            depth = PYSTACK_MAX_DEPTH;
        d = depth;
        while (depth-- > 0)
            cc->ips[cc->nr++] = (u64)node->stack[depth];

        if (event->header.size > copy_len)
            memcpy((void *)&cc->ips[cc->nr], (void *)event + copy_len, event->header.size - copy_len);

        new_event->header.size += (d+1) * sizeof(u64);
        *writable = 1;
        return new_event;
    }
    return event;
}

