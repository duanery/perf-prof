# kprobe/uprobe 事件格式重设计 Implementation Plan

> **状态：已完成（2026-07-02）** — 六个任务全部落地，主线共 5 次提交：`db231e3 → 475b4a4 → b8a8977 → e07e3e7 → ec4ae9f`。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 perf-prof 的 kprobe/uprobe 事件格式从旧的 `kprobe:func`、`uprobe:func@"file"` 迁移到 bpftrace 风格：`kprobe[:module]:fn[+offset]`、`uprobe:binary:func[+offset]` / `uprobe:binary:offset`。同时把事件字符串的头部识别统一封装成一个 `parse_event_head()`，让 tracepoint / kprobe / uprobe / profiler 的头部解析、`sys:name` 拆分、以及 `/filter/` 起点定位在同一个函数里完成。

**Architecture:**
- 新增 `parse_event_head(char *s, struct event_head *out)`，一次性完成：
  1. 识别事件种类（tracepoint / profiler / k[ret]probe / u[ret]probe）
  2. 拆出 `sys` 与 `body`（对 tracepoint 是 name，对 probe 是待解析目标，对 profiler 是 profiler 名 + 后续 option 空间）
  3. 输出 head 结束位置指针（即 `/filter/` 中第一个 `/`），供后续拆 filter/ATTR 使用
  - 因为 uprobe binary 路径含 `/`，所以只有先识别出 kind、再决定"从哪里开始找 `/`"才能得到正确的 filter 起点。这就是本次重构的核心。
- 重写 `tp_kprobe_uprobe()` 只负责"拿到 sys 和 body → 填 tp 的 kprobe_func / uprobe_path / probe_offset"，不再判断 filter 位置。
- `tp_list_new()` 主循环改为：调 `parse_event_head` → 拆 filter/ATTR → 按 `head->kind` switch 分派到 tracepoint / profiler / probe 的具体处理。

**Tech Stack:** C（perf-prof 主体）、Python pytest（测试）、Markdown 文档。

---

## Completion Summary

| 任务 | 提交 | 交付内容 |
|-----|------|--------|
| Task 1 | `db231e3 tests: add kprobe/uprobe new-syntax expectations (TDD)` | 新增 `tests/test_probe_format.py`（11 个正/反测试） |
| Task 2 | `475b4a4 tep: bpftrace-style kprobe/uprobe event syntax + head classifier` | `src/tep.h`/`src/tep.c` 重写：`enum event_head_kind`、`struct event_head`、`parse_event_head()`、`tp_kprobe_uprobe()` 全新实现，`tp_list_new()` 头部改为分派 |
| Task 4 | `b8a8977 monitor: update -e/--event help for bpftrace-style probe syntax` | `src/monitor.c` 帮助字符串更新 |
| Task 3 | `e07e3e7 tests: migrate test_uprobe / test_uretprobe to bpftrace-style syntax` | `tests/test_trace.py` 中旧 `uprobe:printf@"..."` 语法迁移 |
| Task 5 | `ec4ae9f docs: update kprobe/uprobe syntax to bpftrace-style` | 11 个文档同步：`README.md`、`README_CN.md`、`docs/main_options.md`、`docs/profilers/{trace,kmemleak,multi-trace,sql}.md`、`docs/thread-tracking-design.md`、`skills/perf-prof/SKILL.md`、`skills/perf-prof/references/profilers/{trace,multi-trace,python}.md` |
| Task 6 | (校验，无提交) | make 通过；`test_probe_format.py` 11/11 PASS；`test_trace.py` 40/43 PASS（3 项与 TSC/环境相关的 pre-existing 失败在基线复现）；kprobe/kretprobe/uprobe/uretprobe smoke 全部命中；`kprobe:0xADDR+off`、`uretprobe:BINARY:fn+off`、旧 `uprobe:fn@"file"` 语法均按预期报错拒绝 |

**Plan 与实施的偏离**：
- Task 5 的执行范围被扩展：除 `docs/main_options.md`、`CLAUDE.md` 外，用户额外要求同步 `skills/perf-prof/` 目录及 `README.md`/`README_CN.md`/其他 `docs/` 内的旧语法示例。
- Task 4 与 Task 3 的执行顺序反转（先 4 后 3），plan 中记录的顺序仅为文档组织顺序，不影响正确性。
- Task 2 期间根据 review 反馈完成 3 处 fix-up（合并入原始 commit）：
  1. `expand_event_wildcards` 之前用 `strchr('/')` 定位，会误判 uprobe binary 路径中的 `/` — 改为通过 `parse_event_head` 分类，非 tracepoint 直接透传。
  2. `parse_event_head` 最初仍会临时 `\0` 分割 sys/body — 改为**不修改任何字符**，`\0` 由调用方负责，保持一致的字符修改契约。
  3. `kprobe:0xffffffff810bc4c0+23` 组合被静默接受 — 补上显式报错。

---


## File Structure

**修改：**
- `src/tep.h` — 新增 `enum event_head_kind`、`struct event_head`（若愿意保留在 .c 内部也可；这里为可测性放到头文件）。
- `src/tep.c` — 新增 `parse_event_head()`；重写 `tp_kprobe_uprobe()`；重写 `tp_list_new()` 中头部处理段（L826-891）。
- `src/monitor.c` — 更新 `-e/--event` 帮助字符串（L514-549）。
- `docs/main_options.md` — 同步文档（L269-276 附近）。
- `CLAUDE.md` — 更新事件格式描述。
- `tests/test_trace.py` — 迁移旧 `uprobe:printf@"..."` 语法。

**新增：**
- `tests/test_probe_format.py` — kprobe/uprobe 新语法与拒绝旧语法的正/反测试。

---

## 语法规范（本 plan 依据）

新格式（合法）：
```
kprobe:fn                     # 无 module，函数名
kprobe:fn+offset              # 无 module，函数名+偏移
kprobe:module:fn              # 有 module
kprobe:module:fn+offset       # 有 module + 偏移
kprobe:0xADDR                 # 内核地址（不支持 module，不支持 +offset）
kretprobe:fn
kretprobe:module:fn
kretprobe:0xADDR              # 支持地址（无 +offset）
uprobe:/abs/path/binary:func
uprobe:/abs/path/binary:func+offset
uprobe:/abs/path/binary:0xOFFSET     # 文件偏移（十六进制或十进制）
uretprobe:/abs/path/binary:func      # 不支持 func+offset
uretprobe:/abs/path/binary:0xOFFSET  # 支持文件偏移（用于消歧同名函数）
```

