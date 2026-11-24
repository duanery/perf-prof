#ifndef __BPF_EVENT_H
#define __BPF_EVENT_H

/*
 * BPF event sources: each src/bpf-skel/xxx.bpf.c listed as 'xxx.event.o' in
 * its Build file becomes the event `bpf:xxx', usable with -e like a
 * tracepoint. See src/bpf_event.c for the conventions a source follows.
 *
 * This module knows the BPF side only: the registry, the event struct read
 * from BTF, and (per instance) the BPF object and its PERF_COUNT_SW_BPF_OUTPUT
 * evsel. How a source becomes a struct tp, and when an instance starts, is
 * the business of tep.c and monitor.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <linux/compiler.h>     /* __PASTE */
#include <linux/stringify.h>    /* __stringify */

#ifndef __ctor
#define __ctor __attribute__((constructor))
#endif

struct bpf_object;
struct perf_evsel;
struct perf_event_attr;
struct perf_cpu_map;
struct global_var_declare;
struct bpf_event_source;    /* a registered source, lives for the whole process */
struct bpf_event;           /* an instance: a BPF_OUTPUT evsel and, once started, a loaded BPF object */

/* ---- registration: used by bpf_event.tmpl.c and a source's own .c ---- */

/* Type-agnostic wrappers of a skeleton. @s is what open() returned (struct xxx_bpf *). */
struct bpf_event_skel {
    /* The functions of every skeleton. */
    void *(*open)(void);                    /* NULL: failed */
    struct bpf_object *(*obj)(void *s);
    int (*load)(void *s);                   /* 0 or negative errno */
    int (*attach)(void *s);
    void (*destroy)(void *s);

    /*
     * What only a BPF event source has, read from the members of struct
     * xxx_bpf: a source without them fails to compile in bpf_event.tmpl.c.
     */
    const char *(*desc)(void *s);           /* BPF_EVENT_DESC(), in .rodata */
    struct bpf_map *(*perf_events)(void *s);    /* the map the events are output through */
};

/* What to trace. Given by the caller; the instance decides its output binding from it. */
enum bpf_event_scope_type {
    BPF_EVENT_SCOPE_CPU,        /* everything on @cpus */
    BPF_EVENT_SCOPE_PROCESS,    /* all threads of the processes @tgids, on any cpu */
};
struct bpf_event_scope {
    enum bpf_event_scope_type type;
    struct perf_cpu_map *cpus;          /* SCOPE_CPU */
    const int *tgids;                   /* SCOPE_PROCESS: distinct tgids, nr_tgids >= 1 */
    int nr_tgids;
};

/* The userspace part of a source, attached by its own .c with BPF_EVENT_OPS(). Every member is optional. */
struct bpf_event_ops {
    /*
     * Configure the source for @scope: called once after every open, before
     * load. @s is what open() returned, to be cast to struct xxx_bpf * for
     * setting rodata, bss or autoload. Return 0 to go on; -EOPNOTSUPP when the
     * source cannot serve this scope (e.g. more than one process); any other
     * negative value on failure. Without prepare, a source supports
     * SCOPE_CPU only, and nothing is set.
     */
    int (*prepare)(void *s, const struct bpf_event_scope *scope);
    /*
     * Print the fields of one event (the header -- comm, pid, cpu, time -- is
     * printed by the caller), ending with a newline. For structs the generic
     * BTF printing cannot express, such as a stack the source sampled itself.
     * @size is the raw size, which may include padding added by the kernel.
     * NULL: print generically from BTF.
     */
    void (*print)(const void *data, int size);
};

void bpf_event_register(const char *name, const struct bpf_event_skel *skel);
/*
 * The ops come from another .c, and constructors run in no particular order:
 * they are kept aside and bound by name the first time the registry is
 * queried, which is also when ops naming no source are reported.
 */
void bpf_event_register_ops(const char *name, const struct bpf_event_ops *ops);

#define BPF_EVENT_REGISTER(name, skel) \
    __ctor static void __PASTE(bpf_event_register_, name)(void) \
    { bpf_event_register(__stringify(name), skel); }
#define BPF_EVENT_OPS(name, ops) \
    __ctor static void __PASTE(bpf_event_ops_, name)(void) \
    { bpf_event_register_ops(__stringify(name), ops); }

/* bpf_event_start() flags */
#define BPF_EVENT_USERSPACE_OK  0x1     /* may start without the filter if it cannot go into the kernel */

#ifdef CONFIG_LIBBPF

