#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <monitor.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/list.h>
#include <linux/rblist.h>
#include <linux/const.h>
#include <linux/kernel.h>
#include <monitor.h>
#include <tep.h>


#define MMU_MAX_LEVEL 5

#define SPTE_SET  1
#define SPTE_MMIO 2

struct kvm_mmu_page {
    struct rb_node rbnode;
    __u64 gfn;
    __u32 role;
    __u32 root_count;
    __u32 refcount;
    __u8 *spte_set; // __u8 [512]
};

struct kvm_mmu_gen {
    struct list_head list;
    unsigned long mmu_valid_gen;
    struct rblist kvm_mmu_pages[MMU_MAX_LEVEL];
    // stats
    __u64 kvm_mmu_get_page;
    __u64 kvm_mmu_get_page_created;
    __u64 kvm_mmu_prepare_zap_page;
    __u64 kvm_mmu_set_spte;
};

struct kvmmmu_ctx {
    __u64 kvm_mmu_get_page;
    __u64 kvm_mmu_prepare_zap_page;
    __u64 kvm_mmu_set_spte;
    __u64 mark_mmio_spte;
    int mmu_valid_gen_size;
    unsigned long current_valid_gen;
    struct list_head kvm_mmu_gen_list;
    struct tp_list *tp_list;
};

union kvm_mmu_page_role {
    u32 word;
    struct {
        unsigned level:4;
        unsigned gpte_is_8_bytes:1;
        unsigned quadrant:2;
        unsigned direct:1;
        unsigned access:3;
        unsigned invalid:1;
        unsigned nxe:1;
        unsigned cr0_wp:1;
        unsigned smep_andnot_wp:1;
        unsigned smap_andnot_wp:1;
        unsigned ad_disabled:1;
        unsigned guest_mode:1;
        unsigned :6;
        unsigned smm:8;
    };
};

struct kvm_mmu_set_spte {
    unsigned short common_type;//       offset:0;       size:2; signed:0;
    unsigned char common_flags;//       offset:2;       size:1; signed:0;
    unsigned char common_preempt_count;//       offset:3;       size:1; signed:0;
    int common_pid;//   offset:4;       size:4; signed:1;

    u64 gfn;//  offset:8;       size:8; signed:0;
    u64 spte;// offset:16;      size:8; signed:0;
    u64 sptep;//        offset:24;      size:8; signed:0;
    u8 level;// offset:32;      size:1; signed:0;
    bool r;//   offset:33;      size:1; signed:0;
    bool x;//   offset:34;      size:1; signed:0;
    u8 u;//     offset:35;      size:1; signed:0;
};

struct mark_mmio_spte {
    unsigned short common_type;//       offset:0;       size:2; signed:0;
    unsigned char common_flags;//       offset:2;       size:1; signed:0;
    unsigned char common_preempt_count;//       offset:3;       size:1; signed:0;
    int common_pid;//   offset:4;       size:4; signed:1;

    void * sptep;//     offset:8;       size:8; signed:0;
    u64 gfn;//        offset:16;      size:8; signed:0;
    unsigned access;//  offset:24;      size:4; signed:0;
    unsigned int gen;// offset:28;      size:4; signed:0;
};

struct kvm_mmu_get_page {
    unsigned short common_type;//       offset:0;       size:2; signed:0;
    unsigned char common_flags;//       offset:2;       size:1; signed:0;
    unsigned char common_preempt_count;//       offset:3;       size:1; signed:0;
    int common_pid;//   offset:4;       size:4; signed:1;

    unsigned long mmu_valid_gen;//       offset:8;       size:1; signed:0;
    __u64 gfn;//        offset:16;      size:8; signed:0;
    __u32 role;//       offset:24;      size:4; signed:0;
    __u32 root_count;// offset:28;      size:4; signed:0;
    bool unsync;//      offset:32;      size:1; signed:0;
    bool created;//     offset:33;      size:1; signed:0;
};

struct kvm_mmu_prepare_zap_page {
    unsigned short common_type;//       offset:0;       size:2; signed:0;
    unsigned char common_flags;//       offset:2;       size:1; signed:0;
    unsigned char common_preempt_count;//       offset:3;       size:1; signed:0;
    int common_pid;//   offset:4;       size:4; signed:1;

