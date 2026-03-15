#ifndef __TEP_H
#define __TEP_H

#include <event-parse.h>
#include <expr.h>
#include <net.h>
#include <vcpu_info.h>
#include <tp_struct.h>

void pr_stat(const char *fmt, ...);

struct tep_handle *tep__ref(void);
struct tep_handle *tep__ref_light(void);
void tep__unref(void);
int tep__event_id(const char *sys, const char *name);
void tep__update_comm(const char *comm, int pid);
const char *tep__pid_to_comm(int pid);
void tep__print_event(unsigned long long ts, int cpu, void *data, int size);
bool tep__event_has_field(int id, const char *field);
bool tep__event_field_size(int id, const char *field);
bool tep__string_field_as_binary(struct tep_event *event, struct tep_format_field *field);
int tep__event_size(int id);

typedef struct global_var_declare event_fields;
event_fields *tep__event_fields(int id);

char *next_sep(char *s, int c);

struct prof_dev;
struct tp_matcher;

struct tp {
    struct prof_dev *dev;
    union {
        struct perf_evsel *evsel;
        struct prof_dev *source_dev;
    };
    int id; // id > 0: tp:sys; id < 0: profiler;
    int idx; // index in tp_list
    char *sys;
    char *name;
    char *filter;
    int stack;
    int max_stack;
    char *alias;
    void *private;
    struct tp_matcher *matcher;

    // kprobe, uprobe
    char *kprobe_func;
    char *uprobe_path;
    int uprobe_offset;

    struct expr_prog *ftrace_filter;

    // top profiler
    struct {
        // long tp_prog_run(struct tp *tp, ...)
        struct expr_prog *field_prog;
        char *field;
        bool event;
        bool top_by;
    } *top_add;
    int nr_top;

    // char *tp_get_comm(struct tp *tp, ...)
    struct expr_prog *comm_prog;
    const char *comm;

    // kmemleak profiler
    struct expr_prog *mem_ptr_prog;
    const char *mem_ptr;
    struct expr_prog *mem_size_prog;
    const char *mem_size;

    // num-dist profiler
    struct expr_prog *num_prog;
    const char *num;

    // multi-trace profiler
    // unsigned long tp_get_key(struct tp *tp, ...)
    struct expr_prog *key_prog;
    const char *key;
    bool untraced;
    bool trigger;

    struct expr_prog *printkey_prog;
    const char *printkey;

    // multi-trace profiler
    struct expr_prog *role_prog;
    const char *role;

    // event spread
    void *broadcast;
    void *receive;
    bool kernel; // event from kernel

    // vm
    struct vcpu_info *vcpu; // maybe NULL
    const char *vm;

    // A public expression that can be executed by any profiler.
    struct expr_prog *exec_prog;
    const char *exec;

    // sql index field
    const char *index;

    // cpus
    struct perf_cpu_map *cpus;

    // default tp_matcher
    struct expr_prog *tp_cpu_prog;
    struct expr_prog *tp_pid_prog;
};

struct tp_list {
    char *event_str;
    int nr_tp;
    int nr_real_tp;
    int nr_need_stack;
    bool need_stream_id;
    int nr_top;
    int nr_comm;
    int nr_mem_size;
    int nr_num_prog;
    int nr_untraced;
    int nr_push_to;
    int nr_pull_from;
    int nr_exec_prog;
    struct tp tp[0];
};

struct perf_record_tp {
    struct perf_event_header header;
    u32 id;
    u16 sys_offset;
    u16 name_offset;
    u64 sample_period;
    u64 sample_type;
    u32 event_size;
    u32 unused;
    char str[];
};

/*
 * perf_record_dev - Encapsulates events from source device for forwarding
 *
 * Used by perf_event_forward() to wrap PERF_RECORD_SAMPLE events from a source
 * device (profiler as event source) before forwarding to the target device
 * (e.g., multi-trace).
 *
 * The @event pointer references the original event without copying by default.
 * Deep copy occurs only when:
 *   1. Timestamp conversion is needed (dev->convert.need_conv)
 *   2. Python callchain is attached (pystack)
 *   3. Event is stored for later processing (via perf_event_dup)
 *
 * The @dev pointer is safe to access during event processing because tep's
 * refcount mechanism (prof_dev_use/prof_dev_unuse) guarantees source_dev
 * is released only after target device's deinit completes.
 */
