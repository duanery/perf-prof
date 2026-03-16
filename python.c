// SPDX-License-Identifier: GPL-2.0
/*
 * python - Python scripting profiler for perf-prof
 *
 * Convert perf events to Python objects and process them with Python scripts.
 *
 * Usage: perf-prof python -e EVENT script.py [script-args...]
 *        perf-prof python -e EVENT -- script.py --script-option
 */

/* Python.h must be included first */
#include <Python.h>
#include <structmember.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <monitor.h>
#include <dlfcn.h>
#include <tep.h>
#include <trace_helpers.h>
#include <stack_helpers.h>
#include <linux/bitmap.h>
#include <linux/stringify.h>
#include <asm/perf_regs.h>

/*
 * Register name tables for PERF_SAMPLE_REGS_USER / PERF_SAMPLE_REGS_INTR.
 * Indexed by bit position in sample_regs_user / sample_regs_intr mask.
 */
#if defined(__x86_64__)
static const char *perf_reg_names[] = {
    [PERF_REG_X86_AX]    = "ax",
    [PERF_REG_X86_BX]    = "bx",
    [PERF_REG_X86_CX]    = "cx",
    [PERF_REG_X86_DX]    = "dx",
    [PERF_REG_X86_SI]    = "si",
    [PERF_REG_X86_DI]    = "di",
    [PERF_REG_X86_BP]    = "bp",
    [PERF_REG_X86_SP]    = "sp",
    [PERF_REG_X86_IP]    = "ip",
    [PERF_REG_X86_FLAGS] = "flags",
    [PERF_REG_X86_CS]    = "cs",
    [PERF_REG_X86_SS]    = "ss",
    [PERF_REG_X86_DS]    = "ds",
    [PERF_REG_X86_ES]    = "es",
    [PERF_REG_X86_FS]    = "fs",
    [PERF_REG_X86_GS]    = "gs",
    [PERF_REG_X86_R8]    = "r8",
    [PERF_REG_X86_R9]    = "r9",
    [PERF_REG_X86_R10]   = "r10",
    [PERF_REG_X86_R11]   = "r11",
    [PERF_REG_X86_R12]   = "r12",
    [PERF_REG_X86_R13]   = "r13",
    [PERF_REG_X86_R14]   = "r14",
    [PERF_REG_X86_R15]   = "r15",
};
#define PERF_REG_MAX  PERF_REG_X86_64_MAX
#elif defined(__i386__)
static const char *perf_reg_names[] = {
    [PERF_REG_X86_AX]    = "ax",
    [PERF_REG_X86_BX]    = "bx",
    [PERF_REG_X86_CX]    = "cx",
    [PERF_REG_X86_DX]    = "dx",
    [PERF_REG_X86_SI]    = "si",
    [PERF_REG_X86_DI]    = "di",
    [PERF_REG_X86_BP]    = "bp",
    [PERF_REG_X86_SP]    = "sp",
    [PERF_REG_X86_IP]    = "ip",
    [PERF_REG_X86_FLAGS] = "flags",
    [PERF_REG_X86_CS]    = "cs",
    [PERF_REG_X86_SS]    = "ss",
    [PERF_REG_X86_DS]    = "ds",
    [PERF_REG_X86_ES]    = "es",
    [PERF_REG_X86_FS]    = "fs",
    [PERF_REG_X86_GS]    = "gs",
};
#define PERF_REG_MAX  PERF_REG_X86_32_MAX
#elif defined(__aarch64__)
static const char *perf_reg_names[] = {
    [PERF_REG_ARM64_X0]  = "x0",
    [PERF_REG_ARM64_X1]  = "x1",
    [PERF_REG_ARM64_X2]  = "x2",
    [PERF_REG_ARM64_X3]  = "x3",
    [PERF_REG_ARM64_X4]  = "x4",
    [PERF_REG_ARM64_X5]  = "x5",
    [PERF_REG_ARM64_X6]  = "x6",
    [PERF_REG_ARM64_X7]  = "x7",
    [PERF_REG_ARM64_X8]  = "x8",
    [PERF_REG_ARM64_X9]  = "x9",
    [PERF_REG_ARM64_X10] = "x10",
    [PERF_REG_ARM64_X11] = "x11",
    [PERF_REG_ARM64_X12] = "x12",
    [PERF_REG_ARM64_X13] = "x13",
    [PERF_REG_ARM64_X14] = "x14",
    [PERF_REG_ARM64_X15] = "x15",
    [PERF_REG_ARM64_X16] = "x16",
    [PERF_REG_ARM64_X17] = "x17",
    [PERF_REG_ARM64_X18] = "x18",
    [PERF_REG_ARM64_X19] = "x19",
    [PERF_REG_ARM64_X20] = "x20",
    [PERF_REG_ARM64_X21] = "x21",
    [PERF_REG_ARM64_X22] = "x22",
    [PERF_REG_ARM64_X23] = "x23",
    [PERF_REG_ARM64_X24] = "x24",
    [PERF_REG_ARM64_X25] = "x25",
    [PERF_REG_ARM64_X26] = "x26",
    [PERF_REG_ARM64_X27] = "x27",
    [PERF_REG_ARM64_X28] = "x28",
    [PERF_REG_ARM64_X29] = "x29",
    [PERF_REG_ARM64_LR]  = "lr",
    [PERF_REG_ARM64_SP]  = "sp",
    [PERF_REG_ARM64_PC]  = "pc",
};
#define PERF_REG_MAX  PERF_REG_ARM64_MAX
#else
static const char *perf_reg_names[] = {};
#define PERF_REG_MAX  0
#endif

#define PERF_REG_NAMES_SIZE (sizeof(perf_reg_names) / sizeof(perf_reg_names[0]))

/* Interned Python string keys for register names, initialized by init_perf_interned_keys() */
static PyObject *perf_reg_keys[PERF_REG_MAX > 0 ? PERF_REG_MAX : 1];
static PyObject *perf_reg_key_abi;  /* interned "abi" key */

/* Interned Python string keys for PERF_SAMPLE_READ fields */
static PyObject *perf_read_key_value;
static PyObject *perf_read_key_time_enabled;
static PyObject *perf_read_key_time_running;
static PyObject *perf_read_key_id;
static PyObject *perf_read_key_lost;
static PyObject *perf_read_key_nr;
static PyObject *perf_read_key_cntr;

static int register_perf_prof_module(void);

/* Global script path and arguments, set by argc_init before init */
static char *script_path = NULL;
static int script_argc = 0;
static char **script_argv = NULL;

/*
 * Sample type for Python profiler (no callchain support in initial version)
 * PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU |
 * PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW
 *
 * Same layout as struct sql_sample_type in sqlite/ext.h
 */
struct python_sample_type {
    struct {
        u32    pid;
        u32    tid;
    }    tid_entry;
    u64   time;
    u64   id;
    struct {
        u32    cpu;
        u32    reserved;
    }    cpu_entry;
    u64   period;
    struct {
        u32   size;
        union {
            u8    data[0];
            struct trace_entry common;
        };
    } raw;
};

/*
 * Cached Python string objects for common dict keys.
 * Using interned strings avoids repeated string hashing in PyDict_SetItem.
 * Access via dev->private->key_cache.
 */
struct python_key_cache {
    /* Common sample fields - for PerfEventObject */
    PyObject *key_pid;              /* "_pid" */
    PyObject *key_tid;              /* "_tid" */
    PyObject *key_time;             /* "_time" */
    PyObject *key_cpu;              /* "_cpu" */
    PyObject *key_period;           /* "_period" */
    /* Common trace_entry fields */
    PyObject *key_common_type;      /* "common_type" */
    PyObject *key_common_flags;     /* "common_flags" */
    PyObject *key_common_preempt_count; /* "common_preempt_count" */
    PyObject *key_common_pid;       /* "common_pid" */
    /* Lazy computed fields */
    PyObject *key_realtime;         /* "_realtime" - wall clock time (ns) */
    PyObject *key_callchain;        /* "_callchain" */
    PyObject *key_event;            /* "_event" */
};

/*
 * Per-event data: handler, fields, and cached Python strings.
 * Access via tp->private.
 */
struct python_event_data {
    PyObject *handler;              /* sys__event_name handler */
    PyObject *event_name;           /* Cached "sys:name" or "sys:alias" string for _event field */
    struct tep_format_field **fields;  /* Event fields from tep */
    PyObject **field_keys;          /* Cached PyUnicode for each field name */
    int nr_fields;
};

struct python_ctx {
    struct tp_list *tp_list;
    char *script_path;
    PyObject *perf_prof_module;  /* perf_prof built-in module (keeps types alive) */
    PyObject *module;
    PyObject *func_init;
    PyObject *func_exit;
    PyObject *func_print_stat;
    PyObject *func_interval;
    PyObject *func_lost;
    PyObject *func_sample;  /* default __sample__ handler */
    /* Cached common key strings */
    struct python_key_cache key_cache;
    /* Per-event data array */
    struct python_event_data *events;
    int nr_events;
    /* Callchain support */
    int callchain_flags;    /* CALLCHAIN_KERNEL | CALLCHAIN_USER flags */
    struct callchain_ctx *cc;  /* Callchain context for printing */
    /* minevtime tracking: rb tree of live PerfEventObjects sorted by _time */
    struct rb_root live_events;
    unsigned long nr_live_events;  /* Number of events in live_events tree */
};

/*
 * ============================================================================
 * PerfEvent Type
 *
 * A Python type that represents a perf event with lazy field evaluation.
 * Direct access to common fields, lazy computation for derived fields,
 * and lazy parsing for event-specific fields with caching.
 * ============================================================================
 */

/* Forward declarations */
static PyTypeObject PerfEventType;
static PyTypeObject PerfEventIterType;

/*
 * PerfEventObject - Python object representing a perf event
 *
 * Memory layout optimized for fast direct field access while supporting
 * lazy evaluation for computed and event-specific fields.
 *
 * Key design:
 * - event: Pointer to union perf_event (borrowed or owned)
 * - tp: Borrowed reference to tracepoint info (valid during sample processing)
 * - dev: Borrowed reference to prof_dev (for accessing key_cache and time conversion)
 *
 * Event ownership (determined by rb_node state):
 * - Initially, event points to the original perf_event (borrowed)
 * - If Python script keeps a reference (refcnt > 1 after handler returns),
 *   live_events_insert() copies the event and inserts into rb tree
 * - RB_EMPTY_NODE(&rb_node) means borrowed; in tree means we own the copy
 * - This optimization avoids copying for events that are processed and discarded
 */
typedef struct {
    PyObject_HEAD
    /* Core pointers */
    struct prof_dev *dev;           /* Prof dev - borrowed reference */
    struct tp *tp;                  /* Tracepoint info - borrowed reference */
    union perf_event *event;        /* Entire perf event - borrowed or owned copy */
    /* minevtime tracking: node in python_ctx->live_events rb tree */
    struct rb_node rb_node;

    /* Direct access fields (via PyMemberDef) - extracted from event */
    int _pid;                       /* Process ID */
    int _tid;                       /* Thread ID */
    unsigned long long _time;       /* Event timestamp (ns) */
    int _cpu;                       /* CPU number */
    int instance;                   /* Instance number */
    unsigned long long _period;     /* Sample period (tracepoint only) */

    /* Lazy computed fields (via PyGetSetDef) - cached when first accessed */
    PyObject *_realtime;            /* Wall clock time - lazy computed */
    PyObject *_callchain_list;      /* Callchain as Python list (tracepoint only) - lazy computed */

    /* Event-specific field cache (dict for lazy parsed fields) */
    PyObject *field_cache;          /* Dict caching event-specific field values */
} PerfEventObject;

/*
 * Iterator object for PerfEvent
 */
typedef struct {
    PyObject_HEAD
    PerfEventObject *event;
    PyObject *keys;
    Py_ssize_t index;
} PerfEventIterObject;

static PyObject *PerfEvent_get_callchain(PerfEventObject *self, void *closure);

/* Helper to get raw data from PerfEventObject */
static inline void perfevent_get_raw(PerfEventObject *self, void **raw, int *raw_size,
                                      struct callchain **callchain)
{
    struct python_sample_type *data = (void *)self->event->sample.array;
    bool has_callchain = self->tp->stack;

    if (has_callchain) {
        struct callchain *cc = (struct callchain *)&data->raw;
        struct {
            __u32 size;
            __u8 data[0];
        } *raw_data = (void *)cc->ips + cc->nr * sizeof(__u64);

        if (callchain) *callchain = cc;
        *raw = raw_data->data;
        *raw_size = raw_data->size;
    } else {
        if (callchain) *callchain = NULL;
        *raw = data->raw.data;
        *raw_size = data->raw.size;
    }
}

/*
 * Common field names for tracepoint events.
 * Tracepoint events have: _pid,_tid,_time,_cpu,_period,common_type,common_flags,
 *                         common_preempt_count,common_pid,_realtime,_callchain,_event + tep fields
 */
static const char *tp_common_field_names[] = {
    "_pid", "_tid", "_time", "_cpu", "_period",
    "common_type", "common_flags", "common_preempt_count", "common_pid",
    "_realtime", "_callchain", "_event",
    NULL
};

/*
 * Common field names for profiler (dev_tp) events.
 * Profiler events have: _pid,_tid,_time,_cpu,_realtime,_event + member_cache fields
 */
static const char *dev_common_field_names[] = {
    "_pid", "_tid", "_time", "_cpu", "_realtime", "_event",
    NULL
};

/* Check if a field name is a common field for the given tp type */
static int is_common_field(const char *name, int is_dev)
{
    const char **p;
    const char **fields = is_dev ? dev_common_field_names : tp_common_field_names;
    for (p = fields; *p; p++) {
        if (strcmp(name, *p) == 0)
            return 1;
    }
    return 0;
}

