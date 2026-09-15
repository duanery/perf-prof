#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Check whether a generated expression filter may clobber r2-r5.

src/expr.c bases the expression stack at r2 (BPF_SLOT_FIRST), so a compiled
filter writes r2-r9.  That is sound only while clang keeps to the BPF calling
convention, in which r2-r5 are caller-saved and therefore must not hold a
value across a call.  The call site is compiled against a placeholder whose
body is just `r0 = 1; exit', so nothing but the ABI stops a compiler from
noticing the callee preserves every register.  See docs/bpf_event_filter.md
5.2.1 and 5.2.2 for the full argument.

This script checks that rather than trusting it.  Given the object built from
src/bpf-skel/slot_check.bpf.c, it verifies one invariant:

    for every call, r2-r5 must be written before they are read

Usage: bpf_slot_check.py <slot_check.bpf.o>

Prints "BPF_SLOT_FIRST_R6=y" on stdout if the invariant is violated -- the
build then rebases the expression stack at r6 -- and nothing if all is well.
Anything that prevents the check from running (no llvm-objdump, unreadable
object, no call sites found) is reported on stderr and treated the same way:
r6 is correct either way, just one slot poorer.  Exit status is 0 unless the
arguments were wrong, so the build is never broken by this check itself.

Pass --verbose to print the per-call classification, which is what you want
when investigating a reported violation.
"""

import argparse
import collections
import os
import re
import shutil
import subprocess
import sys

# Instruction line from llvm-objdump: "   12:\tr2 = *(u64 *)(r1 + 0x0)"
INSN_RE = re.compile(r'^\s*(\d+):\s+(.*\S)\s*$')
# Function label: "0000000000000000 <slot_check_8>:"
FUNC_RE = re.compile(r'^[0-9a-f]+\s+<(.+)>:')
# A register mention.  The 32-bit views (wN) alias the same slot as rN.
REG_RE = re.compile(r'\b[rw](\d+)\b')
# "rN = <src>" -- a pure definition, so rN itself is not read.  The negative
# lookahead keeps "r1 == r2" (a comparison) out.
PURE_DEF_RE = re.compile(r'^[rw](\d+)\s*=(?!=)')
# "rN += <src>", "rN <<= ...", "rN s>>= ..." -- destination is also a source.
RMW_RE = re.compile(r'^[rw](\d+)\s*(?:\+|-|\*|/|%|\||&|\^|<<|>>|s>>)=')
# A store, "*(u64 *)(r10 - 0x8) = r2": writes memory, defines no register.
STORE_RE = re.compile(r'^\*\(')

FRAME_POINTER = 10
SLOT_MIN, SLOT_MAX = 2, 9
CALLER_SAVED = range(2, 6)


def disassemble(objdump, obj):
    """Return {function name: [(index, text), ...]} for the object's .text."""
    out = subprocess.run([objdump, '-d', '--no-show-raw-insn', obj],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         universal_newlines=True)
    if out.returncode != 0:
        raise RuntimeError('%s failed: %s' % (objdump, out.stderr.strip()))

    funcs = collections.OrderedDict()
    current = None
    for line in out.stdout.splitlines():
        match = FUNC_RE.match(line)
        if match:
            current = match.group(1)
            funcs[current] = []
            continue
        match = INSN_RE.match(line)
        if match and current is not None:
            funcs[current].append((int(match.group(1)), match.group(2)))
    if not funcs:
        raise RuntimeError('no disassembly produced for %s' % obj)
    return funcs


def reads_and_write(text):
    """Split one instruction into (registers read, register defined).

    Three details decide read vs. write here, and getting any of them wrong
    invents violations that do not exist:

      - "rN = <src>" is a pure definition, so rN is not read; the
        read-modify-write forms do read rN first.
      - a store writes memory, so every register it mentions is read and
        nothing is defined.
      - r10 is the read-only frame pointer: always live, never a slot.
    """
    if STORE_RE.match(text):
        # Store: the whole instruction is read side.
        return _regs(text), None

    match = PURE_DEF_RE.match(text)
    if match:
        dst = int(match.group(1))
        rhs = text.split('=', 1)[1]
        return _regs(rhs), dst

    match = RMW_RE.match(text)
    if match:
        dst = int(match.group(1))
        # Destination is read too, so scan the whole instruction.
        return _regs(text), dst

    # Jump, exit, call, or anything else that defines no register.
    return _regs(text), None


def _regs(text):
    return [int(n) for n in REG_RE.findall(text)]


def classify_call(insns, call_at):
    """First access to each of r2-r9 after insns[call_at].

    Returns {register: 'R' | 'W'}.  'R' means the caller reads the register
    before redefining it, i.e. it expects the value to survive the call.  'W'
    means the register is redefined first, so a callee clobbering it is
    harmless.  Registers absent from the result are never touched again.

    Scanning stops at the next call: a register not yet classified by then is
    clobbered again anyway, so only the first call-to-access span matters.
    """
    state = {}
    for _, text in insns[call_at + 1:]:
        if text.startswith('call'):
            break
        reads, dst = reads_and_write(text)
        for reg in reads:
            if reg == FRAME_POINTER or not SLOT_MIN <= reg <= SLOT_MAX:
                continue
            state.setdefault(reg, 'R')
        if dst is not None and SLOT_MIN <= dst <= SLOT_MAX:
            state.setdefault(dst, 'W')
    return state


def check(funcs, verbose=False):
    """Return (number of call sites, list of violations)."""
    calls = 0
    violations = []
    for name, insns in funcs.items():
        for i, (idx, text) in enumerate(insns):
            if not text.startswith('call'):
                continue
            calls += 1
            state = classify_call(insns, i)
            if verbose:
                cols = ' '.join('r%d:%s' % (r, state.get(r, '-'))
                                for r in range(SLOT_MIN, SLOT_MAX + 1))
                print('  %-20s call@%-4d %s' % (name, idx, cols))
            for reg in CALLER_SAVED:
                if state.get(reg) == 'R':
                    violations.append((name, idx, reg))
    return calls, violations


def fallback(reason):
    """Report why r2-r5 could not be cleared, and select r6."""
    sys.stderr.write('bpf_slot_check: %s, basing expression stack at r6\n'
                     % reason)
    print('BPF_SLOT_FIRST_R6=y')
    sys.exit(0)


def main():
    parser = argparse.ArgumentParser(
        description='Check that a compiled filter may clobber r2-r5.')
    parser.add_argument('object', help='object built from slot_check.bpf.c')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='print the per-call register classification')
    args = parser.parse_args()

    objdump = os.environ.get('LLVM_OBJDUMP', 'llvm-objdump')
    if not shutil.which(objdump):
        fallback('%s not found' % objdump)
    if not os.access(args.object, os.R_OK):
        fallback('cannot read %s' % args.object)

    try:
        funcs = disassemble(objdump, args.object)
    except (RuntimeError, OSError) as err:
        fallback(str(err))

    calls, violations = check(funcs, args.verbose)

    if not calls:
        fallback('no call sites found in %s' % args.object)

    if violations:
        for name, idx, reg in violations:
            sys.stderr.write('bpf_slot_check: %s: r%d is read after the call '
                             'at insn %d without being written first\n'
                             % (name, reg, idx))
        fallback('clang keeps a value in r2-r5 across a call')

    if args.verbose:
        sys.stderr.write('bpf_slot_check: %d call sites, r2-r5 never live '
                         'across a call\n' % calls)
    # Silence means the expression stack can stay based at r2.


if __name__ == '__main__':
    main()
