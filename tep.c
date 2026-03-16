#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <fnmatch.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <api/fs/tracing_path.h>
#include <monitor.h>
#include <tep.h>
#include <stack_helpers.h>
#include <api/fs/fs.h>
#include <trace_helpers.h>
#include <event-parse-local.h>
#include "perf_regs_mask.h"

#define PLUGINS_DIR "/usr/lib64/perf-prof/traceevent/plugins"

static struct tep_handle *tep = NULL;
static struct tep_plugin_list *plugins = NULL;
static int resolver_ref = 0;
static int global_comm = 0;
int kprobe_type = 0;
int uprobe_type = 0;
static char *kprobe_retprobe = NULL;
static char *uprobe_retprobe = NULL;

/* Reserved field names, taken from kernel/trace/trace_probe.h */
#define FIELD_STRING_IP     "__probe_ip"
#define FIELD_STRING_RETIP  "__probe_ret_ip"
#define FIELD_STRING_FUNC   "__probe_func"

__ctor
static void __kprobe_uprobe(void)
{
    size_t size;
    sysfs__read_int("bus/event_source/devices/kprobe/type", &kprobe_type);
    sysfs__read_int("bus/event_source/devices/uprobe/type", &uprobe_type);
    if (sysfs__read_str("bus/event_source/devices/kprobe/format/retprobe", &kprobe_retprobe, &size) < 0)
        kprobe_type = 0;
    else
        kprobe_retprobe = realloc(kprobe_retprobe, size);
    if (sysfs__read_str("bus/event_source/devices/uprobe/format/retprobe", &uprobe_retprobe, &size) < 0)
        uprobe_type = 0;
    else
        uprobe_retprobe = realloc(uprobe_retprobe, size);

}

void pr_stat(const char *fmt, ...)
{
    /* Disable libtraceevent printing.
     *
     * registering plugin:
     * overriding event
     * removing override handler for event
     */
}

/*
 * on_demand_ksym_resolver - Delayed kernel symbol resolver with lazy loading
 *
 * This function implements lazy loading of kernel symbols to avoid the expensive
 * ksyms__load() operation during program startup. The /proc/kallsyms file can
 * be very large and loading it is time-consuming.
 *
 * The resolver only triggers symbol table loading on first actual use, such as
 * when printing events with %ps format specifier for kernel function names.
 * This significantly improves perf-prof startup performance for use cases that
 * don't require symbol resolution.
 */
static char *on_demand_ksym_resolver(void *priv, unsigned long long *addrp, char **modp)
{
    /* Load kernel symbols only on first actual resolution request */
    if (resolver_ref == 0) {
        function_resolver_ref();  /* This calls ksyms__load() internally */
        resolver_ref = 1;
    }

    return function_resolver(priv, addrp, modp);
}

/*
 * tep__ref() - Get fully initialized tep handle with plugins and global_comm
 *
 * Use this when you need:
 * - Global comm tracking: Process name tracking across the system
 * - Plugin support: Event pretty-printing and enhanced formatting
 * - Full tracepoint functionality: Complete event processing capabilities
 */
struct tep_handle *tep__ref(void)
{
    if (!tep) {
        tep = tep_alloc();
        if (!tep)
            return NULL;
    } else
        tep_ref(tep);

    if (!plugins) {
        tep_add_plugin_path(tep, (char *)PLUGINS_DIR, TEP_PLUGIN_FIRST);
        plugins = tep_load_plugins(tep);
        tep_set_function_resolver(tep, on_demand_ksym_resolver, NULL);
    }
    if (global_comm == 0 && global_comm_ref() == 0)
        global_comm = 1;
    return tep;
}

/*
 * tep__ref_light() - Get lightweight tep handle without plugins or global_comm
 *
 * This is a performance-optimized version of tep__ref() that provides minimal
 * tep functionality without expensive initialization overhead. It can be called
 * from both internal tep.c functions and external code that doesn't need full
 * initialization.
 *
 * Use this when you only need:
 * - Event ID lookup: Query event identifiers
 * - Field information: Access event field definitions
 * - Basic tep operations: Event structure queries
 * - Internal utility functions: Helper operations within tep.c
 *
 * Benefits:
 * - Fast initialization: Avoids expensive global_comm_ref() call
 * - Lower overhead: Suitable for frequent internal calls
 * - Flexible upgrade: Can call tep__ref() later if full features needed
 *
 * Typical users:
 * - Internal tep.c functions (tep__event_id, tep__pid_to_comm, etc.)
 * - Utility functions that only query event metadata
 * - Code paths that don't need process name tracking
 * - Performance-sensitive event lookups
 *
 * Lifecycle scenarios:
 *
 * Scenario 1: Full initialization already done
 *   caller:   tep__ref()       -> tep with full init (refcount=1)
 *   caller:   tep__ref_light() -> increment refcount (refcount=2)
 *   caller:   tep__unref()     -> decrement (refcount=1)
 *   caller:   tep__unref()     -> free with full cleanup
 *
 * Scenario 2: Only lightweight operations needed
 *   caller:   tep__ref_light() -> minimal tep (refcount=1, no plugins/global_comm)
 *   caller:   tep__unref()     -> free minimal tep (plugins=NULL safely handled)
 *
 * Scenario 3: Upgrade from light to full
 *   caller:   tep__ref_light() -> minimal tep (refcount=1)
 *   caller:   tep__ref()       -> initialize plugins/global_comm (refcount=2)
 *   caller:   tep__unref()     -> decrement (refcount=1)
 *   caller:   tep__unref()     -> free with full cleanup
 *
 * Important notes:
 * - Always pair with tep__unref() to properly manage reference counting
 * - Returned tep may have limited functionality (no plugins, no resolver)
 * - Can safely call tep__ref() later to upgrade to full initialization
 * - tep__unref() safely handles both minimal and full tep cleanup
 * - Safe to mix with tep__ref() calls
 */
struct tep_handle *tep__ref_light(void)
{
    if (tep != NULL) {
        tep_ref(tep);
        return tep;
    }
    tep = tep_alloc();
    return tep;
}

void tep__unref(void)
{
    if (tep == NULL)
        return;

    if (tep_get_ref(tep) == 1) {
        if (global_comm) {
            global_comm_unref();
            global_comm = 0;
        }
        tep_reset_function_resolver(tep);
        if (resolver_ref) {
            function_resolver_unref();
            resolver_ref = 0;
        }
        if (plugins)
            tep_unload_plugins(plugins, tep);
        tep_free(tep);
        tep = NULL;
    } else
        tep_unref(tep);
}

int tep__event_id(const char *sys, const char *name)
{
    char format[256];
    struct stat st;
    struct tep_event *event;
    int id = -1;

    if (!tep) {
        char path[256];
        snprintf(path, sizeof(path), "%s/events/%s/%s/id", tracing_path_mount(), sys, name);
        filename__read_int(path, &id);
        return id;
    }

    tep__ref_light();
    event = tep_find_event_by_name(tep, sys, name);
    if (event) {
        id = event->id;
        goto unref;
    }
    snprintf(format, sizeof(format), "%s/events/%s/%s/format", tracing_path_mount(), sys, name);
    if (stat(format, &st) == 0) {
        FILE *fp;
        size_t size = 65536;
        char *buff = malloc(size);

        fp = fopen(format, "r");
        size = fread(buff, 1, size, fp);
        fclose(fp);

        tep_parse_format(tep, &event, buff, size, sys);

        free(buff);

        id = event->id;
    }
unref:
    tep__unref();
    return id;
}

