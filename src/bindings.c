// SPDX-License-Identifier: GPL-2.0
#include <stdlib.h>
#include "bindings.h"

void *prof_binding_find(struct prof_bindings *bindings, int cpu, int tid)
{
    struct rb_node *rb = bindings->root.rb_node;
    u64 key = prof_binding_key(cpu, tid);

    while (rb) {
        struct prof_binding *binding = rb_entry(rb, struct prof_binding, rb);
        u64 other = prof_binding_key(binding->cpu, binding->tid);

        if (key == other)
            return binding;
        rb = key < other ? rb->rb_left : rb->rb_right;
    }
    return NULL;
}

void *prof_binding_get(struct prof_bindings *bindings, int cpu, int tid)
{
    struct rb_node **link = &bindings->root.rb_node, *parent = NULL;
    struct prof_binding *binding;
    u64 key = prof_binding_key(cpu, tid);

    while (*link) {
        u64 other;

        parent = *link;
        binding = rb_entry(parent, struct prof_binding, rb);
        other = prof_binding_key(binding->cpu, binding->tid);
        if (key == other)
            return binding;
        link = key < other ? &parent->rb_left : &parent->rb_right;
    }
    binding = calloc(1, bindings->size);
    if (!binding)
        return NULL;
    binding->cpu = cpu;
    binding->tid = tid;
    if (bindings->init)
        bindings->init(binding);
    rb_link_node(&binding->rb, parent, link);
    rb_insert_color(&binding->rb, &bindings->root);
    bindings->nr++;
    return binding;
}

void prof_binding_remove(struct prof_bindings *bindings, struct prof_binding *binding)
{
    rb_erase(&binding->rb, &bindings->root);
    bindings->nr--;
    if (bindings->destroy)
        bindings->destroy(binding);
    free(binding);
}

void prof_bindings_remove_thread(struct prof_bindings *bindings, int tid)
{
    struct rb_node *rb = rb_first(&bindings->root);

    while (rb) {
        struct prof_binding *binding = rb_entry(rb, struct prof_binding, rb);

        rb = rb_next(rb);
        if (binding->tid == tid)
            prof_binding_remove(bindings, binding);
    }
}

void prof_bindings_exit(struct prof_bindings *bindings)
{
    while (bindings->root.rb_node)
        prof_binding_remove(bindings, rb_entry(bindings->root.rb_node, struct prof_binding, rb));
}
