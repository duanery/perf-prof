// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <linux/rblist.h>
#include <internal/threadmap.h>
#include <monitor.h>
#include <trace_helpers.h>

/*
 * The values of one field, 8-byte signed or unsigned, shared by the filters
 * on them. They are kept by an order key: a signed value with its sign bit
 * flipped compares as unsigned, the ranges below are of keys alike.
 */
struct value_set {
    struct rblist values;
    bool is_signed;
    int refcnt;
    u64 dels;   // deletions, see __event_filter_exact()
};

struct value_node {
    struct rb_node rb_node;
    u64 key;
};

static inline u64 value_key(struct value_set *set, s64 value)
{
    return set->is_signed ? (u64)value ^ (1ULL << 63) : (u64)value;
}

static inline s64 key_value(struct value_set *set, u64 key)
{
    return set->is_signed ? (s64)(key ^ (1ULL << 63)) : (s64)key;
}

static int value_node__cmp(struct rb_node *rbn, const void *entry)
{
    struct value_node *node = container_of(rbn, struct value_node, rb_node);
    u64 key = *(const u64 *)entry;

    return node->key < key ? -1 : node->key > key;
}

static struct rb_node *value_node__new(struct rblist *rlist __maybe_unused, const void *entry)
{
    struct value_node *node = malloc(sizeof(*node));

    if (!node)
        return NULL;
    RB_CLEAR_NODE(&node->rb_node);
    node->key = *(const u64 *)entry;
    return &node->rb_node;
}

static void value_node__delete(struct rblist *rblist __maybe_unused, struct rb_node *rbn)
{
    free(container_of(rbn, struct value_node, rb_node));
}

static struct value_set *value_set_new(bool is_signed)
{
    struct value_set *set = zalloc(sizeof(*set));

    if (!set)
        return NULL;
    rblist__init(&set->values);
    set->values.node_cmp = value_node__cmp;
    set->values.node_new = value_node__new;
    set->values.node_delete = value_node__delete;
    set->is_signed = is_signed;
    set->refcnt = 1;
    return set;
}

static struct value_set *value_set_get(struct value_set *set)
{
    set->refcnt++;
    return set;
}

static void value_set_put(struct value_set *set)
{
    if (set && --set->refcnt == 0) {
        rblist__exit(&set->values);
        free(set);
    }
}

/* 1 if @value was added, 0 if it was there already. */
static int value_set_add(struct value_set *set, s64 value)
{
    u64 key = value_key(set, value);
    int err = rblist__add_node(&set->values, &key);

    if (err == -EEXIST)
        return 0;
    return err < 0 ? err : 1;
}

/* 1 if @value was there. */
static int value_set_del(struct value_set *set, s64 value)
{
    u64 key = value_key(set, value);

    if (rblist__find_remove(&set->values, &key) != 1)
        return 0;
    set->dels++;
    return 1;
}

static bool value_set_has(struct value_set *set, s64 value)
{
    u64 key = value_key(set, value);

    return rblist__find(&set->values, &key) != NULL;
}

/*
 * The ranges of keys a filter lets in, ascending, for the length limit: a
 * tracepoint filter is at most a page, the values one by one are over it
 * soon. Consecutive values make a range, the smallest gaps are merged to fit,
 * see value_ranges_new(). @open: the last one has no upper bound, field>=lo;
 * its hi is ULLONG_MAX.
 */
struct value_ranges {
    int nr;
    struct value_range {
        u64 lo, hi;
        bool open;
    } r[];
};

/* field==a, (field>=a&&field<=b), field>=a */
static int value_range_print(char *buf, size_t len, struct value_set *set,
                             const struct value_range *r, const char *field)
{
    const char *fmt = set->is_signed ? "%lld" : "%llu";
    char lo[24], hi[24];

    snprintf(lo, sizeof(lo), fmt, key_value(set, r->lo));
    if (r->open)
        return snprintf(buf, len, "%s>=%s", field, lo);
    if (r->lo == r->hi)
        return snprintf(buf, len, "%s==%s", field, lo);
    snprintf(hi, sizeof(hi), fmt, key_value(set, r->hi));
    return snprintf(buf, len, "(%s>=%s&&%s<=%s)", field, lo, field, hi);
}

/* The gap between @a and the next @b, at least 1. */
static inline u64 value_range_gap(const struct value_range *a, const struct value_range *b)
{
    return b->lo - a->hi - 1;
}

