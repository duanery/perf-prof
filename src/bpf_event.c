// SPDX-License-Identifier: GPL-2.0
/*
 * BPF event sources.
 *
 * Every src/bpf-skel/xxx.bpf.c listed as 'xxx.event.o' in src/bpf-skel/Build
 * is compiled into a skeleton, and bpf_event.tmpl.c registers that skeleton
 * here from a constructor. The source is then the event `bpf:xxx', which
 * tep.c turns into a struct tp like any tracepoint.
 *
 * What a source must follow (see also docs/bpf_event_filter.md):
 *
 *  - Output through the `perf_events' map (bpf-skel/perf_output.bpf.h), and
 *    always the whole event struct: an expression reads the raw data at BTF
 *    offsets without bounds checks.
 *  - Define its filter with DEFINE_EXPR_FILTER(struct T): T is the event, and
 *    the only place its layout is read from.
 *  - Members are scalars or arrays of scalars. Bitfields, nested structs and
 *    pointers are listed by help, but no expression can reach them.
 *  - tp/ programs must return 1, or the tracepoint stops reaching the other
 *    perf users; prefer raw_tp/.
 *  - Describe itself with BPF_EVENT_DESC("...") (bpf-skel/bpf_event.bpf.h).
 *
 * The perf_events map and the description are members of the skeleton, and
 * bpf_event.tmpl.c fails to compile without them.
 *
 * Everything here about a source is read from the opened skeleton, without
 * loading it, so list and help work unprivileged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <monitor.h>
#include <expr.h>
#include <bpf_event.h>

#ifdef CONFIG_LIBBPF

#include <bpf/libbpf.h>
#include <bpf/btf.h>
#include <bpf_expr_filter.h>
#include <bpf-skel/bpf_event.bpf.h>

struct bpf_event_source {
    struct bpf_event_source *next;      /* registry, sorted by name */
    const char *name;
    const struct bpf_event_skel *skel;
    const struct bpf_event_ops *ops;

    /* Parsed on first use, kept until exit. */
    bool parsed;
    int err;
    void *s;                            /* opened, never loaded: owns the BTF */
    struct btf *btf;
    int type_id;                        /* the event struct */
    int size;
    const char *desc;                   /* in the rodata of the opened object */
    struct global_var_declare *fields;
};

struct bpf_event_pending_ops {
    struct bpf_event_pending_ops *next;
    const char *name;
    const struct bpf_event_ops *ops;
};

static struct bpf_event_source *sources;
static struct bpf_event_pending_ops *pending_ops;

void bpf_event_register(const char *name, const struct bpf_event_skel *skel)
{
    struct bpf_event_source *src, **p;

    src = calloc(1, sizeof(*src));
    if (!src)
        return;
    src->name = name;
    src->skel = skel;

    for (p = &sources; *p && strcmp((*p)->name, name) < 0; p = &(*p)->next)
        ;
    src->next = *p;
    *p = src;
}

void bpf_event_register_ops(const char *name, const struct bpf_event_ops *ops)
{
    struct bpf_event_pending_ops *pend = malloc(sizeof(*pend));

    if (!pend)
        return;
    pend->name = name;
    pend->ops = ops;
    pend->next = pending_ops;
    pending_ops = pend;
}

static struct bpf_event_source *source_lookup(const char *name)
{
    struct bpf_event_source *src;

    for (src = sources; src; src = src->next)
        if (strcmp(src->name, name) == 0)
            return src;
    return NULL;
}

/* All constructors have run by the time anyone queries the registry. */
static void bind_pending_ops(void)
{
    struct bpf_event_pending_ops *pend;
    struct bpf_event_source *src;

    while ((pend = pending_ops)) {
        pending_ops = pend->next;
        src = source_lookup(pend->name);
        if (src)
            src->ops = pend->ops;
        else
            fprintf(stderr, "bpf event: BPF_EVENT_OPS(%s) names no source\n", pend->name);
        free(pend);
    }
}

struct bpf_event_source *bpf_event_source_find(const char *name)
{
    bind_pending_ops();
    return source_lookup(name);
}

struct bpf_event_source *bpf_event_source_next(struct bpf_event_source *prev)
{
    bind_pending_ops();
    return prev ? prev->next : sources;
}

