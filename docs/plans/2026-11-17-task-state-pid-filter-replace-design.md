# task-state 跟踪进程：换 evsel 更新内核 pid 过滤器，用户态精确过滤

Status: Done — implemented in commits 30c46a8 309bb79 c4d3fb1 b8f5d53 (src) / 4b23bf3 (tests) / 05c0dc8 (docs)

实现与本稿的差异：去重的"上一条"按 cpu × 角色各存一份（mode 1 的 sched_switch 有 prev、next 两个角色，同一 cpu 上会插在一对重复记录之间）；另修了 order drain 被其他 ring 的 lost 等待打断的问题（309bb79）。末尾按角色各自开放、关闭：收紧时若上次生成后没有新线程，就关闭末尾。

## 1 问题

`task-state -p <pid>` 把 sched 事件转为绑定 cpu（`dev->cpus = all`、`dev->threads = dummy`），
线程集合放在 `ctx->tids`。

- `--ptrace` / workload（`ctx->dynamic_threads`）：tracepoint 的 `SET_FILTER` 每个 fd 只能设一次
  （`ftrace_profile_set_filter()` 返回 `-EEXIST`），线程增删后无法更新，于是**不设 pid 过滤器**，
  全系统的 sched_switch / sched_wakeup 都进用户态，由 `task_state_accept()` 丢弃。
- 不跟踪线程变化时设一次 `prev_pid==a||...` 过滤器。内核 `perf_event_set_filter()` 用
  `strndup_user(arg, PAGE_SIZE)` 取过滤串，线程多（Java 进程常见几百上千）时超长失败。
- eBPF（tid 放进 map）不可行：tracepoint 的 BPF 程序挂在 `trace_event_call->prog_array` 上，由
  `perf_trace_run_bpf_submit()` 在所有 perf event 之前执行，返回 0 时整条记录丢弃——**全系统所有**
  采集该 tracepoint 的 perf event（包括其他 perf-prof 命令）都会被过滤。

另外，`profiler.accept` 回调只有 task-state 使用，它与已有的 `ftrace_filter` 回调做的是同一件事
（用户态过滤，作用于 sample 与转发两条路径），多一个回调没有必要。

## 2 原则

1. **内核过滤器只做粗筛（tid 集合的超集），用户态 `ftrace_filter` 回调做精确过滤。**
   正确性不依赖内核过滤器，换过滤器只为减少进入用户态的事件。
2. **内核过滤器由有序的 tid 生成**：连续 tid 合并成区间；超出长度预算时合并最小的缝隙。**默认不开放
   末尾**，过滤器只覆盖已知的 tid；第一次新增线程时转换为开放末尾——最后接一个不封顶的区间 `f>=tail`，
   后来新建的线程 tid 递增，直接落在覆盖范围内，不再每个线程替换一次。开放之后保持开放。
3. **换过滤器 = 换 evsel，每个 evsel 独立替换**：新 evsel 设好过滤器后先加入并 enable，再删除旧 evsel
   （停止 → 收割 → 完成）。不留空窗；重叠窗口内的重复记录在用户态精确去重。各 evsel 分别替换，一个
   成功、另一个失败是允许的状态：失败的保留旧 evsel 与旧过滤器，之后重试。
4. **能不换就不换**：
   - 新增线程：该角色的过滤器已覆盖 tid 就不换；否则（第一次新增、pid 回绕、落在缝隙里）立即换该
     evsel，此时新线程停在 ptrace 初始 stop，换完才继续运行。
   - 线程退出：不换。过滤器里多一个 tid 只是多收事件，tid 复用由用户态过滤挡住。
   - **按阈值批量收紧**：内核放过、用户态丢弃的事件（已退出的 tid、缝隙里的别的任务、开放末尾放进来的
     后来新建的任务）按角色计数，超过阈值时在安全点批量替换这些角色的过滤器（第 7 节）。退出了、不再
     产生事件的 tid 不花任何代价，所以不需要按线程退出来触发。

## 3 去掉 `.accept`，改用 `ftrace_filter`

`perf_event_process_record()` 在三处做用户态过滤：开头的 `prof->accept`、`perf_event_forward()` 第 1
步、sample 路径里的 `prof->ftrace_filter`（`dev->ftrace_filter` 置位时）。后两处已经覆盖 sample 与
转发，`accept` 去掉：