/* The filter length once every gap <= @gap is merged; ranges joined by "||". */
static size_t value_ranges_len(const struct value_ranges *ranges, struct value_set *set,
                               const char *field, u64 gap)
{
    struct value_range r;
    size_t len = 0;
    int i;

    if (ranges->nr == 0)
        return snprintf(NULL, 0, "%s<0", field);
    r = ranges->r[0];
    for (i = 1; i < ranges->nr; i++) {
        if (value_range_gap(&r, &ranges->r[i]) <= gap) {
            r.hi = ranges->r[i].hi;
            r.open = ranges->r[i].open;
            continue;
        }
        len += value_range_print(NULL, 0, set, &r, field) + 2;
        r = ranges->r[i];
    }
    return len + value_range_print(NULL, 0, set, &r, field);
}

static void value_ranges_merge(struct value_ranges *ranges, u64 gap)
{
    int i, n = 0;

    for (i = 1; i < ranges->nr; i++) {
        if (value_range_gap(&ranges->r[n], &ranges->r[i]) <= gap) {
            ranges->r[n].hi = ranges->r[i].hi;
            ranges->r[n].open = ranges->r[i].open;
        } else
            ranges->r[++n] = ranges->r[i];
    }
    if (ranges->nr)
        ranges->nr = n + 1;
}

/*
 * Cover the values of @set and, if @open, the ones from @next_value on: up
 * to the next range, every one from @next_value on when there is none above
 * it. The gaps are merged, smallest first, until the filter on @field fits
 * in @budget bytes; with no gap left it is taken as it is.
 */
static struct value_ranges *value_ranges_new(struct value_set *set, bool open, s64 next_value,
                                             const char *field, size_t budget)
{
    int nr = rblist__nr_entries(&set->values);
    struct value_ranges *ranges;
    struct rb_node *rbn;
    int i, n;

    ranges = malloc(sizeof(*ranges) + (nr + 1) * sizeof(struct value_range));
    if (!ranges)
        return NULL;

    // Consecutive values make a range.
    ranges->nr = 0;
    for (rbn = rb_first_cached(&set->values.entries); rbn; rbn = rb_next(rbn)) {
        u64 key = container_of(rbn, struct value_node, rb_node)->key;
        struct value_range *last = ranges->nr ? &ranges->r[ranges->nr - 1] : NULL;

        if (last && key == last->hi + 1)
            last->hi = key;
        else
            ranges->r[ranges->nr++] = (struct value_range){key, key, false};
    }

    /*
     * The tail: the values come upwards from it, up to the next range. Above
     * all of the ranges it is open; after a wrap it is not, the ranges above
     * stay as they are.
     */
    if (open) {
        struct value_range t = {value_key(set, next_value), ULLONG_MAX, true};

        for (i = 0; i < ranges->nr; i++) {
            if (ranges->r[i].lo > t.lo) {
                t.hi = ranges->r[i].lo - 1;
                t.open = false;
                break;
            }
        }
        // Insert at i, then join the overlapping and adjacent neighbours.
        memmove(&ranges->r[i + 1], &ranges->r[i], (ranges->nr - i) * sizeof(t));
        ranges->r[i] = t;
        ranges->nr++;
        n = 0;
        for (i = 1; i < ranges->nr; i++) {
            if (ranges->r[n].open || ranges->r[i].lo <= ranges->r[n].hi + 1) {
                if (ranges->r[i].hi > ranges->r[n].hi)
                    ranges->r[n].hi = ranges->r[i].hi;
                ranges->r[n].open |= ranges->r[i].open;
            } else
                ranges->r[++n] = ranges->r[i];
        }
        ranges->nr = n + 1;
    }

    // The smallest gap that fits: the length does not grow with the gap.
    if (ranges->nr > 1 && value_ranges_len(ranges, set, field, 0) > budget) {
        u64 lo = 1, hi = ranges->r[ranges->nr - 1].lo - ranges->r[0].hi;

        while (lo < hi) {
            u64 mid = lo + (hi - lo) / 2;
            if (value_ranges_len(ranges, set, field, mid) > budget)
                lo = mid + 1;
            else
                hi = mid;
        }
        value_ranges_merge(ranges, lo);
    }
    return ranges;
}

