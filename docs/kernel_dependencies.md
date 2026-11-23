# perf-prof 使用的内核功能及其依赖

perf-prof 用内核的功能完成采样、过滤、跟踪，于是依赖了这些功能的实现：一个取值、一种顺序、一次通知、
一个版本才有的接口。本文按 perf-prof 使用的内核功能组织，每节三部分：

- **用途**：perf-prof 用这个功能做什么，哪些模块在用；
- **原理**：该功能与 perf-prof 相关的实现，是后面各条依赖的基础；
- **依赖**：perf-prof 依赖的具体实现细节（代码注释里称为 `Kernel constraint`）：内核出处、引入版本、
  依赖它的代码、不成立时的后果。

规则见 `AGENTS.md`「内核依赖：代码注释 + 登记」。

- perf-prof 支持的最旧内核是 v3.10；"v3.10 已有"表示早于 v3.10 存在，不再追溯 commit。
- commit 与版本均在内核源码上用 `git describe --contains` 核实。
- 每个依赖位置的代码里都有 `Kernel constraint:` 注释，`grep -rn "Kernel constraint" src lib` 可找到全部。
- 兼容优先运行时探测；仍比较 `KERNEL_VERSION` 的条目注明了原因。

依赖条目格式：

```
#### <一句话依赖>
- 内核：<文件> <函数>()；<12位SHA> ("<subject>"), in v<版本> ｜ v3.10 已有
- 依赖：<perf-prof 文件> <函数>()，可多处
- 做法与后果：perf-prof 依赖它做了什么；不成立时如何检测 / 退化
```

---

## 1 perf_event

**用途**：所有事件源。`lib/perf/` 封装 `perf_event_open()`、evlist/evsel、mmap ring buffer 与 poll；框架（`src/monitor.c`、`src/order.c`）在其上打开事件、读 ring buffer、按时间排序，所有 profiler 都经由它采样。

### 1.1 事件的创建与属性

**原理**：`perf_event_open()` 由 `perf_copy_attr()` 检查 `struct perf_event_attr`。内核不认识的位落在
`__reserved_1` 等保留区，直接返回 `-EINVAL`；新内核每增加一个功能就占用一位或一个字段。所以
perf-prof 对可选属性的做法都是"先带上，`-EINVAL` 时去掉再试"，或者确认旧内核上该功能本来不可用。

#### PERF_FLAG_FD_CLOEXEC
- 内核：`kernel/events/core.c` `perf_event_open()`；a21b0b354d4a ("perf: Introduce a flag to enable close-on-exec in perf_event_open()"), in v3.14
- 依赖：`lib/perf/evsel.c` `perf_evsel__open_one()`
- 做法与后果：带该 flag 打开，`-EINVAL` 时去掉重试。

#### 辅助记录的属性位逐版本增加
- 内核：`perf_copy_attr()`；`ksymbol` 76193a94522f ("perf, bpf: Introduce PERF_RECORD_KSYMBOL")、`bpf_event` 6ee52e2a3fe4 ("perf, bpf: Introduce PERF_RECORD_BPF_EVENT"), in v5.1；`cgroup` 96aaab686505 ("perf/core: Add PERF_RECORD_CGROUP event"), in v5.7；`text_poke` e17d43b93e54 ("perf: Add perf text poke event"), in v5.9
- 依赖：`src/misc.c` `misc_reinit()`
- 做法与后果：`-EINVAL` 时去掉最新的一位重开，直到不剩任何一位。

#### use_clockid
- 内核：34f439278cef ("perf: Add per event clockid support"), in v4.1
- 依赖：`src/order.c` `prof_dev_env2attr()`（`--monotonic`）
- 做法与后果：没有回退，旧内核上打开失败。

#### write_backward
- 内核：9ecda41acb97 ("perf/core: Add ::write_backward attribute to perf event"), in v4.7
- 依赖：`src/trace.c` `trace_init()`（`--overwrite`）
- 做法与后果：没有回退，旧内核上打开失败。