    unsigned long mmu_valid_gen;//      offset:8;       size:1; signed:0;
    __u64 gfn;//        offset:16;      size:8; signed:0;
    __u32 role;//       offset:24;      size:4; signed:0;
    __u32 root_count;// offset:28;      size:4; signed:0;
    bool unsync;//      offset:32;      size:1; signed:0;
};

// in linux/perf_event.h
// PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW
struct sample_type_raw {
    struct {
        __u32    pid;
        __u32    tid;
    }    tid_entry;
    __u64   time;
    struct {
        __u32    cpu;
        __u32    reserved;
    }    cpu_entry;
    struct {
        __u32   size;
        union {
            __u8    data[0];
            unsigned short common_type;
            struct kvm_mmu_set_spte kvm_mmu_set_spte;
            struct mark_mmio_spte mark_mmio_spte;
            struct kvm_mmu_get_page kvm_mmu_get_page;
            struct kvm_mmu_prepare_zap_page kvm_mmu_prepare_zap_page;
        };
    } __packed raw;
};

static int kvm_mmu_page_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct kvm_mmu_page *b = container_of(rbn, struct kvm_mmu_page, rbnode);
    const struct kvm_mmu_page *e = entry;

    if (b->gfn > e->gfn)
        return 1;
    else if (b->gfn < e->gfn)
        return -1;

    if (b->role > e->role)
        return 1;
    else if (b->role < e->role)
        return -1;

    return 0;
}

static int kvm_mmu_page_find(struct rb_node *rbn, const void *entry)
{
    struct kvm_mmu_page *b = container_of(rbn, struct kvm_mmu_page, rbnode);
    const __u64 gfn = (__u64)entry;
    union kvm_mmu_page_role role;
    __u64 gfn_end;

    role.word = b->role;
    gfn_end = b->gfn + (1UL << (role.level * 9)) - 1;

    if (gfn_end > gfn)
        return 1;
    else if (b->gfn < gfn)
        return -1;
    else
        return 0;
}

static struct rb_node *kvm_mmu_page_node_new(struct rblist *rlist, const void *new_entry)
{
    const struct kvm_mmu_page *e = new_entry;
    struct kvm_mmu_page *b = malloc(sizeof(*b));
    if (b) {
        RB_CLEAR_NODE(&b->rbnode);
        b->gfn = e->gfn;
        b->role = e->role;
        b->root_count = e->root_count;
        b->refcount = 1;
        b->spte_set = NULL;
        return &b->rbnode;
    } else
        return NULL;
}

static void kvm_mmu_page_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct kvm_mmu_page *b = container_of(rb_node, struct kvm_mmu_page, rbnode);
    if (b->spte_set)
        free(b->spte_set);
    free(b);
}

static int kvm_mmu_gen_get_page(struct kvmmmu_ctx *ctx, unsigned long mmu_valid_gen, struct kvm_mmu_page *entry, bool created)
{
    struct kvm_mmu_gen *mmu_gen;
    union kvm_mmu_page_role role;
    struct kvm_mmu_page *mmu_page;
    struct rb_node *rbn;
    struct rblist *rblist;
    int i, ret = 0;

    list_for_each_entry(mmu_gen, &ctx->kvm_mmu_gen_list, list) {
        if (mmu_gen->mmu_valid_gen == mmu_valid_gen)
            goto found;
    }

    // new kvm_mmu_gen
    mmu_gen = malloc(sizeof(*mmu_gen));
    if (!mmu_gen)
        return -ENOMEM;

    mmu_gen->mmu_valid_gen = mmu_valid_gen;
    for (i = 0; i < MMU_MAX_LEVEL; i++) {
        rblist = &mmu_gen->kvm_mmu_pages[i];
        rblist__init(rblist);
        rblist->node_cmp = kvm_mmu_page_node_cmp;
        rblist->node_new = kvm_mmu_page_node_new;
        rblist->node_delete = kvm_mmu_page_node_delete;
    }
    mmu_gen->kvm_mmu_get_page = 0;
    mmu_gen->kvm_mmu_get_page_created = 0;
    mmu_gen->kvm_mmu_prepare_zap_page = 0;
    mmu_gen->kvm_mmu_set_spte = 0;
    list_add(&mmu_gen->list, &ctx->kvm_mmu_gen_list);

found:
    role.word = entry->role;
    rblist = &mmu_gen->kvm_mmu_pages[role.level - 1];

    if (created) {
        ret = rblist__add_node(rblist, entry);
        if (ret == -EEXIST) {
            rbn = rblist__find(rblist, entry);
            if (rbn) {
                /*
                 * Duplicate mmu_page exist and usually don't.
                 * refcount is the number of duplications.
                **/
                mmu_page = rb_entry(rbn, struct kvm_mmu_page, rbnode);
                mmu_page->refcount ++;
                print_time(stderr);
                fprintf(stderr, "kvm_mmu_get_page EXIST gen %lu gfn %llu role %u root_count %u refcount %u\n",
                        mmu_valid_gen, mmu_page->gfn, mmu_page->role, mmu_page->root_count, mmu_page->refcount);
            }
        }
    } else if (entry->gfn != 0) {
        ret = -1;
    } else {
        rbn = rblist__find(rblist, entry);
        if (rbn) {
            mmu_page = rb_entry(rbn, struct kvm_mmu_page, rbnode);
            mmu_page->root_count = max(mmu_page->root_count, entry->root_count);
        } else {
            ret = -1;
        }
    }

    mmu_gen->kvm_mmu_get_page ++;
    if (created)
        mmu_gen->kvm_mmu_get_page_created ++;

    return ret;
}