struct perf_record_dev {
    struct perf_event_header header;

    // Same as multi_trace_type_header
    // PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU
    u32 pid, tid;
    u64 time;
    u64 id;
    u32 cpu, instance;
    struct prof_dev *dev;   /* source device, valid during target's lifetime */
    union perf_event *event; /* points to original or deep-copied event */
};

struct perf_record_order_time {
    struct perf_event_header header;
    u64 order_time;
};

enum tp_event_type {
    PERF_RECORD_TP = PERF_RECORD_HEADER_MAX + 1,
    PERF_RECORD_DEV,
    PERF_RECORD_ORDER_TIME,
};

#define TRACE_EVENT_TYPE_MAX \
    ((1 << (sizeof(((struct sched_wakeup *)0)->common_type) * 8)) - 1)

enum {
    KPROBE = TRACE_EVENT_TYPE_MAX + 1,
    KRETPROBE,
    UPROBE,
    URETPROBE,
};
extern int kprobe_type;
extern int uprobe_type;


static inline bool tp_is_dev(struct tp *tp)
{
    return tp->id < 0;
}

#define for_each_tp(_list, _tp, i) \
    for (i = 0, (_tp) = &(_list)->tp[i]; i < (_list)->nr_tp; \
         i++, (_tp) = &(_list)->tp[i])

#define for_each_real_tp(_list, _tp, i) \
    for (i = 0, (_tp) = &(_list)->tp[i]; i < (_list)->nr_tp; \
         i++, (_tp) = &(_list)->tp[i]) \
         if (!tp_is_dev(_tp))

#define for_each_dev_tp(_list, _tp, i) \
    for (i = 0, (_tp) = &(_list)->tp[i]; i < (_list)->nr_tp; \
         i++, (_tp) = &(_list)->tp[i]) \
         if (tp_is_dev(_tp))


struct tp_list *tp_list_new(struct prof_dev *dev, char *event_str);
void tp_list_free(struct tp_list *tp_list);
void tp_update_filter(struct tp *tp, const char *filter);
static inline bool tp_kernel(struct tp *tp)
{
    return tp->kernel;
}
void tp_print_marker(struct tp *tp);

#include <event-spread.h>

struct expr_prog *tp_new_prog(struct tp *tp, char *expr_str);
long tp_prog_run(struct tp *tp, struct expr_prog *prog, struct expr_global *global);
int tp_set_key(struct tp *tp, const char *key);
long tp_print_key(struct tp *tp, u64 key);
char *tp_get_comm(struct tp *tp, struct expr_global *global);
void *tp_get_mem_ptr(struct tp *tp, struct expr_global *global);
unsigned long tp_get_mem_size(struct tp *tp, struct expr_global *global);
unsigned long tp_get_key(struct tp *tp, struct expr_global *global);
unsigned long tp_get_role(struct tp *tp, struct expr_global *global);
unsigned long tp_get_num(struct tp *tp, struct expr_global *global);
void tp_print_event(struct tp *tp, unsigned long long ts, int cpu, void *data, int size);

struct perf_evsel *tp_evsel_new(struct tp *tp, struct perf_event_attr *tmpl);
struct tp *tp_from_evsel(struct perf_evsel *evsel, struct tp_list *tp_list);
int tp_list_apply_filter(struct prof_dev *dev, struct tp_list *tp_list);
long tp_list_ftrace_filter(struct prof_dev *dev, struct tp_list *tp_list, struct expr_global *global);


/*
 * Each tp is used to determine whether it is related to the specified cpu/pid.
 *
 * If tp is a forwarding source device, @raw, @size specify the `perf_record_dev'
 * structure and size, and the forwarding device determines whether @raw is related
 * to the specified cpu/pid.
 * If not, tp is used for kernel events, @raw, @size specifies the tracepoint
 * structure and size. Specify PERF_SAMPLE_RAW to sample the tracepoint structure.
 *
 * Only tp itself knows whether there are fields in its structure that are the
 * same as the specified cpu/pid.
 */