/* Parse a single event-specific field */
static PyObject *parse_event_field(void *raw_data, struct tep_format_field *field)
{
    PyObject *value = NULL;
    void *base = raw_data;
    void *ptr;
    int len;

    if (field->flags & TEP_FIELD_IS_STRING) {
        /* String field */
        if (field->flags & TEP_FIELD_IS_DYNAMIC) {
            ptr = base + *(unsigned short *)(base + field->offset);
            len = *(unsigned short *)(base + field->offset + sizeof(unsigned short));
            if (len > 0)
                value = PyUnicode_FromStringAndSize((char *)ptr, len - 1);
            else
                value = PyUnicode_FromString("");
        } else {
            ptr = base + field->offset;
            value = PyUnicode_FromString((char *)ptr);
        }
    } else if (field->flags & TEP_FIELD_IS_ARRAY) {
        /* Array field -> bytes */
        if (field->flags & TEP_FIELD_IS_DYNAMIC) {
            ptr = base + *(unsigned short *)(base + field->offset);
            len = *(unsigned short *)(base + field->offset + sizeof(unsigned short));
        } else {
            ptr = base + field->offset;
            len = field->size;
        }
        value = PyBytes_FromStringAndSize((char *)ptr, len);
    } else {
        /* Numeric field */
        bool is_signed = field->flags & TEP_FIELD_IS_SIGNED;
        long long val = 0;

        if (field->size == 1)
            val = is_signed ? *(char *)(base + field->offset)
                            : *(unsigned char *)(base + field->offset);
        else if (field->size == 2)
            val = is_signed ? *(short *)(base + field->offset)
                            : *(unsigned short *)(base + field->offset);
        else if (field->size == 4)
            val = is_signed ? *(int *)(base + field->offset)
                            : *(unsigned int *)(base + field->offset);
        else if (field->size == 8)
            val = is_signed ? *(long long *)(base + field->offset)
                            : *(unsigned long long *)(base + field->offset);

        if (field->size <= 8)
            value = PyLong_FromLongLong(val);
    }

    return value;
}

/* Get all field names (common + event-specific) */
static PyObject *perfevent_get_all_field_names(PerfEventObject *self)
{
    struct python_ctx *ctx = self->dev->private;
    struct python_key_cache *kc = &ctx->key_cache;
    struct python_event_data *ev = self->tp->private;
    PyObject *list;
    int i;

    list = PyList_New(0);
    if (!list)
        return NULL;

    /* Add common field names from key_cache */
#define APPEND_KEY(key) do { \
        if (kc->key && PyList_Append(list, kc->key) < 0) { \
            Py_DECREF(list); \
            return NULL; \
        } \
    } while (0)

    /*
     * Field layout:
     * - Tracepoint events: _pid,_tid,_time,_cpu,_period,common_type,common_flags,
     *                      common_preempt_count,common_pid,_realtime,_callchain,_event + tep fields
     * - Profiler events:   _pid,_tid,_time,_cpu,_realtime,_event + member_cache fields
     */
    APPEND_KEY(key_pid);
    APPEND_KEY(key_tid);
    APPEND_KEY(key_time);
    APPEND_KEY(key_cpu);

    if (tp_is_dev(self->tp)) {
        /* Profiler event: _pid,_tid,_time,_cpu,_realtime,_event + member_cache fields */
        struct prof_dev *source_dev = self->tp->source_dev;
        struct perf_evsel *evsel;
        struct perf_event_member_cache *cache;

        APPEND_KEY(key_realtime);
        APPEND_KEY(key_event);

        /* Add member_cache fields */
        evsel = perf_event_evsel(source_dev, self->event);
        if (evsel) {
            cache = perf_evsel_member_cache(evsel);
            if (cache) {
                for (i = 0; i < cache->nr_members; i++) {
                    PyObject *key = cache->members[i].private;
                    if (key && PyList_Append(list, key) < 0) {
                        Py_DECREF(list);
                        return NULL;
                    }
                }
            }
        }
    } else {
        /* Tracepoint event: full field set */
        APPEND_KEY(key_period);
        APPEND_KEY(key_common_type);
        APPEND_KEY(key_common_flags);
        APPEND_KEY(key_common_preempt_count);
        APPEND_KEY(key_common_pid);
        APPEND_KEY(key_realtime);
        APPEND_KEY(key_callchain);
        APPEND_KEY(key_event);

        /* Add tep event-specific fields */
        for (i = 0; i < ev->nr_fields; i++) {
            if (ev->field_keys[i]) {
                if (PyList_Append(list, ev->field_keys[i]) < 0) {
                    Py_DECREF(list);
                    return NULL;
                }
            }
        }
    }

#undef APPEND_KEY

    return list;
}

/*
 * Initialize interned Python string keys for register names.
 * Called once after Py_Initialize().
 */
static int init_perf_interned_keys(void)
{
    int i;

    perf_reg_key_abi = PyUnicode_InternFromString("abi");
    if (!perf_reg_key_abi)
        return -1;

    for (i = 0; i < PERF_REG_MAX; i++) {
        if (i < (int)PERF_REG_NAMES_SIZE && perf_reg_names[i]) {
            perf_reg_keys[i] = PyUnicode_InternFromString(perf_reg_names[i]);
            if (!perf_reg_keys[i])
                return -1;
        }
    }

    /* Intern PERF_SAMPLE_READ field keys */
    perf_read_key_value = PyUnicode_InternFromString("value");
    perf_read_key_time_enabled = PyUnicode_InternFromString("time_enabled");
    perf_read_key_time_running = PyUnicode_InternFromString("time_running");
    perf_read_key_id = PyUnicode_InternFromString("id");
    perf_read_key_lost = PyUnicode_InternFromString("lost");
    perf_read_key_nr = PyUnicode_InternFromString("nr");
    perf_read_key_cntr = PyUnicode_InternFromString("cntr");
    if (!perf_read_key_value || !perf_read_key_time_enabled ||
        !perf_read_key_time_running || !perf_read_key_id ||
        !perf_read_key_lost || !perf_read_key_nr || !perf_read_key_cntr)
        return -1;

    return 0;
}

static void free_perf_interned_keys(void)
{
    int i;

    Py_XDECREF(perf_reg_key_abi);
    perf_reg_key_abi = NULL;

    for (i = 0; i < PERF_REG_MAX; i++) {
        Py_XDECREF(perf_reg_keys[i]);
        perf_reg_keys[i] = NULL;
    }

    Py_XDECREF(perf_read_key_value);  perf_read_key_value = NULL;
    Py_XDECREF(perf_read_key_time_enabled);  perf_read_key_time_enabled = NULL;
    Py_XDECREF(perf_read_key_time_running);  perf_read_key_time_running = NULL;
    Py_XDECREF(perf_read_key_id);  perf_read_key_id = NULL;
    Py_XDECREF(perf_read_key_lost);  perf_read_key_lost = NULL;
    Py_XDECREF(perf_read_key_nr);  perf_read_key_nr = NULL;
    Py_XDECREF(perf_read_key_cntr);  perf_read_key_cntr = NULL;
}

/*
 * Convert { u64 abi; u64 regs[hweight64(mask)]; } to Python dict {'reg': value}.
 * The mask indicates which registers were sampled. Each set bit corresponds to
 * a register, and the regs array contains values in set-bit order.
 * Uses interned string keys from perf_reg_keys[] for fast dict construction.
 */
static PyObject *perf_regs_to_pydict(void *data, u64 mask)
{
    PyObject *dict;
    PyObject *abi_val;
    u64 abi = *(u64 *)data;
    u64 *regs = (u64 *)(data + sizeof(u64));
    unsigned long m = (unsigned long)mask;
    int bit, idx = 0;

    dict = PyDict_New();
    if (!dict)
        return NULL;

    /* Add abi field using interned key */
    abi_val = PyLong_FromUnsignedLongLong(abi);
    if (abi_val) {
        PyDict_SetItem(dict, perf_reg_key_abi, abi_val);
        Py_DECREF(abi_val);
    }

    for_each_set_bit(bit, &m, PERF_REG_MAX) {
        PyObject *key;
        PyObject *val;

        key = perf_reg_keys[bit];
        val = PyLong_FromUnsignedLongLong(regs[idx]);
        if (val) {
            if (key)
                PyDict_SetItem(dict, key, val);
            else {
                /* Unknown register bit, use "regN" as key */
                char buf[16];
                snprintf(buf, sizeof(buf), "reg%d", bit);
                PyDict_SetItemString(dict, buf, val);
            }
            Py_DECREF(val);
        }
        idx++;
    }

    return dict;
}

/*
 * Decode PERF_SAMPLE_READ data based on attr->read_format into a Python dict.
 *
 * !PERF_FORMAT_GROUP:
 *   { u64 value; u64 time_enabled; u64 time_running; u64 id; u64 lost; }
 *   -> {'value': int, 'time_enabled': int, 'time_running': int, 'id': int, 'lost': int}
 *
 * PERF_FORMAT_GROUP:
 *   { u64 nr; u64 time_enabled; u64 time_running; { u64 value; u64 id; u64 lost; } cntr[nr]; }
 *   -> {'nr': int, 'time_enabled': int, 'time_running': int,
 *       'cntr': [{'value': int, 'id': int, 'lost': int}, ...]}
 */
static inline void dict_set_u64(PyObject *dict, PyObject *key, u64 val)
{
    PyObject *v = PyLong_FromUnsignedLongLong(val);
    PyDict_SetItem(dict, key, v);
    Py_DECREF(v);
}

