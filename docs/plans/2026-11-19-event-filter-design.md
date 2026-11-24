# 事件过滤器：替换、按取值范围构建，task-state / rundelay 共用

Status: Done — 第 3 版 implemented in commits d72e8ce/9f289fd（修复）、3af6e07（框架）、
aba0aad/6dbe4d5/56eb161/8782e8b（源码）/9f9076a（测试）/218c5bc（文档，含 docs/event_filter.md），即当前 main 与分支 `ef-v3`。
第 1 版在本地分支 `ef-v1`（4c7f88c..4b5f9a3），第 2 版在 `ef-v2`（d5fdb56..f196845）。
4–7 节是第 3 版：event_filter / pid_filter 内聚（结构体不可见、按生命周期的接口、自带定时器、替换与
ftrace_filter 开关走回调）。

## 1 问题

`task-state` 已经实现了"宽过滤器 + 丢弃计数反馈 + 慢速收紧"跟踪线程（见
`2026-11-17-task-state-pid-filter-replace-design.md`），但全部写在 task-state 内部：角色、
过滤器构建、开放末尾、pid 回绕、evsel 替换、重复记录去重、重建定时器混在一起。

`rundelay`（multi-trace 派生）遇到的问题与改造前的 task-state 完全一样：

- `--ptrace` / workload（`ctx->dynamic_threads`）：sched 事件不设 pid 过滤器，全系统的
  sched_switch / sched_wakeup 进用户态，由 `rundelay_untracked()` 在 `sample()` 里丢弃。
- 静态 `-p`：`tp_filter_new(thread_map)` 设一次 `pid==a||...`，线程多时超过一页失败。

本稿把 task-state 的机制拆成可复用的层次，rundelay 接入同一套。

## 2 分层

| 层 | 位置 | 职责 | 不管 |
|---|---|---|---|
| 过滤器替换 | `src/monitor.c` | 用新过滤器替换一个 evsel：不丢、不重 | 过滤器内容 |
| `event_filter` | `src/filter/event_filter.c` | 一个字段的取值集合 → 不超过一页的过滤器字符串；是否精确、是否覆盖某值 | evsel、prof_dev、raw、时间 |
| `pid_filter` | 同上，派生自 `event_filter` | 跟踪集合同步到内核：开放末尾、pid 回绕、最新 pid；何时同步（增删的延迟、否决计数、限速、重试），自带定时器 | evsel、prof_dev、raw |
| 使用者 | 各 profiler | 报告线程增删、从 raw 取 pid 调 match；`ops->set` 里 apply / `prof_dev_replace_filter()`；`ops->ftrace_filter` 里开关 `dev->ftrace_filter` | 过滤器内部、何时同步 |

原则：

1. **内核过滤器只做粗筛**（取值集合的超集），`ftrace_filter` 回调做精确过滤。正确性不依赖内核过滤器。
2. **过滤器只产出字符串。** `event_filter` / `pid_filter` 不持有 evsel、不调用 `prof_dev_*`、不读 raw：
   evsel 的替换在 `ops->set` 回调里、字段值的提取在 profiler 里，结果（回调返回值、match）反馈给过滤器。
3. **取值集合由过滤器持有、可共享**：`value_set` 带引用计数，多个过滤器（sched_switch 的 prev / next、
   sched_wakeup 的 pid）共用一个集合，不再由 profiler 另持 `tid_set`。
4. **定时策略按值的类型分**：增删线程的延迟（1s / 10s / 60s）、丢弃计数是 pid 特有的，放在 `pid_filter`；
   `event_filter` 没有时间。每个 `pid_filter` 自带一个定时器，自己决定何时同步，profiler 不管定时。
5. **去重归框架**：只有框架知道替换窗口、新旧 evsel、记录来自哪个 ring buffer。

## 3 框架：过滤器替换与去重（已实现，3af6e07）

### 3.1 接口

```c
/*
 * 用 @filter 替换 *@evsel：同一 attr、cpus、threads，继承 tp 链接与 cookie，member_cache 重建。
 * 成功返回 0，*@evsel 为新 evsel，旧的已收割并删除；失败返回负 errno，*@evsel 原样保留。
 * -EAGAIN：order 正在处理事件，需要在事件处理之外重试。-EBUSY：已有替换在进行。
 * -EINVAL：记录没有 sample id。
 */
int prof_dev_replace_filter(struct prof_dev *dev, struct perf_evsel **evsel, const char *filter);
```

