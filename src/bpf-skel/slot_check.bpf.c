/*
 * Compile-time check: is it safe for a generated expression filter to clobber
 * r2-r5?
 *
 * src/expr.c maps expression stack slot N onto register r(2+N), so a filter
 * freely writes r2-r9 (see docs/bpf_event_filter.md 5.2.1). That is only sound
 * because r2-r5 are caller-saved in the BPF calling convention: clang must not
 * keep a value in one of them across a call. The placeholder it compiles the
 * call site against has a body of just `r0 = 1; exit', so nothing stops a
 * sufficiently clever compiler from noticing the callee preserves everything --
 * except that the ABI forbids relying on it.
 *
 * This file exists to prove that clang still honours that, rather than trusting
 * it. It deliberately creates values that must survive a call to the real
 * placeholder, at every register-pressure level the translator can reach. The
 * build then disassembles the result and checks the one invariant that matters
 * (see scripts/bpf_slot_check.py):
 *
 *     for every call, r2-r5 must be WRITTEN before they are read
 *
 * If a future clang ever breaks it, the build switches the translator's first
 * slot to r6 instead of guessing. Nothing here is loaded or run; only the
 * generated instructions are inspected, so the programs need not be attachable
 * and the event need not be real.
 *
 * Two things must be defeated for the test to have any teeth, both learned the
 * hard way:
 *
 *  - constant folding: a value the compiler can rematerialise after the call
 *    never needs to survive it. bpf_get_prandom_u32() is opaque, so its result
 *    cannot be recomputed.
 *
 *  - sinking: a plain load from a global can simply be moved below the call,
 *    which also dissolves the pressure. Helper results cannot move across the
 *    call, and the volatile reads in slot_check_fields() cannot either.
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include "expr_filter.bpf.h"

/* Mirrors a real event: the filter takes a pointer to one of these in r1 and
 * may both read and write it. Fields are volatile so loads cannot be folded or
 * sunk past the call. */
struct slot_check_event {
	volatile __u64 f0, f1, f2, f3, f4, f5, f6, f7;
};

/* The event lives in writable memory, as expr_to_bpf() requires. */
struct slot_check_event slot_check_ev;

__u64 slot_check_sink;

/* The placeholder under test -- the same macro the real BPF programs use, so
 * this check tracks whatever DEFINE_EXPR_FILTER() actually expands to. */
DEFINE_EXPR_FILTER(struct slot_check_event)

/*
 * N values live across the call, for N = 1..8: with slots based at r2 the
 * translator can reach a depth of 8, so this spans every allocation shape from
 * "r6-r9 are plenty" to "r6-r9 are full and the rest spills to the stack".
 *
 * Each program is one call site. The values come from a helper so they are
 * opaque, and they are summed only after the call so they must survive it.
 */
#define SLOT_CHECK_PRESSURE(n, ...)                             \
	SEC("tp_btf/sched_switch")                              \
	int slot_check_##n(void *ctx)                           \
	{                                                       \
		__VA_ARGS__                                     \
		return 0;                                       \
	}

SLOT_CHECK_PRESSURE(1,
	__u64 a0 = bpf_get_prandom_u32();
	if (EXPR_FILTER_FUNC(&slot_check_ev))
		slot_check_sink = a0;
)

SLOT_CHECK_PRESSURE(2,
	__u64 a0 = bpf_get_prandom_u32(), a1 = bpf_get_prandom_u32();
	if (EXPR_FILTER_FUNC(&slot_check_ev))
		slot_check_sink = a0 + a1;
)

SLOT_CHECK_PRESSURE(3,
	__u64 a0 = bpf_get_prandom_u32(), a1 = bpf_get_prandom_u32();
	__u64 a2 = bpf_get_prandom_u32();
	if (EXPR_FILTER_FUNC(&slot_check_ev))
		slot_check_sink = a0 + a1 + a2;
)

SLOT_CHECK_PRESSURE(4,
	__u64 a0 = bpf_get_prandom_u32(), a1 = bpf_get_prandom_u32();
	__u64 a2 = bpf_get_prandom_u32(), a3 = bpf_get_prandom_u32();
	if (EXPR_FILTER_FUNC(&slot_check_ev))
		slot_check_sink = a0 + a1 + a2 + a3;
)

SLOT_CHECK_PRESSURE(5,
	__u64 a0 = bpf_get_prandom_u32(), a1 = bpf_get_prandom_u32();
	__u64 a2 = bpf_get_prandom_u32(), a3 = bpf_get_prandom_u32();
	__u64 a4 = bpf_get_prandom_u32();
	if (EXPR_FILTER_FUNC(&slot_check_ev))
		slot_check_sink = a0 + a1 + a2 + a3 + a4;
)

SLOT_CHECK_PRESSURE(6,
	__u64 a0 = bpf_get_prandom_u32(), a1 = bpf_get_prandom_u32();
	__u64 a2 = bpf_get_prandom_u32(), a3 = bpf_get_prandom_u32();
	__u64 a4 = bpf_get_prandom_u32(), a5 = bpf_get_prandom_u32();
	if (EXPR_FILTER_FUNC(&slot_check_ev))
		slot_check_sink = a0 + a1 + a2 + a3 + a4 + a5;
)

SLOT_CHECK_PRESSURE(7,
	__u64 a0 = bpf_get_prandom_u32(), a1 = bpf_get_prandom_u32();
	__u64 a2 = bpf_get_prandom_u32(), a3 = bpf_get_prandom_u32();
	__u64 a4 = bpf_get_prandom_u32(), a5 = bpf_get_prandom_u32();
	__u64 a6 = bpf_get_prandom_u32();
	if (EXPR_FILTER_FUNC(&slot_check_ev))
		slot_check_sink = a0 + a1 + a2 + a3 + a4 + a5 + a6;
)

SLOT_CHECK_PRESSURE(8,
	__u64 a0 = bpf_get_prandom_u32(), a1 = bpf_get_prandom_u32();
	__u64 a2 = bpf_get_prandom_u32(), a3 = bpf_get_prandom_u32();
	__u64 a4 = bpf_get_prandom_u32(), a5 = bpf_get_prandom_u32();
	__u64 a6 = bpf_get_prandom_u32(), a7 = bpf_get_prandom_u32();
	if (EXPR_FILTER_FUNC(&slot_check_ev))
		slot_check_sink = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
)

/*
 * The shape real code actually has: event fields are loaded, the filter is
 * called, and the fields are used afterwards. Less register pressure than the
 * cases above, but it is the pattern the real BPF programs have, so it is
 * worth checking directly rather than only in the abstract.
 */
SEC("tp_btf/sched_switch")
int slot_check_fields(void *ctx)
{
	struct slot_check_event *e = &slot_check_ev;
	__u64 a0 = e->f0, a1 = e->f1, a2 = e->f2, a3 = e->f3;
	__u64 a4 = e->f4, a5 = e->f5, a6 = e->f6, a7 = e->f7;

	if (EXPR_FILTER_FUNC(e))
		slot_check_sink = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
	return 0;
}

char LICENSE[] SEC("license") = "GPL";