static PyObject *perf_read_to_pydict(void *data, struct perf_evsel *evsel)
{
    u64 read_format = perf_evsel__attr(evsel)->read_format;
    u64 *ptr = data;
    PyObject *dict;

    dict = PyDict_New();
    if (!dict)
        return NULL;

    if (!(read_format & PERF_FORMAT_GROUP)) {
        /* Non-group: value, [time_enabled], [time_running], [id], [lost] */
        dict_set_u64(dict, perf_read_key_value, *ptr++);

        if (read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
            dict_set_u64(dict, perf_read_key_time_enabled, *ptr++);
        if (read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
            dict_set_u64(dict, perf_read_key_time_running, *ptr++);
        if (read_format & PERF_FORMAT_ID)
            dict_set_u64(dict, perf_read_key_id, *ptr++);
        if (read_format & PERF_FORMAT_LOST)
            dict_set_u64(dict, perf_read_key_lost, *ptr++);
    } else {
        /* Group: nr, [time_enabled], [time_running], cntr[nr] */
        u64 nr = *ptr++;
        u64 i;
        PyObject *cntr_list;

        dict_set_u64(dict, perf_read_key_nr, nr);

        if (read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
            dict_set_u64(dict, perf_read_key_time_enabled, *ptr++);
        if (read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
            dict_set_u64(dict, perf_read_key_time_running, *ptr++);

        cntr_list = PyList_New(nr);
        if (!cntr_list) {
            Py_DECREF(dict);
            return NULL;
        }

        for (i = 0; i < nr; i++) {
            PyObject *entry = PyDict_New();
            if (!entry) {
                Py_DECREF(cntr_list);
                Py_DECREF(dict);
                return NULL;
            }

            dict_set_u64(entry, perf_read_key_value, *ptr++);
            if (read_format & PERF_FORMAT_ID)
                dict_set_u64(entry, perf_read_key_id, *ptr++);
            if (read_format & PERF_FORMAT_LOST)
                dict_set_u64(entry, perf_read_key_lost, *ptr++);

            PyList_SET_ITEM(cntr_list, i, entry); /* steals ref */
        }

        PyDict_SetItem(dict, perf_read_key_cntr, cntr_list);
        Py_DECREF(cntr_list);
    }

    return dict;
}

/*
 * Get profiler event field by name (with caching).
 * For profiler events (tp_is_dev), fields are defined in perf_event_member_cache.
 */
static PyObject *perfevent_get_dev_field(PerfEventObject *self, PyObject *field_name)
{
    struct prof_dev *source_dev = self->tp->source_dev;
    struct perf_evsel *evsel;
    struct perf_event_member_cache *cache;
    struct perf_event_member *member = NULL;
    PyObject *value = NULL;
    int i, offset;
    void *data;

    /* Check cache first */
    value = PyDict_GetItem(self->field_cache, field_name);
    if (value) {
        Py_INCREF(value);
        return value;
    }

    /* Find evsel for the event */
    evsel = perf_event_evsel(source_dev, self->event);
    if (!evsel)
        return NULL;

    cache = perf_evsel_member_cache(evsel);
    if (!cache)
        return NULL;

    /* Find the member by comparing cached Python string keys */
    for (i = 0; i < cache->nr_members; i++) {
        if (cache->members[i].private == field_name) {
            member = &cache->members[i];
            break;
        }
    }

    if (!member)
        return NULL;

    /* Calculate offset in the sample data */
    offset = perf_event_member_offset(cache, member, self->event);
    data = (void *)self->event->sample.array + offset;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
    /* Convert member value to Python object based on format */
    switch (member->format) {
        case PERF_SAMPLE_CALLCHAIN:
            /* Callchain is handled separately via _callchain attribute */
            value = PerfEvent_get_callchain(self, NULL);
            break;
        case PERF_SAMPLE_RAW:
            /* Raw data as bytes */
            {
                u32 size = *(u32 *)data;
                void *raw = data + sizeof(u32);
                value = PyBytes_FromStringAndSize((char *)raw, size);
            }
            break;
        case PERF_SAMPLE_READ:
            /* Read format values -> dict */
            value = perf_read_to_pydict(data, evsel);
            break;
        case PERF_SAMPLE_REGS_USER:
            /* { u64 abi; u64 regs[hweight64(mask)]; } -> dict {'reg': value} */
            {
                struct perf_event_attr *attr = perf_evsel__attr(evsel);
                value = perf_regs_to_pydict(data, attr->sample_regs_user);
            }
            break;
        case PERF_SAMPLE_REGS_INTR:
            /* { u64 abi; u64 regs[hweight64(mask)]; } -> dict {'reg': value} */
            {
                struct perf_event_attr *attr = perf_evsel__attr(evsel);
                value = perf_regs_to_pydict(data, attr->sample_regs_intr);
            }
            break;
        case PERF_SAMPLE_BRANCH_STACK:
            /* { u64 nr; struct perf_branch_entry lbr[nr]; } -> bytes */
            {
                u64 nr = *(u64 *)data;
                u64 total = sizeof(u64) + nr * sizeof(struct perf_branch_entry);
                value = PyBytes_FromStringAndSize((char *)data, total);
            }
            break;
        case PERF_SAMPLE_STACK_USER:
            /* { u64 size; char data[size]; u64 dyn_size; } -> bytes (data part) */
            {
                u64 sz = *(u64 *)data;
                void *stack_data = data + sizeof(u64);
                value = PyBytes_FromStringAndSize((char *)stack_data, sz);
            }
            break;
        case PERF_SAMPLE_AUX:
            /* { u64 size; char data[size]; } -> bytes (data part) */
            {
                u64 sz = *(u64 *)data;
                void *aux_data = data + sizeof(u64);
                value = PyBytes_FromStringAndSize((char *)aux_data, sz);
            }
            break;
        default:
            /* Numeric fields */
            if (member->size == 8) {
                value = PyLong_FromUnsignedLongLong(*(u64 *)data);
            } else if (member->size == 4) {
                value = PyLong_FromLong(*(u32 *)data);
            } else if (member->size == 2) {
                value = PyLong_FromLong(*(u16 *)data);
            } else if (member->size == 1) {
                value = PyLong_FromLong(*(u8 *)data);
            } else {
                /* Unknown size, return as bytes */
                value = PyBytes_FromStringAndSize((char *)data, member->size);
            }
            break;
    }
#pragma GCC diagnostic pop

    if (value) {
        /* Cache the value */
        PyDict_SetItem(self->field_cache, field_name, value);
    }

    return value;
}

/* Get event-specific field by name (with caching) */
static PyObject *perfevent_get_field(PerfEventObject *self, PyObject *field_name)
{
    PyObject *value;
    struct python_event_data *ev;
    void *raw;
    int raw_size;
    int i;

    /* Check cache first */
    value = PyDict_GetItem(self->field_cache, field_name);
    if (value) {
        Py_INCREF(value);
        return value;
    }

    /* For profiler events, use separate path */
    if (tp_is_dev(self->tp)) {
        return perfevent_get_dev_field(self, field_name);
    }

    /* Look up field in cached event data */
    ev = (struct python_event_data *)self->tp->private;
    for (i = 0; i < ev->nr_fields; i++) {
        if (ev->field_keys[i] == field_name)
            break;
    }

    if (i >= ev->nr_fields)
        return NULL;

    /* Parse field value */
    perfevent_get_raw(self, &raw, &raw_size, NULL);
    value = parse_event_field(raw, ev->fields[i]);
    if (value) {
        /* Cache the value using the cached key */
        PyDict_SetItem(self->field_cache, ev->field_keys[i], value);
    }

    return value;
}

/*
 * Insert PerfEventObject into live_events rb tree (sorted by _time, then by pointer)
 * This is called when Python script keeps a reference to the event (refcnt > 1).
 * We copy the event here since the original event buffer will be reused.
 */
static void live_events_insert(struct python_ctx *ctx, PerfEventObject *obj)
{
    struct rb_node **p = &ctx->live_events.rb_node;
    struct rb_node *parent = NULL;
    PerfEventObject *entry;
    size_t event_size;
    union perf_event *event_copy;

    /* Copy event since original buffer will be reused */
    event_size = obj->event->header.size;
    event_copy = malloc(event_size);
    if (!event_copy) {
        /* Memory allocation failed, cannot track this event */
        PyErr_NoMemory();
        return;
    }
    memcpy(event_copy, obj->event, event_size);
    obj->event = event_copy;

    while (*p) {
        parent = *p;
        entry = rb_entry(parent, PerfEventObject, rb_node);

        if (obj->_time < entry->_time)
            p = &parent->rb_left;
        else if (obj->_time > entry->_time)
            p = &parent->rb_right;
        else if (obj < entry)
            p = &parent->rb_left;
        else
            p = &parent->rb_right;
    }

    rb_link_node(&obj->rb_node, parent, p);
    rb_insert_color(&obj->rb_node, &ctx->live_events);
    ctx->nr_live_events++;
}

/*
 * Remove PerfEventObject from live_events rb tree and free the copied event.
 * Objects in the tree own their event copy; objects not in tree have borrowed event.
 */
static void live_events_remove(struct python_ctx *ctx, PerfEventObject *obj)
{
    if (!RB_EMPTY_NODE(&obj->rb_node)) {
        rb_erase(&obj->rb_node, &ctx->live_events);
        RB_CLEAR_NODE(&obj->rb_node);
        ctx->nr_live_events--;

        /* Free the copied event (objects in tree own their event) */
        free(obj->event);
        obj->event = NULL;
    }
}

/*
 * Create a new PerfEventObject (internal use)
 * This is called from python_sample() to create event objects.
 *
 * Initially, event points to the original perf_event buffer (borrowed reference).
 * If the Python script keeps a reference (refcnt > 1 after handler returns),
 * live_events_insert() will copy the event. This avoids copying for most events
 * that are processed and immediately discarded.
 */
static PerfEventObject *PerfEvent_create(struct prof_dev *dev, struct tp *tp,
                                          union perf_event *event, int instance)
{
    PerfEventObject *self;
    struct python_sample_type *data;

    self = PyObject_New(PerfEventObject, &PerfEventType);
    if (!self)
        return NULL;

    /* Store borrowed references */
    self->dev = dev;
    self->tp = tp;

    /* Initially use borrowed reference to event (no copy) */
    self->event = event;

    /* Extract header fields */
    data = (void *)self->event->sample.array;
    self->_pid = data->tid_entry.pid;
    self->_tid = data->tid_entry.tid;
    self->_time = data->time;
    self->_cpu = data->cpu_entry.cpu;
    self->instance = instance;
    self->_period = data->period;

    /* Initialize lazy computed fields to NULL */
    self->_realtime = NULL;
    self->_callchain_list = NULL;

    /* Initialize field cache */
    self->field_cache = PyDict_New();
    if (!self->field_cache) {
        Py_DECREF(self);
        return NULL;
    }

    /* Initialize rb_node for minevtime tracking (not inserted yet) */
    RB_CLEAR_NODE(&self->rb_node);

    return self;
}

/*
 * Create a PerfEventObject from a forwarded profiler event (PERF_RECORD_DEV).
 *
 * For profiler events, the data format is defined by perf_event_member_cache
 * rather than tep event fields. The header fields (pid, tid, time, cpu) come
 * from perf_record_dev.
 *
 * self->event stores the inner event (&event_dev->event), not the wrapper.
 * The source device can be accessed via tp->source_dev.
 *
 * Profiler events have: _pid, _tid, _time, _cpu, _realtime, _event +
 * member_cache fields
 *
 * @dev: The python device receiving the event
 * @tp: The tp representing the profiler source (tp_is_dev(tp) == true)
 * @event: The PERF_RECORD_DEV wrapper event
 */
static PerfEventObject *PerfEvent_create_from_dev(struct prof_dev *dev, struct tp *tp,
                                                   union perf_event *event)
{
    PerfEventObject *self;
    struct perf_record_dev *event_dev = (void *)event;

    self = PyObject_New(PerfEventObject, &PerfEventType);
    if (!self)
        return NULL;

    /* Store borrowed references */
    self->dev = dev;
    self->tp = tp;

    /*
     * Store the inner event, not the PERF_RECORD_DEV wrapper.
     * The source device can be accessed via tp->source_dev.
     */
    self->event = event_dev->event;

    /*
     * Extract header fields from perf_record_dev.
     * These were pre-extracted in perf_event_forward() from the inner event.
     * Profiler events only have: _pid, _tid, _time, _cpu, _realtime, _event
     */
    self->_pid = event_dev->pid;
    self->_tid = event_dev->tid;
    self->_time = event_dev->time;
    self->_cpu = event_dev->cpu;
    self->instance = event_dev->instance;
    self->_period = 0;  /* Not used for profiler events */

    /* Initialize lazy computed fields to NULL */
    self->_realtime = NULL;
    self->_callchain_list = NULL; /* Not used for profiler events */

    /* Initialize field cache */
    self->field_cache = PyDict_New();
    if (!self->field_cache) {
        Py_DECREF(self);
        return NULL;
    }

    /* Initialize rb_node for minevtime tracking (not inserted yet) */
    RB_CLEAR_NODE(&self->rb_node);

    return self;
}

/*
 * PerfEvent_new - Allocator (not used for internal creation)
 */
static PyObject *PerfEvent_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyErr_SetString(PyExc_TypeError, "PerfEvent objects cannot be created directly");
    return NULL;
}

/*
 * PerfEvent_init - Initializer (not used for internal creation)
 */
static int PerfEvent_init(PerfEventObject *self, PyObject *args, PyObject *kwds)
{
    return 0;
}

/*
 * PerfEvent_dealloc - Destructor
 *
 * Event ownership: live_events_remove() handles freeing the copied event
 * if the object was in the live_events tree. For objects that were never
 * inserted (owns_event=0), the event pointer is borrowed and must not be freed.
 */
static void PerfEvent_dealloc(PerfEventObject *self)
{
    struct python_ctx *ctx = self->dev->private;

    /* Remove from live_events rb tree (also frees owned event if any) */
    live_events_remove(ctx, self);

    /* Release cached Python objects */
    Py_XDECREF(self->_realtime);
    Py_XDECREF(self->_callchain_list);
    Py_XDECREF(self->field_cache);

    /* Free the object */
    Py_TYPE(self)->tp_free((PyObject *)self);
}

/*
 * Getter for _realtime field (lazy computed)
 */
static PyObject *PerfEvent_get_realtime(PerfEventObject *self, void *closure)
{
    if (!self->_realtime) {
        struct prof_dev *dev = tp_is_dev(self->tp) ? self->tp->source_dev : self->dev;
        u64 realtime_ns = evclock_to_realtime_ns(dev, (evclock_t)(u64)self->_time);
        self->_realtime = PyLong_FromUnsignedLongLong(realtime_ns);
    }
    Py_XINCREF(self->_realtime);
    return self->_realtime;
}

/*
 * Getter for _callchain field (lazy computed)
 */
static PyObject *PerfEvent_get_callchain(PerfEventObject *self, void *closure)
{
    struct python_ctx *ctx;
    struct callchain *callchain = NULL;
    int callchain_flags = 0;
    struct callchain_data cd;

    if (!self->_callchain_list) {
        ctx = (struct python_ctx *)self->dev->private;

        if (tp_is_dev(self->tp)) {
            /* Profiler event: get callchain from member_cache */
            struct prof_dev *source_dev = self->tp->source_dev;
            struct perf_evsel *evsel;
            struct perf_event_member_cache *cache;

            evsel = perf_event_evsel(source_dev, self->event);
            if (evsel) {
                cache = perf_evsel_member_cache(evsel);
                if (cache && cache->callchain) {
                    perf_event_build_callchain_data(evsel, self->event, &cd);
                    self->_callchain_list = callchain_data_to_pylist(&cd,
                                            CALLCHAIN_KERNEL | CALLCHAIN_USER);
                }
            }
        } else {
            /* Tracepoint event: get callchain from raw data */
            void *raw;
            int raw_size;
            perfevent_get_raw(self, &raw, &raw_size, &callchain);
            callchain_flags = ctx->callchain_flags;

            if (callchain && callchain_flags) {
                if (self->tp->dwarf_unwind) {
                    perf_event_build_callchain_data(self->tp->evsel, self->event, &cd);
                    self->_callchain_list = callchain_data_to_pylist(&cd, callchain_flags);
                } else
                    self->_callchain_list = callchain_to_pylist(callchain, self->_pid,
                                                                callchain_flags);
            }
        }

        if (!self->_callchain_list) {
            self->_callchain_list = PyList_New(0);  /* Empty list if no callchain */
        }
    }
    Py_XINCREF(self->_callchain_list);
    return self->_callchain_list;
}

/*
 * Getter for _event field (lazy computed, cached in python_event_data)
 */
static PyObject *PerfEvent_get_event(PerfEventObject *self, void *closure)
{
    struct python_event_data *ev = (struct python_event_data *)self->tp->private;
    char event_name[256];

    if (ev && ev->event_name) {
        Py_INCREF(ev->event_name);
        return ev->event_name;
    }

    /* Fallback: compute event name */
    snprintf(event_name, sizeof(event_name), "%s:%s", self->tp->sys,
             self->tp->alias ? self->tp->alias : self->tp->name);
    return PyUnicode_InternFromString(event_name);
}

/*
 * PerfEvent_getattro - Custom attribute access
 * Handles lazy parsing of event-specific fields
 */
static PyObject *PerfEvent_getattro(PerfEventObject *self, PyObject *name)
{
    struct python_ctx *ctx = self->dev->private;
    struct python_key_cache *kc = &ctx->key_cache;
    PyObject *result;
    const char *attr_name;
    int is_dev = tp_is_dev(self->tp);

    /*
     * Fast path: builtin fields with pointer comparison
     * Python interns attribute names, so pointer comparison works
     *
     * Field layout:
     * - Tracepoint: _pid,_tid,_time,_cpu,_period,common_type,common_flags,
     *               common_preempt_count,common_pid,_realtime,_callchain,_event + tep fields
     * - Profiler:   _pid,_tid,_time,_cpu,_realtime,_event + member_cache fields
     */

    /* Common fields for both event types */
    if (name == kc->key_time)
        return PyLong_FromUnsignedLongLong(self->_time);
    if (name == kc->key_cpu)
        return PyLong_FromLong(self->_cpu);
    if (name == kc->key_pid)
        return PyLong_FromLong(self->_pid);
    if (name == kc->key_tid)
        return PyLong_FromLong(self->_tid);

    /* Try event-specific field lookup */
    result = perfevent_get_field(self, name);
    if (result)
        return result;

    /* Lazy computed fields (common to both) */
    if (name == kc->key_realtime)
        return PerfEvent_get_realtime(self, NULL);
    if (name == kc->key_event)
        return PerfEvent_get_event(self, NULL);

    /* Tracepoint-only fields */
    if (!is_dev) {
        struct trace_entry *entry;
        int raw_size;

        if (name == kc->key_callchain)
            return PerfEvent_get_callchain(self, NULL);
        if (name == kc->key_period)
            return PyLong_FromUnsignedLongLong(self->_period);

        if (name == kc->key_common_type) {
            perfevent_get_raw(self, (void *)&entry, &raw_size, NULL);
            return PyLong_FromLong(entry->common_type);
        }
        if (name == kc->key_common_flags) {
            perfevent_get_raw(self, (void *)&entry, &raw_size, NULL);
            return PyLong_FromLong(entry->common_flags);
        }
        if (name == kc->key_common_preempt_count) {
            perfevent_get_raw(self, (void *)&entry, &raw_size, NULL);
            return PyLong_FromLong(entry->common_preempt_count);
        }
        if (name == kc->key_common_pid) {
            perfevent_get_raw(self, (void *)&entry, &raw_size, NULL);
            return PyLong_FromLong(entry->common_pid);
        }
    }

    /* Slow path: try the generic attribute lookup (handles members, methods, getset) */
    result = PyObject_GenericGetAttr((PyObject *)self, name);
    if (result || !PyErr_ExceptionMatches(PyExc_AttributeError))
        return result;

    PyErr_Clear();

    attr_name = PyUnicode_AsUTF8(name);
    PyErr_Format(PyExc_AttributeError,
                 "'PerfEvent' object has no attribute '%s'", attr_name ? : "");
    return NULL;
}

/*
 * PerfEvent_length - Number of fields
 */
static Py_ssize_t PerfEvent_length(PerfEventObject *self)
{
    struct python_event_data *ev;
    Py_ssize_t count;

    if (tp_is_dev(self->tp)) {
        /* Profiler event: count fields from member_cache */
        struct prof_dev *source_dev = self->tp->source_dev;
        struct perf_evsel *evsel;
        struct perf_event_member_cache *cache;

        /* Common fields for profiler events: _pid, _tid, _time, _cpu, _realtime, _event = 6 */
        count = 6;

        evsel = perf_event_evsel(source_dev, self->event);
        if (evsel) {
            cache = perf_evsel_member_cache(evsel);
            if (cache)
                count += cache->nr_members;
        }
    } else {
        /* Tracepoint event: common fields count: _pid, _tid, _time, _cpu, _period,
           common_type, common_flags, common_preempt_count, common_pid,
           _realtime, _callchain, _event = 12 */
        count = 12;

        ev = (struct python_event_data *)self->tp->private;
        if (ev)
            count += ev->nr_fields;
    }

    return count;
}

/*
 * PerfEvent_subscript - event['field_name'] access
 */
static PyObject *PerfEvent_subscript(PerfEventObject *self, PyObject *key)
{
    if (!PyUnicode_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "field name must be a string");
        return NULL;
    }

    /* Try attribute access (handles both common and event-specific fields) */
    return PyObject_GetAttr((PyObject *)self, key);
}

/*
 * PerfEvent_contains - 'field_name' in event
 */
static int PerfEvent_contains(PerfEventObject *self, PyObject *key)
{
    const char *field_name;
    struct python_event_data *ev;
    int i;

    if (!PyUnicode_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "field name must be a string");
        return -1;
    }

    field_name = PyUnicode_AsUTF8(key);
    if (!field_name)
        return -1;

    /* Check common fields */
    if (is_common_field(field_name, tp_is_dev(self->tp)))
        return 1;

    if (tp_is_dev(self->tp)) {
        /* Profiler event: check fields in member_cache */
        struct prof_dev *source_dev = self->tp->source_dev;
        struct perf_evsel *evsel;
        struct perf_event_member_cache *cache;

        evsel = perf_event_evsel(source_dev, self->event);
        if (evsel) {
            cache = perf_evsel_member_cache(evsel);
            if (cache) {
                for (i = 0; i < cache->nr_members; i++) {
                    if (strcmp(cache->members[i].name, field_name) == 0)
                        return 1;
                }
            }
        }
    } else {
        /* Tracepoint event: check event-specific fields using cached fields */
        ev = (struct python_event_data *)self->tp->private;
        if (ev && ev->fields) {
            for (i = 0; i < ev->nr_fields; i++) {
                if (strcmp(ev->fields[i]->name, field_name) == 0)
                    return 1;
            }
        }
    }

    return 0;
}

/*
 * PerfEvent_repr - repr(event)
 */
static PyObject *PerfEvent_repr(PerfEventObject *self)
{
    if (tp_is_dev(self->tp)) {
        /* Profiler event: format as "<PerfEvent profiler_name cpu=X pid=X time=X>" */
        return PyUnicode_FromFormat("<PerfEvent %s cpu=%d pid=%d time=%llu>",
                                     self->tp->alias ? self->tp->alias : self->tp->name,
                                     self->_cpu, self->_pid, self->_time);
    } else {
        /* Tracepoint event: format as "<PerfEvent sys:name cpu=X pid=X time=X>" */
        return PyUnicode_FromFormat("<PerfEvent %s:%s cpu=%d pid=%d time=%llu>",
                                     self->tp->sys,
                                     self->tp->alias ? self->tp->alias : self->tp->name,
                                     self->_cpu, self->_pid, self->_time);
    }
}

/*
 * PerfEvent_hash - hash(event)
 * Compute hash of entire union perf_event
 */
static Py_hash_t PerfEvent_hash(PerfEventObject *self)
{
    Py_hash_t hash;
    size_t event_size = self->event->header.size;
    unsigned char *data = (unsigned char *)self->event;
    size_t i;

    /* FNV-1a hash algorithm */
    hash = 2166136261U;
    for (i = 0; i < event_size; i++) {
        hash ^= data[i];
        hash *= 16777619U;
    }

    if (hash == -1)
        hash = -2;  /* -1 is reserved for errors */

    return hash;
}

/* Iterator dealloc */
static void PerfEventIter_dealloc(PerfEventIterObject *self)
{
    Py_XDECREF(self->event);
    Py_XDECREF(self->keys);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

/* Iterator next */
static PyObject *PerfEventIter_next(PerfEventIterObject *self)
{
    PyObject *key, *value, *result;

    if (self->index >= PyList_Size(self->keys))
        return NULL;  /* StopIteration */

    key = PyList_GetItem(self->keys, self->index++);  /* Borrowed reference */
    if (!key)
        return NULL;

    value = PerfEvent_getattro(self->event, key);
    if (!value)
        return NULL;

    result = PyTuple_Pack(2, key, value);
    Py_DECREF(value);
    return result;
}

/*
 * PerfEvent_iter - iter(event)
 */
static PyObject *PerfEvent_iter(PerfEventObject *self)
{
    PerfEventIterObject *iter;

    iter = PyObject_New(PerfEventIterObject, &PerfEventIterType);
    if (!iter)
        return NULL;

    Py_INCREF(self);
    iter->event = self;
    iter->keys = perfevent_get_all_field_names(self);
    if (!iter->keys) {
        Py_DECREF(iter);
        return NULL;
    }
    iter->index = 0;

    return (PyObject *)iter;
}

/*
 * PerfEvent_get - event.get(field, default=None)
 * Get field value, return default if field not found
 */
static PyObject *PerfEvent_get(PerfEventObject *self, PyObject *args)
{
    PyObject *field_name;
    PyObject *default_value = Py_None;
    PyObject *result;

    if (!PyArg_ParseTuple(args, "O|O", &field_name, &default_value))
        return NULL;

    if (!PyUnicode_Check(field_name)) {
        PyErr_SetString(PyExc_TypeError, "field name must be a string");
        return NULL;
    }

    /* Try to get the attribute */
    result = PerfEvent_getattro(self, field_name);
    if (result)
        return result;

    /* If AttributeError, return default value */
    if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
        Py_INCREF(default_value);
        return default_value;
    }

    /* Other errors propagate */
    return NULL;
}

/*
 * PerfEvent_keys - event.keys()
 */
static PyObject *PerfEvent_keys(PerfEventObject *self, PyObject *args)
{
    return perfevent_get_all_field_names(self);
}

/*
 * PerfEvent_values - event.values()
 */
static PyObject *PerfEvent_values(PerfEventObject *self, PyObject *args)
{
    PyObject *keys, *values, *key, *value;
    Py_ssize_t i, n;

    keys = perfevent_get_all_field_names(self);
    if (!keys)
        return NULL;

    n = PyList_Size(keys);
    values = PyList_New(n);
    if (!values) {
        Py_DECREF(keys);
        return NULL;
    }

    for (i = 0; i < n; i++) {
        key = PyList_GetItem(keys, i);  /* Borrowed reference */
        value = PerfEvent_getattro(self, key);
        if (!value) {
            Py_DECREF(keys);
            Py_DECREF(values);
            return NULL;
        }
        PyList_SET_ITEM(values, i, value);  /* Steals reference */
    }

    Py_DECREF(keys);
    return values;
}

/*
 * PerfEvent_items - event.items()
 */
static PyObject *PerfEvent_items(PerfEventObject *self, PyObject *args)
{
    PyObject *keys, *items, *key, *value, *tuple;
    Py_ssize_t i, n;

    keys = perfevent_get_all_field_names(self);
    if (!keys)
        return NULL;

    n = PyList_Size(keys);
    items = PyList_New(n);
    if (!items) {
        Py_DECREF(keys);
        return NULL;
    }

    for (i = 0; i < n; i++) {
        key = PyList_GetItem(keys, i);  /* Borrowed reference */
        value = PerfEvent_getattro(self, key);
        if (!value) {
            Py_DECREF(keys);
            Py_DECREF(items);
            return NULL;
        }
        tuple = PyTuple_Pack(2, key, value);
        Py_DECREF(value);
        if (!tuple) {
            Py_DECREF(keys);
            Py_DECREF(items);
            return NULL;
        }
        PyList_SET_ITEM(items, i, tuple);  /* Steals reference */
    }

    Py_DECREF(keys);
    return items;
}

/*
 * PerfEvent_to_dict - event.to_dict()
 */
static PyObject *PerfEvent_to_dict(PerfEventObject *self, PyObject *args)
{
    PyObject *dict, *keys, *key, *value;
    Py_ssize_t i, n;

    dict = PyDict_New();
    if (!dict)
        return NULL;

    keys = perfevent_get_all_field_names(self);
    if (!keys) {
        Py_DECREF(dict);
        return NULL;
    }

    n = PyList_Size(keys);
    for (i = 0; i < n; i++) {
        key = PyList_GetItem(keys, i);  /* Borrowed reference */
        value = PerfEvent_getattro(self, key);
        if (!value) {
            Py_DECREF(keys);
            Py_DECREF(dict);
            return NULL;
        }
        if (PyDict_SetItem(dict, key, value) < 0) {
            Py_DECREF(value);
            Py_DECREF(keys);
            Py_DECREF(dict);
            return NULL;
        }
        Py_DECREF(value);
    }

    Py_DECREF(keys);
    return dict;
}

/*
 * PerfEvent_print - event.print(timestamp=True, callchain=True)
 */
static PyObject *PerfEvent_print(PerfEventObject *self, PyObject *args, PyObject *kwargs)
{
    int print_timestamp = 1;
    int print_callchain = 1;
    static const char *kwlist[] = {"timestamp", "callchain", NULL};
    struct python_ctx *ctx;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|pp", (void *)kwlist,
                                     &print_timestamp, &print_callchain))
        return NULL;

    if (tp_is_dev(self->tp)) {
        /*
         * For profiler events (dev_tp), use prof_dev_print_event() which
         * calls the source device's print_event method. The inner event
         * is passed to the source profiler for printing.
         */
        struct prof_dev *source_dev = self->tp->source_dev;
        int flags = 0;

        if (!print_timestamp)
            flags |= OMIT_TIMESTAMP;
        if (!print_callchain)
            flags |= OMIT_CALLCHAIN;

        prof_dev_print_event(source_dev, self->event, self->instance, flags);
    } else {
        void *raw;
        int raw_size;
        struct callchain *callchain;

        /* Print timestamp if requested */
        if (print_timestamp)
            prof_dev_print_time(self->dev, self->_time, stdout);

        /* Print event */
        perfevent_get_raw(self, &raw, &raw_size, &callchain);
        tp_print_event(self->tp, self->_time, self->_cpu, raw, raw_size);

        /* Print callchain if requested */
        if (print_callchain && callchain) {
            ctx = (struct python_ctx *)self->dev->private;
            if (self->tp->dwarf_unwind) {
                struct callchain_data cd;
                perf_event_build_callchain_data(self->tp->evsel, self->event, &cd);
                print_callchain_data(ctx->cc, &cd);
            } else if (callchain->nr > 0)
                print_callchain_common(ctx->cc, callchain, self->_pid);
        }
    }

    Py_RETURN_NONE;
}

