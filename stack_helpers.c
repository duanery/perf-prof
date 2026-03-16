#ifdef HAVE_LIBPYTHON
#include <Python.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <linux/const.h>
#include <linux/refcount.h>
#include <linux/rblist.h>
#include <monitor.h>
#include <tep.h>
#include <trace_helpers.h>
#include <stack_helpers.h>
#include "dwarf_unwind.h"

#define ALIGN(x, a)  __ALIGN_KERNEL((x), (a))

static struct global_syms {
    struct ksyms *ksyms;
    struct syms_cache *syms_cache;
    refcount_t ksyms_ref;
    refcount_t syms_ref;
    struct comm_notify notify;
    int dwarf_ref;
} ctx;

/*
 * ffffffff81ad6db9 system_call_fastpath+0x16 ([kernel.kallsyms])
 * 00007efd88d7ea20 __write_nocancel+0x7 (/usr/lib64/libc-2.17.so)
 * addr             symbol  +offset      (dso)
 *
 * addr symbol+offset (dso)...addr symbol+offset (dso)
 *                         ^                          ^
 *                         `seperate                  `end
**/
struct callchain_ctx {
    u64 kernel      : 1, /* need kernel symbols, /proc/kallsyms */
        user        : 1, /* need user symbols, /proc/pid/maps */
        dwarf       : 1, /* need dwarf unwind, need user symbols */
        addr        : 1, /* print addr */
        symbol      : 1, /* print symbol */
        offset      : 1, /* print +offset */
        dso         : 1, /* print (dso) */
        reverse     : 1, /* reverse, down to top */
        debug       : 1, /* debug, print PERF_CONTEXT_* */
        print2string_kernel : 1, /* convert to string, for kernel callchain */
        print2string_user   : 1; /* convert to string, for user callchain */
    char seperate;
    char end;
    FILE *fout;
};

static int task_exit_free_syms(struct comm_notify *notify, int pid, int state, u64 free_time)
{
    struct global_syms *g = container_of(notify, struct global_syms, notify);
    if (g->syms_cache) {
        syms_cache__free_syms(g->syms_cache, pid);
    }
    return 0;
}

static int global_syms_ref(bool kernel, bool user, bool dwarf)
{
    if (kernel) {
        if (!ctx.ksyms) {
            ctx.ksyms = ksyms__load();
            if (!ctx.ksyms)
                return -1;
            /*
             * Temporarily disable ksymbol_dev_open to improve startup performance
             * Opening this 'ksymbol_dev' causes perf-prof startup slowdown.
             * This removes kernel symbol change monitoring capability but
             * significantly improves performance for most use cases.
             */
            //ksymbol_dev_open(ctx.ksyms);
            refcount_set(&ctx.ksyms_ref, 1);
        } else
            refcount_inc(&ctx.ksyms_ref);
    }
    if (user) {
        if (!ctx.syms_cache) {
            ctx.syms_cache = syms_cache__new();
            if (!ctx.syms_cache)
                return -1;
            refcount_set(&ctx.syms_ref, 1);
            ctx.notify.notify = task_exit_free_syms;
            global_comm_register_notify(&ctx.notify);
        } else
            refcount_inc(&ctx.syms_ref);
    }
    if (dwarf && ctx.dwarf_ref++ == 0)
        dwarf_unwind_init();
    return 0;
}

static void global_syms_unref(bool kernel, bool user, bool dwarf)
{
    if (kernel && ctx.ksyms && refcount_dec_and_test(&ctx.ksyms_ref)) {
        //ksymbol_dev_close();
        ksyms__free(ctx.ksyms);
        ctx.ksyms = NULL;
    }
    if (user && ctx.syms_cache && refcount_dec_and_test(&ctx.syms_ref)) {
        global_comm_unregister_notify(&ctx.notify);
        syms_cache__free(ctx.syms_cache);
        ctx.syms_cache = NULL;
    }
    if (dwarf && --ctx.dwarf_ref == 0)
        dwarf_unwind_exit();
}

void global_syms_stat(FILE *fp)
{
    if (ctx.syms_cache) {
        obj__stat(fp);
        syms_cache__stat(ctx.syms_cache, fp);
    }
}

void function_resolver_ref(void)
{
    global_syms_ref(true, false, false);
}

void function_resolver_unref(void)
{
    global_syms_unref(true, false, false);
}

char *function_resolver(void *priv, unsigned long long *addrp, char **modp)
{
    unsigned long addr = *(unsigned long *)addrp;
    if (ctx.ksyms && addr >= START_OF_KERNEL) {
        const struct ksym *ksym = ksyms__map_addr(ctx.ksyms, addr);
        if (ksym) {
            *addrp = ksym->addr;
            return (char *)ksym->name;
        }
    }
    return NULL;
}

static void callchain_ctx_debug_init(struct callchain_ctx *cc, bool kernel, bool user, FILE *fout)
{
    cc->kernel = kernel;
    cc->user   = user;
    cc->addr   = 1;
    cc->symbol = 1;
    cc->offset = 1;
    cc->dso    = 1;
    cc->reverse = 0;
    cc->debug  = 1;
    cc->seperate = '\n';
    cc->end = '\n';
    cc->fout = fout;
}

struct callchain_ctx *callchain_ctx_new(int flags, FILE *fout)
{
    struct callchain_ctx *cc;
    bool kernel = flags & CALLCHAIN_KERNEL;
    bool user   = flags & CALLCHAIN_USER;
    bool dwarf  = flags & CALLCHAIN_DWARF;

    if (kernel == false && user == false)
        return NULL;

    if (dwarf && !user)
        return NULL;

    if (global_syms_ref(kernel, user, dwarf) < 0)
        return NULL;

    cc = calloc(1, sizeof(*cc));
    if (!cc)
        return NULL;

    cc->kernel = kernel;
    cc->user   = user;
    cc->dwarf  = dwarf;
    cc->addr   = 1;
    cc->symbol = 1;
    cc->offset = 1;
    cc->dso    = 1;
    cc->reverse = 0;
    cc->seperate = '\n';
    cc->end = '\n';
    cc->fout = fout;
    return cc;
}

void callchain_ctx_config(struct callchain_ctx *cc, bool addr, bool symbol, bool offset,
        bool dso, bool reverse, char sep, char end)
{
    if (!cc)
        return ;
    if (!(addr || symbol || offset || dso))
        return ;
    cc->addr   = addr;
    cc->symbol = symbol;
    cc->offset = offset;
    cc->dso    = dso;
    cc->reverse = reverse;
    cc->seperate = sep;
    cc->end = end;
}

