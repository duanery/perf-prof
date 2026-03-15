/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
// Copyright (c) 2020 Wenbo Zhang
//
// Based on ksyms improvements from Andrii Nakryiko, add more helpers.
// 28-Feb-2020   Wenbo Zhang   Created this.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <time.h>
#include <sched.h>
#include <linux/refcount.h>
#include <linux/rblist.h>
#include <linux/time64.h>
#include <linux/hashtable.h>
#include <bpf/btf.h>
#include <bpf/libbpf.h>
#include <limits.h>
#include <demangle-cxx.h>
#include <demangle-java.h>
#include <demangle-rust.h>
#include <monitor.h>
#include "trace_helpers.h"
#include "uprobe_helpers.h"

#define min(x, y) ({                \
    typeof(x) _min1 = (x);          \
    typeof(y) _min2 = (y);          \
    (void) (&_min1 == &_min2);      \
    _min1 < _min2 ? _min1 : _min2; })

#define DISK_NAME_LEN   32

#define MINORBITS   20
#define MINORMASK   ((1U << MINORBITS) - 1)

#define MKDEV(ma, mi)   (((ma) << MINORBITS) | (mi))

struct ksym_dyn {
    struct ksym sym;
    int len;
    int type;
    struct list_head link;
    char name[0];
};

struct ksyms {
    struct ksym *syms;
    int syms_sz;
    int syms_cap;
    char *strs;
    int strs_sz;
    int strs_cap;
    struct list_head ksym_dyn_list;
};

static void ksym_dyn_free(struct ksyms *ksyms)
{
    struct ksym_dyn *dyn, *n;
    list_for_each_entry_safe(dyn, n, &ksyms->ksym_dyn_list, link) {
        list_del(&dyn->link);
        free(dyn);
    }
}

void ksym__register_unregister(struct ksyms *ksyms, struct perf_record_ksymbol *ksymbol)
{
    struct ksym_dyn *dyn;

    if (ksymbol->flags & PERF_RECORD_KSYMBOL_FLAGS_UNREGISTER) {
        list_for_each_entry(dyn, &ksyms->ksym_dyn_list, link) {
            if (ksymbol->addr == dyn->sym.addr &&
                ksymbol->len == dyn->len) {
                list_del(&dyn->link);
                free(dyn);
                break;
            }
        }
    } else {
        int name_len = strlen(ksymbol->name);
        dyn = malloc(sizeof(*dyn) + name_len + 1);
        dyn->sym.name = dyn->name;
        dyn->sym.addr = ksymbol->addr;
        dyn->len = ksymbol->len;
        dyn->type = ksymbol->ksym_type; // PERF_RECORD_KSYMBOL_TYPE_*
        strcpy(dyn->name, ksymbol->name);
        list_add(&dyn->link, &ksyms->ksym_dyn_list);
    }
}

static int ksyms__add_symbol(struct ksyms *ksyms, const char *name, unsigned long addr)
{
    size_t new_cap, name_len = strlen(name) + 1;
    struct ksym *ksym;
    void *tmp;

    if (ksyms->strs_sz + name_len > ksyms->strs_cap) {
        new_cap = ksyms->strs_cap * 4 / 3;
        if (new_cap < ksyms->strs_sz + name_len)
            new_cap = ksyms->strs_sz + name_len;
        if (new_cap < 1024)
            new_cap = 1024;
        tmp = realloc(ksyms->strs, new_cap);
        if (!tmp)
            return -1;
        ksyms->strs = tmp;
        ksyms->strs_cap = new_cap;
    }
    if (ksyms->syms_sz + 1 > ksyms->syms_cap) {
        new_cap = ksyms->syms_cap * 4 / 3;
        if (new_cap < 1024)
            new_cap = 1024;
        tmp = realloc(ksyms->syms, sizeof(*ksyms->syms) * new_cap);
        if (!tmp)
            return -1;
        ksyms->syms = tmp;
        ksyms->syms_cap = new_cap;
    }

    ksym = &ksyms->syms[ksyms->syms_sz];
    /* while constructing, re-use pointer as just a plain offset */
    ksym->name = (void *)(unsigned long)ksyms->strs_sz;
    ksym->addr = addr;

    memcpy(ksyms->strs + ksyms->strs_sz, name, name_len);
    ksyms->strs_sz += name_len;
    ksyms->syms_sz++;

    return 0;
}

static int ksym_cmp(const void *p1, const void *p2)
{
    const struct ksym *s1 = p1, *s2 = p2;

    if (s1->addr == s2->addr)
        return strcmp(s1->name, s2->name);
    return s1->addr < s2->addr ? -1 : 1;
}

struct ksyms *ksyms__load(void)
{
    char sym_type, sym_name[256];
    struct ksyms *ksyms;
    unsigned long sym_addr;
    int i, ret;
    FILE *f;

    ksyms = calloc(1, sizeof(*ksyms));
    if (!ksyms)
        return NULL;

    /*
     * Enable BPF JIT symbols temporarily for early kernels (< 5.5)
     * This ensures BPF JIT compiled functions are visible in /proc/kallsyms
     */
    sysctl_bpf_jit_kallsyms(1);
    f = fopen("/proc/kallsyms", "r");
    if (!f)
        goto err_out1;

    while (true) {
        ret = fscanf(f, "%lx %c %s%*[^\n]\n",
                 &sym_addr, &sym_type, sym_name);
        if (ret == EOF && feof(f))
            break;
        if (ret != 3)
            goto err_out;
        if (ksyms__add_symbol(ksyms, sym_name, sym_addr))
            goto err_out;
    }
    fclose(f);
    sysctl_bpf_jit_kallsyms(0);

    /* now when strings are finalized, adjust pointers properly */
    for (i = 0; i < ksyms->syms_sz; i++)
        ksyms->syms[i].name += (unsigned long)ksyms->strs;

    qsort(ksyms->syms, ksyms->syms_sz, sizeof(*ksyms->syms), ksym_cmp);

    INIT_LIST_HEAD(&ksyms->ksym_dyn_list);
    return ksyms;

err_out:
    fclose(f);
err_out1:
    sysctl_bpf_jit_kallsyms(0);
    ksyms__free(ksyms);
    return NULL;
}

void ksyms__free(struct ksyms *ksyms)
{
    if (!ksyms)
        return;
    ksym_dyn_free(ksyms);
    free(ksyms->syms);
    free(ksyms->strs);
    free(ksyms);
}

const struct ksym *ksyms__map_addr(const struct ksyms *ksyms,
                   unsigned long addr)
{
    int start = 0, end = ksyms->syms_sz - 1, mid;
    unsigned long sym_addr;
    struct ksym_dyn *dyn;

    if (unlikely(!list_empty(&ksyms->ksym_dyn_list))) {
        list_for_each_entry(dyn, &ksyms->ksym_dyn_list, link) {
            if (addr >= dyn->sym.addr && addr <= dyn->sym.addr + dyn->len)
                return &dyn->sym;
        }
    }

    /* find largest sym_addr <= addr using binary search */
    while (start < end) {
        mid = start + (end - start + 1) / 2;
        sym_addr = ksyms->syms[mid].addr;

        if (sym_addr <= addr)
            start = mid;
        else
            end = mid - 1;
    }

    if (start == end && ksyms->syms[start].addr <= addr)
        return &ksyms->syms[start];
    return NULL;
}

const struct ksym *ksyms__get_symbol(const struct ksyms *ksyms,
                     const char *name)
{
    int i;

    for (i = 0; i < ksyms->syms_sz; i++) {
        if (strcmp(ksyms->syms[i].name, name) == 0)
            return &ksyms->syms[i];
    }

    return NULL;
}