事件字符串完整形式：`<head>[/filter/ATTR/ATTR/.../]`。

**关键约束**（写在代码注释里）：
- kprobe/kretprobe 头部内部**不允许出现 `/`**——第一个 `/` 就是 filter 起点。
- uprobe/uretprobe 头部形如 `binary:target`，binary 可含 `/` 但不含 `:`（与 bpftrace 一致）；从 `binary:` 后面的第一个 `/` 才是 filter 起点。
- tracepoint 头部形如 `sys:name`，其中 `sys`、`name` 都不含 `/`。
- profiler 头部就是 profiler 名字（无 `/`）；后续 `/option/ATTR/ATTR/` 中，"option" 结构上占据 filter 的位置——保留现有把首 `/` 复原为空格再交给 `parse_string_options` 的做法。

**分类顺序**（在 `parse_event_head()` 里）：
1. 前缀是 `kprobe:` / `kretprobe:` / `uprobe:` / `uretprobe:` → probe
2. 否则拆出 head 部分（第一个 `/` 之前）：
   - 若 head 整体（可能含冒号，如 `bpf:kvm_exit`）能被 `monitor_find` 找到 → profiler。这一步优先，因为 profiler 名字本身可以包含 `:`。
   - 否则 head 中若含 `:` → tracepoint（`sys:name`）
   - 否则 → profiler（无冒号的普通 profiler，如 `profile`、`trace`）
3. 结果 kind + head 结束位置一并返回。

---

## Task 1: 写失败测试（TDD 起点）

**Files:**
- Create: `tests/test_probe_format.py`

- [x] **Step 1: 创建测试文件**

写 `tests/test_probe_format.py`。**规范约束**（对齐 `tests/` 现有用例，如 `test_trace.py`、`test_kmemleak.py`）：

1. 顶部固定 imports：`from PerfProf import PerfProf`、`from conftest import result_check`。
2. 每个正向用例的模板一律是：
   ```python
   prof = PerfProf([...])
   for std, line in prof.run(runtime, memleak_check):
       result_check(std, line, runtime, memleak_check)
   ```
   不能用 `prof.run(runtime, memleak_check)` 单行调用——那样不会消费 stdout/stderr、也不会让 `result_check` 触发失败。
3. **不要**直接 `subprocess.run(['./perf-prof', ...], capture_output=True)`；反证测试同样通过 `PerfProf.run()` 迭代 + 扫 stderr 是否含预期关键字。这样能沿用 fixture、避免路径漂移。
4. 事件字符串用**普通字符串拼接**（`'uprobe:' + libc + ':printf'`），保持与其他测试同一风格。
5. libc 探测独立成一个模块级 helper `_libc_path()`，用 `ctypes.util.find_library('c')` 得到文件名，再在 `/lib64/`、`/usr/lib64/`、`/lib/x86_64-linux-gnu/` 里挑存在的那个。

具体交付形态（也是仓库最终提交的内容）：