void callchain_ctx_free(struct callchain_ctx *cc)
{
    if (!cc)
        return ;
    global_syms_unref(cc->kernel, cc->user, cc->dwarf);
    free(cc);
}

static int __print_callchain_kernel(struct callchain_ctx *cc, u64 ip, bool *printed)
{
    int len = 0, len_sep = 0;
    if (*printed)
        len_sep += fprintf(cc->fout, "%c", cc->seperate);
    if (cc->print2string_kernel)
        len += fprintf(cc->fout, "%s", (char *)ip);
    else {
        const struct ksym *ksym = cc->kernel ? ksyms__map_addr(ctx.ksyms, ip) : NULL;
        if (cc->addr)
            len += fprintf(cc->fout, "    %016lx", ip);
        if (cc->symbol) {
            if (ksym || cc->addr) {
                len += fprintf(cc->fout, "%s%s", len ? " " : "", ksym ? ksym->name : "Unknown");
                if (cc->offset)
                    len += fprintf(cc->fout, "+0x%lx", ksym ? ip - ksym->addr : 0L);
            } else // Symbol not found.
                len += fprintf(cc->fout, "Unknown");
        }
        if (cc->dso)
            len += fprintf(cc->fout, "%s([kernel.kallsyms])", len ? " " : "");
    }
    *printed = len > 0;
    return len_sep + len;
}

static int __print_callchain_user(struct callchain_ctx *cc, struct syms *syms, u64 ip, bool *printed)
{
    struct dso *dso = NULL;
    const struct sym *sym = NULL;
    const char *symbol = "Unknown";
    u64 offset = 0L;
    const char *dso_name = "Unknown";
    int len = 0, len_sep = 0;

    if (!cc->print2string_user && syms) {
        dso = syms__find_dso(syms, ip, &offset);
        if (dso) {
            sym = dso__find_sym(dso, offset);
            if (sym) {
                symbol = sym__name(sym);
                offset = offset - sym->start;
                dso_name = dso__name(dso)?:"Unknown";
            }
        }
    }
    if (*printed)
        len_sep += fprintf(cc->fout, "%c", cc->seperate);
    if (cc->print2string_user)
        len += fprintf(cc->fout, "%s", (char *)ip);
    else {
        if (cc->addr)
            len += fprintf(cc->fout, "    %016lx", ip);
        if (cc->symbol) {
            if (sym || cc->addr) {
                len += fprintf(cc->fout, "%s%s", len ? " " : "", symbol);
                if (cc->offset)
                    len += fprintf(cc->fout, "+0x%lx", offset);
            } else // Symbol not found.
                len += fprintf(cc->fout, "Unknown");
        }
        if (cc->dso)
            len += fprintf(cc->fout, "%s(%s)", len ? " " : "", dso_name);
    }
    *printed = len > 0;
    return len_sep + len;
}

static int __print_callchain_py(struct callchain_ctx *cc, u64 ip, bool *printed)
{
    int len = 0, len_sep = 0;
    if (*printed)
        len_sep += fprintf(cc->fout, "%c", cc->seperate);
    if (cc->print2string_user)
        len += fprintf(cc->fout, "%s", (char *)ip);
    else {
        char *str = (char *)ip;
        char *dso = strchr(str, '(');
        if (cc->addr && (cc->symbol || cc->offset || cc->dso))
            len += fprintf(cc->fout, "    %16s", "");
        if (cc->symbol || cc->offset) {
            int n = dso ? dso - str - 1 : strlen(str);
            len += fprintf(cc->fout, "%s%.*s", len ? " " : "", n, str);
        }
        if (cc->dso)
            len += fprintf(cc->fout, "%s%s", len ? " " : "", dso);
    }
    *printed = len > 0;
    return len_sep + len;
}


static bool __print_callchain(struct callchain_ctx *cc, struct callchain *callchain, u32 pid)
{
    u64 i;
    bool kernel = false, user = false, py = false, printed = false;
    struct syms *syms = NULL;
    int len = 0;

    for (i = 0; i < callchain->nr; i++) {
        u64 ip = callchain->ips[i];
        if (ip == PERF_CONTEXT_KERNEL) {
            kernel = true;
            user = py = false;
            continue;
        } else if (ip == PERF_CONTEXT_USER) {
            user = true;
            kernel = py = false;
            if (ctx.syms_cache && cc->user && !cc->print2string_user)
                syms = syms_cache__get_syms(ctx.syms_cache, pid);
            continue;
        } else if (ip == PERF_CONTEXT_PYSTACK) {
            py = true;
            kernel = user = false;
            continue;
        }

        if (kernel) {
            len += __print_callchain_kernel(cc, ip, &printed);
        } else if (user) {
            len += __print_callchain_user(cc, syms, ip, &printed);
        } else if (py)
            len += __print_callchain_py(cc, ip, &printed);
    }
    return len > 0;
}

static bool __print_callchain_reverse(struct callchain_ctx *cc, struct callchain *callchain, u32 pid)
{
    u64 i, kstart = 0, kend = 0, ustart = 0, uend = 0, pystart = 0, pyend = 0;
    struct syms *syms = NULL;
    bool printed = false;
    int len = 0;

    for (i = 0; i < callchain->nr; i++) {
        u64 ip = callchain->ips[i];
        if (ip == PERF_CONTEXT_KERNEL) {
            kstart = i + 1;
            kend = callchain->nr - 1;
            if (!cc->user)
                break;
        } else if (ip == PERF_CONTEXT_USER) {
            if (ctx.syms_cache && cc->user && !cc->print2string_user)
                syms = syms_cache__get_syms(ctx.syms_cache, pid);
            kend = i - 1;
            ustart = i + 1;
            uend = callchain->nr - 1;
        } else if (ip == PERF_CONTEXT_PYSTACK) {
            uend = i - 1;
            pystart = i + 1;
            pyend = callchain->nr - 1;
            break;
        }
    }
    if (pystart) {
        for (; pyend >= pystart; pyend--) {
            u64 ip = callchain->ips[pyend];
            // There may be more than 1 PERF_CONTEXT_* tag.
            if (ip > PERF_CONTEXT_MAX)
                continue;
            len += __print_callchain_py(cc, ip, &printed);
        }
    }
    if (ustart) {
        for (; uend >= ustart; uend--) {
            u64 ip = callchain->ips[uend];
            // There may be more than 1 PERF_CONTEXT_* tag.
            if (ip > PERF_CONTEXT_MAX)
                continue;
            len += __print_callchain_user(cc, syms, ip, &printed);
        }
    }
    if (kstart) {
        for (; kend >= kstart; kend--) {
            u64 ip = callchain->ips[kend];
            // There may be more than 1 PERF_CONTEXT_* tag.
            if (ip > PERF_CONTEXT_MAX)
                continue;
            len += __print_callchain_kernel(cc, ip, &printed);
        }
    }
    return len > 0;
}