/*
 * PerfEvent_str - str(event)
 * Returns a user-friendly string representation
 */
static PyObject *PerfEvent_str(PerfEventObject *self)
{
    PyObject *dict, *str;

    /* Convert to dict and use dict's str representation */
    dict = PerfEvent_to_dict(self, NULL);
    if (!dict)
        return NULL;

    str = PyObject_Str(dict);
    Py_DECREF(dict);
    return str;
}


/* Member definitions for direct access fields */
/*
 * Python C API compatibility:
 * - Python < 3.7: PyMemberDef/PyGetSetDef use char* (non-const)
 * - Python >= 3.7: these structures use const char*
 */
#if PY_VERSION_HEX < 0x03070000
#define S (char *)
#else
#define S
#endif

static PyMemberDef PerfEvent_members[] = {
    {S"_pid", T_INT, offsetof(PerfEventObject, _pid), READONLY, S"Process ID"},
    {S"_tid", T_INT, offsetof(PerfEventObject, _tid), READONLY, S"Thread ID"},
    {S"_time", T_ULONGLONG, offsetof(PerfEventObject, _time), READONLY, S"Event timestamp (ns)"},
    {S"_cpu", T_INT, offsetof(PerfEventObject, _cpu), READONLY, S"CPU number"},
    {NULL}
};

