#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <monitor.h>

// comm ~ "xyz*" || comm ~ "abc?"
static int comm_filter(char **result, const char *s, const char *comm_field)
{
    const char *op;
    int ret;

    if (!s || strlen(s) == 0)
        return 0;

    if (strchr(s, '*') || strchr(s, '?') || strchr(s, '['))
        op = "~";
    else
        op = "==";

    if (*result == NULL)
        ret = asprintf(result, "%s %s \"%s\"", comm_field, op, s);
    else {
        char *tmp;
        ret = asprintf(&tmp, "%s || %s %s \"%s\"", *result, comm_field, op, s);
        if (ret >= 0) {
            free(*result);
            *result = tmp;
        }
    }
    return ret;
}

// pid==x || pid==y || (pid>=z0&&pid<=z1)
static int range_filter(char **result, int start, int end, const char *field)
{
    int ret = 0;

    if (start < 0 || end < 0)
        return -1;

    if (*result == NULL) {
        if (start == end)
            ret = asprintf(result, "%s==%d", field, start);
        else
            ret = asprintf(result, "(%s>=%d&&%s<=%d)", field, start, field, end);
    } else {
        char *tmp;
        if (start == end)
            ret = asprintf(&tmp, "%s || %s==%d", *result, field, start);
        else
            ret = asprintf(&tmp, "%s || (%s>=%d&&%s<=%d)", *result, field, start, field, end);
        if (ret >= 0) {
            free(*result);
            *result = tmp;
        }
    }
    return ret;
}

char *pid_filter(struct perf_thread_map *threads, const char *field)
{
    int pid, idx, pid_start = -2, pid_end = -2;
    char *filter = NULL;

    perf_thread_map__for_each_thread(pid, idx, threads) {
        if (pid >= 0) {
            // The pids are sorted from small to large and can be used to
            // judge whether they are numerically continuous.
            if (pid_end + 1 != pid) {
                range_filter(&filter, pid_start, pid_end, field);
                pid_start = pid;
            }
            pid_end = pid;
        }
    }
    range_filter(&filter, pid_start, pid_end, field);
    return filter;
}

char *cpu_filter(struct perf_cpu_map *cpus, const char *field)
{
    int cpu, idx, cpu_start = -2, cpu_end = -2;
    char *filter = NULL;

    perf_cpu_map__for_each_cpu(cpu, idx, cpus) {
        if (cpu >= 0) {
            // The cpus are sorted from small to large and can be used to
            // judge whether they are numerically continuous.
            if (cpu_end + 1 != cpu) {
                range_filter(&filter, cpu_start, cpu_end, field);
                cpu_start = cpu;
            }
            cpu_end = cpu;
        }
    }
    range_filter(&filter, cpu_start, cpu_end, field);
    return filter;
}


// field==a, (field>=a&&field<=b), field>=a
static int pid_range_len(const struct pid_range *r, const char *field)
{
    if (r->hi == INT_MAX)
        return snprintf(NULL, 0, "%s>=%d", field, r->lo);
    if (r->lo == r->hi)
        return snprintf(NULL, 0, "%s==%d", field, r->lo);
    return snprintf(NULL, 0, "(%s>=%d&&%s<=%d)", field, r->lo, field, r->hi);
}

/* The filter length once every gap <= @gap is merged; ranges joined by "||". */
static size_t pid_ranges_len(const struct pid_ranges *ranges, const char *field, long gap)
{
    struct pid_range r;
    size_t len = 0;
    int i;

    if (ranges->nr == 0)
        return snprintf(NULL, 0, "%s<0", field);
    r = ranges->r[0];
    for (i = 1; i < ranges->nr; i++) {
        if ((long)ranges->r[i].lo - r.hi - 1 <= gap) {
            r.hi = ranges->r[i].hi;
            continue;
        }
        len += pid_range_len(&r, field) + 2;
        r = ranges->r[i];
    }
    return len + pid_range_len(&r, field);
}

static void pid_ranges_merge(struct pid_ranges *ranges, long gap)
{
    int i, n = 0;

    for (i = 1; i < ranges->nr; i++) {
        if ((long)ranges->r[i].lo - ranges->r[n].hi - 1 <= gap)
            ranges->r[n].hi = ranges->r[i].hi;
        else
            ranges->r[++n] = ranges->r[i];
    }
    if (ranges->nr)
        ranges->nr = n + 1;
}

static int pid_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