static DEFINE_HASHTABLE(mntns_ino_fds, 6);
static unsigned long default_mntns_ino = 0;
static int default_mntns_fd = 0;
struct mntns {
    struct hlist_node node;
    unsigned long mntns_ino; // /proc/self/ns/mnt -> mnt:[4026531840]
    int mntns_fd;
    int refcnt;
};
/*
 * Get the symbol table of the process under the non-init mount namespace.
 * Typically used in container scenarios.
 *
 * When we need to read symbols of non-default mntns, enter the corresponding
 * mount namespace through setns(), and return to the default mount namespace
 * after reading the symbols.
 */
static __ctor void __mntns_init(void)
{
    const char *mntns_path = "/proc/self/ns/mnt";
    char buf[128];
    if (readlink(mntns_path, buf, sizeof(buf)) > 5) {
        default_mntns_ino = atol(buf + 5 /* mnt:[ */);
        default_mntns_fd = open(mntns_path, O_RDONLY);
    }
}

static struct mntns *get_mntns(int pid)
{
    char path[128];
    char buf[128];
    unsigned long mntns_ino;
    int mntns_fd;
    struct mntns *mnt;

    snprintf(path, sizeof(buf), "/proc/%d/ns/mnt", pid);
    if (readlink(path, buf, sizeof(buf)) > 5)
        mntns_ino = atol(buf + 5 /* mnt:[ */);
    else
        return NULL;

    if (mntns_ino == default_mntns_ino)
        return NULL;

    hash_for_each_possible(mntns_ino_fds, mnt, node, mntns_ino) {
        if (mnt->mntns_ino == mntns_ino) {
            mnt->refcnt++;
            return mnt;
        }
    }

    mntns_fd = open(path, O_RDONLY);
    if (mntns_fd == -1)
        return NULL;

    mnt = zalloc(sizeof(*mnt));
    if (mnt) {
        mnt->mntns_ino = mntns_ino;
        mnt->mntns_fd = mntns_fd;
        mnt->refcnt = 1;
        hash_add(mntns_ino_fds, &mnt->node, mntns_ino);
    } else
        close(mntns_fd);

    return mnt;
}

static void put_mntns(struct mntns *mnt)
{
    if (mnt && --mnt->refcnt == 0) {
        hash_del(&mnt->node);
        close(mnt->mntns_fd);
        free(mnt);
    }
}

static void enter_mntns(struct mntns *mnt)
{
    if (mnt)
        setns(mnt->mntns_fd, CLONE_NEWNS);
}

static void exit_mntns(struct mntns *mnt)
{
    if (mnt)
        setns(default_mntns_fd, CLONE_NEWNS);
}

/*
 * syms_cache --> syms --> dso --> object --> sym
 *            pid      maps    file       sym
**/

struct load_range {
    uint64_t start;
    uint64_t end;
    uint64_t file_off;
};

enum elf_type {
    EXEC,
    DYN,
    PERF_MAP,
    VDSO,
    UNKNOWN,
};

struct object {
    struct rb_node rbnode;
    refcount_t refcnt;
    struct mntns *mnt;
    char *name;
    char *name_atmnt;

    char *buildid;
    int buildid_sz;
    /*
     * Points to the buildid object, which is also the object
     * that actually stores the symbols. Why the pointer?
     * Copied binaries have the same buildid, the common
     * buildid_obj can reduce memory.
     * If buildid_obj is not equal to itself, get its `refcnt'.
    **/
    struct object *buildid_obj;
    struct rb_node buildid_rbnode;

    /* Dyn's first text section virtual addr at execution */
    uint64_t sh_addr;
    /* Dyn's first text section file offset */
    uint64_t sh_offset;
    enum elf_type type;

    struct sym *syms;
    int syms_sz;
    int syms_cap;

    char *strs;
    int strs_sz;
    int strs_cap;

    char *demangled;
    int demangled_sz; // demangled_cap = strs_cap;

#ifdef HAVE_LIBUNWIND
    /* Cached .eh_frame_hdr data for DWARF unwind */
    void *eh_frame_hdr;         /* copied eh_frame_hdr segment data */
    uint64_t eh_frame_hdr_sz;   /* size of eh_frame_hdr data */
    uint64_t eh_frame_hdr_vaddr; /* ELF virtual address of eh_frame_hdr */
    /* Cached .eh_frame data for DWARF unwind */
    void *eh_frame;             /* copied eh_frame data */
    uint64_t eh_frame_sz;       /* size of eh_frame data */
    uint64_t eh_frame_vaddr;    /* ELF virtual address of eh_frame */
    /* First PT_LOAD segment vaddr (for base address computation) */
    uint64_t elf_load_vaddr;
    bool unwind_info_loaded;    /* whether we attempted to load unwind info */
#endif
};

struct dso {
    struct load_range *ranges;
    int range_sz;
    struct object *obj;
};

struct map {
    uint64_t start_addr;
    uint64_t end_addr;
    uint64_t file_off;
    uint64_t dev_major;
    uint64_t dev_minor;
    uint64_t inode;
};

struct syms {
    struct dso *dsos;
    int dso_sz;
};

static bool is_file_backed(const char *mapname)
{
#define STARTS_WITH(mapname, prefix) \
    (!strncmp(mapname, prefix, sizeof(prefix) - 1))

    return mapname[0] && !(
        STARTS_WITH(mapname, "//anon") ||
        STARTS_WITH(mapname, "/dev/zero") ||
        STARTS_WITH(mapname, "/anon_hugepage") ||
        STARTS_WITH(mapname, "socket:") ||
        STARTS_WITH(mapname, "[stack") ||
        STARTS_WITH(mapname, "/SYSV") ||
        STARTS_WITH(mapname, "[heap]") ||
        STARTS_WITH(mapname, "[vsyscall]"));
}

static bool is_perf_map(const char *path)
{
    return false;
}

static bool is_vdso(const char *path)
{
    return !strcmp(path, "[vdso]");
}

static int get_elf_type(Elf *e)
{
    GElf_Ehdr hdr;
    void *res;

    res = gelf_getehdr(e, &hdr);
    if (!res)
        return -1;
    return hdr.e_type;
}

static int get_elf_text_scn_info(Elf *e, uint64_t *addr,
                 uint64_t *offset)
{
    Elf_Scn *section = NULL;
    GElf_Shdr header;
    size_t stridx;
    char *name;

    if (elf_getshdrstrndx(e, &stridx) < 0)
        return -1;

    while ((section = elf_nextscn(e, section)) != 0) {
        if (!gelf_getshdr(section, &header))
            continue;

        name = elf_strptr(e, stridx, header.sh_name);
        if (name && !strcmp(name, ".text")) {
            *addr = (uint64_t)header.sh_addr;
            *offset = (uint64_t)header.sh_offset;
            return 0;
        }
    }
    return -1;
}

static int get_elf_build_id(Elf *e, char **buildid)
{
    Elf_Scn *section = NULL;
    GElf_Shdr header;
    Elf_Data *rawdata;
    int note_offs;
    int ret = 0;

    while ((section = elf_nextscn(e, section)) != 0) {
        if (!gelf_getshdr(section, &header))
            continue;
        if (header.sh_type != SHT_NOTE)
            continue;

        rawdata = elf_rawdata(section, NULL);
        note_offs = 0;

        while (note_offs < header.sh_size) {
            GElf_Nhdr *note = rawdata->d_buf + note_offs;
            const char *name = note->n_namesz == 0 ? NULL : (const char *)(note + 1);

            if (note->n_type == NT_GNU_BUILD_ID &&
                note->n_namesz == 4 &&
                strncmp (name, ELF_NOTE_GNU, 4) == 0 &&
                note->n_descsz > 0) {
                if (buildid) {
                    const char *desc = name + ALIGN(note->n_namesz, 4);
                    *buildid = memdup(desc, note->n_descsz);
                }
                WARN_ON_ONCE(note->n_descsz != 20);
                ret = note->n_descsz;
                goto out;
            }

            note_offs += sizeof(*note) + ALIGN(note->n_namesz, 4) +
                         ALIGN(note->n_descsz, 4);
        }
    }

out:
    return ret;
}