`prof_dev_replace_filter()` 只是包装：拷贝 attr、priv（tp、cookie），打开、设过滤器，交给
`prof_dev_replace_evsel()`。去重与检测全部在 `prof_dev_replace_evsel()` / `prof_dev_replace_dup()`。
统计在 `dev->replace_stat`（replaced / dups / kept）。

`struct prof_evsel` 的 `long cookie`：profiler 的私有标记（task-state 的角色、rundelay 的 follow 下标），
替换时拷贝。profiler 保存的记录不能再按 sample id 反查 evsel（evsel 可能已被替换删除）。

### 3.2 去重规则

`prof_dev_replace_evsel()`：

1. **先排空**：`perf_evlist__for_each_mmap()` 读完所有 ring（不区分 evsel 对应哪些 ring）。替换窗口开始时
   旧 evsel 的记录所剩无几，`prof_dev_replace_dup()` 的执行次数少。
2. 加入并 enable 新 evsel，再停止、收割旧 evsel。重叠窗口内同一次触发两个 evsel 都写，产生重复记录。

**内核写入顺序**：tracepoint 的 perf 事件由 `perf_trace_add()` 用 `hlist_add_head_rcu()` 挂到每 cpu 的
链表头，`perf_tp_event()` 从链表头依次写——后加入的新 evsel 先写。syscalls、kprobe、uprobe 的 perf 路径
共用这个链表。线程绑定的事件在 sched-in 时挂链，新 evsel 后 sched-in，同样在链表头。

**规则**：替换窗口内，按 ring（`(cpu, tid)` 绑定）记录"是否已见过新 evsel 的记录"；见过之后，旧 evsel
的记录全部丢弃；新 evsel 的记录永不丢弃。不需要字段、不需要 raw。

| 旧 evsel 的记录 | 新 evsel 是否覆盖该值 | 处理 | 理由 |
|---|---|---|---|
| 该 ring 首条新记录之前 | 任意 | 保留 | 新 evsel 尚未在该 ring 生效，或只有旧的覆盖该值，无重复 |
| 该 ring 首条新记录之后 | 覆盖 | 丢弃 | 重复，新记录在它之前 |
| 该 ring 首条新记录之后 | 不覆盖 | 丢弃 | 过滤器已切换，该值不再需要 |

#### 3.2.1 各内核的写入顺序（`/root/linux`：v3.10 / v4.19 / v5.4 / v6.1 / v6.6 / v7.3-rc1）

`perf_tp_event()` 有两条投递路径：

1. **每 cpu 链表**：`perf_trace_add()` 一直是 `hlist_add_head_rcu()`，遍历一直是
   `hlist_for_each_entry_rcu()`：**新的先写**，各版本未变。
2. **目标任务**（`task && task != current`，只有 `TP_ARGS(__perf_task(p))` 的 tracepoint：
   sched_waking / sched_wakeup / sched_wakeup_new / sched_stat_*）：投递给目标任务 context 里的 evsel。
   - v3.13 – v6.1：遍历 `ctx->event_list`，头插：**新的先写**。v5.4 起只投递 `event->cpu == 本 cpu` 的。
   - 571f97f7d51f ("perf: Optimize perf_tp_event()")，v6.2 起：`perf_tp_event_target_task()` 用
     `perf_event_groups_for_cpu_pmu()` 按 `{cpu, pmu, cgroup, group_index}` 升序遍历，且只取本 cpu
     的 evsel：**旧的先写**。
   - v3.10 没有这条路径。

| evsel 绑定 | 目标任务路径 | 写入顺序 |
|---|---|---|
| 只绑 cpu `(c, -1)` | 不在任务 context 里 | 新的先写 |
| 只绑线程 `(-1, t)` | v5.4 起不投递（cpu = -1）；之前头插 | 新的先写 |
| `(c, t)` | v6.2 起按 group 顺序 | 旧的可能先写 |

例外只有 **v6.2+、`(c, t)` 绑定的 tracepoint evsel**：每次替换、每个 ring 至多放过一条旧重复
（旧的写第一条，新的写第二条，这条新记录让它前面的旧重复通过）。task-state / rundelay 的 sched evsel
只绑 cpu，不受影响。

#### 3.2.2 规则不成立的检测