```python
#!/usr/bin/env python3

from PerfProf import PerfProf
from conftest import result_check
import ctypes.util
import os
import pytest


def _libc_path():
    libc_name = ctypes.util.find_library('c')
    if not libc_name:
        return None
    for d in ('/lib64/', '/usr/lib64/', '/lib/x86_64-linux-gnu/'):
        p = d + libc_name
        if os.path.exists(p):
            return p
    return None


# --- Positive: kprobe / kretprobe --------------------------------------------

def test_kprobe_new_bare(runtime, memleak_check):
    if not PerfProf.pmu_exists('kprobe'):
        pytest.skip("'kprobe' does not exist")
    prof = PerfProf(['trace', '-e', 'kprobe:try_to_wake_up', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


def test_kprobe_new_with_offset(runtime, memleak_check):
    if not PerfProf.pmu_exists('kprobe'):
        pytest.skip("'kprobe' does not exist")
    prof = PerfProf(['trace', '-e', 'kprobe:try_to_wake_up+0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


def test_kretprobe_new(runtime, memleak_check):
    if not PerfProf.pmu_exists('kprobe'):
        pytest.skip("'kprobe' does not exist")
    prof = PerfProf(['trace', '-e', 'kretprobe:try_to_wake_up', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


def test_kprobe_still_works_after_refactor(runtime, memleak_check):
    if not PerfProf.pmu_exists('kprobe'):
        pytest.skip("'kprobe' does not exist")
    prof = PerfProf(
        ['trace', '-e', 'kprobe:try_to_wake_up/common_pid+1>10/', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# --- Positive: uprobe / uretprobe --------------------------------------------

def test_uprobe_new_absolute_path(runtime, memleak_check):
    if not PerfProf.pmu_exists('uprobe'):
        pytest.skip("'uprobe' does not exist")
    libc = _libc_path()
    if libc is None:
        pytest.skip("libc.so.6 not found")
    e = 'uprobe:' + libc + ':printf'
    prof = PerfProf(['trace', '-e', e, '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


def test_uprobe_new_with_filter(runtime, memleak_check):
    if not PerfProf.pmu_exists('uprobe'):
        pytest.skip("'uprobe' does not exist")
    libc = _libc_path()
    if libc is None:
        pytest.skip("libc.so.6 not found")
    e = 'uprobe:' + libc + ':printf/cpu==0/'
    prof = PerfProf(['trace', '-e', e, '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


def test_uretprobe_new(runtime, memleak_check):
    if not PerfProf.pmu_exists('uprobe'):
        pytest.skip("'uprobe' does not exist")
    libc = _libc_path()
    if libc is None:
        pytest.skip("libc.so.6 not found")
    e = 'uretprobe:' + libc + ':printf'
    prof = PerfProf(['trace', '-e', e, '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


def test_uretprobe_new_bare_offset(runtime, memleak_check):
    # uretprobe with a bare file offset — used to disambiguate duplicate
    # function names inside the binary. Discover printf's file offset via
    # nm(1) so the test exercises a real, valid offset.
    if not PerfProf.pmu_exists('uprobe'):
        pytest.skip("'uprobe' does not exist")
    libc = _libc_path()
    if libc is None:
        pytest.skip("libc.so.6 not found")
    import subprocess
    try:
        out = subprocess.Popen(['nm', '-D', libc],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               universal_newlines=True).communicate()[0]
    except FileNotFoundError:
        pytest.skip("nm(1) not available")
    off_hex = None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1] == 'printf' and parts[-2] in ('T', 'W', 'i'):
            off_hex = '0x' + (parts[0].lstrip('0') or '0')
            break
    if off_hex is None:
        pytest.skip("printf offset not found in libc")
    e = 'uretprobe:' + libc + ':' + off_hex
    prof = PerfProf(['trace', '-e', e, '-C', '0', '-m', '64'])
    for std, line in prof.run(runtime, memleak_check):
        result_check(std, line, runtime, memleak_check)


# --- Negative: parser must reject --------------------------------------------
#
# Negative tests use PerfProf with a short runtime and scan stderr for a
# parser rejection message. If the parser accepts the malformed event, the
# tool starts running and stderr stays quiet — we detect that as a failure.

def _expect_parse_reject(prof, keywords):
    """Iterate prof output; assert stderr contains any of `keywords`."""
    seen_reject = False
    for std, line in prof.run(3, 0):
        if std == PerfProf.STDERR:
            low = line.lower()
            if any(k in low for k in keywords):
                seen_reject = True
    assert seen_reject, \
        "expected parser to reject the event, but no matching stderr message"


def test_uretprobe_rejects_func_plus_offset():
    if not PerfProf.pmu_exists('uprobe'):
        pytest.skip("'uprobe' does not exist")
    libc = _libc_path()
    if libc is None:
        pytest.skip("libc.so.6 not found")
    e = 'uretprobe:' + libc + ':printf+0'
    prof = PerfProf(['trace', '-e', e, '-m', '64', '-N', '1'])
    _expect_parse_reject(prof, ['uretprobe', 'offset', 'not found', 'malformed'])


def test_old_uprobe_at_syntax_rejected():
    if not PerfProf.pmu_exists('uprobe'):
        pytest.skip("'uprobe' does not exist")
    libc = _libc_path()
    if libc is None:
        pytest.skip("libc.so.6 not found")
    e = 'uprobe:printf@"' + libc + '"'
    prof = PerfProf(['trace', '-e', e, '-m', '64', '-N', '1'])
    _expect_parse_reject(prof, ['uprobe', 'not found', 'malformed', 'not accessible'])


# --- profiler-name-with-colon regression -------------------------------------

def test_bpf_kvm_exit_profiler_still_works():
    # bpf:kvm_exit is a single profiler name (registered via MONITOR_REGISTER
    # with .name="bpf:kvm_exit") — the ':' is part of the profiler name, not
    # a sys:name separator. Ensure the parser classifies it as PROFILER, not
    # TRACEPOINT / kprobe / uprobe.
    prof = PerfProf(['trace', '-e', 'bpf:kvm_exit', '-N', '1'])
    for std, line in prof.run(3, 0):
        if std == PerfProf.STDERR:
            low = line.lower()
            assert 'bpf:kvm_exit not found' not in low, \
                "bpf:kvm_exit misclassified as tracepoint: " + line
            assert 'profiler bpf not found' not in low, \
                "bpf:kvm_exit misclassified via 'bpf' prefix: " + line
```

- [x] **Step 2: 运行测试确认失败**

Run: `cd tests && pytest test_probe_format.py -v 2>&1 | tail -40`
Expected:
- `test_kprobe_new_bare`, `test_kretprobe_new`, `test_kprobe_still_works_after_refactor` PASS（旧代码就支持）
- `test_kprobe_new_with_offset` FAIL（旧代码不支持 `+offset`）
- `test_uprobe_new_*`、`test_uretprobe_new` FAIL（旧代码只认 `@"path"` 语法）
- `test_old_uprobe_at_syntax_rejected` PASS（当前旧语法确实工作，测试断言"应失败"，因此这一条现在会 FAIL）—— 这是预期，Task 2 会翻转它

**注意**：`test_old_uprobe_at_syntax_rejected` 是"新语法生效后旧语法应被拒绝"的反证测试。目前它 FAIL 是正常的。

- [x] **Step 3: Commit**

```bash
git add tests/test_probe_format.py
git commit -m "tests: add kprobe/uprobe new-syntax expectations (TDD, currently failing)"
```

---

## Task 2: 引入统一的 `parse_event_head()` 头部解析器 + 重写 `tp_kprobe_uprobe()` + 重写 dispatch

这是本次重构的核心 task。步骤颗粒度小，逐步推进。

**Files:**
- Modify: `src/tep.h`
- Modify: `src/tep.c`

- [x] **Step 1: 在 `src/tep.h` 声明公开类型**

在 `src/tep.h` 现有 `enum` `KPROBE`/`KRETPROBE`/`UPROBE`/`URETPROBE`（约 L196-199）之后追加：

```c
enum event_head_kind {
    EVT_HEAD_TRACEPOINT,   /* sys:name */
    EVT_HEAD_PROFILER,     /* profiler[/option/...] or profiler:X (e.g., bpf:kvm-exit) */
    EVT_HEAD_KPROBE,       /* kprobe:[module:]fn[+off] | kprobe:0xADDR */
    EVT_HEAD_KRETPROBE,    /* kretprobe:[module:]fn    | kretprobe:0xADDR */
    EVT_HEAD_UPROBE,       /* uprobe:binary:fn[+off]   | uprobe:binary:offset */
    EVT_HEAD_URETPROBE,    /* uretprobe:binary:fn */
};

struct event_head {
    enum event_head_kind kind;
    /* For tracepoint & probe: `sys` and `body` are '\0'-terminated slices
     * of the original event string (the ':' between them has been overwritten
     * with '\0'). For profiler: `sys` is the profiler name; `body` is NULL. */
    char *sys;
    char *body;
    /* Points at the '/' that starts /filter/... within the event string,
     * or NULL if there is no /filter/ block. Not '\0'-terminated by the
     * parser — the caller is responsible for splitting further. */
    char *head_end;
};

/* Returns 0 on success; -1 on structural failure (e.g., missing ':' after
 * a probe prefix). Does NOT validate that `sys` names an existing tracepoint
 * subsystem, kernel symbol, uprobe binary, etc. — that's the caller's job. */
int parse_event_head(char *s, struct event_head *out);
```