static int obj_get_elf_info(struct object *obj)
{
    const char *name = obj->name;
    Elf *e;
    int fd, type, err = -1;

    if (is_perf_map(name)) {
        obj->type = PERF_MAP;
        return 0;
    } else if (is_vdso(name)) {
        obj->type = VDSO;
        return 0;
    }

    e = open_elf(name, &fd);
    if (!e)
        return -1;

    type = get_elf_type(e);
    if (type == ET_EXEC) {
        obj->type = EXEC;
        if (get_elf_text_scn_info(e, &obj->sh_addr, &obj->sh_offset) < 0)
            goto err;
        obj->buildid_sz = get_elf_build_id(e, &obj->buildid);
    } else if (type == ET_DYN) {
        obj->type = DYN;
        if (get_elf_text_scn_info(e, &obj->sh_addr, &obj->sh_offset) < 0)
            goto err;
        obj->buildid_sz = get_elf_build_id(e, &obj->buildid);
    } else
        obj->type = UNKNOWN;

    err = 0;
err:
    close_elf(e, fd);
    return err;
}


static int buildid_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct object *obj = container_of(rbn, struct object, buildid_rbnode);
    struct object *tmp = (void *)entry;
    return memcmp(obj->buildid, tmp->buildid, tmp->buildid_sz);
}

static struct rb_node *buildid_node_new(struct rblist *rlist, const void *new_entry)
{
    struct object *obj = (void *)new_entry;
    return &obj->buildid_rbnode;
}

static void buildid_node_delete(struct rblist *rblist, struct rb_node *rbn)
{
    // empty
}

static struct rblist buildid_objects = {
    .entries = RB_ROOT_CACHED,
    .nr_entries = 0,
    .node_cmp = buildid_node_cmp,
    .node_new = buildid_node_new,
    .node_delete = buildid_node_delete,
};

struct tmp_object {
    struct mntns *mnt;
    const char *name;
};
static void obj__put(struct object *obj);
static int object_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct object *obj = container_of(rbn, struct object, rbnode);
    struct tmp_object *tmp = (void *)entry;

    if (likely(obj->mnt == tmp->mnt))
        return strcmp(obj->name, tmp->name);
    else
        return (int)((s64)obj->mnt - (s64)tmp->mnt);
}

static struct rb_node *object_node_new(struct rblist *rlist, const void *new_entry)
{
    struct tmp_object *tmp = (void *)new_entry;
    const char *name = tmp->name;
    struct object *obj = malloc(sizeof(*obj));
    int err = 0;

    if (obj) {
        memset(obj, 0, sizeof(*obj));
        RB_CLEAR_NODE(&obj->rbnode);
        refcount_set(&obj->refcnt, 0);
        obj->mnt = tmp->mnt;
        obj->name = strdup(name);
        if (obj->mnt &&
            asprintf(&obj->name_atmnt, "%s @mnt:[%lu]", name, obj->mnt->mntns_ino) < 0)
            obj->name_atmnt = NULL;

        obj->buildid_obj = obj;
        RB_CLEAR_NODE(&obj->buildid_rbnode);

        enter_mntns(obj->mnt);
        err = obj_get_elf_info(obj);
        exit_mntns(obj->mnt);

        if (err < 0) {
            free(obj->name);
            if (obj->name_atmnt) free(obj->name_atmnt);
            if (obj->buildid) free(obj->buildid);
            free (obj);
            return NULL;
        }

        if (obj->buildid_sz) {
            struct rb_node *buildid_node = rblist__findnew(&buildid_objects, obj);
            struct object *buildid_obj = rb_entry(buildid_node, struct object, buildid_rbnode);
            // rblist__findnew() will always succeed and buildid_obj will not be NULL.
            if (buildid_obj != obj) {
                refcount_inc(&buildid_obj->refcnt);
                obj->buildid_obj = buildid_obj;
            }
        }
        tmp->mnt = NULL;
        return &obj->rbnode;
    } else
        return NULL;
}

static void object_node_delete(struct rblist *rblist, struct rb_node *rbn)
{
    struct object *obj = container_of(rbn, struct object, rbnode);

    if (obj->buildid_sz) {
        struct object *buildid_obj = obj->buildid_obj;
        if (buildid_obj != obj)
            obj__put(buildid_obj);
        else
            rblist__remove_node(&buildid_objects, &obj->buildid_rbnode);
    }

    put_mntns(obj->mnt);
    free(obj->name);
    if (obj->name_atmnt) free(obj->name_atmnt);
    if (obj->buildid) free(obj->buildid);
    if (obj->syms) free(obj->syms);
    if (obj->strs) free(obj->strs);
    if (obj->demangled) free(obj->demangled);
#ifdef HAVE_LIBUNWIND
    if (obj->eh_frame_hdr) free(obj->eh_frame_hdr);
    if (obj->eh_frame) free(obj->eh_frame);
#endif
    free(obj);
}

static struct rblist objects = {
    .entries = RB_ROOT_CACHED,
    .nr_entries = 0,
    .node_cmp = object_node_cmp,
    .node_new = object_node_new,
    .node_delete = object_node_delete,
};

static struct object *obj__get(const char *name, pid_t tgid)
{
    struct rb_node *rbnode;
    struct object *obj = NULL;
    struct tmp_object tmp;

    tmp.mnt = tgid ? get_mntns(tgid) : NULL;
    tmp.name = name;
    rbnode = rblist__findnew(&objects, &tmp);
    if (rbnode) {
        obj = rb_entry(rbnode, struct object, rbnode);
        if (refcount_read(&obj->refcnt) == 0)
            refcount_set(&obj->refcnt, 1);
        else
            refcount_inc(&obj->refcnt);
    }

    if (tmp.mnt)
        put_mntns(tmp.mnt);
    return obj;
}

static void obj__put(struct object *obj)
{
    if (obj && refcount_dec_and_test(&obj->refcnt))
        rblist__remove_node(&objects, &obj->rbnode);
}

void obj__stat(FILE *fp)
{
    static const char *str_type[] = {"EXEC", "DYN", "PERF_MAP", "VDSO", "UNKNOWN"};
    struct rb_node *node;
    struct object *obj;
    long used, size, total_used = 0, total_size = 0;

    if (rblist__nr_entries(&objects) == 0)
        return;

    fprintf(fp, "OBJECT STAT:\n");
    fprintf(fp, "%-4s %-8s %-8s %-12s %-12s %s\n", "REF", "TYPE", "SYMS", "USED", "MEMS", "OBJECT");
    for (node = rb_first_cached(&objects.entries); node;
         node = rb_next(node)) {
        obj = container_of(node, struct object, rbnode);
        used = obj->syms_sz * sizeof(*obj->syms) + obj->strs_sz + obj->demangled_sz;
        size = obj->syms_cap * sizeof(*obj->syms) + obj->strs_cap + (obj->demangled ? obj->strs_cap : 0);
#ifdef HAVE_LIBUNWIND
        used += obj->eh_frame_hdr_sz + obj->eh_frame_sz;
        size += obj->eh_frame_hdr_sz + obj->eh_frame_sz;
#endif
        total_used += used;
        total_size += size;
        fprintf(fp, "%-4u %-8s %-8d %-12ld %-12ld %s%s", refcount_read(&obj->refcnt),
                str_type[obj->type], obj->syms_sz, used, size, obj->name_atmnt ? : obj->name,
                obj->buildid ? "    " : "\n");
        if (obj->buildid) {
            if (obj->buildid_obj == obj)
                fprintf(fp, "BuildID: %02x%02x%02x%02x..\n", (u8)obj->buildid[0], (u8)obj->buildid[1],
                    (u8)obj->buildid[2], (u8)obj->buildid[3]);
            else
                fprintf(fp, "-> BuildObj: %s%s", obj->buildid_obj->name_atmnt ? : obj->buildid_obj->name,
                    (obj->syms || obj->strs || obj->demangled) ? "  Error\n" : "\n");
        }
    }
    fprintf(fp, "OBJECTS %u USED %ld MEMS %ld\n", rblist__nr_entries(&objects), total_used, total_size);
}