只在 `(c, t)` 绑定的 tracepoint evsel 上检测（替换时由 `perf_evsel__cpus()` / `perf_evsel__threads()`
定下 `r->check`）。其余 evsel 不算 hash：只绑 cpu 的 sched_switch 上 raw 相同的连续事件很常见（同一线程
反复睡眠），检测会误报；它们的顺序由 3.2.1 保证。

检测取最简单的方式：该 ring 首条新记录与它之前最后一条旧记录 raw 相同（大小 + FNV-1a），即认为是
旧的先写、保留了一条重复，在 `prof_dev_replace_dup()` 里计数，收割后打印错误日志：

```
<time> <prof>: replace <sys:name>: the old evsel wrote first on ring cpu X tid Y, N duplicates kept
```

- 允许误报：raw 相同的两次事件，前一次发生在新 evsel enable 之前、只有旧的写。先排空 ring 把这段时间缩到
  很短。
- 不比较时间：虚拟机里两次写入之间可能退出到 Host，时间差不可控。
- 没有 `PERF_SAMPLE_RAW` 的 evsel 不检测。

强制 `check` 的对比（5.4、`task-state --ptrace -i 1000 -- task-threads pingpong 6` + fork 负载，
只绑 cpu）：正常顺序 33 次替换报 12 次（误报），对调 old/new 30 次报 9 次；按 `(c, t)` 限制后正常运行
21 次替换报 0 次。

### 3.3 验证实验（2026-11-19，5.4.241，16 cpu）

**独立程序**（`syscalls:sys_enter_getppid`）：参考事件 R 全程 enable，old/new 反复替换，工作线程
不停调用 `getppid`；每个 R 结束一组记录，按组检查。无 lost 的运行：

| 模式 | 替换次数 | 组数 | 重复组 | 新在前 | 旧在前 | 规则后每组恰好保留一条 |
|---|---|---|---|---|---|---|
| cpu（16 ring） | 500 | 1093870 | 142009 | 142009 | 0 | 是 |
| thread（4 ring） | 500 | 579616 | 72537 | 72537 | 0 | 是 |
| cpu，12 线程 | 300 | 1623211 | 172066 | 172066 | 0 | 是 |
| thread，12 线程 | 300 | 1538963 | 157846 | 157846 | 0 | 是 |

ring 溢出（lost）时组被破坏，异常只出现在有 lost 的运行中。

**task-state 实测**（临时插桩，规则代替 raw 去重；fork 负载 + `--ptrace`）：规则丢弃的每条旧记录，
都能在同一 ring 里找到一条未被配对、raw 相同的新记录：

| 命令 | 替换 | 规则丢弃 | 找到配对 | 未配对 |
|---|---|---|---|---|
| `pingpong 6` | 21 | 60 / 59 | 60 / 59 | 0 |
| `pingpong 8 2` | 12 | 26 / 22 | 26 / 22 | 0 |
| `-S pingpong 8` | 18 | 60 / 46 | 60 / 46 | 0 |

同时对比了现有 raw 去重：raw 方法有时丢的是新记录（前一条旧记录恰好与下一次触发的 raw 相同），
还有疑似把真实事件当重复丢掉的情况；"同角色上一条"在中断插入时也会漏判。

## 4 `event_filter`

一个字段的取值集合，转换成不超过一页的过滤器字符串。不持有 evsel、prof_dev，不读 raw。

`event_filter` 提供事件过滤器，内部包括：

- `value_set`：值集合（4.1）。
- `value_ranges`：过滤器放进来的取值范围，**解决过滤器超过 4k 的问题**（4.2）。tracepoint 过滤器最长一页
  （`perf_event_set_filter()` 只拷贝一页），逐个值写成 `pid==a||pid==b||...`，几百个值就超了，设置失败。
  连续值合成一段，超过一页时按缝隙从小到大合并，直到放得下——**范围合并在这里解决**。合并后的过滤器是取值
  集合的超集：内核过滤掉绝大部分记录，多放进来的一小部分由调用者在用户态按 `event_filter_has()` 精确过滤
  （`exact` 为 false 时）。

### 4.1 `value_set`（仅 `event_filter.c` 内部）

```c
struct value_set {
    struct rblist values;   // s64 / u64，按 is_signed 比较
    bool is_signed;
    int refcnt;             // 共享它的 event_filter 数
    u64 dels;               // 删除次数，供各 event_filter 判断 exact
};
```

