#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/perf_event.h>
#include <sys/time.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#ifdef HAVE_LIBTCMALLOC
#include <gperftools/malloc_extension_c.h>
#endif
#ifdef HAVE_RPMALLOC
#include <rpmalloc.h>
#endif
#include <linux/thread_map.h>
#include <linux/cgroup.h>
#include <trace_helpers.h>
#include <monitor.h>
#include <tep.h>
#include <timer.h>
#include <stack_helpers.h>
#include <api/fs/fs.h>


static int daylight_active;
static unsigned int page_size;

struct event_poll *main_epoll = NULL;

struct list_head prof_dev_list = LIST_HEAD_INIT(prof_dev_list);

struct monitor *monitors_list = NULL;
struct monitor *monitor = NULL;

void monitor_register(struct monitor *m)
{
    m->next = monitors_list;
    monitors_list = m;
}

struct monitor * monitor_find(const char *name)
{
    struct monitor *m = monitors_list;
    while(m) {
        if (!strcmp(m->name, name))
            return m;
        m = m->next;
    }
    return NULL;
}

struct monitor *monitor_next(struct monitor *m)
{
    if (!m)
        return monitors_list;
    else
        return m->next;
}

int prof_dev_nr_ins(struct prof_dev *dev)
{
    int nr_ins;

    nr_ins = perf_cpu_map__nr(dev->cpus);
    if (perf_cpu_map__empty(dev->cpus))
        nr_ins = perf_thread_map__nr(dev->threads);

    return nr_ins;
}

int prof_dev_ins_cpu(struct prof_dev *dev, int ins)
{
    return perf_cpu_map__cpu(dev->cpus, ins);
}

int prof_dev_ins_thread(struct prof_dev *dev, int ins)
{
    return perf_thread_map__pid(dev->threads, ins);
}

int prof_dev_ins_oncpu(struct prof_dev *dev)
{
    return !perf_cpu_map__empty(dev->cpus);
}

int main_epoll_add(int fd, unsigned int events, void *ptr, handle_event handle)
{
    return event_poll__add(main_epoll, fd, events, ptr, handle);
}

int main_epoll_del(int fd)
{
    return event_poll__del(main_epoll, fd);
}

/******************************************************
perf-prof argc argv
******************************************************/

struct env env;

static volatile int running = 0;

const char *main_program_version = PROGRAME " 1.9";

enum {
    LONG_OPT_start = 500,
    LONG_OPT_than,
    LONG_OPT_only_than,
    LONG_OPT_lower,
    LONG_OPT_threshold,
    LONG_OPT_detail,
    LONG_OPT_period,
};

static int workload_prepare(struct workload *workload, char *argv[]);

/**
 *  Parses a string into ns.  The number stored at @ptr is
 *  potentially suffixed with s, ms, us, ns.
 */
static unsigned long nsparse(const char *ptr, char **retptr)
{
    char *endptr;   /* local pointer to end of parsed string */

    unsigned long ret = strtoul(ptr, &endptr, 10);
    unsigned long tmp = ret;

    switch (*endptr) {
    case 'S':
    case 's':
        tmp *= 1000;
        endptr--;
        /* fall through */
    case 'M':
    case 'm':
        tmp *= 1000;
        /* fall through */
    case 'U':
    case 'u':
        tmp *= 1000;
        /* fall through */
    case 'N':
    case 'n':
        endptr++;
        if (*endptr == 's') {
            endptr++;
            ret = tmp;
        }
    default:
        break;
    }

    if (retptr)
        *retptr = endptr;

    return ret;
}

static void detail_parse(const char *s)
{
    if (strcmp(s, "samecpu") == 0)
        env.samecpu = true;
    else if (strcmp(s, "samepid") == 0)
        env.samepid = true;
    else if (strcmp(s, "sametid") == 0)
        env.sametid = true;
    else if (strcmp(s, "samekey") == 0)
        env.samekey = true;
    else if (strncmp(s, "hide<", 5) == 0)
        env.hide_than = nsparse(s+5, NULL);
    else if (s[0] == '-')
        env.before_event1 = nsparse(s+1, NULL);
    else if (s[0] == '1' && s[1] == '\0')
        env.same1 = true;
    else if (s[0] == '2' && s[1] == '\0')
        env.same2 = true;
    else
        env.after_event2 = nsparse(s, NULL);
}

static int parse_arg(int key, char *arg)
{
    switch (key) {
    case 'e':
        env.events = realloc(env.events, (env.nr_events + 1) * sizeof(*env.events));
        env.events[env.nr_events] = strdup(arg);
        if (env.nr_events == 0)
            env.event = env.events[0];
        env.nr_events ++;
        break;
    case LONG_OPT_only_than:
        env.only_print_greater_than = true;
        // fall through
    case LONG_OPT_than:
        env.greater_than = nsparse(arg, NULL);
        break;
    case LONG_OPT_lower:
        env.lower_than = nsparse(arg, NULL);
        break;
    case LONG_OPT_threshold:
        env.threshold = nsparse(arg, NULL);
        break;
    case LONG_OPT_detail:
        env.detail = true;
        if (arg) {
            char *ss = strdup(arg);
            char *sep, *s = ss;
            while ((sep = strchr(s, ',')) != NULL) {
                *sep = '\0';
                detail_parse(s);
                s = sep + 1;
            }
            if (*s)
                detail_parse(s);
            free(ss);
        }
        break;
    case LONG_OPT_period:
        env.sample_period = nsparse(arg, NULL);
        break;
    case 'V':
        printf("%s\n", main_program_version);
        exit(0);
    default:
        break;
    }
    return 0;
}

static int parse_help_cb(const struct option *opt, const char *arg, int unset)
{
    help();
    return 0;
}

static int parse_arg_cb(const struct option *opt, const char *arg, int unset)
{
    return parse_arg(opt->short_name, (char *)arg);
}

static void compgen_events(char **evt_list, int evt_num, void *opaque)
{
    struct {
        char *prefix;
        const char *match;
        int skiplen;
        int comp_type;
    } *op = opaque;
    const char *prefix = op->prefix ?: "";
    char *prev = NULL, *found;
    int match_len = op->match ? strlen(op->match) : 0;
    int i, j, matched = 0;
    unsigned int maxprefix = -1;

    if (match_len == 0) {
        for (i = 0; i < evt_num; i++)
            printf("'%s%s'\n", prefix, evt_list[i]);
        return;
    }

    /*
     # COMP_TYPE              Need prefix?   Substring matching?   matched==1
     # 9 [Tab]                Y              Y (Prefix first)      Output ','
     # ? [Tab][Tab]           N              Y                     Output ','
     # % menu-complete        Y              Y                     Output ','
     # * insert-completions   Y              Y (!prefix)           No output ','
     #
     # ! show-all-if-ambiguous   Same as [Tab][Tab]
     # @ show-all-if-unmodified  Same as [Tab][Tab]
     */

    // ![Tab]: [Tab][Tab], menu-complete, etc.
    // substring matching: strstr.
    if (op->comp_type != 9) {
        for (i = 0; i < evt_num; i++)
            if (strstr(evt_list[i], op->match) != NULL) {
                if (op->comp_type != '*')
                    printf("'%s%s'\n", prefix, evt_list[i] + op->skiplen);
                else if (!op->prefix)
                    printf("'%s'\n", evt_list[i]);
                found = evt_list[i];
                matched++;
            }
        if (matched == 1 && op->comp_type != '*')
            printf("'%s%s,'\n", prefix, found + op->skiplen);
        return;
    }

    // [Tab] prefix matching: strncmp.
    for (i = 0; i < evt_num; i++)
        if (strncmp(evt_list[i], op->match, match_len) == 0)
            matched++;
    if (matched != 0) {
        j = matched;
        for (i = 0; j; i++)
            if (strncmp(evt_list[i], op->match, match_len) == 0) {
                printf("'%s%s'\n", prefix, evt_list[i] + op->skiplen);
                if (matched == 1)
                    printf("'%s%s,'\n", prefix, evt_list[i] + op->skiplen);
                j--;
            }
        return;
    }

    // [Tab] substring matching. The longest common prefix must contain `op->match'.
    for (i = 0; i < evt_num; i++)
        if ((found = strstr(evt_list[i], op->match)) != NULL) {
            matched++;
            if (prev) {
                const char *s1 = prev, *s2 = evt_list[i];
                j = 0; while (*s1++ == *s2++) j++;
                if (j < maxprefix) maxprefix = j;
                if (maxprefix < found - evt_list[i] + match_len)
                    goto failed;
            }
            prev = evt_list[i];
        }
    if (matched != 0) {
        j = matched;
        for (i = 0; j; i++)
            if (strstr(evt_list[i], op->match) != NULL) {
                printf("'%s%s'\n", prefix, evt_list[i] + op->skiplen);
                if (matched == 1)
                    printf("'%s%s,'\n", prefix, evt_list[i] + op->skiplen);
                j--;
            }
        return;
    }

failed:
    printf("'%s%s'\n", prefix, op->match + op->skiplen);
    printf("'%s%s,'\n", prefix, op->match + op->skiplen);
}

static int compgen_arg(const struct option *opt, const char *arg, int comp_type)
{
    const char *comma;
    char *prefix = NULL;
    char *COMP_SKIPLEN = getenv("COMP_SKIPLEN");
    int skiplen = 0;

    switch (opt->short_name) {
    case 'e':
        comma = strrchr(arg, ',');
        if (comp_type == 9 || comp_type == '%' || comp_type == '*') {
            if (COMP_SKIPLEN) {
                /*
                 * If option parameter contains "$COMP_WORDBREAKS" characters, it will be separated
                 * into multiple words. But completion only operates on the word pointed to by
                 * COMP_POINT.
                 * COMP_SKIPLEN represents the length of the previous word, which needs to be skipped
                 * when perf-prof outputs completion.
                 *
                 * perf-prof trace -e sched:sched_wakeup/pid>1/,sch[TAB]
                 *   COMP_WORDBREAKS=" \n\"'><=;|&(:"
                 *   COMP_WORDS='(... [2]="-e" [3]="sched" [4]=":" [5]="sched_wakeup/pid" [6]=">" [7]="1/,sch")'
                 *   COMP_SKIPLEN=23 (Contains the length of COMP_WORDS [3], [4], [5], [6])
                 *   OUTPUT='([0]="1/,sched:sched_kthread_stop" ...)'
                 */
                int comp_skiplen = atoi(COMP_SKIPLEN);
                const char *skip = arg + comp_skiplen;

                if (comma) {
                    if (skip < comma) {
                        arg = skip;
                        goto make_prefix;
                    }
                    arg = comma + 1;
                    comma = NULL;
                }
                if (arg < skip)
                    skiplen = skip - arg;
            } else if (comma) {
            make_prefix:
                prefix = strndup(arg, comma - arg + 1);
            }
        } {
            struct {
                char *prefix;
                const char *match;
                int skiplen;
                int comp_type;
            } op = {
                .prefix = prefix,
                .match = comma ? comma + 1 : arg,
                .skiplen = skiplen,
                .comp_type = comp_type,
            };
            print_tracepoint_events(compgen_events, (void *)&op);
        }
        if (prefix) free(prefix);
        break;
    default:
        break;
    }
    return 0;
}

#define OPT_BIT_NONEG(s, l, v, h, b)      { .type = OPTION_BIT, .short_name = (s), .long_name = (l), .value = check_vtype(v, int *), .help = (h), .defval = (b), .flags = PARSE_OPT_NONEG }
#define OPT_BOOL_NONEG(s, l, v, h)       { .type = OPTION_BOOLEAN, .short_name = (s), .long_name = (l), .value = check_vtype(v, bool *), .help = (h), .flags = PARSE_OPT_NONEG }
#define OPT_INT_NONEG(s, l, v, a, h)     { .type = OPTION_INTEGER, .short_name = (s), .long_name = (l), .value = check_vtype(v, int *), .argh = (a), .help = (h), .flags = PARSE_OPT_NONEG }
#define OPT_INT_NONEG_SET(s, l, v, os, a, h) { .type = OPTION_INTEGER, .short_name = (s), .long_name = (l), .value = check_vtype(v, int *), .set = check_vtype(os, bool *), .argh = (a), .help = (h), .flags = PARSE_OPT_NONEG }
#define OPT_UINT_NONEG(s, l, v, a, h)    { .type = OPTION_UINTEGER, .short_name = (s), .long_name = (l), .value = check_vtype(v, unsigned int *), .argh = (a), .help = (h), .flags = PARSE_OPT_NONEG }
#define OPT_LONG_NONEG(s, l, v, a, h)    { .type = OPTION_LONG, .short_name = (s), .long_name = (l), .value = check_vtype(v, long *), .argh = (a), .help = (h), .flags = PARSE_OPT_NONEG }
#define OPT_ULONG_NONEG(s, l, v, a, h)   { .type = OPTION_ULONG, .short_name = (s), .long_name = (l), .value = check_vtype(v, unsigned long *), .argh = (a), .help = (h), .flags = PARSE_OPT_NONEG }
#define OPT_U64_NONEG(s, l, v, a, h)     { .type = OPTION_U64, .short_name = (s), .long_name = (l), .value = check_vtype(v, u64 *), .argh = (a), .help = (h), .flags = PARSE_OPT_NONEG }
#define OPT_STRDUP_NONEG(s, l, v, a, h)  { .type = OPTION_STRING,  .short_name = (s), .long_name = (l), .value = check_vtype(v, char **), .argh = (a), .help = (h), .flags = PARSE_OPT_NONEG | PARSE_OPT_NOEMPTY }
#define OPT_STRDUP_EMPTY(s, l, v, a, h)  { .type = OPTION_STRING,  .short_name = (s), .long_name = (l), .value = check_vtype(v, char **), .argh = (a), .help = (h), .flags = PARSE_OPT_NONEG }
#define OPT_PARSE_NONEG(s, l, v, a, h) \
    { .type = OPTION_CALLBACK, .short_name = (BUILD_BUG_ON_ZERO(s==0) + s), .long_name = (l), .value = (v), .argh = (a), .help = (h), .flags = PARSE_OPT_NONEG, .callback = (parse_arg_cb), .compgen = (compgen_arg) }
#define OPT_PARSE_NOARG(s, l, v, a, h) \
    { .type = OPTION_CALLBACK, .short_name = (BUILD_BUG_ON_ZERO(s==0) + s), .long_name = (l), .value = (v), .argh = (a), .help = (h), .flags = PARSE_OPT_NONEG | PARSE_OPT_NOARG, .callback = (parse_arg_cb) }
#define OPT_PARSE_OPTARG(s, l, v, a, h) \
    { .type = OPTION_CALLBACK, .short_name = (BUILD_BUG_ON_ZERO(s==0) + s), .long_name = (l), .value = (v), .argh = (a), .help = (h), .flags = PARSE_OPT_NONEG | PARSE_OPT_OPTARG, .callback = (parse_arg_cb) }
#define OPT_INT_OPTARG(s, l, v, d, a, h) \
    { .type = OPTION_INTEGER, .short_name = (s), .long_name = (l), .value = check_vtype(v, int *), .argh = (a), .defval = (intptr_t)(d), .help = (h), .flags = PARSE_OPT_NONEG | PARSE_OPT_OPTARG }
