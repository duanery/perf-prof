# perf-prof 的内部模块与接口

perf-prof 的分析器建立在一组公共模块上：事件的打开与分发、过滤、排序、线程跟踪、统计、栈、时间换算……
实现新功能前先在这里找已有的能力，**能复用就复用，缺的扩展模块，不要在分析器里另写一份**
（`AGENTS.md`「模块边界设计准则」第 8 条）。

每个模块写：

- **源码**：实现文件与头文件，接口看头文件；谁在用，`grep -rn <函数名> src`；
- **能力**：它能做什么；
- **注意事项**：使用时容易踩的坑——调用顺序、内存归属、引用计数、回调里的限制、与其他功能的组合、热路径开销；
- **备注**（可选）：它不管的事，越过这条线就该放在调用者或别的模块；
- **内核功能**：它使用的内核功能大类，具体依赖见 [kernel_dependencies.md](kernel_dependencies.md)。

新增公共模块、接口变化、发现新的坑时，同步本文（随文档提交）。

---

## 1 框架

### 1.1 profiler 与 prof_dev

- **源码**：`src/monitor.c`、`src/monitor.h`
- **能力**：profiler 是一个分析器的定义（回调集合），prof_dev 是它的实例：按 env 打开 evlist，attach 到
  cpus / threads，建立 ring buffer、order、时间换算、interval 定时器，把事件分发到回调。prof_dev 可以组成树：
  子设备（parent）、clone（同一 profiler 与 env，换一组 cpus / threads）、forward（源设备的事件包成
  `PERF_RECORD_DEV` 交给目标设备）。运行期可增删线程、替换 evsel 与过滤器。
- **注意事项**：
  - **env 归 dev 所有**：`prof_dev_open*()` 失败时框架已 `free_env(env)`，调用者不能再释放或复用。
    带 parent 打开的 dev 初始是 OFF，等 parent enable 或自己 `prof_dev_enable()`。
  - **init / deinit 的时机**：`init()` 在时间换算、open、mmap、order 之前执行，拿不到这些状态；init 可以改
    `dev->cpus/threads`。init 之后任一步失败、遇到 ESRCH、`reinit()` 都会调 `deinit()`，init/deinit 可能执行多次，
    **deinit 必须能处理只初始化了一半的状态**。正常关闭时 deinit 在最后一次 flush 之后，只释放内存。
  - **回调的 `(cpu, tid)` 是 ring buffer 的绑定**，不是记录里的 cpu/tid，二者只有一个有效（另一个为 -1）；
    状态按绑定索引，不按 evlist / mmap 数组下标。
  - **事件只在回调期间有效**：回调返回后 ring buffer 被消费；转换过的事件可能在共享缓冲里，下一个事件覆盖它。
    要保存用 `perf_event_dup()`，并在 `tp_list_free()` 之前释放。
  - **回调里可以关闭 dev**（有 `inclose` 防重入，分发方持有引用），但关闭会同步执行 disable、最终 flush，
    回调可能被重入；`prof_dev_close()` 之后检查 `dev->inclose / state` 再继续。关闭 forward 目标会连带关闭所有源。
  - **引用计数**：`prof_dev_get()` 在计数为 0 时返回 NULL，必须检查；`prof_dev_put()` 返回 true 表示 dev 已释放；
    `for_each_*_get()` 中途 break 要手动 `prof_dev_put()`；`prof_dev_unuse()` 减到 0 会关闭 dev。
  - **`prof_dev_replace_evsel()` / `prof_dev_replace_filter()`**：开了 order 时在 sample 回调里调用返回 -EAGAIN，
    嵌套调用返回 -EBUSY，没有 sample id 返回 -EINVAL，均应稍后重试；成功后旧 evsel 已删除，缓存的旧指针全部失效。
    调用前检查 `dev->inclose`。
  - **线程增删**：profiler 实现了 `add_thread` 回调就取代了默认的 `perf_evlist__add_thread`，需自己调用；
    `del_thread` 只是通知，cpu ring buffer 读空之后才触发。dev 在关闭中时 add/del 直接返回 0。
  - **forward**：源 dev 的 sample_type 必须包含 TID | TIME | CPU；只转发 `PERF_RECORD_SAMPLE`。
  - **`main_epoll_add()`**：fd 被设为非阻塞；同一 fd 再 add 会覆盖 handler；水平触发，handler 不读空会反复调用；
    关闭 fd 前先 `main_epoll_del()`（handler 内调用是安全的）。
- **内核功能**：perf_event。

### 1.2 evsel 的私有数据与采样布局

- **源码**：`src/convert.c`、`src/monitor.h`
- **能力**：evsel 关联它的 `struct tp` 与分析器的 cookie；按 sample_type 计算记录中各成员的偏移。
- **注意事项**：
  - libperf 的 evsel priv 固定是 `struct prof_evsel`，**profiler 不能调用 `perf_evsel__set_priv()`**，自己的标记用 cookie。
    cookie 为 0 表示未设置，惯例存 `下标 + 1`。
  - `perf_evsel_tp()` 只对 `tp_evsel_new()` 创建的 evsel 有值；`perf_evsel_copy_priv()` 只复制 tp 与 cookie。
  - member_cache 第一次使用时构建、之后不更新，**必须在 sample_type 定型之后使用**（`prof_dev_env2attr()`
    还会追加 REGS_USER / STACK_USER）；可能返回 NULL。
  - `perf_event_member_offset()` 对变长成员（callchain、raw、read 之后的成员）每次都要逐个解析，热路径上有开销；
    cache 必须属于产生该事件的 evsel。
  - 多个采样 evsel 且没有 sample id 时，`perf_event_evsel()` 返回 NULL。
