/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DWARF-based user-space stack unwinding using libunwind.
 *
 * The kernel captures user registers (PERF_SAMPLE_REGS_USER) and a raw
 * stack snapshot (PERF_SAMPLE_STACK_USER). This module walks the stack
 * using .eh_frame unwind tables from DSO ELF files via libunwind's
 * remote unwinding interface.
 *
 * The eh_frame_hdr and eh_frame data are cached in struct object
 * (trace_helpers.c) and lazy-loaded on first access via
 * dso__get_unwind_data(). Loading enters the correct mount
 * namespace so that ELF files in other namespaces are accessible.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libunwind.h>
#include <monitor.h>
#include <trace_helpers.h>
#include <stack_helpers.h>
#include "perf_regs_mask.h"
#include "dwarf_unwind.h"

extern int UNW_OBJ(dwarf_search_unwind_table) (unw_addr_space_t as,
                                                unw_word_t ip,
                                                unw_dyn_info_t *di,
                                                unw_proc_info_t *pi,
                                                int need_unwind_info, void *arg);
#define dwarf_search_unwind_table UNW_OBJ(dwarf_search_unwind_table)

/*
 * Context passed to libunwind accessors during stack unwinding.
 *
 * For memory reads: the captured stack covers [sp, sp + stack_sz).
 * For eh_frame data reads: we keep track of the current DSO's cached
 * unwind data so access_mem can read from it.
 */
struct unwind_info {
    struct syms *syms;          /* for DSO lookup */
    u64 *regs;                  /* captured user registers */
    int nr_regs;                /* number of registers in array */
    void *stack;                /* captured stack data */
    u64 stack_sz;               /* actual dynamic stack size */
    u64 sp;                     /* stack pointer from registers */

    /* Current DSO's unwind data (set by find_proc_info) */
    struct dso_unwind_data cur_dso;
};

static unw_addr_space_t addr_space;

/*
 * Map libunwind register ID to index in the perf sample_regs_user array.
 *
 * The perf regs array only contains registers whose bits are set in
 * PERF_REGS_MASK. On x86_64, DS/ES/FS/GS are excluded (REG_NOSUPPORT),
 * so register indices above DS need adjustment.
 */
#if defined(__x86_64__)
static int libunwind_to_perf_idx(int unw_reg)
{
    int perf_reg;

    switch (unw_reg) {
    case UNW_X86_64_RAX: perf_reg = PERF_REG_X86_AX; break;
    case UNW_X86_64_RDX: perf_reg = PERF_REG_X86_DX; break;
    case UNW_X86_64_RCX: perf_reg = PERF_REG_X86_CX; break;
    case UNW_X86_64_RBX: perf_reg = PERF_REG_X86_BX; break;
    case UNW_X86_64_RSI: perf_reg = PERF_REG_X86_SI; break;
    case UNW_X86_64_RDI: perf_reg = PERF_REG_X86_DI; break;
    case UNW_X86_64_RBP: perf_reg = PERF_REG_X86_BP; break;
    case UNW_X86_64_RSP: perf_reg = PERF_REG_X86_SP; break;
    case UNW_X86_64_R8:  perf_reg = PERF_REG_X86_R8; break;
    case UNW_X86_64_R9:  perf_reg = PERF_REG_X86_R9; break;
    case UNW_X86_64_R10: perf_reg = PERF_REG_X86_R10; break;
    case UNW_X86_64_R11: perf_reg = PERF_REG_X86_R11; break;
    case UNW_X86_64_R12: perf_reg = PERF_REG_X86_R12; break;
    case UNW_X86_64_R13: perf_reg = PERF_REG_X86_R13; break;
    case UNW_X86_64_R14: perf_reg = PERF_REG_X86_R14; break;
    case UNW_X86_64_R15: perf_reg = PERF_REG_X86_R15; break;
    case UNW_X86_64_RIP: perf_reg = PERF_REG_X86_IP; break;
    default:
        return -1;
    }

    if (!((1ULL << perf_reg) & PERF_REGS_MASK))
        return -1;

    return perf_reg > PERF_REG_X86_DS ? perf_reg - REG_NOSUPPORT_N : perf_reg;
}