#define OPT_INT_OPTARG_SET(s, l, v, os, d, a, h) \
        { .type = OPTION_INTEGER, .short_name = (s), .long_name = (l), .value = check_vtype(v, int *), .set = check_vtype(os, bool *), .argh = (a), .defval = (intptr_t)(d), .help = (h), .flags = PARSE_OPT_NONEG | PARSE_OPT_OPTARG }
#define OPT_HELP() \
    { .type = OPTION_CALLBACK, .short_name = ('h'), .long_name = ("help"), .help = ("Give this help list"), .flags = PARSE_OPT_NONEG | PARSE_OPT_NOARG, .callback = (parse_help_cb) }

struct option main_options[] = {
    OPT_GROUP("OPTION:"),
    OPT_STRDUP_NONEG('C',        "cpus", &env.cpumask,    "cpu[-cpu],...", "Monitor the specified CPU, Dflt: all cpu"),
    OPT_STRDUP_NONEG('p',        "pids", &env.pids,       "pid,...",       "Attach to processes"),
    OPT_STRDUP_NONEG('t',        "tids", &env.tids,       "tid,...",       "Attach to threads"),
    OPT_STRDUP_NONEG( 0 ,     "cgroups", &env.cgroups,    "cgroup,...",    "Attach to cgroups, support regular expression."),
    OPT_BOOL_NONEG  ( 0 ,     "inherit", &env.inherit,                     "Child tasks do inherit counters."),
    OPT_INT_NONEG_SET( 0 ,  "watermark", &env.watermark,  &env.watermark_set, "0-100",  "Wake up "PROGRAME" watermark."),
    OPT_INT_NONEG   ('i',    "interval", &env.interval,   "ms",            "Interval, Unit: ms"),
    OPT_STRDUP_NONEG('o',      "output", &env.output,     "file",          "Output file name"),
    OPT_BOOL_NONEG  ( 0 ,       "order", &env.order,                       "Order events by timestamp."),
    OPT_INT_NONEG   ('m',  "mmap-pages", &env.mmap_pages, "pages",         "Number of mmap data pages and AUX area tracing mmap pages"),
    OPT_LONG_NONEG  ('N',      "exit-N", &env.exit_n, "N",                 "Exit after N events have been sampled."),
    OPT_BOOL_NONEG  ( 0 ,         "tsc", &env.tsc,                         "Convert perf clock to tsc."),
    OPT_STRDUP_NONEG( 0 ,    "kvmclock", &env.kvmclock,    "uuid",         "Convert perf clock to Guest's kvmclock."),
    OPT_U64_NONEG   ( 0 ,"clock-offset", &env.clock_offset, NULL,          "Sum with clock-offset to get the final clock."),
    OPT_BOOL_NONEG  ( 0 ,   "monotonic", &env.monotonic,                   "Use CLOCK_MONOTONIC as perf clock."),
    OPT_INT_NONEG   ( 0 ,  "usage-self", &env.usage_self,  "ms",           "Periodically output the CPU usage of perf-prof itself, Unit: ms"),
    OPT_INT_NONEG   ( 0 ,"sampling-limit", &env.sampling_limit, "N",       "Limit the number of samples per second per instance."),
    OPT_STRDUP_NONEG( 0 , "perfeval-cpus", &env.perfeval_cpus, "cpu",      "Performance evaluation cpu list."),
    OPT_STRDUP_NONEG( 0 , "perfeval-pids", &env.perfeval_pids, "pid",      "Performance evaluation pid list."),
    OPT_PARSE_NOARG ('V',     "version", NULL,             NULL,           "Version info"),
    OPT__VERBOSITY(&env.verbose),
    OPT_HELP(),

    OPT_GROUP("FILTER OPTION:"),
    OPT_BOOL_NONEG  ('G',     "exclude-host", &env.exclude_host,                "Monitor GUEST, exclude host"),
    OPT_BOOL_NONEG  ( 0 ,    "exclude-guest", &env.exclude_guest,               "exclude guest"),
    OPT_BOOL_NONEG  ( 0 ,     "exclude-user", &env.exclude_user,                "exclude user"),
    OPT_BOOL_NONEG  ( 0 ,   "exclude-kernel", &env.exclude_kernel,              "exclude kernel"),
    OPT_BOOLEAN_SET ( 0 ,   "user-callchain", &env.user_callchain,   &env.user_callchain_set,   "include user callchains, no- prefix to exclude"),
    OPT_BOOLEAN_SET ( 0 , "kernel-callchain", &env.kernel_callchain, &env.kernel_callchain_set, "include kernel callchains, no- prefix to exclude"),
    OPT_BOOL_NONEG  ( 0 , "python-callchain", &env.python_callchain,                            "include python callchains"),
    OPT_INT_OPTARG_SET( 0 ,    "irqs_disabled", &env.irqs_disabled,    &env.irqs_disabled_set,    1, "0|1",  "ebpf, irqs disabled or not."),
    OPT_INT_OPTARG_SET( 0 , "tif_need_resched", &env.tif_need_resched, &env.tif_need_resched_set, 1, "0|1",  "ebpf, TIF_NEED_RESCHED is set or not."),
    OPT_INT_NONEG_SET ( 0 ,      "exclude_pid", &env.exclude_pid,      &env.exclude_pid_set,         "pid",  "ebpf, exclude pid"),
    OPT_INT_NONEG_SET ( 0 ,   "nr_running_min", &env.nr_running_min,   &env.nr_running_min_set,       NULL,  "ebpf, minimum number of running processes for CPU runqueue."),
    OPT_INT_NONEG_SET ( 0 ,   "nr_running_max", &env.nr_running_max,   &env.nr_running_max_set,       NULL,  "ebpf, maximum number of running processes for CPU runqueue."),
    OPT_INT_NONEG_SET ( 0 ,     "sched_policy", &env.sched_policy,     &env.sched_policy_set,         NULL,  "ebpf, schedule policy, 0:NORMAL, 1:FIFO, 2:RR, etc."),


    OPT_GROUP("PROFILER OPTION:"),
    OPT_PARSE_NONEG ('e', "event", NULL,    "EVENT,...",        "Event selector. use '"PROGRAME" list' to list available tp events.\n"
                                                                "  EVENT,EVENT,...\n"
                                                                "  EVENT: sys:name[/filter/ATTR/ATTR/.../]\n"
                                                                "         profiler[/option/ATTR/ATTR/.../]\n"
                                                                "         kprobe:func[/filter/ATTR/ATTR/.../]\n"
                                                                "         uprobe:func@\"file\"[/filter/ATTR/ATTR/.../]\n"
                                                                "  filter: trace events filter\n"
                                                                "  ATTR:\n"
                                                                "      stack: sample_type PERF_SAMPLE_CALLCHAIN\n"
                                                                "      max-stack=int : sample_max_stack\n"
                                                                "      alias=str: event alias\n"
                                                                "      exec=EXPR: a public expression executed by any profiler\n"
                                                                "      cpus=cpu[-cpu]: attach to a different cpu list.\n"
                                                                "      top-by=EXPR: add to top, sort by this field\n"
                                                                "      top-add=EXPR: add to top\n"
                                                                "      comm=EXPR: top, show COMM\n"
                                                                "      ptr=EXPR: kmemleak, ptr field, Dflt: ptr=ptr\n"
                                                                "      size=EXPR: kmemleak, size field, Dflt: size=bytes_alloc\n"
                                                                "      num=EXPR: num-dist, num field\n"
                                                                "      key=EXPR: key for multiple events: top, multi-trace\n"
                                                                "      printkey=EXPR: Print the key (e.g. printf(\"%d\", key))\n"
                                                                "      role=EXPR: multi-trace, Bit 0: as event1, Bit 1: as event2.\n"
                                                                "      untraced: multi-trace, auxiliary, no two-event analysis\n"
                                                                "      trigger: multi-trace, use events to trigger interval output\n"
                                                                "      vm=uuid: get the mapping from Guest vcpu to Host tid\n"
                                                                "      push=[IP:]PORT: push events to the local broadcast server IP:PORT\n"
                                                                "      push=chardev: push events to chardev, e.g., /dev/virtio-ports/*\n"
                                                                "      push=file: push events to file\n"
                                                                "      pull=[IP:]PORT: pull events from server IP:PORT\n"
                                                                "      pull=chardev: pull events from chardev\n"
                                                                "      pull=file: pull events from file\n"
                                                                "      index=field: sql, set the index field to replace the default\n"
                                                                "  EXPR:\n"
                                                                "      C expression. See `"PROGRAME" expr -h` for more information."
                                                                ),
    OPT_INT_NONEG   ('F',            "freq", &env.freq,                  NULL,  "Profile at this frequency, No profile: 0"),
    OPT_STRDUP_NONEG('k',             "key", &env.key,                  "str",  "Key for series events"),
    OPT_STRDUP_NONEG( 0 ,          "filter", &env.filter,            "filter",  "Event filter, See TRACEPOINT"),
    OPT_PARSE_NONEG (LONG_OPT_period, "period", &env.sample_period,      "ns",   "Sample period, Unit: s/ms/us/*ns"),
    OPT_STRDUP_NONEG(0, "impl", &env.impl,    "impl",       "Implementation of two-event analysis class. Dflt: delay.\n"
                                                                "    delay: latency distribution between two events\n"
                                                                "    pair: determine if two events are paired\n"
                                                                "    kmemprof: profile memory allocated and freed bytes\n"
                                                                "    syscalls: syscall delay\n"
                                                                "    call: analyze function calls, only for nested-trace.\n"
                                                                "    call-delay: call + delay, only for nested-trace."),
    OPT_BOOLEAN_SET ('S',   "interruptible", &env.interruptible, &env.interruptible_set, "TASK_INTERRUPTIBLE, no- prefix to exclude"),
    OPT_BOOL_NONEG  ('D', "uninterruptible", &env.uninterruptible,              "TASK_UNINTERRUPTIBLE"),
    OPT_PARSE_NONEG ( LONG_OPT_than, "than", &env.greater_than,          "n",   "Greater than specified time, Unit: s/ms/us/*ns"),
    OPT_PARSE_NONEG ( LONG_OPT_only_than, "only-than", &env.greater_than,"ns",  "Only print those that are greater than the specified time."),
    OPT_PARSE_NONEG ( LONG_OPT_lower, "lower", &env.lower_than,          "ns",  "Lower than specified time, Unit: s/ms/us/*ns"),
    OPT_PARSE_NONEG ( LONG_OPT_threshold, "threshold", &env.threshold,   "ns",  "Threshold for BPF output events, Unit: s/ms/us/*ns"),
    OPT_STRDUP_NONEG( 0 ,           "alloc", &env.tp_alloc,           "EVENT",  "Memory alloc tracepoint/kprobe/uprobe"),
    OPT_STRDUP_NONEG( 0 ,            "free", &env.tp_free,            "EVENT",  "Memory free tracepoint/kprobe/uprobe"),
    OPT_BOOL_NONEG  ( 0 ,        "syscalls", &env.syscalls,                     "Trace syscalls"),
    OPT_BOOL_NONEG  ( 0 ,          "perins", &env.perins,                       "Print per instance stat"),
    OPT_BOOL_NONEG  ('g',      "call-graph", &env.callchain,                    "Enable call-graph recording"),
    OPT_STRDUP_EMPTY( 0 ,     "flame-graph", &env.flame_graph,         "file",  "Specify the folded stack file."),
    OPT_STRDUP_NONEG( 0 ,         "heatmap", &env.heatmap,             "file",  "Specify the output latency file."),
    OPT_PARSE_OPTARG( LONG_OPT_detail, "detail", NULL, "-N,+N,1,2,hide<N,same*",
                                                       "More detailed information output.\n"
                                                       "For multi-trace profiler:\n"
                                                       "   -N: Before event1, print events within N nanoseconds.\n"
                                                       "   +N: After event2, print events within N nanoseconds.\n"
                                                       "    1: Only show events same as event1.\n"
                                                       "    2: Only show events same as event2.\n"
                                                       "hide<N: Hide event intervals less than N nanoseconds.\n"
                                                       "samecpu: Only show events with the same cpu as event1 or event2.\n"
                                                       "samepid: Only show events with the same pid as event1 or event2.\n"
                                                       "sametid: Only show events with the same tid as event1 or event2.\n"
                                                       "samekey: Only show events with the same key as event1 or event2."),
    OPT_INT_NONEG   ('T',         "trigger", &env.trigger_freq,          NULL,  "Trigger Threshold, No trigger: 0"),
    OPT_BOOL_NONEG  ( 0 ,            "test", &env.test,                         "Split-lock test verification"),
    OPT_STRDUP_NONEG( 0 ,         "symbols", &env.symbols,               NULL,  "Maps addresses to symbol names.\n"
                                                                                "Similar to pprof --symbols."),
    OPT_STRDUP_NONEG('d',          "device", &env.device,            "device",  "Block device, /dev/sdx"),
    OPT_INT_NONEG   ( 0 ,           "ldlat", &env.ldlat,             "cycles",  "mem-loads latency, Unit: cycles"),
    OPT_BOOL_NONEG  ( 0 ,       "overwrite", &env.overwrite,                    "use overwrite mode"),
    OPT_BOOL_NONEG  ( 0 ,            "spte", &env.spte,                         "kvmmmu: enable kvmmmu:kvm_mmu_set_spte"),
    OPT_BOOL_NONEG  ( 0 ,            "mmio", &env.mmio,                         "kvmmmu: enable kvmmmu:mark_mmio_spte"),
    OPT_BOOL_NONEG  ( 0 ,       "only-comm", &env.only_comm,                    "top: only show comm but not key"),
    OPT_BOOL_NONEG  ( 0 ,           "cycle", &env.cycle,                        "multi-trace: event cycle, from the last one back to the first."),
    OPT_BOOL_NONEG  ( 0 ,          "ptrace", &env.using_ptrace,                 "Use ptrace to track newly created threads."),
    OPT_BOOL_NONEG  ( 0 ,          "string", &env.string,                       "Output a printable string."),
    OPT_BIT_NONEG   ('1',               "1", &env.bytes,                        "Display output in 8-bit values", 1),
    OPT_BIT_NONEG   ('2',               "2", &env.bytes,                        "Display output in 16-bit values", 2),
    OPT_BIT_NONEG   ('4',               "4", &env.bytes,                        "Display output in 32-bit values", 4),
    OPT_BIT_NONEG   ('8',               "8", &env.bytes,                        "Display output in 64-bit values", 8),
    OPT_STRDUP_NONEG( 0 ,         "output2", &env.output2,             "file",  "Nonstandard output file name"),
    OPT_STRDUP_NONEG( 0 ,            "prio", &env.prio_map, "prio[-prio],...",  "Specify the priority range for real-time and normal scheduling"),
    OPT_STRDUP_NONEG( 0 ,           "query", &env.query,                "SQL",  "Execute SQL query on collected events"),
    OPT_BOOL_NONEG  ( 0 ,          "verify", &env.verify,                       "Validate SQL virtual table implementation and indexing integrity"),
    OPT_BOOL_NONEG  ( 0 ,            "comm", &env.comm,                         "Show command names"),

    OPT_END()
};