- [x] **Step 2: 编译验证声明可用**

Run: `make V=1 2>&1 | tail -5`
Expected: 编译通过（新增 enum + struct 声明只增不减）。若报 `parse_event_head` 未定义引用——只是声明，链接不发生调用点，无引用问题。

- [x] **Step 3: 在 `src/tep.c` 实现 `parse_event_head()`**

在 `src/tep.c` `next_sep()` 定义之后（约 L590）追加：

```c
static bool head_starts_with(const char *s, const char *prefix, char **rest)
{
    size_t n = strlen(prefix);
    if (strncmp(s, prefix, n) == 0) {
        if (rest) *rest = (char *)s + n;
        return true;
    }
    return false;
}

/* For kprobe/kretprobe head, first '/' is the filter delimiter — head has no
 * '/' inside. */
static char *kprobe_head_end(char *body)
{
    return next_sep(body, '/');
}

/* For uprobe/uretprobe, head is "binary:target". `binary` may contain '/',
 * but not ':' (matching bpftrace). Find the first ':' → target start; then
 * find the first '/' after target → filter delimiter. */
static char *uprobe_head_end(char *body)
{
    char *colon = strchr(body, ':');
    if (!colon)
        return NULL; /* malformed; caller detects via tp_kprobe_uprobe */
    return next_sep(colon + 1, '/');
}

int parse_event_head(char *s, struct event_head *out)
{
    char *rest = NULL;
    char *colon;
    char *slash_naive;

    memset(out, 0, sizeof(*out));

    if (head_starts_with(s, "kprobe:", &rest)) {
        out->kind = EVT_HEAD_KPROBE;
        out->sys = s;
        out->body = rest;
        out->head_end = kprobe_head_end(rest);
        rest[-1] = '\0'; /* terminate sys */
        return 0;
    }
    if (head_starts_with(s, "kretprobe:", &rest)) {
        out->kind = EVT_HEAD_KRETPROBE;
        out->sys = s;
        out->body = rest;
        out->head_end = kprobe_head_end(rest);
        rest[-1] = '\0';
        return 0;
    }
    if (head_starts_with(s, "uprobe:", &rest)) {
        out->kind = EVT_HEAD_UPROBE;
        out->sys = s;
        out->body = rest;
        out->head_end = uprobe_head_end(rest);
        rest[-1] = '\0';
        return 0;
    }
    if (head_starts_with(s, "uretprobe:", &rest)) {
        out->kind = EVT_HEAD_URETPROBE;
        out->sys = s;
        out->body = rest;
        out->head_end = uprobe_head_end(rest);
        rest[-1] = '\0';
        return 0;
    }

    /* Non-probe. Determine tracepoint vs profiler.
     *
     * A profiler name may itself contain ':' (e.g. "bpf:kvm_exit" is a single
     * profiler name registered via MONITOR_REGISTER). So the whole head — up
     * to the first '/' if any — must be tried against monitor_find() FIRST.
     * Only if that fails, and the head contains ':', do we fall back to
     * "sys:name" tracepoint parsing. */
    slash_naive = next_sep(s, '/');
    if (slash_naive)
        *slash_naive = '\0';

    if (monitor_find(s)) {
        if (slash_naive) *slash_naive = '/';
        out->kind = EVT_HEAD_PROFILER;
        out->sys = s;
        out->body = NULL;
        out->head_end = slash_naive;
        return 0;
    }

    colon = strchr(s, ':');
    if (colon) {
        /* tracepoint sys:name */
        *colon = '\0';
        out->kind = EVT_HEAD_TRACEPOINT;
        out->sys = s;
        out->body = colon + 1;
        if (slash_naive) *slash_naive = '/';
        out->head_end = slash_naive;
        return 0;
    }

    /* No ':' in head, and monitor_find() didn't recognize it either — treat
     * as profiler; the caller will produce a "profiler X not found" error
     * once it re-runs monitor_find(). */
    if (slash_naive) *slash_naive = '/';
    out->kind = EVT_HEAD_PROFILER;
    out->sys = s;
    out->body = NULL;
    out->head_end = slash_naive;
    return 0;
}
```

- [x] **Step 4: 编译**

Run: `make V=1 2>&1 | tail -10`
Expected: 通过（`parse_event_head` 定义完整；三个静态帮助函数都被使用）。
若报"函数未使用"，Task 5 会用它们；此处应不会报，因为都在 `parse_event_head` 内部调用。

- [x] **Step 5: 重写 `tp_kprobe_uprobe()`（`src/tep.c` L605-644）**

此函数改成"接收已经分类好的 sys 与 body（无 filter），填 tp 的 kprobe_func / uprobe_path / probe_offset"：