void print_callchain(struct callchain_ctx *cc, struct callchain *callchain, u32 pid)
{
    if (cc == NULL ||
        callchain == NULL || callchain->nr == 0)
        return ;
    if (ctx.ksyms == NULL &&
        ctx.syms_cache == NULL)
        return ;

    if ((cc->reverse ? __print_callchain_reverse : __print_callchain)(cc, callchain, pid))
        fprintf(cc->fout, "%c", cc->end);
}

void print_callchain_common_cbs(struct callchain_ctx *cc, struct callchain *callchain, u32 pid,
                                callchain_cbs kernel_cb, callchain_cbs user_cb, void *opaque)
{
    __u64 i;
    bool kernel = false, user = false, py = false;
    struct syms *syms = NULL;

    if (cc == NULL ||
        callchain == NULL || callchain->nr == 0)
        return ;
    if (ctx.ksyms == NULL &&
        ctx.syms_cache == NULL)
        return ;

    for (i = 0; i < callchain->nr; i++) {
        __u64 ip = callchain->ips[i];
        if (ip == PERF_CONTEXT_KERNEL) {
            kernel = true;
            user = py = false;
            if (kernel_cb)
                kernel_cb(opaque, PERF_CONTEXT_KERNEL);
            if (cc->debug)
                fprintf(cc->fout, "    %016llx PERF_CONTEXT_KERNEL\n", ip);
            continue;
        } else if (ip == PERF_CONTEXT_USER) {
            user = true;
            kernel = py = false;
            if (ctx.syms_cache && cc->user) {
                syms = syms_cache__get_syms(ctx.syms_cache, pid);
            }
            if (user_cb)
                user_cb(opaque, PERF_CONTEXT_USER);
            if (cc->debug)
                fprintf(cc->fout, "    %016llx PERF_CONTEXT_USER\n", ip);
            continue;
        } else if (ip == PERF_CONTEXT_PYSTACK) {
            py = true;
            kernel = user = false;
            if (cc->debug)
                fprintf(cc->fout, "    %016llx PERF_CONTEXT_PYSTACK\n", ip);
            continue;
        }

        if (kernel) {
            const struct ksym *ksym = cc->kernel ? ksyms__map_addr(ctx.ksyms, ip) : NULL;
            fprintf(cc->fout, "    %016llx %s+0x%llx ([kernel.kallsyms])\n", ip, ksym ? ksym->name : "Unknown",
                                ksym ? ip - ksym->addr : 0L);
        } else if (user) {
            struct dso *dso = NULL;
            uint64_t offset;
            if (syms)
                dso = syms__find_dso(syms, ip, &offset);
            if (dso) {
                const struct sym *sym = dso__find_sym(dso, offset);
                fprintf(cc->fout, "    %016llx %s+0x%lx (%s)\n", ip, sym ? sym__name(sym) : "Unknown",
                                sym ? offset - sym->start : 0L, dso__name(dso)?:"Unknown");
            } else
                fprintf(cc->fout, "    %016llx Unknown\n", ip);
        } else if (py)
            fprintf(cc->fout, "    %16s %s\n", "", (char *)ip);
        else
            fprintf(cc->fout, "    %016llx\n", ip);
    }
}

/*
 * Build a combined callchain from kernel FP-based and DWARF-unwound user stacks.
 *
 * The kernel samples both FP-based user callchain and DWARF data (regs + stack).
 * This function synthesizes a new callchain:
 *   1. Copy the kernel part (up to PERF_CONTEXT_USER)
 *   2. Perform DWARF unwinding for the user part
 *   3. Compare DWARF frames with FP frames: if they match, append remaining
 *      FP frames. This handles Go binaries that have frame pointers but lack
 *      DWARF unwind info, where DWARF stops early but FP can go further.
 *
 * If DWARF data is unavailable or unwinding fails, returns the original
 * FP-based callchain unchanged.
 */
static inline
struct callchain *build_callchain(struct callchain *synth, u64 nr, struct callchain_data *data)
{
    struct callchain *callchain = data->callchain;
    struct syms *syms = NULL;
    int i, user, end;

    if (data->pid > 0 && ctx.syms_cache &&
        data->regs && data->regs_abi != PERF_SAMPLE_REGS_ABI_NONE &&
        data->stack && data->stack_sz) {

        syms = syms_cache__get_syms(ctx.syms_cache, data->pid);
        if (!syms)
            goto out;

        /* Copy kernel part of callchain (up to PERF_CONTEXT_USER) */
        synth->nr = 0;
        if (callchain) {
            for (i = 0; i < callchain->nr && synth->nr < nr; i++) {
                u64 ip = callchain->ips[i];
                synth->ips[synth->nr++] = ip;
                if (ip == PERF_CONTEXT_USER)
                    break;
            }
        }

        /* If no PERF_CONTEXT_USER found, add it */
        if (synth->nr < nr &&
            (synth->nr == 0 || synth->ips[synth->nr - 1] != PERF_CONTEXT_USER)) {
            synth->ips[synth->nr++] = PERF_CONTEXT_USER;
        }

        if (synth->nr >= nr)
            goto out;

        /* DWARF unwind for user space */
        user = dwarf_unwind_user(syms, data->regs, data->stack, data->stack_sz,
                        (u64 *)&synth->ips[synth->nr], nr - synth->nr);
        if (user <= 0)
            goto out;

        synth->nr += user;
        if (callchain) {
            end = synth->nr < callchain->nr ? synth->nr : callchain->nr;

            /* Compare kernel FP-based user callchain with DWARF-unwound.
             * If they match, copy the remaining entries from the FP callchain. */
            for (i = synth->nr - user; i < end; i++) {
                if (synth->ips[i] != callchain->ips[i])
                    break;
            }
            if (i == synth->nr && synth->nr < callchain->nr) {
                for (; i < callchain->nr && synth->nr < nr; i++)
                    synth->ips[synth->nr++] = callchain->ips[i];
            }
        }

        callchain = synth;
    }
out:
    return callchain;
}