#### perf_kprobe / perf_uprobe PMU
- 内核：`kernel/events/core.c`；e12f03d7031a ("perf/core: Implement the 'perf_kprobe' PMU")、33ea4b24277b ("perf/core: Implement the 'perf_uprobe' PMU"), in v4.17
- 依赖：`src/tep.c` `__kprobe_uprobe()`
- 做法与后果：只用 `perf_event_open()` 就能创建探针，不写 `kprobe_events`/`uprobe_events`；PMU 类型和 attr.config 里 retprobe 的位从 sysfs 读取。读不到时类型为 0，`kprobe:`/`uprobe:` 事件被拒绝。

#### perf_kprobe / perf_uprobe 创建的探针没有 trace event 类型
- 内核：`kernel/trace/trace_kprobe.c` `create_local_trace_kprobe()`、`kernel/trace/trace_uprobe.c` `create_local_trace_uprobe()`
  不调用 `register_trace_event()`，记录里的 `common_type` 为 0；同上两个 commit，in v4.17
- 依赖：`src/tep.c` `tp_list_ftrace_filter()`
- 做法与后果：用户态过滤时按 `common_type` 找不到 tp（探针的 `tp->id` 是 `KPROBE..URETPROBE`），改按记录的 evsel 找；
  否则过滤器退到用户态的 kprobe/uprobe 记录全部被丢弃。

#### 调用栈深度的 sysctl
- 内核：`kernel/events/callchain.c`；c5dfd78eb798 ("perf core: Allow setting up max frame stack depth via sysctl")、c85b03349640 ("perf core: Separate accounting of contexts and real addresses in a stack trace"), in v4.7
- 依赖：`src/order.c` 构造函数 `init()`
- 做法与后果：读取 `kernel.perf_event_max_stack` 与 `perf_event_max_contexts_per_stack` 来估算样本大小；没有这两个 sysctl 时用 `PERF_MAX_STACK_DEPTH` 和 0。

### 1.2 ioctl

#### 一个 fd 的过滤器只能设置一次
- 内核：`kernel/trace/trace_events_filter.c` `ftrace_profile_set_filter()`：`event->filter` 已存在就返回 `-EEXIST`；v3.10 已有
- 依赖：`lib/perf/evsel.c` `perf_evsel__apply_filter_cpu()`；`src/monitor.c` `prof_dev_replace_filter()`；pid_filter 的 `ops->set()`（`src/task-state.c`、`src/multi-trace.c`）
- 做法与后果：要改过滤器只能替换 evsel，即在每个 cpu 上打开新 fd，代价大。所以 pid_filter 延迟同步，并用开放末尾减少替换次数。

#### PERF_EVENT_IOC_ID
- 内核：cf4957f17f2a ("perf: Add PERF_EVENT_IOC_ID ioctl to return event ID"), in v3.12
- 依赖：`lib/perf/evlist.c` `perf_evlist__id_add_fd()`
- 做法与后果：返回 `-ENOTTY` 时改用 `read()` 加 `PERF_FORMAT_ID` 取 id。

#### 过滤器不超过一页
- 内核：`kernel/events/core.c` `perf_event_set_filter()`：`strndup_user(arg, PAGE_SIZE)`；v3.10 已有
- 依赖：`src/filter/event_filter.c` `event_filter_make()`
- 做法与后果：取值放不进一页时，合并区间之间最小的缝隙，过滤器成为超集，多放进来的记录由 `pid_filter_match()` 在用户态丢弃。

### 1.3 ring buffer、mmap 与 poll