const char * const main_usage[] = {
    PROGRAME " profiler [PROFILER OPTION...] [help] [cmd [args...]]",
    PROGRAME " --symbols /path/to/bin",
    "",
    "Profiling based on perf_event and ebpf",
    NULL
};

static void free_env(struct env *e)
{
    if (e->nr_events) {
        while (e->nr_events--) free(e->events[e->nr_events]);
        free(e->events);
    }
    if (e->cpumask) free(e->cpumask);
    if (e->pids) free(e->pids);
    if (e->tids) free(e->tids);
    if (e->cgroups) free(e->cgroups);
    if (e->output) free(e->output);
    if (e->key) free(e->key);
    if (e->filter) free(e->filter);
    if (e->impl) free(e->impl);
    if (e->tp_alloc) free(e->tp_alloc);
    if (e->tp_free) free(e->tp_free);
    if (e->flame_graph) free(e->flame_graph);
    if (e->heatmap) free(e->heatmap);
    if (e->symbols) free(e->symbols);
    if (e->device) free(e->device);
    if (e->kvmclock) free(e->kvmclock);
    if (e->perfeval_cpus) free(e->perfeval_cpus);
    if (e->perfeval_pids) free(e->perfeval_pids);
    if (e->output2) free(e->output2);
    if (e->prio_map) free(e->prio_map);
    if (e->query) free(e->query);

    if (e->workload.pid > 0) {
        kill(e->workload.pid, SIGTERM);
    }
    if (e != &env) free(e);
    else
        memset(e, 0, sizeof(*e));
}

struct env *clone_env(struct env *p)
{
    struct env *e = malloc(sizeof(*e));

    if (!e) return NULL;

    *e = *p;
    e->workload.cork_fd = 0;
    e->workload.pid = 0;
    e->help_monitor = NULL;

    if (e->nr_events) {
        int i;
        e->events = calloc(e->nr_events, sizeof(*e->events));
        e->nr_events = 0;
        if (!e->events) goto failed;
        for (i = 0 ; i < p->nr_events; i++) {
            e->events[i] = strdup(p->events[i]);
            if (!e->events[i]) goto failed;
            e->nr_events ++;
        }
        e->event = e->events[0];
    }
    #define CLONE(f) if (e->f) {e->f = strdup(e->f); if (!e->f) goto failed;}
    CLONE (cpumask);
    CLONE (pids);
    CLONE (tids);
    CLONE (cgroups);
    CLONE (output);
    CLONE (key);
    CLONE (filter);
    CLONE (impl);
    CLONE (tp_alloc);
    CLONE (tp_free);
    CLONE (flame_graph);
    CLONE (heatmap);
    CLONE (symbols);
    CLONE (device);
    CLONE (kvmclock);
    CLONE (perfeval_cpus);
    CLONE (perfeval_pids);
    CLONE (output2);
    CLONE (prio_map);
    CLONE (query);

    if (p->workload.pid && !e->pids) {
        asprintf(&e->pids, "%d", p->workload.pid);
    }

    return e;

failed:
    free_env(e);
    return NULL;
}

void help(void)
{
    int argc = 2;
    const char *argv[] = {PROGRAME, "--help"};
    const char * const *usagestr = main_usage;
    struct monitor *m = monitor;

    if (m) {
        if (m->argv && m->desc) {
            argc = 0;
            while (m->argv[argc++] != NULL);
            parse_options(argc - 1, m->argv, main_options, m->desc, PARSE_OPT_INTERNAL_HELP_NO_ORDER);
        } else
            parse_options(argc, argv, main_options, main_usage, PARSE_OPT_INTERNAL_HELP_NO_ORDER);
    }

    fprintf(stderr, "\n Usage: %s\n", *usagestr++);
    while (*usagestr && **usagestr)
        fprintf(stderr, "    or: %s\n", *usagestr++);
    while (*usagestr) {
        fprintf(stderr, "%s%s\n",
                **usagestr ? "    " : "",
                *usagestr);
        usagestr++;
    }

#ifdef CONFIG_LIBBPF
    fprintf(stderr, "\n Available eBPF Profilers:\n");
    while((m = monitor_next(m))) {
        if (!strcasestr(m->name, "bpf")) continue;
        fprintf(stderr, "   %-20s", m->name);
        if (m->desc && m->desc[2] && m->desc[2][0])
            fprintf(stderr, " %s\n", m->desc[2]);
        else
            fprintf(stderr, "\n");
    }
#endif

    fprintf(stderr, "\n Available Profilers:\n");
    while((m = monitor_next(m))) {
        if (strcasestr(m->name, "bpf")) continue;
        fprintf(stderr, "   %-20s", m->name);
        if (m->desc && m->desc[2] && m->desc[2][0])
            fprintf(stderr, " %s\n", m->desc[2]);
        else
            fprintf(stderr, "\n");
    }

    fprintf(stderr, "\n See '%s profiler -h' for more information on a specific profiler.\n\n", PROGRAME);
    exit(129);
}

static void disable_help(void)
{
    struct option *opts = main_options;
    for (; opts->type != OPTION_END; opts++) {
        if (opts->short_name == 'h') {
            opts->short_name = 0;
            opts->long_name = "disable-help";
        }
    }
}

static void flush_main_options(profiler *p)
{
    struct option *opts;
    int i;

    if (!p->argv)
        return ;

    // disable all opts
    opts = main_options;
    for (; opts->type != OPTION_END; opts++) {
        opts->flags |= PARSE_OPT_DISABLED;
    }

    // enable profiler opts
    opts = main_options;
    for (; opts->type != OPTION_END; opts++) {
        for (i = 2/*PROGRAME, "-h"*/; p->argv[i]; i ++) {
            if (p->argv[i][1] == '\0' &&
                opts->short_name < 256 && isalnum(opts->short_name) && /* isshort */
                p->argv[i][0] == opts->short_name)
                goto enable;
            if (opts->long_name && strstarts(p->argv[i], opts->long_name))
                goto enable;
            if (opts->type == OPTION_GROUP && strcmp(opts->help, p->argv[i]) == 0)
                goto enable;
        }
        continue;
    enable:
        opts->flags &= (~PARSE_OPT_DISABLED);
        if (opts->long_name) {
            int len = strlen(opts->long_name);
            if (p->argv[i][len] != '\0') {
                opts->help = p->argv[i] + len;
                while (opts->help[0] == '\n' || opts->help[0] == ' ' || opts->help[0] == ':')
                    opts->help++;
                if (opts->flags & PARSE_OPT_OPTARG)
                    opts->argh = NULL;
                p->argv[i] = opts->long_name;
            }
        }
    }
}

static profiler *parse_main_options(int argc, char *argv[])
{
    profiler *prof = NULL;
    bool stop_at_non_option = true;
    bool dashdash = false;
    char *COMP_TYPE = getenv("COMP_TYPE"); // Bash Completion COMP_TYPE variable
    int comp_type = COMP_TYPE ? atoi(COMP_TYPE) : 0;
    bool enable_optcomp = false;

#ifndef CONFIG_LIBBPF
    static const char *LIBBPF_BUILD = "NO CONFIG_LIBBPF=y";
    set_option_nobuild(main_options, 0,    "irqs_disabled", LIBBPF_BUILD, true);
    set_option_nobuild(main_options, 0, "tif_need_resched", LIBBPF_BUILD, true);
    set_option_nobuild(main_options, 0,      "exclude_pid", LIBBPF_BUILD, true);
    set_option_nobuild(main_options, 0,   "nr_running_min", LIBBPF_BUILD, true);
    set_option_nobuild(main_options, 0,   "nr_running_max", LIBBPF_BUILD, true);
    set_option_nobuild(main_options, 0,     "sched_policy", LIBBPF_BUILD, true);
#endif

    while (argc > 0) {
        argc = parse_options(argc, (const char **)argv, main_options, main_usage,
                             PARSE_OPT_NO_INTERNAL_HELP | PARSE_OPT_KEEP_DASHDASH |
                             (enable_optcomp ? PARSE_OPT_BASH_COMPLETION : 0) |
                             (stop_at_non_option ? PARSE_OPT_STOP_AT_NON_OPTION :
                                                   PARSE_OPT_KEEP_ARGV0 | PARSE_OPT_KEEP_UNKNOWN));
        if (argc && argv[0][0] != '\0' && argv[0][0] != '-') {
            struct monitor *m = monitor_find(argv[0]);
            if (m != NULL) {
                if (strcmp(m->name, "help") == 0)
                    env.help_monitor = prof;
                else if (prof) {
#ifdef MULTI_PROF
                    struct env *e = zalloc(sizeof(struct env));
                    if (e) {
                        *e = env;
                        prof_dev_open(prof, e);
                    }
                    memset(&env, 0, sizeof(struct env));
#else
                    goto stop_at;
#endif
                }

                prof = m;
                monitor = m; // monitor only used in help();
                flush_main_options(m);
                enable_optcomp = comp_type ? true : false;
                continue;
            } else if (comp_type) {
                break;
            } else stop_at: if (stop_at_non_option) {
                stop_at_non_option = false;
                disable_help();
                continue;
            }
        }
        if (comp_type)
            break;
        // --
        if (argc && argv[0][0] == '-' && argv[0][1] == '-') {
            argc--;
            memmove(argv, argv + 1, argc * sizeof(argv[0]));
            argv[argc] = NULL;
            dashdash = true;
        }
        break;
    }

    if (comp_type) {
        if (prof) {
            if (argc == 0)
                printf(prof->compgen ? "\"%s %s\"\n" : "%s%s\n", prof->name, prof->compgen ?: "");
        } else {
            char *COMP_SKIPLEN = getenv("COMP_SKIPLEN");
            int skiplen = COMP_SKIPLEN ? atoi(COMP_SKIPLEN) : 0;
            struct monitor *m = NULL;
            while((m = monitor_next(m))) {
                if (!argc || argv[0][0] == '\0' ||
                    strncmp(m->name, argv[0], strlen(argv[0])) == 0) {
                    if (m->compgen && comp_type != '?')
                        printf("\"%s %s\"\n", m->name + skiplen, m->compgen);
                    else
                        printf("%s\n", m->name + skiplen);
                }
            }
        }
        exit(0);
    }

    if (env.symbols) {
        syms__convert(stdin, stdout, env.symbols);
        exit(0);
    }

    if (prof == NULL)
        help();

    /*
     * Allow profiler to parse its own arguments via argc_init callback.
     * This must be called regardless of dashdash ('--') presence, because
     * the first '--' is used to isolate profiler-specific options that are
     * fully parsed by the profiler internally, maximizing profiler capability.
     *
     * Supports two '--' separators:
     *   perf-prof <profiler> [main-opts] -- [profiler-opts] -- [workload]
     *
     * The profiler's argc_init stops parsing at the second '--', and the
     * remaining arguments become the workload command line.
     *
     * Only show "Unparsed options" warning when:
     *   1. No dashdash present (otherwise remaining args are for profiler/workload)
     *   2. There are remaining arguments
     *   3. Verbose mode is enabled
     *   4. Profiler doesn't have argc_init (can't handle extra args)
     */
    if (prof && prof->argc_init)
        argc = prof->argc_init(argc, argv);
    else if (!dashdash && argc && env.verbose > 0) {
        int i;
        printf("Unparsed options:");
        for (i = 0; i < argc; i ++)
            printf(" %s", argv[i]);
        printf("\n");
    }

    if (argc > 0) {
        if (workload_prepare(&env.workload, argv) < 0)
            goto failed;
    }

    return prof;

failed:
    free_env(&env);
    return NULL;
}

struct env *parse_string_options(char *str)
{
    char *token;
    int argc = 1; // argv[0] = "perf-prof"
    char **argv = malloc((argc + 1)*sizeof(char*));
    struct env *e = NULL;

    token = strtok(str, " ");
    while (token && argv) {
        argv[argc++] = token;
        argv = realloc(argv, (argc + 1)*sizeof(char*));
        token = strtok(NULL, " ");
    }
    if (!argv)
        return NULL;

    memset(&env, 0, sizeof(struct env));
    if (parse_main_options(argc, argv)) {
        e = malloc(sizeof(*e));
        if (e) *e = env;
    }
    free(argv);
    return e;
}

static char *perf_type_str(int type)
{
    static int nr_types = 0;
    static char **perf_types = NULL;

    if (!nr_types) {
        char path[PATH_MAX];
        struct dirent **namelist = NULL;
        int i, items, n, type;

        n = snprintf(path, PATH_MAX, "%s/bus/event_source/devices/", sysfs__mountpoint());
        items = scandir(path, &namelist, NULL, NULL);
        if (items <= 0)
            return NULL;

        for (i = 0; i < items; i++) {
            if (namelist[i]->d_name[0] == '.')
                continue;
            snprintf(path+n, PATH_MAX-n, "%s/type", namelist[i]->d_name);
            if (filename__read_int(path, &type) == 0) {
                int nr = type + 1;
                if (nr_types < nr) {
                    perf_types = realloc(perf_types, nr*sizeof(*perf_types));
                    if (!perf_types) goto failed;
                    memset(perf_types+nr_types, 0, (nr-nr_types)*sizeof(*perf_types));
                    nr_types = nr;
                }
                perf_types[type] = strdup(namelist[i]->d_name);
            }
        }

    failed:
        for (i = 0; i < items; i++)
            free(namelist[i]);
        free(namelist);
    }

    return type < nr_types ? perf_types[type] : NULL;
}