void tep__update_comm(const char *comm, int pid)
{
    char buff[16];

    if (global_comm)
        return;

    if (comm == NULL) {
        char path[64];
        int fd, len;

        snprintf(path, sizeof(path), "/proc/%d/comm", pid);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            return;
        len = (int)read(fd, buff, sizeof(buff));
        close(fd);
        if (len <= 0)
            return;
        len--;
        if (buff[len] == '\n' || len == sizeof(buff)-1)
            buff[len] = '\0';
        comm = buff;
    }
    tep__ref_light();
    tep_override_comm(tep, comm, pid);
    tep__unref();
}

const char *tep__pid_to_comm(int pid)
{
    const char *comm;

    if (global_comm) {
        comm = global_comm_get(pid);
        return comm ? comm : "<...>";
    }

    tep__ref_light();
    comm = tep_data_comm_from_pid(tep, pid);
    tep__unref();
    return comm;
}

void tep__print_event(unsigned long long ts, int cpu, void *data, int size)
{
    struct tep_record record;
    struct trace_seq s;
    struct tep_event *e;

    ts = (ts + 500) / 1000; // us

    memset(&record, 0, sizeof(record));
    record.ts = ts;
    record.cpu = cpu;
    record.size = size;
    record.data = data;

    tep__ref();
    // For KPROBE, common_type = 0, e = NULL.
    e = tep_find_event_by_record(tep, &record);

    trace_seq_init(&s);
    if (global_comm) {
        int pid = tep_data_pid(tep, &record);
        char *comm = global_comm_get(pid);
        trace_seq_printf(&s, "%16s %6u ", comm ? : "<...>", pid);
        tep_print_event(tep, &s, &record, "%s", TEP_PRINT_LATENCY);
        if (likely(cpu >= 0))
            trace_seq_printf(&s, " [%03d] %llu.%06llu: %s:%s: ", cpu, ts/USEC_PER_SEC, ts%USEC_PER_SEC,
                         e ? e->system : "NULL", e ? e->name : "NULL");
        else
            trace_seq_printf(&s, " [---] %llu.%06llu: %s:%s: ", ts/USEC_PER_SEC, ts%USEC_PER_SEC,
                         e ? e->system : "NULL", e ? e->name : "NULL");
    } else {
        if (likely(cpu >= 0))
            tep_print_event(tep, &s, &record, "%16s %6u %s [%03d] %6d: ", TEP_PRINT_COMM, TEP_PRINT_PID,
                    TEP_PRINT_LATENCY, TEP_PRINT_CPU, TEP_PRINT_TIME);
        else
            tep_print_event(tep, &s, &record, "%16s %6u %s [---] %6d: ", TEP_PRINT_COMM, TEP_PRINT_PID,
                    TEP_PRINT_LATENCY, TEP_PRINT_TIME);
        trace_seq_printf(&s, "%s:%s: ", e ? e->system : "NULL", e ? e->name : "NULL");
    }
    if (e)
        tep_print_event(tep, &s, &record, "%s\n", TEP_PRINT_INFO);
    else
        trace_seq_printf(&s, "\n");
    tep__unref();
    trace_seq_do_fprintf(&s, stdout);
    trace_seq_destroy(&s);
}

bool tep__event_has_field(int id, const char *field)
{
    bool has_field = false;
    struct tep_event *event;

    tep__ref_light();
    event = tep_find_event(tep, id);
    if (event) {
        has_field = !!tep_find_any_field(event, field);
    }
    tep__unref();

    return has_field;
}

bool tep__event_field_size(int id, const char *field)
{
    struct tep_event *event;
    struct tep_format_field *format;
    int size = -1;

    tep__ref_light();
    event = tep_find_event(tep, id);
    if (event) {
        format = tep_find_any_field(event, field);
        if (format)
            size = format->size;
    }
    tep__unref();

    return size;
}

/*
 * Check if a string field should be treated as binary data.
 *
 * Determination order:
 *  1. %p format in print_fmt → binary (pointer-formatted data, e.g., %pI4, %pM)
 *  2. %s format in print_fmt → string
 *  3. u8 or unsigned char type → binary
 *     Uses strstr instead of TEP_FIELD_IS_SIGNED because kernel commit
 *     3bc753c06dd0 ("kbuild: treat char as always unsigned") makes char
 *     unsigned, so IS_SIGNED is unreliable for distinguishing char arrays.
 *  4. Otherwise → string
 *
 * Returns true if the field's IS_STRING flag should be cleared.
 */
bool tep__string_field_as_binary(struct tep_event *event, struct tep_format_field *field)
{
    struct tep_print_parse *parse = event->print_fmt.print_cache;

    if (!(field->flags & TEP_FIELD_IS_STRING))
        return false;

    while (parse) {
        if (parse->type == PRINT_FMT_ARG_POINTER &&
            parse->arg && parse->arg->type == TEP_PRINT_FIELD) {
            if (strcmp(parse->arg->field.name, field->name) == 0)
                return true;
        }
        if (parse->type == PRINT_FMT_ARG_STRING &&
            parse->arg && parse->arg->type == TEP_PRINT_FIELD) {
            if (strcmp(parse->arg->field.name, field->name) == 0)
                return false;
        }
        parse = parse->next;
    }

    if (strstr(field->type, "u8") || strstr(field->type, "unsigned"))
        return true;

    return false;
}

int tep__event_size(int id)
{
    struct tep_event *event;
    struct tep_format_field **fields;
    int size = -1;

    if (id == KPROBE || id == UPROBE)
        return sizeof(struct kprobe_trace_entry_head);
    else if (id == KRETPROBE || id == URETPROBE)
        return sizeof(struct kretprobe_trace_entry_head);

    tep__ref_light();
    event = tep_find_event(tep, id);
    if (event) {
        fields = tep_event_fields(event);
        if (fields) {
            int i = 0;
            while (fields[i]) {
                if (fields[i]->offset + fields[i]->size > size)
                    size = fields[i]->offset + fields[i]->size;
                i++;
            }
            free(fields);
        }
    }
    tep__unref();
    return size;
}

static event_fields *kprobe_uprobe_event_fields(int id)
{
    event_fields *ef = NULL;
    int nr_fields = 0;
    struct trace_entry entry;
    int offset = 0;

    if (id == KPROBE || id == UPROBE)
        nr_fields = 5;
    else if (id == KRETPROBE || id == URETPROBE)
        nr_fields = 6;
    else
        return NULL;

    ef = calloc(1, (nr_fields+1) * sizeof(*ef));
    if (!ef) return NULL;

    #define EF(i, _name, _size, _unsigned) \
        ef[i].name = _name; \
        ef[i].offset = offset; \
        ef[i].size = ef[i].elementsize = _size; \
        ef[i].is_unsigned = _unsigned; \
        offset += _size;

    EF(0, "common_type", sizeof(entry.common_type), 1);
    EF(1, "common_flags", sizeof(entry.common_flags), 1);
    EF(2, "common_preempt_count", sizeof(entry.common_preempt_count), 1);
    EF(3, "common_pid", sizeof(entry.common_pid), 0);
    if (id == KPROBE || id == UPROBE) {
        EF(4, FIELD_STRING_IP, sizeof(unsigned long), 1);
    } else if (id == KRETPROBE || id == URETPROBE) {
        EF(4, FIELD_STRING_FUNC, sizeof(unsigned long), 1);
        EF(5, FIELD_STRING_RETIP, sizeof(unsigned long), 1);
    }
    return ef;
}