**原理**：每个 fd 可以有自己的 ring buffer，也可以用 `PERF_EVENT_IOC_SET_OUTPUT` 写进别的 fd 的
ring buffer。ring buffer 由 mmap 它的那个 fd 持有。任何一个写者写入后，挂在该 ring buffer 上的所有
event 都会被唤醒。任务退出时，它上下文里的 event 进入 EXIT 状态，`perf_poll()` 报告 `EPOLLHUP`。
mmap 页头 `perf_event_mmap_page` 提供用户态读计数器（rdpmc）和时间换算参数，由 `lock` 这个 seqlock 保护。
libperf 的 ring buffer 生命周期管理建立在这些行为上，详见 `docs/libperf_ring_lifetime.md`。

#### 任一写者的写入唤醒挂在该 ring buffer 上的所有 event
- 内核：`ring_buffer_wakeup()`；10c6db110d0e ("perf: Fix loss of notification with multi-event"), in v3.2
- 依赖：`lib/perf/evlist.c` poll 角色（"The registered fds serve two roles"）
- 做法与后果：每个 ring buffer 只把一个 fd 注册进 epoll。

#### 任务退出报告 EPOLLHUP
- 内核：`perf_poll()`；179033b3e064 ("perf: Add PERF_EVENT_STATE_EXIT state for events with exited task"), in v3.18
- 依赖：`lib/perf/evlist.c` poll 角色与回收（reap）；`src/monitor.c` 中的 EPOLLHUP 计数
- 做法与后果：靠 EPOLLHUP 得知线程退出并回收它的 ring buffer，被禁用的 event 也会报告。在 v3.18 之前收不到通知，ring buffer 要到关闭时才回收。

#### 没有 ring buffer 的 event 一被 poll 就报告 EPOLLHUP
- 内核：`perf_poll()`；61b67684c4a4 ("perf: Fix perf_poll to return proper POLLHUP value"), in v3.18
- 依赖：`lib/perf/evlist.c` poll 角色
- 做法与后果：只把挂着 ring buffer 的 fd 注册进 epoll。

#### 被 SET_OUTPUT 重定向的 event 不能 mmap
- 内核：`perf_mmap()`；448f97fba901 ("perf: Convert mmap() refcounts to refcount_t"), in v6.18（WARN）；d06bf78e55d5 ("perf: Fix refcount warning on event->mmap_count increment"), in v6.19（`-EBUSY`）
- 依赖：`lib/perf/mmap.c` `perf_mmap__mmap()`
- 做法与后果：只对 ring buffer 的属主 fd 做 mmap。

#### 用户态读计数器与时间换算
- 内核：`perf_event_mmap_page`；`time_zero` 与 `cap_user_time_zero` 来自 c73deb6aecda ("perf/x86: Add ability to calculate TSC from perf sample timestamps"), in v3.12；`cap_user_time_short` 来自 6c0246a4588d ("perf: Add perf_event_mmap_page::cap_user_time_short ABI"), in v5.9
- 依赖：`lib/perf/mmap.c` `perf_mmap__read_tsc_conversion()`、`perf_mmap__read_self()`
- 做法与后果：没有 `time_zero` 时返回 `-EOPNOTSUPP`，`--tsc`/`--kvmclock` 关闭。没有 `cap_user_rdpmc` 时改用 `read()` 读计数器。

---

## 2 tracepoint / ftrace

**用途**：`-e sys:name` 事件、kprobe/uprobe 事件与它们的 ftrace 过滤器。`src/tep.c` 用 libtraceevent 解析 format；task-state、rundelay、multi-trace、kvm-exit、blktrace 等按固定布局读 raw，并依赖 sched 事件的语义区分抢占与睡眠；pid_filter 通过替换 evsel 改过滤器。

**原理**：每个 tracepoint 在每个 cpu 上有一条 perf event 的 hlist。事件触发时从链表头开始，依次写入各个
event 的 ring buffer，写入前先经过各自的 ftrace 过滤器。事件的二进制布局由
`tracing/events/<sys>/<name>/format` 描述，print fmt 里还带着 `__print_flags()` 等打印逻辑。内核升级
可能合并或删除事件、增删或移动字段，所以布局变化要在运行时通过字段是否存在、raw 大小或 print fmt
来区分，不比较版本号。