static int kvm_mmu_gen_zap_page(struct kvmmmu_ctx *ctx, unsigned long mmu_valid_gen, struct kvm_mmu_page *entry)
{
    struct kvm_mmu_gen *mmu_gen;
    union kvm_mmu_page_role role;
    struct kvm_mmu_page *mmu_page;
    struct rb_node *rbn;
    struct rblist *rblist;
    int i, ret = 0;
    bool remove = false;

    list_for_each_entry(mmu_gen, &ctx->kvm_mmu_gen_list, list) {
        if (mmu_gen->mmu_valid_gen == mmu_valid_gen)
            goto found;
    }
    return -1;

found:
    role.word = entry->role;
    rblist = &mmu_gen->kvm_mmu_pages[role.level - 1];

    rbn = rblist__find(rblist, entry);
    if (rbn) {
        mmu_page = rb_entry(rbn, struct kvm_mmu_page, rbnode);

        if (!entry->root_count) {
            mmu_page->refcount --;
            if (mmu_page->refcount == 0)
                rblist__remove_node(rblist, rbn);
            remove = true;
        } else {
            /*
             * Refer to the kernel kvm __kvm_mmu_prepare_zap_page() implementation,
             * the root kvm_mmu_page first sets invalid=1, and then waits for
             * root_count=0 to be released.
            **/
            role.word = mmu_page->role;
            role.invalid = 1;
            if (mmu_page->refcount == 1) {
                mmu_page->role = role.word;
            } else {
                /*
                 * Duplicate root mmu_page exist.
                 * To adjust the role to invalid, split it into 2 mmu_pages.
                 * One is invalid, refcount=1. One is valid, refcount--.
                **/
                mmu_page->refcount --;
                entry->gfn = mmu_page->gfn;
                entry->role = role.word;
                entry->root_count = mmu_page->root_count;
                ret = rblist__add_node(rblist, entry);
                if (ret == -EEXIST) {
                    print_time(stderr);
                    fprintf(stderr, "kvm_mmu_get_page EXIST gen %lu gfn %llu role %u root_count %u refcount %u\n",
                        mmu_valid_gen, mmu_page->gfn, mmu_page->role, mmu_page->root_count, mmu_page->refcount);
                }
            }
        }
    } else {
        print_time(stderr);
        fprintf(stderr, "kvm_mmu_prepare_zap_page not EXIST gfn 0x%llx\n", entry->gfn);
        ret = -1;
    }

    if (remove)
        mmu_gen->kvm_mmu_prepare_zap_page ++;

    if (remove && rblist__nr_entries(rblist) == 0) {
        for (i = 0; i < MMU_MAX_LEVEL; i++) {
            rblist = &mmu_gen->kvm_mmu_pages[i];
            if (rblist__nr_entries(rblist) != 0)
                break;
        }
        if (i == MMU_MAX_LEVEL) {
            print_time(stdout);
            printf("kvm_mmu_gen %lu obsoleted get_page %llu created %llu zap_page %llu set_spte %llu\n",
                    mmu_gen->mmu_valid_gen,
                    mmu_gen->kvm_mmu_get_page,
                    mmu_gen->kvm_mmu_get_page_created,
                    mmu_gen->kvm_mmu_prepare_zap_page,
                    mmu_gen->kvm_mmu_set_spte);
            list_del(&mmu_gen->list);
            free(mmu_gen);
        }
    }

    return ret;
}