/* Getter/setter definitions for lazy computed fields */
static PyGetSetDef PerfEvent_getsetters[] = {
    {S"_realtime", (getter)PerfEvent_get_realtime, NULL, S"Wall clock time (ns since Unix epoch)", NULL},
    {S"_event", (getter)PerfEvent_get_event, NULL, S"Event name (sys:name or sys:alias)", NULL},
    {NULL}
};

/* Method definitions */
static PyMethodDef PerfEvent_methods[] = {
    {"get", (PyCFunction)PerfEvent_get, METH_VARARGS, "Get field value with default"},
    {"keys", (PyCFunction)PerfEvent_keys, METH_NOARGS, "Return list of all field names"},
    {"values", (PyCFunction)PerfEvent_values, METH_NOARGS, "Return list of all field values"},
    {"items", (PyCFunction)PerfEvent_items, METH_NOARGS, "Return list of (field_name, value) tuples"},
    {"print", (PyCFunction)PerfEvent_print, METH_VARARGS | METH_KEYWORDS,
     "Print event in standard perf-prof format"},
    {"to_dict", (PyCFunction)PerfEvent_to_dict, METH_NOARGS, "Convert event to dict"},
    {NULL}
};

#undef S

/* Mapping protocol */
static PyMappingMethods PerfEvent_as_mapping = {
    .mp_length = (lenfunc)PerfEvent_length,
    .mp_subscript = (binaryfunc)PerfEvent_subscript,
};

/* Sequence protocol (for 'in' operator) */
static PySequenceMethods PerfEvent_as_sequence = {
    .sq_contains = (objobjproc)PerfEvent_contains,
};

/* Iterator type definition */
static PyTypeObject PerfEventIterType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "perf_prof.PerfEventIterator",
    .tp_basicsize = sizeof(PerfEventIterObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_dealloc = (destructor)PerfEventIter_dealloc,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)PerfEventIter_next,
};

/* PerfEvent type definition */
/* Detailed docstring for PerfEvent type */
static const char PerfEvent_doc[] =
    "PerfEvent - Perf event object with lazy field evaluation.\n"
    "\n"
    "Fields (direct access):\n"
    "    _pid              Process ID (int)\n"
    "    _tid              Thread ID (int)\n"
    "    _time             Event timestamp in nanoseconds (int)\n"
    "    _cpu              CPU number (int)\n"
    "    _period           Sample period (int)\n"
    "    common_flags      Trace entry common flags (int)\n"
    "    common_preempt_count  Trace entry preempt count (int)\n"
    "    common_pid        Trace entry common_pid (int)\n"
    "\n"
    "Fields (lazy computed):\n"
    "    _realtime         Wall clock time in ns since Unix epoch (int)\n"
    "                      Note: Has drift, for display only, not latency calc\n"
    "    _callchain        Call stack list (when -g or stack attribute is set)\n"
    "                      Each frame: {'addr', 'symbol', 'offset', 'kernel', 'dso'}\n"
    "    _event            Event name (sys:name or sys:alias, only in __sample__)\n"
    "    <field>           Event-specific fields (int/str/bytes)\n"
    "\n"
    "Access methods:\n"
    "    event.field or event['field']  - Access field value\n"
    "    event.get(field, default=None) - Get field with default fallback\n"
    "    'field' in event               - Check if field exists\n"
    "    len(event)                     - Number of fields\n"
    "    event.keys()                   - List of all field names\n"
    "    event.values()                 - List of all field values\n"
    "    event.items()                  - List of (name, value) tuples\n"
    "    for name, value in event       - Iterate over fields\n"
    "    event.print(timestamp=True, callchain=True)\n"
    "                                   - Print in perf-prof format\n"
    "    event.to_dict()                - Convert to regular dict\n"
    "    str(event), repr(event)        - String representations\n"
    "    hash(event)                    - Hash of entire perf event\n";

static PyTypeObject PerfEventType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "perf_prof.PerfEvent",
    .tp_doc = PerfEvent_doc,
    .tp_basicsize = sizeof(PerfEventObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PerfEvent_new,
    .tp_init = (initproc)PerfEvent_init,
    .tp_dealloc = (destructor)PerfEvent_dealloc,
    .tp_repr = (reprfunc)PerfEvent_repr,
    .tp_str = (reprfunc)PerfEvent_str,
    .tp_hash = (hashfunc)PerfEvent_hash,
    .tp_getattro = (getattrofunc)PerfEvent_getattro,
    .tp_members = PerfEvent_members,
    .tp_getset = PerfEvent_getsetters,
    .tp_methods = PerfEvent_methods,
    .tp_as_mapping = &PerfEvent_as_mapping,
    .tp_as_sequence = &PerfEvent_as_sequence,
    .tp_iter = (getiterfunc)PerfEvent_iter,
};

/* ============================================================================
 * End of PerfEvent Type
 * ============================================================================ */

/*
 * ============================================================================
 * perf_prof built-in module
 *
 * Provides:
 *   - PerfEvent type: Lazy-evaluated event object with fields and methods
 *
 * The perf_prof module exposes the PerfEvent and PerfEventIter types,
 * which are used to represent perf events passed to Python script handlers.
 * ============================================================================
 */

/* perf_prof module definition */
static struct PyModuleDef perf_prof_module = {
    PyModuleDef_HEAD_INIT,
    "perf_prof",                              /* module name */
    "perf-prof built-in module for event processing utilities",  /* docstring */
    -1,                                       /* size of per-interpreter state, -1 = global */
    NULL
};

/* Module initialization function */
static PyObject *PyInit_perf_prof(void)
{
    PyObject *m;

    /* Prepare PerfEvent type */
    if (PyType_Ready(&PerfEventType) < 0)
        return NULL;

    /* Prepare PerfEventIterator type */
    if (PyType_Ready(&PerfEventIterType) < 0)
        return NULL;

    m = PyModule_Create(&perf_prof_module);
    if (!m)
        return NULL;

    /* Add PerfEvent type to module */
    Py_INCREF(&PerfEventType);
    if (PyModule_AddObject(m, "PerfEvent", (PyObject *)&PerfEventType) < 0) {
        Py_DECREF(&PerfEventType);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}

/*
 * Register perf_prof module as a built-in module.
 * Must be called before Py_Initialize().
 */
static int register_perf_prof_module(void)
{
    return PyImport_AppendInittab("perf_prof", PyInit_perf_prof);
}

/* ============================================================================
 * End of perf_prof built-in module
 * ============================================================================ */


/*
 * Get Python callable from module, returns NULL if not found (not an error)
 * Supports:
 *   - PyFunction (pure Python functions)
 *   - PyCFunction (C extension functions, including Cython)
 *   - Other user-defined callables
 * Excludes:
 *   - Module's built-in method wrappers (e.g., module.__init__, also callable)
 *   - Non-callable attributes
 *
 * Why exclude "method-wrapper":
 *   PyModule_Type defines tp_init slot (module___init__). PyType_Ready() creates
 *   a wrapper descriptor via PyDescr_NewWrapper(). When accessing module.__init__,
 *   if no user-defined __init__ exists in module's __dict__, PyObject_GenericGetAttr()
 *   falls back to the descriptor's tp_descr_get, which calls PyWrapper_New() and
 *   returns a "method-wrapper" object. Since PyDescr_IsData() returns false for
 *   wrapper descriptors, user-defined functions in __dict__ take precedence.
 *
 *   Other callbacks (__exit__, __sample__, etc.) have no corresponding slots in
 *   PyModule_Type (see CPython's slotdefs[]), so they either exist in __dict__
 *   or are not found at all - no "method-wrapper" issue for them.
 */
static PyObject *get_python_func(PyObject *module, const char *name)
{
    PyObject *func = PyObject_GetAttrString(module, name);
    if (func) {
        /* Must be callable */
        if (!PyCallable_Check(func)) {
            Py_DECREF(func);
            func = NULL;
        } else {
            /*
             * Reject method-wrapper objects (e.g., module.__init__ when no
             * user-defined __init__ exists). This is a wrapper around
             * PyModule_Type.tp_init, not a user-defined function.
             */
            const char *type_name = Py_TYPE(func)->tp_name;
            if (type_name && strcmp(type_name, "method-wrapper") == 0) {
                Py_DECREF(func);
                func = NULL;
            }
        }
    }
    if (!func)
        PyErr_Clear();
    return func;
}

/*
 * Build event handler function name: sys__event_name or sys__alias
 * e.g., "sched:sched_wakeup" -> "sched__sched_wakeup"
 * e.g., "sched:sched-wakeup" -> "sched__sched_wakeup"
 * e.g., "sched:sched_wakeup" with alias="wakeup1" -> "sched__wakeup1"
 * Convert invalid characters (like '-') to '_' for valid Python function names.
 *
 * When alias is provided, use sys__alias to allow distinguishing multiple
 * instances of the same event with different aliases.
 */
static char *build_handler_name(const char *sys, const char *name, const char *alias)
{
    char *handler_name;
    const char *event_part = alias ? alias : name;
    size_t len = (sys ? strlen(sys) + 2 : 0) + strlen(event_part) + 1;
    char *p;

    handler_name = malloc(len);
    if (handler_name) {
        if (sys)
            snprintf(handler_name, len, "%s__%s", sys, event_part);
        else
            snprintf(handler_name, len, "%s", event_part);
        /* Convert invalid characters to underscore */
        for (p = handler_name; *p; p++) {
            if (*p == '-' || *p == '.' || *p == ':')
                *p = '_';
        }
    }
    return handler_name;
}

/*
 * Initialize common key cache with interned strings.
 * Interned strings are guaranteed unique and allow pointer comparison.
 */
static int init_key_cache(struct python_key_cache *kc)
{
#define INTERN_KEY(field, str) do { \
        kc->field = PyUnicode_InternFromString(str); \
        if (!kc->field) return -1; \
    } while (0)

    /* Common sample fields */
    INTERN_KEY(key_pid, "_pid");
    INTERN_KEY(key_tid, "_tid");
    INTERN_KEY(key_time, "_time");
    INTERN_KEY(key_cpu, "_cpu");
    INTERN_KEY(key_period, "_period");
    /* Common trace_entry fields */
    INTERN_KEY(key_common_type, "common_type");
    INTERN_KEY(key_common_flags, "common_flags");
    INTERN_KEY(key_common_preempt_count, "common_preempt_count");
    INTERN_KEY(key_common_pid, "common_pid");
    /* Lazy computed fields */
    INTERN_KEY(key_realtime, "_realtime");
    INTERN_KEY(key_callchain, "_callchain");
    INTERN_KEY(key_event, "_event");

#undef INTERN_KEY
    return 0;
}

static void free_key_cache(struct python_key_cache *kc)
{
    Py_XDECREF(kc->key_pid);
    Py_XDECREF(kc->key_tid);
    Py_XDECREF(kc->key_time);
    Py_XDECREF(kc->key_cpu);
    Py_XDECREF(kc->key_period);
    Py_XDECREF(kc->key_common_type);
    Py_XDECREF(kc->key_common_flags);
    Py_XDECREF(kc->key_common_preempt_count);
    Py_XDECREF(kc->key_common_pid);
    Py_XDECREF(kc->key_realtime);
    Py_XDECREF(kc->key_callchain);
    Py_XDECREF(kc->key_event);
    memset(kc, 0, sizeof(*kc));
}

/*
 * Cache event fields for faster lookup during sampling.
 *
 * For tracepoint events (real_tp):
 *   After tep__ref(), tep_find_event() returns pointers that remain valid.
 *   Clears TEP_FIELD_IS_STRING flag for fields that require special pointer
 *   format output (e.g., %pI4, %pM), as they should be treated as binary data.
 *
 * For profiler event sources (dev_tp):
 *   Cache Python string keys in member_cache->members[].private for fast
 *   field lookup during PerfEvent_getattro().
 */
