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
#include <perf/evsel.h>
#include <perf/cpumap.h>
#include <perf/threadmap.h>
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

struct bpf_event {
    struct bpf_event_source *src;
    struct bpf_event_scope scope;       /* own copy: cpus referenced, tgids copied */
    struct perf_evsel *evsel;           /* not owned, see bpf_event_new() */
    void *s;                            /* the skeleton, once started */
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

/* ---- instances ---- */

struct bpf_event *bpf_event_new(struct bpf_event_source *src, const struct bpf_event_scope *scope,
                                const struct perf_event_attr *tmpl)
{
    struct perf_event_attr attr = *tmpl;
    struct perf_cpu_map *cpus = NULL;
    struct perf_thread_map *threads = NULL;
    struct bpf_event *ev;

    if (source_parse(src) < 0)
        return NULL;
    if (scope->type == BPF_EVENT_SCOPE_PROCESS && !(src->ops && src->ops->prepare)) {
        fprintf(stderr, "bpf:%s: can only trace cpus, not processes\n", src->name);
        return NULL;
    }
    /*
     * Kernel constraint: a BPF output sample has period 0 and does not count
     * the event, bpf_perf_event_output() in kernel/trace/bpf_trace.c inits it
     * with perf_sample_data_init(sd, 0, 0), already in a43eec304259 ("bpf:
     * introduce bpf_perf_event_output() helper"), in v4.4. Counting it would
     * always read 0, so only sampling is allowed.
     */
    if (!attr.freq && attr.sample_period == 0) {
        fprintf(stderr, "bpf:%s: can only be sampled, not counted\n", src->name);
        return NULL;
    }

    ev = calloc(1, sizeof(*ev));
    if (!ev)
        return NULL;
    ev->src = src;
    ev->scope.type = scope->type;
    if (scope->type == BPF_EVENT_SCOPE_PROCESS) {
        ev->scope.tgids = malloc(scope->nr_tgids * sizeof(*scope->tgids));
        if (!ev->scope.tgids)
            goto failed;
        memcpy((int *)ev->scope.tgids, scope->tgids, scope->nr_tgids * sizeof(*scope->tgids));
        ev->scope.nr_tgids = scope->nr_tgids;
    }

    /*
     * Kernel constraint: a perf event stored in a PERF_EVENT_ARRAY must not
     * be inherited, perf_event_fd_array_get_ptr() in kernel/bpf/arraymap.c:
     * the map update fails with -EINVAL since 62544ce8e01c ("bpf: fix
     * bpf_perf_event_read() helper"), in v4.4, and with -EOPNOTSUPP, through
     * perf_event_read_local(), since f91840a32dee ("perf, bpf: Add BPF support
     * to all perf_event types"), in v4.13.
     */
    attr.type = PERF_TYPE_SOFTWARE;
    attr.config = PERF_COUNT_SW_BPF_OUTPUT;
    attr.inherit = 0;

    /*
     * Kernel constraint: __bpf_perf_event_output() in kernel/trace/bpf_trace.c
     * outputs to the map entry of the current cpu, and drops the event if
     * that is empty (-ENOENT) or holds an event of another cpu (-EOPNOTSUPP),
     * already in a43eec304259 ("bpf: introduce bpf_perf_event_output()
     * helper"), in v4.4. So the evsel is opened per cpu, for no task, on every
     * cpu an event may happen on: the traced ones, or all of them when tracing
     * processes, which run anywhere. Events on other cpus are lost.
     */
    cpus = scope->type == BPF_EVENT_SCOPE_CPU ? perf_cpu_map__get(scope->cpus) : perf_cpu_map__new(NULL);
    threads = perf_thread_map__new_dummy();
    if (!cpus || !threads)
        goto failed;
    ev->evsel = perf_evsel__new(&attr);
    if (!ev->evsel)
        goto failed;
    perf_evsel__set_own_cpus(ev->evsel, cpus);
    perf_evsel__set_own_threads(ev->evsel, threads);
    if (scope->type == BPF_EVENT_SCOPE_CPU)
        ev->scope.cpus = cpus;
    else
        perf_cpu_map__put(cpus);
    perf_thread_map__put(threads);
    return ev;

failed:
    perf_cpu_map__put(cpus);
    perf_thread_map__put(threads);
    free((int *)ev->scope.tgids);
    free(ev);
    return NULL;
}

struct perf_evsel *bpf_event_evsel(struct bpf_event *ev)
{
    return ev->evsel;
}

static int quiet_print(enum libbpf_print_level level, const char *format, va_list args)
{
    return 0;
}

enum {
    STEP_OK,
    STEP_FILTER,    /* failed, and @filter may be why */
    STEP_FAILED,
};

/* open -> filter -> prepare -> load. The skeleton is in ev->s on STEP_OK only. */
static int bpf_event_load(struct bpf_event *ev, const char *filter, bool quiet, int verbose)
{
    struct bpf_event_source *src = ev->src;
    libbpf_print_fn_t old = NULL;
    int err;

    ev->s = src->skel->open();
    if (!ev->s) {
        fprintf(stderr, "bpf:%s: failed to open the BPF object\n", src->name);
        return STEP_FAILED;
    }
    /* The fallback without the filter follows, the error of this try only distracts. */
    if (quiet && !verbose)
        old = libbpf_set_print(quiet_print);

    err = STEP_FILTER;
    if (filter && bpf_expr_filter_apply(src->skel->obj(ev->s), filter, verbose) < 0)
        goto failed;

    err = STEP_FAILED;
    if (src->ops && src->ops->prepare) {
        int ret = src->ops->prepare(ev->s, &ev->scope);
        if (ret < 0) {
            if (ret == -EOPNOTSUPP)
                fprintf(stderr, "bpf:%s: cannot trace %s\n", src->name,
                        ev->scope.type == BPF_EVENT_SCOPE_CPU ? "cpus" : "these processes");
            else
                fprintf(stderr, "bpf:%s: failed to prepare the BPF object\n", src->name);
            goto failed;
        }
    }

    if (src->skel->load(ev->s) < 0) {
        err = filter ? STEP_FILTER : STEP_FAILED;
        /*
         * Kernel constraint: a source needs bpf-to-bpf calls (expr_filter() is
         * a subprog), since cc8b0b92a169 ("bpf: introduce function calls
         * (function boundaries)"), in v4.16, and global data in .bss/.rodata,
         * since d8eca5bbb2be ("bpf: implement lookup-free direct value access
         * for maps"), in v5.2. A source using CO-RE also needs the kernel's
         * BTF, which libbpf looks for in several places. The verifier log
         * alone does not say this.
         */
        if (!quiet || verbose)
            fprintf(stderr, "bpf:%s: failed to load the BPF object, may need a newer kernel or kernel BTF\n",
                    src->name);
        goto failed;
    }
    if (old)
        libbpf_set_print(old);
    return STEP_OK;

failed:
    if (old)
        libbpf_set_print(old);
    src->skel->destroy(ev->s);
    ev->s = NULL;
    return err;
}

/*
 * Point the perf_events map at the evsel's fds.
 *
 * Kernel constraint: BPF_F_CURRENT_CPU indexes the map with
 * smp_processor_id(), __bpf_perf_event_output() in kernel/trace/bpf_trace.c,
 * since 1e33759c788c ("bpf, trace: add BPF_F_CURRENT_CPU flag for
 * bpf_perf_event_output"), in v4.7. The key is the cpu number, not an index
 * into the cpu map: they differ once the cpus are not 0..n-1. A map without
 * max_entries is sized by libbpf to the possible cpus.
 */
static int bpf_event_bind_outputs(struct bpf_event *ev)
{
    struct bpf_map *map = ev->src->skel->perf_events(ev->s);
    struct perf_evsel_fd *pos;

    perf_evsel__for_each_fd(ev->evsel, pos) {
        int cpu = perf_evsel_fd__cpu(pos);
        int fd = perf_evsel__fd(ev->evsel, cpu, perf_evsel_fd__tid(pos));
        int err = bpf_map__update_elem(map, &cpu, sizeof(cpu), &fd, sizeof(fd), 0);

        if (err) {
            fprintf(stderr, "bpf:%s: failed to bind the output on cpu %d: %s\n",
                    ev->src->name, cpu, strerror(-err));
            return err;
        }
    }
    return 0;
}

int bpf_event_start(struct bpf_event *ev, const char *filter, int flags, int verbose)
{
    struct bpf_event_source *src = ev->src;
    bool userspace_ok = flags & BPF_EVENT_USERSPACE_OK;
    int ret = 0, err;

    if (ev->s)
        return -EBUSY;
    if (!perf_evsel__next_fd(ev->evsel, NULL))
        return -EINVAL;
    if (filter && !filter[0])
        filter = NULL;

    err = bpf_event_load(ev, filter, userspace_ok, verbose);
    if (err == STEP_FILTER && userspace_ok) {
        if (verbose)
            fprintf(stderr, "bpf:%s: the filter cannot run in the kernel, retrying without it\n", src->name);
        err = bpf_event_load(ev, NULL, false, verbose);
        ret = 1;
    }
    if (err != STEP_OK)
        return -EINVAL;

    err = bpf_event_bind_outputs(ev);
    if (!err) {
        err = src->skel->attach(ev->s);
        if (err)
            fprintf(stderr, "bpf:%s: failed to attach: %s\n", src->name, strerror(-err));
    }
    if (err) {
        src->skel->destroy(ev->s);
        ev->s = NULL;
        return err < 0 ? err : -EINVAL;
    }
    return ret;
}

void *bpf_event_skel(struct bpf_event *ev)
{
    return ev->s;
}

/*
 * Kernel constraint: a perf event in a PERF_EVENT_ARRAY is held by a
 * reference to its file, and the entries a map file added are removed when it
 * is released, perf_event_fd_array_release() in kernel/bpf/arraymap.c, since
 * 3b1efb196eee ("bpf, maps: flush own entries on perf map release"), in v4.8
 * (before, when the map is freed). So the BPF object and the evsel's fds can
 * go in either order.
 */
void bpf_event_free(struct bpf_event *ev)
{
    if (!ev)
        return;
    if (ev->s)
        ev->src->skel->destroy(ev->s);
    perf_cpu_map__put(ev->scope.cpus);
    free((int *)ev->scope.tgids);
    free(ev);
}

#endif