- **内核功能**：perf_event。

### 1.3 order：多 ring buffer 按时间排序

- **源码**：`src/order.c`、`src/monitor.h`
- **能力**：各 ring buffer 与外部事件流（stream）的记录按时间堆排序后再分发；父子设备、forward 源汇聚到
  顶层 dev 一起排序；按 lost 预测等待。
- **注意事项**：
  - 不开 order，多个 ring buffer 之间的事件不保证时间序。**依赖全局时序的 profiler 要用 `using_order()` 检查**，
    或在 profiler 上设 `.order = true`。
  - 需要 `PERF_SAMPLE_TIME`；堆的大小按 mmap 数确定，在 mmap 之后初始化。
  - **lost**：不开 order 时 lost 回调的 `lost_start/lost_end` 为 0；开了 order 是一个 evclock 区间。
    只有实现了 `lost` 回调才做丢失预测，可能暂停等待（LOST_WAIT），增加输出延迟。lost 之后的状态正确性由 profiler 自己保证。
  - **stream**：`read_event` 必须正确设置 writable / converted；处理中 unregister 或删除 mmap 只打标记、延迟释放；
    `order_process()` 不可重入，在处理中调用直接返回。
  - `reduce_wakeup_times()` 作用于 open 之前的 attr；子 dev 的水位改为 50%，事件要等 parent flush 才可见。
  - **`minevtime()` 回调**返回 evclock，必须不晚于 ring buffer 中最早的事件；`ULLONG_MAX` 和 0 是特殊值。
- **内核功能**：perf_event。

### 1.4 时间换算

- **源码**：`src/convert.c`、`src/monitor.h`
- **能力**：perf 时间与 tsc、kvmclock、monotonic、墙上时间之间换算，改写记录里的时间（`--tsc`、`--kvmclock`、
  `--clock-offset`、`--monotonic`）。
- **注意事项**：
  - **回调里的时间是 evclock**（tsc、kvmclock 或 perf 时间，加上 clock_offset），不一定是 ns：
    时间差用 `evclock_to_real_ns()`，和 perf 时间比较用 `evclock_to_perfclock()`。
  - `perf_event_convert()` 对不可写的事件返回共享缓冲，下次调用覆盖。forward 来的事件已按源 dev 转换过，不能再转一次。
  - 换算可能在运行时被关掉（TSC 不可用、与 `--monotonic` 同用），不要在 init 里缓存是否换算。
  - 所有采样 evsel 的 sample_type 前缀（IDENTIFIER / IP / TID / TIME）必须一致。
- **内核功能**：perf_event、时钟（x86）。

### 1.5 定时器

- **源码**：`src/timer.c`、`src/timer.h`
- **能力**：主循环里的 timerfd 定时器，单次或周期，过期多次时限制一次回调的次数。
- **注意事项**：
  - `timer_destroy()` 只能在 `timer_init()` 成功后调用（零初始化的结构体会关掉 fd 0），用 `timer_started()` 判断。
  - 回调在主 epoll 循环里同步执行，不能阻塞；用 `container_of` 找回宿主结构。
  - 再次 `timer_start()` 会重设周期；`timer_start/cancel` 的返回值（剩余时间）不准确，不要依赖。
- **备注**：prof_dev 的 `interval()` 由框架按 `-i` 驱动，不需要另建定时器。
- **内核功能**：无专门依赖。

### 1.6 采样量评估（perfeval）

- **源码**：`src/perfeval.c`、`src/monitor.h`
- **能力**：按 cpu / tid 统计采样数，超过 `--sampling-limit` 时关闭 dev，控制 perf-prof 自身开销。
- **注意事项**：
  - 只在设置了 `-i` 时生效，在 interval 里评估，超限时直接 `prof_dev_close()`。
  - 计数在用户态过滤（ftrace_filter）、`-N` 之前，被过滤掉的事件也计入。
  - forward 场景下，target 与源 dev 的 cpu/tid 偏移须与 `perf_record_dev` 一致，只在 target open 时检查一次。
- **内核功能**：perf_event。

### 1.7 bindings：按 ring buffer 绑定保存状态

- **源码**：`src/bindings.c`、`src/bindings.h`
- **能力**：以回调的 `(cpu, tid)` 为键的红黑树，每个绑定一份 profiler 自定义的状态，按需创建、随线程删除；
  另有把 `(cpu, tid)` 编成一个 u64 的 `prof_binding_key()` 及反解函数，可单独用作统计的 instance。
- **注意事项**：
  - 状态结构体的**第一个成员必须是 `struct prof_binding`**，`size` 填整个状态的大小；节点不移动，指针可保存到 remove 为止。
  - `prof_binding_get()` 不存在时创建（calloc 后调 `init`），内存不足返回 NULL；`prof_binding_find()` 只查不建。
  - `destroy` 只释放状态里的资源，节点本身由 bindings 释放。
  - `prof_bindings_remove_thread()` 只删 tid 绑定（per-thread 的 ring buffer），在 `del_thread` 回调里调用；
    cpu 绑定只在 `prof_bindings_exit()` 时释放。
  - `prof_bindings_for_each()` 的循环体内不能删除当前节点。
  - 键里的 cpu / tid 只有一个有效（见 1.1），打印用 `prof_binding_id()` 并按 `prof_binding_cpu() >= 0` 区分 CPU / thread。
- **内核功能**：无。

---

## 2 事件

### 2.1 tep：tracepoint 格式

