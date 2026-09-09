#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/compiler.h>
#include <linux/rblist.h>
#include <api/fs/fs.h>
#include <monitor.h>
#include <tep.h>
#include <trace_helpers.h>
#include <stack_helpers.h>
#include <tp_struct.h>


static profiler blktrace;

enum block_tp {
    BLOCK_GETRQ,
    BLOCK_RQ_INSERT,
    BLOCK_RQ_ISSUE,
    BLOCK_RQ_COMPLETE,
    BLOCK_MAX,
};
struct request_track {
    struct rb_node rbnode;
    enum block_tp tp;
    dev_t dev;
    sector_t sector;
    unsigned int nr_sector;
    u64 time;
};
struct block_iostat {
    const char *name;
    __u64 type;
    __u64 min;
    __u64 max;
    __u64 n;
    __u64 sum;
    __u64 than;
};

struct block_lost_node {
    struct list_head lost_link;
    int ins;
    bool reclaim;
    u64 start_time;
    u64 end_time;
};

struct blktrace_ctx {
    struct block_iostat stats[BLOCK_MAX];
    dev_t  dev;
    int partition;
    sector_t start_sector;
    sector_t end_sector;
    int max_name_len;
    struct rblist rq_tracks;
    struct list_head lost_list;
};

// in linux/perf_event.h
// PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW
struct sample_type_raw {
    struct {
        __u32    pid;
        __u32    tid;
    }    tid_entry;
    __u64   time;
    struct {
        __u32    cpu;
        __u32    reserved;
    }    cpu_entry;
    struct {
        __u32   size;
        union {
            __u8    data[0];
            unsigned short common_type;
            struct block_getrq getrq;
            struct block_rq_insert rq_insert;
            struct block_rq_issue rq_issue;
            struct block_rq_complete rq_complete;
        };
    } __packed raw;
};

#define MINORBITS   20
#define MKDEV(ma,mi)    (((ma) << MINORBITS) | (mi))

//linux fs/stat.c
static inline dev_t new_decode_dev(u32 dev)
{
    unsigned major = (dev & 0xfff00) >> 8;
    unsigned minor = (dev & 0xff) | ((dev >> 12) & 0xfff00);
    return MKDEV(major, minor);
}

static int request_track_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct request_track *rq = container_of(rbn, struct request_track, rbnode);
    const struct request_track *r = entry;

    if (rq->dev > r->dev)
        return 1;
    else if (rq->dev < r->dev)
        return -1;

    if (rq->sector >= r->sector + r->nr_sector)
        return 1;
    else if (rq->sector + rq->nr_sector <= r->sector)
        return -1;
    else
        return 0;
}
static struct rb_node *request_track_node_new(struct rblist *rlist, const void *new_entry)
{
    const struct request_track *r = new_entry;
    struct request_track *rq = malloc(sizeof(*rq));
    if (rq) {
        *rq = *r;
        RB_CLEAR_NODE(&rq->rbnode);
        return &rq->rbnode;
    } else
        return NULL;
}
static void request_track_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct request_track *rq = container_of(rb_node, struct request_track, rbnode);
    free(rq);
}

static void iostat_reset(struct blktrace_ctx *ctx)
{
    int i;
    for (i = 0; i < BLOCK_MAX; i ++) {
        ctx->stats[i].min = ~0UL;
        ctx->stats[i].max = 0UL;
        ctx->stats[i].n = 0UL;
        ctx->stats[i].sum = 0UL;
        ctx->stats[i].than = 0UL;
    }
}

static inline unsigned get_dev_major(u32 dev)
{
    return (dev & 0xfff00) >> 8;
}

static inline unsigned get_dev_minor(u32 dev)
{
    return (dev & 0xff) | ((dev >> 12) & 0xfff00);
}