static int kvm_mmu_gen_set_spte(struct kvmmmu_ctx *ctx, unsigned long mmu_valid_gen, u64 gfn, u8 level, u8 flags)
{
    struct kvm_mmu_gen *mmu_gen;
    union kvm_mmu_page_role role;
    struct kvm_mmu_page *mmu_page;
    struct rb_node *rbn;
    struct rblist *rblist;
    __u64 gfn_end;
    int bit;

    list_for_each_entry(mmu_gen, &ctx->kvm_mmu_gen_list, list) {
        if (mmu_gen->mmu_valid_gen == mmu_valid_gen)
            goto found;
    }
    return -1;

found:
    rblist = &mmu_gen->kvm_mmu_pages[level - 1];

    rblist->node_cmp = kvm_mmu_page_find;
    rbn = rblist__find_first(rblist, (const void *)gfn);
    rblist->node_cmp = kvm_mmu_page_node_cmp;
    if (!rbn)
        return -1;

    mmu_page = rb_entry(rbn, struct kvm_mmu_page, rbnode);
    role.word = mmu_page->role;
    gfn_end = mmu_page->gfn + (1UL << (role.level * 9)) - 1;

    if (role.level != level ||
        gfn < mmu_page->gfn ||
        gfn > gfn_end)
        return -1;

    bit = (gfn - mmu_page->gfn) >> ((role.level-1)*9);
    if (bit >= 512)
        return -1;

    if (mmu_page->spte_set == NULL) {
        mmu_page->spte_set = calloc(512, sizeof(__u8));
        if (mmu_page->spte_set == NULL)
            return -1;
    }

    mmu_gen->kvm_mmu_set_spte ++;

    mmu_page->spte_set[bit] |= flags;
    return 0;
}

static void kvm_mmu_print_role(__u32 word)
{
    static const char *access_str[] = {
        "---", "--x", "w--", "w-x", "-u-", "-ux", "wu-", "wux"
    };
    union kvm_mmu_page_role role;

    role.word = word;
    printf(" ; l%u q%u%s %s%s %snxe %swp%s%s%s%s",
                 role.level,
                 role.quadrant,
                 role.direct ? " direct" : "",
                 access_str[role.access],
                 role.invalid ? " invalid" : "",
                 role.nxe ? "" : "!",
                 role.cr0_wp ? "" : "!",
                 role.smep_andnot_wp ? " smep" : "",
                 role.smap_andnot_wp ? " smap" : "",
                 role.ad_disabled ? " !ad" : "",
                 role.smm ? " smm" : "");
}