- **源码**：`src/tep.c`、`src/tep.h`
- **能力**：加载 tracepoint format，查事件 id、字段是否存在与大小、打印事件；运行时探测内核差异。
- **注意事项**：
  - 全局一个 tep，引用计数。`tep__ref()` 加载 plugins、ksym resolver 并持有 global_comm（扫描 /proc，开销大）；
    `tep__ref_light()` 只分配 tep。都与 `tep__unref()` 配对。
  - **字段探测要在持有 tep 时做**：tep 为 NULL 时 `tep__event_id()` 只读 id 不解析 format，之后的
    `tep__event_has_field()` 等会在空 tep 上查找而失败。profiler 在 init 开头 `tep__ref()`，deinit 才 unref。
  - `tep__event_fields()` 返回的数组由调用者 free，但 name 指向 tep 内部，tep 释放后不能用。
    BPF 事件（`tp_is_bpf()`）的字段来自 BTF，name 指向源的缓存，进程退出前一直有效。
  - `event_field_scalar()` 按 `is_unsigned` 读出一个标量字段（`size == elementsize`）并扩展到 `long long`；
    无符号字段由调用者转回 `unsigned long long`。python、sql 读 BPF 事件的字段共用它。
  - 有人持有 `tep__ref()` 时 `tep__update_comm()` 不生效，comm 走 global_comm（见 5.2）。
  - `tep__sched_switch_preempt_state()` 结果缓存在静态变量；探测失败为 0，只警告一次。
- **内核功能**：tracepoint / ftrace。

### 2.2 tp / tp_list：`-e` 事件串

- **源码**：`src/tep.c`、`src/tep.h`
- **能力**：把 `-e` 事件串解析为 `struct tp` 列表（tracepoint、kprobe、uprobe、profiler 设备），带事件属性
  （filter、key、role、num、stack、alias、cpus、push / pull ……）；为 tp 建 evsel、应用过滤器、运行属性表达式。
- **注意事项**：
  - `tp->name/sys/alias/key` 和属性值都指向 `tp_list` 持有的字符串，随 `tp_list_free()` 释放，不能单独 free；
    改过滤器用 `tp_update_filter()`。
  - 属性 `cpus=` 与 `dev->cpus` 取交集，`dev->cpus` 要在 `tp_list_new()` 之前确定。`push=` / `pull=` 对 profiler 设备事件无效。
  - **profiler 设备事件**（`tp_is_dev()`）：`tp->evsel` 与 `tp->source_dev` 是同一个 union，不能当 evsel 用；
    遍历 evsel 用 `for_each_real_tp()`，设备用 `for_each_dev_tp()`。source_dev 随父 dev 关闭，不随 `tp_list_free()`。
  - `tp_evsel_new()` 复制 attr 模板；kprobe / uprobe 采样时模板必须带 sample id（ID / IDENTIFIER / STREAM_ID）。
  - **`tp_list_apply_filter()` 在 evsel open 之后调用**。内核拒绝过滤器时，只有 profiler 实现了 `ftrace_filter`
    才退到用户态过滤（`dev->ftrace_filter = 1`，每条记录跑表达式，有开销），否则报错。
  - `tp_list_ftrace_filter()` 先按 raw 的 `common_type` 找 tp，找不到再按记录的 evsel 找（kprobe / uprobe 的 common_type 为 0），
    所以要传入原始 event。
  - `tp_prog_run()` 出错返回 -1，与表达式算出 -1 无法区分；`tp_get_key/role/num()` 把 -1 当 0。
    `tp_new_prog()` 的 prog 由调用者 `expr_destroy()`。
  - 新增 probe 解析路径要遵守出口的约定：`uprobe_path` / `kprobe_func` 非空即已 pin / get，在 `tp_list_new()` 出口统一释放。
- **内核功能**：tracepoint / ftrace、perf_event。

### 2.3 tp_matcher：筛选与指定 cpu / tid 关联的事件

- **源码**：`src/tep.h`、`src/tep.c`（注册分散在使用处，`grep -rn TP_MATCHER_REGISTER src`）
- **能力**：事件发生在哪个 cpu、在哪个线程里采样，调用方从样本头判断；matcher 补充只有事件自己知道的关联，按事件注册：
  - `refer_cpu` / `refer_tid`：事件字段是否提到某 cpu / tid（如 sched_wakeup 的 target_cpu、被唤醒的 pid）；
  - `task_cpu`：跟踪某任务所在的 cpu，事件挪动了它时给出之后所在的 cpu（离开运行队列为 -1）和原因（`[wakeup]`、`[migrate]`、`[sleeping]` 等）；
  - `switch_to`：上下文切换事件，给出切入任务的 tid 和切出任务的 comm。
- **注意事项**：
  - 按 sys + name 精确匹配；sys 为 NULL 时 name 是 profiler 名（profiler 设备事件）。raw 的含义随之不同：
    设备事件是 `perf_record_dev`，由源 profiler 解出原始事件再匹配；否则是 tracepoint raw。
  - tid 即内核的 pid，与 sched tracepoint 的 `pid` 字段同义，不是 tgid。
  - 没注册 matcher 的 tracepoint，format 有 `cpu` / `pid` 字段时用默认 matcher，每次调用跑一遍表达式，热路径有开销；
    需要高频调用的事件应注册专门的 matcher。
  - 回调为 NULL 时返回 false（如 sched_switch 没有 refer_cpu：它只关联发生的 cpu）。
- **内核功能**：tracepoint / ftrace。

### 2.4 sched：调度事件的状态

- **源码**：`src/sched.c`、`src/monitor.h`
- **能力**：从 sched_switch / sched_wakeup 维护每个 cpu 上运行的任务，判断一次 sched_wakeup 是否多余
  （唤醒自己、唤醒正在运行的任务，这类唤醒没有配对的 sched_switch）。