static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct blktrace_ctx *ctx;
    struct stat st;
    char path[4096];
    unsigned int major, minor;
    char *buf;
    size_t len;

    if (!env->device)
        return -1;

    if (stat(env->device, &st) < 0)
        return -1;

    ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;
    INIT_LIST_HEAD(&ctx->lost_list);

    ctx->dev = new_decode_dev((u32)st.st_rdev);
    major = get_dev_major((u32)st.st_rdev);
    minor = get_dev_minor((u32)st.st_rdev);

    memset(path, 0, 4096);
    sprintf(path, "dev/block/%u:%u/partition", major, minor);
    if (sysfs__read_str(path, &buf, &len) < 0)
        ctx->partition = 0;
    else {
        ctx->partition = atoi(buf);
        ctx->dev -= ctx->partition;
        free(buf);

        memset(path, 0, 4096);
        sprintf(path, "dev/block/%u:%u/start", major, minor);
        if (sysfs__read_str(path, &buf, &len) < 0)
            goto failed;
        else {
            ctx->start_sector = atol(buf);
            free(buf);
        }

        memset(path, 0, 4096);
        sprintf(path, "dev/block/%u:%u/size", major, minor);
        if (sysfs__read_str(path, &buf, &len) < 0)
            goto failed;
        else {
            ctx->end_sector = ctx->start_sector + atol(buf);
            free(buf);
        }
    }

    iostat_reset(ctx);

    rblist__init(&ctx->rq_tracks);
    ctx->rq_tracks.node_cmp = request_track_node_cmp;
    ctx->rq_tracks.node_new = request_track_node_new;
    ctx->rq_tracks.node_delete = request_track_node_delete;

    if (env->verbose)
        tep__ref();
    else
        tep__ref_light();

    return 0;

failed:
    free(ctx);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct blktrace_ctx *ctx = dev->private;
    struct block_lost_node *lost, *next;

    list_for_each_entry_safe(lost, next, &ctx->lost_list, lost_link)
        free(lost);
    rblist__exit(&ctx->rq_tracks);
    tep__unref();
    free(ctx);
}

static struct perf_evsel *add_tp_event(struct prof_dev *dev, const char *sys, const char *name, int i)
{
    struct perf_evlist *evlist = dev->evlist;
    struct blktrace_ctx *ctx = dev->private;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW,
        .read_format   = 0,
        .pinned        = 1,
        .disabled      = 1,
        .watermark     = 1,
    };
    struct perf_evsel *evsel;
    int id;

    id = tep__event_id(sys, name);
    if (id < 0)
        return NULL;

    prof_dev_env2attr(dev, &attr);

    attr.config = id;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        return NULL;
    }
    perf_evlist__add(evlist, evsel);

    ctx->stats[i].name = name;
    ctx->stats[i].type = id;
    if (strlen(name) > ctx->max_name_len)
        ctx->max_name_len = strlen(name);

    return evsel;
}