- `src/monitor.h`：删除 `accept` 成员及注释。
- `src/monitor.c`：删除 `perf_event_process_record()` 开头的 accept 判断。
- `src/task-state.c`：`task_state_accept()` 改名 `task_state_ftrace_filter()`，返回 `long`（>0 保留，
  <=0 丢弃）；`.accept` 改为 `.ftrace_filter`；`task_state_init()` 在用户态过滤生效时置
  `dev->ftrace_filter = 1`（第一步：`ctx->dynamic_threads`；第 6 节之后：`ctx->thread_map` 非空）。

行为差异：accept 在 `perfeval_sample()` 和 `exit_n` 计数之前丢弃；ftrace_filter 在其后，`exit_n` 由
`__break` 回退计数，与其他用 ftrace_filter 的分析器一致。perfeval 会多统计被丢弃的事件，可接受。

角色判断简化为每个 evsel 只看自己的字段（第 6 节之后内核过滤器与之一致）：

| 角色 | 用户态判断 |
|---|---|
| sched_switch（prev） | `prev_pid` 在 tid_set |
| sched_switch_next | `next_pid` 在 tid_set |
| sched_wakeup / sched_wakeup_new | `pid` 在 tid_set |

现有 `!ctx->sched_switch_next && next tracked` 分支只在 mode 3 出现，而 mode 3 的 `sample()` 不处理
next 侧，删掉不影响结果。`task_state_sample()` 里的 `task_state_tracked()` 判断随之去掉：有
thread_map 时 ftrace_filter 已过滤，没有时它恒为真。

## 4 libperf：不需要新接口

现有接口已满足"先开后停"：

1. `perf_evsel__new(perf_evsel__attr(old))`，`perf_evsel__open(new, perf_evlist__cpus(), NULL)`，
   `attr.disabled = 1` 打开；`perf_evsel__apply_filter(new, filter)`（fd 上第一次设置）。
2. `perf_evlist__add_evsel(evlist, new)`：fd `SET_OUTPUT` 接入已有 (c,-1) ring，`evlist->enabled`
   时立即 enable。
3. `perf_evlist__del_evsel(evlist, old, &reap)`：DISABLE、停止；收割期间旧 sample id 仍能解析。
4. 调用方把 ring 读到末尾，`perf_evlist__reap_done()`，`perf_evsel__close()` / `perf_evsel__delete()`。

add 失败时旧 evsel 不受影响；del 只在 old 是组 leader 时 `-EBUSY`，task-state 的 evsel 不分组。
`docs/libperf_ring_lifetime.md` 第 7 节补一个"替换 evsel"的示例即可。

perf-prof 侧在 `src/monitor.c` 加一个辅助函数（`src/monitor.h` 声明）：

```c
/*
 * Replace @old by @new, opened on the cpus of the evlist with its filter
 * set: @new is enabled before @old is stopped, so no event is missed; the
 * events written by both meanwhile are delivered twice. @old is reaped
 * (its ring buffers read to the end) and closed before the return.
 */
int prof_dev_replace_evsel(struct prof_dev *dev, struct perf_evsel *old, struct perf_evsel *new);
```

- 内部：`perf_evlist__add_evsel(new)` → `perf_evlist__del_evsel(old, &reap)` → `prof_dev_reap()` →
  `perf_evsel__close(old)` / `perf_evsel__delete(old)`。
- 一次只换一个 evsel，各 evsel 互不依赖。失败时 `@new` 已从 evlist 摘下，由调用方关闭；`@old` 不受
  影响，仍在工作。
- order 正在处理事件（`order.inprocess`）时 `order_drain()` 不读 ring，收割读不到末尾、旧 id 会失效：
  返回 `-EAGAIN`，调用方推迟。task-state 的调用点（ptrace 回调、自己的 timer）都不在事件处理中。

## 5 重复记录的识别

重叠窗口（新 fd enable 到旧 fd disable）内，同时满足新旧过滤器的命中写两条记录：

- `perf_tp_event()` 在同一 cpu 的 hlist 上依次输出，两条记录在同一个 (c,-1) ring 里**紧挨着**：
  sched_switch 在 `__schedule()` 关中断下，sched_wakeup / sched_wakeup_new 持 `pi_lock` 关中断，
  其间不会插入别的 sched 记录。
