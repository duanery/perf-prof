# obj__find_name 重名符号冲突处理

**Status:** Done — implemented in commits 88f7630 (src) / 6d355bd (tests) / d75e04b (docs).
**Date:** 2026-07-02
**Files touched:** `src/trace_helpers.[ch]`, `src/tep.c`, `tests/`, `docs/`

## 背景

`obj__find_name`（`src/trace_helpers.c:1301`）线性扫描 `obj->syms[]`，
命中第一个 `strcmp` 就返回，静默忽略后续同名符号。当前有两个调用点：

1. **`syms__convert()`** — 支撑 `perf-prof --symbols <bin>`（pprof 风格：
   stdin 一行符号名，stdout 一行 `0x<file offset>`）。
2. **`syms__file_offset()`** — `src/tep.c:837` 处解析
   `-e uprobe:/lib64/libc.so.6:func` 用它拿 uprobe 的 `probe_offset`。
   命错了直接 uprobe 到错的函数。

真正会打错点的重名场景：
- 不同 TU 里的 `static helper` 恰好重名。
- glibc 多版本符号：`pthread_cond_wait` 同时存在
  `@GLIBC_2.2.5`（`start=0x8b280`）与 `@@GLIBC_2.3.2`（`start=0x8f0a0`）。
  当前加载器只调 `elf_strptr()` 拿基础名（`src/trace_helpers.c:921`），
  版本后缀在 `.gnu.version*` 段里，没被拼进 `sym->name`——所以
  `syms[]` 里两个版本都是同名字符串。

无害重名（weak alias，如 `strdup` / `__strdup` 指向同一 `start`）不该报错。

## 需求

- **冲突判定**：仅当同名且 `start` 不同时算冲突（weak alias 不算）。
- **uprobe 冲突**：报错并中止，不落回"当数字 offset 解析"分支。
- **`--symbols` 冲突**：stdout 保留首命中（向后兼容 pprof 协议），
  stderr 打 warning。
- **消歧手段**：报错列出候选偏移，用户改用 `binary:0x<offset>`
  精确定位。
- **候选列表**：按 offset 展示，每行一个 offset，名字来自
  `obj__find_name_all` 返回的严格同名匹配指针，加上通过在 `obj->syms`
  中前后邻位扫描相同 `start` 得到的跨名 alias（如
  `__pthread_cond_wait`），逗号连接（`0x<off>  sym1, sym2, ...`）。

## 设计

### 接口变化

**新增（`src/trace_helpers.c` 内部私有）：**

```c
/*
 * Return the number of syms in obj whose name matches `name`.
 * If out != NULL and count > 0, malloc a `struct sym *` pointer array
 * of that length and store it in *out; each element points into
 * obj->syms (do NOT free the pointed-to syms; caller frees only *out).
 * If out == NULL, only the count is returned (no allocation).
 * Returns -1 on load failure.
 *
 * Because obj->syms is sorted by start (trace_helpers.c:1156), the
 * emitted pointers are also sorted by start; caller can dedup adjacent
 * entries with the same start to collapse weak alias, and can walk
 * (*out)[i] backward/forward through obj->syms to gather cross-name
 * alias sharing that start (via pointer arithmetic against obj->syms).
 */
static int obj__find_name_all(struct object *obj, const char *name,
                              struct sym ***out);

/*
 * Format a multi-line candidate description into `buf`, consuming
 * `matches` (a pointer array of `n` syms sorted by start, e.g. the
 * output of obj__find_name_all).
 * Layout (one line per distinct start; per-line names are the input
 * name plus any cross-name alias found by walking obj->syms outward
 * from `matches[i]` while start remains equal, joined with ", "):
 *   0x<off>  sym1[, sym2, ...]
 * offset uses the same convention as syms__file_offset()'s return value
 * and --symbols stdout: sym.start - (obj->sh_addr - obj->sh_offset).
 */
static void obj__format_candidates(struct object *obj,
                                   const char *name,
                                   struct sym **matches, int n,
                                   char *buf, size_t buflen);
```

**调用者判定唯一/冲突（在 `syms__file_offset` 与 `syms__convert` 内部）：**

```c
struct sym **matches = NULL;
int n = obj__find_name_all(obj, name, &matches);
if (n <= 0) {
    /* 未命中（或加载失败）*/
} else {
    /* 相邻去重（matches 已按 start 排序）判断唯一/冲突。
       ngroups == 1 -> 唯一，用 matches[0]->start。
       ngroups >= 2 -> 冲突，把 matches 交给 obj__format_candidates；
                       它可通过 (matches[i] - obj->syms) 定位 obj->syms
                       内的下标，再向前/向后扫描相同 start 的相邻项
                       收集跨名 alias。 */
    free(matches);
}
```

**修改（`src/trace_helpers.h` 公开 API）：**

```c
/* Old */
unsigned long syms__file_offset(const char *binpath, const char *func);

/* New */
unsigned long syms__file_offset(const char *binpath, const char *func,
                                char **err_msg);
```

调用者语义：
| 返回值 | `*err_msg` | 含义 |
|--------|-----------|------|
| `> 0`  | `NULL`    | 唯一命中，返回 file offset |
| `0`    | `NULL`    | 未命中；调用者可继续把 `func` 当数字 offset 解析（保留旧行为） |
| `0`    | 非 NULL   | 冲突；调用者需 `fputs(*err_msg, stderr); free(*err_msg);` 后失败退出 |

### 调用点改造

**`src/tep.c:837`（uprobe 解析）：**