```c
static int parse_offset(const char *s, unsigned long *out)
{
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return -1;
    v = strtoul(s, &end, 0); /* auto-detects 0x prefix */
    if (!end || *end) return -1;
    *out = v;
    return 0;
}

/*
 * Fill tp->{kprobe_func, uprobe_path, probe_offset} from a probe body.
 *
 * `kind` is one of EVT_HEAD_K{,RET}PROBE / EVT_HEAD_U{,RET}PROBE.
 * `body` is the substring AFTER the leading "sys:" prefix, with any /filter/
 *        already stripped (i.e., the head's target-only portion).
 *
 * Formats accepted:
 *   k[ret]probe:  fn | fn+off | module:fn | module:fn+off | 0xADDR
 *                 (kretprobe rejects +off)
 *   uprobe:       /path/to/bin:fn | /path/to/bin:fn+off | /path/to/bin:offset
 *   uretprobe:    /path/to/bin:fn | /path/to/bin:offset
 *                 (rejects fn+off; a bare offset is allowed to disambiguate
 *                  duplicate-named symbols in the binary)
 *
 * Returns id (KPROBE / KRETPROBE / UPROBE / URETPROBE) or -1.
 */
static int tp_kprobe_uprobe(struct tp *tp, enum event_head_kind kind, char *body)
{
    int id;
    bool is_ret;
    bool is_uprobe;
    char resolved_path[PATH_MAX];

    switch (kind) {
    case EVT_HEAD_KPROBE:    id = kprobe_type ? KPROBE    : -1; is_ret = false; is_uprobe = false; break;
    case EVT_HEAD_KRETPROBE: id = kprobe_type ? KRETPROBE : -1; is_ret = true;  is_uprobe = false; break;
    case EVT_HEAD_UPROBE:    id = uprobe_type ? UPROBE    : -1; is_ret = false; is_uprobe = true;  break;
    case EVT_HEAD_URETPROBE: id = uprobe_type ? URETPROBE : -1; is_ret = true;  is_uprobe = true;  break;
    default: return -1;
    }
    if (id < 0)
        return -1;

    tp->kprobe_func = NULL;
    tp->uprobe_path = NULL;
    tp->probe_offset = 0;

    if (is_uprobe) {
        /* body = "/path/to/bin:target" (binary contains no ':') */
        char *first_colon = strchr(body, ':');
        char *target;
        char *plus;
        unsigned long off = 0;

        if (!first_colon || first_colon == body) {
            fprintf(stderr, "uprobe: expected 'binary:target', got '%s'\n", body);
            return -1;
        }
        *first_colon = '\0';
        target = first_colon + 1;

        if (!realpath(body, resolved_path) ||
            access(resolved_path, X_OK) != 0) {
            fprintf(stderr, "uprobe: binary '%s' not accessible\n", body);
            return -1;
        }
        tp->uprobe_path = body;

        plus = strchr(target, '+');
        if (plus) {
            if (is_ret) {
                fprintf(stderr, "uretprobe: '+offset' not supported\n");
                return -1;
            }
            *plus = '\0';
            if (parse_offset(plus + 1, &off) < 0) {
                fprintf(stderr, "uprobe: bad offset '%s'\n", plus + 1);
                return -1;
            }
        }
        if (*target) {
            unsigned long sym_off = syms__file_offset(resolved_path, target);
            if (sym_off) {
                tp->probe_offset = sym_off + off;
            } else {
                unsigned long num_off;
                if (plus || parse_offset(target, &num_off) < 0) {
                    fprintf(stderr, "uprobe: symbol '%s' not found in %s\n",
                            target, resolved_path);
                    return -1;
                }
                tp->probe_offset = num_off;
            }
        }
        if (!tp->probe_offset)
            return -1;
        return id;
    }

    /* k[ret]probe */
    {
        char *plus;
        unsigned long off = 0;
        char *first_colon = strchr(body, ':');

        /* "0xADDR" (or decimal address) form: only valid when there is no
         * module prefix and no '+offset'. */
        if (!first_colon && !strchr(body, '+') &&
            body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) {
            unsigned long addr;
            if (parse_offset(body, &addr) < 0) {
                fprintf(stderr, "kprobe: bad address '%s'\n", body);
                return -1;
            }
            tp->kprobe_func = NULL;
            tp->probe_offset = addr;
            return id;
        }

        /* Symbol form: fn | fn+off | module:fn | module:fn+off.
         * Kernel accepts "module:fn" via kprobe_func passthrough. */
        plus = strchr(first_colon ? first_colon + 1 : body, '+');
        if (plus) {
            if (is_ret) {
                fprintf(stderr, "kretprobe: '+offset' not supported\n");
                return -1;
            }
            *plus = '\0';
            if (parse_offset(plus + 1, &off) < 0) {
                fprintf(stderr, "kprobe: bad offset '%s'\n", plus + 1);
                return -1;
            }
        }
        tp->kprobe_func = body;
        tp->probe_offset = off;
    }
    return id;
}
```

- [x] **Step 6: 编译**

Run: `make V=1 2>&1 | tail -10`
Expected: 编译通过。旧的 `tp_kprobe_uprobe(struct tp *, char *sys, char *name)` 签名已改为 `(struct tp *, enum event_head_kind, char *body)`——`tp_list_new` 中原有调用点（L875）会报"参数类型不匹配"。**这在 Step 7 修好前是预期状态**；先继续下一步。

如需临时让编译通过（以便在 Step 7 前 push），可以先把 Step 5 的旧函数保留一个 wrapper：
```c
static int tp_kprobe_uprobe_legacy(struct tp *tp, char *sys, char *name)
{
    (void)tp; (void)sys; (void)name;
    return -1; /* unused after Step 7 */
}
```
并把 L875 的调用暂时替换为 `tp_kprobe_uprobe_legacy(tp, sys, name)`。**Step 7 会删除**。

**建议做法：Step 6 与 Step 7 合并到一次编译验证**（即 Step 6 允许暂时报错，直接进 Step 7）。

- [x] **Step 7: 重写 `tp_list_new()` 头部处理段（`src/tep.c` L826-891）**

将 `for (i = 0; i < nr_tp; i++)` 循环体内 L826 到 L891 之间的代码整块替换：

```c
        s = tp->name;
        {
            struct event_head head;
            if (parse_event_head(s, &head) < 0) {
                fprintf(stderr, "malformed event: %s\n", s);
                goto err_out;
            }

            slash = head.head_end;
            if (slash) {
                *slash = '\0';
                filter = slash + 1;
                slash = next_sep(filter, '/');
                if (!slash)
                    goto err_out;
                *slash = '\0';
            }

            switch (head.kind) {
            case EVT_HEAD_PROFILER:
                sys = head.sys;
                if (filter && filter[0])
                    filter[-1] = ' ';
                rm_quotes(filter, ' ');
                env = parse_string_options(s);
                if (!env) {
                    fprintf(stderr, "profiler %s: bad option string\n", sys);
                    goto err_out;
                }
                if (!monitor_find(sys)) {
                    fprintf(stderr, "profiler %s not found\n", sys);
                    goto err_out;
                }
                prof = monitor_find(sys);
                env->tsc = dev->env->tsc;
                env->clock_offset = dev->env->clock_offset;
                if (env->kvmclock) free(env->kvmclock);
                env->kvmclock = dev->env->kvmclock ? strdup(dev->env->kvmclock) : NULL;
                tp->source_dev = prof_dev_open_cpu_thread_map(prof, env, dev->cpus, dev->threads, dev);
                if (!tp->source_dev)
                    goto err_out;
                prof_dev_use(tp->source_dev);
                name = sys;
                sys = NULL;
                break;

            case EVT_HEAD_TRACEPOINT:
                sys = head.sys;
                name = rm_quotes(head.body, '\0');
                id = tep__event_id(sys, name);
                if (id < 0) {
                    fprintf(stderr, "%s:%s not found\n", sys, name);
                    goto err_out;
                }
                event = tep_find_event_by_name(tep, sys, name);
                if (!event)
                    goto err_out;
                filter = rm_quotes(filter, '\0');
                break;

            case EVT_HEAD_KPROBE:
            case EVT_HEAD_KRETPROBE:
            case EVT_HEAD_UPROBE:
            case EVT_HEAD_URETPROBE:
                sys = head.sys;
                id = tp_kprobe_uprobe(tp, head.kind, head.body);
                if (id < 0)
                    goto err_out;
                name = head.body;
                filter = rm_quotes(filter, '\0');
                break;
            }
        }
        // ATTR
        if (slash) {
```

