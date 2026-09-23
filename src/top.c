#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#include <linux/compiler.h>
#include <termios.h>
#include <monitor.h>
#include <dlfcn.h>
#include <tep.h>
#include <linux/rblist.h>
#include <trace_helpers.h>
#include <stack_helpers.h>

#define TASK_COMM_LEN 16

struct top_ctx {
    struct tp_list *tp_list;
    struct rblist top_list;
    unsigned long nr_events;
    struct tp *tp_printkey;
    char *EVENT; //toupper(env->event)
    int nr_fields;
    int nr_top_by;
    struct {
        char *field;
        int len;
        bool top_by;
    } *fields;
    char *key_name;// toupper(env->key)
    int  key_len;
    char *comm;
    int show_comm; // 0: no comm; 1: comm ATTR; 2: pid->comm
    bool only_comm; // only COMM

    bool altwin;
};

struct top_info {
    struct rb_node rbnode;
    unsigned long key;
    char comm[TASK_COMM_LEN];
    char *pcomm;
    unsigned long counter[0];
};

struct tmp_entry {
    struct top_ctx *ctx;
    struct top_info *info;
};

static int top_info_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct top_info *t = container_of(rbn, struct top_info, rbnode);
    const struct top_info *e = entry;

    if (t->key > e->key)
        return 1;
    else if (t->key < e->key)
        return -1;
    else
        return e->pcomm ? strcmp(t->pcomm, e->pcomm) : 0;
}

static struct rb_node *top_info_node_new(struct rblist *rlist, const void *new_entry)
{
    struct top_ctx *ctx = container_of(rlist, struct top_ctx, top_list);
    int size = offsetof(struct top_info, counter[ctx->nr_fields]);
    const struct top_info *e = new_entry;
    struct top_info *t = malloc(size);
    if (t) {
        RB_CLEAR_NODE(&t->rbnode);
        t->key = e->key;
        if (e->pcomm) {
            int len = strlen(e->pcomm);
            if (len < TASK_COMM_LEN) {
                t->pcomm = t->comm;
                strcpy(t->pcomm, e->pcomm);
            } else {
                t->pcomm = strdup(e->pcomm);
                if (!t->pcomm) {
                    t->pcomm = t->comm;
                    strncpy(t->pcomm, e->pcomm, TASK_COMM_LEN - 1);
                    t->pcomm[TASK_COMM_LEN - 1] = '\0';
                }
            }
        } else
            t->pcomm = NULL;
        memset((void *)t + sizeof(struct top_info), 0, size - sizeof(struct top_info));
        return &t->rbnode;
    } else
        return NULL;
}
static void top_info_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct top_info *t = container_of(rb_node, struct top_info, rbnode);
    if (t->pcomm && t->pcomm != t->comm)
        free(t->pcomm);
    free(t);
}
static void top_info_node_delete_empty(struct rblist *rblist, struct rb_node *rb_node)
{
}
static int top_info_sorted_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct top_info *t = container_of(rbn, struct top_info, rbnode);
    const struct tmp_entry *tmp = entry;
    const struct top_ctx *ctx = tmp->ctx;
    const struct top_info *e = tmp->info;
    int i;

    if (ctx->nr_top_by)
    for (i = 0; i < ctx->nr_fields; i++) {
        if (!ctx->fields[i].top_by)
            continue;
        if (t->counter[i] > e->counter[i])
            return -1;
        else if (t->counter[i] < e->counter[i])
            return 1;
    }
    for (i = 0; i < ctx->nr_fields; i++) {
        if (ctx->fields[i].top_by)
            continue;
        if (t->counter[i] > e->counter[i])
            return -1;
        else if (t->counter[i] < e->counter[i])
            return 1;
    }
    return top_info_node_cmp(rbn, entry);
}
static struct rb_node *top_info_sorted_node_new(struct rblist *rlist, const void *new_entry)
{
    struct tmp_entry *tmp = (void *)new_entry;
    struct top_info *t = tmp->info;
    RB_CLEAR_NODE(&t->rbnode);
    return &t->rbnode;
}
static void set_term_quiet_input(struct termios *old)
{
    struct termios tc;

    tcgetattr(0, old);
    tc = *old;
    tc.c_lflag &= ~(ICANON | ECHO);
    tc.c_cc[VMIN] = 0;
    tc.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &tc);
}
struct termios save;
static void altwin_new(void)
{
    set_term_quiet_input(&save);
    /* ANSI Escape Codes
     * ESC[?1049h  enables the alternative buffer
     * ESC[?25l    make cursor invisible
     * ESC[H       moves cursor to home position (0, 0)
     * ESC[2J      erase entire screen
    **/
    printf("\033[?1049h\033[?25l\033[H\033[2J\n");
}