- 8 字节数值，有符号或无符号（`is_signed` 决定比较与打印）。
- 增、删、遍历（按值升序）、查询。
- 引用计数：`event_filter_init()` 新建或引用，`event_filter_exit()` 释放。

### 4.2 `value_ranges`（仅 `event_filter.c` 内部）

```c
struct value_ranges {
    int nr;
    struct value_range {
        s64 lo, hi;         // 按 is_signed 解释
        bool open;          // field>=lo，不设上界
    } r[];
};
```

由 `value_set` 生成：

- 连续值合并为一段：`field==a`、`(field>=a&&field<=b)`。
- 提供 `next_value`（原 tail，下一个可能出现的值）时，加一段 `field>=next_value`：
  - `next_value` 在所有段之上：开区间，`open = true`。
  - `next_value` 在段的中间或之间（pid 回绕）：截止到下一段的起点前，与相邻、重叠的段合并；之上的段原样保留。
- 超过预算（一页减去 `and_filter` 的长度）时按缝隙从小到大合并，二分找最小可用缝隙。
- 空集合：`field<0`（无符号用 `field<0` 在内核恒假，同样适用）。
- `value_set` 不变，`value_ranges` 就不变；集合增删后 `value_ranges` 不跟随，直到下次构建。

### 4.3 结构与可见性

`struct event_filter`、`struct pid_filter`、`value_set`、`value_ranges` 的定义都在 `event_filter.c` 内，
`filter.h` 只有前向声明和函数。profiler 持有指针，读不到任何内部字段（pending、filter、dropped、tail 等），
越界在编译期就会报错。`PID_FILTER_*` 常量也是 `event_filter.c` 内部的。

`event_filter.c` 不出现 evsel、prof_dev、raw、tep；`perf_thread_map` 只出现在 `pid_filter_add_threads()`（5.4）：

- 值从哪里来（ptrace、workload）是调用者的事，`-p/-t` 的 thread_map 由 5.4 的函数直接加入；
- 过滤器字符串交给谁（`perf_evsel__apply_filter()`、`prof_dev_replace_filter()`、`tp->filter`）通过回调交出去；
- 事件的字段值由调用者从 raw 取出再传进来。

### 4.4 `event_filter` 接口（静态：构建一次）

```c
struct event_filter *event_filter_new(const char *field, const char *and_filter, bool is_signed);
void event_filter_free(struct event_filter *ef);
int  event_filter_add(struct event_filter *ef, s64 value);     // 1 加入，0 已有，<0 错误
int  event_filter_del(struct event_filter *ef, s64 value);     // 1 删除，0 不在
bool event_filter_has(struct event_filter *ef, s64 value);
/* 按当前集合生成过滤器（无末尾），返回的字符串归 @ef，下次 build 或 free 前有效；NULL：内存不足。 */
const char *event_filter_build(struct event_filter *ef);
bool event_filter_exact(struct event_filter *ef);              // 构建后：没有合并缝隙、没有删除
```

用于替换各处一次性的过滤器生成（`tp_filter_new()` 的 pid 部分、oncpu 的 prio）：过滤器保证不超过一页，
`exact` 为 false 时调用者在用户态用 `event_filter_has()` 精确过滤。动态替换只有 `pid_filter` 需要，
等出现第二种需要动态替换的值时，再把 5 节的回调机制下沉到 `event_filter`。

## 5 `pid_filter`

派生自 `event_filter`（`event_filter.c` 内部嵌入，调用者看不到），值是 pid。

`pid_filter` 不只是一个过滤器字符串，它是**跟踪的线程集合 + 内核过滤器的副本 + 两者之间的同步**。

`pid_filter` **解决延迟设置过滤器的问题，并决策与内核同步的时机**。

`--ptrace`、workload 下线程不断创建、退出，而 fd 的过滤器不能修改（SET_FILTER 返回 EEXIST），
每次变化都要替换 evsel、在每个 cpu 上打开新的 fd；每个线程替换一次代价太大。线程很多时超过一页，缝隙合并，
内核过滤器的范围变大，本来也不精确。所以**延迟设置过滤器，并让它放进来的比跟踪集合多**：

- 开放末尾 `pid>=x`：新线程拿到的是随后分配的 pid，末尾打开后它们不需要替换就能放进来；
- 退出的线程暂时留在过滤器里，等退出平息后一起收紧；
- 内核过滤掉绝大部分，用户态 `pid_filter_match()` 过滤剩下的一小部分；否决的数量就是内核多放进来的量，
  太多时收紧。