- **注意事项**：
  - 只有事件里有 sched_wakeup 才生效；有不带过滤器的 sched_switch 才能判断"唤醒正在运行的任务"（level 2），
    sched_switch 带过滤器时 cpu 上的任务不准，只判断唤醒自己（level 1）。
  - 每个事件都要先 `sched_event()`，再 `sched_wakeup_unnecessary()`，且按时间序（需要 order）。
  - 状态是全局的，所有 dev 共享。
- **内核功能**：tracepoint / ftrace。

### 2.5 tracepoint 子设备

- **源码**：`src/trace.c`、`src/monitor.h`
- **能力**：`trace_dev_open()` 在分析器内部打开一组 tracepoint，raw 回调给父设备。
- **注意事项**：
  - 传了 parent 时默认不 enable，等 parent enable 或自己 `prof_dev_enable()`。
  - 回调只有 raw，没有 cpu / tid / time；`hangup` 可直接传 `prof_dev_close`。
  - attach 到频繁切换的线程会抬高这些线程的开销（见 task-state 改用 ptrace 的原因）。
- **内核功能**：tracepoint / ftrace、perf_event。

### 2.6 事件传播（push / pull）

- **源码**：`src/event-spread.c`、`src/event-spread.h`、`src/net.c`、`src/net.h`
- **能力**：事件属性 `push=` 把事件广播出去（本机或 TCP），`pull=` 接收别处的事件作为本地事件源，如 Guest → Host。
- **注意事项**：
  - `push=` 强制开启 order，并引用一个全局 perf-clock 设备，每 10ms 刷新排序时间。
  - `tp_broadcast_event()` 对配置了 push 的 tp 返回 1；trace 把 1 当作"已转发，本地不再输出"，新 profiler 自行决定是否沿用。
  - pull 端校验第一个记录：sys:name、sample_type 相同，不带 CALLCHAIN，事件大小一致，否则关闭该连接。
    pull 来的事件改写 cpu / pid / id 后注入本地；带 pull 的 tp 本地 evsel 一直保持 disable。
  - dev 未 enable 时收到的事件直接丢弃（如 `--kvmclock` 等待 pvclock 时）。
  - 每个连接 64K 环形缓冲，写满不阻塞，计为 `PERF_RECORD_LOST`。
- **内核功能**：perf_event。

### 2.7 USDT 探针

- **源码**：`src/usdt.c`、`include/linux/sdt_arg.h`、`arch/{x86,arm64}/lib/sdt_arg.c`
- **能力**：`perf-prof usdt {add|del|list}`：读 ELF 的 `.note.stapsdt`，算出探针的文件偏移、semaphore 偏移，
  把 `N@OP` 形式的参数转成 uprobe 的 fetch 参数，写 `uprobe_events` 创建 / 删除 uprobe 事件；之后以
  `provider:name` 当作 tracepoint 使用。
- **注意事项**：
  - 解析函数都是 static，现在只能通过 `usdt` 命令使用；要在事件串或其他模块里直接用 USDT，先把 ELF 解析与
    命令生成抽成公共接口。
  - 创建的 uprobe 事件是持久的，perf-prof 退出后仍在，要 `usdt del` 删除；group 名就是 provider。
  - 带 semaphore 的探针在命令里写 `(ref_ctr_offset)`，内核从 v4.20（1cc33161a83d）才支持，旧内核上写入失败，
    只打印错误、不创建事件。
  - 参数转换只实现了 x86 与 arm64，其他架构走 weak 实现返回 SKIP，参数被静默丢弃；
    `N` 超出 ±8 时整条探针失败。自定义参数格式里的 `%s` 代入转换后的操作数（如 `+0(%s):string`）。
  - 探针地址小于第一个可执行段时只告警并跳过；地址按 `.stapsdt.base` 修正（prelink）。
- **备注**：python 栈（7.4）依赖先用它添加 python 的 USDT。
- **内核功能**：uprobe_events。

### 2.8 bpf_event：BPF 事件源（`-e bpf:xxx`）

- **源码**：`src/bpf_event.c`、`src/bpf_event.h`；构建 `build/Makefile.build`（`%.event.o`）、`src/bpf-skel/bpf_event.tmpl.c`、
  `src/bpf-skel/bpf_event.bpf.h`；事件源：`src/bpf-skel/xxx.bpf.c` + `xxx.h`（事件结构体）+ 可选的 `src/bpf_xxx.c`（用户态 ops）。
- **能力**：把内置 BPF 程序产生的事件当作 tracepoint 用：`list` / `help` / 补全 / 通配符都能看到；字段与大小从 BTF 读出，
  `/EXPR/` 是内核态过滤（3.5）；样本经 `PERF_COUNT_SW_BPF_OUTPUT` 进 ring buffer，可与 tracepoint 一起 order、multi-trace、top、num-dist。
  - 两层接口：**源**（`bpf_event_source_*`：find / fields / size / desc / help / print，只读元信息，随进程存在）；
    **实例**（`bpf_event_new` / `_start` / `_free`：每个 tp 一个，自己 open、过滤、prepare、load 并 attach 一份 skeleton）。
  - 写一个新源：`.bpf.c` 里写 `BPF_EVENT_DESC("...")`、名为 `perf_events` 的 `PERF_EVENT_ARRAY` map、`DEFINE_EXPR_FILTER(struct 事件)`，
    用 `perf_output()` 输出整个结构体；`src/bpf-skel/Build` 加 `xxx.event.o`。skeleton 由模板生成，缺 `bpf_event_desc` 或 `perf_events` 编译失败。
    需要源自己的配置（跟踪进程、写 rodata）加 `ops->prepare`；结构体里有通用打印表达不了的内容（采样栈等）加 `ops->print`，`BPF_EVENT_OPS(xxx, &ops)` 注册。
    范例：`bpf_sched_rundelay.c`（单进程）、`bpf_kvm_exit.c`（prepare + print，与 `bpf:kvm_exit` 分析器共用）。
