// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel-side expression filter for BPF event sources.
 *
 * A BPF program that emits events is an event source like a tracepoint, and
 * needs a filter layer of its own. This file owns everything BPF-specific
 * about that: locating the filter placeholder inside a bpf_object, recovering
 * the event struct's layout, and installing generated code.
 *
 * The division of labour is:
 *   expr.c  - compiles the expression and translates it to BPF instructions
 *             (expr_to_bpf(); it knows nothing about bpf_object or BTF)
 *   here    - everything about the BPF object and its BTF
 *
 * A BPF event source is expected to define exactly one event and one filter,
 * using DEFINE_EXPR_FILTER() from src/bpf-skel/expr_filter.bpf.h. The filter's
 * name comes from EXPR_FILTER_NAME in that same header: with one event per
 * program there is nothing to disambiguate, so the name is fixed rather than
 * configurable, and defined in one place that both sides include.
 *
 * The event layout is not hardcoded anywhere. It is read out of the object's
 * BTF by following the placeholder's own signature:
 *
 *   FUNC 'expr_filter' -> FUNC_PROTO -> PTR -> STRUCT -> members
 *
 * which is exactly why DEFINE_EXPR_FILTER() names the event type instead of
 * taking a void *.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <monitor.h>
#include <expr.h>

#ifdef CONFIG_LIBBPF

#include <linux/filter.h>
#include <bpf/libbpf.h>
#include <bpf/btf.h>
#include <bpf-skel/expr_filter.bpf.h>
#include "bpf_expr_filter.h"

/*
 * Resolve the event struct that the filter placeholder takes a pointer to.
 * Returns the STRUCT type, or NULL with a diagnostic; its BTF id goes to
 * `type_id' if that is non-NULL.
 *
 * `obj' is only needed to name the offending object in diagnostics; several
 * BPF event sources may be loaded at once, so a bare complaint about
 * expr_filter() would not say which one it came from.
 */
static const struct btf_type *
expr_filter_event_type(struct bpf_object *obj, struct btf *btf,
                       const char **type_name, __s32 *type_id)
{
    const char *objname = bpf_object__name(obj) ? : "(unknown)";
    const struct btf_type *t;
    const struct btf_member *param;
    __s32 id;

    id = btf__find_by_name_kind(btf, EXPR_FILTER_NAME, BTF_KIND_FUNC);
    if (id < 0) {
        fprintf(stderr, "bpf filter: %s: no BTF for '%s()'\n",
                objname, EXPR_FILTER_NAME);
        return NULL;
    }

    t = btf__type_by_id(btf, id);                 /* FUNC */
    t = t ? btf__type_by_id(btf, t->type) : NULL; /* FUNC_PROTO */
    if (!t || btf_vlen(t) != 1) {
        fprintf(stderr, "bpf filter: %s: '%s()' must take exactly one argument\n",
                objname, EXPR_FILTER_NAME);
        return NULL;
    }

    param = (const struct btf_member *)(t + 1);
    id = btf__resolve_type(btf, param->type);     /* strip cv/typedef */
    t = id < 0 ? NULL : btf__type_by_id(btf, id);
    if (!t || !btf_is_ptr(t)) {
        fprintf(stderr, "bpf filter: %s: '%s()' argument is not a pointer\n",
                objname, EXPR_FILTER_NAME);
        return NULL;
    }

    id = btf__resolve_type(btf, t->type);         /* pointee */
    t = id < 0 ? NULL : btf__type_by_id(btf, id);
    if (!t || !btf_is_struct(t)) {
        fprintf(stderr, "bpf filter: %s: '%s()' argument does not point to a struct\n",
                objname, EXPR_FILTER_NAME);
        return NULL;
    }

    if (type_name)
        *type_name = btf__name_by_offset(btf, t->name_off) ? : "(anon)";
    if (type_id)
        *type_id = id;
    return t;
}

/*
 * BTF id of the event struct, in bpf_object__btf(obj). For callers that need
 * more of the struct than bpf_expr_filter_fields() describes: the members it
 * skips, or the type names. Returns -1 with a diagnostic on failure.
 */
int bpf_expr_filter_event_id(struct bpf_object *obj)
{
    struct btf *btf = bpf_object__btf(obj);
    __s32 id;

    if (!btf) {
        fprintf(stderr, "bpf filter: %s: object has no BTF\n",
                bpf_object__name(obj) ? : "(unknown)");
        return -1;
    }
    if (!expr_filter_event_type(obj, btf, NULL, &id))
        return -1;
    return id;
}

/*
 * Build the global variable declarations for the event struct, in the form
 * expr_compile() expects. Scalar members and arrays of scalars are usable;
 * anything else is skipped, so an expression naming it fails to compile with
 * the usual "undefined variable" diagnostic.
 *
 * Returns a malloc'd array terminated by a NULL name, or NULL on failure.
 * The strings point into the BTF, which outlives the returned array's use.
 */
struct global_var_declare *bpf_expr_filter_fields(struct bpf_object *obj)
{
    struct global_var_declare *declare;
    const struct btf_type *st;
    const struct btf_member *m;
    const char *type_name = NULL;
    struct btf *btf;
    int i, n, vlen;

    btf = bpf_object__btf(obj);
    if (!btf) {
        fprintf(stderr, "bpf filter: %s: object has no BTF\n",
                bpf_object__name(obj) ? : "(unknown)");
        return NULL;
    }

    st = expr_filter_event_type(obj, btf, &type_name, NULL);
    if (!st)
        return NULL;

    vlen = btf_vlen(st);
    declare = calloc(vlen + 1, sizeof(*declare));
    if (!declare)
        return NULL;