static u64 get_sp(struct unwind_info *ui)
{
    int idx = libunwind_to_perf_idx(UNW_X86_64_RSP);
    if (idx >= 0 && idx < ui->nr_regs)
        return ui->regs[idx];
    return 0;
}

#elif defined(__aarch64__)
static int libunwind_to_perf_idx(int unw_reg)
{
    int perf_reg;

    if (unw_reg >= UNW_AARCH64_X0 && unw_reg <= UNW_AARCH64_X30)
        perf_reg = PERF_REG_ARM64_X0 + (unw_reg - UNW_AARCH64_X0);
    else if (unw_reg == UNW_AARCH64_SP)
        perf_reg = PERF_REG_ARM64_SP;
    else if (unw_reg == UNW_AARCH64_PC)
        perf_reg = PERF_REG_ARM64_PC;
    else
        return -1;

    if (!((1ULL << perf_reg) & PERF_REGS_MASK))
        return -1;

    return perf_reg - PERF_REG_ARM64_X0;
}

static u64 get_sp(struct unwind_info *ui)
{
    int idx = libunwind_to_perf_idx(UNW_AARCH64_SP);
    if (idx >= 0 && idx < ui->nr_regs)
        return ui->regs[idx];
    return 0;
}

#else
static int libunwind_to_perf_idx(int unw_reg) { return -1; }
static u64 get_sp(struct unwind_info *ui) { return 0; }
#endif

/*
 * libunwind accessor: read memory.
 *
 * Handles reads from three regions:
 * 1. Captured stack: [sp, sp + stack_sz)
 * 2. Cached ELF unwind data: eh_frame_hdr and eh_frame.
 *    dwarf_search_unwind_table accesses them via the runtime addresses
 *    we provide in unw_dyn_info_t.
 *
 * For addresses outside these regions, we return zero instead of an error.
 *
 * Why: libunwind may read addresses we don't have cached. This happens
 * when eh_frame CIE entries use DW_EH_PE_indirect encoding for the
 * personality routine pointer. The indirect reference points to data in
 * segments we don't cache (.data, .data.rel.ro, .got), e.g.:
 *
 *   DW.ref.__gcc_personality_v0  (in .data or .data.rel.ro)
 *   GOT entries for personality   (in .got, for PIC code)
 *
 * These personality routine pointers are only used for C++/language-level
 * exception handling, NOT for stack unwinding. The unw_step() process
 * only needs CFA rules and register restore rules from the FDE/CIE to
 * walk the stack. Returning zero makes libunwind treat the personality
 * routine as NULL, which is harmless for our stack-walking purpose.
 *
 * Returning -UNW_EINVAL would cause dwarf_search_unwind_table() to fail
 * entirely, aborting the unwind for that frame — which is worse than
 * returning a benign zero value.
 */
static int access_mem(unw_addr_space_t as, unw_word_t addr,
                      unw_word_t *valp, int write, void *arg)
{
    struct unwind_info *ui = arg;
    u64 start, end;

    if (write)
        return -UNW_EINVAL;

    /* Try captured stack */
    if (ui->stack) {
        start = ui->sp;
        end = start + ui->stack_sz;
        if (addr >= start && addr + sizeof(unw_word_t) <= end) {
            *valp = *(unw_word_t *)((char *)ui->stack + (addr - start));
            return 0;
        }
    }

    /* Try cached eh_frame_hdr data. */
    if (ui->cur_dso.eh_frame_hdr) {
        u64 rt_start = ui->cur_dso.eh_frame_hdr_addr;
        u64 rt_end = rt_start + ui->cur_dso.eh_frame_hdr_sz;
        if (addr >= rt_start && addr + sizeof(unw_word_t) <= rt_end) {
            *valp = *(unw_word_t *)((char *)ui->cur_dso.eh_frame_hdr + (addr - rt_start));
            return 0;
        }
    }

    /* Try cached eh_frame data */
    if (ui->cur_dso.eh_frame) {
        u64 rt_start = ui->cur_dso.eh_frame_addr;
        u64 rt_end = rt_start + ui->cur_dso.eh_frame_sz;
        if (addr >= rt_start && addr + sizeof(unw_word_t) <= rt_end) {
            *valp = *(unw_word_t *)((char *)ui->cur_dso.eh_frame + (addr - rt_start));
            return 0;
        }
    }

    /*
     * Address not in any cached region. Return zero rather than an error
     * to avoid aborting the unwind. See function comment for details.
     */
    *valp = 0;
    return 0;
}