void print_callchain_data_cbs(struct callchain_ctx *cc, struct callchain_data *data,
                              callchain_cbs kernel_cb, callchain_cbs user_cb, void *opaque)
{
    struct {
        __u64 nr;
        __u64 ips[PERF_MAX_STACK_DEPTH + PERF_MAX_CONTEXTS_PER_STACK];
    } synth;
    struct callchain *callchain;

    if (!data)
        return ;

    if (cc && cc->dwarf)
        callchain = build_callchain((struct callchain *)&synth,
                    PERF_MAX_STACK_DEPTH + PERF_MAX_CONTEXTS_PER_STACK, data);
    else
        callchain = data->callchain;

    print_callchain_common_cbs(cc, callchain, data->pid, kernel_cb, user_cb, opaque);
}

static void print2string_callchain(struct callchain_ctx *cc, struct callchain *callchain, u32 pid,
                                          int *context_kernel_num, int *context_user_num)
{
    __u64 i;
    bool kernel = false, user = false, py = false;
    struct syms *syms = NULL;
    char buff[1024];
    int len = 0;

    if (cc == NULL ||
        callchain == NULL || callchain->nr == 0)
        return ;
    if (!cc->print2string_kernel && !cc->print2string_user)
        return ;
    if (ctx.ksyms == NULL &&
        ctx.syms_cache == NULL)
        return ;

    *context_kernel_num = 0;
    *context_user_num = 0;
    for (i = 0; i < callchain->nr; i++) {
        u64 ip = callchain->ips[i];
        if (ip == PERF_CONTEXT_KERNEL) {
            kernel = true;
            user = py = false;
            if (i + 1 < callchain->nr)
                (*context_kernel_num) ++;
            continue;
        } else if (ip == PERF_CONTEXT_USER) {
            user = true;
            kernel = py = false;
            if (ctx.syms_cache && cc->user) {
                syms = syms_cache__get_syms(ctx.syms_cache, pid);
            }
            if (i + 1 < callchain->nr)
                (*context_user_num) ++;
            continue;
        } else if (ip == PERF_CONTEXT_PYSTACK) {
            py = true;
            kernel = user = false;
            continue;
        }

        if (kernel && cc->print2string_kernel) {
            const struct ksym *ksym = cc->kernel ? ksyms__map_addr(ctx.ksyms, ip) : NULL;
            len = 0;
            if (cc->addr)
                len += snprintf(buff+len, sizeof(buff)-len, "    %016lx", ip);
            if (cc->symbol) {
                len += snprintf(buff+len, sizeof(buff)-len, "%s%s", len ? " " : "", ksym ? ksym->name : "Unknown");
                if (cc->offset)
                    len += snprintf(buff+len, sizeof(buff)-len, "+0x%lx", ksym ? ip - ksym->addr : 0L);
            }
            if (cc->dso)
                len += snprintf(buff+len, sizeof(buff)-len, "%s([kernel.kallsyms])", len ? " " : "");
            // Convert to unique string.
            callchain->ips[i] = (__u64)(void *)unique_string(buff);
        } else if (user && cc->print2string_user) {
            struct dso *dso;
            const char *symbol = "Unknown";
            u64 offset = 0L;
            const char *dso_name = "Unknown";

            if (syms) {
                dso = syms__find_dso(syms, ip, &offset);
                if (dso) {
                    const struct sym *sym = dso__find_sym(dso, offset);
                    if (sym) {
                        symbol = sym__name(sym);
                        offset = offset - sym->start;
                        dso_name = dso__name(dso)?:"Unknown";
                    }
                }
            }
            len = 0;
            if (cc->addr)
                len += snprintf(buff+len, sizeof(buff)-len, "    %016lx", ip);
            if (cc->symbol) {
                len += snprintf(buff+len, sizeof(buff)-len, "%s%s", len ? " " : "", symbol);
                if (cc->offset)
                    len += snprintf(buff+len, sizeof(buff)-len, "+0x%lx", offset);
            }
            if (cc->dso)
                len += snprintf(buff+len, sizeof(buff)-len, "%s(%s)", len ? " " : "", dso_name);
            // Convert to unique string.
            callchain->ips[i] = (__u64)(void *)unique_string(buff);
        } else if (py && cc->print2string_user) {
            char *s = strchr((char *)ip, '(');
            int len = s ? s - (char *)ip - 1 : 0;
            callchain->ips[i] = (__u64)(void *)unique_string_len((char *)ip, len);
        }
    }
}


struct key_value {
    /* void *value;
     * The value is placed at the beginning of the key_value structure, and the
     * key_value pointer and the value pointer can be converted to each other.
     *
     *   void *value = (void *)(struct key_value *)kv - pairs->value_size;
     *   struct key_value *kv = (void *)value + pairs->value_size;
     */
    struct rb_node rbnode;
    unsigned int n;
    struct_key key;
};

struct key_value_paires {
    struct rblist kv_pairs;
    int value_size;
};

static int key_value_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct key_value *kv = container_of(rbn, struct key_value, rbnode);
    const struct_key *key = entry;
    int i = 0, j = 0;

    /*
     * In the flame_graph_add_callchain function, the PERF_CONTEXT_FLAME_GRAPH
     * will be added, which can be sorted by time(ips[1]).
    **/
    for (; i < (int)kv->key.nr && j < (int)key->nr; i++, j++) {
        if (kv->key.ips[i] > key->ips[j])
            return 1;
        else if (kv->key.ips[i] < key->ips[j])
            return -1;
    }
    return (int)kv->key.nr - (int)key->nr;
}

static struct rb_node *key_value_node_new(struct rblist *rlist, const void *new_entry)
{
    struct key_value_paires *pairs = container_of(rlist, struct key_value_paires, kv_pairs);
    const struct_key *key = new_entry;
    void *value = malloc(pairs->value_size + sizeof(struct key_value) + key->nr * sizeof(key->ips[0]));
    if (value) {
        struct key_value *kv = value + pairs->value_size;
        RB_CLEAR_NODE(&kv->rbnode);
        kv->n = 0;
        kv->key.nr = key->nr;
        memcpy(kv->key.ips, key->ips, key->nr * sizeof(key->ips[0]));
        memset(value, 0, pairs->value_size);
        return &kv->rbnode;
    } else
        return NULL;
}

static void key_value_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct key_value_paires *pairs = container_of(rblist, struct key_value_paires, kv_pairs);
    struct key_value *kv = container_of(rb_node, struct key_value, rbnode);
    void *value = (void *)kv - pairs->value_size;
    free(value);
}