/* ---- sources ---- */
struct bpf_event_source *bpf_event_source_find(const char *name);   /* NULL: none */
/* Iterate in name order: @prev NULL gives the first, NULL ends. */
struct bpf_event_source *bpf_event_source_next(struct bpf_event_source *prev);
const char *bpf_event_source_name(struct bpf_event_source *src);

/*
 * The first of these opens the skeleton (no load, no root needed), reads the
 * event struct from BTF and the description, and caches them until the
 * process exits. A source breaking the conventions fails them all, and is
 * reported once.
 */
/* Fields an expression can use: a malloc'd copy, NULL-name terminated, for the caller to free(). NULL: failed. */
struct global_var_declare *bpf_event_source_fields(struct bpf_event_source *src);
int bpf_event_source_size(struct bpf_event_source *src);            /* sizeof the event; negative: failed */
const char *bpf_event_source_desc(struct bpf_event_source *src);    /* BPF_EVENT_DESC(); "" if failed */
/* Description, then every member's type, offset, size and sign, as a tracefs format does. */
void bpf_event_source_help(struct bpf_event_source *src);
/*
 * Print the fields of one event: ops->print if the source has it, otherwise
 * " a=1 b=2 comm=x\n" from BTF. Fields past @size are left out.
 */
void bpf_event_source_print(struct bpf_event_source *src, const void *data, int size);

/* ---- instances: new -> start -> free ---- */

/*
 * Create an instance and its BPF_OUTPUT evsel, from @tmpl with type/config
 * replaced and inherit cleared. The evsel has its own cpus and a dummy
 * thread: @scope->cpus for SCOPE_CPU, every online cpu for SCOPE_PROCESS.
 * @scope is copied. NULL on failure, reported: a SCOPE_PROCESS the source has
 * no prepare for, a counting @tmpl (sample_period 0), or no memory.
 *
 * The caller adds the evsel to its evlist, which deletes it; the instance
 * only remembers it. Its cpus and threads must be left alone.
 */
struct bpf_event *bpf_event_new(struct bpf_event_source *src, const struct bpf_event_scope *scope,
                                const struct perf_event_attr *tmpl);
struct perf_evsel *bpf_event_evsel(struct bpf_event *ev);

/*
 * Once the evsel is open: open the BPF object, install @filter, prepare,
 * load, point the perf_events map at the evsel's fd on each cpu and attach.
 * If @filter cannot go into the kernel (a construct the BPF backend lacks, or
 * the verifier rejecting the program with it) and BPF_EVENT_USERSPACE_OK is
 * given, start over without it.
 *   0: started, @filter (if any) runs in the kernel.
 *   1: started without @filter; only with USERSPACE_OK, the caller filters.
 *   negative errno: failed, reported, and the instance is not started.
 *     -EINVAL if the evsel is not open, -EBUSY if already started.
 */
int bpf_event_start(struct bpf_event *ev, const char *filter, int flags, int verbose);
/* For the source's own code: the skeleton (struct xxx_bpf *) once started, else NULL. */
void *bpf_event_skel(struct bpf_event *ev);
/* Detach and destroy, in any order with closing the evsel. NULL is a no-op. */
void bpf_event_free(struct bpf_event *ev);

#else /* !CONFIG_LIBBPF: no sources, the rest is unreachable */

static inline struct bpf_event_source *bpf_event_source_find(const char *name) { return NULL; }
static inline struct bpf_event_source *bpf_event_source_next(struct bpf_event_source *prev) { return NULL; }
static inline const char *bpf_event_source_name(struct bpf_event_source *src) { return NULL; }
static inline struct global_var_declare *bpf_event_source_fields(struct bpf_event_source *src) { return NULL; }
static inline int bpf_event_source_size(struct bpf_event_source *src) { return -1; }
static inline const char *bpf_event_source_desc(struct bpf_event_source *src) { return ""; }
static inline void bpf_event_source_help(struct bpf_event_source *src) { }
static inline void bpf_event_source_print(struct bpf_event_source *src, const void *data, int size) { }
static inline struct bpf_event *bpf_event_new(struct bpf_event_source *src, const struct bpf_event_scope *scope,
                                              const struct perf_event_attr *tmpl) { return NULL; }
static inline struct perf_evsel *bpf_event_evsel(struct bpf_event *ev) { return NULL; }
static inline int bpf_event_start(struct bpf_event *ev, const char *filter, int flags, int verbose) { return -EOPNOTSUPP; }
static inline void *bpf_event_skel(struct bpf_event *ev) { return NULL; }
static inline void bpf_event_free(struct bpf_event *ev) { }

#endif
#endif