新旧 evsel 的衔接不归 `pid_filter`：替换期间新旧 evsel 都在写，事件重复由框架的 `prof_dev_replace_dup()` 解决，
在每个 ring 上见到新 evsel 的第一条记录后丢弃旧 evsel 的记录（3.2）。`set` 一返回成功，生效的就只有新过滤器，内核集合直接换成新的范围，
不需要保留新旧的并集，也不需要在替换期间强制打开 ftrace_filter。

### 5.0 两个集合与同步

| 集合 | 内部表示 | 含义 | 何时变化 | 谁查询 |
|---|---|---|---|---|
| 跟踪集合 | `value_set` | 现在跟踪的线程 | `add` / `del` 立即变化 | 采样：`pid_filter_match()`；`add_thread` / `del_thread` 查重：`pid_filter_follows()` |
| 内核集合 | `value_ranges` | 内核过滤器现在放进来的 pid（合并的缝隙、开放末尾） | 只在 set 回调成功后变化 | 内部：`add` 判断是否同步、收紧时重算 |

- 内核集合是内核里那个过滤器的副本：只有 `set` 回调成功（内核真的换上了新过滤器），副本才更新。
  因此"内核过滤器能否放进 pid"只需查副本，不碰内核。替换窗口内旧 evsel 的重复记录由 `prof_dev_replace_dup()`
  丢弃，不会遗留到副本里。
- 同步的方向只有一个：跟踪集合 → 内核。`add` 时内核集合不含新 pid 就立即同步；其余的差异（内核集合多出来的：
  退出的线程、缝隙、末尾）延迟同步，即收紧。
- 是否需要用户态进一步过滤：内核集合多于跟踪集合。为真时采样必须经过 `pid_filter_match()`（ftrace_filter）。
  只通过 `ops->ftrace_filter` 的变化通知交出，不提供查询（5.2）。
- 查内核集合不对外：调用者只关心"这条事件要不要"（跟踪集合）；"内核要不要换"由 `add` 自己决定。
  对外的两个查询按集合命名，不再用含糊的 `has` / `covers`：

```c
bool pid_filter_follows(struct pid_filter *pf, pid_t pid);           // 跟踪集合
int  pid_filter_match(struct pid_filter *pf, pid_t pid);             // 跟踪集合，计数否决
```

  内部对应 `value_set_has()` 与 `value_ranges_has()`，`event_filter.c` 里也按"跟踪 / 内核"两词注释。

### 5.1 生命周期

```
pid_filter_new()          分配；记下 field、and_filter、共享的集合、ops 回调；初始化定时器
    │
pid_filter_add_threads()  初始状态：只往跟踪集合里加初始值，不生成、不同步
pid_filter_add() × N
    │
pid_filter_start()        用初始值生成过滤器（精确、无末尾），ops->set(initial) 设置到内核，进入工作状态
    │
    ├─ pid_filter_add()   新线程：内核集合已含 → 只推迟末尾收窄；不含 → 末尾从它打开，立即同步
    │                     （ptrace 让线程停着，不丢事件）
    ├─ pid_filter_del()   线程退出：从跟踪集合删除，需要用户态过滤，延迟收紧
    ├─ pid_filter_match() 采样：pid 在跟踪集合中则接受，否则否决；否决即是内核真实放进来的多余值的反馈
    └─ （内部定时器）     到期时收紧：ops->set 替换
    │
pid_filter_free()         销毁定时器
```

**何时同步内核由 `pid_filter` 自己决定。** 外部只做三件事：告诉它跟踪集合的变化（add / del），
用 `match` 反馈内核真正放进来的值，按 `ops` 回调执行替换、开关 ftrace_filter。到期时间、限速、末尾、
重试都在内部，不再对外暴露 `due` / `tighten` / `now`。

**每个 `pid_filter` 一个定时器**（`src/timer.h`，timerfd，在主循环里触发）：

- 触发在事件处理之外：`prof_dev_replace_filter()` 不会因为 order 正在处理事件返回 -EAGAIN。
- `match` 在事件处理之中：否决数到 DROPS 时只 `timer_start()`（timerfd_settime），不替换。
- `add` 在 ptrace 的处理里（主循环）：未覆盖时直接同步，与第一版相同。
- 一个 profiler 至多 4 个 timerfd，开销可以忽略；每个过滤器独立到期、独立限速，与第一版"按角色分别到期"等价，
  profiler 不再需要 `refilter_schedule()` / 定时器回调 / `refilter_at`。