event_fields *tep__event_fields(int id)
{
    struct tep_event *event;
    struct tep_format_field **common_fields;
    struct tep_format_field **fields;
    int nr_common = 0, nr_fields = 0, nr_dynamic = 0, first_dynamic = -1;
    int extra_len = 0;
    event_fields *ef = NULL;
    char *extra = NULL;
    int i = 0, f = 0;

    if (id > TRACE_EVENT_TYPE_MAX)
        return kprobe_uprobe_event_fields(id);

    tep__ref_light();
    event = tep_find_event(tep, id);
    if (!event)
        goto _return;

    common_fields = tep_event_common_fields(event);
    fields = tep_event_fields(event);
    if (!common_fields || !fields)
        goto _return;

    while (common_fields[nr_common]) nr_common++;
    while (fields[nr_fields]) {
        if (fields[nr_fields]->flags & TEP_FIELD_IS_DYNAMIC) {
            /* For dynamic fields, we need to create additional fields:
             * - First dynamic field creates 3 entries: base, base_offset, base_len
             * - Subsequent dynamic fields create 2 entries: base_offset, base_len
             */
            int dyns = (nr_dynamic==0 ? 3 : 2);
            extra_len += dyns * strlen(fields[nr_fields]->name) + sizeof("_offset") + sizeof("_len");
            nr_dynamic += dyns-1;
        }
        nr_fields++;
    }

    ef = calloc(1, (nr_common + nr_fields + nr_dynamic + 1) * sizeof(*ef) + extra_len);
    if (!ef)
        goto _free;
    extra = (char *)ef + (nr_common + nr_fields + nr_dynamic + 1) * sizeof(*ef);

    f = 0;
    while (common_fields[f]) {
        ef[i].name = common_fields[f]->name;
        ef[i].offset = common_fields[f]->offset;
        ef[i].size = common_fields[f]->size;
        ef[i].elementsize = common_fields[f]->elementsize;
        ef[i].is_unsigned = !(common_fields[f]->flags & TEP_FIELD_IS_SIGNED);
        i++;
        f++;
    }
    f = 0;
    while (fields[f]) {
        if (fields[f]->flags & TEP_FIELD_IS_DYNAMIC) {
            ef[i].name = extra;
            extra += sprintf((char *)ef[i].name, "%s_offset", fields[f]->name)+1;
            ef[i].offset = fields[f]->offset;
            ef[i].size = 2;
            ef[i].elementsize = 2;
            ef[i].is_unsigned = 1;
            i++;

            ef[i].name = extra;
            extra += sprintf((char *)ef[i].name, "%s_len", fields[f]->name)+1;
            ef[i].offset = fields[f]->offset + 2;
            ef[i].size = 2;
            ef[i].elementsize = 2;
            ef[i].is_unsigned = 1;

            if (first_dynamic < 0)
                first_dynamic = f;
        } else {
            ef[i].name = fields[f]->name;
            ef[i].offset = fields[f]->offset;
            ef[i].size = fields[f]->size;
            ef[i].elementsize = fields[f]->elementsize;
            ef[i].is_unsigned = !(fields[f]->flags & TEP_FIELD_IS_SIGNED);
        }
        i++;
        f++;
    }
    if (first_dynamic >= 0 && i > 0) {
        /* Create the base field entry for the first dynamic field.
         * This field represents the actual dynamic data and is marked
         * as an array type (size=0) to indicate variable length. */
        ef[i].name = fields[first_dynamic]->name;
        ef[i].offset = ef[i-1].offset + ef[i-1].size;
        ef[i].size = 0; /* Mark as ARRAY type for variable length */
        ef[i].elementsize = 1;
        i++;
    }
    ef[i].name = NULL;

_free:
    if (common_fields) free(common_fields);
    if (fields) free(fields);
_return:
    tep__unref();
    return ef;
}

char *next_sep(char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        if (*s == '\'' || *s == '"') {
            int quote = *s++;
            while (*s && *s++ != quote);
        } else if (*s == '(') {
            s = next_sep(++s, ')');
            if (!s) break;
            s++;
        } else
            s++;
    }
    return NULL;
}

static char *rm_quotes(char *s, char fill)
{
    // Remove single and double quotes around `s'
    if (s && (s[0] == '\'' || s[0] == '"')) {
        int len = strlen(s);
        if (s[len-1] == s[0]) {
            s[len-1] = fill;
            s[0] = fill;
            return s+1;
        }
    }
    return s;
}

static int tp_kprobe_uprobe(struct tp *tp, char *sys, char *name)
{
    int id = -1;
    char *path;
    char resolved_path[PATH_MAX];

    // kprobe:func
    // kretprobe:func
    // uprobe:"func@path" | uprobe:func@"path"
    // uretprobe:"func@path" | uretprobe:func@"path"
    if (strcmp(sys, "kprobe") == 0) {
        id = kprobe_type ? KPROBE : -1;
        tp->kprobe_func = name;
    } else if (strcmp(sys, "kretprobe") == 0) {
        id = kprobe_type ? KRETPROBE : -1;
        tp->kprobe_func = name;
    } else if (strcmp(sys, "uprobe") == 0) {
        id = uprobe_type ? UPROBE : -1;
        goto uprobe;
    } else if (strcmp(sys, "uretprobe") == 0) {
        id = uprobe_type ? URETPROBE : -1;
    uprobe:
        tp->uprobe_path = NULL;
        tp->uprobe_offset = 0;
        path = strchr(name, '@');
        if (path) {
            *path++ = '\0';
            path = rm_quotes(path, '\0');
            if (id > 0 &&
                realpath(path, resolved_path) &&
                access(resolved_path, X_OK) == 0) {
                tp->uprobe_path = path;
                tp->uprobe_offset = syms__file_offset(resolved_path, name);
            }
        }
        if (!tp->uprobe_offset)
            id = -1;
    }
    return id;
}

struct wildcard_match_ctx {
    const char *pattern;
    const char *slash;
    char *result;
};

static bool has_wildcard(const char *str)
{
    return strchr(str, '*') != NULL || strchr(str, '?') != NULL || strchr(str, '[') != NULL;
}

// Callback for matching events against wildcard pattern
static void wildcard_match_cb(char **evt_list, int evt_num, void *opaque)
{
    struct wildcard_match_ctx *ctx = opaque;
    int i;

    for (i = 0; i < evt_num; i++) {
        if (fnmatch(ctx->pattern, evt_list[i], 0) == 0) {
            ctx->result = straddf(ctx->result, free, "%s%s%s,", evt_list[i],
                                  ctx->slash ? "/" : "",
                                  ctx->slash ? ctx->slash+1 : "");
            if (!ctx->result)
                return;
        }
    }
}

// Expand wildcard event pattern to concrete event names
// Returns: expanded event string (comma-separated), or NULL on error
// Caller must free the returned string
static char *expand_wildcard_events(const char *pattern, const char *slash)
{
    struct wildcard_match_ctx ctx = {
        .pattern = pattern,
        .slash = slash,
        .result = NULL,
    };

    print_tracepoint_events(wildcard_match_cb, &ctx);
    if (ctx.result == NULL)
        fprintf(stderr, "No events matching pattern '%s'\n", pattern);
    return ctx.result;
}