struct key_value_paires *keyvalue_pairs_new(int value_size)
{
    struct key_value_paires *pairs;
    pairs = calloc(1, sizeof(*pairs));
    if (!pairs)
        return NULL;

    rblist__init(&pairs->kv_pairs);
    pairs->kv_pairs.node_cmp = key_value_node_cmp;
    pairs->kv_pairs.node_new = key_value_node_new;
    pairs->kv_pairs.node_delete = key_value_node_delete;
    pairs->value_size = ALIGN(value_size, 8);
    return pairs;
}

void keyvalue_pairs_free(struct key_value_paires *pairs)
{
    if (!pairs)
        return ;
    rblist__exit(&pairs->kv_pairs);
    free(pairs);
}

void *keyvalue_pairs_add_key(struct key_value_paires *pairs, struct_key *key)
{
    struct rb_node *rbn;
    struct key_value *kv = NULL;
    void *value = NULL;

    if (!pairs)
        return NULL;

    rbn = rblist__findnew(&pairs->kv_pairs, key);
    if (rbn) {
        kv = container_of(rbn, struct key_value, rbnode);
        kv->n ++;
        value = pairs->value_size ? (void *)kv - pairs->value_size : NULL;
    }
    return value;
}

void *keyvalue_pairs_add_callchain(struct key_value_paires *pairs, struct callchain_data *data)
{
    struct {
        __u64 nr;
        __u64 ips[PERF_MAX_STACK_DEPTH + PERF_MAX_CONTEXTS_PER_STACK];
    } synth;
    struct callchain *callchain = NULL;

    if (data)
        callchain = build_callchain((struct callchain *)&synth,
                    PERF_MAX_STACK_DEPTH + PERF_MAX_CONTEXTS_PER_STACK, data);

    return callchain ? keyvalue_pairs_add_key(pairs, callchain) : NULL;
}

void keyvalue_pairs_foreach(struct key_value_paires *pairs, foreach_keyvalue f, void *opaque)
{
    struct rblist *rblist;
    struct rb_node *pos, *next;
    struct key_value *kv = NULL;
    void *value = NULL;

    if (!pairs)
        return ;

    rblist = &pairs->kv_pairs;
    next = rb_first_cached(&rblist->entries);

    while (next) {
        pos = next;
        next = rb_next(pos);
        kv = container_of(pos, struct key_value, rbnode);
        value = pairs->value_size ? (void *)kv - pairs->value_size : NULL;
        f(opaque, &kv->key, value, kv->n);
    }
}

static int kv_n_cmp(const void *k1, const void *k2)
{
    struct key_value *kv1 = *(struct key_value **)k1;
    struct key_value *kv2 = *(struct key_value **)k2;
    return kv2->n - kv1->n; // Sort from largest to smallest
}

void keyvalue_pairs_sorted_firstn(struct key_value_paires *pairs, keyvalue_cmp cmp, foreach_keyvalue f, void *opaque, unsigned int n)
{
    struct rblist *rblist;
    struct rb_node *pos, *next;
    struct key_value *kv = NULL;
    void *value = NULL;
    void **sorted_values = NULL;
    unsigned int nr = 0, i;
    int cmp_value_size;

    if (!pairs || rblist__empty(&pairs->kv_pairs))
        return;

    rblist = &pairs->kv_pairs;
    next = rb_first_cached(&rblist->entries);

    sorted_values = calloc(rblist__nr_entries(rblist), sizeof(*sorted_values));
    if (!sorted_values)
        return;

    cmp_value_size = cmp ? pairs->value_size : 0;
    while (next) {
        pos = next;
        next = rb_next(pos);
        kv = container_of(pos, struct key_value, rbnode);
        value = (void *)kv - cmp_value_size;
        sorted_values[nr++] = value;
    }

    qsort(sorted_values, nr, sizeof(*sorted_values), cmp_value_size ? (__compar_fn_t)cmp : kv_n_cmp);

    if (n && nr > n)
        nr = n;
    for (i = 0; i < nr; i++) {
        kv = sorted_values[i] + cmp_value_size;
        value = pairs->value_size ? (void *)kv - pairs->value_size : NULL;
        f(opaque, &kv->key, value, kv->n);
    }
    free(sorted_values);
}

void keyvalue_pairs_sorted_foreach(struct key_value_paires *pairs, keyvalue_cmp cmp, foreach_keyvalue f, void *opaque)
{
    keyvalue_pairs_sorted_firstn(pairs, cmp, f, opaque, 0);
}

void keyvalue_pairs_reinit(struct key_value_paires *pairs)
{
    if (pairs) {
        rblist__exit(&pairs->kv_pairs);
    }
}

unsigned int keyvalue_pairs_nr_entries(struct key_value_paires *pairs)
{
    if (pairs)
        return rblist__nr_entries(&pairs->kv_pairs);
    else
        return 0;
}

static bool keyvalue_pairs_empty(struct key_value_paires *pairs)
{
    return !pairs || rblist__empty(&pairs->kv_pairs);
}

struct unique_string {
    struct rb_node rbnode;
    unsigned int n, len;
    char str[0];
};
struct unique_string_tmp {
    const char *str;
    int len;
};

static int unique_string_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct unique_string *s = container_of(rbn, struct unique_string, rbnode);
    struct unique_string_tmp *tmp = (void *)entry;

    if (s->len == tmp->len)
        return strncmp(s->str, tmp->str, tmp->len);
    else
        return s->len - tmp->len;
}
static struct rb_node *unique_string_node_new(struct rblist *rlist, const void *new_entry)
{
    struct unique_string_tmp *tmp = (void *)new_entry;
    struct unique_string *s = malloc(sizeof(struct unique_string) + tmp->len + 1);
    if (s) {
        RB_CLEAR_NODE(&s->rbnode);
        s->n = 0;
        s->len = tmp->len;
        strncpy(s->str, tmp->str, s->len);
        s->str[s->len] = '\0';
        return &s->rbnode;
    } else
        return NULL;
}
static void unique_string_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct unique_string *s = container_of(rb_node, struct unique_string, rbnode);
    free(s);
}

static struct rblist unique_strings = {
    .entries = RB_ROOT_CACHED,
    .nr_entries = 0,
    .node_cmp = unique_string_node_cmp,
    .node_new = unique_string_node_new,
    .node_delete = unique_string_node_delete,
};