### 2.1 写入顺序

#### tracepoint 先写入最新加入的 perf event
- 内核：`kernel/trace/trace_event_perf.c` `perf_trace_add()`：`hlist_add_head_rcu()`；v3.10 已有
- 依赖：`src/monitor.c` `prof_dev_replace_dup()`
- 做法与后果：替换 evsel 期间，新旧 evsel 写同一个 ring buffer。见到新 evsel 的第一条记录之后，旧 evsel 的记录要么是重复，要么只通过了旧过滤器，一律丢弃。

#### 带目标任务的 tracepoint 按 group 顺序写目标任务的 event
- 内核：`kernel/events/core.c` `perf_tp_event_target_task()`；571f97f7d51f ("perf: Optimize perf_tp_event()"), in v6.2
- 依赖：`src/monitor.c` `prof_dev_replace_dup()`（`r->check`）
- 做法与后果：sched_waking、sched_wakeup*、sched_stat_* 写目标任务（不是 current）的 event 时，旧 evsel 先写。只对同时绑定 cpu 和线程的 evsel 做 hash 检查，每个 ring buffer 最多有一条孪生记录。

### 2.2 事件布局

#### 固定布局的结构体
- 内核：各 `include/trace/events/*.h`；kprobe/uprobe 的 entry head 来自 `kernel/trace/trace.h`、`trace_uprobe.c`，保留字段名来自 `trace_probe.h`；v3.10 已有
- 依赖：`src/tp_struct.h`；`src/tep.c`
- 做法与后果：profiler 按固定偏移读 raw，不经过 tep。布局变化过的用字段或 raw 大小区分；其余的如果变了，只会表现为数据错误，排查时先看 format。

#### sched_wakeup 去掉了 success 字段
- 内核：58b9987de86c ("sched/tracing: Remove the redundant 'success' in the sched tracepoint"), in v5.14：`target_cpu` 的偏移从 36 变为 32
- 依赖：`src/tp_struct.h` `struct sched_wakeup_no_success`；`src/sched.c` `sched_init()`（`tep__event_has_field()`）；`src/tep.c` sched_wakeup 的 matcher（按 raw 大小区分）；`src/task-state.c`

#### kvm:kvm_exit 的布局（x86）
- 内核：`arch/x86/kvm/trace.h`；aa17911e3c21 ("KVM: Record instruction set in kvm_exit tracepoint"), in v2.6.38 起有 `isa`/`info1`/`info2`；e6e1e2593592 ("tracing: Remove lock_depth from event entry"), in v2.6.39 起没有 `common_lock_depth`
- 依赖：`src/kvm-exit.c` `__exit_reason()`
- 做法与后果：按 raw 大小区分。

#### kvmmmu 的 mmu_valid_gen 改为 u8
- 内核：`arch/x86/kvm/mmutrace.h`；ca333add6933 ("KVM: x86/mmu: Explicitly track only a single invalid mmu generation"), in v5.4，之前是 unsigned long
- 依赖：`src/kvmmmu.c`（`tep__event_field_size()`）

### 2.3 sched 事件的语义

task-state、rundelay、multi-trace 的 matcher 用 `prev_state` 区分抢占与睡眠，watchdog 用 sched_switch 观察 softlockup 线程。

`sched:sched_switch` 的 `prev_state` 由 `__trace_sched_switch_state()` 生成。被抢占的任务
（`preempt == true`）报告一个专门的"抢占值"，其余任务报告状态位；print fmt 用
`REC->prev_state & <抢占位> ? "+" : ""` 打印 "R+"。状态位的取值和抢占值在 v4.2–v4.15 之间改了多次。