// Expand all wildcard patterns in event_str
// Returns: expanded event string, or original string if no wildcards
// Caller must free the returned string if it differs from input
static char *expand_event_wildcards(const char *event_str)
{
    char *result = NULL;
    char *tmp_str = NULL;
    char *s, *sep;
    int result_len = 0;

    tmp_str = event_str && *event_str ? strdup(event_str) : NULL;
    if (!tmp_str)
        return NULL;

    s = tmp_str;
    while ((sep = next_sep(s, ',')) != NULL || *s) {
        char *event_name = s;
        char *slash;

        if (sep) {
            *sep = '\0';
            s = sep + 1;
        }
        slash = strchr(event_name, '/');
        if (slash)
            *slash = '\0';

        // Check if event name (before any '/') contains wildcard
        if (has_wildcard(event_name)) {
            // Expand this event pattern
            // If there were attributes after the event, append them to each expanded event
            char *expanded = expand_wildcard_events(event_name, slash);
            if (!expanded) {
                free(tmp_str);
                free(result);
                return NULL;
            }
            result = straddf(result, free, expanded);
            free (expanded);
        } else {
            if (slash)
                *slash = '/';
            result = straddf(result, free, "%s,", event_name);
        }

        if (!result || !sep)
            break;
    }

    if (result) {
        result_len = strlen(result);
        result[result_len - 1] = '\0'; // Remove ','
        free(tmp_str);
        return result;
    } else
        return tmp_str;
}

struct tp_list *tp_list_new(struct prof_dev *dev, char *event_str)
{
    char *expanded_str;
    char *s;
    char *sep;
    int i;
    int nr_tp = 0;
    struct tp_list *tp_list = NULL;

    // Expand wildcard patterns if present
    expanded_str = expand_event_wildcards(event_str);
    if (!expanded_str)
        return NULL;

    if (dev->env->verbose)
        fprintf(stdout, "Wildcard expansion: '%s' -> '%s'\n", event_str, expanded_str);

    s = expanded_str;
    while ((sep = next_sep(s, ',')) != NULL) {
        nr_tp ++;
        s = sep + 1;
    }
    if (*s)
        nr_tp ++;

    if (nr_tp == 0)
        return NULL;

    tp_list = calloc(1, sizeof(struct tp_list) + nr_tp * sizeof(struct tp));
    if (!tp_list)
        return NULL;

    tp_list->event_str = expanded_str;
    tp_list->nr_tp = nr_tp;
    s = tp_list->event_str;
    i = 0;
    while ((sep = next_sep(s, ',')) != NULL) {
        tp_list->tp[i++].name = s;
        *sep = '\0';
        s = sep + 1;
    }
    if (*s)
        tp_list->tp[i++].name = s;

    tep__ref_light();
    /*
     * Event syntax:
     *    EVENT,EVENT,...
     * EVENT:
     *    sys:name[/filter/ATTR/ATTR/.../]
     *    profiler[/option/ATTR/ATTR/.../]
     * ATTR:
     *    stack : sample_type PERF_SAMPLE_CALLCHAIN
     *    max-stack=int : sample_max_stack
     *    top-by=field : top add and sort by this field
     *    top-add=field: top add this field
     *    ...
    **/
    for (i = 0; i < nr_tp; i++) {
        struct tp *tp = &tp_list->tp[i];
        struct tep_event *event = NULL;
        char *slash = NULL;
        char *sys = NULL;
        char *name = NULL;
        char *filter = NULL;
        int stack = 0;
        int max_stack = 0;
        bool top_by;
        char *alias = NULL;
        int id = -1;
        event_fields *fields = NULL;
        struct expr_prog *prog = NULL;
        profiler *prof = NULL;
        struct env *env = NULL;

        tp->dev = dev;

        s = tp->name;
        slash = next_sep(s, '/');
        if (slash) {
            *slash = '\0';
            filter = slash + 1;
            slash = next_sep(filter, '/');
            if (!slash)
                goto err_out;
            *slash = '\0';
        }

        sys = s = tp->name;
        sep = strchr(s, ':');
        if (!sep) { // profiler/option/
        profiler:
            prof = monitor_find(sys);
            if (!prof) {
                fprintf(stderr, "profiler %s not found\n", sys);
                goto err_out;
            }
            if (filter && filter[0])
                filter[-1] = ' ';

            rm_quotes(filter, ' ');

            env = parse_string_options(s);
            if (!env)
                goto err_out;
            // --tsc, --kvmclock remains the same.
            env->tsc = dev->env->tsc;
            env->clock_offset = dev->env->clock_offset;
            if (env->kvmclock) free(env->kvmclock);
            env->kvmclock = dev->env->kvmclock ? strdup(dev->env->kvmclock) : NULL;
            // Using (dev->cpus, dev->threads).
            // Make sure instance is consistent between source_dev and dev.
            tp->source_dev = prof_dev_open_cpu_thread_map(prof, env, dev->cpus, dev->threads, dev);
            if (!tp->source_dev)
                goto err_out;
            prof_dev_use(tp->source_dev);
            name = sys;
            sys = NULL;
        } else { // sys:name/filter/
            *sep = '\0';

            name = sep + 1;
            name = rm_quotes(name, '\0');

            id = tep__event_id(sys, name);
            if (id < 0) {
                id = tp_kprobe_uprobe(tp, sys, name);
                if (id < 0) {
                    *sep = ':';
                    if (monitor_find(sys)) // e.g. bpf:kvm-exit
                        goto profiler;
                    fprintf(stderr, "%s not found\n", sys);
                    goto err_out;
                }
            } else {
                event = tep_find_event_by_name(tep, sys, name);
                if (!event)
                    goto err_out;
            }

            // Remove single and double quotes around filter
            filter = rm_quotes(filter, '\0');
        }
        // ATTR
        if (slash) {
            *slash = '\0';
            s = slash + 1;
            while ((sep = next_sep(s, '/')) != NULL) {
                char *attr = s;
                char *value = NULL;
                *sep = '\0';
                s = sep + 1;
                if ((sep = next_sep(attr, '=')) != NULL) {
                    *sep = '\0';
                    value = sep + 1;
                }
                // Remove single and double quotes around value
                value = rm_quotes(value, '\0');

                top_by = false;
                prog = NULL;
                if (strcmp(attr, "stack") == 0)
                    stack = 1;
                else if (strcmp(attr, "max-stack") == 0) {
                    stack = 1;
                    max_stack = value ? atoi(value) : 0;
                    if (max_stack > PERF_MAX_STACK_DEPTH)
                        max_stack = PERF_MAX_STACK_DEPTH;
                } else if (strcmp(attr, "top-by") == 0 ||
                           strcmp(attr, "top_by") == 0) {
                    top_by = true;
                    goto top_add;
                } else if (strcmp(attr, "top-add") == 0 ||
                           strcmp(attr, "top_add") == 0) {
                    top_add:
                    if (!fields) fields = tep__event_fields(id);
                    if (fields)  prog = expr_compile(value, fields);
                    if (!prog) { free(fields); goto err_out; }

                    tp->nr_top ++;
                    tp->top_add = realloc(tp->top_add, tp->nr_top * sizeof(*tp->top_add));
                    tp->top_add[tp->nr_top-1].field_prog = prog;
                    tp->top_add[tp->nr_top-1].field = value;
                    tp->top_add[tp->nr_top-1].event = false;
                    tp->top_add[tp->nr_top-1].top_by = top_by;
                } else if (strcmp(attr, "comm") == 0) {
                    if (!fields) fields = tep__event_fields(id);
                    if (fields)  prog = expr_compile(value, fields);
                    if (!prog) { free(fields); goto err_out; }

                    tp->comm_prog = prog;
                    tp->comm = value;
                } else if (strcmp(attr, "alias") == 0) {
                    alias = value;
                } else if (strcmp(attr, "ptr") == 0) {
                    if (!fields) fields = tep__event_fields(id);
                    if (fields)  prog = expr_compile(value, fields);
                    if (!prog) { free(fields); goto err_out; }

                    tp->mem_ptr_prog = prog;
                    tp->mem_ptr = value;
                } else if (strcmp(attr, "size") == 0) {
                    if (!fields) fields = tep__event_fields(id);
                    if (fields)  prog = expr_compile(value, fields);
                    if (!prog) { free(fields); goto err_out; }

                    tp->mem_size_prog = prog;
                    tp->mem_size = value;
                } else if (strcmp(attr, "num") == 0) {
                    if (!fields) fields = tep__event_fields(id);
                    if (fields)  prog = expr_compile(value, fields);
                    if (!prog) { free(fields); goto err_out; }

                    tp->num_prog = prog;
                    tp->num = value;
                } else if (strcmp(attr, "key") == 0) {
                    if (!fields) fields = tep__event_fields(id);
                    if (fields)  prog = expr_compile(value, fields);
                    if (!prog) { free(fields); goto err_out; }

                    tp->key_prog = prog;
                    tp->key = value;
                } else if (strcmp(attr, "printkey") == 0) {
                    struct global_var_declare declare[2];
                    declare[0].name = "key";
                    declare[0].offset = 0;
                    declare[0].size = declare[0].elementsize = sizeof(u64);
                    declare[0].is_unsigned = 1;
                    declare[1].name = NULL;
                    tp->printkey_prog = expr_compile(value, declare);
                    tp->printkey = value;
                    if (!tp->printkey_prog)
                        goto err_out;
                } else if (strcmp(attr, "role") == 0) {
                    if (!fields) fields = tep__event_fields(id);
                    if (fields)  prog = expr_compile(value, fields);
                    if (!prog) { free(fields); goto err_out; }

                    tp->role_prog = prog;
                    tp->role = value;
                } else if (strcmp(attr, "untraced") == 0) {
                    tp->untraced = true;
                } else if (strcmp(attr, "trigger") == 0) {
                    tp->trigger = true;
                } else if (strcmp(attr, "push") == 0) {
                    if (id >= 0 && tp_broadcast_new(tp, value) < 0) goto err_out;
                } else if (strcmp(attr, "pull") == 0) {
                    if (id >= 0 && tp_receive_new(tp, value) < 0) goto err_out;
                } else if (strcmp(attr, "vm") == 0) {
                    tp->vcpu = vcpu_info_get(value);
                    if (tp->vcpu)
                        tp->vm = value;
                } else  if (strcmp(attr, "exec") == 0) {
                    if (!fields) fields = tep__event_fields(id);
                    if (fields)  prog = expr_compile(value, fields);
                    if (!prog) { free(fields); goto err_out; }

                    tp->exec_prog = prog;
                    tp->exec = value;
                } else  if (strcmp(attr, "cpus") == 0) {
                    tp->cpus = perf_cpu_map__new(value);
                    tp->cpus = perf_cpu_map__and(tp->cpus, dev->cpus);
                    if (perf_cpu_map__nr(tp->cpus) == 0) {
                        perf_cpu_map__put(tp->cpus);
                        tp->cpus = NULL;
                    }
                } else if (strcmp(attr, "index") == 0) {
                    tp->index = value;
                } else if (*attr) {
                    fprintf(stderr, "Syntax error: Unknown attr %s\n", attr);
                    goto err_out;
                }
            }
            if (*s) {
                fprintf(stderr, "Syntax error: %s\n", s);
                goto err_out;
            }
        }

        tp->id = id;
        tp->idx = i;
        tp->sys = sys;
        tp->name = name;
        tp->filter = (filter && filter[0]) ? strdup(filter) : NULL;
        tp->stack = stack;
        tp->max_stack = max_stack;
        tp->alias = alias;
        tp->kernel = !tp->receive;
        tp->matcher = tp_matcher_find(tp, sys, name);
        if (tp->nr_top == 0) {
            tp->nr_top = 1;
            tp->top_add = realloc(tp->top_add, tp->nr_top * sizeof(*tp->top_add));
            tp->top_add[0].field_prog = NULL;
            tp->top_add[0].field = alias ? : name;
            tp->top_add[0].event = true;
            tp->top_add[0].top_by = false;
        }

        if (!tp->mem_ptr && event && tep_find_any_field(event, "ptr")) {
            if (!fields) fields = tep__event_fields(id);
            if (fields)  prog = expr_compile((char *)"ptr", fields);
            if (!prog) { free(fields); goto err_out; }

            tp->mem_ptr_prog = prog;
            tp->mem_ptr = "ptr";
        }
        if (!tp->mem_size && event && tep_find_any_field(event, "bytes_alloc")) {
            if (!fields) fields = tep__event_fields(id);
            if (fields)  prog = expr_compile((char *)"bytes_alloc", fields);
            if (!prog) { free(fields); goto err_out; }

            tp->mem_size_prog = prog;
            tp->mem_size = "bytes_alloc";
        }

        tp_list->nr_real_tp += id >= 0;
        tp_list->nr_need_stack += stack;
        tp_list->nr_top += tp->nr_top;
        tp_list->nr_comm += !!tp->comm_prog;
        tp_list->nr_mem_size += !!tp->mem_size_prog;
        tp_list->nr_num_prog += !!tp->num_prog;
        tp_list->nr_untraced += !!tp->untraced;
        tp_list->nr_push_to += !!tp->broadcast;
        tp_list->nr_pull_from += !!tp->receive;
        tp_list->nr_exec_prog += !!tp->exec_prog;

        if (fields)
            free(fields);
    }