const char *bpf_event_source_name(struct bpf_event_source *src)
{
    return src->name;
}

static int source_parse(struct bpf_event_source *src)
{
    struct bpf_object *obj;

    if (src->parsed)
        return src->err;
    src->parsed = true;
    src->err = -1;

    src->s = src->skel->open();
    if (!src->s) {
        fprintf(stderr, "bpf:%s: failed to open the BPF object\n", src->name);
        return -1;
    }
    obj = src->skel->obj(src->s);
    src->desc = src->skel->desc(src->s);

    src->type_id = bpf_expr_filter_event_id(obj);
    if (src->type_id < 0)
        goto bad;
    src->btf = bpf_object__btf(obj);
    src->size = btf__resolve_size(src->btf, src->type_id);
    if (src->size <= 0)
        goto bad;
    src->fields = bpf_expr_filter_fields(obj);
    if (!src->fields)
        goto bad;
    src->err = 0;
    return 0;

bad:
    fprintf(stderr, "bpf:%s: cannot tell the event struct, see DEFINE_EXPR_FILTER()\n", src->name);
    return -1;
}

static __attribute__((destructor)) void bpf_event_sources_free(void)
{
    struct bpf_event_source *src;

    bind_pending_ops();
    while ((src = sources)) {
        sources = src->next;
        free(src->fields);
        if (src->s)
            src->skel->destroy(src->s);
        free(src);
    }
}

struct global_var_declare *bpf_event_source_fields(struct bpf_event_source *src)
{
    struct global_var_declare *fields;
    int n = 0;

    if (source_parse(src) < 0)
        return NULL;
    while (src->fields[n].name)
        n++;
    fields = malloc((n + 1) * sizeof(*fields));
    if (fields)
        memcpy(fields, src->fields, (n + 1) * sizeof(*fields));
    return fields;
}

int bpf_event_source_size(struct bpf_event_source *src)
{
    return source_parse(src) < 0 ? -1 : src->size;
}

const char *bpf_event_source_desc(struct bpf_event_source *src)
{
    return source_parse(src) < 0 || !src->desc ? "" : src->desc;
}

static bool field_accessible(struct bpf_event_source *src, const char *name)
{
    struct global_var_declare *f;

    for (f = src->fields; f->name; f++)
        if (strcmp(f->name, name) == 0)
            return true;
    return false;
}

static void help_printf(void *ctx, const char *fmt, va_list args)
{
    vprintf(fmt, args);
}

void bpf_event_source_help(struct bpf_event_source *src)
{
    const struct btf_type *st, *t;
    const struct btf_member *m;
    struct btf_dump *d;
    int i;

    if (source_parse(src) < 0)
        return;

    d = btf_dump__new(src->btf, help_printf, NULL, NULL);
    if (!d)
        return;

    st = btf__type_by_id(src->btf, src->type_id);
    printf("name: bpf:%s\n", src->name);
    if (src->desc && src->desc[0])
        printf("desc: %s\n", src->desc);
    printf("format: %s %s, size %d\n", btf_is_union(st) ? "union" : "struct",
           btf__name_by_offset(src->btf, st->name_off), src->size);

    for (i = 0, m = btf_members(st); i < btf_vlen(st); i++, m++) {
        DECLARE_LIBBPF_OPTS(btf_dump_emit_type_decl_opts, opts,
                            .field_name = btf__name_by_offset(src->btf, m->name_off));
        __u32 bit_off = btf_member_bit_offset(st, i);
        __u32 bit_size = btf_member_bitfield_size(st, i);
        int id = btf__resolve_type(src->btf, m->type);
        bool is_signed = false;

        t = id < 0 ? NULL : btf__type_by_id(src->btf, id);
        while (t && btf_is_array(t)) {
            id = btf__resolve_type(src->btf, btf_array(t)->type);
            t = id < 0 ? NULL : btf__type_by_id(src->btf, id);
        }
        if (t && btf_is_int(t))
            is_signed = btf_int_encoding(t) & BTF_INT_SIGNED;
        else if (t && btf_is_any_enum(t))
            is_signed = btf_kflag(t);

        printf("\tfield:");
        btf_dump__emit_type_decl(d, m->type, &opts);
        if (bit_size)
            printf(":%u;\toffset:%u.%u;\tbits:%u;", bit_size, bit_off / 8, bit_off % 8, bit_size);
        else
            printf(";\toffset:%u;\tsize:%lld;", bit_off / 8,
                   (long long)btf__resolve_size(src->btf, m->type));
        printf("\tsigned:%d;%s\n", is_signed,
               field_accessible(src, opts.field_name) ? "" : "\t(not accessible)");
    }
    btf_dump__free(d);
}

