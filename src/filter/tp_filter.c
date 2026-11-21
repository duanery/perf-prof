#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
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