**注意**：这一段之后紧接的是原有的 `if (slash) { *slash = '\0'; s = slash + 1; while ...` ATTR 循环，无需改动。原来 L892 的 `// ATTR` 注释保留。

删掉原来的 `profiler:` label（不再需要 `goto profiler`）以及原来那段 `if (!sep) { profiler: ... } else { ... }` 分支——已被上面的 switch 完整替代。

- [x] **Step 8: 编译**

Run: `make V=1 2>&1 | tail -20`
Expected: 通过。若某些变量（`sep`、`slash`、`event`）在 switch 之后不再使用，编译器可能 warn；将它们保留即可（后续 ATTR 循环仍会用 `sep`）。

- [x] **Step 9: 运行 Task 1 的测试**

Run: `cd tests && pytest test_probe_format.py -v 2>&1 | tail -40`
Expected: 全部 PASS：
- 六个新语法用例 PASS
- `test_old_uprobe_at_syntax_rejected` PASS（旧语法解析失败，返回非 0）
- `test_kprobe_still_works_after_refactor` PASS
- `test_bpf_kvm_exit_profiler_still_works` PASS

- [x] **Step 10: 运行现有 kprobe/uprobe 相关测试确认无回归**

Run:
```bash
cd tests && pytest test_trace.py::test_kprobe test_trace.py::test_kprobe1 \
    test_trace.py::test_kprobe_tsc test_multi-trace.py::test_multi_trace_kprobe \
    -v 2>&1 | tail -30
```
Expected: 全 PASS（旧的 `kprobe:try_to_wake_up` 语法无 `+offset` / `module:` / `@`，兼容新解析器）。

**注意**：`test_trace.py::test_uprobe*` 使用了旧 `uprobe:printf@"..."` 语法，此处会 FAIL——由 Task 3 迁移。

- [x] **Step 11: Commit**

```bash
git add src/tep.h src/tep.c
git commit -m "tep: unify event head parsing across tracepoint / probe / profiler

Introduce parse_event_head() which classifies the event kind (tracepoint,
profiler, k[ret]probe, u[ret]probe) and returns the head-end pointer.
Only after classification can we correctly locate /filter/ — for uprobe,
the binary path may contain '/' which used to be confused with the filter
delimiter.

Rewrites tp_kprobe_uprobe() to consume the pre-parsed body and support
the new bpftrace-style syntax:
  kprobe[:module]:fn[+offset] | kprobe:0xADDR
  kretprobe[:module]:fn       | kretprobe:0xADDR
  uprobe:BINARY:func[+offset] | uprobe:BINARY:offset
  uretprobe:BINARY:func

The old uprobe:func@\"file\" syntax is no longer accepted."
```

---

## Task 3: 迁移 tests/ 目录下所有受影响的测试用例

**Files:**
- Modify: `tests/test_trace.py` (uprobe/uretprobe `@"path"` 语法 → 新语法)
- Verify unchanged: `tests/test_multi-trace.py`、`tests/test_python.py`、`tests/test_trace.py` 里的 kprobe/kretprobe 简单形式

**扫描结果**（`grep -rn 'uprobe:\|uretprobe:\|kprobe:\|kretprobe:' tests/`）：

| 文件:行 | 现有写法 | 需要迁移？ | 原因 |
|---|---|---|---|
| tests/test_trace.py:135 | `kprobe:try_to_wake_up/cpu==0/` | 否 | 无 `+offset`/`module:`/`@`；新解析器把首 `/` 识别为 filter 起点，兼容 |
| tests/test_trace.py:145 | `kprobe:try_to_wake_up/common_pid+1>10/` | 否 | filter 里的 `+` 位于第一个 `/` 之后，不影响头部解析 |
| tests/test_trace.py:155 | `kprobe:try_to_wake_up` | 否 | 兼容 |
| tests/test_trace.py:165 | `kretprobe:try_to_wake_up` | 否 | 兼容 |
| tests/test_trace.py:179 | `uprobe:printf@"$libc"/cpu==0/` | **是** | 旧 `func@"path"` 语法在 Task 2 之后被拒 |
| tests/test_trace.py:195 | `uretprobe:printf@"$libc"` | **是** | 同上 |
| tests/test_multi-trace.py:54-55 | `kprobe:try_to_wake_up`, `kretprobe:try_to_wake_up` | 否 | 兼容 |
| tests/test_python.py:544 | `kprobe:try_to_wake_up` | 否 | 兼容 |
| tests/test_python.py:557 | `kretprobe:try_to_wake_up` | 否 | 兼容 |

只有 `tests/test_trace.py` 的 `test_uprobe` 和 `test_uretprobe` 两个函数需要改。

- [x] **Step 1: 二次确认扫描结果**

Run: `grep -rn 'uprobe:\|uretprobe:\|kprobe:\|kretprobe:' tests/ 2>/dev/null | grep -v __pycache__`
Expected: 输出与上表完全一致。若出现新增行（例如仓库在此期间添加了新测试），需一起纳入本 task。

Run: `grep -n 'uprobe:.*@\|uretprobe:.*@' tests/*.py`
Expected: 只有 `tests/test_trace.py:179` 和 `tests/test_trace.py:195` 两行。

- [x] **Step 2: 迁移 `tests/test_trace.py` 的 `test_uprobe`（L179）**