- raw 相同；sample id 不同（新旧 evsel）；time 不同（每个 event 在 `__perf_event_header__init_id()`
  各读一次 `perf_event_clock()`，相差几十 ns）。
- order 按时间合并各 ring，同一 cpu 内的先后不变。

规则（只在替换期间启用）：按 `data->cpu_entry.cpu` 记住上一条 sched 记录 `{evsel, role, raw}`。

> 当前记录与上一条 role 相同、evsel 不同、raw 相同 → 丢弃当前记录，并清空上一条；
> 否则当前记录成为上一条。

- 清空保证一对只丢一条：同一 cpu 连续两次相同命中 `new1 old1 new2 old2` 丢 old1、old2。
- 不依赖新旧谁先写。窗口开始前的单条 `old0` 后接 `new1 old1` 时，丢的是 new1、留 old1——两条 raw
  相同，结果只差几十 ns 的时间戳。
- 误配只可能发生在同一 cpu 上连续出现 raw 完全相同的真实命中：sched_switch 不可能（切到 next 后下一次
  的 prev 就是 next）；sched_wakeup 可能（runnable 任务被重复唤醒），但 task-state 对 RUNNING 任务的
  wakeup 是幂等的（不重置 `task->time`、`state & task_report` 为 0 不计数），多一条少一条结果不变。

不去重的后果（作为测试的反证依据）：被抢占（prev_state 为 R）的 sched_switch 重复一次会多记一个几十
ns 的 RUNNING、next 侧多记一个 RUNDELAY，calls 偏大、min 被拉低。

实现：

- evsel 的角色存进 `perf_evsel__set_priv(evsel, (void *)(long)role, NULL)`，新旧 evsel 同角色；
  `task_state_sample()` 与 ftrace_filter 用 `perf_evsel__priv()` 判断角色，不再与
  `ctx->sched_switch` 等指针比较——收割期间旧 evsel 仍在产生记录，新 evsel 也已在写同一个 ring。
  `ctx->sched_switch` 等指针仍指向当前一代，供创建新 evsel、`print_dev` 使用。
- 去重状态 `ctx->twin`：每 cpu 一个槽 `{evsel, role, size, raw[128]}`，按 cpu 数分配一次。raw 超过
  128 字节的记录不参与去重（sched 记录约 64～80 字节，`-g` 时只比 raw 不比 callchain）。
- 放在 `task_state_ftrace_filter()` 里，先 tid 判断还是先去重无所谓：一对记录 raw 相同，tid 判断结果
  也相同。去重先于 tid 判断，保证"上一条"是该 cpu 的真实上一条。
- `prof_dev_replace_evsel()` 前清空并置 `ctx->replacing`，返回后清除：所有重复记录都写在旧 fd disable
  之前，`prof_dev_reap()` 读到末尾时都已经过 ftrace_filter。

## 6 内核过滤器的生成

`src/filter/tp_filter.c` 已有 `pid_filter()`（连续 tid 合并成 `f>=a&&f<=b`）。新增：

```c
struct pid_ranges {
    int nr;
    struct { int lo, hi; } r[];  /* sorted; hi == INT_MAX: the open tail */
};

/*
 * @threads sorted. The ranges cover @threads and, when @tail >= 0, every pid
 * from @tail on; their filter on @field fits @budget bytes.
 */
struct pid_ranges *pid_ranges_new(struct perf_thread_map *threads, int tail,
                                  const char *field, size_t budget);
bool pid_ranges_has(const struct pid_ranges *ranges, int pid);   /* binary search */
char *pid_ranges_filter(const struct pid_ranges *ranges, const char *field);
```

1. 合并连续 tid 成区间；`tail >= 0` 时追加 `[tail, INT_MAX]`，与相邻或重叠的区间合并，输出 `f>=lo`。
2. 单点 `f==a`，区间 `(f>=a&&f<=b)`，以 `||` 连接。
3. 超出预算：缝隙 `g_i = lo_{i+1} - hi_i - 1`，对阈值 G 二分，合并所有 `g_i <= G` 的缝隙，取能放下的
   最小 G。长度随 G 单调不增；G 取最大值时只剩一个区间，总能放下。
4. 没有 tid 也没有末尾：`f<0`（不匹配任何记录；`f==0` 会放过 idle 的切换）。