static int syms__add_dso(struct syms *syms, struct map *map, const char *name, pid_t tgid)
{
    struct dso *dso = NULL;
    struct object *obj = NULL;
    int i;
    void *tmp;

    for (i = 0; i < syms->dso_sz; i++) {
        if (!strcmp(syms->dsos[i].obj->name, name)) {
            dso = &syms->dsos[i];
            break;
        }
    }

    if (!dso) {
        obj = obj__get(name, tgid);
        if (!obj)
            goto err_out;

        tmp = realloc(syms->dsos, (syms->dso_sz + 1) *
                  sizeof(*syms->dsos));
        if (!tmp)
            goto err_out;
        syms->dsos = tmp;
        dso = &syms->dsos[syms->dso_sz++];
        memset(dso, 0, sizeof(*dso));
        dso->obj = obj;
        obj = NULL;
    }

    tmp = realloc(dso->ranges, (dso->range_sz + 1) * sizeof(*dso->ranges));
    if (!tmp)
        goto err_out;
    dso->ranges = tmp;
    dso->ranges[dso->range_sz].start = map->start_addr;
    dso->ranges[dso->range_sz].end = map->end_addr;
    dso->ranges[dso->range_sz].file_off = map->file_off;
    dso->range_sz++;
    return 0;

err_out:
    if (obj)
        obj__put(obj);
    return -1;
}

static void dso__free_fields(struct dso *dso)
{
    if (!dso)
        return;

    obj__put(dso->obj);
    free(dso->ranges);
}

struct dso *syms__find_dso(const struct syms *syms, unsigned long addr,
                  uint64_t *offset)
{
    struct load_range *range;
    struct dso *dso;
    int i, j;

    for (i = 0; i < syms->dso_sz; i++) {
        dso = &syms->dsos[i];
        for (j = 0; j < dso->range_sz; j++) {
            range = &dso->ranges[j];
            if (addr <= range->start || addr >= range->end)
                continue;
            if (dso->obj->type == DYN || dso->obj->type == VDSO) {
                /* Offset within the mmap */
                *offset = addr - range->start + range->file_off;
                /* Offset within the ELF for dyn symbol lookup */
                *offset += dso->obj->sh_addr - dso->obj->sh_offset;
            } else {
                *offset = addr;
            }

            return dso;
        }
    }

    return NULL;
}

static int obj__load_sym_table_from_perf_map(struct object *obj)
{
    return -1;
}

static int obj__add_sym(struct object *obj, const char *name, uint64_t start,
            uint64_t size)
{
    struct sym *sym;
    size_t new_cap, name_len = strlen(name) + 1;
    void *tmp;

    if (obj->strs_sz + name_len > obj->strs_cap) {
        new_cap = obj->strs_cap * 4 / 3;
        if (new_cap < obj->strs_sz + name_len)
            new_cap = obj->strs_sz + name_len;
        if (new_cap < 1024)
            new_cap = 1024;
        tmp = realloc(obj->strs, new_cap);
        if (!tmp)
            return -1;
        obj->strs = tmp;
        obj->strs_cap = new_cap;
    }

    if (obj->syms_sz + 1 > obj->syms_cap) {
        new_cap = obj->syms_cap * 4 / 3;
        if (new_cap < 1024)
            new_cap = 1024;
        tmp = realloc(obj->syms, sizeof(*obj->syms) * new_cap);
        if (!tmp)
            return -1;
        obj->syms = tmp;
        obj->syms_cap = new_cap;
    }

    sym = &obj->syms[obj->syms_sz++];
    /* while constructing, re-use pointer as just a plain offset */
    sym->name = (void *)(unsigned long)obj->strs_sz;
    sym->demangled = NULL;
    sym->start = start;
    sym->size = size;

    memcpy(obj->strs + obj->strs_sz, name, name_len);
    obj->strs_sz += name_len;
    return 0;
}

static int sym_cmp(const void *p1, const void *p2)
{
    const struct sym *s1 = p1, *s2 = p2;

    if (s1->start == s2->start)
        return strcmp(s1->name, s2->name);
    return s1->start < s2->start ? -1 : 1;
}

static int obj__add_syms(struct object *obj, Elf *e, Elf_Scn *section,
             size_t stridx, size_t symsize)
{
    Elf_Data *data = NULL;

    while ((data = elf_getdata(section, data)) != 0) {
        size_t i, symcount = data->d_size / symsize;

        if (data->d_size % symsize)
            return -1;

        for (i = 0; i < symcount; ++i) {
            const char *name;
            GElf_Sym sym;

            if (!gelf_getsym(data, (int)i, &sym))
                continue;
            if (!(name = elf_strptr(e, stridx, sym.st_name)))
                continue;
            if (name[0] == '\0')
                continue;

            if (sym.st_value == 0)
                continue;

            if (obj__add_sym(obj, name, sym.st_value, sym.st_size))
                goto err_out;
        }
    }

    return 0;

err_out:
    return -1;
}

static void obj__free_fields(struct object *obj)
{
    free(obj->syms);
    free(obj->strs);
    obj->syms_sz = 0;
    obj->syms_cap = 0;
    obj->strs_sz = 0;
    obj->syms_cap = 0;
}

#ifdef HAVE_LZMA_SUPPORT
#include <lzma.h>
#define MAGIC		"\xFD" "7zXZ\0" /* XZ file format.  */
#define MAGIC2		"\x5d\0"	/* Raw LZMA format.  */
static int unlzma(void *input, size_t input_size, void **output, size_t *output_size)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_action action = LZMA_RUN;
    lzma_ret ret;
    void *buffer = NULL;
    size_t size = 0;

    *output = NULL;
    *output_size = 0;

    #define NOMAGIC(magic) \
      (input_size <= sizeof magic || \
       memcmp (input, magic, sizeof magic - 1))
    if (NOMAGIC (MAGIC) && NOMAGIC (MAGIC2))
        return -1;

    ret = lzma_stream_decoder(&strm, 1UL << 30, 0);
    if (ret != LZMA_OK)
        return -1;

    strm.next_in = input;
	strm.avail_in = input_size;

    do {
        if (strm.avail_out == 0) {
            ptrdiff_t pos = (void *) strm.next_out - buffer;
            size_t more = size ? size * 2 : input_size;
            char *b = realloc (buffer, more);
            while (unlikely (b == NULL) && more >= size + 1024)
                b = realloc (buffer, more -= 1024);
            if (unlikely (b == NULL)) {
                ret = LZMA_MEM_ERROR;
                break;
            }
            buffer = b;
            size = more;
            strm.next_out = buffer + pos;
            strm.avail_out = size - pos;
        }
    } while ((ret = lzma_code(&strm, action)) == LZMA_OK);

    size = strm.total_out;
    buffer = realloc (buffer, size) ?: size == 0 ? NULL : buffer;

    lzma_end(&strm);

    if (ret == LZMA_STREAM_END) {
        *output = buffer;
        *output_size = size;
        return 0;
    }

    free(buffer);
    return -1;
}
#else
static int unlzma(void *input, size_t input_size, void **output, size_t *output_size)
{
    return -1;
}
#endif

