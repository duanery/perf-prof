# event_filter：按取值集合构建、延迟同步的内核事件过滤器

本文说明 `src/filter/event_filter.c` 的动机与原理：`event_filter` 把一个字段的取值集合转换成不超过一页的
tracepoint 过滤器，`pid_filter` 在此之上跟踪不断创建、退出的线程，决定何时把过滤器同步到内核。
task-state、rundelay 用它跟踪 `-p`、`-t`、`--ptrace`、workload 的线程。

面向的读者是要**让新的分析器按线程（或其他取值集合）过滤 tracepoint**、或要**改动这套机制**的人。
trace event 过滤器的语法见 [Event_filtering.md](Event_filtering.md)。

## 1 动机

task-state、rundelay 关心的是 `sched:sched_switch`、`sched:sched_wakeup` 等调度事件。这些事件不适合绑定到线程
（一个线程的 sched_wakeup 由唤醒它的线程在别的 cpu 上触发），只能绑定 cpu，再用过滤器按 pid 字段选出关心的线程，
例如 `prev_pid==100||prev_pid==101`。这样做会遇到三个问题：

1. **过滤器最长一页。** 内核只拷贝一页过滤器字符串（`perf_event_set_filter()`）。逐个值写成
   `pid==a||pid==b||...`，几百个线程就超过 4k，设置失败。
2. **过滤器不能修改。** 一个 fd 设置过过滤器后再设置返回 `EEXIST`。线程增删时只能打开带新过滤器的 evsel
   替换旧的——每次替换要在每个 cpu 上打开一个 fd。
3. **线程变化很快。** `--ptrace`、workload 下线程不断创建、退出，每个线程替换一次 evsel 代价太大；
   新线程从创建到第一次被调度只有很短的时间，过滤器来不及就会丢事件。

没有内核过滤器时，全系统的调度事件都进用户态再丢弃，开销随系统负载增长。

## 2 思路

**内核过滤器只做粗筛，用户态做精确过滤。** 内核过滤器是关心的取值集合的超集，内核过滤掉绝大部分记录，
多放进来的一小部分在用户态（profiler 的 `ftrace_filter` 回调）按集合精确丢弃。正确性只依赖用户态的精确过滤，
不依赖内核过滤器。

既然允许超集，三个问题都可以放宽：

- 超过一页：把相邻的值合成一段，再合并最小的缝隙，直到放得下（`event_filter`）。
- 不能修改、变化快：过滤器**延迟设置**，并且有意放宽——一个开放末尾 `pid>=x` 放进将来的新线程，
  退出的线程暂时留在过滤器里，攒到一起再收紧（`pid_filter`）。
- 替换期间新旧 evsel 同时写：重复记录由框架去掉（`prof_dev_replace_dup()`），过滤器不用管。

| 层 | 位置 | 解决的问题 |
|---|---|---|
| `value_set` | `event_filter.c` 内部 | 值集合，8 字节有符号或无符号，多个过滤器共享 |
| `value_ranges` | `event_filter.c` 内部 | 过滤器超过 4k：连续值成段、合并最小缝隙 |
| `event_filter` | `event_filter.c` | 一个字段上的事件过滤器 `(and_filter) && (ranges)`，只产出字符串 |
| `pid_filter` | `event_filter.c` | 延迟设置过滤器，决定何时与内核同步，自带定时器 |
| 过滤器替换 | `src/monitor.c` | `prof_dev_replace_filter()` 替换 evsel，`prof_dev_replace_dup()` 去掉重复记录 |
| 使用者 | task-state、rundelay | 报告线程增删，从 raw 取出 pid 交给 `pid_filter_match()`，在回调里设置过滤器 |

`event_filter` / `pid_filter` 不持有 evsel、prof_dev，不读 raw：结构体只在 `event_filter.c` 内定义，
`filter.h` 只有前向声明和函数。过滤器字符串通过回调交出去，事件的字段值由调用者取出再传进来。

## 3 `event_filter`：超过 4k 的过滤器

### 3.1 取值范围

`value_set` 是一棵按值排序的红黑树。`value_ranges` 由它生成：

1. 连续值合成一段：`pid==100`、`(pid>=100&&pid<=103)`。
2. 字符串超过预算（一页减去 `and_filter` 的长度）时，合并段之间的缝隙。合并后的长度随缝隙阈值单调不增，
   二分找出**能放下的最小缝隙阈值**，把不超过它的缝隙全部合并。合并的总是最小的缝隙，多放进来的值尽量少。
3. 没有值时生成恒假的 `pid<0`。

例：pid 100–103、200、205、1000，缝隙分别是 96、4、794。预算只够三段时合并最小的缝隙 4：

```
(pid>=100&&pid<=103)||(pid>=200&&pid<=205)||pid==1000
```

201–204 被多放进来，用户态丢弃。

有符号值翻转符号位后按无符号比较（序键），段和缝隙都在序键上计算，打印时再转回原值，
所以负数、`LLONG_MIN` 等边界也能正确成段。

### 3.2 精确与否