static bool value_ranges_has(const struct value_ranges *ranges, u64 key)
{
    int lo = 0, hi = ranges ? ranges->nr - 1 : -1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (key < ranges->r[mid].lo)
            hi = mid - 1;
        else if (key > ranges->r[mid].hi)
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

/* No open tail, no merged gap: the ranges cover the values of @set only. */
static bool value_ranges_exact(const struct value_ranges *ranges, struct value_set *set)
{
    u64 nr = rblist__nr_entries(&set->values), covered = 0;
    int i;

    for (i = 0; i < ranges->nr; i++) {
        if (ranges->r[i].open || ranges->r[i].hi - ranges->r[i].lo >= nr)
            return false;
        covered += ranges->r[i].hi - ranges->r[i].lo + 1;
    }
    return covered == nr;
}

// Return an allocated string, which needs to be freed. No range: field<0.
static char *value_ranges_filter(const struct value_ranges *ranges, struct value_set *set,
                                 const char *field)
{
    size_t len = value_ranges_len(ranges, set, field, 0) + 1, off = 0;
    char *filter = malloc(len);
    int i;

    if (!filter)
        return NULL;
    if (ranges->nr == 0)
        snprintf(filter, len, "%s<0", field);
    for (i = 0; i < ranges->nr; i++) {
        if (i)
            off += snprintf(filter + off, len - off, "||");
        off += value_range_print(filter + off, len - off, set, &ranges->r[i], field);
    }
    return filter;
}

/*
 * The event filter on @field over @values: "(and_filter) && (ranges)",
 * @ranges is what it lets in, a superset of @values once gaps are merged.
 */
struct event_filter {
    char *field;
    char *and_filter;               // NULL: none
    struct value_set *values;       // shared by the filters on the same values
    struct value_ranges *ranges;    // what @filter lets in
    char *filter;                   // NULL: not built yet
    bool exact;                     // @ranges was @values only when built
    u64 dels;                       // values->dels when built
};

/* @share: share its values; NULL: new ones, of @is_signed 8-byte values. */
static int event_filter_init(struct event_filter *ef, const char *field, const char *and_filter,
                             bool is_signed, struct value_set *share)
{
    ef->values = share ? value_set_get(share) : value_set_new(is_signed);
    ef->field = strdup(field);
    ef->and_filter = and_filter ? strdup(and_filter) : NULL;
    if (!ef->values || !ef->field || (and_filter && !ef->and_filter))
        return -ENOMEM;
    return 0;
}

static void event_filter_exit(struct event_filter *ef)
{
    value_set_put(ef->values);
    free(ef->field);
    free(ef->and_filter);
    free(ef->ranges);
    free(ef->filter);
}

/*
 * Make the filter over the values now, and the values from @next_value on if
 * @open, see value_ranges_new(). Both allocated, to be given to
 * event_filter_set() or freed.
 */
static int event_filter_make(struct event_filter *ef, bool open, s64 next_value,
                             struct value_ranges **pranges, char **pfilter)
{
    struct value_ranges *ranges;
    long budget = sysconf(_SC_PAGESIZE) - 1;
    char *values, *filter = NULL;

    // The whole filter fits in a page, see perf_event_set_filter(). "(and_filter) && ()"
    if (ef->and_filter)
        budget -= strlen(ef->and_filter) + 8;
    ranges = value_ranges_new(ef->values, open, next_value, ef->field, budget > 0 ? budget : 0);
    if (!ranges)
        return -ENOMEM;
    values = value_ranges_filter(ranges, ef->values, ef->field);
    if (values && ef->and_filter) {
        if (asprintf(&filter, "(%s) && (%s)", ef->and_filter, values) < 0)
            filter = NULL;
        free(values);
    } else
        filter = values;
    if (!filter) {
        free(ranges);
        return -ENOMEM;
    }
    *pranges = ranges;
    *pfilter = filter;
    return 0;
}

/* @filter, made with @ranges, is the one now. */
static void event_filter_set(struct event_filter *ef, struct value_ranges *ranges, char *filter)
{
    free(ef->ranges);
    free(ef->filter);
    ef->ranges = ranges;
    ef->filter = filter;
    ef->exact = value_ranges_exact(ranges, ef->values);
    ef->dels = ef->values->dels;
}

/*
 * A deletion of the shared values counts in values->dels: each filter on
 * them finds it there, none has to tell the others.
 */
static bool __event_filter_exact(struct event_filter *ef)
{
    return ef->filter && ef->exact && ef->dels == ef->values->dels;
}

struct event_filter *event_filter_new(const char *field, const char *and_filter, bool is_signed)
{
    struct event_filter *ef = zalloc(sizeof(*ef));

    if (ef && event_filter_init(ef, field, and_filter, is_signed, NULL) < 0) {
        event_filter_free(ef);
        return NULL;
    }
    return ef;
}

void event_filter_free(struct event_filter *ef)
{
    if (!ef)
        return;
    event_filter_exit(ef);
    free(ef);
}

int event_filter_add(struct event_filter *ef, s64 value)
{
    return value_set_add(ef->values, value);
}

int event_filter_del(struct event_filter *ef, s64 value)
{
    return value_set_del(ef->values, value);
}

bool event_filter_has(struct event_filter *ef, s64 value)
{
    return value_set_has(ef->values, value);
}

const char *event_filter_build(struct event_filter *ef)
{
    struct value_ranges *ranges;
    char *filter;

    if (event_filter_make(ef, false, 0, &ranges, &filter) < 0)
        return NULL;
    event_filter_set(ef, ranges, filter);
    return ef->filter;
}

bool event_filter_exact(struct event_filter *ef)
{
    return __event_filter_exact(ef);
}

/*
 * pid_filter: set the kernel filter late, for threads created and exiting
 * fast, and decide when to sync it with the kernel.
 *
 * Each sync replaces the evsel, a fd on every cpu, and many threads are over
 * a page, the gaps merged: setting the filter on each thread is too costly,
 * and it is not exact anyway. Let in more instead: the open tail for the
 * threads to come, the exited threads for a while. The kernel drops most of
 * the records, pid_filter_match() the rest; the dropped ones tell how much
 * more it lets in, too many tighten it.
 *
 * Two sets of pids:
 *  - followed: ef.values, the threads followed now, changed at once by
 *    pid_filter_add() / pid_filter_del(). Records are matched against it.
 *  - kernel: ef.ranges, a copy of what the kernel filter (ef.filter) lets
 *    in: the merged gaps, the open tail, the exited threads. It changes only
 *    once ops->set() has put a new filter in the kernel, then it is the new
 *    filter's alone: while the old and new evsels both write,
 *    prof_dev_replace_dup() of the framework drops the duplicates, nothing
 *    of the old filter is left to the kernel set.
 * The kernel set follows the followed one: at once for a new thread it does
 * not let in, later when it lets in more (tightened).
 *
 * The open tail lets in the pids allocated from @tail on, the threads to come
 * need no sync each; after a pid wrap it ends at the next followed pid.
 *
 * The sync is due, by urgency:
 *  - a followed thread is not let in (@uncovered), or DROPS records let in
 *    are not followed (@dropped): at once;
 *  - ADD_DELAY: no new thread for this long, the open tail is not needed:
 *    each new thread puts it off (@add_due);
 *  - DEL_DELAY: an exited thread is still let in, pid_filter_match() drops
 *    it meanwhile: each exit puts it off (@del_due), DEL_MAX after the first
 *    (@del_first) at most.
 * At most once an INTERVAL: a replacement opens a fd on every cpu. The timer
 * fires out of event processing.
 */
#define PID_FILTER_DROPS      1000
#define PID_FILTER_INTERVAL   NSEC_PER_SEC
#define PID_FILTER_ADD_DELAY  NSEC_PER_SEC
#define PID_FILTER_DEL_DELAY  (10 * NSEC_PER_SEC)
#define PID_FILTER_DEL_MAX    (60 * NSEC_PER_SEC)

struct pid_filter {
    struct event_filter ef;
    const struct pid_filter_ops *ops;
    void *opaque;
    struct timer timer;
    u64 timer_at;               // ktime the timer fires, 0: not started
    bool started;               // pid_filter_start() done
    bool need;                  // the last ops->ftrace_filter()
    bool tail_open;
    pid_t tail;
    pid_t last;                 // added last
    bool uncovered;             // a followed pid is not in the kernel set
    u64 synced;                 // ktime of the last sync, for the rate limit
    u64 add_due;                // ktime the tail may close, 0: none
    u64 del_due, del_first;     // exited pids let in: ktime of the sync, of the first exit; 0: none
    u64 dropped;                // pid_filter_match() failed since the last sync
};

static void pid_filter_timer(struct timer *timer);

struct pid_filter *pid_filter_new(const char *field, const char *and_filter, struct pid_filter *share,
                                  const struct pid_filter_ops *ops, void *opaque)
{
    struct pid_filter *pf = zalloc(sizeof(*pf));

    if (!pf)
        return NULL;
    pf->timer.fd = -1;
    pf->ops = ops;
    pf->opaque = opaque;
    if (event_filter_init(&pf->ef, field, and_filter, true, share ? share->ef.values : NULL) < 0 ||
        timer_init(&pf->timer, 1, pid_filter_timer) < 0) {
        pid_filter_free(pf);
        return NULL;
    }
    return pf;
}

void pid_filter_free(struct pid_filter *pf)
{
    if (!pf)
        return;
    if (pf->timer.fd >= 0)
        timer_destroy(&pf->timer);
    event_filter_exit(&pf->ef);
    free(pf);
}

/* ktime the sync is due, 0: none. */
static u64 pid_filter_due(struct pid_filter *pf)
{
    u64 due = 0;

    if (!pf->started)
        return 0;
    // The most urgent: the earliest.
    if (pf->uncovered || pf->dropped >= PID_FILTER_DROPS)
        due = pf->synced;
    if (pf->tail_open && pf->add_due && (!due || pf->add_due < due))
        due = pf->add_due;
    if (pf->del_due && (!due || pf->del_due < due))
        due = pf->del_due;
    if (due && due < pf->synced + PID_FILTER_INTERVAL)
        due = pf->synced + PID_FILTER_INTERVAL;
    return due;
}

/* Fire when due, at least 1ms on: 0 stops a timerfd. */
static void pid_filter_schedule(struct pid_filter *pf)
{
    u64 at = pid_filter_due(pf), now;

    if (at == pf->timer_at)
        return;
    pf->timer_at = at;
    if (!at) {
        timer_cancel(&pf->timer);
        return;
    }
    now = get_ktime_ns();
    timer_start(&pf->timer, at > now + NSEC_PER_MSEC ? at - now : NSEC_PER_MSEC, true);
}

/* The kernel set lets in more than the followed one: records need pid_filter_match(). */
static void pid_filter_notify(struct pid_filter *pf)
{
    bool need = pf->started && !__event_filter_exact(&pf->ef);

    if (need == pf->need)
        return;
    pf->need = need;
    if (pf->ops->ftrace_filter)
        pf->ops->ftrace_filter(pf, pf->opaque, need);
}

/*
 * The pid allocated last, from /proc/loadavg. It counts in the pid namespace
 * of perf-prof, the tracepoints report the pids of the init one: -1 when not
 * there, or unknown.
 */
static pid_t pid_filter_last_pid(void)
{
    char line[256];
    int pid = -1;
    FILE *fp;

    fp = fopen("/proc/self/status", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            int a, b;
            // NSpid: 1234 1 - a nested pid namespace.
            if (!strncmp(line, "NSpid:", 6) && sscanf(line + 6, "%d %d", &a, &b) == 2) {
                fclose(fp);
                return -1;
            }
        }
        fclose(fp);
    }
    fp = fopen("/proc/loadavg", "r");
    if (fp) {
        if (fscanf(fp, "%*s %*s %*s %*s %d", &pid) != 1)
            pid = -1;
        fclose(fp);
    }
    return pid;
}