```c
char *err_msg = NULL;
unsigned long sym_off = syms__file_offset(resolved_path, target, &err_msg);
if (err_msg) {
    fputs(err_msg, stderr);
    free(err_msg);
    return -1;
}
if (sym_off) {
    tp->probe_offset = sym_off + off;
} else if (plus) {
    fprintf(stderr, "uprobe symbol '%s' not found in %s\n", ...);
    return -1;
} else {
    /* 保留旧行为：把 target 当数字 offset 解析 */
    ...
}
```

**`syms__convert()` 内部：**

调用 `obj__find_name_all()`，冲突时保留首命中输出到 stdout（不变），
额外向 stderr 打 warning，格式见下节。

### 报错文案

**uprobe 冲突（stderr）：**

```
uprobe: symbol 'pthread_cond_wait' has 2 candidates in /lib64/libc.so.6:
  0x8b280  pthread_cond_wait, __pthread_cond_wait
  0x8f0a0  pthread_cond_wait, pthread_cond_wait@GLIBC_2.2.5
Try one of:
  -e uprobe:/lib64/libc.so.6:0x8b280
  -e uprobe:/lib64/libc.so.6:0x8f0a0
```

**`--symbols` 冲突（stderr；stdout 仍写 `0x<first_offset>\n`）：**

```
warning: symbol 'pthread_cond_wait' has 2 candidates in /lib64/libc.so.6:
  0x8b280  pthread_cond_wait, __pthread_cond_wait
  0x8f0a0  pthread_cond_wait, pthread_cond_wait@GLIBC_2.2.5
using 0x8b280 (first match); pass '0x<offset>' explicitly to disambiguate.
```

### offset 计算

`syms__file_offset()` 现有实现：`sym.start - (obj->sh_addr - obj->sh_offset)`。
报错里显示的 `0x<offset>` 与 `--symbols` stdout 语义一致，用户可直接
拷回 `binary:0x<off>` 使用。

### 别名收集

在候选组内，扫描 `obj->syms[]` 收集所有 `syms[i].start == group.start` 的
`syms[i].name`（**不限**用户输入的名字，让 `__pthread_cond_wait` 这类
alias 一并出现）。列表按名字排序。若一个组的 alias 数量超过 8，尾部
截断为 `..., N more`。`syms[]` 已按 `start` qsort（`src/trace_helpers.c:1156`），
同 offset 的项相邻，扫描 O(n)。

## 实现拆分（按 CLAUDE.md 提交约定）

**Commit 1（src）：**
- `src/trace_helpers.c`：新增 `obj__find_name_all` 与
  `obj__format_candidates`，删掉旧 `obj__find_name`；改 `syms__file_offset`
  与 `syms__convert`。
- `src/trace_helpers.h`：更新 `syms__file_offset` 声明。
- `src/tep.c`：更新 uprobe 解析调用点。

**Commit 2（tests）：** 新增 `tests/test_symbols_conflict.py`：
1. **正向 uprobe 唯一命中**：挑一个 libc 里唯一的符号（fixture 动态选），
   `-e uprobe:/lib64/libc.so.6:<uniq>` 能启动。
2. **反向 uprobe 冲突报错**：`nm -D /lib64/libc.so.6` fixture 里挑
   出现 ≥ 2 次且 offset 不同的名字；跑 `perf-prof trace -e uprobe:...`
   验证 stderr 出现 `has N candidates` 与 `Try one of:`。
3. **`--symbols` 冲突**：把冲突名字 stdin 喂给 `perf-prof --symbols`，
   验证 stdout 首行 `0x<hex>`，stderr 含
   `warning: symbol '<name>' has N candidates`。
4. **消歧后偏移可用**：把上一条报错第一行的 `0x<offset>` 拿去
   `-e uprobe:/lib64/libc.so.6:0x<offset>`，能启动。
5. 若 fixture 探测不到冲突符号（罕见发行版）→ pytest.skip，不 fail。

规范：顶部 `from PerfProf import PerfProf; from conftest import result_check`，
正向用例走 `for std, line in prof.run(...): result_check(...)`，反证同样
走 `PerfProf.run()` 迭代扫 stderr。**不用** `subprocess.run`。

**Commit 3（docs）：**
- `docs/main_options.md`：`--symbols` 段加"多义符号处理"。
- `docs/tool_usage.md`：uprobe 例子补一句消歧说明。
- `README.md` / `README_CN.md`：uprobe 例子同步。
- `docs/profilers/trace.md`：uprobe 段落追加消歧示例。
- `skills/perf-prof/SKILL.md` 与 `skills/perf-prof/references/profilers/*.md`
  里凡涉及 uprobe 语法的 profiler 文档。

## 兼容性与风险

- `syms__file_offset` 只在 `src/tep.c` 一处使用（已 grep 确认），
  签名变更影响面可控。
- `--symbols` 的 stdout 协议不变，pprof 消费者脚本不会坏；只是
  stderr 多了 warning。
- 唯一命中路径的开销：仅比旧实现多"看到第二个不同 offset 立刻
  short-circuit"，量级 O(n) 无变化。
- 不引入 `.gnu.version*` 段解析，`struct sym` 布局不动。

## 未来延伸（不在本次范围）

- 若后续需要按版本名精确指定（如 `pthread_cond_wait@@GLIBC_2.3.2`），
  可在加载器里读 `.gnu.version*` 拼进 `sym->name`；报错文案已经把
  版本后缀写在 alias 列表里，从当前设计过渡到那种方案是加法而非
  破坏。