`event_filter_exact()`：构建出的过滤器只放进集合里的值——没有合并缝隙、构建之后没有删除过值。
为 false 时调用者必须在用户态用 `event_filter_has()` 精确过滤。删除计数放在共享的 `value_set` 里，
共享集合的每个过滤器都能看到删除，不需要相互通知。

### 3.3 `and_filter`

最终的过滤器是 `(and_filter) && (ranges)`。`and_filter` 是额外的条件（task-state 的 `prev_state` 状态、
rundelay 的 `prev_state==0`），与取值范围是逻辑与，长度从预算里扣除。

### 3.4 接口

```c
struct event_filter *event_filter_new(const char *field, const char *and_filter, bool is_signed);
void event_filter_free(struct event_filter *ef);
int  event_filter_add(struct event_filter *ef, s64 value);     // 1 加入，0 已有，<0 错误
int  event_filter_del(struct event_filter *ef, s64 value);     // 1 删除，0 不在
bool event_filter_has(struct event_filter *ef, s64 value);
const char *event_filter_build(struct event_filter *ef);       // 字符串归 ef，下次 build 前有效
bool event_filter_exact(struct event_filter *ef);
```

静态使用：加完值后 build 一次，设置到 evsel；`exact` 为 false 时在 `ftrace_filter` 回调里用 `has` 精确过滤。

## 4 `pid_filter`：延迟设置，决定何时同步

### 4.1 两个集合

| 集合 | 内部表示 | 含义 | 何时变化 | 用途 |
|---|---|---|---|---|
| 跟踪集合 | `value_set` | 现在跟踪的线程 | `add` / `del` 立即变化 | `pid_filter_match()`、`pid_filter_follows()` |
| 内核集合 | `value_ranges` | 内核过滤器现在放进来的 pid | `ops->set` 成功后才变化 | 内部：`add` 判断是否要立即同步 |

内核集合是内核里那个过滤器的副本，只有回调真的把新过滤器设进内核后才更新。同步只有一个方向：
跟踪集合 → 内核。内核集合比跟踪集合多（合并的缝隙、开放末尾、退出的线程）时，需要用户态精确过滤。

### 4.2 开放末尾

新线程的 pid 是随后分配的。一个线程不在内核集合里时，从它的 pid 打开末尾 `pid>=x` 并立即同步，
之后创建的线程不需要再替换 evsel 就能放进来：

```
prev_pid==100||prev_pid==200                     初始
prev_pid==100||prev_pid==200||prev_pid>=300      300 创建：打开末尾，替换一次
                                                 301、302…… 已被末尾放进来，不替换
prev_pid==100||prev_pid==200||(prev_pid>=300&&prev_pid<=302)
                                                 1 秒没有新线程：末尾关闭
```

- **pid 回绕**：新 pid 小于已跟踪的 pid 时，末尾截止到下一个已跟踪 pid 之前，之上的段原样保留。
- **末尾前移**：末尾放进来太多不相干的线程（否决数达到 1000），收紧时把末尾移到系统最新分配的 pid + 1
  （`/proc/loadavg` 第 5 列）。perf-prof 运行在嵌套 pid namespace 中时读到的 pid 与 tracepoint 不一致，
  改用最后加入的 pid + 1。
- ptrace 让新线程停着，直到 `add` 返回、新过滤器已经生效，不丢这个线程的事件。

### 4.3 同步的时机

| 情况 | 何时同步 |
|---|---|
| 新线程不在内核集合中 | `add` 里立即同步，不受限速 |
| 否决数达到 1000（DROPS） | 立即（受限速） |
| 新线程后 1 秒（ADD_DELAY）没有新线程 | 关闭末尾；每个新线程推迟一次 |
| 线程退出后 10 秒（DEL_DELAY） | 去掉退出的线程；每次退出推迟一次，首次退出起最多 60 秒（DEL_MAX） |
| 两次同步 | 至少隔 1 秒（INTERVAL）：每次替换都在每个 cpu 上打开 fd |
| `set` 失败 | 内核集合不变，1 秒后重试 |

退出的线程不立即收紧：pid 短时间内不会被复用，退出往往成批发生，攒到一起替换一次。
期间这些 pid 的记录由 `pid_filter_match()` 丢弃。

每个 `pid_filter` 一个 timerfd 定时器，在主循环里触发，处于事件处理之外，`prof_dev_replace_filter()`
不会因为正在处理事件而返回 `-EAGAIN`。`pid_filter_match()` 在事件处理之中，否决数到 DROPS 时只启动定时器。
生成的过滤器与内核里的相同时不调用回调。

### 4.4 回调

```c
struct pid_filter_ops {
    int  (*set)(struct pid_filter *pf, void *opaque, const char *filter, bool initial);
    void (*ftrace_filter)(struct pid_filter *pf, void *opaque, bool need);
};
```

- `set`：`initial` 时 evsel 还没启用，直接设置过滤器（`perf_evsel__apply_filter()`，或写进 `tp->filter`）；
  否则用 `prof_dev_replace_filter()` 替换 evsel。返回 0 后生效的只有新过滤器，失败时旧的继续工作。