/*
 * Sync the kernel set to the followed one, the tail as due: open while new
 * threads come; moved on to the pid allocated last when too many are
 * dropped; closed once no new thread came for ADD_DELAY. The same filter as
 * the kernel's is not set again. On error the kernel set stays, the sync is
 * due again an INTERVAL on.
 */
static int pid_filter_sync(struct pid_filter *pf, bool initial)
{
    u64 now = get_ktime_ns();
    bool open = pf->tail_open;
    pid_t tail = pf->tail;
    struct value_ranges *ranges = NULL;
    char *filter = NULL;
    int err;

    if (open) {
        if (!pf->add_due || pf->add_due <= now)
            // No new thread for ADD_DELAY: the followed threads only.
            open = false;
        else if (!pf->uncovered && pf->dropped >= PID_FILTER_DROPS) {
            // Too many let in: the pids allocated so far are not followed.
            pid_t last = pid_filter_last_pid();
            tail = (last > 0 ? last : pf->last) + 1;
        }
    }
    err = event_filter_make(&pf->ef, open, tail, &ranges, &filter);
    if (err == 0 && (!pf->ef.filter || strcmp(filter, pf->ef.filter)))
        err = pf->ops->set(pf, pf->opaque, filter, initial);
    pf->synced = now;
    if (err < 0) {
        free(ranges);
        free(filter);
        return err;
    }
    // Synced: what was due is done.
    event_filter_set(&pf->ef, ranges, filter);
    pf->tail_open = open;
    pf->tail = open ? tail : 0;
    pf->uncovered = false;
    if (!open)
        pf->add_due = 0;
    pf->del_due = pf->del_first = 0;
    pf->dropped = 0;
    return 0;
}