static void print_event(struct perf_evsel *evsel, int indent)
{
    struct perf_event_attr *attr = perf_evsel__attr(evsel);
    struct perf_event_member_cache *member_cache = perf_evsel_member_cache(evsel);
    const char *str = "unknown";
    int i;

    if (attr->type == PERF_TYPE_HARDWARE) {
        switch (attr->config) {
            case PERF_COUNT_HW_CPU_CYCLES: str = "cpu-cycles"; break;
            case PERF_COUNT_HW_INSTRUCTIONS: str = "instructions"; break;
            case PERF_COUNT_HW_CACHE_REFERENCES: str = "cache-references"; break;
            case PERF_COUNT_HW_CACHE_MISSES: str = "cache-misses"; break;
            case PERF_COUNT_HW_BRANCH_INSTRUCTIONS: str = "branch-instructions"; break;
            case PERF_COUNT_HW_BRANCH_MISSES: str = "branch-misses"; break;
            case PERF_COUNT_HW_BUS_CYCLES: str = "bus-cycles"; break;
            case PERF_COUNT_HW_STALLED_CYCLES_FRONTEND: str = "stalled-frontend"; break;
            case PERF_COUNT_HW_STALLED_CYCLES_BACKEND: str = "stalled-backend"; break;
            case PERF_COUNT_HW_REF_CPU_CYCLES: str = "ref-cpu-cycles"; break;
            default: break;
        }
        dev_printf("- %s\n", str);
    } else if (attr->type == PERF_TYPE_SOFTWARE) {
        switch (attr->config) {
            case PERF_COUNT_SW_CPU_CLOCK: str = "cpu-clock"; break;
            case PERF_COUNT_SW_TASK_CLOCK: str = "task-clock"; break;
            case PERF_COUNT_SW_PAGE_FAULTS: str = "page-faults"; break;
            case PERF_COUNT_SW_CONTEXT_SWITCHES: str = "context-switches"; break;
            case PERF_COUNT_SW_CPU_MIGRATIONS: str = "cpu-migrations"; break;
            case PERF_COUNT_SW_PAGE_FAULTS_MIN: str = "page-faults-min"; break;
            case PERF_COUNT_SW_PAGE_FAULTS_MAJ: str = "page-faults-maj"; break;
            case PERF_COUNT_SW_ALIGNMENT_FAULTS: str = "alignment-faults"; break;
            case PERF_COUNT_SW_EMULATION_FAULTS: str = "emulation-faults"; break;
            case PERF_COUNT_SW_DUMMY: str = "dummy"; break;
            case PERF_COUNT_SW_BPF_OUTPUT: str = "bpf-output"; break;
            case PERF_COUNT_SW_CGROUP_SWITCHES: str = "cgroup-switches"; break;
            default: break;
        }
        dev_printf("- %s\n", str);
    } else if (attr->type == PERF_TYPE_TRACEPOINT) {
        struct tep_event *e = tep_find_event(tep__ref_light(), (int)attr->config);
        if (e) dev_printf("- %s:%s\n", e->system, e->name);
        tep__unref();
    } else if (attr->type == PERF_TYPE_RAW) {
        dev_printf("- raw:0x%lx\n", (long)attr->config);
    } else if (attr->type == PERF_TYPE_BREAKPOINT) {
        dev_printf("- breakpoint\n");
    } else {
        dev_printf("- %s\n", perf_type_str(attr->type) ?: "unknown");
    }

    if (member_cache) {
        dev_printf("    sample_type:");
        for (i = 0; i < member_cache->nr_members; i++) {
            struct perf_event_member *m = &member_cache->members[i];
            printf(" %s", m->name);
        }
        printf("\n");
    }
}

static void print_thread(struct perf_thread_map *threads)
{
    int pid, pid_1 = -1, pid_start = -1;
    int idx;

    perf_thread_map__for_each_thread(pid, idx, threads) {
        if (idx == 0) {
            printf("%d", pid);
            pid_1 = pid_start = pid;
            continue;
        }
        // The pids are sorted from small to large and can be used to
        // judge whether they are numerically continuous.
        if (pid_1 + 1 != pid) {
            if (pid_start == pid_1) printf(",%d", pid);
            else printf("-%d,%d", pid_1, pid);
            pid_start = pid;
        }
        pid_1 = pid;
    }
    if (pid_start != pid_1)
        printf("-%d", pid_1);
}

static void print_dev(struct prof_dev *dev, int indent)
{
    struct prof_dev *source, *child, *tmp;
    struct perf_evsel *evsel;
    char *cpu_str = perf_cpu_map__string(dev->cpus);

    printf("%*s- %s:\n", indent-4, "", dev->prof->name);
    dev_printf("state: %s\n", prof_dev_state(dev));
    dev_printf("cpu: %s\n", cpu_str);
    dev_printf("thread: "); print_thread(dev->threads); printf("\n");
    if (dev->env->workload.pid) dev_printf("workload: %d\n", dev->env->workload.pid);
    dev_printf("event:\n");
    perf_evlist__for_each_evsel(dev->evlist, evsel) {
        print_event(evsel, indent);
    }
    dev_printf("ringbuffer_size: %lu\n", (u64)dev->pages * page_size * prof_dev_nr_ins(dev));
    dev_printf("users: %d\n", dev->dev_users);
    dev_printf("refcount: %d\n", dev->refcount - (indent>4) /* for_each_child_dev_get */);
    dev_printf("clone: %s\n", dev->clone ? "true" : "false");
    if (dev->convert.need_conv) {
        dev_printf("convert:");
        if (dev->convert.need_conv == CONVERT_TO_TSC)
            printf(" tsc");
        else if (dev->convert.need_conv == CONVERT_TO_KVMCLOCK)
            printf(" kvmclock");
        printf(" +%lu\n", dev->env->clock_offset);
    }
    if (using_order(dev)) {
        dev_printf("wakeup_watermark: %lu\n", dev->order.wakeup_watermark);
        dev_printf("order: unordered %lu fixed %lu\n", dev->order.nr_unordered_events,
                    dev->order.nr_fixed_events);
        dev_printf("order: lost %lu maybe %lu pause %lu pause_time %lu\n",
                    dev->order.nr_lost, dev->order.nr_maybe_lost,
                    dev->order.nr_maybe_lost_pause, dev->order.maybe_lost_pause_time);
        if (dev->order.nr_streams)
            dev_printf("order: stream pause %lu pause_time %lu\n",
                    dev->order.nr_stream_pause, dev->order.stream_pause_time);
    }
    ptrace_print(dev, indent);
    if (dev->prof->print_dev)
        dev->prof->print_dev(dev, indent);

    free(cpu_str);

    if (!list_empty(&dev->forward.source_list)) {
        dev_printf("forward_source:\n");
        for_each_source_dev_get(source, tmp, dev)
            print_dev(source, indent + 4);
    }
    if (!list_empty(&dev->links.child_list)) {
        bool child_pr = false;
        for_each_child_dev_get(child, tmp, dev) {
            if (child->forward.target != dev) {
                if (!child_pr) {
                    dev_printf("child:\n");
                    child_pr = true;
                }
                print_dev(child, indent + 4);
            }
        }
    }
}

static void print_devtree(void)
{
    struct prof_dev *dev, *next;
    printf("running: %d\n", running);
    list_for_each_entry_safe(dev, next, &prof_dev_list, dev_link)
        if (!dev->links.parent)
            print_dev(dev, 4);
}

static void sigusr2_handler(int sig)
{
    static unsigned long utime = 0, stime = 0;
    static struct timeval tv;
    static bool init = 0;
    struct rusage usage;

    if (init == 0) {
        tzset(); /* Now 'timezone' global is populated. */
        daylight_active = daylight;
        gettimeofday(&tv, NULL);
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            utime = usage.ru_utime.tv_sec * 1000000UL + usage.ru_utime.tv_usec;
            stime = usage.ru_stime.tv_sec * 1000000UL + usage.ru_stime.tv_usec;
        } else
            return ;
        init = 1;
        return ;
    }

    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        unsigned long user, sys, us;
        struct timeval t;
        char timebuff[64];
        struct tm result;

        gettimeofday(&t, NULL);
        us = t.tv_sec * 1000000UL + t.tv_usec - tv.tv_sec * 1000000UL - tv.tv_usec;
        user = usage.ru_utime.tv_sec * 1000000UL + usage.ru_utime.tv_usec - utime;
        sys = usage.ru_stime.tv_sec * 1000000UL + usage.ru_stime.tv_usec - stime;

        nolocks_localtime(&result, t.tv_sec, timezone, daylight_active);
        strftime(timebuff, sizeof(timebuff), "%Y-%m-%d %H:%M:%S", &result);
        printf("%s.%06u CPU %%usr %.2f %%sys %.2f MAXRSS %luk\n", timebuff, (unsigned int)tv.tv_usec,
                user*100.0/us, sys*100.0/us, usage.ru_maxrss);

        utime += user;
        stime += sys;
        tv = t;
    }
}

static void usage_self_handle(struct timer *timer)
{
    sigusr2_handler(0);
}

static void prof_dev_winsize(struct prof_dev *new)
{
    struct prof_dev *dev, *next;
    struct winsize size;
    int n;
    bool shared;

    if (isatty(STDOUT_FILENO) &&
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
        n = new && list_empty(&new->dev_link);
        list_for_each_entry(dev, &prof_dev_list, dev_link)
            n ++;
        shared = n > 1;
        list_for_each_entry_safe(dev, next, &prof_dev_list, dev_link) {
            if (dev != new && dev->prof->sigusr) {
                dev->tty.shared = shared;
                dev->tty.row = size.ws_row;
                dev->tty.col = size.ws_col;
                dev->prof->sigusr(dev, SIGWINCH); // May cause disable the alternative buffer.
            }
            n ++;
        }
        dev = new;
        if (dev && dev->prof->sigusr) {
            dev->tty.istty = true;
            dev->tty.shared = shared;
            dev->tty.row = size.ws_row;
            dev->tty.col = size.ws_col;
        }
    }
}

static bool __continue(struct timeval *start)
{
    struct timeval tv;
    u64 us;
    gettimeofday(&tv, NULL);
    us = tv.tv_sec*1000000UL+tv.tv_usec - (start->tv_sec*1000000UL+start->tv_usec);
    return us < 100000UL /*100ms*/; // The interval timer delays up to 100ms.
}

static void handle_SIGCHLD(void)
{
    static int USE_WAITID = 1;
    int pid;
    int code; // same as siginfo_t::si_code
    int status; // same as siginfo_t::si_status
    const char * __maybe_unused str = NULL;
    bool more_child = true;
    struct timeval start_time;
    int priority = getpriority(PRIO_PROCESS, 0);

    gettimeofday(&start_time, NULL);
    setpriority(PRIO_PROCESS, 0, -20 /*highest priority*/);
    while (__continue(&start_time)) {
        if (USE_WAITID) {
            siginfo_t siginfo = {.si_pid = 0};
            if (waitid(P_ALL, 0, &siginfo, WEXITED | WSTOPPED | WCONTINUED | WNOHANG | __WALL) < 0 &&
                errno == EINVAL) {
                USE_WAITID = 0;
                continue;
            }
            if (siginfo.si_pid == 0) {
                more_child = false;
                break;
            }

            pid = siginfo.si_pid;
            code = siginfo.si_code;
            status = siginfo.si_status;
        } else {
            pid = waitpid(-1, &status, WUNTRACED | WCONTINUED | WNOHANG | __WALL);
            if (pid <= 0) {
                more_child = false;
                break;
            }

            if (WIFSTOPPED(status)) {
                code = CLD_TRAPPED; // use ptrace, not CLD_STOPPED
                status >>= 8;
            } else if (WIFEXITED(status)) {
                code = CLD_EXITED;
                status = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                code = WCOREDUMP(status) ? CLD_DUMPED : CLD_KILLED;
                status = WTERMSIG(status);
            } else if (WIFCONTINUED(status)) {
                code = CLD_CONTINUED;
                status = SIGCONT;
            } else
                break;
        }

        switch (code) {
        case CLD_EXITED:    str = "EXITED return";  break;
        case CLD_KILLED:    str = "KILLED sig";     break;
        case CLD_DUMPED:    str = "DUMPED sig";     break;
        case CLD_TRAPPED:                           break;
        case CLD_STOPPED:   str = "STOPPED sig";    break;
        case CLD_CONTINUED: str = "CONTINUED sig";  break;
        default: continue;
        }
        if (code != CLD_TRAPPED)
            d_printf("CHILD %d %s %d\n", pid, str, status);

        switch (code) {
        case CLD_EXITED:
        case CLD_KILLED:
        case CLD_DUMPED: {
            struct prof_dev *dev, *next;
            /*
             * ptrace_detach() needs to know the exit of the process.
             * Call chain:
             *  ptrace_exited() ->
             *      __ptrace_unlink() ->
             *          prof_dev_unuse() ->
             *              prof_dev_close() ->
             *                  ptrace_detach() # dev->ptrace_list
             *
             * ptrace uses @dev_users to use prof_dev to prevent it from being closed.
             * For the exited process, it may not be closed in ptrace_exited(), so we
             * can't close it directly.
             */
            ptrace_exited(pid);
            list_for_each_entry_safe(dev, next, &prof_dev_list, dev_link)
                if (prof_dev_is_final(dev) && dev->env->workload.pid == pid &&
                    !ptraced_dev(dev)) {
                    dev->env->workload.pid = 0;
                    // Automatically close prof_dev after the workload exits.
                    prof_dev_close(dev);
                    break;
                }
            }
            break;
        case CLD_TRAPPED:
            ptrace_stop(pid, status);
            break;
        default:
            break;
        }
    }
    setpriority(PRIO_PROCESS, 0, priority);
    if (more_child)
        kill(getpid(), SIGCHLD);
}

static void handle_signal(int fd, unsigned int revents, void *ptr)
{
    struct prof_dev *dev, *next;
    struct signalfd_siginfo fdsi;
    int s;

    s = read(fd, &fdsi, sizeof(struct signalfd_siginfo));
    if (s != sizeof(fdsi)) return ;

    switch (fdsi.ssi_signo) {
        case SIGCHLD:
            handle_SIGCHLD();
            break;
        case SIGINT:
        case SIGTERM:
            running = -1;
            break;
        case SIGUSR1: {
                list_for_each_entry_safe(dev, next, &prof_dev_list, dev_link) {
                    prof_dev_reopen_output(dev);
                    if (dev->prof->sigusr)
                        dev->prof->sigusr(dev, SIGUSR1);
                }
            }
            break;
        case SIGUSR2:
            sigusr2_handler(SIGUSR2);
            print_devtree();
            unique_string_stat(stdout);
            obj__stat(stdout);
        #ifdef HAVE_LIBTCMALLOC
            {
                char buff[8192];
                MallocExtension_GetStats(buff, sizeof(buff));
                write(STDOUT_FILENO, buff, strlen(buff));
            }
        #endif
        #ifdef HAVE_RPMALLOC
            rpmalloc_dump_statistics(stdout);
        #endif
            break;
        case SIGWINCH:
            prof_dev_winsize(NULL);
            break;
        default:
            break;
    }
}

static int epoll_wait_signal(int signum, ...)
{
    va_list ap;
    sigset_t mask;
    int sfd = -1;

    sigemptyset(&mask);
    sigaddset(&mask, signum);

    va_start(ap, signum);
    while ((signum = va_arg(ap, int)))
        sigaddset(&mask, signum);
    va_end(ap);

    sigprocmask(SIG_BLOCK, &mask, NULL);

    if ((sfd = signalfd(-1, &mask, SFD_NONBLOCK|SFD_CLOEXEC)) < 0) goto e1;
    if (main_epoll_add(sfd, EPOLLIN, NULL, handle_signal) < 0) goto e2;
    return 0;

e2: close(sfd);
e1: sigprocmask(SIG_UNBLOCK, &mask, NULL);
    return -1;
}

int get_present_cpus(void)
{
    struct perf_cpu_map *cpumap = NULL;
    FILE *f;
    int cpus;

    f = fopen("/sys/devices/system/cpu/present", "r");
    if (!f)
        f = fopen("/sys/devices/system/cpu/online", "r");
    cpumap = perf_cpu_map__read(f);
    fclose(f);

    cpus = perf_cpu_map__max(cpumap);
    perf_cpu_map__put(cpumap);
    return cpus + 1;
}

int get_tsc_khz(void)
{
    int kvm, vm, vcpu;
    int tsc_khz = 0;

    kvm = open("/dev/kvm", O_RDWR);
    if (kvm < 0) {
        fprintf(stderr, "open kvm failed\n");
        return 0;
    }
    vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (kvm < 0) {
        fprintf(stderr, "create vm failed\n");
        close(kvm);
        return 0;
    }
    vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
    if (vcpu < 0) {
        fprintf(stderr, "create vm failed\n");
        close(vm);
        close(kvm);
        return 0;
    }
    if (ioctl(vm, KVM_CHECK_EXTENSION, KVM_CAP_GET_TSC_KHZ) == 1) {
        tsc_khz = ioctl(vcpu, KVM_GET_TSC_KHZ, 0);
        if (tsc_khz < 0)
            tsc_khz = 0;
    }
    close(vcpu);
    close(vm);
    close(kvm);
    return tsc_khz;
}