static int elf__load_sym_table(struct object *obj, Elf *e)
{
    Elf_Scn *section = NULL;
    size_t shstrndx;
    void *buffer = NULL;
    size_t size = 0;
    int added = 0;

    if (elf_getshdrstrndx(e, &shstrndx) < 0)
        return -1;

    while ((section = elf_nextscn(e, section)) != 0) {
        GElf_Shdr header;
        const char *name;

        if (!gelf_getshdr(section, &header))
            continue;

        name = elf_strptr(e, shstrndx, header.sh_name);
        if (name == NULL)
            continue;

        if (header.sh_type == SHT_SYMTAB ||
            header.sh_type == SHT_DYNSYM) {
            if (obj__add_syms(obj, e, section, header.sh_link,
                      header.sh_entsize))
                goto err_out;
            added ++;
            continue;
        }

        if (!strcmp (name, ".gnu_debugdata")) {
            /* Uncompress LZMA data found in a minidebug file.  The minidebug
             * format is described at
             * https://sourceware.org/gdb/current/onlinedocs/gdb/MiniDebugInfo.html.
             */
            Elf_Data *rawdata = elf_rawdata(section, NULL);
            if (rawdata != NULL &&
                unlzma(rawdata->d_buf, rawdata->d_size, &buffer, &size) == 0) {
                Elf *debuginfo = open_elf_memory(buffer, size);
                if (debuginfo &&
                    elf__load_sym_table(obj, debuginfo) == 0) {
                    close_elf(debuginfo, 0);
                }
                free(buffer);
            }
        }
    }

    return added ? 0 : -1;

err_out:
    return -1;
}

#define SYSTEM_BUILD_ID_DIR "/usr/lib/debug/.build-id/"

/*
 * Open a separate debug info file, using the build ID to find it.
 * The GDB manual says that the only place gdb looks for a debug file
 * when the build ID is known is in /usr/lib/debug/.build-id.
 * https://sourceware.org/gdb/onlinedocs/gdb/Separate-Debug-Files.html
 */
static Elf *open_elf_debugfile_by_buildid(struct object *obj, int *debug_fd)
{
    const char * const prefix = SYSTEM_BUILD_ID_DIR;
    const size_t prefix_len = strlen (prefix);
    const char * const suffix = ".debug";
    const size_t suffix_len = strlen (suffix);
    size_t len;
    char *bd_filename, *t;
    size_t i;
    Elf *debug = NULL;

    if (!obj->buildid_sz)
        return NULL;

    len = prefix_len + obj->buildid_sz * 2 + suffix_len + 2;
    bd_filename = malloc(len);
    if (bd_filename == NULL)
        return NULL;

    t = bd_filename;
    memcpy(t, prefix, prefix_len);
    t += prefix_len;
    for (i = 0; i < obj->buildid_sz; i++) {
        unsigned char b;
        unsigned char nib;

        b = (unsigned char) obj->buildid[i];
        nib = (b & 0xf0) >> 4;
        *t++ = nib < 10 ? '0' + nib : 'a' + nib - 10;
        nib = b & 0x0f;
        *t++ = nib < 10 ? '0' + nib : 'a' + nib - 10;
        if (i == 0)
            *t++ = '/';
    }
    memcpy (t, suffix, suffix_len);
    t[suffix_len] = '\0';

    debug = open_elf(bd_filename, debug_fd);
    free(bd_filename);
    return debug;
}

static int obj__load_sym_table_from_elf(struct object *obj, int fd)
{
    Elf *e, *debug = NULL;
    int debug_fd = 0;
    int i, err = -1;
    void *tmp;

    e = fd > 0 ? open_elf_by_fd(fd) : open_elf(obj->name, &fd);
    if (!e)
        return err;

    debug = open_elf_debugfile_by_buildid(obj, &debug_fd);

    if (!debug || elf__load_sym_table(obj, debug) < 0) {
        if (elf__load_sym_table(obj, e) < 0)
            goto err_out;
    }

    tmp = realloc(obj->strs, obj->strs_sz);
    if (!tmp)
        goto err_out;
    obj->strs = tmp;
    obj->strs_cap = obj->strs_sz;

    tmp = realloc(obj->syms, sizeof(*obj->syms) * obj->syms_sz);
    if (!tmp)
        goto err_out;
    obj->syms = tmp;
    obj->syms_cap = obj->syms_sz;

    /* now when strings are finalized, adjust pointers properly */
    for (i = 0; i < obj->syms_sz; i++)
        obj->syms[i].name += (unsigned long)obj->strs;

    qsort(obj->syms, obj->syms_sz, sizeof(*obj->syms), sym_cmp);

    err = 0;

out:
    close_elf(e, fd);
    if (debug)
        close_elf(debug, debug_fd);
    return err;

err_out:
    obj__free_fields(obj);
    goto out;
}

static int create_tmp_vdso_image(struct object *obj)
{
    uint64_t start_addr, end_addr;
    long pid = getpid();
    char buf[PATH_MAX];
    void *image = NULL;
    char tmpfile[128];
    int ret, fd = -1;
    uint64_t sz;
    char *name;
    FILE *f;

    snprintf(tmpfile, sizeof(tmpfile), "/proc/%ld/maps", pid);
    f = fopen(tmpfile, "r");
    if (!f)
        return -1;

    while (true) {
        ret = fscanf(f, "%lx-%lx %*s %*x %*x:%*x %*u%[^\n]",
                 &start_addr, &end_addr, buf);
        if (ret == EOF && feof(f))
            break;
        if (ret != 3)
            goto err_out;

        name = buf;
        while (isspace(*name))
            name++;
        if (!is_file_backed(name))
            continue;
        if (is_vdso(name))
            break;
    }

    sz = end_addr - start_addr;
    image = malloc(sz);
    if (!image)
        goto err_out;
    memcpy(image, (void *)start_addr, sz);

    snprintf(tmpfile, sizeof(tmpfile),
         "/tmp/libbpf_%ld_vdso_image_XXXXXX", pid);
    fd = mkostemp(tmpfile, O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "failed to create temp file: %s\n",
            strerror(errno));
        goto err_out;
    }
    /* Unlink the file to avoid leaking */
    if (unlink(tmpfile) == -1)
        fprintf(stderr, "failed to unlink %s: %s\n", tmpfile,
            strerror(errno));
    if (write(fd, image, sz) == -1) {
        fprintf(stderr, "failed to write to vDSO image: %s\n",
            strerror(errno));
        close(fd);
        fd = -1;
        goto err_out;
    }

err_out:
    fclose(f);
    free(image);
    return fd;
}

static int obj__load_sym_table_from_vdso_image(struct object *obj)
{
    int fd = create_tmp_vdso_image(obj);

    if (fd < 0)
        return -1;
    return obj__load_sym_table_from_elf(obj, fd);
}

static int obj__load_sym_table(struct object *obj)
{
    int err = -1;
    if (obj->type == UNKNOWN)
        return -1;
    enter_mntns(obj->mnt);
    if (obj->type == PERF_MAP)
        err = obj__load_sym_table_from_perf_map(obj);
    if (obj->type == EXEC || obj->type == DYN)
        err = obj__load_sym_table_from_elf(obj, 0);
    if (obj->type == VDSO)
        err = obj__load_sym_table_from_vdso_image(obj);
    exit_mntns(obj->mnt);
    return err;
}

static const struct sym *obj__demangle_sym(struct object *obj, struct sym *sym)
{
    char *demangled = NULL;
    int sym_len;

    if (sym->demangled)
        return sym;

    demangled = cxx_demangle_sym(sym->name, 0, 0);
    if (demangled == NULL)
        demangled = java_demangle_sym(sym->name, JAVA_DEMANGLE_NORET);
    else if (rust_is_mangled(demangled))
        /*
         * Input to Rust demangling is the BFD-demangled
         * name which it Rust-demangles in place.
         */
        rust_demangle_sym(demangled);

    if (demangled == NULL)
        sym->demangled = sym->name;
    else {
        sym_len = strlen(demangled) + 1;

        if (unlikely(!obj->demangled))
            obj->demangled = calloc(1, obj->strs_cap);

        if (obj->demangled &&
            obj->demangled_sz + sym_len <= obj->strs_cap) {
            sym->demangled = obj->demangled + obj->demangled_sz;
            memcpy((char *)sym->demangled, demangled, sym_len);
            obj->demangled_sz += sym_len;
        } else
            sym->demangled = sym->name;

        free(demangled);
    }
    return sym;
}