const char *unique_string_len(const char *str, int len)
{
    struct rb_node *rbn;
    struct unique_string *s = NULL;
    struct unique_string_tmp tmp;

    tmp.str = str;
    tmp.len = len ? : strlen(str);
    rbn = rblist__findnew(&unique_strings, &tmp);
    if (rbn) {
        s = container_of(rbn, struct unique_string, rbnode);
        s->n ++;
        return s->str;
    }
    return NULL;
}

void unique_string_stat(FILE *fp)
{
    struct rb_node *pos, *next = rb_first_cached(&unique_strings.entries);
    struct unique_string *s = NULL;
    size_t str_len = 0, node_len = 0;

    while (next) {
        pos = next;
        next = rb_next(pos);
        s = container_of(pos, struct unique_string, rbnode);
        str_len += s->len + 1;
        node_len += sizeof(*s);
    }
    fprintf(fp, "UNIQUE STRING STAT: strlen %lu, nodelen %lu\n", str_len, node_len);
}


struct flame_graph {
    struct callchain_ctx *cc;
    struct key_value_paires *kv_pairs;
    char *filename;
    bool special;
};

static inline bool special_file(mode_t mode)
{
    return S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode) || S_ISSOCK(mode);
}

struct flame_graph *flame_graph_new(int flags, FILE *fout)
{
    struct flame_graph *fg = malloc(sizeof(*fg));
    struct callchain_ctx *cc = callchain_ctx_new(flags, fout);
    struct key_value_paires *kv_pairs = keyvalue_pairs_new(0);
    struct stat buf;

    if (!fg || !cc || !kv_pairs) {
        free(fg);
        callchain_ctx_free(cc);
        keyvalue_pairs_free(kv_pairs);
        return NULL;
    }

    if (fout != stdout && fout != stderr &&
        fstat(fileno(fout), &buf) == 0 &&
        special_file(buf.st_mode)) {
        fg->special = true;

        cc->addr   = 0;
        cc->symbol = 1;
        cc->offset = 1;
        cc->dso    = 0;
        cc->reverse = 0;
        cc->seperate = '\n';
        cc->end = '\n';
    } else {
        fg->special = false;

        cc->addr   = 0;
        cc->symbol = 1;
        cc->offset = 0;
        cc->dso    = 0;
        cc->reverse = 1;
        cc->seperate = ';';
        cc->end = ' ';
    }

    cc->print2string_kernel = 1;
    cc->print2string_user = 1;

    fg->cc = cc;
    fg->kv_pairs = kv_pairs;
    fg->filename = NULL;
    return fg;
}

void flame_graph_free(struct flame_graph *fg)
{
    if (!fg)
        return ;

    callchain_ctx_free(fg->cc);
    keyvalue_pairs_free(fg->kv_pairs);
    free(fg);
}

void flame_graph_add_callchain_at_time(struct flame_graph *fg, struct callchain_data *data,
                                         u32 pid, const char *comm,
                                         u64 time, const char *time_str)
{
    struct {
        __u64   nr;
        __u64   ips[PERF_MAX_STACK_DEPTH + PERF_MAX_CONTEXTS_PER_STACK + 5];
    } key;
    struct callchain *callchain;
    __u64 callchain_nr = 0;
    char buff[128];
    int context_kernel_num = 0;
    int context_user_num = 0;

    if (!fg || !data)
        return ;

    if (fg->cc && fg->cc->dwarf) {
        /*
         * Build DWARF-unwound callchain directly into key to avoid extra copy.
         *
         * Without time: synth = &key, build_callchain writes nr and ips directly
         *   into key. Zero-copy: key.nr = synth->nr, key.ips = synth->ips.
         * With time: key.ips[0] = PERF_CONTEXT_FLAME_GRAPH, synth = &key.ips[1]
         *   where key.ips[1] temporarily holds synth->nr and key.ips[2..] holds
         *   synth->ips. On success, save synth->nr then restore key.ips[1] = time.
         */
        struct callchain *synth;

        if (time)
            synth = (struct callchain *)&key.ips[1];
        else
            synth = (struct callchain *)&key;

        callchain = build_callchain(synth, PERF_MAX_STACK_DEPTH + PERF_MAX_CONTEXTS_PER_STACK, data);
        /* DWARF succeeded, data built directly into key */
        if (callchain == synth)
            callchain_nr = synth->nr;
    }

    key.nr = 0;
    /*
     * The time is placed at the front of the stack.
     *   1. The flame graph can be sorted by time.
     *   2. The print_callchain function is not affected.
    **/
    if (time) {
        key.ips[key.nr++] = PERF_CONTEXT_FLAME_GRAPH;
        key.ips[key.nr++] = time;
    }

    /* DWARF unavailable, fallback to FP-based callchain */
    if (callchain_nr == 0) {
        callchain = data->callchain;
        callchain_nr = callchain->nr;
        memcpy(&key.ips[key.nr], callchain->ips, callchain_nr * sizeof(callchain->ips[0]));
    }
    key.nr += callchain_nr;

    /*
     * convert callchain to unique string.
     * For user-mode stacks, symbols are freed after the process exits. Therefore,
     * the stack needs to be converted into a unique string first.
    **/
    print2string_callchain(fg->cc, (struct callchain *)&key, pid, &context_kernel_num, &context_user_num);
    // callchain empty
    if (context_kernel_num + context_user_num == callchain_nr) {
        return;
    }
    /*
     * There may be more than 1 PERF_CONTEXT_* tag. So, pre-print the error message.
    **/
    if (context_kernel_num > 1 || context_user_num > 1) {
        struct callchain_ctx debug;
        callchain_ctx_debug_init(&debug, fg->cc->kernel, fg->cc->user, stderr);
        print_time(stderr);
        fprintf(stderr, "BUG: callchain error%s%s\n",
                context_kernel_num > 1 ? " PERF_CONTEXT_KERNEL >1" : "",
                context_user_num > 1 ? " PERF_CONTEXT_USER >1" : "");
        print_callchain_common(&debug, data->callchain, pid);
    }

    if ((fg->cc->user && fg->cc->print2string_user) || comm) {
        /*
         * There is no user-mode stack in callchain, add PERF_CONTEXT_USER isolates
         * the user-mode and kernel-mode stacks.
        **/
        if (!context_user_num) {
            key.ips[key.nr++] = PERF_CONTEXT_USER;
            context_user_num++;
        }
        if (comm)
            snprintf(buff, sizeof(buff), "%s", comm);
        else
            snprintf(buff, sizeof(buff), "%d", pid);
        key.ips[key.nr++] = (__u64)(void *)unique_string(buff);
    }
    if (time && time_str) {
        if (!context_user_num) {
            key.ips[key.nr++] = PERF_CONTEXT_USER;
            context_user_num++;
        }
        key.ips[key.nr++] = (__u64)(void *)unique_string(time_str);
    }

    /*
     * Add to the storage pool with the stack as the key.
    **/
    keyvalue_pairs_add_key(fg->kv_pairs, (struct_key *)&key);
}