- 关闭：`pid_filter_free()` 先销毁定时器；关闭过程中（`dev->inclose`）到期的，`set` 回调返回错误即可，按失败处理。

### 5.2 回调

```c
struct pid_filter_ops {
    /*
     * 把 @filter 设置到内核。@initial：pid_filter_start() 的初次设置（perf_evsel__apply_filter()，
     * 或写进 tp->filter）；否则替换（prof_dev_replace_filter()）。0 成功，<0 失败（旧过滤器仍在工作）。
     */
    int  (*set)(struct pid_filter *pf, void *opaque, const char *filter, bool initial);
    /*
     * 是否需要用户态进一步过滤变了，@need 为新值。只在变化时调用（边沿触发）：
     * 初始为 false（start 之前内核里没有过滤器、evsel 未启用），start 后过滤器已合并缝隙则调用 true；
     * 之后在同步成功、del 后按需调用。free 时不调用。
     */
    void (*ftrace_filter)(struct pid_filter *pf, void *opaque, bool need);
};
```

构建、调用 `set`、按结果更新内核集合都在内部完成，不再有 `pending` / `commit`：

- 生成的字符串与内核里的相同：不调用 `set`，视为成功；
- `set` 成功：内核集合更新为新的范围（只是新过滤器的，旧 evsel 的重复记录由 `prof_dev_replace_dup()` 丢弃），末尾、`dropped`、到期时间
  随之更新，需要用户态过滤与否变了则调用 `ftrace_filter`；
- `set` 失败：丢弃新字符串，内核集合、`uncovered` 不变，一个 INTERVAL 后再到期。

### 5.3 接口

```c
struct pid_filter *pid_filter_new(const char *field, const char *and_filter, struct pid_filter *share,
                                  const struct pid_filter_ops *ops, void *opaque);
void pid_filter_free(struct pid_filter *pf);

/*
 * 加入跟踪集合。初始状态只加入；工作状态下内核集合不含 @pid 时打开末尾并立即同步。
 * 1 新加入，0 已在跟踪集合中，<0 错误（同步失败不算错误：保持 uncovered，一个 INTERVAL 后重试）。
 */
int  pid_filter_add(struct pid_filter *pf, pid_t pid);
/* 从跟踪集合删除：1 删除，0 不在。 */
int  pid_filter_del(struct pid_filter *pf, pid_t pid);
/* 生成初始过滤器并 ops->set(initial)；失败返回 <0，仍在初始状态。 */
int  pid_filter_start(struct pid_filter *pf);

/* 采样到事件，@pid 由调用者从 raw 取出：1 接受（在跟踪集合中）；0 否决，计数，可能让收紧到期。 */
int  pid_filter_match(struct pid_filter *pf, pid_t pid);
/* 是否在跟踪集合中，不计数：add_thread / del_thread 查重。 */
bool pid_filter_follows(struct pid_filter *pf, pid_t pid);
const char *pid_filter_string(struct pid_filter *pf);     // 内核里的过滤器，打印用
```

- 多个过滤器共同决定 `dev->ftrace_filter`（有一个需要就打开，全都不需要才关闭）：调用者维护一个计数，
  `ops->ftrace_filter` 的 `need` 为真加一、为假减一，`dev->ftrace_filter = 计数 > 0`（rundelay 再或上
  静态的 `tp->ftrace_filter`）。回调是边沿触发、初始为 false，计数从 0 开始就始终正确，不需要回头查询各个
  过滤器，所以不提供 `pid_filter_need_ftrace_filter()`。
- 共享集合：`pid_filter_add()` 对共享同一集合的每个过滤器都要调用（第一个返回 1，其余返回 0），
  各自判断自己的内核集合是否已含；`del` 同理。
- 收紧的时机（不变）：否决数到 DROPS、新线程后 1s（末尾收窄或关闭）、退出后 10s（首次退出起最多 60s），
  两次替换至少隔 1s；未覆盖的新线程在 `add` 里立即同步，不受限速。
- 末尾：`add` 未覆盖时从该 pid 打开；收紧时 `add_due` 已过则关闭，否决过多则前移到系统最新 pid + 1，
  否则保持；回绕后截止到下一个已跟踪的 pid。

### 5.4 初始值