- `ftrace_filter`：内核集合是否多于跟踪集合（是否需要用户态精确过滤）变化时调用，边沿触发，初始为 false。
  一个 profiler 有多个过滤器（sched_switch 的 prev / next、sched_wakeup），共同决定 `dev->ftrace_filter`：
  调用者计数，`need` 为真加一、为假减一，`dev->ftrace_filter = 计数 > 0`。

### 4.5 生命周期与接口

```
pid_filter_new()                 记下字段、and_filter、共享的集合、回调；初始化定时器
pid_filter_add_threads() / add() 初始线程，只加入跟踪集合
pid_filter_start()               生成精确的初始过滤器，ops->set(initial)
pid_filter_add() / del()         线程增删
pid_filter_match()               采样到的记录
pid_filter_free()                销毁定时器
```

```c
struct pid_filter *pid_filter_new(const char *field, const char *and_filter, struct pid_filter *share,
                                  const struct pid_filter_ops *ops, void *opaque);
void pid_filter_free(struct pid_filter *pf);
int  pid_filter_add_threads(struct pid_filter *pf, struct perf_thread_map *threads);
int  pid_filter_add(struct pid_filter *pf, pid_t pid);    // 1 新加入，0 已跟踪；同步失败不算错误
int  pid_filter_del(struct pid_filter *pf, pid_t pid);    // 1 删除，0 不在
int  pid_filter_start(struct pid_filter *pf);
int  pid_filter_match(struct pid_filter *pf, pid_t pid);  // 1 接受，0 否决并计数
bool pid_filter_follows(struct pid_filter *pf, pid_t pid);
const char *pid_filter_string(struct pid_filter *pf);
```

`share` 不为 NULL 时共享它的跟踪集合：`add` / `del` 对共享的每个过滤器都要调用（第一个返回 1，其余返回 0），
各自判断自己的内核集合。

## 5 替换时的重复记录

替换 evsel 时先启用新的，再停止、读完、关闭旧的，不丢事件；但两者同时启用的短暂时间内，同一次触发会写两条
记录。这由框架的 `prof_dev_replace_dup()` 解决，`pid_filter` 不需要保留新旧过滤器的并集：

- tracepoint 的 perf 事件挂在每 cpu 链表上，新 evsel 用 `hlist_add_head_rcu()` 挂在链表头，触发时从链表头
  依次写入：**同一个 ring buffer 里新 evsel 总是先写**。
- 替换期间按 ring buffer 记录是否已见过新 evsel 的记录；见过之后，旧 evsel 在这个 ring buffer 里的记录全部丢弃，
  新 evsel 的记录全部保留。不需要读字段、比较 raw。
- 例外：v6.2+ 内核上同时绑定 cpu 和线程的 tracepoint evsel，旧的可能先写（571f97f7d51f
  ("perf: Optimize perf_tp_event()")）。框架只对这类 evsel 检测（新 evsel 第一条记录与它之前的旧记录 raw 相同），
  并打印 `replace sys:name: the old evsel wrote first on ring cpu X tid Y, N duplicates kept`。
  task-state、rundelay 的 sched 事件只绑 cpu，不受影响。

替换前先读完所有 ring buffer，替换窗口开始时旧记录所剩无几。统计在 `dev->replace_stat`，
`-v` 退出时打印的 `twins` 就是丢弃的重复记录数。

## 6 使用示例

task-state（`src/task-state.c`）的 4 个角色各一个 `pid_filter`，共享 sched_switch prev 侧的跟踪集合：

```c
static const struct pid_filter_ops task_state_pf_ops = {
    .set = task_state_set_filter,               // initial: apply；否则 prof_dev_replace_filter()
    .ftrace_filter = task_state_need_ftrace_filter, // 计数，开关 dev->ftrace_filter
};

// filter 回调
r->pf = pid_filter_new(role_pid[role], states,
                       role == ROLE_SWITCH ? NULL : ctx->role[ROLE_SWITCH].pf,
                       &task_state_pf_ops, dev);
if (role == ROLE_SWITCH)
    err = pid_filter_add_threads(r->pf, ctx->thread_map);
if (err == 0)
    err = pid_filter_start(r->pf);

// add_thread / del_thread 回调
if (pid_filter_follows(ctx->role[ROLE_SWITCH].pf, pid))
    return 0;
for (role = 0; role < ROLE_MAX; role++)
    pid_filter_add(ctx->role[role].pf, pid);

// ftrace_filter 回调：按角色从 raw 取出 pid
if (!pid_filter_match(r->pf, pid))
    return 0;
```

`-v` 下可以看到过滤器的变化：

```
sched:sched_switch filter "(prev_state==1) && (prev_pid==4321||prev_pid==4322)"
task-state: ftrace_filter off
task-state: add thread 4400
task-state: replace sched:sched_switch filter "(prev_state==1) && (prev_pid==4321||prev_pid==4322||prev_pid>=4400)"
task-state: ftrace_filter on
...
task-state: replaced 6 replace_failed 0 dropped 1520 twins 37
```

rundelay（`src/multi-trace.c`）相同，只是初始过滤器写进 `tp->filter`、由 `multi_trace_filter()` 统一设置，
替换后同步更新 `tp->evsel` 与 `tp->filter`。
