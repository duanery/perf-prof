// SPDX-License-Identifier: GPL-2.0
/*
 * The userspace part of the BPF event source bpf:sched_rundelay, see
 * src/bpf-skel/sched_rundelay.bpf.c: what to set for tracing a process.
 */
#include <errno.h>
#include <stdint.h>
#include <bpf_event.h>
#include <bpf-skel/sched_rundelay.h>

/* Only the struct is used here; the skeleton is opened by sched_rundelay.event.o. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <bpf-skel/sched_rundelay.skel.h>
#pragma GCC diagnostic pop

static int sched_rundelay_prepare(void *s, const struct bpf_event_scope *scope)
{
    struct sched_rundelay_bpf *obj = s;

    if (scope->type == BPF_EVENT_SCOPE_CPU)
        return 0;
    // One process fits the rodata; more would take a map.
    if (scope->nr_tgids > 1)
        return -EOPNOTSUPP;
    obj->rodata->filter_tgid = scope->tgids[0];
    return 0;
}

static const struct bpf_event_ops sched_rundelay_ops = {
    .prepare = sched_rundelay_prepare,
};
BPF_EVENT_OPS(sched_rundelay, &sched_rundelay_ops);