struct pid_ranges *pid_ranges_new(struct perf_thread_map *threads, int tail,
                                  const char *field, size_t budget)
{
    int nr = threads ? perf_thread_map__nr(threads) : 0;
    struct pid_ranges *ranges;
    int *pids = NULL;
    int pid, idx, i, n = 0;
    long lo, hi;

    ranges = malloc(sizeof(*ranges) + (nr + 1) * sizeof(struct pid_range));
    if (!ranges)
        return NULL;
    if (nr) {
        pids = malloc(nr * sizeof(int));
        if (!pids) {
            free(ranges);
            return NULL;
        }
        perf_thread_map__for_each_thread(pid, idx, threads) {
            if (pid >= 0)
                pids[n++] = pid;
        }
        qsort(pids, n, sizeof(int), pid_cmp);
    }

    // Consecutive pids make a range.
    ranges->nr = 0;
    for (i = 0; i < n; i++) {
        struct pid_range *last = ranges->nr ? &ranges->r[ranges->nr - 1] : NULL;
        if (last && pids[i] <= last->hi + 1)
            last->hi = pids[i];
        else
            ranges->r[ranges->nr++] = (struct pid_range){pids[i], pids[i]};
    }
    free(pids);

    /*
     * The tail: the pids are allocated upwards from it, skipping the ones in
     * use, up to the next range. Above all of the ranges it is open; after a
     * pid wrap it is not, the ranges above stay as they are.
     */
    if (tail >= 0) {
        struct pid_range t = {tail, INT_MAX};

        for (i = 0; i < ranges->nr; i++) {
            if (ranges->r[i].lo > tail) {
                t.hi = ranges->r[i].lo - 1;
                break;
            }
        }
        // Insert at i, then join the overlapping and adjacent neighbours.
        memmove(&ranges->r[i + 1], &ranges->r[i], (ranges->nr - i) * sizeof(t));
        ranges->r[i] = t;
        ranges->nr++;
        n = 0;
        for (i = 1; i < ranges->nr; i++) {
            if ((long)ranges->r[i].lo <= (long)ranges->r[n].hi + 1) {
                if (ranges->r[i].hi > ranges->r[n].hi)
                    ranges->r[n].hi = ranges->r[i].hi;
            } else
                ranges->r[++n] = ranges->r[i];
        }
        ranges->nr = n + 1;
    }

    // The smallest gap that fits: the length does not grow with the gap.
    if (ranges->nr > 1 && pid_ranges_len(ranges, field, 0) > budget) {
        lo = 1;
        hi = (long)ranges->r[ranges->nr - 1].lo - ranges->r[0].hi;
        while (lo < hi) {
            long mid = lo + (hi - lo) / 2;
            if (pid_ranges_len(ranges, field, mid) > budget)
                lo = mid + 1;
            else
                hi = mid;
        }
        pid_ranges_merge(ranges, lo);
    }
    return ranges;
}

bool pid_ranges_has(const struct pid_ranges *ranges, int pid)
{
    int lo = 0, hi = ranges ? ranges->nr - 1 : -1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (pid < ranges->r[mid].lo)
            hi = mid - 1;
        else if (pid > ranges->r[mid].hi)
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

char *pid_ranges_filter(const struct pid_ranges *ranges, const char *field)
{
    size_t len = pid_ranges_len(ranges, field, 0) + 1, off = 0;
    char *filter = malloc(len);
    int i;

    if (!filter)
        return NULL;
    if (ranges->nr == 0)
        snprintf(filter, len, "%s<0", field);
    for (i = 0; i < ranges->nr; i++) {
        const struct pid_range *r = &ranges->r[i];
        const char *or = i ? "||" : "";

        if (r->hi == INT_MAX)
            off += snprintf(filter + off, len - off, "%s%s>=%d", or, field, r->lo);
        else if (r->lo == r->hi)
            off += snprintf(filter + off, len - off, "%s%s==%d", or, field, r->lo);
        else
            off += snprintf(filter + off, len - off, "%s(%s>=%d&&%s<=%d)", or, field, r->lo, field, r->hi);
    }
    return filter;
}

/*
 * https://docs.kernel.org/trace/events.html
 * 5. Event filtering
 *
 * The operators available for numeric fields are:
 * ==, !=, <, <=, >, >=, &
 *
 * And for string fields they are:
 * ==, !=, ~
 *
**/
struct tp_filter *tp_filter_new(struct perf_thread_map *threads, const char *pid_field,
                                     const char *filter, const char *comm_field)
{
    struct tp_filter *tp_filter = NULL;

    tp_filter = malloc(sizeof(*tp_filter));
    if (!tp_filter)
        return NULL;

    memset(tp_filter, 0, sizeof(*tp_filter));

    if (filter) {
        char *sep;
        char *f = strdup(filter);
        char *s = f;

        while ((sep = strchr(s, ',')) != NULL) {
            *sep = '\0';
            comm_filter(&tp_filter->comm, s, comm_field);
            s = sep + 1;
        }
        comm_filter(&tp_filter->comm, s, comm_field);

        free(f);

        tp_filter->filter = tp_filter->comm;
    } else if (threads) {
        tp_filter->pid = pid_filter(threads, pid_field);
        tp_filter->filter = tp_filter->pid;
    }

    if (tp_filter->comm || tp_filter->pid)
        return tp_filter;
    else {
        free(tp_filter);
        return NULL;
    }
}

void tp_filter_free(struct tp_filter *tp_filter)
{
    if (tp_filter) {
        if (tp_filter->comm)
            free(tp_filter->comm);
        if (tp_filter->pid)
            free(tp_filter->pid);
        free(tp_filter);
    }
}

