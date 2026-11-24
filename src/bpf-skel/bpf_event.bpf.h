#ifndef __BPF_EVENT_BPF_H
#define __BPF_EVENT_BPF_H

/*
 * Conventions of a BPF event source, shared by the BPF program and by
 * userspace (bpf_event.tmpl.c). See src/bpf_event.c for the whole contract.
 *
 * BPF_EVENT_DESC("...") gives the source its one-line description, shown by
 * `-e bpf:xxx help'; every source must have one. It is an ordinary .rodata
 * variable the program never reads, so it is a member of the skeleton's
 * rodata, and bpf_event.tmpl.c takes it from the opened, not loaded, object.
 * A section of its own would not be in the skeleton, and libbpf logs every
 * data section it does not recognise.
 */
#define BPF_EVENT_DESC_VAR  bpf_event_desc

#ifdef __BPF__
#define BPF_EVENT_DESC(desc) \
    const volatile char BPF_EVENT_DESC_VAR[] = desc
#endif

#endif