    tp_list->need_stream_id = (tp_list->nr_need_stack && tp_list->nr_need_stack != tp_list->nr_real_tp);

    tep__unref();
    return tp_list;

err_out:
    tep__unref();
    tp_list_free(tp_list);
    return NULL;
}

void tp_list_free(struct tp_list *tp_list)
{
    int i, j;
    if (!tp_list)
        return ;
    for (i = 0; i < tp_list->nr_tp; i++) {
        struct tp *tp = &tp_list->tp[i];
        if (tp_is_dev(tp) && tp->source_dev)
            prof_dev_unuse(tp->source_dev);
        if (tp->filter)
            free(tp->filter);
        if (tp->ftrace_filter)
            expr_destroy(tp->ftrace_filter);
        for (j = 0; j < tp->nr_top; j++) {
            if (tp->top_add[j].field_prog)
                expr_destroy(tp->top_add[j].field_prog);
        }
        if (tp->top_add)
            free(tp->top_add);
        if (tp->comm_prog)
            expr_destroy(tp->comm_prog);
        if (tp->mem_ptr_prog)
            expr_destroy(tp->mem_ptr_prog);
        if (tp->mem_size_prog)
            expr_destroy(tp->mem_size_prog);
        if (tp->num_prog)
            expr_destroy(tp->num_prog);
        if (tp->key_prog)
            expr_destroy(tp->key_prog);
        if (tp->printkey_prog)
            expr_destroy(tp->printkey_prog);
        if (tp->role_prog)
            expr_destroy(tp->role_prog);
        tp_broadcast_free(tp);
        tp_receive_free(tp);
        if (tp->vcpu)
            vcpu_info_put(tp->vcpu);
        if (tp->exec_prog)
            expr_destroy(tp->exec_prog);
        if (tp->cpus) {
            if (!tp_is_dev(tp) && tp->evsel)
                perf_evsel__set_own_cpus(tp->evsel, NULL);
            perf_cpu_map__put(tp->cpus);
        }
        if (tp->tp_cpu_prog)
            expr_destroy(tp->tp_cpu_prog);
        if (tp->tp_pid_prog)
            expr_destroy(tp->tp_pid_prog);
    }
    free(tp_list->event_str);
    free(tp_list);
}