预算 = `sysconf(_SC_PAGESIZE) - 1` 减去该 evsel 过滤器的其余部分（mode 3 的 `prev_state==…` 条件、
括号与 `&&`）。3.10 的过滤器支持 `>=`、`<=`、`&&`、`||`、括号；谓词数在这个长度下远低于老内核
`MAX_FILTER_PRED`。

**开放末尾的起点**：

- 第一次新增线程（转换为开放末尾）：`tail = 新线程的 tid`。之后新建的线程 tid 更大（直到 pid 回绕）；
  在它与当前之间创建的别的任务也会放进来，数量很少，由用户态丢弃。
- 按阈值重新生成（第 7.3 节）：`/proc/loadavg` 第 5 个字段是最近分配的 pid（所有内核都有；
  `ns_last_pid` 依赖 `CONFIG_CHECKPOINT_RESTORE`），`tail = last_pid + 1`，把此前创建的别的任务都挡在
  末尾之外。读取失败时退回 `tail = 最大 tid + 1`。tracepoint 里的 pid 是 init pid namespace 的，
  perf-prof 不在 init pid namespace 时 `/proc/loadavg` 不可用作起点，也走退回路径。

重新生成与新线程之间的竞争：读 `/proc/loadavg` 之前已创建、尚未经 ptrace 报告的线程，tid 不大于
`last_pid`，可能不在范围内；它的 `add_thread` 发现不覆盖，立即替换。只是多一次替换，不丢事件。

`--filter`（comm）时 `tp_filter_new()` 只用 comm、不用 pid，且不启用 ptrace，不受影响。

## 7 task-state 的改动

### 7.1 状态

`struct task_state_ctx`：

- 角色 `enum { ROLE_SWITCH, ROLE_SWITCH_NEXT, ROLE_WAKEUP, ROLE_WAKEUP_NEW, ROLE_MAX }`，每个角色一份：
  - `evsel`：当前一代（取代 `ctx->sched_switch` 等指针；保留这些名字作为访问宏或直接改用数组）；
  - `ranges`：当前内核过滤器覆盖的范围；`filter`：当前过滤串（取代 `ctx->filter_*`）；
  - `dropped`：本代过滤器下，内核放过、用户态按 tid 丢弃的记录数；
  - `uncovered`：有 tid 不在 `ranges` 内（新增线程时替换失败）；
  - `built`：本代过滤器生效的时间（单调时钟），用于限频。
- `bool tail_open`：已转换为开放末尾（第一次新增线程时置位，之后不清）。
- `ctx->twin`、`ctx->replacing`（第 5 节）。
- `struct timer refilter`：oneshot，触发批量替换；`struct prof_dev *dev` 回指，timer 回调用。
- `ctx->stat` 增加 `replaced`、`replace_failed`、`dropped`（累计），`sigusr` / `print_dev` 输出。
- `dynamic_threads` 去掉：有 thread_map 时一律"内核超集 + 用户态精确"，`task_state_tracked()` 只剩
  `tid_set__has()`。

### 7.2 阈值

```c
/* Replace the filter of a role once this many records it lets in are dropped. */
#define TASK_STATE_REFILTER_DROPS     1000
/* At most once a second per role: a replacement opens a fd on every cpu. */
#define TASK_STATE_REFILTER_INTERVAL  NSEC_PER_SEC
```

- 一次替换是"cpu 数"次 `perf_event_open` 加 `SET_OUTPUT`，限频防止抖动：缝隙里别的任务若长期活跃且
  预算不允许把它们排除，替换后很快又达阈值，限频把代价压到每秒一次。
- 新过滤串与当前相同（比如只能合并缝隙、末尾起点也没变）时不换，`dropped` 清零重新计数。
- 先用常量，有需要再做成 task-state 的选项。

### 7.3 流程

- **`task_state_filter()`**（`.filter` 回调，open 后调用一次）：`tid_set__map()` 得到有序 tid，不开放
  末尾，按每个角色的字段与预算 `pid_ranges_new()`、`pid_ranges_filter()`，
  `perf_evsel__apply_filter()`，记 `built`。
- **`task_state_ftrace_filter()`**：替换期间先去重（第 5 节）；再按角色判断 tid，不在 tid_set 时
  `dropped++`，达到 `TASK_STATE_REFILTER_DROPS` 且 timer 未启动时启动 timer，到期时间
  `max(1ms, built + TASK_STATE_REFILTER_INTERVAL - now)`（timerfd 的 0 表示停止，至少 1ms）。
  ftrace_filter 在事件处理中，不能直接替换。