static int cache_event_fields(struct python_ctx *ctx)
{
    struct tep_handle *tep;
    struct tp *tp;
    int i, j, ret = -1;

    /* Initialize common key cache */
    if (init_key_cache(&ctx->key_cache) < 0)
        return -1;

    ctx->nr_events = ctx->tp_list->nr_tp;
    ctx->events = calloc(ctx->nr_events, sizeof(struct python_event_data));

    if (!ctx->events)
        return -1;

    tep = tep__ref();

    for_each_real_tp(ctx->tp_list, tp, i) {
        struct tep_event *event = tep_find_event(tep, tp->id);
        struct python_event_data *ev = &ctx->events[i];
        char *handler_name;
        char event_name[256];

        tp->private = ev; /* Link tp to its python_event_data */

        if (event) {
            ev->fields = tep_event_fields(event);
            if (!ev->fields)
                goto failed;

            /* Count fields and allocate key cache */
            for (j = 0; ev->fields[j]; j++) ;
            ev->nr_fields = j;

            ev->field_keys = calloc(ev->nr_fields, sizeof(PyObject *));
            if (!ev->field_keys)
                goto failed;

            /* Cache field keys and fix IS_STRING flag */
            for (j = 0; j < ev->nr_fields; j++) {
                struct tep_format_field *field = ev->fields[j];

                /* Clear IS_STRING flag for fields requiring special pointer format.
                 * These fields (e.g., IP addresses, MAC addresses, UUIDs) have IS_STRING
                 * flag but their data should be treated as binary (bytes in Python). */
                if ((field->flags & TEP_FIELD_IS_STRING) &&
                    tep__string_field_as_binary(event, field)) {
                    field->flags &= ~TEP_FIELD_IS_STRING;
                }

                /* Cache interned string for field name */
                ev->field_keys[j] = PyUnicode_InternFromString(field->name);
                if (!ev->field_keys[j])
                    goto failed;
            }
        }

        /* Cache event name string for __sample__ handler */
        snprintf(event_name, sizeof(event_name), "%s:%s", tp->sys,
                    tp->alias ? tp->alias : tp->name);
        ev->event_name = PyUnicode_InternFromString(event_name);
        if (!ev->event_name)
            goto failed;

        /* Look for event-specific handler */
        handler_name = build_handler_name(tp->sys, tp->name, tp->alias);
        if (handler_name) {
            ev->handler = get_python_func(ctx->module, handler_name);
            free(handler_name);
        }
    }

    /*
     * Cache for profiler event sources (dev_tp).
     * Store Python interned strings in member->private for fast field lookup.
     */
    for_each_dev_tp(ctx->tp_list, tp, i) {
        struct prof_dev *source_dev = tp->source_dev;
        struct python_event_data *ev = &ctx->events[i];
        struct perf_evlist *evlist;
        struct perf_evsel *evsel;
        char *handler_name;
        const char *name;

        if (!source_dev)
            continue;

        tp->private = ev; /* Link tp to its python_event_data */

        /*
         * Cache event name string for profiler events.
         * For dev_tp, tp->sys is NULL, use profiler name or alias.
         */
        name = tp->alias ? tp->alias : tp->name;
        ev->event_name = PyUnicode_InternFromString(name);
        if (!ev->event_name)
            goto failed;

        /* Look for profiler-specific handler. */
        handler_name = build_handler_name(tp->sys, tp->name, tp->alias);
        if (handler_name) {
            ev->handler = get_python_func(ctx->module, handler_name);
            free(handler_name);
        }

        /* Cache field keys in each evsel's member_cache */
        evlist = source_dev->evlist;
        perf_evlist__for_each_evsel(evlist, evsel) {
            struct perf_event_member_cache *cache = perf_evsel_member_cache(evsel);

            if (!cache)
                continue;

            for (j = 0; j < cache->nr_members; j++) {
                struct perf_event_member *member = &cache->members[j];
                /* Store interned Python string in member->private for fast lookup */
                if (!member->private) {
                    member->private = PyUnicode_InternFromString(member->name);
                    if (!member->private)
                        goto failed;
                }
            }
        }
    }
    ret = 0;

failed:
    tep__unref();
    return ret;
}

/*
 * Get the module's file path from __file__ attribute
 * Returns a newly allocated string that must be freed by caller, or NULL on failure
 */
static char *get_module_file_path(PyObject *module)
{
    PyObject *file_attr;
    const char *file_str;
    char *result = NULL;

    file_attr = PyObject_GetAttrString(module, "__file__");
    if (file_attr && PyUnicode_Check(file_attr)) {
        file_str = PyUnicode_AsUTF8(file_attr);
        if (file_str)
            result = strdup(file_str);
        Py_DECREF(file_attr);
    } else {
        PyErr_Clear();
    }
    return result;
}

/*
 * Extract module name from a file path
 * Handles:
 *   - myscript.py -> myscript
 *   - mymodule.cpython-36m-x86_64-linux-gnu.so -> mymodule
 *   - /path/to/myscript.py -> myscript
 *   - modname (no extension) -> modname
 *
 * Returns a newly allocated string that must be freed by caller
 */
static char *extract_module_name(const char *path)
{
    char *name_copy, *base, *dot, *result;

    name_copy = strdup(path);
    if (!name_copy)
        return NULL;

    /* Remove directory path */
    base = strrchr(name_copy, '/');
    if (base)
        base++;
    else
        base = name_copy;

    /* Remove extension:
     * - .py for Python scripts
     * - .cpython-*.so for Cython modules (find first '.' after module name)
     * - .so for other shared libraries
     */
    dot = strchr(base, '.');
    if (dot)
        *dot = '\0';

    result = strdup(base);
    free(name_copy);
    return result;
}

/*
 * Check if the path looks like a file (has extension or contains '/')
 */
static int looks_like_file_path(const char *path)
{
    /* Contains directory separator */
    if (strchr(path, '/'))
        return 1;

    /* Has common extension */
    if (strstr(path, ".py") || strstr(path, ".so"))
        return 1;

    return 0;
}

/*
 * Initialize Python interpreter and load script/module
 *
 * Supports multiple module types:
 *   - Python script: myscript.py or /path/to/myscript.py
 *   - Cython module: mymodule.cpython-36m-x86_64-linux-gnu.so
 *   - Module name only: mymodule (searched in sys.path and current dir)
 *   - Shared library: mymodule.so
 */
static int python_script_init(struct python_ctx *ctx)
{
    PyObject *sys_path, *path;
    char *script_dir = NULL, *module_name = NULL;
    char *script_path_copy = NULL;
    char *module_file_path = NULL;
    int is_file_path;

    /* Register perf_prof built-in module before Py_Initialize */
    if (register_perf_prof_module() < 0) {
        fprintf(stderr, "Failed to register perf_prof module\n");
        return -1;
    }

    /* Set PYTHONHOME for standalone Python distributions */
#ifdef PYTHON_HOME
    {
        wchar_t *python_home;
        python_home = Py_DecodeLocale(__stringify(PYTHON_HOME), NULL);
        if (python_home) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            Py_SetPythonHome(python_home);
#pragma GCC diagnostic pop
        }
    }
#endif

    /* Initialize Python */
    Py_Initialize();
    if (!Py_IsInitialized()) {
        fprintf(stderr, "Failed to initialize Python interpreter\n");
        return -1;
    }

    /* Initialize interned register name keys */
    if (init_perf_interned_keys() < 0) {
        fprintf(stderr, "Failed to initialize register name keys\n");
        return -1;
    }

    /* Set sys.argv from script arguments */
    if (script_argc > 0 && script_argv) {
#if PY_VERSION_HEX >= 0x030b0000  /* Python 3.11+: PySys_SetArgvEx deprecated */
        PyObject *argv_list = PyList_New(script_argc);
        if (argv_list) {
            int i;
            for (i = 0; i < script_argc; i++) {
                PyObject *arg = PyUnicode_DecodeFSDefault(script_argv[i]);
                if (!arg) { script_argc = i; break; }
                PyList_SET_ITEM(argv_list, i, arg);  /* steals ref */
            }
            PySys_SetObject("argv", argv_list);
            Py_DECREF(argv_list);
        }
#else
        wchar_t **wargv = calloc(script_argc, sizeof(wchar_t *));
        if (wargv) {
            int i;
            for (i = 0; i < script_argc; i++) {
                wargv[i] = Py_DecodeLocale(script_argv[i], NULL);
                if (!wargv[i]) {
                    script_argc = i;
                    break;
                }
            }
            if (script_argc)
                PySys_SetArgvEx(script_argc, wargv, 0);
            for (i = 0; i < script_argc; i++)
                PyMem_RawFree(wargv[i]);
            free(wargv);
        }
#endif
    }

    /* Set stdout and stderr to line-buffered mode if not already */
    if (PyRun_SimpleString(
            "import sys, os\n"
            "if sys.stdout.line_buffering != True:\n"
            "    sys.stdout = os.fdopen(sys.stdout.fileno(), 'w', 1)\n"
            "if sys.stderr.line_buffering != True:\n"
            "    sys.stderr = os.fdopen(sys.stderr.fileno(), 'w', 1)\n"
        ) < 0) {
        PyErr_Print();
        fprintf(stderr, "Warning: Failed to set line buffering for Python stdio\n");
        /* Continue anyway, this is not fatal */
    }

    /* Import perf_prof module to ensure PerfEvent types are initialized */
    ctx->perf_prof_module = PyImport_ImportModule("perf_prof");
    if (!ctx->perf_prof_module) {
        PyErr_Print();
        fprintf(stderr, "Failed to import perf_prof module\n");
        return -1;
    }

    /* Determine if input looks like a file path or module name */
    is_file_path = looks_like_file_path(ctx->script_path);

    if (is_file_path) {
        /* Add script/module directory to sys.path */
        script_path_copy = strdup(ctx->script_path);
        if (!script_path_copy)
            return -1;

        script_dir = dirname(script_path_copy);
        sys_path = PySys_GetObject("path");
        if (sys_path) {
            path = PyUnicode_FromString(script_dir);
            if (path) {
                PyList_Insert(sys_path, 0, path);
                Py_DECREF(path);
            }
        }
    } else {
        /* Module name only - add current directory to sys.path */
        sys_path = PySys_GetObject("path");
        if (sys_path) {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd))) {
                path = PyUnicode_FromString(cwd);
                if (path) {
                    PyList_Insert(sys_path, 0, path);
                    Py_DECREF(path);
                }
            }
        }
    }

    /* Extract module name from path */
    module_name = extract_module_name(ctx->script_path);
    if (!module_name) {
        free(script_path_copy);
        return -1;
    }

    /* Import the module */
    ctx->module = PyImport_ImportModule(module_name);

    if (!ctx->module) {
        PyErr_Print();
        fprintf(stderr, "Failed to load Python module: %s\n", ctx->script_path);
        fprintf(stderr, "  Searched module name: %s\n", module_name);
        if (is_file_path && script_dir)
            fprintf(stderr, "  Added to sys.path: %s\n", script_dir);
        free(module_name);
        free(script_path_copy);
        return -1;
    }

    /* Get and print module file path */
    module_file_path = get_module_file_path(ctx->module);
    if (module_file_path) {
        printf("Loaded module: %s\n", module_file_path);
        free(module_file_path);
    } else {
        /* Some built-in modules don't have __file__, but user modules should */
        printf("Loaded module: %s (file path not available)\n", module_name);
    }

    free(module_name);
    free(script_path_copy);

    /* Get callback functions */
    ctx->func_init = get_python_func(ctx->module, "__init__");
    ctx->func_exit = get_python_func(ctx->module, "__exit__");
    ctx->func_print_stat = get_python_func(ctx->module, "__print_stat__");
    ctx->func_interval = get_python_func(ctx->module, "__interval__");
    ctx->func_lost = get_python_func(ctx->module, "__lost__");
    ctx->func_sample = get_python_func(ctx->module, "__sample__");

    return 0;
}

static void python_script_exit(struct python_ctx *ctx)
{
    int i, j;
    struct tp *tp;

    if (ctx->func_init) Py_DECREF(ctx->func_init);
    if (ctx->func_exit) Py_DECREF(ctx->func_exit);
    if (ctx->func_print_stat) Py_DECREF(ctx->func_print_stat);
    if (ctx->func_interval) Py_DECREF(ctx->func_interval);
    if (ctx->func_lost) Py_DECREF(ctx->func_lost);
    if (ctx->func_sample) Py_DECREF(ctx->func_sample);

    /* Free per-event data */
    if (ctx->events) {
        for (i = 0; i < ctx->nr_events; i++) {
            struct python_event_data *ev = &ctx->events[i];

            Py_XDECREF(ev->handler);
            Py_XDECREF(ev->event_name);

            if (ev->field_keys) {
                for (j = 0; j < ev->nr_fields; j++) {
                    Py_XDECREF(ev->field_keys[j]);
                }
                free(ev->field_keys);
            }

            if (ev->fields)
                free(ev->fields);
        }
        free(ctx->events);
    }

    /*
     * Free cached Python string keys in member_cache for profiler event sources.
     * These were created in cache_event_fields() and stored in member->private.
     *
     * Safe to access source_dev here: tp holds a refcount on source_dev (added in
     * commit 78a6066 "tep: Add refcount for source_dev to control release order"),
     * guaranteeing it outlives the tp that references it.
     */
    for_each_dev_tp(ctx->tp_list, tp, i) {
        struct prof_dev *source_dev = tp->source_dev;
        struct perf_evlist *evlist;
        struct perf_evsel *evsel;

        if (!source_dev)
            continue;

        evlist = source_dev->evlist;
        perf_evlist__for_each_evsel(evlist, evsel) {
            struct perf_event_member_cache *cache = perf_evsel_member_cache(evsel);
            if (cache) {
                for (j = 0; j < cache->nr_members; j++) {
                    Py_XDECREF(cache->members[j].private);
                    cache->members[j].private = NULL;
                }
            }
        }
    }

    /* Free common key cache */
    free_key_cache(&ctx->key_cache);

    /* Free interned register name keys */
    free_perf_interned_keys();

    if (ctx->module) Py_DECREF(ctx->module);
    if (ctx->perf_prof_module) Py_DECREF(ctx->perf_prof_module);

    if (Py_IsInitialized())
        Py_Finalize();
}

/*
 * Call Python __init__() function
 */
static int python_call_init(struct python_ctx *ctx)
{
    PyObject *result;

    if (!ctx->func_init)
        return 0;

    result = PyObject_CallObject(ctx->func_init, NULL);
    if (!result) {
        PyErr_Print();
        return -1;
    }
    Py_DECREF(result);
    return 0;
}