int get_cpuinfo(struct cpuinfo *info)
{
#if defined(__i386__) || defined(__x86_64__)
    __u32 eax, ebx, ecx, edx;

    eax = ebx = ecx = edx = 0;
    __get_cpuid(0, &eax, &ebx, &ecx, &edx);

    if (info)
        memset(info, 0, sizeof(*info));
    if (ebx == 0x756e6547 && ecx == 0x6c65746e && edx == 0x49656e69) {
        if (info && eax > 1) {
            int family, model, stepping;
            __get_cpuid(1, &eax, &ebx, &ecx, &edx);
            family = (eax >> 8) & 0xf;
            model = (eax >> 4) & 0xf;
            stepping = eax & 0xf;
            if (family == 0x6 || family == 0xf)
                model += ((eax > 16) & 0xf) << 4;
            if (family == 0xf)
                family += (eax >> 20) & 0xff;
            info->vendor = X86_VENDOR_INTEL;
            info->family = family;
            info->model = model;
            info->stepping = stepping;
        }
        return X86_VENDOR_INTEL;
    } else if (ebx == 0x68747541 && ecx == 0x444d4163 && edx == 0x69746e65) {
        if (info && eax > 1) {
            int family, model, stepping;
            __get_cpuid(1, &eax, &ebx, &ecx, &edx);
            family = (eax >> 8) & 0xf;
            model = (eax >> 4) & 0xf;
            stepping = eax & 0xf;
            if (family == 0xf) {
                model += ((eax > 16) & 0xf) << 4;
                family += (eax >> 20) & 0xff;
            }
            info->vendor = X86_VENDOR_AMD;
            info->family = family;
            info->model = model;
            info->stepping = stepping;
        }
        return X86_VENDOR_AMD;
    } else if (ebx == 0x6f677948 && ecx == 0x656e6975 && edx == 0x6e65476e) {
        return X86_VENDOR_HYGON;
    } else
        return -1;
#elif defined(__aarch64__)
    if (info) {
        memset(info, 0, sizeof(*info));
        info->vendor = ARM64_VENDER;
    }
    return ARM64_VENDER;
#else
    if (info)
        memset(info, 0, sizeof(*info));
    return -1;
#endif
}

#define CPUID_EXT_HYPERVISOR  (1U << 31)
int in_guest(void)
{
#if defined(__i386__) || defined(__x86_64__)

    __u32 eax, ebx, ecx, edx;

    eax = ebx = ecx = edx = 0;
    __get_cpuid(1, &eax, &ebx, &ecx, &edx);

    return !!(ecx & CPUID_EXT_HYPERVISOR);
#else
    return 0;
#endif
}

int kernel_release(void)
{
    struct utsname kernel_info;
    int major, minor, patch;

    if (uname(&kernel_info) == 0 &&
        sscanf(kernel_info.release, "%d.%d.%d", &major, &minor, &patch) == 3) {
        return KERNEL_VERSION(major, minor, patch);
    }
    return -1;
}

/*
 * BPF JIT Symbols Management Function
 *
 * Background: In early Linux kernels, /proc/kallsyms doesn't include BPF JIT
 * compiled function symbols by default. The net.core.bpf_jit_kallsyms kernel
 * parameter needs to be enabled to include these symbols. Since kernel 5.5,
 * BPF JIT symbols are included by default, no longer depending on this switch.
 *
 * Additionally: Enabling bpf_jit_kallsyms ensures bpf_get_stack can correctly
 * retrieve the stack trace. Kernel call path:
 *   unwind_get_return_address->
 *     __kernel_text_address->
 *       is_bpf_text_address returns true
 */
void sysctl_bpf_jit_kallsyms(bool enable)
{
    const char *sysctl_jit_enable = "net/core/bpf_jit_enable";
    const char *sysctl_jit_kallsyms = "net/core/bpf_jit_kallsyms";
    static int bpf_jit_enable = 0;
    static int bpf_jit_kallsyms = 0;
    static int ref = 0;

    /* Kernel 5.5+ includes BPF JIT symbols by default, no manual handling needed */
    /* Kernel commit af91acb: "bpf: Fix bpf jit kallsym access" fixed this issue */
    if (kernel_release() >= KERNEL_VERSION(5, 5, 0))
        return;

    if (enable) {
        ref++;
        if (ref == 1 &&
            sysctl__read_int(sysctl_jit_enable, &bpf_jit_enable) == 0) {
            sysctl__read_int(sysctl_jit_kallsyms, &bpf_jit_kallsyms);
            if (bpf_jit_enable && !bpf_jit_kallsyms)
                sysctl__write_int(sysctl_jit_kallsyms, 1);
        }
    } else {
        if (ref > 0) ref--;
        if (ref == 0) {
            if (bpf_jit_enable && !bpf_jit_kallsyms)
                sysctl__write_int(sysctl_jit_kallsyms, 0);
        }
    }
}

int callchain_flags(struct prof_dev *dev, int dflt_flags)
{
    int flags = dflt_flags;

    if (dev->env->user_callchain_set) {
        if (dev->env->user_callchain)
            flags |= CALLCHAIN_USER;
        else
            flags &= ~CALLCHAIN_USER;
    }
    if (dev->env->kernel_callchain_set) {
        if (dev->env->kernel_callchain)
            flags |= CALLCHAIN_KERNEL;
        else
            flags &= ~CALLCHAIN_KERNEL;
    }
    return flags;
}

int exclude_callchain_user(struct prof_dev *dev, int dflt_flags)
{
    int flags = callchain_flags(dev, dflt_flags);
    return flags & CALLCHAIN_USER ? 0 : 1;
}

int exclude_callchain_kernel(struct prof_dev *dev, int dflt_flags)
{
    int flags = callchain_flags(dev, dflt_flags);
    return flags & CALLCHAIN_KERNEL ? 0 : 1;
}

static int workload_prepare(struct workload *workload, char *argv[])
{
    int child_ready_pipe[2], go_pipe[2];
    char bf;

    if (pipe(child_ready_pipe) < 0) {
        perror("failed to create 'ready' pipe");
        return -1;
    }

    if (pipe(go_pipe) < 0) {
        perror("failed to create 'go' pipe");
        goto out_close_ready_pipe;
    }

    workload->pid = fork();
    if (workload->pid < 0) {
        perror("failed to fork");
        goto out_close_pipes;
    }

    if (!workload->pid) {
        int ret;

        signal(SIGTERM, SIG_DFL);

        close(child_ready_pipe[0]);
        close(go_pipe[1]);
        fcntl(go_pipe[0], F_SETFD, FD_CLOEXEC);

        /*
         * Change the name of this process not to confuse --exclude-perf users
         * that sees 'perf' in the window up to the execvp() and thinks that
         * perf samples are not being excluded.
         */
        prctl(PR_SET_NAME, "perf-exec");

        /*
         * Kill me when my parent dies.
         */
        prctl(PR_SET_PDEATHSIG, SIGKILL);

        // Disable HEAPCHECK.
        // See: https://gperftools.github.io/gperftools/heap_checker.html
        unsetenv("HEAPCHECK");

        /*
         * Tell the parent we're ready to go
         */
        close(child_ready_pipe[1]);

        /*
         * Wait until the parent tells us to go.
         */
        ret = read(go_pipe[0], &bf, 1);
        /*
         * The parent will ask for the execvp() to be performed by
         * writing exactly one byte, in workload.cork_fd, usually via
         * evlist__start_workload().
         *
         * For cancelling the workload without actually running it,
         * the parent will just close workload.cork_fd, without writing
         * anything, i.e. read will return zero and we just exit()
         * here.
         */
        if (ret != 1) {
            if (ret == -1)
                perror("unable to read pipe");
            exit(ret);
        }

        execvp(argv[0], (char **)argv);

        exit(-1);
    }

    close(child_ready_pipe[1]);
    close(go_pipe[0]);
    /*
     * wait for child to settle
     */
    if (read(child_ready_pipe[0], &bf, 1) == -1) {
        perror("unable to read pipe");
        goto out_close_pipes;
    }

    fcntl(go_pipe[1], F_SETFD, FD_CLOEXEC);
    workload->cork_fd = go_pipe[1];
    close(child_ready_pipe[0]);
    return 0;

out_close_pipes:
    close(go_pipe[0]);
    close(go_pipe[1]);
out_close_ready_pipe:
    close(child_ready_pipe[0]);
    close(child_ready_pipe[1]);
    return -1;
}

static int workload_start(struct workload *workload)
{
    if (workload->cork_fd > 0) {
        char bf = 0;
        int ret;
        /*
         * Remove the cork, let it rip!
         */
        ret = write(workload->cork_fd, &bf, 1);
        if (ret < 0)
            perror("unable to write to pipe");
        else
            global_comm_flush(workload->pid); // flush pid's comm, "perf-exec" is temporary.

        close(workload->cork_fd);
        return ret;
    }

    return 0;
}

void print_time(FILE *fp)
{
    char timebuff[64];
    struct timeval tv;
    struct tm *result;

    gettimeofday(&tv, NULL);
    result = localtime(&tv.tv_sec);
    daylight_active = result->tm_isdst;
    strftime(timebuff, sizeof(timebuff), "%Y-%m-%d %H:%M:%S", result);
    fprintf(fp, "%s.%06u ", timebuff, (unsigned int)tv.tv_usec);
}

static inline void print_lost_events(struct prof_dev *dev)
{
    if (unlikely(dev->lost_events)) {
        print_time(stderr);
        fprintf(stderr, "%s: lost %lu events (total)\n", dev->prof->name, dev->lost_events);
        dev->lost_events = 0;
    }
}

void print_lost_fn(struct prof_dev *dev, union perf_event *event, int ins)
{
    struct env *env = dev->env;
    if (env->exit_n) return;
    if (unlikely(env->verbose >= VERBOSE_NOTICE)) {
        int oncpu = prof_dev_ins_oncpu(dev);
        print_time(stderr);
        fprintf(stderr, "%s: lost %llu events on %s #%d\n", dev->prof->name, event->lost.lost,
                        oncpu ? "CPU" : "thread",
                        oncpu ? prof_dev_ins_cpu(dev, ins) : prof_dev_ins_thread(dev, ins));
    } else {
        dev->lost_events += event->lost.lost;
        if (!env->interval) {
            u64 lost_time = get_ktime_ns();
            if (dev->lost_print_time == 0 || lost_time - dev->lost_print_time > NSEC_PER_SEC) {
                dev->lost_print_time = lost_time;
                print_lost_events(dev);
            }
        }
    }
}

static void print_fork_exit_fn(struct prof_dev *dev, union perf_event *event, int ins, int exit)
{
    if (dev->env->verbose >= VERBOSE_ALL) {
        int oncpu = prof_dev_ins_oncpu(dev);
        print_time(stderr);
        fprintf(stderr, "%s: %s ppid %u ptid %u pid %u tid %u on %s #%d\n", dev->prof->name,
                        exit ? "exit" : "fork",
                        event->fork.ppid, event->fork.ptid,
                        event->fork.pid,  event->fork.tid,
                        oncpu ? "CPU" : "thread",
                        oncpu ? prof_dev_ins_cpu(dev, ins) : prof_dev_ins_thread(dev, ins));
    }
}

static void print_comm_fn(struct prof_dev *dev, union perf_event *event, int ins)
{
    if (dev->env->verbose >= VERBOSE_ALL) {
        int oncpu = prof_dev_ins_oncpu(dev);
        print_time(stderr);
        fprintf(stderr, "%s: comm pid %u tid %u %s on %s #%d\n", dev->prof->name,
                        event->comm.pid,  event->comm.tid,
                        event->comm.comm,
                        oncpu ? "CPU" : "thread",
                        oncpu ? prof_dev_ins_cpu(dev, ins) : prof_dev_ins_thread(dev, ins));
    }
}

static void print_throttle_unthrottle_fn(struct prof_dev *dev, union perf_event *event, int ins, int unthrottle)
{
    if (dev->env->verbose >= VERBOSE_NOTICE) {
        int oncpu = prof_dev_ins_oncpu(dev);
        prof_dev_print_time(dev, event->throttle.time, stderr);
        fprintf(stderr, "%s: %llu.%06llu: %s events on %s #%d\n", dev->prof->name,
                        event->throttle.time / NSEC_PER_SEC, (event->throttle.time % NSEC_PER_SEC)/1000,
                        unthrottle ? "unthrottle" : "throttle",
                        oncpu ? "CPU" : "thread",
                        oncpu ? prof_dev_ins_cpu(dev, ins) : prof_dev_ins_thread(dev, ins));
    }
}

static void print_context_switch_fn(struct prof_dev *dev, union perf_event *event, int ins)
{
    if (dev->env->verbose >= VERBOSE_ALL) {
        int oncpu = prof_dev_ins_oncpu(dev);
        print_time(stderr);
        fprintf(stderr, "%s: switch on %s #%d\n", oncpu ? "CPU" : "thread", dev->prof->name,
                        oncpu ? prof_dev_ins_cpu(dev, ins) : prof_dev_ins_thread(dev, ins));
    }
}

static void print_context_switch_cpu_fn(struct prof_dev *dev, union perf_event *event, int ins)
{
    if (dev->env->verbose >= VERBOSE_ALL) {
        int oncpu = prof_dev_ins_oncpu(dev);
        print_time(stderr);
        fprintf(stderr, "%s: switch next pid %u tid %u on %s #%d\n", dev->prof->name,
                        event->context_switch.next_prev_pid, event->context_switch.next_prev_tid,
                        oncpu ? "CPU" : "thread",
                        oncpu ? prof_dev_ins_cpu(dev, ins) : prof_dev_ins_thread(dev, ins));
    }
}

/*
 * perf_event_forward - Forward a PERF_RECORD_SAMPLE event to the target device.
 *
 * This function handles the complete preprocessing of events before forwarding:
 *   1. ftrace filter check - filter events in userspace if kernel filter failed
 *   2. Python stack attachment - attach Python callchain if enabled
 *   3. Event encapsulation - wrap event into PERF_RECORD_DEV format
 *   4. Timestamp conversion - convert to target clock domain
 *   5. enabled_after check - skip events before the enabled time
 *   6. exit_n accounting - count sampled events and close source device if limit reached
 *
 * The target device will use prof_dev_print_event() to print the forwarded event,
 * and its own exit_n setting controls the target device's lifecycle independently.
 *
 * Returns the wrapped PERF_RECORD_DEV event, or NULL if the event should be skipped.
 */