/*
 * libunwind accessor: read a register value.
 */
static int access_reg(unw_addr_space_t as, unw_regnum_t regnum,
                      unw_word_t *valp, int write, void *arg)
{
    struct unwind_info *ui = arg;
    int idx;

    if (write)
        return -UNW_EINVAL;

    idx = libunwind_to_perf_idx(regnum);
    if (idx < 0 || idx >= ui->nr_regs)
        return -UNW_EBADREG;

    *valp = ui->regs[idx];
    return 0;
}

static int access_fpreg(unw_addr_space_t as, unw_regnum_t regnum,
                        unw_fpreg_t *valp, int write, void *arg)
{
    return -UNW_EINVAL;
}

static void put_unwind_info(unw_addr_space_t as, unw_proc_info_t *pi, void *arg)
{
}

static int get_dyn_info_list_addr(unw_addr_space_t as, unw_word_t *dilap, void *arg)
{
    return -UNW_ENOINFO;
}

static int resume(unw_addr_space_t as, unw_cursor_t *cp, void *arg)
{
    return -UNW_EINVAL;
}

static int get_proc_name(unw_addr_space_t as, unw_word_t addr,
                         char *bufp, size_t buf_len, unw_word_t *offp, void *arg)
{
    return -UNW_ENOINFO;
}

/*
 * libunwind accessor: find unwind info for an IP address.
 *
 * Uses syms_cache to find the DSO containing ip, then lazy-loads
 * its eh_frame_hdr and eh_frame via dso__get_unwind_data().
 * Parses the eh_frame_hdr binary search table and calls
 * dwarf_search_unwind_table to locate the FDE for the given ip.
 */