/*
 * Call Python __exit__() function
 */
static void python_call_exit(struct python_ctx *ctx)
{
    PyObject *result;

    if (!ctx->func_exit)
        return;

    result = PyObject_CallObject(ctx->func_exit, NULL);
    if (!result) {
        PyErr_Print();
        return;
    }
    Py_DECREF(result);
}

/*
 * Call Python __print_stat__(indent) function
 */
static void python_call_print_stat(struct python_ctx *ctx, int indent)
{
    PyObject *result;

    if (!ctx->func_print_stat)
        return;

    result = PyObject_CallFunction(ctx->func_print_stat, "i", indent);
    if (!result) {
        PyErr_Print();
        return;
    }
    Py_DECREF(result);
}

/*
 * Call Python __interval__() function
 */
static void python_call_interval(struct python_ctx *ctx)
{
    PyObject *result;

    if (!ctx->func_interval)
        return;

    result = PyObject_CallObject(ctx->func_interval, NULL);
    if (!result) {
        PyErr_Print();
        return;
    }
    Py_DECREF(result);
}

/*
 * Call Python __lost__(lost_start, lost_end) function
 * lost_start: timestamp of the last sample before lost (0 if --order not enabled)
 * lost_end: timestamp of the first sample after lost (0 if --order not enabled)
 */
static void python_call_lost(struct python_ctx *ctx, u64 lost_start, u64 lost_end)
{
    PyObject *result;

    if (!ctx->func_lost)
        return;

    result = PyObject_CallFunction(ctx->func_lost, "KK",
                                   (unsigned long long)lost_start,
                                   (unsigned long long)lost_end);
    if (!result) {
        PyErr_Print();
        return;
    }
    Py_DECREF(result);
}

/*
 * python_argc_init - Parse extra command line arguments (script.py [script args...])
 * Called before init() to capture the script path and arguments.
 * Usage: perf-prof python -e EVENT -- script.py --script-opts
 */
static int python_argc_init(int argc, char *argv[])
{
    if (argc >= 1) {
        script_path = argv[0];
        script_argc = argc;
        script_argv = argv;
    } else {
        script_path = NULL;
        script_argc = 0;
        script_argv = NULL;
    }
    return 0;
}

/*
 * monitor_ctx_init - Initialize the Python profiler context
 */
static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct python_ctx *ctx;

    if (!env->event) {
        fprintf(stderr, "Error: -e EVENT is required\n");
        return -1;
    }

    if (!script_path) {
        fprintf(stderr, "Error: Python module/script is required\n");
        fprintf(stderr, "Usage: perf-prof python -e EVENT [--] module [args...]\n");
        fprintf(stderr, "  module can be:\n");
        fprintf(stderr, "    - Python script: myscript.py or /path/to/myscript.py\n");
        fprintf(stderr, "    - Cython module: mymodule.cpython-36m-x86_64-linux-gnu.so\n");
        fprintf(stderr, "    - Module name: mymodule (searched in sys.path and current dir)\n");
        return -1;
    }

    ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;

    /* Initialize live_events rb tree for minevtime tracking */
    ctx->live_events = RB_ROOT;

    tep__ref();

    ctx->script_path = strdup(script_path);
    if (!ctx->script_path)
        goto failed;

    ctx->tp_list = tp_list_new(dev, env->event);
    if (!ctx->tp_list)
        goto failed;

    /* Initialize callchain support if enabled */
    if (env->callchain || ctx->tp_list->nr_need_stack) {
        ctx->callchain_flags = callchain_flags(dev, CALLCHAIN_KERNEL);
        if (ctx->callchain_flags) {
            if (callchain_pylist_init(ctx->callchain_flags) < 0)
                goto failed;
            ctx->cc = callchain_ctx_new(ctx->callchain_flags, stdout);
            if (!ctx->cc)
                goto failed;
            dev->pages *= 2;  /* Increase buffer for callchain data */
        }
    }

    /* Initialize Python and load script */
    if (python_script_init(ctx) < 0)
        goto failed;

    /* Cache event fields */
    if (cache_event_fields(ctx) < 0)
        goto failed;

    dev->private = ctx;
    return 0;

failed:
    if (ctx->cc)
        callchain_ctx_free(ctx->cc);
    if (ctx->callchain_flags)
        callchain_pylist_exit(ctx->callchain_flags);
    python_script_exit(ctx);
    if (ctx->tp_list)
        tp_list_free(ctx->tp_list);
    if (ctx->script_path)
        free(ctx->script_path);
    tep__unref();
    free(ctx);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct python_ctx *ctx = dev->private;

    if (ctx->cc)
        callchain_ctx_free(ctx->cc);
    if (ctx->callchain_flags)
        callchain_pylist_exit(ctx->callchain_flags);

    python_script_exit(ctx);

    tp_list_free(ctx->tp_list);
    free(ctx->script_path);
    tep__unref();
    free(ctx);
}

static int python_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct python_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID |
                         PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW |
                         (env->callchain ? PERF_SAMPLE_CALLCHAIN : 0),
        .read_format   = PERF_FORMAT_ID,
        .pinned        = 1,
        .disabled      = 1,
        .watermark     = 1,
        .exclude_callchain_user = exclude_callchain_user(dev, CALLCHAIN_KERNEL),
        .exclude_callchain_kernel = exclude_callchain_kernel(dev, CALLCHAIN_KERNEL),
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

    /* Forward events from profiler event sources (dev_tp) to python device */
    for_each_dev_tp(ctx->tp_list, tp, i) {
        struct prof_dev *source_dev = tp->source_dev;
        if (source_dev) {
            if (prof_dev_forward(source_dev, dev) < 0) {
                fprintf(stderr, "Failed to forward events from %s to python\n", tp->name);
                goto failed;
            }
        }
    }

    /* Call Python __init__ */
    if (python_call_init(ctx) < 0)
        goto failed;

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static int python_filter(struct prof_dev *dev)
{
    struct python_ctx *ctx = dev->private;
    return tp_list_apply_filter(dev, ctx->tp_list);
}

static void python_exit(struct prof_dev *dev)
{
    struct python_ctx *ctx = dev->private;
    python_call_exit(ctx);
    monitor_ctx_exit(dev);
}

static void python_lost(struct prof_dev *dev, union perf_event *event,
                        int instance, u64 lost_start, u64 lost_end)
{
    struct python_ctx *ctx = dev->private;
    print_lost_fn(dev, event, instance);
    python_call_lost(ctx, lost_start, lost_end);
}

static long python_ftrace_filter(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct python_ctx *ctx = dev->private;
    struct python_sample_type *data = (void *)event->sample.array;
    struct perf_evsel *evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
    struct tp *tp = tp_from_evsel(evsel, ctx->tp_list);
    void *raw;
    int size;

    if (!tp) return 0;
    if (!tp->ftrace_filter)
        return 1;

    if (tp->stack) {
        struct callchain *cc = (struct callchain *)&data->raw;
        struct {
            __u32 size;
            __u8 data[0];
        } *raw_data = (void *)cc->ips + cc->nr * sizeof(__u64);
        raw = raw_data->data;
        size = raw_data->size;
    } else {
        raw = data->raw.data;
        size = data->raw.size;
    }
    return tp_prog_run(tp, tp->ftrace_filter, GLOBAL(data->cpu_entry.cpu, data->tid_entry.pid, raw, size));
}

static void python_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct python_ctx *ctx = dev->private;
    struct python_sample_type *data;
    struct perf_evsel *evsel;
    struct tp *tp = NULL;
    struct python_event_data *ev;
    PerfEventObject *perf_event;
    PyObject *result;

    /*
     * Check if this is a forwarded event from a profiler source (PERF_RECORD_DEV).
     * For forwarded events, extract the source device and inner event.
     */
    if (event->header.type == PERF_RECORD_DEV) {
        struct perf_record_dev *event_dev = (void *)event;
        struct prof_dev *source_dev = event_dev->dev;
        int i;

        /* Find the matching tp for this source device */
        for_each_dev_tp(ctx->tp_list, tp, i) {
            if (tp->source_dev == source_dev)
                break;
        }
        if (!tp || tp->source_dev != source_dev)
            return;

        /* Profiler event: use special creation path */
        perf_event = PerfEvent_create_from_dev(dev, tp, event);
    } else {
        /* Regular tracepoint event: find tp from evsel */
        data = (void *)event->sample.array;
        evsel = perf_evlist__id_to_evsel(dev->evlist, data->id, NULL);
        tp = tp_from_evsel(evsel, ctx->tp_list);
        if (!tp)
            return;

        /* Tracepoint event: use normal creation path */
        perf_event = PerfEvent_create(dev, tp, event, instance);
    }

    if (!perf_event)
        return;

    ev = tp->private;
    /* Call event-specific handler or default __sample__ */
    if (ev->handler) {
        result = PyObject_CallFunctionObjArgs(ev->handler, perf_event, NULL);
        if (!result)
            PyErr_Print();
        else
            Py_DECREF(result);
    } else if (ctx->func_sample) {
        /* _event field is available via PerfEvent_get_event getter */
        result = PyObject_CallFunctionObjArgs(ctx->func_sample, perf_event, NULL);
        if (!result)
            PyErr_Print();
        else
            Py_DECREF(result);
    }

    /*
     * Check if Python script kept a reference to the event.
     * If refcnt > 1, the script stored it somewhere (e.g., in a list),
     * so we need to track it for minevtime calculation.
     */
    if (Py_REFCNT(perf_event) > 1)
        live_events_insert(ctx, perf_event);

    Py_DECREF(perf_event);
}

/*
 * python_minevtime - Return minimum event time of all live PerfEventObjects
 *
 * This is called by comm module to determine when it's safe to garbage collect
 * pid->comm mappings. Returns ULLONG_MAX if no live events exist.
 */
static u64 python_minevtime(struct prof_dev *dev)
{
    struct python_ctx *ctx = dev->private;
    struct rb_node *rbn;
    PerfEventObject *obj;

    rbn = rb_first(&ctx->live_events);
    if (!rbn)
        return ULLONG_MAX;

    obj = rb_entry(rbn, PerfEventObject, rb_node);
    return obj->_time;
}

static void python_interval(struct prof_dev *dev)
{
    struct python_ctx *ctx = dev->private;
    python_call_interval(ctx);
}

static void python_print_dev(struct prof_dev *dev, int indent)
{
    struct python_ctx *ctx = dev->private;
    dev_printf("live_events: %lu\n", ctx->nr_live_events);
    python_call_print_stat(ctx, indent);
}

/*
 * Generate Python type hint based on field flags
 */
static const char *python_type_hint(struct tep_event *event, struct tep_format_field *field)
{
    if (field->flags & TEP_FIELD_IS_STRING) {
        /* Check if field needs binary output despite having IS_STRING flag */
        if (tep__string_field_as_binary(event, field))
            return "bytes";
        return "str";
    } else if (field->flags & TEP_FIELD_IS_ARRAY)
        return "bytes";
    else
        return "int";
}

/*
 * Output Python script template with event handler functions
 */