static void __flame_graph_print(void *opaque, struct_key *key, void *value, unsigned int n)
{
    struct flame_graph *fg = opaque;
    print_callchain(fg->cc, key, 0);
    fprintf(fg->cc->fout, "%u\n", n);
}

void flame_graph_output(struct flame_graph *fg)
{
    if (!fg)
        return ;

    keyvalue_pairs_foreach(fg->kv_pairs, __flame_graph_print, fg);
}

struct flame_graph *flame_graph_open(int flags, const char *path)
{
    char filename[PATH_MAX];
    FILE *fp;
    struct flame_graph *fg;
    struct stat buf;
    bool special = false;

    if (!path)
        return NULL;

    if (path[0] == '\0')
        return flame_graph_new(flags, stdout);

    if (stat(path, &buf) == 0 &&
        special_file(buf.st_mode)) {
        special = true;
        goto _open;
    }

    snprintf(filename, sizeof(filename), "%s.folded", path);
    if (access(filename, F_OK) == 0) {
        char filename_old[PATH_MAX];
        snprintf(filename_old, sizeof(filename_old), "%s.folded.old", path);
        rename(filename, filename_old);
    }

_open:
    fp = fopen(special ? path : filename, "w+");
    if (!fp)
        return NULL;

    fg = flame_graph_new(flags, fp);
    if (fg)
        fg->filename = strdup(path);
    return fg;
}

void flame_graph_close(struct flame_graph *fg)
{
    FILE *fp;

    if (!fg)
        return ;

    fp = fg->cc->fout;
    if (!fg->special && fp != stdout && !keyvalue_pairs_empty(fg->kv_pairs)) {
        printf("To generate the flame graph, running THIS shell command:\n");
        printf("\n  flamegraph.pl %s.folded > %s.svg\n\n", fg->filename, fg->filename);
    }

    if (fg->filename)
        free(fg->filename);
    flame_graph_free(fg);
    if (fp != stdout)
        fclose(fp);
}

void flame_graph_reset(struct flame_graph *fg)
{
    if (!fg)
        return ;
    rblist__exit(&fg->kv_pairs->kv_pairs);
}


struct heatmap {
    const char *time_units;    //"s", "ms", "us", "ns"
    const char *latency_units; //"s", "ms", "us", "ns"
    char *filename;
    FILE *fp;
};

struct heatmap *heatmap_open(const char *time_uints, const char *latency_units, const char *path)
{
    char filename[PATH_MAX];
    FILE *fp;
    struct heatmap *heatmap;

    if (!path)
        return NULL;

    snprintf(filename, sizeof(filename), "%s.lat", path);
    if (access(filename, F_OK) == 0) {
        char filename_old[PATH_MAX];
        snprintf(filename_old, sizeof(filename_old), "%s.lat.old", path);
        rename(filename, filename_old);
    }
    fp = fopen(filename, "w+");
    if (!fp)
        return NULL;

    heatmap = malloc(sizeof(*heatmap));
    if (heatmap) {
        heatmap->time_units = time_uints;
        heatmap->latency_units = latency_units;
        heatmap->filename = strdup(path);
        heatmap->fp = fp;
    }
    return heatmap;
}

void heatmap_close(struct heatmap *heatmap)
{
    if (!heatmap)
        return ;

    if (ftell(heatmap->fp)) {
        printf("To generate the heatmap, running THIS shell command:\n");
        printf("\n  trace2heatmap.pl --unitstime=%s --unitslabel=%s --grid %s.lat > %s.svg\n\n",
                heatmap->time_units, heatmap->latency_units, heatmap->filename, heatmap->filename);
    }

    fclose(heatmap->fp);
    free(heatmap->filename);
    free(heatmap);
}

void heatmap_write(struct heatmap *heatmap, unsigned long time, unsigned long latency)
{
    if (heatmap)
        fprintf(heatmap->fp, "%ld %ld\n", time, latency);
}


#ifdef HAVE_LIBPYTHON

/*
 * Cached Python string objects for callchain dict keys.
 * These are created once and reused to avoid repeated string creation.
 */
static struct {
    PyObject *key_addr;     /* "addr" */
    PyObject *key_symbol;   /* "symbol" */
    PyObject *key_offset;   /* "offset" */
    PyObject *key_kernel;   /* "kernel" */
    PyObject *key_dso;      /* "dso" */
    int initialized;
} callchain_key_cache;

/*
 * Initialize cached Python string keys for callchain dicts.
 * Returns 0 on success, -1 on failure.
 */
static int init_callchain_key_cache(void)
{
    if (callchain_key_cache.initialized)
        return 0;

#define INTERN_CALLCHAIN_KEY(field, str) do { \
        callchain_key_cache.field = PyUnicode_InternFromString(str); \
        if (!callchain_key_cache.field) return -1; \
    } while (0)

    INTERN_CALLCHAIN_KEY(key_addr, "addr");
    INTERN_CALLCHAIN_KEY(key_symbol, "symbol");
    INTERN_CALLCHAIN_KEY(key_offset, "offset");
    INTERN_CALLCHAIN_KEY(key_kernel, "kernel");
    INTERN_CALLCHAIN_KEY(key_dso, "dso");

#undef INTERN_CALLCHAIN_KEY

    callchain_key_cache.initialized = 1;
    return 0;
}

/*
 * Convert a callchain to a Python list of frame dicts.
 *
 * Each frame dict contains:
 *   - addr: int (instruction pointer address)
 *   - symbol: str (function name or "Unknown")
 *   - offset: int (offset from symbol start)
 *   - kernel: bool (True if kernel frame, False if user frame)
 *   - dso: str (DSO name or "[kernel.kallsyms]" or "Unknown")
 *
 * Frames are ordered from stack top to bottom (caller direction).
 *
 * @callchain: The callchain to convert
 * @pid: Process ID for user space symbol resolution
 * @flags: CALLCHAIN_KERNEL and/or CALLCHAIN_USER flags
 *
 * Returns a new reference to a Python list, or NULL on error.
 */