static inline union perf_event *
perf_event_forward(struct prof_dev *dev, union perf_event *event, int *instance, bool *writable, bool *converted)
{
    struct perf_record_dev *event_dev = (void *)dev->forward.event_dev;
    void *data;
    int header_size = sizeof(struct perf_record_dev);
    bool add_pystack = false;

    // 1. Userspace ftrace filter: skip events that don't match the filter expression.
    if (unlikely(dev->ftrace_filter &&
                 dev->prof->ftrace_filter(dev, event, *instance) <= 0))
        return NULL;

    // 2. Python stack: attach Python callchain to the event if enabled.
    if (unlikely(dev->links.pystack)) {
        union perf_event *py_event = pystack_perf_event(dev, event, writable, header_size);
        if (py_event != event) {
            event_dev = (void *)py_event - header_size;
            event = py_event;
            add_pystack = true;
        }
    }

    // 3. Event encapsulation: wrap the event into PERF_RECORD_DEV format.
    event_dev->header.size = header_size + event->header.size;
    event_dev->header.misc = 0;
    event_dev->header.type = PERF_RECORD_DEV;

    // Extract fields from the sample for quick access by target device.
    data = (void *)event->sample.array;
    event_dev->pid = *(u32 *)(data + dev->pos.tid_pos);
    event_dev->tid = *(u32 *)(data + dev->pos.tid_pos + sizeof(u32));
    if (dev->pos.id_pos >= 0)
        event_dev->id = *(u64 *)(data + dev->pos.id_pos);
    event_dev->cpu = dev->pos.cpu_pos >= 0 ? *(u32 *)(data + dev->pos.cpu_pos) : perf_cpu_map__cpu(dev->cpus, *instance);
    event_dev->instance = *instance;
    event_dev->dev = dev;
    event_dev->event = event;

    // 4. Timestamp conversion: convert to target clock domain.
    if (unlikely(dev->convert.need_conv) && !*converted) {
        if (!add_pystack) {
            event_dev->event = (void *)(event_dev + 1);
            memcpy(event_dev->event, event, event->header.size);
        }
        perf_event_convert(dev, event_dev->event, true);
        // Read time from converted event
        data = (void *)event_dev->event->sample.array;
    }
    event_dev->time = *(u64 *)(data + dev->pos.time_pos);

    // 5. enabled_after check: skip events before the source device's enabled time.
    if (unlikely(event_dev->time < dev->time_ctx.enabled_after.clock))
        return NULL;

    // 6. exit_n accounting: count and close source device if limit reached.
    //    The target device has its own exit_n for independent lifecycle control.
    if (unlikely(dev->env->exit_n &&
                 ++dev->sampled_events > dev->env->exit_n)) {
        prof_dev_close(dev);
        return NULL;
    }

    if (dev->forward.ins_reset)
        *instance = 0;

    // Mark event as writable (copied to event_dev buffer) and converted.
    *writable = 1;
    *converted = 1;
    return (union perf_event *)event_dev;
}

int perf_event_process_record(struct prof_dev *dev, union perf_event *event, int instance, bool writable, bool converted)
{
    profiler *prof;
    struct env *env;

    if (dev->forward.target) {
        // Forward upward.
        if (event->header.type == PERF_RECORD_SAMPLE) {
            event = perf_event_forward(dev, event, &instance, &writable, &converted);
            if (event == NULL)
                return 0;
        }
        // Only PERF_RECORD_SAMPLE events are forwarded.
        if (event->header.type == PERF_RECORD_DEV)
            dev = dev->forward.target;
    } else if (event->header.type == PERF_RECORD_DEV) {
        // Return down.
        struct perf_record_dev *event_dev = (void *)event;
        event = event_dev->event;
        dev = event_dev->dev;
        instance = event_dev->instance;
        converted = true;
    } else if (unlikely(dev->links.pystack) &&
               event->header.type == PERF_RECORD_SAMPLE)
        event = pystack_perf_event(dev, event, &writable, 0);

    prof = dev->prof;
    env = dev->env;

    switch (event->header.type) {
    case PERF_RECORD_MMAP:
        if (prof->mmap)
            prof->mmap(dev, event, instance);
        break;
    case PERF_RECORD_MMAP2:
        if (prof->mmap2)
            prof->mmap2(dev, event, instance);
        break;
    case PERF_RECORD_LOST:
        if (prof->lost)
            prof->lost(dev, event, instance, 0, 0);
        else
            print_lost_fn(dev, event, instance);
        break;
    case PERF_RECORD_FORK:
        if (prof->fork)
            prof->fork(dev, event, instance);
        else
            print_fork_exit_fn(dev, event, instance, 0);
        break;
    case PERF_RECORD_COMM:
        if (prof->comm)
            prof->comm(dev, event, instance);
        else
            print_comm_fn(dev, event, instance);
        break;
    case PERF_RECORD_EXIT:
        if (prof->exit)
            prof->exit(dev, event, instance);
        else
            print_fork_exit_fn(dev, event, instance, 1);
        break;
    case PERF_RECORD_THROTTLE:
        if (prof->throttle)
            prof->throttle(dev, event, instance);
        else
            print_throttle_unthrottle_fn(dev, event, instance, 0);
        break;
    case PERF_RECORD_UNTHROTTLE:
        if (prof->unthrottle)
            prof->unthrottle(dev, event, instance);
        else
            print_throttle_unthrottle_fn(dev, event, instance, 1);
        break;
    case PERF_RECORD_DEV:
    case PERF_RECORD_SAMPLE:
        perfeval_sample(dev, event, instance);
        if (likely(!env->exit_n) || ++dev->sampled_events <= env->exit_n) {
            if (prof->sample) {
                if (unlikely(dev->ftrace_filter && event->header.type == PERF_RECORD_SAMPLE &&
                             prof->ftrace_filter(dev, event, instance) <= 0))
                    goto __break;

                if (likely(!converted))
                    event = perf_event_convert(dev, event, writable);

                if (dev->pos.sample_type & PERF_SAMPLE_TIME) {
                    dev->time_ctx.last_evtime.clock = *(u64 *)((void *)event->sample.array + dev->pos.time_pos);
                    if (unlikely(dev->time_ctx.last_evtime.clock < dev->time_ctx.enabled_after.clock)) {
                    __break:
                        if (dev->sampled_events > 0)
                            dev->sampled_events --;
                        break;
                    }
                }

                prof->sample(dev, event, instance);
            }
        }
        if (unlikely(env->exit_n) && dev->sampled_events >= env->exit_n)
            prof_dev_close(dev);
        break;
    case PERF_RECORD_SWITCH:
        if (prof->context_switch)
            prof->context_switch(dev, event, instance);
        else
            print_context_switch_fn(dev, event, instance);
        break;
    case PERF_RECORD_SWITCH_CPU_WIDE:
        if (prof->context_switch_cpu)
            prof->context_switch_cpu(dev, event, instance);
        else
            print_context_switch_cpu_fn(dev, event, instance);
        break;
    case PERF_RECORD_NAMESPACES:
        if (prof->namespaces)
            prof->namespaces(dev, event, instance);
        break;
    case PERF_RECORD_KSYMBOL:
        if (prof->ksymbol)
            prof->ksymbol(dev, event, instance);
        break;
    case PERF_RECORD_BPF_EVENT:
        if (prof->bpf_event)
            prof->bpf_event(dev, event, instance);
        break;
    case PERF_RECORD_CGROUP:
        if (prof->cgroup)
            prof->cgroup(dev, event, instance);
        break;
    case PERF_RECORD_TEXT_POKE:
        if (prof->text_poke)
            prof->text_poke(dev, event, instance);
        break;
    case PERF_RECORD_ORDER_TIME:
        break;
    default:
        if (likely(!env->exit_n) || ++dev->sampled_events <= env->exit_n)
            fprintf(stderr, "unknown perf sample type %d\n", event->header.type);
        if (unlikely(env->exit_n) && dev->sampled_events >= env->exit_n)
            prof_dev_close(dev);
        return -1;
    }
    return 0;
}

static void perf_event_handle_mmap(struct prof_dev *dev, struct perf_mmap *map)
{
    union perf_event *event;
    bool writable = false;
    int idx;

    if (dev->order.enabled) {
        order_mmap(dev, map);
        return;
    }

    if (perf_mmap__read_init(map) < 0)
        return;

    idx = perf_mmap__idx(map);
    perf_event_convert_read_tsc_conversion(dev, map);
    while ((event = perf_mmap__read_event(map, &writable)) != NULL) {
        /* process event */
        perf_event_process_record(dev, event, idx, writable, false);
        perf_mmap__consume(map);
    }
    perf_mmap__read_done(map);
}

static void perf_event_handle(int fd, unsigned int revents, void *ptr)
{
    struct prof_dev *dev = perf_evlist_poll__get_external(NULL, ptr);

    prof_dev_get(dev);
    perf_event_handle_mmap(dev, ptr);
    if (revents & EPOLLHUP) {
        main_epoll_del(fd);
        dev->nr_pollfd --;
        // dev->nr_pollfd == 0, All attached processes exit.
        if (dev->nr_pollfd == 0) {
            // In hangup(), you can call prof_dev_close() as well.
            if (dev->prof->hangup)
                dev->prof->hangup(dev);
            prof_dev_close(dev);
        }
    }
    prof_dev_put(dev);
}

static int __addfn(int fd, unsigned events, struct perf_mmap *mmap)
{
    struct prof_dev *dev = perf_evlist_poll__get_external(NULL, mmap);
    dev->nr_pollfd ++;
    return main_epoll_add(fd, events, mmap, perf_event_handle);
}

static int __delfn(int fd, unsigned events, struct perf_mmap *mmap)
{
    main_epoll_del(fd);
    return 0;
}

static void interval_handle(struct timer *timer)
{
    struct prof_dev *dev = container_of(timer, struct prof_dev, timer);
    struct perf_evlist *evlist = dev->evlist;
    struct perf_cpu_map *cpus = dev->cpus;
    struct perf_thread_map *threads = dev->threads;
    profiler *prof = dev->prof;
    struct prof_dev *source, *tmp;

    prof_dev_get(dev);
    print_lost_events(dev);

    // Do not use prof_dev_flush().
    if (dev->pages) {
        struct perf_mmap *map;
        perf_evlist__for_each_mmap(evlist, map, dev->env->overwrite) {
            perf_event_handle_mmap(dev, map);
        }
    }

    // Recursively execute the interval_handle() of the source prof_dev, including: flush, read, interval.
    /*
     * Cannot use list_for_each_entry_safe(source, next, &dev->forward.source_list, forward.link_to_target)
     * Next is possible to be free.
     *
     *   multi-trace -e x:xx -e y:yy,task-state/untraced/ --order -i 1000 -N 50 -- multi_thread_app
     *
     * Task-state events are forwarded to multi-trace and cached in order. If a thread of
     * multi_thread_app exits, task-state will close its prof_dev, but its events are still
     * cached in the internal of multi-trace, so prof_dev has not been freed until it is flushed.
     *
     *     000000000041bdb6 prof_dev_free+0x0       # free prof_dev
     *     000000000043869b perf_event_put+0x2f
     *     0000000000436386 multi_trace_flush+0x6a  # flush internal buffer of multi-trace
     *     000000000041bda8 prof_dev_flush+0x16e
     *     000000000041ba96 prof_dev_close+0x222    # -N 50, close & disable multi-trace
     *     000000000041a068 perf_event_process_record+0x45f # task-state forward to multi-trace
     *     000000000041a21f perf_event_handle_mmap+0x68
     *     000000000041a43b interval_handle+0xae    # task-state
     *     000000000041a517 interval_handle+0x18a   # multi-trace
     *     000000000041f821 timer_expire+0x66
     *     0000000000414fea event_poll__poll+0xd0
     *     000000000041cb3d main+0x19d
     *
     * Each iteration obtains the reference to the `source' to ensure that it is not freed,
     * so the next prof_dev can be obtained safely.
     */
    for_each_source_dev_get(source, tmp, dev)
        interval_handle(&source->timer);

    if (prof->read && dev->values) {
        struct perf_evsel *evsel;
        int cpu, ins, tins;
        perf_cpu_map__for_each_cpu(cpu, ins, cpus) {
            for (tins = 0; tins < perf_thread_map__nr(threads); tins++) {
                perf_evlist__for_each_evsel(evlist, evsel) {
                    struct perf_counts_values *count = dev->values;
                    struct perf_cpu_map *evsel_cpus = perf_evsel__cpus(evsel);

                    if (unlikely(evsel_cpus != cpus) &&
                        perf_cpu_map__idx(evsel_cpus, cpu) < 0) {
                        continue;
                    }

                    if (perf_evsel__read(evsel, ins, tins, count) == 0 &&
                        prof->read(dev, evsel, count, cpu != -1 ? ins : tins))
                        break;
                }
            }
        }
    }

    if (prof->interval)
        prof->interval(dev);

    perfeval_evaluate(dev);

    prof_dev_put(dev);
}

static
struct prof_dev *prof_dev_open_internal(profiler *prof, struct env *env,
                 struct perf_cpu_map *cpu_map, struct perf_thread_map *thread_map,
                 struct prof_dev *parent, bool clone)
{
    struct perf_evlist *evlist = NULL;
    struct perf_cpu_map *cpus = NULL, *online = NULL;
    struct perf_thread_map *threads = NULL;
    struct prof_dev *dev, *child, *tmp;
    int reinit = 0;
    int err = 0;

    dev = malloc(sizeof(*dev));
    if (!dev)
        goto out_free;

    memset(dev, 0, sizeof(*dev));
    dev->prof = prof;
    dev->env = env;
    INIT_LIST_HEAD(&dev->dev_link);
    dev->refcount = 1;
    dev->type = PROF_DEV_TYPE_NORMAL;
    dev->state = parent && !clone && parent->state <= PROF_DEV_STATE_INACTIVE ?
                 PROF_DEV_STATE_OFF : PROF_DEV_STATE_INACTIVE;
    dev->print_title = true;
    INIT_LIST_HEAD(&dev->order.heap_event_list);
    INIT_LIST_HEAD(&dev->links.child_list);
    INIT_LIST_HEAD(&dev->links.link_to_parent);
    INIT_LIST_HEAD(&dev->forward.source_list);
    INIT_LIST_HEAD(&dev->forward.link_to_target);
    INIT_LIST_HEAD(&dev->ptrace_list);

    if (parent) {
        dev->clone = clone;
        dev->links.parent = parent;
        list_add_tail(&dev->links.link_to_parent, &parent->links.child_list);
    }

    // workload output to stdout & stderr
    // perf-prof output to env.output file
    // TODO per prof_dev output
    if (prof_dev_reopen_output(dev) < 0)
        goto out_free;

    dev->pages = prof->pages;
    if (env->mmap_pages)
        dev->pages = env->mmap_pages;

    prof_dev_get(dev);
    prof_dev_winsize(dev);

reinit:
    reinit = 0;

    evlist = perf_evlist__new();
    if (!evlist) {
        fprintf(stderr, "failed to create evlist\n");
        goto out_free;
    }
    dev->evlist = evlist;
    perf_evlist_poll__external(evlist, dev);