    for (i = 0, n = 0, m = btf_members(st); i < vlen; i++, m++) {
        const struct btf_type *ft, *elem;
        __u32 bit_off = btf_member_bit_offset(st, i);
        __s64 size, elemsize;
        int fid;

        /* Bitfields have no byte offset to load from. */
        if (bit_off % 8 || btf_member_bitfield_size(st, i))
            continue;

        fid = btf__resolve_type(btf, m->type);
        ft = fid < 0 ? NULL : btf__type_by_id(btf, fid);
        size = btf__resolve_size(btf, m->type);
        if (!ft || size <= 0)
            continue;

        /*
         * An array of scalars is usable too: expr_compile() derives the
         * element type from size vs elementsize, so `comm[0]' resolves to a
         * load of one element. Describe it that way, as tep__event_fields()
         * does for a tracepoint's char[] fields.
         *
         * The subscript has to be something the verifier can bound. A constant
         * or a masked value (`comm[i & 15]') is fine; an unconstrained variable
         * is rejected at load time with "invalid access to map value". Note the
         * expression compiler does not range-check a constant subscript either,
         * so a deliberately out-of-range one reads adjacent event bytes rather
         * than failing -- same as the userspace VM.
         */
        elem = ft;
        elemsize = size;
        if (btf_is_array(ft)) {
            const struct btf_array *arr = btf_array(ft);
            int eid = btf__resolve_type(btf, arr->type);

            elem = eid < 0 ? NULL : btf__type_by_id(btf, eid);
            elemsize = btf__resolve_size(btf, arr->type);
            if (!elem || elemsize <= 0 || arr->nelems == 0)
                continue;
        }

        /* Scalars only: an integer or an enum. Pointers into kernel memory
         * would need bpf_probe_read(), and structs have no value semantics
         * the expression language can use. */
        if (!btf_is_int(elem) && !btf_is_enum(elem))
            continue;
        if (elemsize != 1 && elemsize != 2 && elemsize != 4 && elemsize != 8)
            continue;

        declare[n].name = btf__name_by_offset(btf, m->name_off);
        declare[n].offset = bit_off / 8;
        declare[n].size = size;
        declare[n].elementsize = elemsize;
        /* Enums are unsigned unless BTF says otherwise. */
        declare[n].is_unsigned = btf_is_enum(elem) ||
                                 !(btf_int_encoding(elem) & BTF_INT_SIGNED);
        n++;
    }

    return declare;
}

/*
 * Compile `expr_str' against the event struct and install it as the kernel-side
 * filter, replacing the expr_filter() placeholder.
 *
 * Must be called between bpf_object__open() and bpf_object__load(): the
 * placeholder is a subprogram, and libbpf copies it into its callers during
 * load, so this is the only window in which it can be rewritten. Because the
 * copy happens afterwards, the replacement may be a different length than the
 * placeholder.
 *
 * Returns 0 on success, -1 on failure (with a diagnostic printed).
 */
int bpf_expr_filter_apply(struct bpf_object *obj, const char *expr_str, int verbose)
{
    struct global_var_declare *declare = NULL;
    struct expr_prog *prog = NULL;
    struct bpf_program *sub;
    struct bpf_insn *insns = NULL;
    const char *objname;
    char *dup = NULL;
    int nr_insn = 0;
    int err = -1;

    if (!obj || !expr_str || !expr_str[0])
        return 0;

    objname = bpf_object__name(obj) ? : "(unknown)";

    sub = bpf_object__find_subprog_by_name(obj, EXPR_FILTER_NAME);
    if (!sub) {
        fprintf(stderr, "bpf filter: %s: event source does not define %s(), "
                        "so it cannot be filtered\n", objname, EXPR_FILTER_NAME);
        return -1;
    }

    declare = bpf_expr_filter_fields(obj);
    if (!declare)
        goto out;

    /* expr_compile() writes through its argument while tokenising. */
    dup = strdup(expr_str);
    if (!dup)
        goto out;

    prog = expr_compile_flags(dup, declare, EXPR_F_NO_SAMPLE_GLO);
    if (!prog)
        goto out;

    insns = expr_to_bpf(prog, &nr_insn);
    if (!insns)
        goto out;

    if (verbose) {
        expr_dump(prog);
        expr_bpf_dump(insns, nr_insn);
    }

    if (bpf_program__set_insns(sub, insns, nr_insn)) {
        fprintf(stderr, "bpf filter: %s: failed to install %d instructions\n",
                objname, nr_insn);
        goto out;
    }

    err = 0;

out:
    free(insns);
    if (prog)
        expr_destroy(prog);
    free(dup);
    free(declare);
    return err;
}

/*
 * Print the fields available to an expression, for `help'.
 */
void bpf_expr_filter_help(struct bpf_object *obj)
{
    struct global_var_declare *d, *declare;

    declare = bpf_expr_filter_fields(obj);
    if (!declare)
        return;

    printf("Available fields (%s):\n", bpf_object__name(obj) ? : "(unknown)");
    for (d = declare; d->name; d++) {
        /* Name the element type, not the whole field: for an array `size' is
         * the total length, and only `elementsize' says what a subscript
         * yields. */
        printf("    %s%s %s", d->is_unsigned ? "unsigned " : "",
               d->elementsize == 1 ? "char" : d->elementsize == 2 ? "short" :
               d->elementsize == 4 ? "int" : "long",
               d->name);
        if (d->size != d->elementsize)
            printf("[%d]", d->size / d->elementsize);
        printf("\n");
    }
    free(declare);
}

#endif /* CONFIG_LIBBPF */