void tp_update_filter(struct tp *tp, const char *filter)
{
    if (tp->filter)
        free(tp->filter);
    tp->filter = strdup(filter);
}

void tp_print_marker(struct tp *tp)
{
    printf("%c ", (!tp || tp_kernel(tp)) ? ' ' : 'G');
}

struct expr_prog *tp_new_prog(struct tp *tp, char *expr_str)
{
    event_fields *fields = tep__event_fields(tp->id);
    struct expr_prog *prog = NULL;
    if (fields) {
        prog = expr_compile(expr_str, fields);
        free(fields);
    }
    return prog;
}

long tp_prog_run(struct tp *tp, struct expr_prog *prog, struct expr_global *global)
{
    if (expr_load_global(prog, global) != 0) {
        expr_dump(prog);
        fprintf(stderr, "tp %s:%s prog load global failed!\n", tp->sys, tp->name);
        return -1;
    }
    return expr_run(prog);
}

int tp_set_key(struct tp *tp, const char *key)
{
    if (!tp->key || strcmp(tp->key, key)) {
        if (tp->key_prog)
            expr_destroy(tp->key_prog);

        tp->key_prog = tp_new_prog(tp, (char *)key);
        if (!tp->key_prog)
            return -1;
        tp->key = key;

        if (tp->dev->env->verbose >= VERBOSE_NOTICE)
            printf("%s:%s key \"%s\"\n", tp->sys, tp->name, tp->key);
    }
    return 0;
}

long tp_print_key(struct tp *tp, u64 key)
{
    if (tp->printkey_prog)
        return tp_prog_run(tp, tp->printkey_prog, GLOBAL(0, 0, &key, sizeof(key)));
    else
        return 0;
}

char *tp_get_comm(struct tp *tp, struct expr_global *global)
{
    long comm = tp_prog_run(tp, tp->comm_prog, global);
    return comm == -1 ? (char *)"Error" : (char *)(unsigned long)comm;
}

unsigned long tp_get_key(struct tp *tp, struct expr_global *global)
{
    long key = tp_prog_run(tp, tp->key_prog, global);
    return key == -1 ? 0 : (unsigned long)key;
}

unsigned long tp_get_role(struct tp *tp, struct expr_global *global)
{
    long key = tp_prog_run(tp, tp->role_prog, global);
    return key == -1 ? 0 : (unsigned long)key;
}

unsigned long tp_get_num(struct tp *tp, struct expr_global *global)
{
    long num = tp_prog_run(tp, tp->num_prog, global);
    return num == -1 ? 0 : (unsigned long)num;
}

void *tp_get_mem_ptr(struct tp *tp, struct expr_global *global)
{
    long mem_ptr = tp_prog_run(tp, tp->mem_ptr_prog, global);
    return mem_ptr == -1 ? NULL : (void *)mem_ptr;
}

unsigned long tp_get_mem_size(struct tp *tp, struct expr_global *global)
{
    long mem_size = tp_prog_run(tp, tp->mem_size_prog, global);
    return mem_size == -1 ? 1 : (unsigned long)mem_size;
}

/* taken from kernel/trace/trace_output.c */
static void trace_print_lat_fmt(struct trace_entry *entry)
{
    unsigned char flags = entry->common_flags;
    unsigned char preempt_count = entry->common_preempt_count;
    char hardsoft_irq;
    char need_resched;
    char irqs_off;
    int hardirq;
    int softirq;
    int bh_off;
    int nmi;

    nmi = flags & TRACE_FLAG_NMI;
    hardirq = flags & TRACE_FLAG_HARDIRQ;
    softirq = flags & TRACE_FLAG_SOFTIRQ;
    bh_off = flags & TRACE_FLAG_BH_OFF;

    irqs_off =
        (flags & TRACE_FLAG_IRQS_OFF && bh_off) ? 'D' :
        (flags & TRACE_FLAG_IRQS_OFF) ? 'd' :
        bh_off ? 'b' :
        '.';

    switch (flags & (TRACE_FLAG_NEED_RESCHED | TRACE_FLAG_NEED_RESCHED_LAZY |
                TRACE_FLAG_PREEMPT_RESCHED)) {
    case TRACE_FLAG_NEED_RESCHED | TRACE_FLAG_NEED_RESCHED_LAZY | TRACE_FLAG_PREEMPT_RESCHED:
        need_resched = 'B';
        break;
    case TRACE_FLAG_NEED_RESCHED | TRACE_FLAG_PREEMPT_RESCHED:
        need_resched = 'N';
        break;
    case TRACE_FLAG_NEED_RESCHED_LAZY | TRACE_FLAG_PREEMPT_RESCHED:
        need_resched = 'L';
        break;
    case TRACE_FLAG_NEED_RESCHED | TRACE_FLAG_NEED_RESCHED_LAZY:
        need_resched = 'b';
        break;
    case TRACE_FLAG_NEED_RESCHED:
        need_resched = 'n';
        break;
    case TRACE_FLAG_PREEMPT_RESCHED:
        need_resched = 'p';
        break;
    case TRACE_FLAG_NEED_RESCHED_LAZY:
        need_resched = 'l';
        break;
    default:
        need_resched = '.';
        break;
    }

    hardsoft_irq =
        (nmi && hardirq)     ? 'Z' :
        nmi                  ? 'z' :
        (hardirq && softirq) ? 'H' :
        hardirq              ? 'h' :
        softirq              ? 's' :
                               '.' ;

    if (!preempt_count)
        printf("%c%c%c.", irqs_off, need_resched, hardsoft_irq);
    else {
        printf("%c%c%c", irqs_off, need_resched, hardsoft_irq);
        if (preempt_count & 0xf)
            printf("%x", preempt_count & 0xf);
        if (preempt_count & 0xf0)
            printf("%x", preempt_count >> 4);
    }
}

void tp_print_event(struct tp *tp, unsigned long long ts, int cpu, void *data, int size)
{
    if (!tp)
        tep__print_event(ts, cpu, data, size);
    else {
        struct trace_entry *entry = data;
        int pid = entry->common_pid;
        char *comm = global_comm_get(pid);

        printf("%16s %6u ", comm ? : "<...>", pid);
        trace_print_lat_fmt(entry);

        if (likely(cpu >= 0))
             printf(" [%03d]", cpu);
        else printf(" [---]");

        ts = (ts + 500) / 1000; // us
        printf(" %llu.%06llu: %s:%s:%s", ts/USEC_PER_SEC, ts%USEC_PER_SEC,
               tp->sys, tp->name, tp->id <= URETPROBE ? "" : "\n");

        if (tp->id <= TRACE_EVENT_TYPE_MAX) {
            struct tep_record record = {.size = size, .data = data};
            struct tep_event *e = tep_find_event_by_record(tep__ref_light(), &record);
            if (e) {
                static struct trace_seq s;
                static int inited = 0;

                if (!inited) {
                    inited = 1;
                    trace_seq_init(&s);
                } else
                    trace_seq_reset(&s);

                tep_print_event(tep, &s, &record, " %s\n", TEP_PRINT_INFO);
                trace_seq_do_fprintf(&s, stdout);
            } else
                printf("\n");
            tep__unref();
        } else {
            if (tp->id == KPROBE) {
                struct kprobe_trace_entry_head *field = data;
                printf(" (0x%lx)\n", field->ip);
            } else if (tp->id == KRETPROBE) {
                struct kretprobe_trace_entry_head *field = data;
                printf(" (0x%lx <- 0x%lx)\n", field->ret_ip, field->func);
            } else if (tp->id == UPROBE) {
                struct uprobe_trace_entry_head *field = data;
                printf(" (0x%lx)\n", field->vaddr[0]);
            } else if (tp->id == URETPROBE) {
                struct uprobe_trace_entry_head *field = data;
                printf(" (0x%lx <- 0x%lx)\n", field->vaddr[1], field->vaddr[0]);
            }
        }
    }
}