static void pid_filter_timer(struct timer *timer)
{
    struct pid_filter *pf = container_of(timer, struct pid_filter, timer);
    u64 due;

    timer_cancel(timer);
    pf->timer_at = 0;
    due = pid_filter_due(pf);
    if (due && due <= get_ktime_ns()) {
        pid_filter_sync(pf, false);
        pid_filter_notify(pf);
    }
    pid_filter_schedule(pf);
}

int pid_filter_add(struct pid_filter *pf, pid_t pid)
{
    int ret = value_set_add(pf->ef.values, pid);

    if (ret < 0 || !pf->started)
        return ret;
    // Keep the tail open a while for the threads to come.
    pf->last = pid;
    pf->add_due = get_ktime_ns() + PID_FILTER_ADD_DELAY;
    /*
     * Not in the kernel set: the tail opens from it, the later threads get
     * the next pids. After a pid wrap the tail ends at the next followed pid;
     * a new thread past it, or past the gaps, moves the tail on to its pid.
     * Synced at once; ptrace keeps the thread stopped meanwhile, none of its
     * events is missed. On error it stays uncovered, retried later.
     */
    if (!value_ranges_has(pf->ef.ranges, value_key(pf->ef.values, pid))) {
        pf->tail_open = true;
        pf->tail = pid;
        pf->uncovered = true;
        pid_filter_sync(pf, false);
        pid_filter_notify(pf);
    }
    pid_filter_schedule(pf);
    return ret;
}