```c
/* @threads 的线程加入跟踪集合，跳过 cgroup 和 -1；在 pid_filter_start() 之前调用。 */
int pid_filter_add_threads(struct pid_filter *pf, struct perf_thread_map *threads);
```

与 `pid_filter` 的接口放在一起（`event_filter.c`，声明在 `filter.h`），取代 task-state、rundelay 各自的
`*_follow_threads()`。只有它看得到 `perf_thread_map`。

## 6 profiler 的使用

### 6.1 task-state

- `struct task_state_role { struct perf_evsel *evsel; char *filter; struct pid_filter *pf; }`。
- `task_state_filter()`：ROLE_SWITCH `pid_filter_new()` + `pid_filter_add_threads()`，其余角色共享它；
  逐个 `pid_filter_start()`。
- `set` 回调：`initial` 时 `perf_evsel__apply_filter(r->evsel, filter)`；否则 `dev->inclose` 时返回错误，
  再 `prof_dev_replace_filter(dev, &r->evsel, filter)`，计数 replaced / replace_failed、`-v` 打印。
- `ftrace_filter` 回调：`ctx->nr_ftrace_filter` 按 `need` 加一 / 减一，`dev->ftrace_filter = nr > 0`，变化时 `-v` 打印 on/off。
- `ftrace_filter`：按角色从 raw 取 pid，`pid_filter_match()`，否决时 `stat.dropped++`。
- `add_thread` / `del_thread`：ROLE_SWITCH 的 `pid_filter_follows()` 查重，再对各角色 `add` / `del`。
- 删除：`refilter` 定时器、`refilter_at`、`refilter_inited`、`task_state_refilter*()`、`task_state_follow_threads()`。

### 6.2 rundelay

- `struct rundelay_follow { struct tp *tp; int offset; struct pid_filter *pf; }`。
- `set` 回调：`initial` 时只 `tp_update_filter(tp, filter)`，由 `multi_trace_filter()` 统一 apply；
  否则 `prof_dev_replace_filter(dev, &tp->evsel, filter)` 并 `tp_update_filter()`。
- `ftrace_filter` 回调：同 task-state 计数，再或上 init 时算好的"有 tp 设了 `tp->ftrace_filter`"。
- 其余与 task-state 相同；`rundelay_untracked()` 用 follows[0] 的 `pid_filter_follows()`。
- 删除：`refilter` 定时器及其字段、`rundelay_refilter*()`、`rundelay_follow_threads()`。

两边仍有相似的部分（回调里的计数与打印、线程增删的循环），量已经很小，`thread_follow` 暂不做。

## 7 提交拆分（第二版）

d5fdb56 之后都未推送，按同样的粒度改写，测试、文档提交不变（`-v` 输出不变）：

| # | 改写的提交 | 内容 |
|---|---|---|
| 2 | d5fdb56 filter | 结构体移进 `event_filter.c`；4.4、5.3 的接口，去掉 pending / commit；`pid_filter_add_threads()`（5.4） |
| 3 | 41c5bd3 task-state | 6.1；删掉 `task_state_follow_threads()` |
| 4 | b178f45 rundelay | 6.2；删掉 `rundelay_follow_threads()` |
| 5 | bca56d0 删除无用代码 | 不变 |
| 6 | 87e89ae 测试 | 不变 |
| 7 | f196845 文档 | 不变 |

每步 `make`，第 3、4 步后跑对应测试。

## 8 已确认

1. rundelay `--ptrace` 下不必要唤醒判断降到 level 1，可以接受。
2. 过滤器文本统一为 `(and_filter) && (ranges)`；`and_filter` 是增强的过滤逻辑，与取值范围是逻辑与关系。
3. 写入顺序：本机 5.4 实测，其余内核按源码确认（3.2.1）。
4. 规则不成立的检测只针对 `(c, t)` 绑定，最简单的 hash 比较，允许误报，不比较时间（3.2.2）。
5. 替换前先排空所有 ring，不区分 evsel 对应的 ring。
6. 框架层直接提交（并入 3af6e07）；event_filter 先改 plan，task-state、multi-trace、测试、文档暂不动。
7. 提交粒度：event_filter.c、task-state.c、multi-trace.c、删除无用代码、测试、文档各自单独提交（7 节）。
8. 第二版：结构体对 profiler 不可见；`event_filter.c` 不见 `perf_thread_map`，初始值由外部函数加入；
   生命周期 new → add 初始值 → start → add/del/match/tighten；替换通过回调，去掉 commit。