struct tp_matcher {
    struct list_head link;
    const char *sys;
    const char *name;
    bool (*samecpu)(struct tp *tp, void *raw, int size, int cpu);
    bool (*samepid)(struct tp *tp, void *raw, int size, int pid);
    bool (*target_cpu)(struct tp *tp, void *raw, int size, int cpu, int pid,
                       int *target_cpu, const char **reason);
    bool (*oncpu)(struct tp *tp, void *raw, int size,
                        int *pid, const char **prev_comm);
};

#define __TP_MATCHER_REGISTER(SYS, NAME, SAMECPU, SAMEPID, TARGET_CPU, ONCPU) \
__ctor \
static void __PASTE(tp_matcher_register_, __LINE__) (void) \
{ \
    static struct tp_matcher tp_matcher = { \
        .sys = SYS, \
        .name = NAME, \
        .samecpu = SAMECPU, \
        .samepid = SAMEPID, \
        .target_cpu = TARGET_CPU, \
        .oncpu = ONCPU, \
    }; \
    tp_matcher_register(&tp_matcher); \
}

#define TP_MATCHER_REGISTER(a,b,c,d) __TP_MATCHER_REGISTER((a),(b),(c),(d),NULL,NULL)
#define TP_MATCHER_REGISTER5(a,b,c,d,e) __TP_MATCHER_REGISTER((a),(b),(c),(d),(e),NULL)
#define TP_MATCHER_REGISTER6(a,b,c,d,e,f) __TP_MATCHER_REGISTER((a),(b),(c),(d),(e),(f))


void tp_matcher_register(struct tp_matcher *matcher);
struct tp_matcher *tp_matcher_find(struct tp *tp, const char *sys, const char *name);


static inline bool tp_matcher_samecpu(struct tp_matcher *matcher, struct tp *tp, void *raw, int size, int cpu)
{
    return matcher && matcher->samecpu ?
           matcher->samecpu(tp, raw, size, cpu) : false;
}

static inline bool tp_matcher_samepid(struct tp_matcher *matcher, struct tp *tp, void *raw, int size, int pid)
{
    return matcher && matcher->samepid ?
           matcher->samepid(tp, raw, size, pid) : false;
}

static inline bool tp_matcher_target_cpu(struct tp_matcher *matcher, struct tp *tp, void *raw, int size, int cpu, int pid,
                                                 int *target_cpu, const char **reason)
{
    return matcher && matcher->target_cpu ?
           matcher->target_cpu(tp, raw, size, cpu, pid, target_cpu, reason) : false;
}

static inline bool tp_matcher_oncpu(struct tp_matcher *matcher, struct tp *tp, void *raw, int size,
                                                 int *pid, const char **prev_comm)
{
    return matcher && matcher->oncpu ?
           matcher->oncpu(tp, raw, size, pid, prev_comm) : false;
}

static inline bool tp_samecpu(struct tp *tp, void *raw, int size, int cpu)
{
    return tp_matcher_samecpu(tp->matcher, tp, raw, size, cpu);
}

static inline bool tp_samepid(struct tp *tp, void *raw, int size, int pid)
{
    return tp_matcher_samepid(tp->matcher, tp, raw, size, pid);
}

// From the tracepoint(@raw, @size, @cpu), get the @target_cpu and @reason where the @pid may survive.
static inline bool tp_target_cpu(struct tp *tp, void *raw, int size, int cpu, int pid, int *target_cpu, const char **reason)
{
    return tp_matcher_target_cpu(tp->matcher, tp, raw, size, cpu, pid, target_cpu, reason);
}

// From the tracepoint(@raw, @size), get the current @pid and the previous @prev_comm.
static inline bool tp_oncpu(struct tp *tp, void *raw, int size, int *pid, const char **prev_comm)
{
    return tp_matcher_oncpu(tp->matcher, tp, raw, size, pid, prev_comm);
}

#endif