static void kvm_mmu_gen_dump(struct kvm_mmu_gen *mmu_gen, int level, bool root, __u32 role_mask, __u64 gfn_start, __u64 gfn_end)
{
    struct rblist *rblist;
    struct rb_node *node;
    struct kvm_mmu_page *mmu_page;
    union kvm_mmu_page_role role;
    __u64 __gfn_end;
    int l;

    if (level < 1)
        return;

    rblist = &mmu_gen->kvm_mmu_pages[level - 1];

    if (rblist__empty(rblist))
        return;

    rblist->node_cmp = kvm_mmu_page_find;
    node = rblist__find_first(rblist, (const void *)gfn_start);
    rblist->node_cmp = kvm_mmu_page_node_cmp;

    for (; node; node = rb_next(node)) {
        mmu_page = rb_entry(node, struct kvm_mmu_page, rbnode);
        role.word = mmu_page->role;

        // The same role belongs to a set of page tables.
        if (!root &&
            (role.word ^ role_mask) != role.level)
            continue;

        __gfn_end = mmu_page->gfn + (1UL << (role.level * 9)) - 1;
        if (__gfn_end > gfn_end)
            return;

        for (l = 0; l < MMU_MAX_LEVEL-level; l++)
            printf("    ");
        printf("sp%d 0x%08llx-0x%08llx role 0x%04x", level, mmu_page->gfn, __gfn_end, mmu_page->role);
        if (mmu_page->root_count)
            printf(" root_count %d", mmu_page->root_count);
        if (mmu_page->refcount > 1)
            printf(" ref %d", mmu_page->refcount);
        kvm_mmu_print_role(mmu_page->role);
        printf("\n");

        if (level > 1) {
            if (root) {
                role_mask = role.word ^ role.level;
            }
            kvm_mmu_gen_dump(mmu_gen, level-1, false, role_mask, mmu_page->gfn, __gfn_end);
        }

        if (mmu_page->spte_set) {
            __u64 start, end;
            int i, e;
            __u8 flags;
            for (i = 0; i < 512; i++) {
                if (mmu_page->spte_set[i]) {
                    flags = mmu_page->spte_set[i];
                    e = i;
                    while (++e < 512 && mmu_page->spte_set[e] == flags);
                    start = mmu_page->gfn + i * (1UL << ((role.level-1) * 9));
                    end = mmu_page->gfn + e * (1UL << ((role.level-1) * 9)) - 1;
                    for (l = 0; l < MMU_MAX_LEVEL-level; l++)
                        printf("    ");
                    printf("    spte 0x%08llx-0x%08llx%s\n", start, end, (flags & SPTE_MMIO) ? " mmio" : "");
                    i = e;
                }
            }
        }
    }
}

static void monitor_ctx_exit(struct prof_dev *dev);
static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct kvmmmu_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    if (env->event || env->verbose >= VERBOSE_EVENT)
        tep__ref();
    else
        tep__ref_light();

    INIT_LIST_HEAD(&ctx->kvm_mmu_gen_list);

    if (env->event) {
        ctx->tp_list = tp_list_new(dev, env->event);
        if (!ctx->tp_list)
            goto failed;
    }

    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct kvmmmu_ctx *ctx = dev->private;
    struct kvm_mmu_gen *mmu_gen, *next;
    int i;

    list_for_each_entry_safe(mmu_gen, next, &ctx->kvm_mmu_gen_list, list) {
        for (i = 0; i < MMU_MAX_LEVEL; i++) {
            struct rblist *rblist = &mmu_gen->kvm_mmu_pages[i];
            rblist__exit(rblist);
        }
        list_del(&mmu_gen->list);
        free(mmu_gen);
    }
    tp_list_free(ctx->tp_list);
    tep__unref();
    free(ctx);
}