static void altwin_end(void)
{
    tcsetattr(0, TCSANOW, &save);
    /*
     * ESC[?25h     make cursor visible
     * ESC[?1049l   disables the alternative buffer
    **/
    printf("\033[?25h\033[?1049l\n");
}

static void altwin_title_begin(void)
{
    /*
     * ESC[H       moves cursor to home position (0, 0)
     * ESC[2J      erase entire screen
     * ESC[?25h    make cursor visible
     * ESC[7m      set inverse mode
    **/
    printf("\033[H\033[2J\033[?25h\033[7m");
}

static void altwin_title_end(void)
{
    /*
     * ESC[0m      reset all modes (styles and colors)
    **/
    printf("\033[0m");
}

static void monitor_ctx_exit(struct prof_dev *dev);
static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct top_ctx *ctx;
    int i, j, f = 0;
    int len, nr_key = 0;
    char *key_name = NULL;
    char *comm = NULL;
    struct tp *tp;

    if (!env->event)
        return -1;

    ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    tep__ref();

    ctx->EVENT = strdup(env->event);
    len = strlen(ctx->EVENT);
    for (i = 0; i < len; i++)
        ctx->EVENT[i] = (char)toupper(ctx->EVENT[i]);

    ctx->tp_list = tp_list_new(dev, env->event);
    if (!ctx->tp_list)
        goto failed;

    ctx->nr_fields = ctx->tp_list->nr_top;
    ctx->fields = calloc(ctx->nr_fields, sizeof(*ctx->fields));
    if (!ctx->fields)
        goto failed;
    for_each_real_tp(ctx->tp_list, tp, i) {
        for (j = 0; j < tp->nr_top; j++) {
            char *field = (j == 0 && tp->alias) ? tp->alias : tp->top_add[j].field;
            ctx->fields[f].field = ctx->EVENT + (field - ctx->tp_list->event_str);
            ctx->fields[f].len = strlen(field);
            ctx->fields[f].field[ctx->fields[f].len] = '\0';
            ctx->fields[f].top_by = tp->top_add[j].top_by;
            if (ctx->fields[f].len < 12)
                ctx->fields[f].len = 12;
            if (ctx->fields[f].top_by)
                ctx->nr_top_by ++;
            f ++;
        }
        if (env->key && !tp->key) {
            tp->key_prog = tp_new_prog(tp, env->key);
            if (!tp->key_prog) {
                fprintf(stderr, "%s:%s: Cannot set key '%s'\n", tp->sys, tp->name, env->key);
                goto failed;
            }
            tp->key = env->key;
        }
        nr_key += !!tp->key_prog;

        if (tp->printkey_prog && !ctx->tp_printkey)
            ctx->tp_printkey = tp;

        if (tp->key && !key_name) {
            key_name = strdup(tp->key);
        }
        if (tp->comm && !comm) {
            comm = strdup(tp->comm);
        }
    }

    if (nr_key > 0 && nr_key != ctx->tp_list->nr_real_tp) {
        fprintf(stderr, "When using key, all events must have a 'key' attr.\n");
        goto failed;
    }

    /*
     * tp->key:  show_comm = !!nr_comm;
     * env->key: show_comm = !!nr_comm;
     * raw->tid: show_comm = true;
    **/
    if (key_name) {
        len = strlen(key_name);
        for (i = 0; i < len; i++)
            key_name[i] = (char)toupper(key_name[i]);
        ctx->key_name = key_name;

        // key!=PID, whether to display COMM is determined by the comm attr.
        ctx->show_comm = ctx->tp_list->nr_comm ? 1 : 0;
    } else {
        ctx->key_name = strdup("PID");

        // key=PID can display COMM
        ctx->show_comm = 2;
    }
    ctx->key_len = strlen(ctx->key_name);
    if (ctx->key_len < 8)
        ctx->key_len = 8;

    if (comm) {
        len = strlen(comm);
        for (i = 0; i < len; i++)
            comm[i] = (char)toupper(comm[i]);
        ctx->comm = comm;
    } else
        ctx->comm = strdup("COMM");

    ctx->only_comm = env->only_comm;
    if (ctx->only_comm && (ctx->show_comm == 0 ||
        (ctx->show_comm == 1 && ctx->tp_list->nr_comm != ctx->tp_list->nr_real_tp))) {
        fprintf(stderr, "--only-comm requires all events to have a 'comm' attr\n");
        goto failed;
    }

    rblist__init(&ctx->top_list);
    ctx->top_list.node_cmp = top_info_node_cmp;
    ctx->top_list.node_new = top_info_node_new;
    ctx->top_list.node_delete = top_info_node_delete;

    ctx->altwin = false;

    if (env->interval == 0)
        env->interval = 1000;

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct top_ctx *ctx = dev->private;
    if (ctx->altwin) altwin_end();
    rblist__exit(&ctx->top_list);
    if (ctx->fields) free(ctx->fields);
    if (ctx->EVENT) free(ctx->EVENT);
    if (ctx->key_name) free(ctx->key_name);
    if (ctx->comm) free(ctx->comm);
    tp_list_free(ctx->tp_list);
    tep__unref();
    free(ctx);
}

