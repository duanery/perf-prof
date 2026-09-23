/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PERF_PROF_BINDINGS_H
#define PERF_PROF_BINDINGS_H

#include <linux/rbtree.h>
#include <linux/types.h>
#include <stddef.h>

/* Embed this first in a profiler's per-binding state. Nodes never move. */
struct prof_binding {
    struct rb_node rb;
    int cpu, tid;
};

struct prof_bindings {
    struct rb_root root;
    size_t size;
    unsigned int nr;
    void (*init)(void *state);
    void (*destroy)(void *state);
};

void *prof_binding_get(struct prof_bindings *bindings, int cpu, int tid);
void *prof_binding_find(struct prof_bindings *bindings, int cpu, int tid);
void prof_binding_remove(struct prof_bindings *bindings, struct prof_binding *binding);
void prof_bindings_remove_thread(struct prof_bindings *bindings, int tid);
void prof_bindings_exit(struct prof_bindings *bindings);

static inline u64 prof_binding_key(int cpu, int tid)
{
    return ((u64)(u32)cpu << 32) | (u32)tid;
}

static inline int prof_binding_cpu(u64 key) { return (int)(key >> 32); }
static inline int prof_binding_tid(u64 key) { return (int)(u32)key; }
static inline int prof_binding_id(u64 key)
{
    int cpu = prof_binding_cpu(key);
    return cpu >= 0 ? cpu : prof_binding_tid(key);
}

#define prof_bindings_for_each(bindings, state) \
    for (struct rb_node *_binding_rb = rb_first(&(bindings)->root); \
         _binding_rb && ((state) = (void *)rb_entry(_binding_rb, struct prof_binding, rb)); \
         _binding_rb = rb_next(_binding_rb))

#endif