打开 `tests/test_trace.py`，把：
```python
    e = 'uprobe:printf@"' + libc_path + '"/cpu==0/'
```
改成：
```python
    e = 'uprobe:' + libc_path + ':printf/cpu==0/'
```

同时更新紧邻的注释 L180：
```python
    #perf-prof trace -e uprobe:printf@"/lib64/libc.so.6"
```
改成：
```python
    #perf-prof trace -e 'uprobe:/lib64/libc.so.6:printf/cpu==0/'
```

- [x] **Step 3: 迁移 `tests/test_trace.py` 的 `test_uretprobe`（L195）**

在同一文件中，把：
```python
    e = 'uretprobe:printf@"' + libc_path + '"'
```
改成：
```python
    e = 'uretprobe:' + libc_path + ':printf'
```

同时更新紧邻的注释 L196：
```python
    #perf-prof trace -e uretprobe:printf@"/lib64/libc.so.6" -C 0
```
改成：
```python
    #perf-prof trace -e 'uretprobe:/lib64/libc.so.6:printf' -C 0
```

- [x] **Step 4: 全量搜检查残余**

Run: `grep -n '@"' tests/*.py | grep -v __pycache__`
Expected: 无 uprobe/uretprobe 相关的 `@"..."` 残余（其他 `@"` 若存在则属于无关字符串，忽略）。

- [x] **Step 5: 跑 test_trace.py 全部用例**

Run: `cd tests && pytest test_trace.py -v 2>&1 | tail -60`
Expected: 全 PASS（无 kprobe/uprobe PMU 时会 skip，也算 PASS）。特别关注：
- `test_kprobe`、`test_kprobe1`、`test_kprobe_tsc`、`test_kretprobe` PASS（旧 kprobe 语法向后兼容）
- `test_uprobe`、`test_uretprobe` PASS（迁移后的新语法）

- [x] **Step 6: 跑 kprobe/kretprobe 保持兼容的其他文件**

Run: `cd tests && pytest test_multi-trace.py::test_multi_trace_kprobe test_python.py -v 2>&1 | tail -40`
Expected: 全 PASS 或 skip（不能因解析器变化而回归）。

**注意**: `tests/test_python.py` 的这两个 kprobe 用例调用 `python` profiler + kprobe 事件，参与联合分析。若失败，说明 Task 2 的 dispatch 逻辑对"profiler + kprobe 事件源"组合处理不周——需要回到 Task 2 修复；不要在这里改测试。

- [x] **Step 7: 跑 test_probe_format.py 确认没退化**

Run: `cd tests && pytest test_probe_format.py -v 2>&1 | tail -30`
Expected: 全 PASS。

- [x] **Step 8: Commit**

```bash
git add tests/test_trace.py
git commit -m "tests: migrate uprobe/uretprobe tests to bpftrace-style syntax

kprobe/kretprobe tests unchanged — the plain 'kprobe:try_to_wake_up'
form (no module, no +offset, no @path) is compatible with the new
parser."
```

---

## Task 4: 更新 `-e/--event` 帮助字符串

**Files:**
- Modify: `src/monitor.c` L514-549

- [x] **Step 1: 编辑 `src/monitor.c`**

找到 L518-519：
```c
"         kprobe:func[/filter/ATTR/ATTR/.../]\n"
"         uprobe:func@\"file\"[/filter/ATTR/ATTR/.../]\n"
```

替换为：
```c
"         kprobe[:module]:fn[+offset][/filter/ATTR/ATTR/.../]\n"
"         kprobe:0xADDR[/filter/ATTR/ATTR/.../]\n"
"         kretprobe[:module]:fn[/filter/ATTR/ATTR/.../]\n"
"         uprobe:BINARY:func[+offset][/filter/ATTR/ATTR/.../]\n"
"         uprobe:BINARY:0xOFFSET[/filter/ATTR/ATTR/.../]\n"
"         uretprobe:BINARY:func[/filter/ATTR/ATTR/.../]\n"
"         uretprobe:BINARY:0xOFFSET[/filter/ATTR/ATTR/.../]\n"
```

- [x] **Step 2: 编译**

Run: `make V=1 2>&1 | tail -5`
Expected: PASS.

- [x] **Step 3: 手动核对**

Run: `./perf-prof trace -h 2>&1 | grep -A2 -B2 'kprobe\|uprobe'`
Expected: 输出显示新的七行说明。

- [x] **Step 4: Commit**

```bash
git add src/monitor.c
git commit -m "monitor: update -e help text for new kprobe/uprobe syntax"
```

---

## Task 5: 更新文档

**Files:**
- Modify: `docs/main_options.md`
- Modify: `CLAUDE.md`

- [x] **Step 1: 更新 `docs/main_options.md`**

在 `-e, --event` 条目（L271 附近），把 `**示例**` 一行扩展为：

```markdown
  - **示例**: [
      "sched:sched_wakeup",
      "kprobe:try_to_wake_up",
      "kprobe:try_to_wake_up+0",
      "kretprobe:try_to_wake_up",
      "uprobe:/lib64/libc.so.6:printf",
      "uretprobe:/lib64/libc.so.6:printf",
      "sched:sched_wakeup,sched:sched_switch"
    ]
```

- [x] **Step 2: 更新 `CLAUDE.md`**

Run: `grep -n 'kprobe:func\|uprobe:func@' CLAUDE.md`
定位到 "perf-prof 事件格式" 章节的旧四行，替换为：

```
      kprobe[:module]:fn[+offset][/filter/ATTR/ATTR/.../]
      kprobe:0xADDR[/filter/ATTR/ATTR/.../]
      kretprobe[:module]:fn[/filter/ATTR/ATTR/.../]
      uprobe:binary:func[+offset][/filter/ATTR/ATTR/.../]
      uprobe:binary:offset[/filter/ATTR/ATTR/.../]
      uretprobe:binary:func[/filter/ATTR/ATTR/.../]
```

紧随的 bullet 说明替换为：