#### 被抢占时 prev_state 的取值
- 内核：`include/trace/events/sched.h` `__trace_sched_switch_state()`：
  | 内核 | 抢占值 |
  |---|---|
  | ≤ v4.1 | `TASK_RUNNING \| TASK_STATE_MAX` = 1024 |
  | v4.2，80ed87c8a9ca ("sched/wait: Introduce TASK_NOLOAD and TASK_IDLE") | 2048 |
  | v4.8，7dc603c9028e ("sched/fair: Fix PELT integrity for new tasks") | 4096 |
  | v4.14，efb40f588b43 ("sched/tracing: Fix trace_sched_switch task-state printing") | `TASK_STATE_MAX` = 0x1000 |
  | v4.15，3f5fe9fef5b2 ("sched/debug: Fix task state recording/printout") | `TASK_REPORT_MAX` = 0x100 |
- 依赖：`src/tep.c` `tep__sched_switch_preempt_state()`；`src/task-state.c`（`report_max`）；`src/multi-trace.c` rundelay 的 `prev_state==X||prev_state==0` 过滤器；`src/tep.c` sched_switch 的 matcher（`[preempt]`）
- 做法与后果：从 print fmt 里算出 "+" 对应的位，不比较版本号。取值错了，抢占会被当成睡眠：rundelay 漏掉抢占事件，task-state 把它计入 S/D。

#### 状态位与退出状态
- 内核：1593baab910d ("sched/debug: Implement consistent task-state printing")、06eb61844d84 ("sched/debug: Add explicit TASK_IDLE printing"), in v4.14
- 依赖：`src/task-state.c` `task_state_init()`（`state_dead`、`TASK_REPORT_IDLE`）
- 做法与后果：在 v4.13 及以前，`prev_state` 就是 `p->state`，退出中的任务是 `TASK_DEAD`；从 v4.14 起是 `(p->state | p->exit_state) & TASK_REPORT`，并新增 `TASK_REPORT_IDLE`。只有 v4.14 一个版本报告的是状态的序号 `fls()`，在 v4.15 被 3f5fe9fef5b2 修正；这个版本里 S、D 不受影响，其余状态区分不出来。这些状态位只出现在 `__print_flags()` 里，没有对应的字段可探测，所以比较版本号。

#### softlockup watchdog 的线程
- 内核：`kernel/watchdog.c`；9cf57731b63e ("watchdog/softlockup: Replace "watchdog/%u" threads with cpu_stop_work"), in v4.19
- 依赖：`src/watchdog.c` `watchdog_filter()`
- 做法与后果：v4.18 及以前过滤 `watchdog/*`，v4.19 起过滤 `migration/*`。没有接口能告诉是哪个线程在运行它，所以比较版本号。

---

## 3 ptrace

**用途**：`--ptrace` 与 workload：跟踪目标进程新建的线程和子进程，在它们运行之前加入 prof_dev（`src/ptrace.c`、`src/monitor.c` 的 SIGCHLD 处理）。

**原理**：`PTRACE_SEIZE` 附着到目标线程，但不让它停下。设置 `PTRACE_O_TRACE{FORK,VFORK,CLONE}` 后，
tracee 每创建一个线程/进程，就有两次 ptrace-stop，tracer 都从 `waitpid()`/`waitid()` 得到：

子线程：在 `copy_process()` → `ptrace_init_task()` 里被自动附着到父线程的 tracer。父线程是 PT_SEIZED，
子线程得到 `JOBCTL_TRAP_STOP`（不是 SIGSTOP），同时被置上 TIF_SIGPENDING。它第一次返回用户态时停下，
还没执行过任何一条用户态指令；tracer 看到 `SIGTRAP | PTRACE_EVENT_STOP << 8`，即 "initial ptrace-stop"：

```
schedule
ptrace_stop                    # TASK_TRACED，通知 tracer
ptrace_do_notify
do_jobctl_trap                 # JOBCTL_TRAP_STOP
get_signal                     # v3.10: get_signal_to_deliver
arch_do_signal_or_restart      # v3.10: do_signal
exit_to_user_mode_loop         # v3.10: do_notify_resume
syscall_exit_to_user_mode
ret_from_fork
```