static int top_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct top_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD |
                         PERF_SAMPLE_RAW,
        .read_format   = PERF_FORMAT_ID,
        .pinned        = 1,
        .disabled      = 1,
        .watermark     = 1,
        .wakeup_watermark = (dev->pages << 12) / 2,
    };
    struct perf_evsel *evsel;
    struct tp *tp;
    int i;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    prof_dev_env2attr(dev, &attr);

    for_each_real_tp(ctx->tp_list, tp, i) {
        evsel = tp_evsel_new(tp, &attr);
        if (!evsel)
            goto failed;

        perf_evlist__add(evlist, evsel);
    }

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int top_filter(struct prof_dev *dev)
{
    struct top_ctx *ctx = dev->private;
    return tp_list_apply_filter(dev, ctx->tp_list);
}

static void top_interval(struct prof_dev *dev);
static void top_exit(struct prof_dev *dev)
{
    struct top_ctx *ctx = dev->private;
    if (!ctx->altwin)
        top_interval(dev);
    monitor_ctx_exit(dev);
}

static void top_sigwinch(struct prof_dev *dev)
{
    struct top_ctx *ctx = dev->private;
    bool altwin = false;

    if (dev->tty.istty && !dev->tty.shared) {
        int printed = 0;
        int i;
        if (!ctx->only_comm)
            printed += ctx->key_len + 1;
        for (i = 0; i < ctx->nr_fields; i++)
            printed += ctx->fields[i].len + 1;
        if (ctx->show_comm)
            printed += TASK_COMM_LEN;

        if (printed < dev->tty.col)
            altwin = true;
    }
    if (altwin != ctx->altwin) {
        ctx->altwin = altwin;
        if (ctx->altwin) altwin_new();
        else altwin_end();
    }
}

static void top_enabled(struct prof_dev *dev)
{
    struct top_ctx *ctx = dev->private;
    top_sigwinch(dev);
    if (ctx->altwin)
        top_interval(dev);
}