- **注意事项**：
  - **只能采样**：计数模式（stat、hrcount、hrtimer 的 `sample_period == 0`）被拒绝；BPF 输出的 period 恒为 0。
  - **每个实例自己 open 一份 skeleton**：libbpf 不允许同一个 object 再次 load，过滤器与 prepare 又会改 object，不能共用。
  - **scope**：`-C` 为 cpu scope，`-p/-t` 为进程 scope（`tp_bpf_scope()` 给出去重后的 tgid 列表），要求源有 `ops->prepare`；
    多个进程时源自行决定（现有两个源都拒绝）。`--cgroups` 被拒绝，`--exclude-*` 提示不生效（BPF 程序对所有任务运行）。
  - **启动顺序**：`prof_dev_bpf_start()` 在 `perf_evlist__open()` 之后、`prof->filter()` 之前，此时才 load 并把 fd 放进 map；
    用户态过滤器先编译，BPF 后端失败（如旧内核的有符号除法）时事件不带内核过滤器重新启动，并退到用户态过滤（`tp->ftrace_filter`）。
  - **样本头的 comm / pid 是运行 BPF 程序的任务**，不一定是事件描述的任务；要给用户看的任务写在结构体里。
  - 不支持：`push=` / `pull=`、`prof_dev_replace_filter()`。
  - **python / sql 按字段访问**：字段一律经 `tep__event_fields(tp->id)` 取（BTF），不要用 `tep_find_event()`——BPF 事件不在 tep 里。
    raw 没有 trace_entry，读 `common_*` 会读到结构体的前 8 字节；要区分 `char[]`（文本）与 `u8[]`（字节）看 `is_string`（第 4 节）。
  - `tp_print_event()` 对 BPF 事件走源的 `ops->print`，没有则按 BTF 通用打印（标量、枚举名、char 数组为字符串）；它的 `tid` 参数是样本的 tid。
  - 事件 id 是 tep 之外的伪 id（从 `BPF_EVENT_BASE` 起），用 `tp_is_bpf()` 判断，不要拿去查 tep。
- **内核功能**：eBPF（4.1）、perf_event。

---

## 3 过滤

三层过滤：eBPF、PMU（attr 的 exclude_* 位，散落在各分析器）、ftrace（tracepoint 过滤器）。

### 3.1 tracepoint 过滤器字符串

- **源码**：`src/filter/tp_filter.c`、`src/filter/filter.h`
- **能力**：由 thread_map / cpu_map 拼出 `pid==x||...`、`cpu==...`；组合 pid、comm 与用户过滤器。
- **注意事项**：
  - `pid_filter()`、`cpu_filter()` 返回的字符串由调用者 free；只合并连续值，**不受一页长度限制**，线程多时会被内核拒绝，
    此时用 event_filter（3.2）。
  - `tp_filter_new()` 既无 comm 也无 pid 时返回 NULL，不是错误。
- **内核功能**：tracepoint / ftrace、perf_event。

### 3.2 event_filter：一个字段上的值集合

- **源码**：`src/filter/event_filter.c`、`src/filter/filter.h`
- **能力**：维护一个字段的值集合，生成 `(and_filter) && (ranges)`；超过一页时合并最小间隔。
- **注意事项**：
  - 超过一页时过滤器是**超集**：`event_filter_exact()` 为假时，调用者要用 `event_filter_has()` 精确判断。
    共享同一份值时，任何一方的删除都会让 exact 变假。
  - `event_filter_build()` 的结果归 ef 所有，下次 build 失效。
- **备注**：只产出字符串，不认识 evsel、prof_dev、raw；不决定何时同步到内核，也没有时间。
- **内核功能**：tracepoint / ftrace、perf_event。

### 3.3 pid_filter：跟随线程变化的 pid 过滤器

- **源码**：`src/filter/event_filter.c`、`src/filter/filter.h`
- **能力**：基于 event_filter 的 pid 过滤器，线程随来随走（`--ptrace`、workload）；新线程不在内核过滤器内时立即同步，
  其余由自带定时器稍后收紧；多个过滤器可共享同一组线程。
- **注意事项**：
  - 顺序：new → add_threads / add → start → 运行期 add / del / match → free。
  - **`ops->set()`**：`initial` 为真时 evsel 还没 open，只改 `tp->filter`；否则用 `prof_dev_replace_filter()` 换 evsel。
    返回 <0 时旧过滤器继续生效、稍后重试；dev 在关闭中应返回 -EBUSY。
  - **`pid_filter_add()` 可能在调用栈里立即调 `ops->set()`**（新线程不在内核过滤器内），回调要能在此上下文执行。
  - `ops->ftrace_filter(need)` 只在变化时调用，初值为 false；need 期间调用方对每条记录 `pid_filter_match()`。
  - `share` 只共享值：add / del 要在每个共享者上各调用一次。
- **备注**：不替换 evsel（交给 `ops->set()`），不管线程从哪来（ptrace、comm 由调用者 add / del）。
- **内核功能**：tracepoint / ftrace、perf_event、进程与 pid。

### 3.4 eBPF 过滤器