static const struct sym *obj__find_name(struct object *obj, const char *name)
{
    int i;

    if (!obj)
        return NULL;

    // get the object that actually stores the symbols
    obj = obj->buildid_obj;
    if (!obj->syms && obj__load_sym_table(obj))
        return NULL;

    for (i = 0; i < obj->syms_sz; i++) {
        if (strcmp(obj->syms[i].name, name) == 0)
            return obj__demangle_sym(obj, &obj->syms[i]);
    }
    return NULL;
}

static const struct sym *obj__find_offset(struct object *obj, uint64_t offset)
{
    unsigned long sym_addr;
    int start, end, mid;

    if (!obj)
        return NULL;

    // get the object that actually stores the symbols
    obj = obj->buildid_obj;
    if (!obj->syms && obj__load_sym_table(obj))
        return NULL;

    start = 0;
    end = obj->syms_sz - 1;

    /* find largest sym_addr <= addr using binary search */
    while (start < end) {
        mid = start + (end - start + 1) / 2;
        sym_addr = obj->syms[mid].start;

        if (sym_addr <= offset)
            start = mid;
        else
            end = mid - 1;
    }

    if (start == end &&
        obj->syms[start].start <= offset &&
        obj->syms[start].start + obj->syms[start].size >= offset)
        return obj__demangle_sym(obj, &obj->syms[start]);
    return NULL;
}

const struct sym *dso__find_sym(struct dso *dso, uint64_t offset)
{
    return obj__find_offset(dso->obj, offset);
}

const char *dso__name(struct dso *dso)
{
    return dso ? (dso->obj->name_atmnt ? : dso->obj->name) : NULL;
}

#ifdef HAVE_LIBUNWIND
/*
 * Lazy-load .eh_frame_hdr and .eh_frame data for DWARF unwind.
 * Called on first use to reduce memory for DSOs that never need unwinding.
 * Must enter the object's mount namespace since the ELF file may be
 * inaccessible from the current namespace.
 */
static void obj__load_unwind_info(struct object *obj)
{
    Elf *e;
    GElf_Ehdr ehdr;
    int fd, phnum, i;

    if (obj->unwind_info_loaded)
        return;
    obj->unwind_info_loaded = true;

    if (obj->type != EXEC && obj->type != DYN)
        return;

    enter_mntns(obj->mnt);
    e = open_elf(obj->name, &fd);
    exit_mntns(obj->mnt);
    if (!e)
        return;

    if (!gelf_getehdr(e, &ehdr))
        goto out;

    phnum = ehdr.e_phnum;

    /* Find first PT_LOAD for base vaddr */
    for (i = 0; i < phnum; i++) {
        GElf_Phdr phdr;
        if (!gelf_getphdr(e, i, &phdr))
            continue;
        if (phdr.p_type == PT_LOAD) {
            obj->elf_load_vaddr = phdr.p_vaddr;
            break;
        }
    }

    /* Find PT_GNU_EH_FRAME and copy its data */
    for (i = 0; i < phnum; i++) {
        GElf_Phdr phdr;
        if (!gelf_getphdr(e, i, &phdr))
            continue;
        if (phdr.p_type == PT_GNU_EH_FRAME && phdr.p_filesz > 0) {
            void *buf = malloc(phdr.p_filesz);
            if (buf) {
                if (pread(fd, buf, phdr.p_filesz, phdr.p_offset) == (ssize_t)phdr.p_filesz) {
                    obj->eh_frame_hdr = buf;
                    obj->eh_frame_hdr_sz = phdr.p_filesz;
                    obj->eh_frame_hdr_vaddr = phdr.p_vaddr;
                } else {
                    free(buf);
                }
            }
            break;
        }
    }

    /* Find PT_LOAD that contains .eh_frame (right after eh_frame_hdr usually) */
    if (obj->eh_frame_hdr) {
        /*
            * Parse eh_frame_hdr to find eh_frame location.
            * eh_frame_hdr format (version 1):
            *   byte  version
            *   byte  eh_frame_ptr_enc
            *   byte  fde_count_enc
            *   byte  table_enc
            *   encoded  eh_frame_ptr  (points to .eh_frame)
            *
            * For DW_EH_PE_sdata4|DW_EH_PE_pcrel (0x1b) encoding,
            * eh_frame_ptr is a signed 32-bit offset relative to its own location.
            */
        unsigned char *hdr = obj->eh_frame_hdr;
        if (hdr[0] == 1 && obj->eh_frame_hdr_sz >= 8) {
            unsigned char eh_frame_ptr_enc = hdr[1];
            int32_t eh_frame_off = *(int32_t *)(hdr + 4);
            uint64_t eh_frame_vaddr = 0;

            if (eh_frame_ptr_enc == 0x1b) {
                /* DW_EH_PE_sdata4 | DW_EH_PE_pcrel */
                eh_frame_vaddr = obj->eh_frame_hdr_vaddr + 4 + eh_frame_off;
            } else if (eh_frame_ptr_enc == 0x03) {
                /* DW_EH_PE_udata4 | DW_EH_PE_absptr */
                eh_frame_vaddr = *(uint32_t *)(hdr + 4);
            } else if (eh_frame_ptr_enc == 0x0b) {
                /* DW_EH_PE_sdata4 | DW_EH_PE_absptr */
                eh_frame_vaddr = (uint64_t)(int64_t)eh_frame_off;
            } else if ((eh_frame_ptr_enc & 0xf0) == 0x30 && (eh_frame_ptr_enc & 0x0f) == 0x0b) {
                /* DW_EH_PE_sdata4 | DW_EH_PE_datarel */
                eh_frame_vaddr = obj->eh_frame_hdr_vaddr + eh_frame_off;
            }

            if (eh_frame_vaddr) {
                /* Find the PT_LOAD segment containing .eh_frame */
                for (i = 0; i < phnum; i++) {
                    GElf_Phdr phdr;
                    if (!gelf_getphdr(e, i, &phdr))
                        continue;
                    if (phdr.p_type == PT_LOAD &&
                        eh_frame_vaddr >= phdr.p_vaddr &&
                        eh_frame_vaddr < phdr.p_vaddr + phdr.p_filesz) {
                        uint64_t offset_in_seg = eh_frame_vaddr - phdr.p_vaddr;
                        uint64_t file_off = phdr.p_offset + offset_in_seg;
                        uint64_t remaining = phdr.p_filesz - offset_in_seg;
                        void *buf = malloc(remaining);
                        if (buf) {
                            if (pread(fd, buf, remaining, file_off) == (ssize_t)remaining) {
                                obj->eh_frame = buf;
                                obj->eh_frame_sz = remaining;
                                obj->eh_frame_vaddr = eh_frame_vaddr;
                            } else {
                                free(buf);
                            }
                        }
                        break;
                    }
                }
            }
        }
    }

out:
    close_elf(e, fd);
}

bool dso__get_unwind_data(struct dso *dso, struct dso_unwind_data *data)
{
    struct object *obj;
    uint64_t runtime_base;

    obj = dso->obj;
    if (!obj)
        return false;

    /* Lazy-load unwind info on first access */
    if (!obj->unwind_info_loaded)
        obj__load_unwind_info(obj);

    if (!obj->eh_frame_hdr)
        return false;

    runtime_base = dso->ranges[0].start - dso->ranges[0].file_off;
    data->eh_frame_hdr = obj->eh_frame_hdr;
    data->eh_frame_hdr_sz = obj->eh_frame_hdr_sz;
    data->eh_frame_hdr_addr = runtime_base + obj->eh_frame_hdr_vaddr - obj->elf_load_vaddr;
    data->eh_frame = obj->eh_frame;
    data->eh_frame_sz = obj->eh_frame_sz;
    data->eh_frame_addr = runtime_base + obj->eh_frame_vaddr - obj->elf_load_vaddr;

    return true;
}
#endif