static int kvm_mmu_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct kvmmmu_ctx *ctx;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW,
        .read_format   = 0,
        .pinned        = 1,
        .disabled      = 1,
        .watermark     = 1,
        .wakeup_watermark = (dev->pages << 12) / 3,
    };
    struct perf_evsel *evsel;
    struct tp *tp;
    int id, i;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    prof_dev_env2attr(dev, &attr);

    // kvmmmu:kvm_mmu_get_page
    id = tep__event_id("kvmmmu", "kvm_mmu_get_page");
    if (id < 0)
        goto failed;
    attr.config = ctx->kvm_mmu_get_page = id;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        goto failed;
    }
    perf_evlist__add(evlist, evsel);
    ctx->mmu_valid_gen_size = tep__event_field_size(id, "mmu_valid_gen");

    // kvmmmu:kvm_mmu_prepare_zap_page
    id = tep__event_id("kvmmmu", "kvm_mmu_prepare_zap_page");
    if (id < 0)
        goto failed;
    attr.config = ctx->kvm_mmu_prepare_zap_page = id;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        goto failed;
    }
    perf_evlist__add(evlist, evsel);

    // kvmmmu:kvm_mmu_set_spte
    if (env->spte) {
        id = tep__event_id("kvmmmu", "kvm_mmu_set_spte");
        if (id < 0)
            ctx->kvm_mmu_set_spte = ~0UL;
        else {
            attr.config = ctx->kvm_mmu_set_spte = id;
            evsel = perf_evsel__new(&attr);
            if (!evsel) {
                goto failed;
            }
            perf_evlist__add(evlist, evsel);
        }
    }

    // kvmmmu:mark_mmio_spte
    if (env->mmio) {
        id = tep__event_id("kvmmmu", "mark_mmio_spte");
        if (id < 0)
            ctx->mark_mmio_spte = ~0UL;
        else {
            attr.config = ctx->mark_mmio_spte = id;
            evsel = perf_evsel__new(&attr);
            if (!evsel) {
                goto failed;
            }
            perf_evlist__add(evlist, evsel);
        }
    }

    // env->event
    if (ctx->tp_list)
    for_each_real_tp(ctx->tp_list, tp, i) {

        if (tp->id == ctx->kvm_mmu_get_page ||
            tp->id == ctx->kvm_mmu_prepare_zap_page ||
            (env->spte && tp->id == ctx->kvm_mmu_set_spte) ||
            (env->mmio && tp->id == ctx->mark_mmio_spte)) {
            fprintf(stderr, "The additional event %s:%s cannot be any of kvmmmu:kvm_mmu_get_page, "
                    "kvmmmu:kvm_mmu_prepare_zap_page, kvmmmu:kvm_mmu_set_spte and kvmmmu:mark_mmio_spte.\n",
                    tp->sys, tp->name);
            goto failed;
        }

        evsel = tp_evsel_new(tp, &attr);
        if (!evsel) {
            goto failed;
        }
        perf_evlist__add(evlist, evsel);
    }
    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void kvm_mmu_interval(struct prof_dev *dev)
{
    struct kvmmmu_ctx *ctx = dev->private;
    struct kvm_mmu_gen *mmu_gen;
    int i, level = 0;

    list_for_each_entry(mmu_gen, &ctx->kvm_mmu_gen_list, list) {

        if (mmu_gen->kvm_mmu_get_page == 0 &&
            mmu_gen->kvm_mmu_get_page_created == 0 &&
            mmu_gen->kvm_mmu_prepare_zap_page == 0 &&
            mmu_gen->kvm_mmu_set_spte == 0)
            continue;

        print_time(stdout);
        printf("kvm_mmu_gen %lu get_page %llu created %llu zap_page %llu set_spte %llu\n",
            mmu_gen->mmu_valid_gen,
            mmu_gen->kvm_mmu_get_page,
            mmu_gen->kvm_mmu_get_page_created,
            mmu_gen->kvm_mmu_prepare_zap_page,
            mmu_gen->kvm_mmu_set_spte);
        mmu_gen->kvm_mmu_get_page = 0;
        mmu_gen->kvm_mmu_get_page_created = 0;
        mmu_gen->kvm_mmu_prepare_zap_page = 0;
        mmu_gen->kvm_mmu_set_spte = 0;
        for (i = 0; i < MMU_MAX_LEVEL; i++) {
            struct rblist *rblist = &mmu_gen->kvm_mmu_pages[i];
            if (rblist__nr_entries(rblist)) {
                level = i+1;
                printf("    level %d nr %d\n", level, rblist__nr_entries(rblist));
            }
        }
        if (dev->env->detail)
            kvm_mmu_gen_dump(mmu_gen, level, true, 0, 0UL, ~0UL);
    }
}

static int kvm_mmu_filter(struct prof_dev *dev)
{
    struct kvmmmu_ctx *ctx = dev->private;
    return tp_list_apply_filter(dev, ctx->tp_list);
}

static void kvm_mmu_deinit(struct prof_dev *dev)
{
    kvm_mmu_interval(dev);
    monitor_ctx_exit(dev);
}

static inline unsigned long __mmu_valid_gen(struct kvmmmu_ctx *ctx, unsigned long gen)
{
    switch(ctx->mmu_valid_gen_size) {
        case sizeof(__u8):
            return (__u8)gen;
        case sizeof(unsigned long):
            return (unsigned long)gen;
        default:
            return gen;
    }
}

static long kvm_mmu_ftrace_filter(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    struct kvmmmu_ctx *ctx = dev->private;
    struct sample_type_raw *raw = (void *)event->sample.array;
    unsigned short common_type = raw->raw.common_type;
    struct expr_global *glo;

    if (common_type == ctx->kvm_mmu_get_page ||
        common_type == ctx->kvm_mmu_prepare_zap_page ||
        common_type == ctx->kvm_mmu_set_spte ||
        common_type == ctx->mark_mmio_spte)
        return 1;
    else {
        glo = GLOBAL(raw->cpu_entry.cpu, raw->tid_entry.pid, raw->raw.data, raw->raw.size);
        return tp_list_ftrace_filter(dev, ctx->tp_list, glo);
    }
}