- **源码**：`src/filter/bpf_filter.c`、`src/filter/perf_event.bpf.c`、`src/filter/filter.h`
- **能力**：perf_event 上挂 BPF 程序过滤：`--irqs_disabled`、`--tif_need_resched`、`--exclude_pid`、
  `--nr_running_min/max`、`--sched_policy` 等。
- **注意事项**：
  - `bpf_filter_init()` 返回条件数，为 0 时不要 open。
  - open 失败、或没有 `CONFIG_LIBBPF` 时 `bpf_fd` 为 -1，挂到 evsel 前检查 `bpf_fd >= 0`。
  - init 之后一定要 close（释放 prio_map）。
- **内核功能**：eBPF、perf_event。

### 3.5 BPF 事件源的表达式过滤

- **源码**：`src/bpf_expr_filter.c`、`src/bpf_expr_filter.h`、`src/bpf-skel/expr_filter.bpf.h`
- **能力**：把用户表达式编译成 eBPF 指令，替换 BPF 程序里的 `expr_filter()` 占位符，在内核态过滤 BPF 事件。
- **注意事项**：
  - **必须在 `bpf_object__open()` 之后、`bpf_object__load()` 之前调用**。
  - BPF 程序用 `DEFINE_EXPR_FILTER()` 定义唯一的占位函数，参数是 struct 指针；字段只支持标量与标量数组，
    数组下标要让 verifier 能确定边界（如 `i & 15`）；不能用 `_cpu` / `_pid`。
  - 事件结构体要在可写内存（表达式可赋值）。细节见 [bpf_event_filter.md](bpf_event_filter.md)；`-e bpf:xxx` 用它做内核态过滤，见 2.8。
- **内核功能**：eBPF。

---

## 4 表达式

- **源码**：`src/expr.c`、`src/expr.h`，语法见 [expr.md](expr.md)
- **能力**：c4 表达式编译器：事件属性里的 EXPR、tracepoint 过滤器的用户态回退；变量由 `global_var_declare` 声明；可生成 eBPF 指令。
  `global_var_declare.is_string` 标记文本型 `char` 数组（与 `u8` 数组区分，供 python 的 str/bytes、sql 的 TEXT/BLOB 使用），
  只有 BPF 事件的字段（`bpf_expr_filter_fields()`，按 BTF 类型名 `char`）会设置；tracepoint 字段恒为 false，
  它们的字符串属性仍从 libtraceevent 读（`tep__string_field_as_binary()`）。表达式编译器不看这一位。
- **注意事项**：
  - 编译器用全局状态与 `setjmp`，**不可重入、不能多线程**。
  - 每个 prog 只有一块 data：先 load 再 run，同一个 prog 不能在嵌套回调里交叉 load / run。
  - data 的大小按声明字段的最大偏移向上取整到 256 字节，变长 raw 超出时 load 失败。
  - 引用了 `ksymbol()` / `comm_get()` 的 prog 持有符号表 / global_comm 引用直到 `expr_destroy()`；
    `system()` 会 fork shell，不能用在热路径。
  - `expr_to_bpf()` 不支持字符串、`printf` / `ksymbol` / `comm_get` / `system` 等函数，嵌套深度受寄存器槽位限制，
    有符号除法需要较新的内核，只能访问事件内的字段。
- **内核功能**：eBPF（仅 `expr_to_bpf()`）。

---

## 5 线程与进程

### 5.1 ptrace：跟踪新线程 / 子进程

- **源码**：`src/ptrace.c`、`src/monitor.h`
- **能力**：`--ptrace` 时跟踪目标进程 fork / vfork / clone 出的线程与子进程，在新线程运行前把它加入 prof_dev；
  线程全部退出后关闭 prof_dev。
- **注意事项**：
  - **dev 的生命周期跟着被跟踪的线程走**：`ptrace_attach()` 内部 use / unuse dev，所有 pid 都没挂上时
    dev 可能在调用里就被关闭；调用者没持有引用时调用后不能再访问 dev。
  - `PTRACE_SEIZE` 失败（已被 gdb / strace / 另一个 perf-prof 跟踪）只跳过，不报错。
  - workload 不 detach，而是 SIGKILL：`-N` 等提前关闭 dev 的功能会杀掉 workload。
  - 解链时先 `prof_dev_del_thread()` 再 unuse（最后一次 unuse 会关闭 dev，del_thread 需要 evlist 还在）；
    dev 在关闭中时跳过 del_thread，避免 close → detach 重入。
  - 被跟踪线程的信号会原样重新注入；group-stop 用 `PTRACE_LISTEN`。
- **备注**：不管新线程加入后的过滤器（由 prof_dev / pid_filter 负责）。
- **内核功能**：ptrace、进程与 pid。

### 5.2 comm：pid → 进程名

- **源码**：`src/comm.c`、`src/monitor.h`
- **能力**：全局的 pid → comm 表，跟踪进程创建、改名、释放；释放时通知注册者。
- **注意事项**：
  - `global_comm_ref()` / `unref()` 配对；第一次 ref 打开一个 comm 设备并扫描整个 /proc，开销大。`tep__ref()` 隐式 ref 一次。
  - `global_comm_get()` 返回内部节点的指针，**不能保存**（进程退出后被回收）；要长期保存就复制，或用 `unique_string()`。
  - 查不到 pid 时会同步 flush comm 设备：在 sample 回调里调用有重入与延迟开销。
  - exec 不更新 comm，需要时 `global_comm_flush(pid)` 标记为过期。
  - `NOTIFY_COMM_DELETE` 在回收时触发，不在进程退出时；只注册通知而没有 `global_comm_ref()` 收不到通知。
- **内核功能**：tracepoint / ftrace、perf_event。