9. `pid_filter_add_threads()` 放在 `event_filter.c`，与 pid_filter 接口一起。跟踪集合（value_set，采样匹配）与
   内核集合（value_ranges，内核过滤器的副本，add 判断是否同步）分开命名；pid_filter 的含义包括把跟踪集合同步到内核（5.0）。
10. 是否需要用户态进一步过滤只通过边沿触发的 `ops->ftrace_filter(need)` 交出，调用者计数，不提供查询函数。同步内核的时机由 pid_filter 自己决定：外部只报 add / del、
    用 match 反馈内核真实放进来的值；每个 pid_filter 一个定时器；回调 `ops->set(initial)` / `ops->ftrace_filter`（5.1、5.2）。

## 9 实现与本稿的差异

1. `value_ranges` 存序键（`u64`）而不是 `s64`：有符号值翻转符号位后按无符号比较，区间、缝隙都在序键上算，
   打印时再转回原值。
2. （第 2 版）`event_filter` 多了 `pending_exact` / `pending_dels`，`pid_filter` 多了 `pending_open` / `pending_tail`：
   构建与 commit 之间分开保存，commit 成功才生效。第 3 版去掉 pending：`event_filter_make()` 生成的范围与字符串
   只在 `ops->set` 成功后由 `event_filter_set()` 换上，失败即释放。
3. （第 2 版）`pid_filter_match()` 是 `filter.h` 里的 inline；第 3 版结构体不可见，改为 `event_filter.c` 里的函数。
4. 末尾的处理：`add_due` 已过则关闭；`add_due` 未过且 `dropped >= DROPS`（不是因新线程未覆盖而重建）时前移到
   系统最新 pid + 1（嵌套 pid namespace 用 `last` + 1）；否则保持。
5. 从 raw 取 pid 用 `tp_struct.h` 的固定布局（task-state 的 `sample()` 本来就这样读），不用 tep 解析 offset；
   rundelay 在 `rundelay_follow()` 里传入 `offsetof()`。
6. 测试提交（ba8f177 → 87e89ae）内容不变，新实现下 task-state + rundelay + multi-trace 56 passed、1 skipped。
7. 文档提交修正了一句：旧事件先写的检测只针对 `(c, t)` 绑定、不比较时间。
8. 第一版的问题（第二版修正）：没有考虑初始值如何加入、初始过滤器如何取得；调用者直接读 `pf.ef.pending`、
   `pf.ef.filter`、`pf.dropped`；`pid_filter_commit()` 语义不清；`event_filter.c` 之外两份 `*_follow_threads()`。
9. 第 3 版：`dev->ftrace_filter` 在 init 里先置 1（start 之前 evsel 未启用，不影响），全部 `pid_filter_start()`
   之后按计数更新一次：初始过滤器都精确时打印 `ftrace_filter off`，与第 2 版的 `-v` 输出一致，测试不用改。
10. 第 3 版：回调的 `opaque` 是 `dev`，task-state / rundelay 按 `pf` 指针找到角色 / follow（至多 4 个）。
11. 第 3 版：`ops->set` 非初次失败时计 `replace_failed`，第一次失败打印
    `replace <sys:name> filter failed, retry later`，取代第 2 版 add_thread 里的 `misses thread`；`dev->inclose`
    时直接返回 -EBUSY，不计数、不打印（关闭过程中到期的收紧不算失败，测试检查 `replace_failed == 0`）。
12. 第 3 版：`pid_filter_del()` 只在内核集合仍含该 pid 时推迟收紧（共享集合的第二个过滤器删除返回 0，
    但它的内核集合仍含该 pid，同样推迟）。
13. 第 3 版：rundelay 的静态 `tp->ftrace_filter` 在每次更新时扫描各 tp，而不是 init 时算好：
    `multi_trace_filter()` 之后才知道哪些过滤器退回了用户态，最后再更新一次。
14. 第 3 版测试：task-state 26 passed（第 3 步提交上）；task-state + rundelay + multi-trace 56 passed、1 skipped
    （最终树）。`event_filter.c` 另用桩定时器的单元测试在 valgrind 下检查了末尾打开 / 关闭、退出延迟与推迟、
    set 失败重试、否决过多、合并缝隙与 ftrace_filter 的边沿通知，无泄漏。