static void top_sigusr(struct prof_dev *dev, int signum)
{
    struct top_ctx *ctx = dev->private;
    if (signum == SIGWINCH) {
        if (ctx->altwin) {
            top_sigwinch(dev);
            top_interval(dev);
        }
    }
}

// in linux/perf_event.h
// PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_FORMAT_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW
struct sample_type_raw {
    struct {
        __u32    pid;
        __u32    tid;
    }    tid_entry;
    __u64   time;
    __u64   id;
    struct {
        __u32    cpu;
        __u32    reserved;
    }    cpu_entry;
    __u64       period;
    struct {
        __u32   size;
        __u8    data[0];
    } raw;
};

static long top_ftrace_filter(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    struct top_ctx *ctx = dev->private;
    struct sample_type_raw *raw = (void *)event->sample.array;
    struct expr_global *glo = GLOBAL(raw->cpu_entry.cpu, raw->tid_entry.pid, raw->raw.data, raw->raw.size);
    return tp_list_ftrace_filter(dev, ctx->tp_list, glo);
}

static void top_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    struct top_ctx *ctx = dev->private;
    struct sample_type_raw *raw = (void *)event->sample.array;
    struct perf_evsel *evsel = NULL;
    void *data = raw->raw.data;
    int size = raw->raw.size;
    struct expr_global *glo = GLOBAL(raw->cpu_entry.cpu, raw->tid_entry.pid, data, size);
    struct tp *tp = NULL;
    struct tp *tmp;
    unsigned long key;
    char *comm = NULL;
    int field = 0;
    int i;
    struct top_info info;
    struct rb_node *rbn;
    struct top_info *p;

    evsel = perf_evlist__id_to_evsel(dev->evlist, raw->id, NULL);
    for_each_real_tp(ctx->tp_list, tmp, i) {
        if (evsel == tmp->evsel) {
            tp = tmp;
            break;
        } else
            field += tmp->nr_top;
    }

    if (unlikely(tp == NULL))
        return;

    if (dev->env->verbose >= VERBOSE_EVENT) {
        if (dev->print_title) {
            prof_dev_print_time(dev, raw->time, stdout);
            tp_print_marker(tp);
        }
        tp_print_event(tp, raw->time, raw->cpu_entry.cpu, data, size);
    }

    tp_broadcast_event(tp, event);

    /*
     * Simplified rules (no mixed key scenarios allowed):
     * - Either all events have key attr, or none do (enforced at init)
     * - X marks forbidden combinations (mixed key scenarios)
     *
     * show_comm states:
     * - 0: no comm display (all events have key, no events have comm)
     * - 1: comm from ATTR only (all events have key, any event has comm)
     * - 2: comm from ATTR or pid->comm (no events have key)
     *
     * For any event:
     *   ATTR      |
     * key   comm  |  PID/KEY              COMM
     * 0     0     |  raw->tid_entry.tid   pid->comm
     * 0     1     |  raw->tid_entry.tid   comm ATTR
     * 1     0     |  key ATTR             (no comm)
     * 1     1     |  key ATTR             comm ATTR
     *
     *
     * All possible scenarios with multiple events:
     *
     *   event1      event2    |              |
     * key   comm  key   comm  |  key   comm  |  show_comm  PID/KEY        E1  E2  COMM
     * 0     0     0     0     |  0     0     |  2          tid_entry.tid          pid->comm
     *             0     1     |  0     1     |  2          tid_entry.tid          pid->comm/commATTR
     *             1     0     |  X           |             X  (mixed key not allowed)
     *             1     1     |  X           |             X  (mixed key not allowed)
     * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -  - - - - - - - - - - - - - -
     *   event1      event2    |              |
     * key   comm  key   comm  |  key   comm  |  show_comm  PID/KEY        E1  E2  COMM
     *             0     0     |  0     1     |  2          tid_entry.tid          pid->comm/commATTR
     * 0     1     0     1     |  0     1     |  2          tid_entry.tid          commATTR
     *             1     0     |  X           |             X  (mixed key not allowed)
     *             1     1     |  X           |             X  (mixed key not allowed)
     * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -  - - - - - - - - - - - - - -
     *   event1      event2    |              |
     * key   comm  key   comm  |  key   comm  |  show_comm  PID/KEY        E1  E2  COMM
     *             0     0     |  X           |             X  (mixed key not allowed)
     *             0     1     |  X           |             X  (mixed key not allowed)
     * 1     0     1     0     |  1     0     |  0          key ATTR               (no comm)
     *             1     1     |  1     1     |  1          key ATTR               commATTR (not only_comm)
     * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -  - - - - - - - - - - - - - -
     *   event1      event2    |              |
     * key   comm  key   comm  |  key   comm  |  show_comm  PID/KEY        E1  E2  COMM
     *             0     0     |  X           |             X  (mixed key not allowed)
     *             0     1     |  X           |             X  (mixed key not allowed)
     *             1     0     |  1     1     |  1          key ATTR               commATTR (not only_comm)
     * 1     1     1     1     |  1     1     |  1          key ATTR               commATTR
     * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -  - - - - - - - - - - - - - -
     *
     * Possible combinations and requirements:
     * key ATTR                key must have the same meaning across all events
     * commATTR                commATTR must have the same meaning across all events
     * pid->comm/commATTR      commATTR must have the meaning of process name
     */
    if (tp->key_prog)
        key = tp_get_key(tp, glo);
    else {
        key = raw->tid_entry.tid;
        // raw->tid_entry.pid may be -1, when process exits.
        if (key == (u32)-1)
            return;
    }

    if (ctx->show_comm) {
        if (tp->comm_prog)
            comm = tp_get_comm(tp, glo);
        else if (ctx->show_comm == 2) {
            // show_comm==2: key has PID meaning, use pid->comm mapping.
            tep__update_comm(NULL, (int)key);
            comm = (void *)tep__pid_to_comm((int)key);
        }
    }

    if (ctx->only_comm) {
        info.key = 0;
        info.pcomm = comm ? : (char *)"NULL";
    } else {
        info.key = key;
        info.pcomm = NULL;
    }

    rbn = rblist__findnew(&ctx->top_list, &info);
    if (!rbn)
        return;

    p = container_of(rbn, struct top_info, rbnode);
    for (i = 0; i < tp->nr_top; i++, field++) {
        if (!tp->top_add[i].event)
            p->counter[field] += (unsigned long)tp_prog_run(tp, tp->top_add[i].field_prog, glo);
        else
            p->counter[field] += 1;
    }

    if (!ctx->only_comm && comm && !p->pcomm) {
        if (ctx->show_comm == 1 && !tp->comm_prog)
            goto done;

        if (strlen(comm) < TASK_COMM_LEN) {
            p->pcomm = p->comm;
            strcpy(p->pcomm, comm);
        } else {
            p->pcomm = strdup(comm);
            if (!p->pcomm) {
                p->pcomm = p->comm;
                strncpy(p->pcomm, comm, TASK_COMM_LEN - 1);
                p->pcomm[TASK_COMM_LEN - 1] = '\0';
            }
        }
    }