static int blktrace_init(struct prof_dev *dev)
{
    if (monitor_ctx_init(dev) < 0)
        return -1;

    if (!add_tp_event(dev, "block", "block_getrq", BLOCK_GETRQ)) goto failed;
    if (!add_tp_event(dev, "block", "block_rq_insert", BLOCK_RQ_INSERT)) goto failed;
    if (!add_tp_event(dev, "block", "block_rq_issue", BLOCK_RQ_ISSUE)) goto failed;
    if (!add_tp_event(dev, "block", "block_rq_complete", BLOCK_RQ_COMPLETE)) goto failed;

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int blktrace_filter(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct blktrace_ctx *ctx = dev->private;
    char filter[256];
    struct perf_evsel *evsel;
    int err;

    if (ctx->partition)
        snprintf(filter, sizeof(filter), "dev==%u && sector>=%lu && sector<=%lu", \
                    (unsigned int)ctx->dev, ctx->start_sector, ctx->end_sector);
    else
        snprintf(filter, sizeof(filter), "dev==%u", (unsigned int)ctx->dev);
    if (dev->env->verbose)
        printf("%s\n", filter);
    perf_evlist__for_each_evsel(evlist, evsel) {
        err = perf_evsel__apply_filter(evsel, filter);
        if (err < 0)
            return err;
    }
    return 0;
}

static void blktrace_interval(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct blktrace_ctx *ctx = dev->private;
    int i;
    bool than = !!env->greater_than;

    print_time(stdout);
    printf("\n");

    printf("%*s => %-*s %8s %16s %12s %12s %12s", ctx->max_name_len, "start", ctx->max_name_len, "end", "reqs",
                    env->tsc ? "total(kcyc)" : "total(us)",
                    env->tsc ? "min(kcyc)" : "min(us)",
                    env->tsc ? "avg(kcyc)" : "avg(us)",
                    env->tsc ? "max(kcyc)" : "max(us)");
    if (than)
        printf("    than(reqs)\n");
    else
        printf("\n");

    for (i=0; i<ctx->max_name_len; i++) printf("-");
    printf("    ");
    for (i=0; i<ctx->max_name_len; i++) printf("-");
    printf(" %8s %16s %12s %12s %12s",
                    "--------", "----------------", "------------", "------------", "------------");
    if (than)
        printf("  -------------\n");
    else
        printf("\n");

    for (i = 1; i < BLOCK_MAX; i++) {
        struct block_iostat *iostat1 = &ctx->stats[i-1];
        struct block_iostat *iostat = &ctx->stats[i];
        printf("%*s => %-*s %8llu %16.3f %12.3f %12.3f %12.3f",
                ctx->max_name_len, iostat1->name,
                ctx->max_name_len, iostat->name,
                iostat->n, iostat->sum/1000.0, iostat->n ? iostat->min/1000.0 : 0.0,
                iostat->n ? iostat->sum/iostat->n/1000.0 : 0.0, iostat->max/1000.0);
        if (than)
            if (iostat->than && isatty(1))
                printf(" \033[31;1m%6llu (%3llu%s)\033[0m\n", iostat->than, iostat->than * 100 / (iostat->n ? iostat->n : 1), "%");
            else
                printf(" %6llu (%3llu%s)\n", iostat->than, iostat->than * 100 / (iostat->n ? iostat->n : 1), "%");
        else
            printf("\n");
    }
    iostat_reset(ctx);
}

static void blktrace_exit(struct prof_dev *dev)
{
    blktrace_interval(dev);
    monitor_ctx_exit(dev);
}

static void blktrace_lost(struct prof_dev *dev, union perf_event *event, int cpu, int tid, u64 lost_start, u64 lost_end)
{
    int ins = prof_dev_ins(dev, cpu, tid);

    struct blktrace_ctx *ctx = dev->private;
    struct block_lost_node *pos;
    struct block_lost_node *lost;

    print_lost_fn(dev, event, cpu, tid);

    // Order is enabled by default.
    // When order is enabled, event loss will be sensed in advance, but it
    // needs to be processed later.
    lost = malloc(sizeof(*lost));
    if (lost) {
        lost->ins = ins;
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

static inline int blktrace_event_lost(struct prof_dev *dev, union perf_event *event)
{
    struct blktrace_ctx *ctx = dev->private;
    struct sample_type_raw *data = (void *)event->sample.array;
    struct block_lost_node *lost, *next;

    if (likely(list_empty(&ctx->lost_list)))
        return 0;

    list_for_each_entry_safe(lost, next, &ctx->lost_list, lost_link) {
        // Events before lost->start_time are processed normally.
        if (data->time <= lost->start_time)
            return 0;

        /*
         * Not sure which events are lost, we can only delete all request tracks
         * in `ctx->rq_tracks'. Restart collection after lost.
         */
        if (!lost->reclaim) {
            rblist__exit(&ctx->rq_tracks);
            lost->reclaim = true;
        }

        // Within the lost range, new events are also unsafe.
        if (data->time < lost->end_time) {
            return -1;
        } else {
            // Re-process subsequent events normally.
            list_del(&lost->lost_link);
            free(lost);
        }
    }
    return 0;
}


#define IF(i, trace) \
if (common_type == ctx->stats[i].type) { \
    r.tp = i; \
    r.dev = data->raw.trace.dev; \
    r.sector = data->raw.trace.sector; \
    r.nr_sector = data->raw.trace.nr_sector; \
}

static void blktrace_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    struct env *env = dev->env;
    struct blktrace_ctx *ctx = dev->private;
    struct sample_type_raw *data = (void *)event->sample.array;
    void *raw = data->raw.data;
    int size = data->raw.size;
    unsigned short common_type = data->raw.common_type;
    struct request_track *rq, *prev_rq, r;
    struct block_iostat *iostat;
    struct rb_node *rbn = NULL;
    u64 delta;
    const char *print = NULL;
    int verbose = dev->env->verbose;

    if (blktrace_event_lost(dev, event) < 0)
        goto verbose_print;

    r.time = data->time;
    IF(BLOCK_GETRQ, getrq)
    else IF(BLOCK_RQ_INSERT, rq_insert)
    else IF(BLOCK_RQ_ISSUE, rq_issue)
    else IF(BLOCK_RQ_COMPLETE, rq_complete)
    else return;

    // sector == -1: flush req
    if (r.sector == (sector_t)-1)
        return;

    iostat = &ctx->stats[r.tp];

    rbn = rblist__find(&ctx->rq_tracks, &r);
    if (rbn) {
        prev_rq = container_of(rbn, struct request_track, rbnode);

        if (r.tp == BLOCK_GETRQ)
            print = "EXIST";
        else if (prev_rq->tp != r.tp - 1) {
            if (r.tp == BLOCK_RQ_ISSUE)
                print = "BYPASS_INSERT";
            else
                print = "LOST";
        }

        delta = r.time - prev_rq->time;
        if (delta < iostat->min)
            iostat->min = delta;
        if (delta > iostat->max)
            iostat->max = delta;
        if (env->greater_than && delta > env->greater_than)
            iostat->than ++;
        iostat->n ++;
        iostat->sum += delta;

        if (env->greater_than &&
            delta > env->greater_than) {
            print = "GREATER_THAN";
            verbose = VERBOSE_NOTICE;
        }

        if (r.tp != BLOCK_RQ_COMPLETE) {
            rq = prev_rq;
            rq->tp = r.tp;
            rq->dev = r.dev;
            rq->sector = r.sector;
            rq->nr_sector = r.nr_sector;
            rq->time = r.time;
        } else
            rblist__remove_node(&ctx->rq_tracks, rbn);
    }
    else if (r.tp != BLOCK_RQ_COMPLETE)
        rblist__add_node(&ctx->rq_tracks, &r);

verbose_print:
    if (verbose &&
        (print || verbose >= VERBOSE_EVENT)) {
        tep__update_comm(NULL, data->tid_entry.tid);
        if (dev->print_title) prof_dev_print_time(dev, data->time, stdout);
        if (print)
            printf("%s", print);
        tep__print_event(data->time, data->cpu_entry.cpu, raw, size);
    }
}

static const char *blktrace_desc[] = PROFILER_DESC("blktrace",
    "[OPTION...] -d device [--than ns]",
    "Block device IO latency tracing analyzer.",
    "",
    "SYNOPSIS",
    "    Tracks the complete IO request lifecycle from creation to completion by tracing 4 key",
    "    events in the kernel block layer. Breaks down iostat's await metric into detailed",
    "    latency stages. Combines blktrace and blkparse functionality for long-term disk IO",
    "    performance monitoring.",
    "",
    "TRACEPOINT",
    "    block:block_getrq, block:block_rq_insert, block:block_rq_issue, block:block_rq_complete",
    "",
    "EXAMPLES",
    "    "PROGRAME" blktrace -d /dev/sda -i 1000",
    "    "PROGRAME" blktrace -d /dev/sda -i 1000 --than 10ms");
static const char *blktrace_argv[] = PROFILER_ARGV("blktrace",
    "OPTION:", "watermark",
    "interval", "output", "order", "mmap-pages", "exit-N", "tsc", "kvmclock", "clock-offset", "monotonic",
    "usage-self", "sampling-limit", "perfeval-cpus", "perfeval-pids", "version", "verbose", "quiet", "help",
    PROFILER_ARGV_PROFILER, "device", "than");
static profiler blktrace = {
    .name = "blktrace",
    .desc = blktrace_desc,
    .argv = blktrace_argv,
    .pages = 8,
    .order = true,
    .init = blktrace_init,
    .filter = blktrace_filter,
    .deinit = blktrace_exit,
    .interval = blktrace_interval,
    .lost = blktrace_lost,
    .sample = blktrace_sample,
};
PROFILER_REGISTER(blktrace)

