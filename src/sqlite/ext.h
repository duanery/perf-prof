/*
 * ext.h - SQLite extension interface for perf tracepoint events
 * Defines data structures shared between sql.c and sqlite/perf_tp.c:
 */
#ifndef __SQLITE_EXT_H
#define __SQLITE_EXT_H

#include <monitor.h>
#include <tep.h>
#include <linux/rblist.h>

#include <sqlite/sqlite3.h>

/*
 * Perf event sample layout matching the sample_type configuration:
 * PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU |
 * PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW
 *
 * raw.common is only there for tracepoints; the raw data of a BPF event is
 * its struct alone.
 */
struct sql_sample_type {
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
    __u64        period;
    struct {
        __u32   size;
        union {
            __u8    data[0];
            struct trace_entry common;
        };
    } raw;
};

/* Constraint usage tracking: where the constraint is applied */
enum {
    USED_BY_NONE,       /* Not used for optimization */
    USED_BY_FTRACE      /* Converted to kernel ftrace filter */
};

/*
 * SQL WHERE clause constraint collected from xBestIndex.
 * Used for: (1) ftrace filter generation, (2) index field selection.
 */
struct constraint {
    int field;              /* Column index (see constraint_can_ftrace() for layout) */
    unsigned char op;       /* Operator: EQ, GT, LE, LT, GE, NE */
    unsigned char used_by;  /* USED_BY_NONE or USED_BY_FTRACE */
    bool value_set;         /* True if RHS value was available at planning time */
    sqlite3_int64 value;    /* RHS value (valid only if value_set is true) */
};

/* Constraint set from a single xBestIndex call */
struct BestIndex {
    struct constraint *constraints;
    int nr_constraints;
    int cost;               /* Estimated query cost for this constraint set */
};

#define for_each_constraint(tp_priv, constraint, i, j) \
    for (i = 0; i < tp_priv->best_index_num; i++) \
        for (j = 0; j < tp_priv->best_index[i].nr_constraints; j++) \
            if ((constraint = &tp_priv->best_index[i].constraints[j]))


/* Event node for memory mode linked list */
struct tp_event {
    struct list_head link;       // link to tp_private::event_list
    struct list_head link_index; // link to IndexNode::event_list
    uint64_t rowid;
    union perf_event event;
};

struct tp_private {
    /* Common fields */
    struct tep_format_field **fields;
    event_fields *bpf_fields;   /* bpf:xxx: event fields from BTF, instead of fields */
    /*
     * Columns: nr_sys system columns, then the event fields, nr_fields in all.
     * System columns: _pid, _tid, _time, _cpu, _period, and for tracepoints
     * common_flags, common_preempt_count, common_pid. A BPF event has no
     * trace_entry: 5 system columns.
     */
    int nr_sys;
    int nr_fields;
    const char *table_name;
    char *function_list;  /* Comma-separated list of available SQL functions for this event */
    time_t created_time;
    enum {
        FILE_MODE,
        MEM_VIRTUAL_TABLE_MODE,
        MEM_REGULAR_TABLE_MODE,
    } mode;

    /* Memory mode: events stored in linked list */
    uint64_t rowid;
    struct list_head event_list; /* struct tp_event linked via tp_event::link */
    struct rb_root index_tree;   /* struct IndexNode for O(log n) lookup */

    /* Query planner constraint collection (populated during init phase) */
    struct BestIndex *best_index;   /* Array of constraint sets from xBestIndex calls */
    int best_index_num;             /* Number of constraint sets collected */
    int have_index;                 /* True if index field was selected */
    int index_field;                /* Column index chosen for indexing */
    bool index_is_str;              /* True if index column is a string type */
    /*
     * Query planner optimization (collected during init phase):
     * col_used:      Bitmask of columns needed by --query (from xBestIndex colUsed).
     * col_refs:      Reference count per column from WHERE + ORDER BY constraints.
     *                Used to auto-select the most referenced field for indexing.
     * ftrace_filter: Kernel filter expression generated from SQL WHERE constraints.
     *                Applied via tp_list_apply_filter() before perf_event_open().
     * init:          True during initialization to enable optimization data collection.
     */
    uint64_t col_used;
    int *col_refs;
    char *ftrace_filter;
    bool init;

    int have_prepare, have_filter;
    int nr_query, nr_filter;

    /* Memory mode: events inserted via prepared statement */
    sqlite3_stmt *mem_insert_stmt;

    /* File mode: events inserted via prepared statement */
    sqlite3_stmt *insert_stmt;

    /* Index statistics */
    uint64_t xFilter;
    uint64_t xEof;
    uint64_t xNext;
    uint64_t xColumn, xRowid;
    uint64_t scan_list;
    uint64_t do_index;
    uint64_t do_filter;

    /* Event statistics */
    uint64_t sample_count;
    uint64_t first_sample_time;
    uint64_t last_sample_time;
};

struct sql_tp_ctx {
    sqlite3 *sql;
    struct tp_list *tp_list;
    struct tep_handle *tep;
    struct rblist symbolic_table;
    int nr_symbolic;
    int event_id;   // nr_symbolic==1
    int field_offset;
    const char *field_name;
    struct sqlite_func {
        int data_type; // SQLITE_INTEGER, SQLITE_BLOB
        const char *func_name;
    } *sqlite_funcs;
    int ksymbol;
    int verbose;

    int (*sample)(struct sql_tp_ctx *ctx, struct tp *tp, union perf_event *event);
    void (*reset)(struct sql_tp_ctx *ctx);
};

struct sql_tp_ctx *sql_tp_file(sqlite3 *sql, struct tp_list *tp_list);
struct sql_tp_ctx *sql_tp_mem(sqlite3 *sql, struct tp_list *tp_list, const char *query);
void sql_tp_free(struct sql_tp_ctx *ctx);

#endif