static void __config_retprobe(const char *retprobe, struct perf_event_attr *attr)
{
    const char *sep = strchr(retprobe, ':');
    int bit = atoi(sep + 1);

    //config:0
    if (strncmp(retprobe, "config", 6) == 0)
        attr->config |= (1<<bit);
}

// Create a per-event evsel from the template attr. Copy tmpl to a local attr
// so per-event modifications (type, config, callchain, DWARF sampling) don't
// leak back to the caller. When tp->stack is set (via /stack/ attribute or
// env->callchain), enable PERF_SAMPLE_CALLCHAIN and DWARF regs/stack sampling.
struct perf_evsel *tp_evsel_new(struct tp *tp, struct perf_event_attr *tmpl)
{
    struct perf_event_attr new;
    struct perf_event_attr *attr = &new;
    struct perf_evsel *evsel;

    if (tp_is_dev(tp))
        return NULL;

    *attr = *tmpl;

    if (tp->id <= TRACE_EVENT_TYPE_MAX) {
        attr->type = PERF_TYPE_TRACEPOINT;
        attr->config = tp->id;
    } else {
        if (attr->sample_period != 0 &&
            !(attr->sample_type & (PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_ID |
                                   PERF_SAMPLE_STREAM_ID))) {
            fprintf(stderr, "kprobe, uprobe: PERF_SAMPLE_ID is required.\n");
            return NULL;
        }
        attr->config = 0;

        switch (tp->id) {
        case KRETPROBE:
            __config_retprobe(kprobe_retprobe, attr);
            // passthrough
        case KPROBE:
            attr->type = kprobe_type;
            attr->kprobe_func = (__u64)tp->kprobe_func;
            break;

        case URETPROBE:
            __config_retprobe(uprobe_retprobe, attr);
            // passthrough
        case UPROBE:
            attr->type = uprobe_type;
            attr->uprobe_path = (__u64)tp->uprobe_path;
            attr->probe_offset = tp->uprobe_offset;
            break;
        default:
            return NULL;
        }
    }

    attr->sample_max_stack = tp->max_stack;
    if (tp->stack) {
        attr->sample_type |= PERF_SAMPLE_CALLCHAIN;

        // --user-callchain=dwarf
        // Only sample REGS_USER/STACK_USER for events with callchain enabled.
        if (tp->dev->env->dwarf_record_size) {
            attr->sample_type |= PERF_SAMPLE_REGS_USER | PERF_SAMPLE_STACK_USER;
            attr->sample_regs_user = PERF_REGS_MASK;
            attr->sample_stack_user = tp->dev->env->dwarf_record_size;
            /* Enable kernel FP-based user callchain */
            attr->exclude_callchain_user = 0;
            tp->dwarf_unwind = 1;
        }
    }

    evsel = perf_evsel__new(attr);
    if (!evsel) {
        return NULL;
    }

    tp->evsel = evsel;
    if (!tp->stack)
        tp->stack = attr->sample_type & PERF_SAMPLE_CALLCHAIN;

    if (!tp->dwarf_unwind) {
        u64 dwarf_sample_type = PERF_SAMPLE_CALLCHAIN |
                                PERF_SAMPLE_REGS_USER | PERF_SAMPLE_STACK_USER;
        tp->dwarf_unwind = (attr->sample_type & dwarf_sample_type) == dwarf_sample_type;
    }

    if (!tp_kernel(tp))
        perf_evsel__keep_disable(evsel, true);
    if (tp->cpus)
        perf_evsel__set_own_cpus(evsel, tp->cpus);

    perf_evsel_link_tp(evsel, tp);

    return evsel;
}

struct tp *tp_from_evsel(struct perf_evsel *evsel, struct tp_list *tp_list)
{
    /* Fast path: direct access via evsel->external->tp */
    struct tp *tp = evsel ? perf_evsel_tp(evsel) : NULL;
    int i;

    if (tp)
        return tp;

    /* Fallback: iterate tp_list if perf_evsel_link_tp() failed */
    for_each_real_tp(tp_list, tp, i) {
        if (tp->evsel == evsel)
            return tp;
    }
    return NULL;
}

int tp_list_apply_filter(struct prof_dev *dev, struct tp_list *tp_list)
{
    struct tp *tp;
    event_fields *fields;
    int i, err;
    int fallback = 0;

    for_each_real_tp(tp_list, tp, i) {
        if (tp->filter && tp->filter[0] && tp->evsel) {
            err = perf_evsel__apply_filter(tp->evsel, tp->filter);
            if (err < 0) {
                err = -errno;
                if (dev && !dev->prof->ftrace_filter)
                    goto err_return;

                fields = tep__event_fields(tp->id);
                if (fields) {
                    tp->ftrace_filter = expr_compile(tp->filter, fields);
                    free(fields);
                }
                if (!tp->ftrace_filter)
                    goto err_return;

                printf("%s:%s filters '%s' in userspace\n", tp->sys, tp->name, tp->filter);
                fallback++;
                if (dev)
                    dev->ftrace_filter = 1;
            }
        }
    }
    return fallback;

err_return:
    fprintf(stderr, "%s:%s filter '%s' failed, %d\n", tp->sys, tp->name, tp->filter, err);
    return err;
}

long tp_list_ftrace_filter(struct prof_dev *dev, struct tp_list *tp_list, struct expr_global *global)
{
    unsigned short common_type;
    struct tp *tp;
    int i;

    if (!global || !global->data)
        return -1;

    common_type = *(unsigned short *)global->data;

    for_each_real_tp(tp_list, tp, i) {
        if (tp->id == common_type) {
            if (!tp->ftrace_filter)
                return 1;
            else
                return tp_prog_run(tp, tp->ftrace_filter, global);
        }
    }
    return -1;
}

static LIST_HEAD(tp_matcher_list);
static struct tp_matcher default_tp_matcher;

void tp_matcher_register(struct tp_matcher *matcher)
{
    INIT_LIST_HEAD(&matcher->link);
    list_add_tail(&matcher->link, &tp_matcher_list);
}

struct tp_matcher *tp_matcher_find(struct tp *tp, const char *sys, const char *name)
{
    struct tp_matcher *matcher;
    struct tep_event *event = NULL;
    event_fields *fields = NULL;

    if (!name)
        return NULL;

    list_for_each_entry(matcher, &tp_matcher_list, link) {
        if ((matcher->sys == sys /* sys == NULL */ ||
                (matcher->sys != NULL && sys != NULL && strcmp(matcher->sys, sys) == 0)) &&
            strcmp(matcher->name, name) == 0)
            return matcher;
    }

    if (!tp || tp_is_dev(tp))
        return NULL;

    matcher = NULL;
    tep__ref_light();

    /*
     * default tp_matcher
     *
     * Get the 'cpu' and 'pid' fields from tracepoint(@raw, @size) and
     * determine whether they are the same. The cpu and pid fields will
     * be compiled as an expression.
     */
    event = tep_find_event_by_name(tep, sys, name);
    if (!event) goto out;
    fields = tep__event_fields(event->id);
    if (!fields) goto out;

    if (!tp->tp_cpu_prog && tep_find_any_field(event, "cpu"))
        tp->tp_cpu_prog = expr_compile((char *)"cpu", fields);
    if (!tp->tp_pid_prog && tep_find_any_field(event, "pid"))
        tp->tp_pid_prog = expr_compile((char *)"pid", fields);