static struct syms *__syms__load_file(FILE *f, char *line, int size, pid_t tgid)
{
    char buf[PATH_MAX], perm[5];
    char proc_pid_exe[128];
    char deleted_bin[PATH_MAX];
    bool got = 0;
    struct syms *syms;
    struct map map;
    char *s;
    char *name;
    int ret;
    int nr_dso = 0;

    syms = calloc(1, sizeof(*syms));
    if (!syms)
        goto err_out;

    while (true) {
        s = fgets(line, size, f);
        if (!s || feof(f))
            break;

        ret = sscanf(s, "%lx-%lx %4s %lx %lx:%lx %lu%[^\n]\n",
                 &map.start_addr, &map.end_addr, perm,
                 &map.file_off, &map.dev_major,
                 &map.dev_minor, &map.inode, buf);

        if (ret != 8)   /* perf-<PID>.map */
            break;

        if (perm[2] != 'x')
            continue;

        name = buf;
        while (isspace(*name))
            name++;
        if (!is_file_backed(name))
            continue;

        ret = strlen(name);
        if (tgid && ret > 10 &&
            strncmp(name + ret - 10, " (deleted)", 10) == 0) {
            if (!got) {
                if (snprintf(proc_pid_exe, sizeof(proc_pid_exe), "/proc/%d/exe", tgid) >=
                    sizeof(proc_pid_exe))
                    continue;
                ret = readlink(proc_pid_exe, deleted_bin, sizeof(deleted_bin));
                if (ret < 0)
                    continue;
                deleted_bin[ret] = '\0';
                got = 1;
            }
            /*
             * The files deleted may be binary or library files.
             * Only binary files are supported.
             */
            if (strcmp(name, deleted_bin) == 0)
                name = proc_pid_exe;
            else
                continue;
        }

        // nr_dso == 0, avoids repeatedly loading tgid's syms, but always fails.
        if (syms__add_dso(syms, &map, name, tgid) < 0 &&
            nr_dso == 0)
            goto err_out;

        nr_dso++;
    }

    return syms;

err_out:
    syms__free(syms);
    return NULL;
}

struct syms *syms__load_file(const char *fname, pid_t tgid)
{
    FILE *f;
    struct syms *syms;
    char line[PATH_MAX];

    f = fopen(fname, "r");
    if (!f)
        return NULL;
    syms = __syms__load_file(f, line, PATH_MAX, tgid);
    fclose(f);
    return syms;
}

struct syms *syms__load_pid(pid_t tgid)
{
    char fname[128];

    snprintf(fname, sizeof(fname), "/proc/%ld/maps", (long)tgid);
    return syms__load_file(fname, tgid);
}

void syms__free(struct syms *syms)
{
    int i;

    if (!syms)
        return;

    for (i = 0; i < syms->dso_sz; i++)
        dso__free_fields(&syms->dsos[i]);
    free(syms->dsos);
    free(syms);
}

const struct sym *syms__map_addr(const struct syms *syms, unsigned long addr)
{
    struct dso *dso;
    uint64_t offset;

    dso = syms__find_dso(syms, addr, &offset);
    if (!dso)
        return NULL;
    return dso__find_sym(dso, offset);
}

/*
 * pprof --symbols <program>
 *  Maps addresses to symbol names.  In this mode, stdin should be a
 *  list of library mappings, in the same format as is found in the heap-
 *  and cpu-profile files (this loosely matches that of /proc/self/maps
 *  on linux), followed by a list of hex addresses to map, one per line.
 **/
void syms__convert(FILE *fin, FILE *fout, char *binpath)
{
    struct object *obj;
    struct syms *syms;
    char line[PATH_MAX];
    char *s;
    int ret;
    unsigned long addr;

    obj = obj__get(binpath, 0);
    if (!obj)
        return;

    syms = __syms__load_file(fin, line, PATH_MAX, 0);
    if (!syms)
        return;

    while (true) {
        ret = sscanf(line, "0x%lx\n", &addr);
        if (ret == 1) {
            struct dso *dso;
            uint64_t offset;
            dso = syms__find_dso(syms, addr, &offset);
            if (dso) {
                const struct sym *sym = dso__find_sym(dso, offset);
                if (sym) {
                    fprintf(fout, "%s+0x%lx\n", sym__name(sym), offset - sym->start);
                    goto next_line;
                }
            }
            fprintf(fout, "??\n");
        } else {
            const struct sym *sym;
            int n = strlen(line);
            if (line[n-1] == '\n')
                line[n-1] = '\0';

            sym = obj__find_name(obj, line);
            if (sym) {
                // byte offset from the beginning of the file.
                fprintf(fout, "0x%lx\n", sym->start - (obj->sh_addr - obj->sh_offset));
                goto next_line;
            }
        }

next_line:
        s = fgets(line, PATH_MAX, fin);
        if (!s && feof(fin))
            break;
    }
    syms__free(syms);
    obj__put(obj);
}

unsigned long syms__file_offset(const char *binpath, const char *func)
{
    struct object *obj;
    const struct sym *sym;

    obj = obj__get(binpath, 0);
    if (obj) {
        sym = obj__find_name(obj, func);
        if (sym)
            return sym->start - (obj->sh_addr - obj->sh_offset);
    }
    return 0;
}

struct syms_cache_node {
    struct rb_node rbnode;
    struct syms *syms;
    int tgid;
};
struct syms_cache {
    struct rblist cache;
};

static int syms_cache_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct syms_cache_node *node = container_of(rbn, struct syms_cache_node, rbnode);
    int tgid = *(const int *)entry;

    if (node->tgid > tgid)
        return 1;
    else if (node->tgid < tgid)
        return -1;
    else
        return 0;
}

static struct rb_node *syms_cache_node_new(struct rblist *rlist, const void *new_entry)
{
    int tgid = *(const int *)new_entry;
    struct syms_cache_node *node = malloc(sizeof(*node));

    if (node) {
        memset(node, 0, sizeof(*node));
        RB_CLEAR_NODE(&node->rbnode);
        node->tgid = tgid;
        node->syms = syms__load_pid(tgid);
        if (!node->syms) {
            free(node);
            return NULL;
        }
        return &node->rbnode;
    } else
        return NULL;
}

static void syms_cache_node_delete(struct rblist *rblist, struct rb_node *rbn)
{
    struct syms_cache_node *node = container_of(rbn, struct syms_cache_node, rbnode);

    syms__free(node->syms);
    free(node);
}

struct syms_cache *syms_cache__new(void)
{
    struct syms_cache *syms_cache;

    syms_cache = calloc(1, sizeof(*syms_cache));
    if (!syms_cache)
        return NULL;

    rblist__init(&syms_cache->cache);
    syms_cache->cache.node_cmp = syms_cache_node_cmp;
    syms_cache->cache.node_new = syms_cache_node_new;
    syms_cache->cache.node_delete = syms_cache_node_delete;
    return syms_cache;
}

void syms_cache__free(struct syms_cache *syms_cache)
{
    if (!syms_cache)
        return;

    rblist__exit(&syms_cache->cache);
    free(syms_cache);
}

struct syms *syms_cache__get_syms(struct syms_cache *syms_cache, int tgid)
{
    struct rb_node *rbn;
    struct syms_cache_node *node = NULL;
    struct syms *syms = NULL;

    rbn = rblist__findnew(&syms_cache->cache, &tgid);
    if (rbn) {
        node = container_of(rbn, struct syms_cache_node, rbnode);
        syms = node->syms;
    }
    return syms;
}

void syms_cache__free_syms(struct syms_cache *syms_cache, int tgid)
{
    struct rb_node *rbn;

    rbn = rblist__find(&syms_cache->cache, &tgid);
    if (rbn) {
        rblist__remove_node(&syms_cache->cache, rbn);
    }
}