static void print_scalar(struct bpf_event_source *src, const struct btf_type *t,
                         const void *p, int size)
{
    bool is_signed = btf_is_int(t) ? !!(btf_int_encoding(t) & BTF_INT_SIGNED) : false;
    unsigned long long u = 0;
    long long v = 0;

    switch (size) {
    case 1: u = *(const __u8 *)p;  v = *(const __s8 *)p;  break;
    case 2: u = *(const __u16 *)p; v = *(const __s16 *)p; break;
    case 4: u = *(const __u32 *)p; v = *(const __s32 *)p; break;
    case 8: u = *(const __u64 *)p; v = *(const __s64 *)p; break;
    default: break;
    }

    if (btf_is_enum(t)) {
        const struct btf_enum *e = btf_enum(t);
        int i;

        for (i = 0; i < btf_vlen(t); i++, e++)
            if ((__u32)e->val == (__u32)u) {
                printf("%s", btf__name_by_offset(src->btf, e->name_off));
                return;
            }
        is_signed = btf_kflag(t);
    }
    if (is_signed)
        printf("%lld", v);
    else
        printf("%llu", u);
}

static bool is_char(struct bpf_event_source *src, const struct btf_type *t)
{
    return btf_is_int(t) && t->size == 1 &&
           strcmp(btf__name_by_offset(src->btf, t->name_off), "char") == 0;
}

void bpf_event_source_print(struct bpf_event_source *src, const void *data, int size)
{
    const struct btf_type *st;
    const struct btf_member *m;
    int i;

    if (source_parse(src) < 0) {
        printf("\n");
        return;
    }
    if (src->ops && src->ops->print) {
        src->ops->print(data, size);
        return;
    }

    st = btf__type_by_id(src->btf, src->type_id);
    for (i = 0, m = btf_members(st); i < btf_vlen(st); i++, m++) {
        const char *name = btf__name_by_offset(src->btf, m->name_off);
        __u32 bit_off = btf_member_bit_offset(st, i);
        __s64 msize = btf__resolve_size(src->btf, m->type);
        const void *p = data + bit_off / 8;
        const struct btf_type *t, *elem;
        int id, esize, n, j;

        if (bit_off % 8 || btf_member_bitfield_size(st, i) || msize <= 0 ||
            bit_off / 8 + msize > size)
            continue;
        id = btf__resolve_type(src->btf, m->type);
        t = id < 0 ? NULL : btf__type_by_id(src->btf, id);
        if (!t)
            continue;

        if (btf_is_int(t) || btf_is_enum(t)) {
            if (msize != 1 && msize != 2 && msize != 4 && msize != 8)
                continue;
            printf(" %s=", name);
            print_scalar(src, t, p, msize);
            continue;
        }
        if (!btf_is_array(t))
            continue;

        id = btf__resolve_type(src->btf, btf_array(t)->type);
        elem = id < 0 ? NULL : btf__type_by_id(src->btf, id);
        esize = btf__resolve_size(src->btf, btf_array(t)->type);
        if (!elem || (!btf_is_int(elem) && !btf_is_enum(elem)) ||
            (esize != 1 && esize != 2 && esize != 4 && esize != 8))
            continue;
        n = msize / esize;
        if (is_char(src, elem)) {
            printf(" %s=%.*s", name, (int)strnlen(p, n), (const char *)p);
            continue;
        }
        printf(" %s={", name);
        for (j = 0; j < n; j++) {
            if (j)
                printf(",");
            print_scalar(src, elem, p + j * esize, esize);
        }
        printf("}");
    }
    printf("\n");
}

#endif