```
- `kprobe[:module]:fn[+offset]` 指定kprobe事件；`module` 可省略；`fn` 可换成 `0xADDR`（十六进制内核地址，此形式不支持 module/+offset）。示例：`kprobe:try_to_wake_up`、`kprobe:try_to_wake_up+0`、`kprobe:0xffffffff81234000`
- `kretprobe[:module]:fn` 指定kretprobe事件（不支持 +offset）。示例：`kretprobe:try_to_wake_up`
- `uprobe:binary:func[+offset]` / `uprobe:binary:offset` 指定uprobe事件；`binary` 是可执行文件绝对路径（不含冒号），`func` 是符号名，`offset` 也可作为文件偏移。示例：`uprobe:/usr/local/bin/qemu-system-x86_64:address_space_rw`
- `uretprobe:binary:func` 指定uretprobe事件（不支持 +offset）。示例：`uretprobe:/usr/local/bin/qemu-system-x86_64:address_space_rw`
```

- [x] **Step 3: 全文搜检查残余旧格式描述**

Run: `grep -rn 'uprobe:[^"]*@"' docs/ CLAUDE.md 2>/dev/null | head -20`
对每处（若属正文说明或示例）改为新语法。忽略 `docs/site.tgz` 之类的二进制/生成物。

- [x] **Step 4: Commit**

```bash
git add docs/main_options.md CLAUDE.md
git commit -m "docs: describe new bpftrace-style kprobe/uprobe syntax"
```

---

## Task 6: 端到端验证

- [x] **Step 1: 全量测试**

Run: `cd tests && pytest -x 2>&1 | tail -60`
Expected: 全 PASS（除硬件/内核不可用而 skip 的用例）。

- [x] **Step 2: 手动烟雾测试**

Run:
```bash
LIBC=$(realpath /usr/lib64/libc.so.6 2>/dev/null || realpath /lib64/libc.so.6)
./perf-prof trace -e 'kprobe:try_to_wake_up' -N 3
./perf-prof trace -e 'kretprobe:try_to_wake_up' -N 3
./perf-prof trace -e "uprobe:$LIBC:printf" -N 3
./perf-prof trace -e 'kprobe:try_to_wake_up/cpu==0/stack/' -N 3
./perf-prof trace -e 'kprobe:try_to_wake_up+0' -N 3
```
Expected: 每条都能正常启动、正常退出，无 "not found" 或 segfault。

- [x] **Step 3: 验证旧格式被拒**

Run:
```bash
LIBC=$(realpath /usr/lib64/libc.so.6 2>/dev/null || realpath /lib64/libc.so.6)
./perf-prof trace -e "uprobe:printf@\"$LIBC\"" -N 1; echo "exit=$?"
```
Expected: 非零退出，stderr 出现 "uprobe: binary … not accessible" 或 "malformed event" 之类信息。

- [x] **Step 4: 验证 profiler 分派仍正确**

Run: `./perf-prof trace -e 'sched:sched_wakeup' -N 1; echo exit=$?`
Expected: exit=0；tracepoint 分派正常。

`bpf:kvm_exit` 是完整含冒号的 profiler 名（在 `src/bpf_kvm_exit.c` 里 `struct monitor bpf_kvm_exit.name = "bpf:kvm_exit"` 注册），必须整段匹配 `monitor_find` 才能命中。测试：
```bash
./perf-prof -e 'bpf:kvm_exit' -N 1 2>&1; echo exit=$?
```
Expected: 不因头解析而报 "bpf:kvm_exit not found"、"profiler bpf not found"、也不误进 kprobe/tracepoint 路径（可能因硬件报别的错，但解析层必须成功识别为 profiler）。

- [x] **Step 5: 若发现问题**

如果第 1-4 步暴露 bug，修补并 commit：
```bash
git add -A
git commit -m "tep: fix issue found during end-to-end verification"
```

---

## Self-Review 检查

**Spec coverage:**
- 新格式六种 → Task 1 测试 + Task 2 实现
- 帮助文本更新 → Task 4
- main_options + CLAUDE.md 文档更新 → Task 5
- tracepoint / profiler（含 `bpf:kvm_exit`——完整含冒号的 profiler 名）与 probe 的分类 → Task 2 `parse_event_head` 用**整个 head** 调 `monitor_find` 优先判断
- 路径感知 filter 分隔符 → Task 2 `uprobe_head_end`

**Placeholder scan:**
- 无 TBD / TODO / "similar to Task X"
- 每个代码步骤都有完整代码块

**Type consistency:**
- `enum event_head_kind`（Task 2 Step 1）在 `parse_event_head`、dispatch switch、`tp_kprobe_uprobe` 参数中一致
- `struct event_head` 字段（`sys`, `body`, `head_end`, `kind`）用法一致
- `probe_offset` 字段类型注意：现为 `int`，若测试或运行发现 kprobe 内核地址 (>2GB) 溢出，需将 `struct tp::probe_offset` 改成 `unsigned long`（同步改 `src/tep.c:1411, 1420` 赋值），Task 6 期间作为 fix-up

**Edge cases 已考虑：**
- 空 body、空 target、缺 `:`、`+offset` 数字非法 → 全部有明确 fprintf 报错
- kretprobe 拒绝 `+offset`；uretprobe 拒绝 `func+offset` 但**允许** `bare_offset`（用于消歧同名函数）→ 显式检查
- profiler 名字含冒号（`bpf:kvm_exit` 是单个 profiler 名，不是 `bpf` + `kvm_exit`）→ `parse_event_head` 用整个 head 调 `monitor_find`；仅当 `monitor_find` 失败且 head 中含 `:` 才回退到 tracepoint `sys:name` 解析
- profiler options 中的空格 → 保留 `filter[-1] = ' '` 恢复分隔符再喂 `parse_string_options`

---

## 参考

- 现行相关代码：
  - `src/tep.c:573-644` — `next_sep` / `rm_quotes` / `tp_kprobe_uprobe`
  - `src/tep.c:749-940` — `tp_list_new` 主体
  - `src/tep.c:1395-1425` — perf_event_attr 中 kprobe/uprobe 字段配置
  - `src/tep.h:32-53` — `struct tp` 字段
  - `src/tep.h:196-199` — probe id 枚举
  - `src/monitor.c:514-549` — `-e/--event` help 字符串
  - `src/monitor.c:955-978` — `parse_string_options`
  - `src/trace_helpers.c:1726-1738` — `syms__file_offset()`
- bpftrace probe 语法参考：https://github.com/bpftrace/bpftrace/blob/master/docs/reference_guide.md