父线程：`kernel_clone()`（v3.10: `do_fork()`）在 `wake_up_new_task(child)` 之后报告 fork，停在 fork 系统调用
里，tracer 看到 `SIGTRAP | PTRACE_EVENT_FORK/VFORK/CLONE << 8`，子线程的 pid 由 `PTRACE_GETEVENTMSG` 取得：

```
schedule
ptrace_stop                    # TASK_TRACED，通知 tracer
ptrace_do_notify
ptrace_notify
ptrace_event                   # PTRACE_EVENT_FORK / VFORK / CLONE
ptrace_event_pid               # v3.10: ptrace_event(trace, nr)
kernel_clone                   # v3.10: do_fork
__x64_sys_clone / fork / vfork
```

子线程先被唤醒、父线程后报告，两者各自走到 `ptrace_stop()`，所以两次 stop 到达 tracer 的顺序不定。
两个线程都停在 `ptrace_stop()` 里，直到 tracer `PTRACE_CONT`/`PTRACE_DETACH`：子线程停着就不会产生
事件，这是 perf-prof 在它运行前加入 prof_dev 的依据。

`PTRACE_INTERRUPT` 让任意 tracee 经同一条 `do_jobctl_trap()` 路径停在 `PTRACE_EVENT_STOP`；group stop
（SIGSTOP 等）要用 `PTRACE_LISTEN` 才能不打断地继续等待。

#### 新线程先停在 initial ptrace-stop，fork 事件与它的到达顺序不定
- 内核：`kernel/fork.c` `kernel_clone()`（旧内核为 `do_fork()`）、`include/linux/ptrace.h` `ptrace_init_task()`、`kernel/signal.c` `do_jobctl_trap()`；`PTRACE_SEIZE`/`PTRACE_INTERRUPT`/`PTRACE_LISTEN` 来自 3544d72a0e10、fca26f260c52、544b2c91a9f1，自动附着不发 SIGSTOP 来自 d184d6eb1dc3 ("ptrace: dont send SIGSTOP on auto-attach if PT_SEIZED")，均 in v3.1
- 依赖：`src/ptrace.c` 文件头注释、`pid_node::initial`、`__fork()`、`__new_or_interrupt()`
- 做法与后果：两次 stop 都看到后（`initial == 2`），才把子线程加入父线程的 prof_dev，然后放行。子线程这时还没运行过，它的事件一个也不会漏。`ptrace_attach()` 附着的线程没有父线程要等，`initial` 直接从 2 开始；此后再收到 `PTRACE_EVENT_STOP`，就是 `ptrace_detach()` 发出的 `PTRACE_INTERRUPT`，或者 group stop 结束。

#### PTRACE_O_EXITKILL
- 内核：992fb6e17063 ("ptrace: introduce PTRACE_O_EXITKILL"), in v3.8
- 依赖：`src/ptrace.c` `ptrace_attach()`（workload）
- 做法与后果：tracer 退出时，内核用 SIGKILL 杀掉 workload。

#### waitid() 接受 __WALL
- 内核：`kernel/exit.c`；91c4e8ea8f05 ("wait: allow sys_waitid() to accept __WNOTHREAD/__WCLONE/__WALL"), in v4.7
- 依赖：`src/monitor.c` `handle_SIGCHLD()`
- 做法与后果：返回 `-EINVAL` 时改用 `waitpid(__WALL)`，再由 status 重建 si_code。

---

## 4 eBPF

**用途**：`--irqs_disabled`、`--tif_need_resched` 等 BPF 过滤器（`src/filter/bpf_filter.c`）、BPF 事件源（`bpf:kvm_exit`）、表达式编译成 eBPF（`src/expr.c`），以及栈回溯里 BPF 程序的符号。

