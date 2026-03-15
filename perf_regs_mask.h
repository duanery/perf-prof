/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PERF_REGS_MASK_H
#define __PERF_REGS_MASK_H

#include <asm/perf_regs.h>
#include <linux/bitops.h>

#if defined(__i386__) || defined(__x86_64__)
#define REG_NOSUPPORT_N 4
#define REG_NOSUPPORT ((1ULL << PERF_REG_X86_DS) | \
		       (1ULL << PERF_REG_X86_ES) | \
		       (1ULL << PERF_REG_X86_FS) | \
		       (1ULL << PERF_REG_X86_GS))
#if defined(__i386__)
#define PERF_REGS_MASK (((1ULL << PERF_REG_X86_32_MAX) - 1) & ~REG_NOSUPPORT)
#else
#define PERF_REGS_MASK (((1ULL << PERF_REG_X86_64_MAX) - 1) & ~REG_NOSUPPORT)
#endif
#elif defined(__aarch64__)
#define PERF_REGS_MASK ((1ULL << PERF_REG_ARM64_MAX) - 1)
#elif defined(__arm__)
#define PERF_REGS_MASK ((1ULL << PERF_REG_ARM_MAX) - 1)
#else
#define PERF_REGS_MASK 0
#endif

#define PERF_REGS_COUNT hweight64(PERF_REGS_MASK)

#endif /* __PERF_REGS_MASK_H */
