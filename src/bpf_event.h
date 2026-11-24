#ifndef __BPF_EVENT_H
#define __BPF_EVENT_H

/*
 * BPF event sources: each src/bpf-skel/xxx.bpf.c listed as 'xxx.event.o' in
 * its Build file becomes the event `bpf:xxx', usable with -e like a
 * tracepoint. See src/bpf_event.c for the conventions a source follows.
 *
 * This module knows the BPF side only: the registry, the event struct read
 * from BTF, and (per instance) the BPF object and its PERF_COUNT_SW_BPF_OUTPUT
 * evsel. How a source becomes a struct tp is tep.c's business.
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
struct global_var_declare;
struct bpf_event_source;    /* a registered source, lives for the whole process */

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

/* The userspace part of a source, attached by its own .c with BPF_EVENT_OPS(). Every member is optional. */
struct bpf_event_ops {
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

#else /* !CONFIG_LIBBPF: no sources, the rest is unreachable */

static inline struct bpf_event_source *bpf_event_source_find(const char *name) { return NULL; }
static inline struct bpf_event_source *bpf_event_source_next(struct bpf_event_source *prev) { return NULL; }
static inline const char *bpf_event_source_name(struct bpf_event_source *src) { return NULL; }
static inline struct global_var_declare *bpf_event_source_fields(struct bpf_event_source *src) { return NULL; }
static inline int bpf_event_source_size(struct bpf_event_source *src) { return -1; }
static inline const char *bpf_event_source_desc(struct bpf_event_source *src) { return ""; }
static inline void bpf_event_source_help(struct bpf_event_source *src) { }
static inline void bpf_event_source_print(struct bpf_event_source *src, const void *data, int size) { }

#endif
#endif