**原理**：BPF 程序先由 verifier 检查，再 JIT 成本机指令。JIT 出来的程序要通过 `bpf_jit_kallsyms` 登记为
内核文本，才能出现在 `/proc/kallsyms`、被栈回溯识别。CO-RE 按 BTF 重定位结构体成员，成员不存在时加载失败。
可用的程序类型和指令集随版本增加。BPF 事件源的内核态过滤见 `docs/bpf_event_filter.md`。

#### BPF_PROG_TYPE_PERF_EVENT
- 内核：0515e5999a46 ("bpf: introduce BPF_PROG_TYPE_PERF_EVENT program type"), in v4.9
- 依赖：`src/filter/bpf_filter.c` `bpf_filter_open()`
- 做法与后果：用 `PERF_EVENT_IOC_SET_BPF` 挂到 perf event 的 fd 上，每次溢出运行一次，返回非 0 才保留样本。旧内核上加载失败。

#### thread_info 在 task_struct 内
- 内核：`CONFIG_THREAD_INFO_IN_TASK`，x86 上来自 15f4eae70d36 ("x86: Move thread_info into task_struct"), in v4.9；`TIF_NEED_RESCHED` 的位置因架构而异
- 依赖：`src/filter/perf_event.bpf.c`
- 做法与后果：在这之前，`task->thread_info` 的 CO-RE 重定位失败，程序加载失败。

#### bpf_jit_kallsyms
- 内核：`kernel/bpf/core.c` `bpf_prog_kallsyms_find()`、`include/linux/filter.h` `bpf_jit_kallsyms_enabled()`；74451e66d516 ("bpf: make jited programs visible in traces"), in v4.11；af91acbc6299 ("bpf: Fix bpf jit kallsym access"), in v5.5
- 依赖：`src/monitor.c` `sysctl_bpf_jit_kallsyms()`；`src/misc.c` `ksymbol_dev_open()`
- 做法与后果：在 v4.11–v5.4 上，开关关闭时 `is_bpf_text_address()` 为假，栈回溯在 BPF 帧处停止，所以需要时临时打开开关。从 v5.5 起，开关只控制 `/proc/kallsyms` 里是否显示名字，不影响栈回溯。`bpf_jit_harden` 打开或 `bpf_jit_kallsyms` 关闭时，`/proc/kallsyms` 里没有 BPF 符号，这时改为跟踪 PERF_RECORD_KSYMBOL。sysctl 在 af91acbc6299 前后都存在，变的只是它对栈回溯的影响，探测不到，所以比较版本号。

#### kvm:kvm_exit 的 exit_reason 不再是 tracepoint 参数
- 内核：`arch/x86/kvm/trace.h`；0a62a0319abb ("KVM: x86: Get exit_reason as part of kvm_x86_ops.get_exit_info"), in v5.16
- 依赖：`src/bpf_kvm_exit.c` `kvm_exit_reason_in_args()`；`src/bpf-skel/kvm_exit.bpf.c`
- 做法与后果：v5.16 起用 `tp/kvm/kvm_exit` 读 trace entry，之前用 `raw_tp` 读参数。两个程序读的是同一个 tracepoint，无法通过谁存在来区分，所以比较版本号。

#### cpu v4 指令（符号扩展加载、有符号 div/mod）
- 内核：1f9a1ea821ff ("bpf: Support new sign-extension load insns")、ec0e2da95f72 ("bpf: Support new signed div/mod instructions."), in v6.6
- 依赖：`src/expr.c` `bpf_have_cpu_v4()`
- 做法与后果：运行时判断。没有 v4 指令时，符号扩展加载用 shl/arsh 代替，有符号 `/`、`%` 在编译期报错。

#### 内核以 -funsigned-char 构建
- 内核：顶层 `Makefile`；3bc753c06dd0 ("kbuild: treat char as always unsigned"), in v6.2
- 依赖：`src/expr.c` `is_char_ptr()`
- 做法与后果：`char comm[16]` 在 format 里标为 `signed:0`，字符串运算符不区分 char 的符号。