done:
    ctx->nr_events ++;
}

static inline int top_print_time(const char *fmt)
{
    char timebuff[64];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    strftime(timebuff, sizeof(timebuff), "%H:%M:%S", localtime(&tv.tv_sec));
    return printf(fmt, timebuff);
}

static inline void top_print_title(struct prof_dev *dev)
{
    struct top_ctx *ctx = dev->private;
    int printed;
    int i;

    if (!ctx->altwin && rblist__empty(&ctx->top_list))
        return;

    if (ctx->altwin) altwin_title_begin();
    else print_time(stdout);

    printed = top_print_time("perf-prof - %s  ");
    printed += printf("sample %lu events", ctx->nr_events);
    printf("%*s\n", ctx->altwin && dev->tty.col>printed ? dev->tty.col-printed : 0, "");

    // PID FIELD FIELD ... COMM
    printed = 0;
    if (!ctx->only_comm)
        printed += printf("%*s ", ctx->key_len, ctx->key_name);
    for (i = 0; i < ctx->nr_fields; i++)
        printed += printf("%*s ", ctx->fields[i].len, ctx->fields[i].field);
    if (ctx->show_comm)
        printed += printf("%s", ctx->comm);
    printf("%*s\n", ctx->altwin && dev->tty.col>printed ? dev->tty.col-printed : 0, "");

    if (ctx->altwin) altwin_title_end();
}