void *callchain_to_pylist(struct callchain *callchain, u32 pid, int flags)
{
    PyObject *list, *frame_dict, *val;
    u64 i;
    bool kernel = false, user = false, py = false;
    struct syms *syms = NULL;
    bool need_kernel = flags & CALLCHAIN_KERNEL;
    bool need_user = flags & CALLCHAIN_USER;

    if (!callchain || callchain->nr == 0)
        return PyList_New(0);

    if (init_callchain_key_cache() < 0)
        return NULL;

    list = PyList_New(0);
    if (!list)
        return NULL;

    for (i = 0; i < callchain->nr; i++) {
        u64 ip = callchain->ips[i];

        /* Handle context markers */
        if (ip == PERF_CONTEXT_KERNEL) {
            kernel = true;
            user = py = false;
            continue;
        } else if (ip == PERF_CONTEXT_USER) {
            user = true;
            kernel = py = false;
            if (need_user && ctx.syms_cache)
                syms = syms_cache__get_syms(ctx.syms_cache, pid);
            continue;
        } else if (ip == PERF_CONTEXT_PYSTACK) {
            py = true;
            kernel = user = false;
            continue;
        }

        /* Skip frames based on flags */
        if (kernel && !need_kernel)
            continue;
        if ((user || py) && !need_user)
            continue;

        frame_dict = PyDict_New();
        if (!frame_dict) {
            Py_DECREF(list);
            return NULL;
        }

#define SET_FRAME_ITEM(dict, key, value) do { \
            val = (value); \
            if (!val || PyDict_SetItem(dict, callchain_key_cache.key, val) < 0) { \
                Py_XDECREF(val); \
                Py_DECREF(dict); \
                Py_DECREF(list); \
                return NULL; \
            } \
            Py_DECREF(val); \
        } while (0)

        if (kernel) {
            const struct ksym *ksym = ctx.ksyms ?
                                      ksyms__map_addr(ctx.ksyms, ip) : NULL;

            SET_FRAME_ITEM(frame_dict, key_addr, PyLong_FromUnsignedLongLong(ip));
            SET_FRAME_ITEM(frame_dict, key_symbol,
                           PyUnicode_FromString(ksym ? ksym->name : "Unknown"));
            SET_FRAME_ITEM(frame_dict, key_offset,
                           PyLong_FromUnsignedLongLong(ksym ? ip - ksym->addr : 0));
            SET_FRAME_ITEM(frame_dict, key_kernel, Py_True); Py_INCREF(Py_True);
            SET_FRAME_ITEM(frame_dict, key_dso,
                           PyUnicode_FromString("[kernel.kallsyms]"));

        } else if (user) {
            struct dso *dso = NULL;
            const struct sym *sym = NULL;
            const char *symbol = "Unknown";
            u64 offset = 0;
            const char *dso_name = "Unknown";

            if (syms) {
                dso = syms__find_dso(syms, ip, &offset);
                if (dso) {
                    sym = dso__find_sym(dso, offset);
                    if (sym) {
                        symbol = sym__name(sym);
                        offset = offset - sym->start;
                        dso_name = dso__name(dso) ? : "Unknown";
                    }
                }
            }

            SET_FRAME_ITEM(frame_dict, key_addr, PyLong_FromUnsignedLongLong(ip));
            SET_FRAME_ITEM(frame_dict, key_symbol, PyUnicode_FromString(symbol));
            SET_FRAME_ITEM(frame_dict, key_offset, PyLong_FromUnsignedLongLong(offset));
            SET_FRAME_ITEM(frame_dict, key_kernel, Py_False); Py_INCREF(Py_False);
            SET_FRAME_ITEM(frame_dict, key_dso, PyUnicode_FromString(dso_name));

        } else if (py) {
            /* Python stack frame: ip is actually a string pointer: "func (file:lineno)" */
            char *str = (char *)ip;
            char *paren = strchr(str, '(');
            int func_len = paren ? paren - str - 1 : strlen(str);
            const char *dso_str = paren ? paren + 1 : "";

            SET_FRAME_ITEM(frame_dict, key_addr, PyLong_FromLong(0));
            SET_FRAME_ITEM(frame_dict, key_symbol, PyUnicode_FromStringAndSize(str, func_len));
            SET_FRAME_ITEM(frame_dict, key_offset, PyLong_FromLong(0));
            SET_FRAME_ITEM(frame_dict, key_kernel, Py_False); Py_INCREF(Py_False);
            SET_FRAME_ITEM(frame_dict, key_dso, PyUnicode_FromStringAndSize(dso_str, strlen(dso_str) - 1 /* ) */));
        }

#undef SET_FRAME_ITEM

        if (PyList_Append(list, frame_dict) < 0) {
            Py_DECREF(frame_dict);
            Py_DECREF(list);
            return NULL;
        }
        Py_DECREF(frame_dict);
    }

    return list;
}

/*
 * Free cached Python string keys for callchain dicts.
 */
static void free_callchain_key_cache(void)
{
    if (!callchain_key_cache.initialized || !Py_IsInitialized())
        return;

    Py_XDECREF(callchain_key_cache.key_addr);
    Py_XDECREF(callchain_key_cache.key_symbol);
    Py_XDECREF(callchain_key_cache.key_offset);
    Py_XDECREF(callchain_key_cache.key_kernel);
    Py_XDECREF(callchain_key_cache.key_dso);

    memset(&callchain_key_cache, 0, sizeof(callchain_key_cache));
}

/*
 * Initialize symbol resolution for callchain_to_pylist.
 * Must be called before using callchain_to_pylist.
 *
 * @flags: CALLCHAIN_KERNEL and/or CALLCHAIN_USER flags
 * Returns 0 on success, -1 on failure.
 */
int callchain_pylist_init(int flags)
{
    bool user = flags & CALLCHAIN_USER;
    bool dwarf = flags & CALLCHAIN_DWARF;
    return global_syms_ref(flags & CALLCHAIN_KERNEL, user || dwarf, dwarf);
}

/*
 * Cleanup symbol resolution resources and cached Python objects.
 * @flags: Same flags passed to callchain_pylist_init.
 */
void callchain_pylist_exit(int flags)
{
    bool user = flags & CALLCHAIN_USER;
    bool dwarf = flags & CALLCHAIN_DWARF;
    free_callchain_key_cache();
    global_syms_unref(flags & CALLCHAIN_KERNEL, user || dwarf, dwarf);
}

#endif /* HAVE_LIBPYTHON */