static int find_proc_info(unw_addr_space_t as, unw_word_t ip,
                          unw_proc_info_t *pip, int need_unwind_info, void *arg)
{
    struct unwind_info *ui = arg;
    struct syms *syms = ui->syms;
    struct dso *dso;
    uint64_t offset;
    struct dso_unwind_data uwdata;
    unw_dyn_info_t di;
    unsigned char *hdr;
    unsigned char fde_count_enc;
    u64 fde_count;
    int32_t *table_start;
    int ret;

    dso = syms__find_dso(syms, ip, &offset);
    if (!dso)
        return -UNW_ENOINFO;

    if (!dso__get_unwind_data(dso, &uwdata)) {
        memset(&ui->cur_dso, 0, sizeof(ui->cur_dso));
        return -UNW_ENOINFO;
    }

    /* Store for access_mem to use */
    ui->cur_dso = uwdata;

    /*
     * Parse the eh_frame_hdr binary search table.
     * Format (version 1):
     *   u8  version (must be 1)
     *   u8  eh_frame_ptr_enc
     *   u8  fde_count_enc
     *   u8  table_enc
     *   encoded eh_frame_ptr  (4 bytes for sdata4 encodings)
     *   encoded fde_count     (4 bytes for sdata4/udata4 encodings)
     *   binary_search_table[fde_count]  (each entry: 2 * 4 bytes)
     */
    hdr = uwdata.eh_frame_hdr;
    if (!hdr || uwdata.eh_frame_hdr_sz < 8 || hdr[0] != 1)
        return -UNW_ENOINFO;

    fde_count_enc = hdr[2];
    /* Skip header (4 bytes) + eh_frame_ptr (4 bytes for common encodings) */
    table_start = (int32_t *)(hdr + 4);
    table_start++; /* skip eh_frame_ptr */

    /* Read fde_count */
    if ((fde_count_enc & 0x0f) == 0x03) { /* DW_EH_PE_udata4 */
        fde_count = *(uint32_t *)table_start;
        table_start++;
    } else if ((fde_count_enc & 0x0f) == 0x0b) { /* DW_EH_PE_sdata4 */
        fde_count = (uint64_t)(int64_t)*(int32_t *)table_start;
        table_start++;
    } else {
        return -UNW_ENOINFO;
    }

    if (fde_count == 0)
        return -UNW_ENOINFO;

    /*
     * Set up unw_dyn_info_t for UNW_INFO_FORMAT_REMOTE_TABLE.
     *
     * segbase: runtime address of eh_frame_hdr
     * table_data: runtime address of the binary search table
     * table_len: table size in bytes / sizeof(unw_word_t)
     *            Each entry is {u32 start_ip_offset, u32 fde_offset} = 8 bytes.
     *
     * dwarf_search_unwind_table will use access_mem to read from
     * these addresses. Our access_mem maps them back to the cached
     * data buffers.
     */
    memset(&di, 0, sizeof(di));
    di.start_ip = 0;
    di.end_ip = ~(unw_word_t)0;
    di.format = UNW_INFO_FORMAT_REMOTE_TABLE;
    di.u.rti.segbase = uwdata.eh_frame_hdr_addr;
    di.u.rti.table_data = di.u.rti.segbase +
        ((unsigned char *)table_start - (unsigned char *)uwdata.eh_frame_hdr);
    di.u.rti.table_len = fde_count * sizeof(u32) * 2 / sizeof(unw_word_t);

    ret = dwarf_search_unwind_table(as, ip, &di, pip, need_unwind_info, arg);
    return ret;
}

static unw_accessors_t accessors = {
    .find_proc_info     = find_proc_info,
    .put_unwind_info    = put_unwind_info,
    .get_dyn_info_list_addr = get_dyn_info_list_addr,
    .access_mem         = access_mem,
    .access_reg         = access_reg,
    .access_fpreg       = access_fpreg,
    .resume             = resume,
    .get_proc_name      = get_proc_name,
};

int dwarf_unwind_init(void)
{
    addr_space = unw_create_addr_space(&accessors, 0);
    if (!addr_space) {
        fprintf(stderr, "dwarf_unwind: failed to create libunwind addr space\n");
        return -1;
    }
    unw_set_caching_policy(addr_space, UNW_CACHE_NONE);
    return 0;
}

void dwarf_unwind_exit(void)
{
    if (addr_space) {
        unw_destroy_addr_space(addr_space);
        addr_space = NULL;
    }
}

int dwarf_unwind_user(struct syms *syms, u64 *regs, void *stack, u64 stack_sz,
                      u64 *ips, int max_ips)
{
    struct unwind_info ui;
    unw_cursor_t cursor;
    unw_word_t ip;
    int ret, nr = 0;

    if (!addr_space)
        return 0;

    memset(&ui, 0, sizeof(ui));
    ui.syms = syms;
    ui.regs = regs;
    ui.nr_regs = PERF_REGS_COUNT;
    ui.stack = stack;
    ui.stack_sz = stack_sz;
    ui.sp = get_sp(&ui);

    if (!ui.sp)
        return 0;

    ret = unw_init_remote(&cursor, addr_space, &ui);
    if (ret)
        return 0;

    while (nr < max_ips) {
        if (unw_get_reg(&cursor, UNW_REG_IP, &ip) || !ip)
            break;

        ips[nr++] = ip;

        ret = unw_step(&cursor);
        if (ret <= 0)
            break;
    }
    return nr;
}