    if (tp->tp_cpu_prog || tp->tp_pid_prog)
        matcher = &default_tp_matcher;

out:
    if (fields) free(fields);
    tep__unref();
    return matcher;
}

static bool default_samecpu(struct tp *tp, void *raw, int size, int cpu)
{
    if (tp->tp_cpu_prog) {
        long result = tp_prog_run(tp, tp->tp_cpu_prog, GLOBAL(0, 0, raw, size));
        return result == cpu;
    } else
        return false;
}

static bool default_samepid(struct tp *tp, void *raw, int size, int pid)
{
    if (tp->tp_pid_prog) {
        long result = tp_prog_run(tp, tp->tp_pid_prog, GLOBAL(0, 0, raw, size));
        return result == pid;
    } else
        return false;
}

static struct tp_matcher default_tp_matcher = {
    .sys = "default",
    .name = "default",
    .samecpu = default_samecpu,
    .samepid = default_samepid,
    .target_cpu = NULL,
    .oncpu = NULL,
};


static bool __sched_wakeup_samecpu(struct tp *tp, void *raw, int size, int cpu)
{
    if (size == TP_RAW_SIZE(struct sched_wakeup)) {
        return ((struct sched_wakeup *)raw)->target_cpu == cpu;
    } else if (size == TP_RAW_SIZE(struct sched_wakeup_no_success)) {
        return ((struct sched_wakeup_no_success *)raw)->target_cpu == cpu;
    } else
        return false;
}

static bool __sched_wakeup_samepid(struct tp *tp, void *raw, int size, int pid)
{
    return pid == ((struct sched_wakeup *)raw)->pid;
}

static bool __sched_wakeup_target_cpu(struct tp *tp, void *raw, int size, int cpu, int pid, int *target_cpu, const char **reason)
{
    if (pid == ((struct sched_wakeup *)raw)->pid) {
        if (size == TP_RAW_SIZE(struct sched_wakeup)) {
            *target_cpu = ((struct sched_wakeup *)raw)->target_cpu;
            *reason = "[wakeup]";
            return true;
        } else if (size == TP_RAW_SIZE(struct sched_wakeup_no_success)) {
            *target_cpu = ((struct sched_wakeup_no_success *)raw)->target_cpu;
            *reason = "[wakeup]";
            return true;
        }
    }
    return false;
}

static bool __sched_switch_samepid(struct tp *tp, void *raw, int size, int pid)
{
    return pid == ((struct sched_switch *)raw)->prev_pid ||
           pid == ((struct sched_switch *)raw)->next_pid;
}

static long preempt_state = 0;
__ctor
static void __sched_switch_preempt_state(void)
{
    #define TASK_STATE_MAX      1024

    /*
    #define TASK_REPORT         (TASK_RUNNING | TASK_INTERRUPTIBLE | \
                         TASK_UNINTERRUPTIBLE | __TASK_STOPPED | \
                         __TASK_TRACED | EXIT_DEAD | EXIT_ZOMBIE | \
                         TASK_PARKED)
    #define TASK_REPORT_IDLE    (TASK_REPORT + 1)
    #define TASK_REPORT_MAX     (TASK_REPORT_IDLE << 1)
    */
    #define TASK_REPORT_MAX  0x100 // kernel 4.14 and later.

    if (kernel_release() >= KERNEL_VERSION(4, 14, 0))
        preempt_state = TASK_REPORT_MAX;
    else
        preempt_state = TASK_STATE_MAX;
}

static bool __sched_switch_target_cpu(struct tp *tp, void *raw, int size, int cpu, int pid, int *target_cpu, const char **reason)
{
    struct sched_switch *sched_switch = raw;

    if (pid == sched_switch->next_pid) {
        *target_cpu = cpu;
        *reason = "[running]";
        return true;
    } else if (pid == sched_switch->prev_pid) {
        if (sched_switch->prev_state == 0 ||
            sched_switch->prev_state == preempt_state) {
            *target_cpu = cpu;
            *reason = "[preempt]";
        } else {
            *target_cpu = -1;
            *reason = sched_switch->prev_state == 1 ? "[sleeping]" : "[waiting]";
        }
        return true;
    }
    return false;
}

static bool __sched_switch_oncpu(struct tp *tp, void *raw, int size, int *pid, const char **prev_comm)
{
    struct sched_switch *sched_switch = raw;
    *pid = sched_switch->next_pid;
    *prev_comm = sched_switch->prev_comm;
    return true;
}

static bool __sched_migrate_task_samecpu(struct tp *tp, void *raw, int size, int cpu)
{
    return cpu == ((struct sched_migrate_task *)raw)->orig_cpu ||
           cpu == ((struct sched_migrate_task *)raw)->dest_cpu;
}

static bool __sched_migrate_task_samepid(struct tp *tp, void *raw, int size, int pid)
{
    return pid == ((struct sched_migrate_task *)raw)->pid;
}

static bool __sched_migrate_task_target_cpu(struct tp *tp, void *raw, int size, int cpu, int pid, int *target_cpu, const char **reason)
{
    if (pid == ((struct sched_migrate_task *)raw)->pid) {
        *target_cpu = ((struct sched_migrate_task *)raw)->dest_cpu;
        *reason = "[migrate]";
        return true;
    }
    return false;
}

static bool __sched_stat_runtime_samepid(struct tp *tp, void *raw, int size, int pid)
{
    return pid == ((struct sched_stat_runtime *)raw)->pid;
}

static bool __sched_process_free_samepid(struct tp *tp, void *raw, int size, int pid)
{
    return pid == ((struct sched_process_free *)raw)->pid;
}

static bool __sched_process_fork_samepid(struct tp *tp, void *raw, int size, int pid)
{
    return pid == ((struct sched_process_fork *)raw)->parent_pid ||
           pid == ((struct sched_process_fork *)raw)->child_pid;
}

static bool __sched_process_exec_samepid(struct tp *tp, void *raw, int size, int pid)
{
    return pid == ((struct sched_process_exec *)raw)->pid;
}


TP_MATCHER_REGISTER5("sched", "sched_wakeup", __sched_wakeup_samecpu, __sched_wakeup_samepid, __sched_wakeup_target_cpu);
TP_MATCHER_REGISTER5("sched", "sched_waking", __sched_wakeup_samecpu, __sched_wakeup_samepid, __sched_wakeup_target_cpu);
TP_MATCHER_REGISTER5("sched", "sched_wakeup_new", __sched_wakeup_samecpu, __sched_wakeup_samepid, __sched_wakeup_target_cpu);
TP_MATCHER_REGISTER6("sched", "sched_switch", NULL, __sched_switch_samepid, __sched_switch_target_cpu, __sched_switch_oncpu);
TP_MATCHER_REGISTER5("sched", "sched_migrate_task", __sched_migrate_task_samecpu, __sched_migrate_task_samepid, __sched_migrate_task_target_cpu);
TP_MATCHER_REGISTER("sched", "sched_stat_runtime", NULL, __sched_stat_runtime_samepid);
TP_MATCHER_REGISTER("sched", "sched_process_free", NULL, __sched_process_free_samepid);
TP_MATCHER_REGISTER("sched", "sched_process_exit", NULL, __sched_process_free_samepid);
TP_MATCHER_REGISTER("sched", "sched_process_fork", NULL, __sched_process_fork_samepid);
TP_MATCHER_REGISTER("sched", "sched_process_exec", NULL, __sched_process_exec_samepid);