int pid_filter_del(struct pid_filter *pf, pid_t pid)
{
    int ret = value_set_del(pf->ef.values, pid);
    u64 now;

    if (!pf->started)
        return ret;
    /*
     * The kernel set still lets in the pid, a new task may get it after a pid
     * wrap: pid_filter_match() drops its records. The pid does not wrap soon:
     * wait for the exits to settle.
     */
    if (value_ranges_has(pf->ef.ranges, value_key(pf->ef.values, pid))) {
        now = get_ktime_ns();
        if (!pf->del_first)
            pf->del_first = now;
        pf->del_due = now + PID_FILTER_DEL_DELAY;
        if (pf->del_due > pf->del_first + PID_FILTER_DEL_MAX)
            pf->del_due = pf->del_first + PID_FILTER_DEL_MAX;
    }
    pid_filter_notify(pf);
    pid_filter_schedule(pf);
    return ret;
}

int pid_filter_add_threads(struct pid_filter *pf, struct perf_thread_map *threads)
{
    int idx, err;
    pid_t pid;

    perf_thread_map__for_each_thread(pid, idx, threads) {
        if (pid < 0 || threads->map[idx].cgroup)
            continue;
        err = pid_filter_add(pf, pid);
        if (err < 0)
            return err;
    }
    return 0;
}

int pid_filter_start(struct pid_filter *pf)
{
    int err;

    if (pf->started)
        return 0;
    err = pid_filter_sync(pf, true);
    if (err < 0)
        return err;
    pf->started = true;
    pid_filter_notify(pf);
    return 0;
}

int pid_filter_match(struct pid_filter *pf, pid_t pid)
{
    if (value_set_has(pf->ef.values, pid))
        return 1;
    // Too many: tighten, out of event processing.
    if (++pf->dropped == PID_FILTER_DROPS)
        pid_filter_schedule(pf);
    return 0;
}

bool pid_filter_follows(struct pid_filter *pf, pid_t pid)
{
    return value_set_has(pf->ef.values, pid);
}

const char *pid_filter_string(struct pid_filter *pf)
{
    return pf->ef.filter;
}