    if (cpu_map || thread_map) {
        cpus = cpu_map ? perf_cpu_map__get(cpu_map) : perf_cpu_map__dummy_new();
        threads = thread_map ? perf_thread_map__get(thread_map) : perf_thread_map__new_dummy();
    } else if (env->pids || env->tids) {
        // attach to processes
        threads = thread_map__new_str(env->pids, env->tids, 0, 0);
        cpus = env->inherit ? perf_cpu_map__new(NULL) : perf_cpu_map__dummy_new();
        if (!threads || !cpus) {
            fprintf(stderr, "failed to create pids\n");
            goto out_delete;
        }
    } else if (env->workload.pid) {
        // attach to workload
        threads = thread_map__new_by_pid(env->workload.pid);
        cpus = env->inherit ? perf_cpu_map__new(NULL) : perf_cpu_map__dummy_new();
        if (!threads || !cpus) {
            fprintf(stderr, "failed attach to workload\n");
            goto out_delete;
        }
    } else {
        // attach to cpus
        cpus = perf_cpu_map__new(env->cpumask);
        if (env->cgroups)
            // attach to cgroups
            threads = thread_map__cgroups(env->cgroups);
        else
            threads = perf_thread_map__new_dummy();
        if (!cpus || !threads) {
            fprintf(stderr, "failed to create cpus\n");
            goto out_delete;
        }
        online = perf_cpu_map__new(NULL);
        if (!online) {
            fprintf(stderr, "failed to create online\n");
            goto out_delete;
        }
        cpus = perf_cpu_map__and(cpus, online);
        if (!cpus) {
            fprintf(stderr, "failed to create cpus\n");
            goto out_delete;
        }
        perf_cpu_map__put(online);
        online = NULL;
    }
    dev->cpus = cpus; cpus = NULL;
    dev->threads = threads; threads = NULL;

    if (prof->init(dev) < 0) {
        fprintf(stderr, "monitor(%s) init failed\n", prof->name);
        goto out_delete;
    }
    if (perf_event_convert_init(dev) < 0) {
        fprintf(stderr, "monitor(%s) convert failed\n", prof->name);
        goto out_deinit;
    }
    if (prof_dev_evsel_external_init(dev) < 0) {
        fprintf(stderr, "monitor(%s) evsel external init failed\n", prof->name);
        goto out_deinit;
    }
    /* prof->init allows reassignment of cpus and threads */
    perf_evlist__set_maps(evlist, dev->cpus, dev->threads);

    err = perf_evlist__open(evlist);
    if (err) {
        if (err == -ESRCH && !env->cgroups && dev->threads != thread_map) {
            int idx, thread;
            perf_thread_map__for_each_thread(thread, idx, dev->threads) {
                if (thread >= 0) {
                    if (kill(thread, 0) < 0 && errno == ESRCH) {
                        fprintf(stderr, "thread %d %s. reinit.\n", thread, strerror(errno));
                        reinit = 1;
                    }
                }
            }
        }
        if (!reinit && prof->reinit)
            reinit = prof->reinit(dev, err);
        if (!reinit && !dev->silent)
            fprintf(stderr, "failed to open evlist, %d\n", err);
        goto out_deinit;
    }

    if (prof->filter && prof->filter(dev) < 0) {
        fprintf(stderr, "monitor(%s) filter failed\n", prof->name);
        goto out_close;
    }

    if (dev->pages) {
        err = perf_evlist__mmap(evlist, dev->pages);
        if (err) {
            fprintf(stderr, "monitor(%s) mmap failed\n", prof->name);
            goto out_close;
        }
        if (env->order || prof->order)
            if (order_init(dev) < 0)
                goto out_munmap;

        if (env->python_callchain)
            if (pystack_link(dev) < 0)
                goto out_order_deinit;
    }

    if (dev->env->interval) {
        dev->max_read_size = perf_evlist__max_read_size(evlist);
        dev->values = zalloc(dev->max_read_size);
        if (!dev->values)
            goto out_order_deinit;

        if (perfeval_init(dev) < 0)
            goto out_del_timer;

        err = timer_init(&dev->timer, 1, interval_handle);
        if (err) {
            fprintf(stderr, "monitor(%s) timer init failed\n", prof->name);
            goto out_del_timer;
        }
    }

    if (dev->env->using_ptrace && !prof_dev_ins_oncpu(dev) &&
        perf_thread_map__pid(dev->threads, 0) != -1)
        ptrace_attach(dev->threads, dev);

    if (dev->type == PROF_DEV_TYPE_NORMAL) {
        /*
         * In prof_dev_list_close(), make sure to close the parent device first. In this way,
         * the reference to the child can be flushed in prof_dev_disable(), if the child is
         * forwarded to the parent, and the event is cached in the parent's internal buffer.
         */
        if (!list_empty(&dev->links.child_list))
            list_add(&dev->dev_link, &prof_dev_list);
        else
            list_add_tail(&dev->dev_link, &prof_dev_list);
    }

    if (dev->state == PROF_DEV_STATE_INACTIVE)
        if (prof_dev_enable(dev) < 0)
            goto out_disable;

    if (dev->clone)
        return dev;
    else
        return prof_dev_put(dev) ? NULL : dev;

out_disable:
    list_del(&dev->dev_link);
    if (dev->pages)
        perf_evlist_poll__foreach_fd(evlist, __delfn);

out_del_timer:
    if (dev->env->interval) {
        timer_destroy(&dev->timer);
        if (dev->values)
            free(dev->values);
        perfeval_free(dev);
    }
out_order_deinit:
    order_deinit(dev);
out_munmap:
    if (dev->pages)
        perf_evlist__munmap(evlist);
out_close:
    perf_evlist__close(evlist);
out_deinit:
    // prof->init() may open child devices.
    for_each_child_dev_get(child, tmp, dev)
        prof_dev_close(child);
    // prof->init() may call ptrace_attach().
    if (!reinit)
        ptrace_detach(dev);

    prof_dev_evsel_external_deinit(dev);
    perf_event_convert_deinit(dev);
    prof->deinit(dev);
out_delete:
    perf_evlist__set_maps(evlist, NULL, NULL);
    perf_evlist__delete(evlist);
    perf_cpu_map__put(cpus);
    perf_cpu_map__put(online);
    perf_thread_map__put(threads);
    perf_cpu_map__put(dev->cpus);
    perf_thread_map__put(dev->threads);
    dev->cpus = NULL;
    dev->threads = NULL;

    if (env->cgroups)
        cgroup_list__delete();

    if (reinit)
        goto reinit;

out_free:
    if (dev)
        list_del(&dev->links.link_to_parent);

    free_env(env);
    if (dev) free(dev);

    return NULL;
}

/**
 * prof_dev_open_cpu_thread_map - open a new prof_dev
 * @prof: profiler
 * @env: env
 * @cpu_map: new prof_dev attach to this cpu_map
 * @thread_map: new prof_dev attach to this thread_map
 * @parent: parent prof_dev
 *
 * If parent is not NULL, the new prof_dev will not be enabled by default. You can
 * call prof_dev_enable() to enable it directly, or wait for parent to enable it.
 */
struct prof_dev *prof_dev_open_cpu_thread_map(profiler *prof, struct env *env,
                 struct perf_cpu_map *cpu_map, struct perf_thread_map *thread_map, struct prof_dev *parent)
{
    return prof_dev_open_internal(prof, env, cpu_map, thread_map, parent, false);
}

struct prof_dev *prof_dev_open(profiler *prof, struct env *env)
{
    return prof_dev_open_internal(prof, env, NULL, NULL, NULL, false);
}

/**
 * prof_dev_clone - clone to get a new prof_dev
 * @parent: parent prof_dev
 * @cpu_map: new prof_dev attach to this cpu_map
 * @thread_map: new prof_dev attach to this thread_map
 *
 * The new prof_dev will use the profiler and environment of the parent. And
 * attach to the new cpu_map and thread_map. Others such as: timer, order,
 * convert, can all be rebuilt using env. Profiler-specific memory, it is up
 * to the profiler to decide how to share it with the new prof_dev. And,
 * forwarded to the same target as the parent device.
 *
 * Similar to clone syscall, the cloned prof_dev is independent of the parent.
 * The disable() and close() of the parent dev will not affect the cloned dev.
 */
struct prof_dev *prof_dev_clone(struct prof_dev *parent,
                 struct perf_cpu_map *cpu_map, struct perf_thread_map *thread_map)
{
    profiler *prof;
    struct env *e;
    struct prof_dev *dev = NULL;

    if (!prof_dev_get(parent)) {
        fprintf(stderr, "WARN: The parent dev is closing.\n");
        return NULL;
    }

    if (parent->state == PROF_DEV_STATE_EXIT)
        goto put;

    prof = parent->prof;

    e = clone_env(parent->env); // free in prof_dev_close()
    if (!e) goto put;

    dev = prof_dev_open_internal(prof, e, cpu_map, thread_map, parent, true);
    if (dev) {
        if (parent->forward.target &&
            prof_dev_forward(dev, parent->forward.target) < 0) {
            prof_dev_close(dev);
            dev = NULL;
        } else {
            dev->print_title = parent->print_title;
            if (prof_dev_put(dev))
                dev = NULL;
        }
    }

put:
    prof_dev_put(parent);
    return dev;
}

static int prof_dev_atomic_enable(struct prof_dev *dev, u64 enable_cost)
{
    struct perf_evlist *evlist = dev->evlist;
    struct perf_mmap *map;
    union perf_event *event;
    bool writable = false;
    u64 enabled_after_ns;

    if (prof_dev_nr_ins(dev) <= 1)
        return 0;

    if (dev->env->overwrite)
        return -1;

    if (!(dev->pos.sample_type & PERF_SAMPLE_TIME))
        return -1;

    /*
     * Stream events and perf_mmap events use different clocks.
     * When time conversion is not enabled, setting `enabled_after'
     * may filter out all stream events.
     */
    if (using_order(dev) &&
        dev->order.nr_streams && // stream event(e.g. pull ATTR)
        !dev->convert.need_conv) // convert
        return -1;

    perf_evlist__for_each_mmap(evlist, map, dev->env->overwrite) {
        if (perf_mmap__read_init(map) < 0)
            continue;

        while ((event = perf_mmap__read_event(map, &writable)) != NULL) {
            perf_mmap__consume(map);
            enabled_after_ns = enable_cost + *(u64 *)((void *)event->sample.array + dev->pos.time_pos);
            goto enabled_after;
        }
        if (!event)
            perf_mmap__read_done(map);
    }
    return 0;

enabled_after:
    /*
     * Start sampling after the events is fully enabled.
     *
     * -e sched:sched_wakeup -e sched:sched_switch -C 0-95
     * A sched_wakeup occurs on CPU0, possibly sched_switch occurs on CPU95. When enabling, CPU0 is
     * enabled first, and CPU95 is enabled last. It is possible that the sched_wakeup event is only
     * sampled on CPU0, and the sched_switch event is not sampled on CPU95.
     *
     * Events after `enabled_after_ns' are safe and there is no enablement loss.
    **/
    perf_event_convert_read_tsc_conversion(dev, map);
    dev->time_ctx.enabled_after = perfclock_to_evclock(dev, enabled_after_ns);
    if (dev->env->verbose) {
        printf("%s: enabled after %lu.%06lu\n", dev->prof->name, dev->time_ctx.enabled_after.clock/NSEC_PER_SEC,
                    (dev->time_ctx.enabled_after.clock%NSEC_PER_SEC)/1000);
    }
    return 0;
}

int prof_dev_enable(struct prof_dev *dev)
{
    profiler *prof;
    struct env *env;
    struct perf_evlist *evlist;
    struct prof_dev *child, *tmp;
    struct timespec before_enable, after_enable;
    u64 enable_cost;
    int err;

    if (!dev ||
        dev->state == PROF_DEV_STATE_ACTIVE ||
        dev->state == PROF_DEV_STATE_EXIT)
        return 0;

    prof = dev->prof;
    env = dev->env;
    evlist = dev->evlist;

    perf_timespec_init(dev);

    err = perf_evlist_poll__foreach_fd(evlist, __addfn);
    if (err) {
        fprintf(stderr, "monitor(%s) poll failed\n", prof->name);
        return -1;
    }

    prof_dev_get(dev);

    dev->state = PROF_DEV_STATE_ACTIVE;
    if (dev->type == PROF_DEV_TYPE_NORMAL)
        if (running >= 0) running ++;

    // Enable child dev before workload_start().
    for_each_child_dev_get(child, tmp, dev)
        prof_dev_enable(child);

    clock_gettime(CLOCK_MONOTONIC, &before_enable);
    perf_evlist__enable(evlist);
    clock_gettime(CLOCK_MONOTONIC, &after_enable);

    enable_cost = (after_enable.tv_sec - before_enable.tv_sec) * NSEC_PER_SEC +
                  (after_enable.tv_nsec - before_enable.tv_nsec);
    prof_dev_atomic_enable(dev, enable_cost);

    if (env->interval)
        timer_start(&dev->timer, env->interval * 1000000UL, false);

    // In enabled(), prof_dev_close() may be called.
    if (prof->enabled)
        prof->enabled(dev);

    if (dev->state == PROF_DEV_STATE_ACTIVE)
        workload_start(&env->workload);

    prof_dev_put(dev);

    return 0;
}

int prof_dev_disable(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct prof_dev *source, *child, *tmp;

    if (dev->state < PROF_DEV_STATE_ACTIVE)
        return 0;

    prof_dev_get(dev);

    dev->state = PROF_DEV_STATE_INACTIVE;
    if (dev->type == PROF_DEV_TYPE_NORMAL)
        if (running > 0) running --;

    for_each_source_dev_get(source, tmp, dev)
        prof_dev_disable(source);

    for_each_child_dev_get(child, tmp, dev) {
        if (!child->clone && child->forward.target != dev)
            prof_dev_disable(child);
    }

    // Disable subsequent interval_handle() calls.
    if (dev->env->interval)
        timer_cancel(&dev->timer);

    perf_evlist__disable(evlist);

    // Disable subsequent perf_event_handle() calls.
    if (dev->pages)
        perf_evlist_poll__foreach_fd(evlist, __delfn);

    if (dev->pages) {
        // Flush the ringbuffer and submit the remaining perf events.
        // Disabled, final flushes other buffers, such as: multi-trace timeline.
        prof_dev_flush(dev, PROF_DEV_FLUSH_FINAL);
    }

    prof_dev_put(dev);

    return 0;
}

/**
 * prof_dev_forward - Forward events from source device to target device.
 * @dev: source device
 * @target: target device
 *
 * Events from @dev will be wrapped in PERF_RECORD_DEV and forwarded to @target.
 *
 * When order is enabled, events from both @dev and @target are sorted together
 * in the same heap. Each device extracts timestamps from its own time_pos
 * independently, so time_pos doesn't need to match between devices. After
 * sorting, order.c calls perf_event_process_record() which wraps @dev's events
 * into PERF_RECORD_DEV and delivers them to @target (trace, multi-trace).
 */