static void top_interval(struct prof_dev *dev)
{
    struct top_ctx *ctx = dev->private;
    struct rb_node *rbn;
    struct top_info *t;
    struct rblist sorted;
    struct tmp_entry tmp;
    int row = 3;
    int i;

    top_print_title(dev);

    //pid_list is empty still print header
    if (rblist__empty(&ctx->top_list))
        return;

    rblist__init(&sorted);
    sorted.node_cmp = top_info_sorted_node_cmp;
    sorted.node_new = top_info_sorted_node_new;
    sorted.node_delete = ctx->top_list.node_delete;
    ctx->top_list.node_delete = top_info_node_delete_empty; //empty, not really delete

    /* sort, remove from `ctx->pid_list', add to `sorted'. */
    tmp.ctx = ctx;
    do {
        rbn = rblist__entry(&ctx->top_list, 0);
        tmp.info = container_of(rbn, struct top_info, rbnode);
        rblist__remove_node(&ctx->top_list, rbn);
        rblist__add_node(&sorted, &tmp);
    } while (!rblist__empty(&ctx->top_list));

    do {
        rbn = rblist__entry(&sorted, 0);

        if (!ctx->altwin || !dev->tty.row || ++row < dev->tty.row) {
            t = container_of(rbn, struct top_info, rbnode);
            if (!ctx->only_comm) {
                if (ctx->tp_printkey) {
                    int ret = tp_print_key(ctx->tp_printkey, t->key);
                    if (ret > 0 && ctx->key_len+1 > ret)
                        printf("%-*s ", ctx->key_len - ret, "");
                } else
                    printf("%*lu ", ctx->key_len, t->key);
            }
            for (i = 0; i < ctx->nr_fields; i++)
                printf("%*lu ", ctx->fields[i].len, t->counter[i]);
            if (ctx->show_comm)
                printf("%-s", t->pcomm);
            printf("\n");
        }

        rblist__remove_node(&sorted, rbn);
    } while (!rblist__empty(&sorted));

    ctx->top_list.node_delete = sorted.node_delete;
}

static void top_help(struct help_ctx *hctx)
{
    struct env *env = hctx->env;
    int i, j, k;
    bool top_by, top_add;
    bool has_key = false;

    printf(PROGRAME " top ");
    printf("-e \"");
    for (i = 0; i < hctx->nr_list; i++) {
        struct tp *tp;
        for_each_real_tp(hctx->tp_list[i], tp, j) {
            printf("%s:%s/%s/", tp->sys, tp->name, tp->filter&&tp->filter[0]?tp->filter:".");
            top_by = false;
            top_add = false;
            if (tp->alias)
                printf("alias=%s/", tp->alias);
            if (tp->nr_top) {
                for (k = 0; k < tp->nr_top; k++)
                    if (!tp->top_add[k].event) {
                        if (tp->top_add[k].top_by) {
                            top_by = true;
                            printf("top-by=%s/", tp->top_add[k].field?:".");
                        } else {
                            top_add = true;
                            printf("top-add=%s/", tp->top_add[k].field?:".");
                        }
                    }
            }
            if (tp->key) {
                has_key = true;
                printf("key=%s/", tp->key);
            }
            if (tp->comm) {
                printf("comm=%s/", tp->comm);
            }
            if (!tp->alias || !top_by || !top_add || !tp->key || !tp->comm)
                printf("[");
            if (!tp->alias)
                printf("alias=./");
            if (!top_by)
                printf("top-by=./");
            if (!top_add)
                printf("top-add=./");
            if (!tp->key)
                printf("key=./");
            if (!tp->comm)
                printf("comm=./");
            if (!tp->alias || !top_by || !top_add || !tp->key || !tp->comm)
                printf("]");
            if (i != hctx->nr_list - 1 ||
                j != hctx->tp_list[i]->nr_tp - 1)
                printf(",");
        }
    }
    printf("\" ");

    if (env->key)
        printf("-k %s ", env->key);
    common_help(hctx, true, true, true, true, false, true, true);

    if (!env->key && !has_key)
        printf("[-k .] ");
    common_help(hctx, false, true, true, true, false, true, true);
    printf("\n");
}


