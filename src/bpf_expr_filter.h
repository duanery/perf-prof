#ifndef __BPF_EXPR_FILTER_H
#define __BPF_EXPR_FILTER_H

#ifdef CONFIG_LIBBPF

struct bpf_object;
struct global_var_declare;

/*
 * Kernel-side expression filter for a BPF event source. See
 * src/bpf_expr_filter.c, and src/bpf-skel/expr_filter.bpf.h for what the
 * event source must define.
 */

/*
 * Compile `expr_str' against the event struct and install it in place of the
 * expr_filter() placeholder. Must be called between bpf_object__open() and
 * bpf_object__load(). Returns 0 on success, -1 on failure.
 */
int bpf_expr_filter_apply(struct bpf_object *obj, const char *expr_str, int verbose);

/* Field declarations of the event struct, read from BTF; malloc'd, NULL-name
 * terminated. Mainly useful for diagnostics, apply() calls it itself. */
struct global_var_declare *bpf_expr_filter_fields(struct bpf_object *obj);

/* BTF id of the event struct in bpf_object__btf(obj), or -1. */
int bpf_expr_filter_event_id(struct bpf_object *obj);

/* Print the fields an expression may refer to. */
void bpf_expr_filter_help(struct bpf_object *obj);

#endif /* CONFIG_LIBBPF */

#endif