### 5.3 vcpu_info：虚拟机 vcpu 线程

- **源码**：`src/vcpu_info.c`、`src/vcpu_info.h`
- **能力**：通过 virsh 取得虚拟机各 vcpu 的线程号。
- **注意事项**：
  - `vcpu_info_get()` / `put()` 配对，相同 uuid 共享；只保存 uuid 指针，字符串要比对象活得久。
  - 会 popen virsh，只在初始化阶段调用；数据是一次性快照，vcpu 迁移、vcpupin 之后不更新。
  - `kvm_vm_fd` 是 qemu 进程里的 fd 号，只能拼 debugfs 路径，不能在本进程当 fd 用。
- **内核功能**：无（依赖 libvirt / qemu）。

---

## 6 统计

### 6.1 延迟分布

- **源码**：`src/latency_helpers.c`、`src/latency_helpers.h`、`lib/tdigest.c`
- **能力**：按 (instance, key) 统计延迟的次数、总和、最小、最大，可选 tdigest 分位。
- **注意事项**：
  - **`latency_dist_print()` / `print_sorted()` 打印的同时删除节点**，打印一次即清空。
  - 分位模式每个节点一个 tdigest、每次 input 都插入，内存与 CPU 开销明显。
  - extra 区域删除节点时直接 free，不能放需要释放的指针。
  - 有引用计数（`latency_dist_ref()` / `free()`）。
- **内核功能**：无。

### 6.2 计数分布

- **源码**：`src/count_helpers.c`、`src/count_helpers.h`
- **能力**：按 (ins, id, key) 计数，可带直方图。
- **注意事项**：没有引用计数；不能传 NULL；`count_dist_print()` 不删除节点，`reset` 同时清空最大值。
- **内核功能**：无。

### 6.3 two-event：两个事件的配对分析

- **源码**：`src/two-event.c`、`src/two-event.h`
- **能力**：两个事件按 key 配对后的分析实现：`delay`、`pair`、`syscalls`、`call`、`call-delay`；配对之间的事件迭代
  （`--detail`）。multi-trace、nested-trace、rundelay 的底层。
- **注意事项**：
  - 派生实现的 delete 最后调用 `two_event_delete()`；class 删除时会回调派生类的 object_delete。
  - `object_new(tp1, NULL)` 合法（remaining、call）。
  - remaining：每个实现只处理特定原因，其余返回 `REMAINING_BREAK`，调用方遇 BREAK 停止遍历。
  - **`event_iter` 只在 `--detail` 时非 NULL**，实现里要判断；迭代的是全局 timeline 上的所有事件，用 `event_need_to_print()` 过滤。
  - 按栈聚合（kmemprof）时 tp 必须带 `stack` 属性，否则只告警并退化为只统计字节数。
- **内核功能**：无（处理已采到的记录）。

### 6.4 热力图

- **源码**：`src/stack_helpers.c`、`src/stack_helpers.h`
- **能力**：输出 时间 × 延迟 的热力图数据。
- **注意事项**：
  - 单位字符串只保存指针，传常量。
  - 打开时把已有的 `<path>.lat` 改名为 `.old`；多个对象要用不同的 path。
  - 每次 write 都直接写文件；`--tsc` 时写入的是 cycle 而不是 ns。
- **内核功能**：无。

---

## 7 栈与符号

### 7.1 符号解析

- **源码**：`src/trace_helpers.c`、`src/trace_helpers.h`、`src/stack_helpers.c`、`src/misc.c`
- **能力**：内核符号（kallsyms，含 BPF JIT 等动态符号）、进程符号（按 pid 加载 ELF / dso，带缓存）、文件偏移、
  MiniDebugInfo、demangle；块设备分区。
- **注意事项**：
  - `ksyms_get/put()`、`function_resolver_ref/unref()`、`callchain_ctx_new/free()` 共用全局引用计数，
    返回的符号只在持有引用期间有效。
  - **进程符号在第一次查询时对 /proc/pid/maps 做快照**，之后 dlopen 的库解析不到；进程已退出时加载失败且不缓存，
    每次查询都重读 /proc。按 pid 回收依赖 comm 的回收（需要有人持有 global_comm）。
  - `syms__file_offset()` 批量调用前 `syms__file_pin()`、用完 `unpin()`（同一字符串），否则每次都重新解析 ELF；
    `*err_msg` 由调用者 free。
  - **`unique_string()` 永不释放**，可比较指针，但不能放高基数内容（如时间戳），否则内存持续增长。
- **内核功能**：perf_event、eBPF。

### 7.2 调用栈输出与聚合

- **源码**：`src/stack_helpers.c`、`src/stack_helpers.h`、`src/monitor.c`（`callchain_flags()` 等）
- **能力**：打印 callchain（内核 / 用户 / python），按键值聚合栈，输出火焰图。
- **注意事项**：
  - attr 的 `exclude_callchain_user/kernel(dev, dflt)` 与 `callchain_ctx_new(callchain_flags(dev, dflt))`
    **必须用同一个 dflt**，否则采集与解析的范围不一致。`CALLCHAIN_DWARF` 必须带 USER。
  - `callchain_data` 指向事件缓冲，只在当前 sample 有效；要保留就加入 keyvalue_pairs / flame_graph（会复制）。
  - keyvalue_pairs 的 value 在 reinit / free 时直接释放，不能放需要释放的资源；`sorted_*` 每次都全量排序。
  - flame_graph 添加时把用户栈帧转成 `unique_string`（进程退出后符号会释放），每个帧永久占用内存；
    `flame_graph_close()` 已包含释放，不要再 `flame_graph_free()`。
  - python 绑定：`callchain_to_pylist()` 之前先 `callchain_pylist_init(flags)`。