int prof_dev_forward(struct prof_dev *dev, struct prof_dev *target)
{
    if (perf_sample_forward_init(dev) == 0) {
        if (using_order(target) && perf_sample_time_init(target) < 0)
            return -1;

        dev->forward.event_dev = malloc(PERF_SAMPLE_MAX_SIZE);
        if (dev->forward.event_dev) {
            dev->forward.target = target;
            list_add_tail(&dev->forward.link_to_target, &target->forward.source_list);

            /*
             * Like this command:
             *   perf-prof multi-trace -e XX:YYY -e XX:ZZZ,task-state//untraced/ -p 1234 --order \
             *             --than 10ms --detail=sametid
             *
             * multi-trace is attached to pid 1234, task-state is also attached to pid 1234, but
             * internally it switches to the cpu.
             *
             * In this scenario, forwarding will reset the instance. Task-state events are forwarded
             * to multi-trace, but the instance seen is 0.
             */
            if (prof_dev_ins_oncpu(dev) != prof_dev_ins_oncpu(target) ||
                prof_dev_nr_ins(dev) != prof_dev_nr_ins(target)) {
                dev->forward.ins_reset = true;
                if (!dev->clone)
                    printf("%s events are forwarded to %s, reset instance.\n", dev->prof->name, target->prof->name);
            }
            return 0;
        }
    }
    return -1;
}

void prof_dev_flush(struct prof_dev *dev, enum profdev_flush how)
{
    struct prof_dev *source, *tmp;

    if (dev->inflush)
        return;

    prof_dev_get(dev);
    dev->inflush = true;

    if (dev->pages) {
        struct perf_evlist *evlist = dev->evlist;
        struct perf_mmap *map;
        perf_evlist__for_each_mmap(evlist, map, dev->env->overwrite) {
            perf_event_handle_mmap(dev, map);
        }
    }

    if (how != PROF_DEV_FLUSH_FINAL) {
        // Recursively flush the source prof_dev.
        for_each_source_dev_get(source, tmp, dev)
            prof_dev_flush(source, how);
    }

    // Flush prof_dev buffers. At the same time, the reference
    // count of the forwarding source is released.
    if (dev->prof->flush)
        dev->prof->flush(dev, how);

    dev->inflush = false;
    prof_dev_put(dev);
}

static void prof_dev_free(struct prof_dev *dev)
{
    profiler *prof = dev->prof;
    struct perf_evlist *evlist = dev->evlist;
    struct prof_dev *child, *next, *parent;

    print_lost_events(dev);

    if (dev->env->interval) {
        timer_destroy(&dev->timer);
        if (dev->values)
            free(dev->values);
        perfeval_free(dev);
    }
    if (timer_started(&dev->time_ctx.base_timer))
        timer_destroy(&dev->time_ctx.base_timer);

    /*
     * prof->deinit() may call perf_evlist__id_to_evsel(), which requires id_hash.
     * Therefore, it is called before perf_evlist__munmap() resets the id_hash.
     *
     * However, the prof_dev internal buffer has been flushed, see PROF_DEV_FLUSH_FINAL.
     * Here deinit() is only used to release memory and no longer handle events.
    **/
    prof->deinit(dev);
    dev->private = NULL;
    prof_dev_evsel_external_deinit(dev);
    perf_event_convert_deinit(dev);

    if (dev->pages) {
        order_deinit(dev);
        perf_evlist__munmap(evlist);
    }

    perf_evlist__close(evlist);

    perf_evlist__set_maps(evlist, NULL, NULL);
    perf_evlist__delete(evlist);
    perf_cpu_map__put(dev->cpus);
    perf_thread_map__put(dev->threads);

    if (dev->env->cgroups)
        cgroup_list__delete();

    for_each_child_dev_safe(child, next, dev) {
        /*
         * The parent device of `dev' inherits all its child devices.
         */
        if (dev->links.parent) {
            // In prof_dev_at_top(), used to identify the topmost device.
            child->clone = child->links.parent->clone;
            child->links.parent = dev->links.parent;
            list_move_tail(&child->links.link_to_parent, &dev->links.parent->links.child_list);
        } else {
            child->clone = false;
            child->links.parent = NULL;
            list_del_init(&child->links.link_to_parent);
        }
    }

    parent = dev->links.parent;
    if (parent) {
        if (parent->links.pystack == dev)
            parent->links.pystack = NULL;
        list_del(&dev->links.link_to_parent);
    }

    if (dev->forward.target) {
        free(dev->forward.event_dev);
        list_del(&dev->forward.link_to_target);
    }

    free_env(dev->env);
    free(dev);
}

struct prof_dev *prof_dev_get(struct prof_dev *dev)
{
    if (dev->refcount > 0) {
        dev->refcount ++;
        return dev;
    }
    return NULL;
}

bool prof_dev_put(struct prof_dev *dev)
{
    dev->refcount --;
    if (dev->refcount < 0) {
        fprintf(stderr, "WARN: %s: dev %p ref(%d) < 0.\n", dev->prof->name, dev, dev->refcount);
    }
    if (dev->refcount == 0) {
        prof_dev_free(dev);
        return true;
    }
    return false;
}

void prof_dev_close(struct prof_dev *dev)
{
    struct prof_dev *source, *child, *tmp;

    if (dev->inclose)
        return;
    /*
     * Make sure prof_dev_close() can only be called once.
     *
     * This prevents prof_dev_put() from being run twice.
     *     prof_dev_close() ->
     *         prof_dev_disable() ->
     *         |   prof_dev_flush() ->
     *         |       perf_event_handle_mmap() ->
     *         |           perf_event_process_record() ->
     *         |               prof_dev_close() -|
     *         |               -   prof_dev_disable() # Not called.
     *         |               -   prof_dev_put()     # Not called.
     *         prof_dev_put()
     */
    dev->inclose = true;

    prof_dev_disable(dev);

    dev->state = PROF_DEV_STATE_EXIT;
    list_del_init(&dev->dev_link);

    ptrace_detach(dev);

    /*
     * Close the child device, the forwarding source.
     *
     * clone = 0, forward = 0.
     *     Devices opened using tp_list_new() are closed when the parent closes, not within
     *     tp_list_free().
     *     The child device can close themselves. If not, here is the final closing point.
     *
     * clone = 0, forward = 1. prof_dev_forward().
     *     The forwarding target is closed, and the forwarding source will also be closed.
     *     It is impossible to continue forwarding.
     *
     * clone = 1, forward = 0. prof_dev_clone().
     *     The cloned device is independent of the parent and will not be closed.
     *
     * clone = 1, forward = 1. prof_dev_forward() && prof_dev_clone().
     *     As a forwarding source, it will also be closed.
     */
    for_each_source_dev_get(source, tmp, dev)
        prof_dev_close(source);

    for_each_child_dev_get(child, tmp, dev) {
        if (!child->clone && child->forward.target != dev)
            prof_dev_close(child);
    }

    /*
     * Control dev to its parent/target. Only inside prof_dev_free().
     *
     * This call stack will forward the event to the target.
     *   prof_dev_close(dev) -> prof_dev_disable(dev) -> prof_dev_flush(dev) ->
     *   perf_event_process_record(dev, event) -> perf_event_forward(dev) => target
     *
     * prof_dev_close(dev) =>
     *    if (dev->links.parent) {
     *        list_del_init(&dev->links.link_to_parent);
     *        dev->links.parent = NULL; ==> Affects prof_dev_at_top(dev)
     *    }
     *    if (dev->forward.target) {
     *        list_del_init(&dev->forward.link_to_target);
     *        dev->forward.target = NULL; ==> Affects prof_dev_is_final(dev)
     *    }
     * Here, disconnect dev and parent/target in advance. Will cause some trouble.
     *
     *   prof_dev_close(target) -> prof_dev_disable(target) -> prof_dev_flush(target) ->
     *   order_flush(target) -> perf_event_process_record(target, event) ->
     *   perf_event_process_record(dev, event) -> sample(dev, event) ->
     *   prof_dev_is_final(dev) ==> return false (target = NULL)
     *
     *   prof_dev_close(target) -> prof_dev_disable(target) -> prof_dev_flush(target) ->
     *   order_flush(target) -> ordered_events__deliver(target) -> perf_event_put() ->
     *   prof_dev_put(dev) -> prof_dev_free(dev) -> deinit(dev) ->
     *   prof_dev_at_top(dev) ==> return true (parent = NULL)
     *
     * When dev is closed, events are still cached in the target's internal buffer. When
     * the target is flushed, it will continue to call dev->sample, dev->deinit.
     */

    prof_dev_put(dev);

    // prof_dev_close() can only be called once, so dev->inclose will not be set to false.
}

void prof_dev_print_time(struct prof_dev *dev, u64 evtime, FILE *fp)
{
    u64 ns;
    s64 off_ns;
    char timebuff[64];
    struct timeval tv;
    struct tm *result;

    if (likely(dev->time_ctx.base_evtime > 0 && evtime > 0)) {
        off_ns = evclock_to_real_ns(dev, (evclock_t)evtime) - dev->time_ctx.base_evtime;
        off_ns += dev->time_ctx.base_timespec.tv_nsec;

        ns = dev->time_ctx.base_timespec.tv_sec * NSEC_PER_SEC + off_ns;
        tv.tv_sec = ns / NSEC_PER_SEC;
        tv.tv_usec = (ns % NSEC_PER_SEC) / 1000;
    } else
        gettimeofday(&tv, NULL);

    result = localtime(&tv.tv_sec);
    strftime(timebuff, sizeof(timebuff), "%Y-%m-%d %H:%M:%S", result);
    fprintf(fp, "%s.%06u ", timebuff, (unsigned int)tv.tv_usec);
}

void prof_dev_print_event(struct prof_dev *dev, union perf_event *event, int instance, int flags)
{
    if (unlikely(event->header.type == PERF_RECORD_DEV)) {
        struct perf_record_dev *event_dev = (void *)event;
        event = event_dev->event;
        dev = event_dev->dev;
        instance = event_dev->instance;
    }

    if (dev->prof->print_event) {
        dev->prof->print_event(dev, event, instance, flags);
    } else {
        fprintf(stderr, "No print_event() for %s\n", dev->prof->name);
    }
}

int prof_dev_reopen_output(struct prof_dev *dev)
{
    if (!dev->env->output)
        return 0;

    if (freopen(dev->env->output, "a", stdout)) {
        dup2(STDOUT_FILENO, STDERR_FILENO);
        setlinebuf(stdin);
        setlinebuf(stdout);
        setlinebuf(stderr);
        return 0;
    }
    return -1;
}

static perfclock_t prof_dev_minevtime(struct prof_dev *dev)
{
    u64 minevtime = ULLONG_MAX;

    /*
     * The minimum event time is taken from the ringbuffer, order, and profiler.
     */

    // profiler
    if (dev->prof->minevtime) {
        minevtime = dev->prof->minevtime(dev);

        // ULLONG_MAX and 0 are special values, not evclock_t, and cannot be converted to ns.
        if (minevtime != ULLONG_MAX && minevtime != 0)
            minevtime = evclock_to_perfclock(dev, (evclock_t)minevtime);
    }

    if (using_order(dev) && dev->order.heap_popped_time) {
        u64 heap_popped_time = heapclock_to_perfclock(dev, dev->order.heap_popped_time);
        minevtime = min(heap_popped_time, minevtime);
    }

    // ringbuffer
    // The minevtime of profiler must be smaller than that of ringbuffer.
    // Therefore, ringbuffer is judged only when minevtime == ULLONG_MAX.
    if (minevtime == ULLONG_MAX &&
        dev->pos.time_pos >= 0 &&
        dev->pages && !dev->env->overwrite) {
        struct perf_mmap *map;

        perf_evlist__for_each_mmap(dev->evlist, map, dev->env->overwrite) {
            union perf_event *event;
            bool writable = false;
            int idx = perf_mmap__idx(map);

            if (perf_mmap__read_init(map) < 0)
                continue;

            while ((event = perf_mmap__read_event(map, &writable)) != NULL) {
                if (event->header.type == PERF_RECORD_SAMPLE) {
                    perfclock_t rb_evtime = *(perfclock_t *)((void *)event->sample.array + dev->pos.time_pos);

                    if (rb_evtime < minevtime)
                        minevtime = rb_evtime;

                    perf_mmap__unread_event(map, event);
                    break;
                } else {
                    perf_event_process_record(dev, event, idx, writable, false);
                    perf_mmap__consume(map);
                }
            }
            if (!event)
                perf_mmap__read_done(map);
        }
    }

    return minevtime;
}

perfclock_t prof_dev_list_minevtime(void)
{
    perfclock_t minevtime = ULLONG_MAX;
    perfclock_t time;
    struct prof_dev *dev, *tmp;

    for_each_dev_get(dev, tmp, &prof_dev_list, dev_link) {
        if (dev->type != PROF_DEV_TYPE_NORMAL)
            continue;
        if (dev->silent)
            continue;
        time = prof_dev_minevtime(dev);
        if (time < minevtime)
            minevtime = time;
    }
    return minevtime;
}

static void prof_dev_list_close(void)
{
    struct prof_dev *dev;

restart:
    list_for_each_entry(dev, &prof_dev_list, dev_link) {
        prof_dev_close(dev);
        // May also close other prof_dev.
        goto restart;
    }
    // SIGINT
    if (running < 0) {
        while (!ptrace_detach_done())
            event_poll__poll(main_epoll, -1);
    }
}

static void print_marker_and_interval(int fd, unsigned int revents, void *ptr)
{
    char buf[512];

    if (revents & EPOLLIN) {
        char *line = fgets(buf, sizeof(buf), stdin);
        if (line) {
            struct prof_dev *dev, *tmp;
            for_each_dev_get(dev, tmp, &prof_dev_list, dev_link)
                if (prof_dev_is_final(dev))
                    interval_handle(&dev->timer);
            print_time(stdout);
            printf("%s", line);
        }
    }
}

static int libperf_print(enum libperf_print_level level,
                         const char *fmt, va_list ap)
{
    if (level > LIBPERF_WARN)
        return 0;
    return vfprintf(stderr, fmt, ap);
}

int main(int argc, char *argv[])
{
    int err = -1;
    struct timer usage_self;
    bool usage_self_start = false;
    profiler *main_prof = NULL;
    struct env *main_env = NULL;

    sigusr2_handler(0);
    setlinebuf(stdin);
    setlinebuf(stdout);
    setlinebuf(stderr);
    libperf_init(libperf_print);
    page_size = sysconf(_SC_PAGE_SIZE);

    main_epoll = event_poll__alloc(64);
    if (!main_epoll) {
        return -1;
    }

    main_prof = parse_main_options(argc, argv);
    if (!main_prof) return err;
    main_env = zalloc(sizeof(*main_env));
    if (!main_env) return err;
    *main_env = env;

    if (epoll_wait_signal(SIGCHLD, SIGINT, SIGTERM, SIGUSR1, SIGUSR2, SIGWINCH, 0) < 0)
        return -1;
    if (!isatty(STDIN_FILENO))
        main_epoll_add(STDIN_FILENO, EPOLLIN, NULL, print_marker_and_interval);
    if (env.usage_self) {
        timer_init(&usage_self, 1, usage_self_handle);
        timer_start(&usage_self, env.usage_self * 1000000UL, false);
        usage_self_start = true;
    }

    if (!prof_dev_open(main_prof, main_env))
        return -1;

    while (running > 0) {
        int fds = event_poll__poll(main_epoll, -1);

        // -ENOENT means there are no file descriptors in event_poll.
        if (fds == -ENOENT)
            running = 0;
    }
    err = 0;

    prof_dev_list_close();

    if (usage_self_start)
        timer_destroy(&usage_self);
    if (!isatty(STDIN_FILENO))
        main_epoll_del(STDIN_FILENO);

    return err;
}