- **`task_state_add_thread()`**：`tid_set__add()`；`tail_open` 未置位时置位，末尾起点取该 tid；
  对每个角色，`ranges` 覆盖该 tid 就跳过，否则 `task_state_refilter(role)`。失败的角色置 `uncovered`、启动 timer 重试，打印一次警告；返回 0——
  tid 已在 tid_set，其他角色照常，失败的角色在重试成功前丢失该线程的事件。
- **`task_state_del_thread()`**：结算节点、`tid_set__del()`，不换、不计数；它若还产生事件（不会，已
  退出），由 `dropped` 驱动。
- **timer 回调**：对 `uncovered` 或 `dropped >= 阈值` 的角色，`built` 距今不足限频间隔的跳过并重新
  定时，其余逐个 `task_state_refilter(role)`。仍有失败或跳过的角色时重新启动 timer。
- **`task_state_refilter(role)`**：用当前 tid 集合生成过滤串，`tail_open` 时带上
  末尾起点（新增线程路径：刚开放时取该 tid，已开放时保持起点，回绕的 tid 作为普通区间加入；阈值
  路径：按 `/proc/loadavg` 前移）；与当前相同则只清 `dropped`；
  否则新建 evsel（`perf_evsel__attr()` 的 attr、同角色 priv）、`perf_evsel__open(cpus)`、
  `apply_filter`、`prof_dev_replace_evsel()`。成功后更新该角色的 evsel、ranges、filter，清
  `dropped` / `uncovered`，记 `built`，`stat.replaced++`；失败关闭新 evsel，`stat.replace_failed++`，
  旧 evsel 继续工作。`-EAGAIN` 当作失败、等待重试。
- **deinit**：`timer_destroy()`，释放各角色的 ranges / filter。

`task_state_enabled()`、`print_dev`、`-v` 打印改为遍历角色数组。

## 8 提交拆分

1. **源码** `perf-prof: filter the samples of profilers by ftrace_filter only`：
   `src/monitor.h`、`src/monitor.c` 删除 `accept`；`src/task-state.c` 改用 `.ftrace_filter`。
   行为不变，可单独 review。
2. **源码** `perf-prof task-state: follow the threads by replacing the evsels`：
   `src/filter/tp_filter.c`、`src/filter/filter.h`（pid_ranges）；`src/monitor.c` / `.h`
   （`prof_dev_replace_evsel()`）；`src/task-state.c`（第 5、7 节）。
3. **测试** `tests/`：
   - `test_dynamic_bindings.py::test_task_state_forward_filter` 保持通过（验证 ftrace_filter 覆盖
     转发路径）。
   - 新增：`task-state --ptrace -v` 跑 `binding-workload`：启动时过滤器不带开放末尾；第一个 worker
     触发每个角色一次替换、转换为开放末尾；之后的 worker tid 递增落在末尾内，不再触发替换。
   - 新增：`-p` 不带 `--ptrace`，过滤器始终不开放末尾。
   - 新增：系统里另起一个持续 fork 的负载（让开放末尾放进大量无关事件），确认达到阈值后发生批量替换，
     且替换后丢弃数回落。
   - 新增：把 task-state 转发给 trace，按 cpu 检查不存在相邻、来自同一 tracepoint、内容完全相同的
     记录（去重的反证）。
   - 新增：几百个线程同时存活（`binding-workload` 加并发参数，或 `pthread` 辅助程序），`-p` 不带
     `--ptrace`，过滤串超过 4KB 时仍能启动（验证预算与缝隙合并）。
4. **文档**：`docs/profilers/task-state.md`、`skills/perf-prof/references/profilers/task-state.md`
   （内核过滤 + 用户态过滤、开放末尾与阈值）；`docs/libperf_ring_lifetime.md`（替换 evsel 示例）。

## 9 已确认

- 各 evsel 独立替换，允许部分成功：失败的角色保留旧过滤器，由 timer 重试。
- 默认不开放末尾；第一次新增线程时转换为开放末尾，之后保持。
- 用户态丢弃数按角色计数，达到阈值后（每角色每秒最多一次）批量替换过滤器；开放末尾时起点随之前移到
  当时的最新 pid。