static const char *top_desc[] = PROFILER_DESC("top",
    "[OPTION...] -e EVENT[...] [-i INT] [-k key] [--only-comm]",
    "Display key-value statistical in top mode.", "",
    "SYNOPSIS",
    "    A multi-dimensional key-value statistics tool that samples events to build",
    "    a (key, [values], name) statistical matrix and displays them in top mode,",
    "    sorted by specified columns.",
    "",
    "    Core components:",
    "    - key:    Row identifier, aggregation key. Acts like SQL GROUP BY.",
    "              Default: tid (thread ID). Use key=EXPR or -k EXPR.",
    "    - values: Cumulative statistics, similar to SQL SELECT SUM(EXPR):",
    "              * top-by=EXPR:  SELECT SUM(EXPR) ORDER BY ... DESC (sort by)",
    "              * top-add=EXPR: SELECT SUM(EXPR) (display only)",
    "              * Default:      SELECT COUNT(*) (event count) when neither top-by",
    "                              nor top-add specified",
    "    - name:   Readable name for key. Set with comm=EXPR or auto pid->comm.",
    "",
    "    Sorting priority: top-by columns (by order), then top-add + default columns.",
    "",
    "    For multiple events, key ATTR must have the same meaning across all events.",
    "    Use printkey=EXPR to customize key display format.",
    "",
    "    For each event, multiple top-by and top-add ATTR can be specified.",
    "",
    "EXAMPLES",
    "    "PROGRAME" top -e kvm:kvm_exit//key=exit_reason/ -i 1000",
    "    "PROGRAME" top -e irq:irq_handler_entry//key=irq/ -C 0",
    "    "PROGRAME" top -e 'sched:sched_stat_runtime//key=pid/comm=comm/top-by=\"runtime/1000\"/alias=run(us)/' -C 0 -i 1000",
    "    "PROGRAME" top -e sched:sched_stat_runtime//key=pid/comm=comm/top-by=runtime/,sched:sched_switch//key=prev_pid/comm=prev_comm/ -C 0 -i 1000",
    "    "PROGRAME" top -e 'sched:sched_process_exec//key=pid/comm=filename/' --only-comm",
    "    "PROGRAME" top -e 'workqueue:workqueue_execute_start//key=common_pid/alias=NUM/comm=ksymbol(function)/' --only-comm",
    "    "PROGRAME" top -e 'irq:softirq_entry//key=(_cpu<<32)|vec/printkey=printf(\"  %03d      %d\", key>>32, (int)key)/'",
    "    "PROGRAME" top -e 'skb:kfree_skb//key=protocol/comm=ksymbol(location)/' -m 32");
static const char *top_argv[] = PROFILER_ARGV("top",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_PROFILER, "event", "key", "only-comm");
static profiler top = {
    .name = "top",
    .desc = top_desc,
    .argv = top_argv,
    .pages = 4,
    .help = top_help,
    .init = top_init,
    .filter = top_filter,
    .enabled = top_enabled,
    .deinit = top_exit,
    .sigusr = top_sigusr,
    .interval = top_interval,
    .ftrace_filter = top_ftrace_filter,
    .sample = top_sample,
};
PROFILER_REGISTER(top)


