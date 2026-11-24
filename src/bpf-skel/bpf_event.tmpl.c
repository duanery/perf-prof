// SPDX-License-Identifier: GPL-2.0
/*
 * Registration glue of a BPF event source, compiled once per source.
 *
 * Never built on its own: build/Makefile.build compiles it as xxx.event.o
 * with -DBPF_EVENT_NAME=xxx, BPF_EVENT_SKEL naming the skeleton xxx.skel.h and,
 * if the source has a header xxx.h for its types, BPF_EVENT_TYPES naming it; so
 * listing 'xxx.event.o' in a Build file is all it takes to register the
 * source. It only wraps the skeleton in type-agnostic functions; everything
 * else about the source lives in src/bpf_event.c.
 */
#include <stdint.h>
#include <bpf_event.h>
#include <bpf-skel/bpf_event.bpf.h>
#ifdef BPF_EVENT_TYPES
#include BPF_EVENT_TYPES
#endif
#include BPF_EVENT_SKEL

/* kvm_exit_bpf__open() ... and struct kvm_exit_bpf, for BPF_EVENT_NAME=kvm_exit */
#define SKEL(x)   __PASTE(__PASTE(BPF_EVENT_NAME, _bpf), x)
#define SKEL_T    struct __PASTE(BPF_EVENT_NAME, _bpf)

static void *skel_open(void)
{
    return SKEL(__open)();
}

static struct bpf_object *skel_obj(void *s)
{
    return ((SKEL_T *)s)->obj;
}

static int skel_load(void *s)
{
    return SKEL(__load)(s);
}

static int skel_attach(void *s)
{
    return SKEL(__attach)(s);
}

static void skel_destroy(void *s)
{
    SKEL(__destroy)(s);
}

/*
 * The members every BPF event source must have: without BPF_EVENT_DESC() or
 * the perf_events map of perf_output.bpf.h, these do not compile.
 */
static const char *skel_desc(void *s)
{
    return (const char *)((SKEL_T *)s)->rodata->BPF_EVENT_DESC_VAR;
}

static struct bpf_map *skel_perf_events(void *s)
{
    return ((SKEL_T *)s)->maps.perf_events;
}

static const struct bpf_event_skel skel = {
    .open    = skel_open,
    .obj     = skel_obj,
    .load    = skel_load,
    .attach  = skel_attach,
    .destroy = skel_destroy,
    .desc    = skel_desc,
    .perf_events = skel_perf_events,
};
BPF_EVENT_REGISTER(BPF_EVENT_NAME, &skel);