- **内核功能**：perf_event。

### 7.3 DWARF 用户态栈回溯

- **源码**：`src/dwarf_unwind.c`、`src/dwarf_unwind.h`
- **能力**：用采到的用户态寄存器与栈拷贝做 DWARF 回溯（`CONFIG_LIBUNWIND`）。
- **注意事项**：
  - 通过符号模块的引用计数初始化；没初始化时直接返回 0，退化为 FP 栈。
  - 假定寄存器按 `PERF_REGS_MASK` 采集，attr 必须由框架设置（`prof_dev_env2attr()`、`tp_evsel_new()`）。
  - 不可重入，不缓存，每次回溯都重新查表，热路径开销大；栈拷贝以外的地址读为 0。
- **内核功能**：perf_event。

### 7.4 python 栈

- **源码**：`src/pystack.c`、`src/monitor.h`
- **能力**：跟踪 python 解释器的函数进出，维护每个线程的 python 栈，拼到事件的 callchain 上（`--python-callchain`）。
- **注意事项**：
  - 由框架在 mmap 之后自动 link，打开一个 pystack 设备并与主设备 `order_together()`；需要先用 `perf-prof usdt add` 添加 python 的 USDT。
  - `pystack_perf_event()` 返回共享缓冲，下一个事件覆盖。
  - callchain 末尾追加的 PYSTACK 段里 ips 是 `char *`（unique_string）不是地址，**自己遍历 ips 的代码要识别
    `PERF_CONTEXT_PYSTACK`**。
  - 丢事件后一段时间拿不到 python 栈（cpu 级丢失清空所有栈）。
- **内核功能**：tracepoint / ftrace、perf_event。

---

## 8 其他

### 8.1 kcore：读内核内存

- **源码**：`src/kcore.c`、`src/monitor.h`
- **注意事项**：`kcore_ref()` 失败时静默，之后 `kcore_read()` 一直返回 -1，必须检查返回值；段表在打开时快照；
  不检查跨段读取；读到的是瞬时值，只适合读不变的数据（如指令）。
- **内核功能**：/proc/kcore。

### 8.2 x86 写指令解码（breakpoint）

- **源码**：`src/breakpoint.c`（`x86_decode_insn()` 及其调用的 decode_* 函数）、`arch/x86/lib/insn.c`、`arch/x86/include/asm/insn.h`
- **能力**：数据写断点触发后，从 `/proc/kcore` 读出触发指令，用 `regs_intr` 算出目的地址和写入的值，并反汇编成 AT&T 格式。
- **注意事项**：
  - 函数都是 static，只在 breakpoint 里用；复用要先抽出来。只在 x86 上编译。
  - **数据断点是 trap**，ip 指向下一条指令：取 ip 前 15 字节，找长度正好到 ip 的解码位置，结果按 ip 缓存。
    只能解码内核指令（kcore 不含用户态地址）；缓存假设代码不变（text_poke、livepatch 之后会错）。
  - 寄存器是指令执行之后的值；attr 必须带 `sample_regs_intr = PERF_REGS_MASK`，ABI 为 NONE 时不解码。
  - 只支持目的操作数是内存的 MOV、CMPXCHG、ADD / SUB / OR / AND / XOR、INC / DEC、NOT / NEG、移位 / 循环移位、
    BTS / BTR / BTC，且 opcode 不超过 2 字节；ADC / SBB、XCHG、字符串指令（rep movs）、SSE 等写入解码不出来。
  - **写入的值是推算出来的，不读内存**（内存可能已被改写）：ADD 这类读-改-写指令要基于上一次的值，所以每个断点
    维护一份推算值，直到出现 MOV / CMPXCHG 才可信（safety）。这要求按时间序、连续地看到每次写入：
    开 order、不丢事件；转发来的事件、不支持的指令、DMA 等其他写入都会让推算值失真。
- **内核功能**：perf_event（硬件断点）、/proc/kcore。

### 8.3 eBPF 反汇编

- **源码**：`lib/bpf_disasm.c`、`include/linux/bpf_disasm.h`，使用者 `src/expr.c` 的 `expr_bpf_dump()`
- **能力**：内核的 eBPF 反汇编器（`bpftool prog dump xlated` 的输出格式），打印 eBPF 指令。
- **注意事项**：
  - 原样复制自 Linux v6.6.156 的 `kernel/bpf/disasm.c`，**不要本地修改**，同步时整体替换；
    升级超过 v6.6 要同时更新 `include/uapi/linux/bpf.h` 与 `scripts/bpf_doc.py`（`lib/bpf/` 由它生成 helper 定义）。
    perf-prof 特有的处理写在调用者里。
  - 只在 `CONFIG_LIBBPF` 下编译。
  - `print_bpf_insn()` 一次打印一条指令并自己换行；ld_imm64 占两个下标，调用者要跳过第二半。
    `cb_print` 必填，`cb_call` / `cb_imm` 为 NULL 时打印数值。
- **内核功能**：无（只打印指令）。

### 8.4 杂项

| 能力 | 源码 | 注意事项 |
|---|---|---|
| uprobe 的二进制与库路径 | `src/uprobe_helpers.c/h` | |
| cpu、虚拟化、内核版本 | `src/monitor.c`（`get_cpuinfo()`、`in_guest()`、`kernel_release()`） | `kernel_release()` 只用于无法运行时探测的场合 |
| kvm exit 原因数值 → 名字 | `src/kvm_exit_reason.c` | |
| 不加锁的 localtime | `src/localtime.c/h` | |
