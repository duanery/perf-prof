/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __DWARF_UNWIND_H
#define __DWARF_UNWIND_H

#include <stack_helpers.h>

#ifdef HAVE_LIBUNWIND

int dwarf_unwind_init(void);
void dwarf_unwind_exit(void);
/*
 * Perform DWARF-based user-space stack unwinding.
 *
 * Uses libunwind remote interface with cached eh_frame/eh_frame_hdr
 * data from each DSO. Walks the user stack captured by the kernel.
 *
 * @syms:     Symbol table (contains DSO list for IP-to-DSO lookup)
 * @regs:     Captured user registers (PERF_SAMPLE_REGS_USER)
 * @stack:    Captured user stack data (PERF_SAMPLE_STACK_USER)
 * @stack_sz: Actual bytes written by kernel (dyn_size)
 * @ips:      Output array to store unwound instruction pointers
 * @max_ips:  Maximum entries in @ips
 *
 * Returns: Number of IPs written to @ips, or 0 on failure
 */
int dwarf_unwind_user(struct syms *syms, u64 *regs, void *stack, u64 stack_sz,
                      u64 *ips, int max_ips);

#else /* !HAVE_LIBUNWIND */

static inline int dwarf_unwind_init(void) { return 0; }
static inline void dwarf_unwind_exit(void) {}
static inline int dwarf_unwind_user(struct syms *syms, u64 *regs, void *stack, u64 stack_sz,
                      u64 *ips, int max_ips) { return 0; }

#endif /* HAVE_LIBUNWIND */

#endif /* __DWARF_UNWIND_H */