void syms_cache__stat(struct syms_cache *syms_cache, FILE *fp)
{
    struct rb_node *node;
    struct syms_cache_node *cache;

    if (!syms_cache)
        return;

    if (rblist__nr_entries(&syms_cache->cache) == 0)
        return;

    fprintf(fp, "SYMS %d\n", rblist__nr_entries(&syms_cache->cache));
    for (node = rb_first_cached(&syms_cache->cache.entries); node;
        node = rb_next(node)) {
        struct dso *dso;
        int i;

        cache = rb_entry(node, struct syms_cache_node, rbnode);
        fprintf(fp, "    PID %d %s\n", cache->tgid, global_comm_get(cache->tgid) ? : "");
        for (i = 0; i < cache->syms->dso_sz; i++) {
            dso = &cache->syms->dsos[i];
            fprintf(fp, "        %s\n", dso__name(dso) ?: "");
        }
    }
}

struct partitions {
    struct partition *items;
    int sz;
};

static int partitions__add_partition(struct partitions *partitions,
                     const char *name, unsigned int dev)
{
    struct partition *partition;
    void *tmp;

    tmp = realloc(partitions->items, (partitions->sz + 1) *
        sizeof(*partitions->items));
    if (!tmp)
        return -1;
    partitions->items = tmp;
    partition = &partitions->items[partitions->sz];
    partition->name = strdup(name);
    partition->dev = dev;
    partitions->sz++;

    return 0;
}

struct partitions *partitions__load(void)
{
    char part_name[DISK_NAME_LEN];
    unsigned int devmaj, devmin;
    unsigned long long nop;
    struct partitions *partitions;
    char buf[64];
    FILE *f;

    f = fopen("/proc/partitions", "r");
    if (!f)
        return NULL;

    partitions = calloc(1, sizeof(*partitions));
    if (!partitions)
        goto err_out;

    while (fgets(buf, sizeof(buf), f) != NULL) {
        /* skip heading */
        if (buf[0] != ' ' || buf[0] == '\n')
            continue;
        if (sscanf(buf, "%u %u %llu %s", &devmaj, &devmin, &nop,
                part_name) != 4)
            goto err_out;
        if (partitions__add_partition(partitions, part_name,
                        MKDEV(devmaj, devmin)))
            goto err_out;
    }

    fclose(f);
    return partitions;

err_out:
    partitions__free(partitions);
    fclose(f);
    return NULL;
}

void partitions__free(struct partitions *partitions)
{
    int i;

    if (!partitions)
        return;

    for (i = 0; i < partitions->sz; i++)
        free(partitions->items[i].name);
    free(partitions->items);
    free(partitions);
}

const struct partition *
partitions__get_by_dev(const struct partitions *partitions, unsigned int dev)
{
    int i;

    for (i = 0; i < partitions->sz; i++) {
        if (partitions->items[i].dev == dev)
            return &partitions->items[i];
    }

    return NULL;
}

const struct partition *
partitions__get_by_name(const struct partitions *partitions, const char *name)
{
    int i;

    for (i = 0; i < partitions->sz; i++) {
        if (strcmp(partitions->items[i].name, name) == 0)
            return &partitions->items[i];
    }

    return NULL;
}

static void print_stars(unsigned int val, unsigned int val_max, int width)
{
    int num_stars, num_spaces, i;
    bool need_plus;

    num_stars = min(val, val_max) * width / val_max;
    num_spaces = width - num_stars;
    need_plus = val > val_max;

    for (i = 0; i < num_stars; i++)
        printf("*");
    for (i = 0; i < num_spaces; i++)
        printf(" ");
    if (need_plus)
        printf("+");
}

void print_log2_hist(unsigned int *vals, int vals_size, const char *val_type)
{
    int stars_max = 40, idx_max = -1, idx0_max = -1;
    unsigned int val, val_max = 0;
    unsigned long long low, high;
    int stars, width, i;

    for (i = 0; i < vals_size; i++) {
        val = vals[i];
        if (val > 0)
            idx_max = i;
        if (val > val_max)
            val_max = val;
        if (idx_max < 0)
            idx0_max = i;
    }

    if (idx_max < 0)
        return;

    printf("%*s%-*s : count    distribution\n", idx_max <= 32 ? 5 : 15, "",
        idx_max <= 32 ? 19 : 29, val_type);

    if (idx_max <= 32)
        stars = stars_max;
    else
        stars = stars_max / 2;

    for (i = 0; i <= idx_max; i++) {
        low = (1ULL << (i + 1)) >> 1;
        high = (1ULL << (i + 1)) - 1;
        if (low == high)
            low -= 1;
        if (idx0_max > i) {
            i = idx0_max;
            high = (1ULL << (i + 1)) - 1;
        }
        val = vals[i];
        width = idx_max <= 32 ? 10 : 20;
        printf("%*lld -> %-*lld : %-8d |", width, low, width, high, val);
        print_stars(val, val_max, stars);
        printf("|\n");
    }
}

void print_linear_hist(unsigned int *vals, int vals_size, unsigned int base,
               unsigned int step, const char *val_type)
{
    int i, stars_max = 40, idx_min = -1, idx_max = -1;
    unsigned int val, val_max = 0;

    for (i = 0; i < vals_size; i++) {
        val = vals[i];
        if (val > 0) {
            idx_max = i;
            if (idx_min < 0)
                idx_min = i;
        }
        if (val > val_max)
            val_max = val;
    }

    if (idx_max < 0)
        return;

    printf("     %-13s : count     distribution\n", val_type);
    for (i = idx_min; i <= idx_max; i++) {
        val = vals[i];
        printf("        %-10d : %-8d |", base + i * step, val);
        print_stars(val, val_max, stars_max);
        printf("|\n");
    }
}

unsigned long long get_ktime_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

bool is_kernel_module(const char *name)
{
    bool found = false;
    char buf[64];
    FILE *f;

    f = fopen("/proc/modules", "r");
    if (!f)
        return false;

    while (fgets(buf, sizeof(buf), f) != NULL) {
        if (sscanf(buf, "%s %*s\n", buf) != 1)
            break;
        if (!strcmp(buf, name)) {
            found = true;
            break;
        }
    }

    fclose(f);
    return found;
}

bool kprobe_exists(const char *name)
{
    char sym_name[256];
    FILE *f;
    int ret;

    f = fopen("/sys/kernel/debug/tracing/available_filter_functions", "r");
    if (!f)
        goto slow_path;

    while (true) {
        ret = fscanf(f, "%s%*[^\n]\n", sym_name);
        if (ret == EOF && feof(f))
            break;
        if (ret != 1) {
            fprintf(stderr, "failed to read symbol from available_filter_functions\n");
            break;
        }
        if (!strcmp(name, sym_name)) {
            fclose(f);
            return true;
        }
    }

    fclose(f);
    return false;

slow_path:
    f = fopen("/proc/kallsyms", "r");
    if (!f)
        return false;

    while (true) {
        ret = fscanf(f, "%*x %*c %s%*[^\n]\n", sym_name);
        if (ret == EOF && feof(f))
            break;
        if (ret != 1) {
            fprintf(stderr, "failed to read symbol from kallsyms\n");
            break;
        }
        if (!strcmp(name, sym_name)) {
            fclose(f);
            return true;
        }
    }

    fclose(f);
    return false;
}

bool vmlinux_btf_exists(void)
{
    if (!access("/sys/kernel/btf/vmlinux", R_OK))
        return true;
    return false;
}

bool module_btf_exists(const char *mod)
{
    char sysfs_mod[80];

    if (mod) {
        snprintf(sysfs_mod, sizeof(sysfs_mod), "/sys/kernel/btf/%s", mod);
        if (!access(sysfs_mod, R_OK))
            return true;
    }
    return false;
}