---

## 5 进程与 pid

**用途**：pid_filter 的开放末尾：预测新线程会拿到的 pid，减少过滤器替换。

#### pid 循环递增分配，/proc/loadavg 给出最近分配的 pid
- 内核：`kernel/pid.c` `alloc_pid()`、`fs/proc/loadavg.c` `loadavg_proc_show()`；v3.10 已有（位图 + `last_pid`），95846ecf9dac ("pid: replace pid bitmap implementation with IDR API"), in v4.15 起改用 `idr_alloc_cyclic()`，分配顺序不变
- 依赖：`src/filter/event_filter.c` `pid_filter_last_pid()`、开放末尾 `pid>=x`、pid 回绕的处理
- 做法与后果：新线程拿到的是随后分配的 pid，开放末尾不用替换 evsel 就能放进它们；回绕后，末尾截止到下一个被跟踪的 pid。处于嵌套 pid namespace 时，loadavg 里的值不是 init namespace 的 pid，此时返回 -1 不用。如果约束不成立，每个新线程都会触发一次同步，但过滤结果仍然精确。

---

## 6 时钟（x86）

**用途**：`--tsc`、`--kvmclock`：把 perf 时间戳换算成 tsc / guest 时钟（`src/convert.c`）。

**原理**：perf 的时间戳是 `sched_clock`。x86 上它由 tsc 按 `cyc2ns` 的 mult/shift 换算得到，
`perf_event_mmap_page` 的 `time_zero`/`time_mult`/`time_shift` 把这组换算参数交给用户态，
`--tsc`/`--kvmclock` 依赖它们做反向换算。

#### 只有 native sched_clock 才提供 time_zero
- 内核：`arch/x86/events/core.c` `arch_perf_update_userpage()`；698eff6355f7 ("sched/clock, x86/perf: Fix "perf test tsc""), in v4.11 起检查 `using_native_sched_clock()`
- 依赖：`src/convert.c` `current_clocksource()`
- 做法与后果：更早的内核在 paravirt sched_clock 下（比如 guest 用 kvm-clock）也会设置 `cap_user_time_zero`，这时参数是错的。所以只有当前 clocksource 是 tsc 时才使用。

#### tsc 校准修正后 sched_clock 没有更新
- 内核：aa7b630ea023 ("x86/tsc: Feed refined TSC calibration into sched_clock()"), in v4.12
- 依赖：`src/convert.c`（`tsc_conv_fixed`）
- 做法与后果：v4.12 之前，用 KVM 读到的 `tsc_khz` 重新计算 mult/shift。这个修复没有改动任何可探测的接口，所以比较版本号。

---

## 7 uprobe_events

**用途**：`perf-prof usdt add/del` 把 ELF 里的 SDT 探针写成 tracefs 的 uprobe 事件（`src/usdt.c`），之后当作 tracepoint 使用；python 栈（`--python-callchain`）依赖它添加的 python USDT。

**原理**：向 `uprobe_events` 写 `p:GROUP/EVENT PATH:OFFSET [FETCHARGS]` 创建一个 uprobe 事件，`-:GROUP/EVENT` 删除。事件是持久的，与创建它的进程无关。

#### SDT 的 semaphore 以 `(REF_CTR_OFFSET)` 传入
- 内核：`kernel/trace/trace_uprobe.c` `__trace_uprobe_create()`；1cc33161a83d ("uprobes: Support SDT markers having reference count (semaphore)"), in v4.20
- 依赖：`src/usdt.c` `sdt_probe_command()`
- 做法与后果：带 semaphore 的探针写 `PATH:OFFSET(REF_CTR_OFFSET)`，内核在探针启用期间递增 semaphore，程序据此才执行 USDT 的参数准备。v4.20 之前 OFFSET 解析失败、写入被拒绝：打印错误，不创建该事件。