static void kvm_mmu_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    struct kvmmmu_ctx *ctx = dev->private;
    // in linux/perf_event.h
    // PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW
    struct sample_type_raw *raw = (void *)event->sample.array;
    unsigned short common_type = raw->raw.common_type;
    unsigned long mmu_valid_gen;
    struct kvm_mmu_page entry;
    int ret = -1, verbose = dev->env->verbose;

    if (common_type == ctx->kvm_mmu_get_page) {
        bool created = raw->raw.kvm_mmu_get_page.created;

        mmu_valid_gen = __mmu_valid_gen(ctx, raw->raw.kvm_mmu_get_page.mmu_valid_gen);
        entry.gfn = raw->raw.kvm_mmu_get_page.gfn;
        entry.role = raw->raw.kvm_mmu_get_page.role;
        entry.root_count = raw->raw.kvm_mmu_get_page.root_count;

        if (created)
            ctx->current_valid_gen = mmu_valid_gen;

        ret = kvm_mmu_gen_get_page(ctx, mmu_valid_gen, &entry, created);
    } else if (common_type == ctx->kvm_mmu_prepare_zap_page) {
        mmu_valid_gen = __mmu_valid_gen(ctx, raw->raw.kvm_mmu_prepare_zap_page.mmu_valid_gen);
        entry.gfn = raw->raw.kvm_mmu_prepare_zap_page.gfn;
        entry.role = raw->raw.kvm_mmu_prepare_zap_page.role;;
        entry.root_count = raw->raw.kvm_mmu_prepare_zap_page.root_count;

        ret = kvm_mmu_gen_zap_page(ctx, mmu_valid_gen, &entry);
    } else if (common_type == ctx->kvm_mmu_set_spte) {
        ret = kvm_mmu_gen_set_spte(ctx, ctx->current_valid_gen, raw->raw.kvm_mmu_set_spte.gfn,
                raw->raw.kvm_mmu_set_spte.level, SPTE_SET);
    } else if (common_type == ctx->mark_mmio_spte) {
        ret = kvm_mmu_gen_set_spte(ctx, ctx->current_valid_gen, raw->raw.mark_mmio_spte.gfn,
                1, SPTE_SET | SPTE_MMIO);
    } else {
        verbose = VERBOSE_NOTICE;
    }

    if ((verbose >= VERBOSE_NOTICE && ret < 0) || verbose >= VERBOSE_EVENT) {
        if (dev->print_title) prof_dev_print_time(dev, raw->time, stdout);
        tep__print_event(raw->time, raw->cpu_entry.cpu, raw->raw.data, raw->raw.size);
    }
}

static const char *kvmmmu_desc[] = PROFILER_DESC("kvmmmu",
    "[OPTION...] [--spte] [--mmio] [--detail] [-e EVENT]",
    "Observe the kvm_mmu_page mapping on x86 platforms.", "",
    "TRACEPOINT",
    "    kvmmmu:kvm_mmu_get_page, kvmmmu:kvm_mmu_prepare_zap_page",
    "    kvmmmu:kvm_mmu_set_spte, kvmmmu:mark_mmio_spte", "",
    "EXAMPLES",
    "    "PROGRAME" kvmmmu -p 2347 -i 5000 --mmio",
    "    "PROGRAME" kvmmmu -p 2347 -i 5000 --spte --mmio --detail");
static const char *kvmmmu_argv[] = PROFILER_ARGV("kvmmmu",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_PROFILER, "spte", "mmio", "detail\nPrint detailed kvm_mmu page tables", "event");
struct monitor kvm_mmu = {
    .name = "kvmmmu",
    .desc = kvmmmu_desc,
    .argv = kvmmmu_argv,
    .pages = 64,
    .order = true,
    .init = kvm_mmu_init,
    .filter = kvm_mmu_filter,
    .deinit = kvm_mmu_deinit,
    .interval = kvm_mmu_interval,
    .ftrace_filter = kvm_mmu_ftrace_filter,
    .sample = kvm_mmu_sample,
};
MONITOR_REGISTER(kvm_mmu)