static void python_help_script_template(struct help_ctx *hctx)
{
    struct tep_handle *tep;
    int i, j, k;
    int has_events = 0;

    /* Check if we have any events */
    for (i = 0; i < hctx->nr_list; i++) {
        if (hctx->tp_list[i]->nr_tp > 0) {
            has_events = 1;
            break;
        }
    }

    printf("\n");
    printf("# =============================================================================\n");
    printf("# Python Script Template for perf-prof python\n");
    printf("# =============================================================================\n");
    printf("#\n");
    printf("# Save this template to a .py file and customize as needed.\n");
    printf("# Functions marked [OPTIONAL] can be safely deleted if not needed.\n");
    printf("# Exceptions raised in functions will be printed but won't stop processing.\n");
    printf("#\n");
    printf("# PerfEvent object fields:\n");
    printf("#\n");
    printf("#   Tracepoint events (-e sys:name):\n");
    printf("#   _pid, _tid    : Process/thread ID (int)\n");
    printf("#   _time         : Event timestamp in nanoseconds (int)\n");
    printf("#   _cpu          : CPU number (int)\n");
    printf("#   _period       : Sample period (int)\n");
    printf("#   common_type, common_flags, common_preempt_count, common_pid : trace_entry fields\n");
    printf("#   _realtime     : Wall clock time in ns since Unix epoch (int, lazy computed)\n");
    printf("#                   Note: Has drift, only for display, not for latency calc\n");
    printf("#   _callchain    : Call stack list (when -g or stack attribute is set, lazy computed)\n");
    printf("#                   Each frame dict: {'addr': int, 'symbol': str,\n");
    printf("#                                     'offset': int, 'kernel': bool, 'dso': str}\n");
    printf("#   _event        : Event name with alias if set (str, only in __sample__, lazy computed)\n");
    printf("#   <field>       : Event-specific fields (int/str/bytes, lazy computed)\n");
    printf("#\n");
    printf("#   Profiler events (-e profiler):\n");
    printf("#   _pid, _tid    : Process/thread ID (int)\n");
    printf("#   _time         : Event timestamp in nanoseconds (int)\n");
    printf("#   _cpu          : CPU number (int)\n");
    printf("#   _realtime     : Wall clock time in ns since Unix epoch (int, lazy computed)\n");
    printf("#   _event        : Event name with alias if set (str, only in __sample__, lazy computed)\n");
    printf("#   <field>       : Profiler-specific fields based on sample_type (lazy computed)\n");
    printf("#\n");
    printf("# PerfEvent access methods:\n");
    printf("#   event.field or event['field']  - Access field value\n");
    printf("#   event.get(field, default=None) - Get field with default fallback\n");
    printf("#   'field' in event               - Check if field exists\n");
    printf("#   len(event)                     - Number of fields\n");
    printf("#   event.keys(), event.values(), event.items()  - Dict-like access\n");
    printf("#   for field, value in event      - Iterate over fields\n");
    printf("#   event.print(timestamp=True, callchain=True)  - Print in perf-prof format:\n");
    printf("#       YYYY-MM-DD HH:MM:SS.uuuuuu            comm   pid .... [cpu] time.us: sys:name: fields\n");
    printf("#           addr symbol+offset (dso)\n");
    printf("#   event.to_dict()                - Convert to regular Python dict\n");
    printf("#   str(event), repr(event)        - String representations\n");
    printf("#   hash(event)                    - Hash of entire perf event\n");
    printf("#\n");
    printf("# =============================================================================\n");
    printf("\n");
    printf("# Import other modules as needed (examples)\n");
    printf("# import sys\n");
    printf("# import json\n");
    printf("# import argparse\n");
    printf("# from collections import defaultdict, Counter\n");
    printf("\n");
    printf("# Script arguments available via sys.argv:\n");
    printf("#   perf-prof python -e EVENT -- script.py --foo bar\n");
    printf("#   sys.argv = ['script.py', '--foo', 'bar']\n");
    printf("\n");

    /* Global variables section */
    printf("# Global variables for statistics\n");
    printf("event_count = 0\n");
    printf("interval_count = 0\n");
    printf("\n");

    /* __init__ function - optional */
    printf("# [OPTIONAL] Delete if no initialization needed\n");
    printf("def __init__():\n");
    printf("    \"\"\"Called once before event processing starts.\"\"\"\n");
    printf("    global event_count, interval_count\n");
    printf("    event_count = 0\n");
    printf("    interval_count = 0\n");
    printf("    print(\"Python script initialized\")\n");
    printf("\n");

    /* __exit__ function - optional */
    printf("# [OPTIONAL] Delete if no cleanup/summary needed\n");
    printf("def __exit__():\n");
    printf("    \"\"\"Called once before program exit.\"\"\"\n");
    printf("    # Redirect stderr to devnull to suppress BrokenPipeError when piped to head/tail\n");
    printf("    # e.g., perf-prof python -e event script.py | head\n");
    printf("    # import os; sys.stderr = open(os.devnull, 'w')\n");
    printf("    print(f\"Total events processed: {event_count}\")\n");
    printf("    print(f\"Total intervals: {interval_count}\")\n");
    printf("\n");

    /* __interval__ function - optional */
    printf("# [OPTIONAL] Delete if -i interval not used\n");
    printf("def __interval__():\n");
    printf("    \"\"\"Called at each -i interval.\"\"\"\n");
    printf("    global interval_count\n");
    printf("    interval_count += 1\n");
    printf("    print(f\"Interval {interval_count}: {event_count} events so far\")\n");
    printf("\n");

    /* __print_stat__ function - optional */
    printf("# [OPTIONAL] Delete if SIGUSR2 stats not needed\n");
    printf("def __print_stat__(indent: int):\n");
    printf("    \"\"\"Called on SIGUSR2 signal.\"\"\"\n");
    printf("    prefix = ' ' * indent\n");
    printf("    print(f\"{prefix}Events: {event_count}\")\n");
    printf("\n");

    /* __lost__ function - optional */
    printf("# [OPTIONAL] Delete if event loss notification not needed\n");
    printf("def __lost__(lost_start: int, lost_end: int):\n");
    printf("    \"\"\"\n");
    printf("    Called when events are lost.\n");
    printf("    lost_start: timestamp of last sample before lost (0 if --order not enabled)\n");
    printf("    lost_end: timestamp of first sample after lost (0 if --order not enabled)\n");
    printf("    Events from other instances within [lost_start, lost_end] may be incomplete.\n");
    printf("    \"\"\"\n");
    printf("    print(f\"Warning: events lost! time range: {lost_start} - {lost_end}\")\n");
    printf("\n");

    /* Generate event-specific handlers if events are specified */
    if (has_events) {
        tep = tep__ref_light();

        printf("# =============================================================================\n");
        printf("# Event-specific handlers (higher priority than __sample__)\n");
        printf("# [OPTIONAL] Delete these if using __sample__ for all events\n");
        printf("# =============================================================================\n");
        printf("\n");

        for (i = 0; i < hctx->nr_list; i++) {
            struct tp *tp;
            char *handler_name;

            for_each_real_tp(hctx->tp_list[i], tp, j) {
                struct tep_event *event = tep_find_event(tep, tp->id);
                struct tep_format_field **fields = NULL;

                /* Build handler name using alias if available */
                handler_name = build_handler_name(tp->sys, tp->name, tp->alias);
                if (!handler_name)
                    continue;

                /* Function definition with docstring */
                printf("def %s(event):\n", handler_name);
                printf("    \"\"\"\n");
                printf("    Handler for %s:%s", tp->sys, tp->name);
                if (tp->alias)
                    printf(" (alias: %s)", tp->alias);
                printf("\n");
                printf("    event is a PerfEvent object with lazy field evaluation.\n");

                /* Document event-specific fields */
                if (event) {
                    fields = tep_event_fields(event);
                    if (fields) {
                        printf("    \n");
                        printf("    Event-specific fields:\n");
                        for (k = 0; fields[k]; k++) {
                            printf("        %s : %s\n", fields[k]->name,
                                   python_type_hint(event, fields[k]));
                        }
                    }
                }
                printf("    \"\"\"\n");

                /* Function body with field access examples */
                printf("    global event_count\n");
                printf("    event_count += 1\n");
                printf("    \n");
                printf("    # Access common fields\n");
                printf("    pid = event['_pid']\n");
                printf("    time_ns = event['_time']\n");
                printf("    \n");

                if (fields) {
                    printf("    # Access event-specific fields\n");
                    for (k = 0; fields[k]; k++) {
                        printf("    # %s = event['%s']  # %s\n", fields[k]->name,
                               fields[k]->name, python_type_hint(event, fields[k]));
                    }
                    printf("    \n");
                    free(fields);
                }

                printf("    # Example: print event\n");
                printf("    # event.print()  # or event.print(timestamp=True, callchain=True)\n");
                printf("\n");

                free(handler_name);
            }

            /* Generate handlers for profiler event sources (dev_tp) */
            for_each_dev_tp(hctx->tp_list[i], tp, j) {
                handler_name = build_handler_name(tp->sys, tp->name, tp->alias);
                if (!handler_name)
                    continue;

                printf("def %s(event):\n", handler_name);
                printf("    \"\"\"\n");
                printf("    Handler for profiler %s:%s", tp->sys, tp->name);
                if (tp->alias)
                    printf(" (alias: %s)", tp->alias);
                printf("\n");
                printf("    event is a PerfEvent object with lazy field evaluation.\n");
                printf("    \"\"\"\n");

                printf("    global event_count\n");
                printf("    event_count += 1\n");
                printf("    \n");
                printf("    # Access common fields\n");
                printf("    pid = event['_pid']\n");
                printf("    time_ns = event['_time']\n");
                printf("    cpu = event['_cpu']\n");
                printf("    \n");
                printf("    # Example: print event\n");
                printf("    # event.print()  # or event.print(timestamp=True, callchain=True)\n");
                printf("\n");

                free(handler_name);
            }
        }

        tep__unref();
    }

    /* Default __sample__ handler */
    printf("# =============================================================================\n");
    printf("# Default event handler (used when no specific handler is defined)\n");
    printf("# [OPTIONAL] Delete if using event-specific handlers for all events\n");
    printf("# =============================================================================\n");
    printf("\n");
    printf("def __sample__(event):\n");
    printf("    \"\"\"\n");
    printf("    Default handler for all events without specific handlers.\n");
    printf("    event is a PerfEvent object. The _event field has format 'sys:name' or 'sys:alias'.\n");
    printf("    \"\"\"\n");
    printf("    global event_count\n");
    printf("    event_count += 1\n");
    printf("    \n");
    printf("    event_name = event._event\n");
    printf("    cpu = event._cpu\n");
    printf("    \n");
    printf("    # Example: print event\n");
    printf("    # event.print()  # or print(event)\n");
}

static void python_help(struct help_ctx *hctx)
{
    int i, j;

    printf("# " PROGRAME " python ");
    printf("-e \"");
    for (i = 0; i < hctx->nr_list; i++) {
        struct tp *tp;
        for_each_tp(hctx->tp_list[i], tp, j) {
            printf("%s%s%s/%s/alias=%s/", tp->sys ? tp->sys : "", tp->sys ? ":" : "", tp->name,
                   tp->filter && tp->filter[0] ? tp->filter : "", tp->alias ? tp->alias : "");
            if (i != hctx->nr_list - 1 ||
                j != hctx->tp_list[i]->nr_tp - 1)
                printf(",");
        }
    }
    printf("\" script.py\n");

    /* Output Python script template */
    python_help_script_template(hctx);
}

static const char *python_desc[] = PROFILER_DESC("python",
    "[OPTION...] -e EVENT[,EVENT...] [--] module [args...]",
    "Process perf events with Python scripts or modules.",
    "",
    "SYNOPSIS",
    "    Convert perf events to PerfEvent objects and process them with custom",
    "    Python scripts or modules. PerfEvent provides lazy field evaluation.",
    "    Arguments after module name are available via sys.argv.",
    "",
#ifdef PYTHON_HOME
    "PYTHONHOME",
    "    " __stringify(PYTHON_HOME),
    "",
#endif
    "MODULE TYPES",
    "    Python script       myscript.py or /path/to/myscript.py",
    "    Cython module       mymodule.cpython-36m-x86_64-linux-gnu.so",
    "    Module name         mymodule (searched in sys.path and current dir)",
    "    Shared library      mymodule.so",
    "",
    "SCRIPT SYNTAX",
    "  CALLBACK FUNCTIONS",
    "    __init__()              - Called once before event processing",
    "    __exit__()              - Called once before program exit",
    "    __print_stat__(indent)  - Called on SIGUSR2 signal",
    "    __interval__()          - Called at each -i interval",
    "    __lost__(lost_start, lost_end)  - Called when events are lost",
    "                              lost_start/lost_end: time range (0 if no --order)",
    "",
    "  EVENT HANDLERS (priority: specific > default)",
    "    sys__event_name(event)  - Event-specific handler (event is PerfEvent)",
    "                              e.g., sched__sched_wakeup for sched:sched_wakeup",
    "                              Characters '-', '.', ':' converted to '_'",
    "    sys__alias(event)       - Alias-specific handler (when alias= is used)",
    "                              e.g., sched__wakeup1 for alias=wakeup1",
    "    __sample__(event)       - Default handler (event includes _event field)",
    "",
    "  PERFEVENT OBJECT FIELDS",
    "    Tracepoint events (-e sys:name):",
    "    _pid, _tid              - Process/thread ID",
    "    _time                   - Event timestamp (ns)",
    "    _cpu                    - CPU number",
    "    _period                 - Sample period",
    "    common_type, common_flags, common_preempt_count, common_pid - trace_entry fields",
    "    _realtime               - Wall clock time (ns since Unix epoch, lazy computed)",
    "                              Note: Has drift, for display only, not latency calc",
    "    _callchain              - Call stack list (when -g or stack attribute, lazy computed)",
    "                              Each frame: {'addr', 'symbol', 'offset', 'kernel', 'dso'}",
    "    _event                  - Event name, uses alias if set (only in __sample__)",
    "    <field>                 - Event-specific fields (int/str/bytes, lazy computed)",
    "",
    "    Profiler events (-e profiler):",
    "    _pid, _tid              - Process/thread ID",
    "    _time                   - Event timestamp (ns)",
    "    _cpu                    - CPU number",
    "    _realtime               - Wall clock time (ns since Unix epoch, lazy computed)",
    "    _event                  - Event name, uses alias if set (only in __sample__)",
    "    <field>                 - Profiler-specific fields based on sample_type",
    "",
    "  PERFEVENT ACCESS METHODS",
    "    event.field or event['field']  - Access field value",
    "    event.get(field, default=None) - Get field with default fallback",
    "    'field' in event        - Check if field exists",
    "    len(event)              - Number of fields",
    "    event.keys(), values(), items()  - Dict-like access",
    "    for field, value in event  - Iterate over fields",
    "    event.print(timestamp=True, callchain=True)  - Print in perf-prof format:",
    "        YYYY-MM-DD HH:MM:SS.uuuuuu            comm   pid .... [cpu] time.us: sys:name: fields",
    "            addr symbol+offset (dso)",
    "    event.to_dict()         - Convert to regular Python dict",
    "    str(event), repr(event) - String representations",
    "    hash(event)             - Hash of entire perf event",
    "",
    "EXAMPLES",
    "    "PROGRAME" python -e sched:sched_wakeup counter.py",
    "    "PROGRAME" python -e sched:sched_wakeup,sched:sched_switch -i 1000 analyzer.py",
    "    "PROGRAME" python -e 'sched:sched_wakeup/pid>1000/' -C 0-3 filter.py",
    "    "PROGRAME" python -e 'sched:sched_wakeup//alias=w1/,sched:sched_wakeup//alias=w2/' multi.py",
    "    "PROGRAME" python -e sched:sched_wakeup -g callstack.py  # with callchain");

static const char *python_argv[] = PROFILER_ARGV("python",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_CALLCHAIN_FILTER,
    PROFILER_ARGV_PROFILER, "event", "call-graph");

static profiler python = {
    .name = "python",
    .desc = python_desc,
    .argv = python_argv,
    .pages = 8,
    .help = python_help,
    .argc_init = python_argc_init,
    .init = python_init,
    .filter = python_filter,
    .deinit = python_exit,
    .print_dev = python_print_dev,
    .interval = python_interval,
    .minevtime = python_minevtime,
    .lost = python_lost,
    .ftrace_filter = python_ftrace_filter,
    .sample = python_sample,
};
PROFILER_REGISTER(python);
