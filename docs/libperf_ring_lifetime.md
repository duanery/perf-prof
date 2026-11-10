# libperf ring 生命周期管理

本文描述 perf-prof 内置 libperf 库如何管理 ring buffer 的创建、共享、引用、轮询与回收，以及它与
perf-prof（external epoll、order、ptrace）之间的接口。

**术语**：文中 **ring** 指一个 perf ring buffer，即源码里的 `struct perf_mmap`。ring 只是文档内的
约定，源码中的类型、函数、变量一律沿用 mmap 命名（`perf_mmap__*`、`perf_evlist__mmap_*`、
`struct perf_mmap_domain`、变量名 `map`）。

---

## 1 背景与问题

### 1.1 核心问题：如何跟踪新创建的线程

用 `-p` 或 workload 跟踪一个多线程进程时，进程会不断创建、销毁线程。perf_event 绑定在线程
上（`perf_event_open(pid=tid)`），新线程需要有自己的 event，否则它的事件采不到。

内核提供 `attr.inherit`：子线程在 fork/clone 时自动继承父线程的 event。它有几个问题：

- **不能用 per-task ring**。c7920614cebb ("perf: Disallow mmap() on per-task inherited events"),
  in v2.6.35，起 `cpu == -1 && attr.inherit` 的 event 不允许 mmap（内核注释：所有子线程写同一个
  ring 会造成性能问题）。因此 perf-prof 的 `--inherit` 只能把 cpus 改为全部 cpu
  （`cpus = env->inherit ? perf_cpu_map__new(NULL) : ...`），每个 evsel 每个 cpu 一个 fd。
- **fork 的开销**。每次 fork/clone，`perf_event_init_task()` 遍历父线程 context 的全部 event，
  `inherit_event()` 为子线程逐个 `perf_event_alloc()`；被跟踪进程的线程创建变慢。
- **用户态感知不到线程的创建与退出**，无法为单个线程建立或回收状态，也无法只跟踪部分线程。

所以 perf-prof 用 ptrace（`PTRACE_O_TRACECLONE` 等）在用户态跟踪线程的创建与退出，再为新线程
打开 event。

### 1.2 重构前的限制

perf-prof 1.11 及之前（a827c64 "perf-prof 1.11"）的 libperf 在 `perf_evlist__open()` / `perf_evlist__mmap()` 之后，fd 和 ring 都是固定的：

- cpu/线程绑定由 evlist 统一决定，ring 按 evlist 的 cpu 下标（`mmap_per_cpu`）或线程下标
  （`mmap_per_thread`）一一分配；
- 打开之后不能增删线程。

因此 ptrace 发现新线程时，只能 **clone 一个完整的 prof_dev**（`prof_dev_clone()`）：新的 evlist、
evsel、ring、order 堆全部复制一份。一个 1000 线程的进程产生 1000 个 prof_dev。同时，ebpf 事件
只能绑 cpu，evlist 级的统一绑定也无法表达"这个 evsel 绑 cpu、那个 evsel 绑线程"。

### 1.3 重构目标

让 evlist/evsel 在打开之后**动态调节**：

- 线程创建时：为每个 evsel 打开该线程的 fd，接入已有 ring 或创建新 ring；
- 线程退出时：把它留在 ring 里的事件收割完整，关闭 fd，回收不再有 writer 的 ring。

evsel 级绑定（`own_cpus` / `own_threads`）和按 `(cpu, tid, overwrite)` 共享 ring 都为此服务。
随之而来的问题就是本文的主题：ring 何时创建、谁撑住映射、何时回收、epoll 注册哪些 fd、线程退出
时如何保证事件不丢。

### 1.4 第一版实现的问题

动态线程的第一版实现（creator 交接 + 每个 writer 注册 epoll + end 快照）存在以下问题，已被本文
的设计替换：

1. **creator 交接在新内核上失败**。ring 的 VMA 依附于创建它的 event；关闭该 event 前，第一版用
   另一个被 `SET_OUTPUT` 重定向的 writer fd 重新 mmap 同一个 ring，再 munmap 原映射。被重定向的
   event 自身 `mmap_count == 0`：
   - 448f97fba901 ("perf: Convert mmap() refcounts to refcount_t"), in v6.18，起该路径触发
     refcount WARN；
   - d06bf78e55d5 ("perf: Fix refcount warning on event->mmap_count increment"), in v6.19，
     `perf_mmap_rb()` 直接返回 `-EBUSY`（带 Fixes，会回合到 stable）。

   交接失败后线程永远无法回收，死线程无限堆积。
2. **每个 writer fd 都注册 epoll**。一个 ring 有 N 个 writer 时，每次唤醒内核对 N 个 waitq 调用
   `ep_poll_callback`，但 `rb->poll` 是 xchg 语义，只有一个 fd 拿得到 EPOLLIN；epitem 内存与线程
   增删时的 `epoll_ctl` 也随 writer 数线性增长。
3. **ring 管理不内聚**。分配、引用、`SET_OUTPUT`、epoll 注册、creator 交接散落在 evlist.c 多个函数
   里；perf-prof 侧的线程回收需要了解 libperf 内部的交接细节，失败时只能重试。
4. **线程退出只由 ptrace 驱动**，用读指针快照（end）近似"该线程的事件已全部写入"的边界。

---

## 2 内核事实

本设计依赖以下内核行为（以 mainline 和 5.4 源码核对）。

| # | 事实 | 位置 / 引入 |
|---|---|---|
| K1 | VMA 持有 `vma->vm_file`。`close(fd)` 不释放 event；event 未 DISABLE 时继续写入 | `perf_mmap_close()` 只在 munmap 时调用 |
| K2 | ring 的最后一个 VMA 被 munmap 时，所有 `SET_OUTPUT` 过来的 writer 被 `ring_buffer_attach(event, NULL)` 摘掉，之后静默丢事件 | `perf_mmap_close()` 的 `detach_rest` |
| K3 | 被重定向的 writer 随时可以关闭，它已写入的数据留在 ring 里 | `free_event()` 只摘除自己 |
| K4 | `SET_OUTPUT` 的目标可以是 ring 上任一 event，约束：同 cpu；cpu == -1 时须同一 task；clock 一致；write_backward 一致；自身不能已有 mmap | `perf_event_set_output()` |
| K5 | 对被重定向（自身没有 mmap）的 event 再 mmap，v6.18 起触发 WARN，v6.19 起返回 `-EBUSY` | 448f97fba901, d06bf78e55d5 |
| K6 | 任一 writer 写入都会唤醒 ring 上**所有** event 的 waitq；`perf_poll()` 读的是 ring 级的 `rb->poll` | `ring_buffer_wakeup()`，10c6db110d0e ("perf: Fix loss of notification with multi-event"), in v3.2 |
| K7 | task 退出时，其 task context 内的所有 event（包括已 DISABLE 的）置为 `PERF_EVENT_STATE_EXIT`，`perf_poll()` 返回 EPOLLHUP；有子 event（inherit）时等子 event 全部退出 | `perf_event_exit_event()` → `DETACH_EXIT`；179033b3e064 ("perf: Add PERF_EVENT_STATE_EXIT state for events with exited task"), dc633982ff3f ("perf: Do not POLLHUP event if it has children"), in v3.18 |
| K8 | EPOLLHUP 无法用事件掩码屏蔽，HUP 的 fd 留在 epoll 里会一直就绪 | epoll 语义 |
| K9 | 已 close 但 file 仍被引用（例如被 VMA 持有）的 fd 仍留在 epoll 里；file 最终释放时 epitem 才自动摘除 | epoll 在 `__fput` 时清理 |
| K10 | `do_exit()` 中 `perf_event_exit_task()` 早于 `exit_notify()`：ptrace（未开 `PTRACE_O_TRACEEXIT`）看到线程退出时，该线程 task-bound event 的事件已全部写入 ring | `kernel/exit.c` |
| K11 | 已退出线程的 event 只通过 `event->hw.target` 持有 task_struct 引用（`perf_event_alloc()` 中 `get_task_struct()`，`_free_event()` 释放），不持有 `struct pid`；waitpid 照常 `release_task()`，pid 可被复用 | `kernel/events/core.c` |
| K12 | perf trace event 过滤器只能设置一次（`-EEXIST`），BPF 程序同样只能挂一次 | `ftrace_profile_set_filter()`；`perf_event_attach_bpf_prog()` |
| K13 | 没有 ring 的 event（既没有自己的 mmap，也没有 `SET_OUTPUT` 到别的 ring），`perf_poll()` 直接返回 EPOLLHUP；`ring_buffer_wakeup()` 也只唤醒 `rb->event_list` 上的 event | `perf_poll()` 初值 `events = EPOLLHUP`，有 rb 才读 `rb->poll`；61b67684c4a4 ("perf: Fix perf_poll to return proper POLLHUP value"), in v3.18 |

简化假设：默认内核支持 HUP（v3.18+）。

---

## 3 模型：fd 空间、ring、HUP 域、线程、监听 fd

### 3.1 从 fd 空间到监听 fd

**fd 空间 (event,c,t)**：每个打开的 writer fd 记为 (event,c,t)。event 是 evsel，c、t 是 evsel 的
cpu map、thread map 中这个 fd 所在槽位的值：

- c = -1：evsel 不绑 cpu（dummy cpu map）；
- t = -1：pid=-1 的 fd；cgroup fd 的 t 是 cgroup 而不是 task，同样按 t = -1 处理；
- (event,-1,-1) 不存在，fd 至少绑 cpu 或 task 之一。

**ring 空间**：fd 按 c 分量选择写入的 ring：

- c ≥ 0 → cpu ring (c,-1)：`cpu=c` 的所有 fd，不论 t 是什么；
- c = -1 → thread ring (-1,t)。

**HUP 域空间 (c,t)**：fd 再按 t 分量选择它在 ring 上的 HUP 域（`struct perf_mmap_domain`），
域内的 fd 一起 HUP：

- t = -1 → cpu 域 (-1)，即 `map->cpu_domain`，永远不 HUP；
- t ≥ 0 → task 域 (t)，task t 退出时域内所有 fd 同时 HUP（K7）。

最后 fd 按 event 分量放进域内（`dom->fds[]`）。

于是 (c,t) 唯一确定一个域：

| (c,t) | 所在 ring | 域 | 对应关系 |
|---|---|---|---|
| (c,-1) | cpu ring (c,-1) | cpu 域 | ring 存在时 cpu 域不一定存在：ring 上可以只有 task 域 |
| (c,t) | cpu ring (c,-1) | task 域 (t) | 一个 cpu ring 可以有多个 task 域 |
| (-1,t) | thread ring (-1,t) | task 域 (t) | 与 ring 一一对应：ring 有 writer 时这个域就存在 |

ring 的登记键还含 overwrite 分量（`attr.write_backward`）：overwrite 与非 overwrite 是两套独立的
ring 与域，规则相同，下文省略。

(event,c,-1)、(event,c,t)、(event,-1,t) 可以在同一个 evlist 里共存：例如 task-state 的 pid=-1
事件、`cpus=` + `--ptrace` 的事件、不绑 cpu 的 `--ptrace` 事件。此时线程 t 同时拥有若干 (c,t) 域
（挂在各个 cpu ring 上）和一个 (-1,t) 域（挂在自己的 thread ring 上）。

**线程（`struct perf_evlist_thread`）**：只有线程退出才会 HUP，所以 t ≥ 0 的域再关联到线程 t 上，
`th->domain_list` 串起线程 t 的所有 (c,t) 与 (-1,t) 域。线程退出时这些域同时离开各自的 ring。

**监听 fd**：注册进 epoll 的 fd。它不是另一个空间，而是在上面三层之上的两次选择，结果落回域里：

- 每个 ring 从它的域中选一个承担**数据角色**（`map->poll_domain`，EPOLLIN）；
- 每个线程从它的 task 域中选一个承担 **HUP 角色**（`th->hup_domain`，只等 EPOLLHUP）。cpu 域不属于
  任何线程，只参与数据角色的选择；
- 被选中的域从自己的 fd 中任取一个注册（`dom->poll_fd`），同时承担两种角色时共用这一个 fd。

即：监听 fd 集合 = { 被 ring 或线程选中的域 } 中每个域的一个 fd，注册数等于被选中的域数。

于是一个 fd (event,c,t) 的完整映射是：

```
(event,c,t) ──c──▶ ring (c,-1) 或 (-1,t)
            ──t──▶ 域 (c,t)，t ≥ 0 时再挂到线程 t
            ──event──▶ 域内的一个 fd
域 (c,t) 被 ring 选为数据角色、或被线程 t 选为 HUP 角色 ──▶ 域内一个 fd 注册进 epoll
```

**owner**：ring 的 VMA 所依附的 event，即创建 ring 的那个 fd。

### 3.2 为什么这样抽象

三个分量的划分顺序 c → t → event，每一步都对应一条内核约束；分量之外的东西（evsel、fd 号）不影响
ring 的共享与生命周期。

**c 分量决定 ring：由 `SET_OUTPUT` 与 mmap 的约束推出。**

- 一个 fd 只能把输出重定向到同 cpu 的 ring；cpu == -1 时只能重定向到同一 task 的 ring（K4）。所以
  c ≥ 0 的 fd 能共享的最大集合是"同一个 c 的所有 fd"，不论 t —— 这就是 (c,-1)；c = -1 的 fd 只能
  在同一个 t 内共享 —— 这就是 (-1,t)。ring 不能比这更大，也没有理由更小。
- 更小的划分（例如每个 (c,t) 一个 ring）会产生 ncpu × 线程数个 ring，每个都占 mmap 页、都要进
  order 堆；按 c 共享只有 ncpu 个。
- per-task 的 inherit event 不能 mmap（c7920614cebb），所以 c = -1 时也只能每个 task 一个 ring。

**t 分量决定 HUP 域：由线程退出的语义推出。**

- task 退出时，它的所有 event 一起进入 EXIT 并 HUP（K7），不分 cpu、不分 evsel；pid=-1 与 cgroup
  的 event 永远不 HUP。所以 t 把一个 ring 的 writer 按"随谁一起消失"分组：同 t 的 fd 一定同时
  消失，不同 t 的 fd 互不影响。
- ring 的 writer 集合只会整组变化。引用计数因此以域为单位而不是以 fd 为单位：域存在就持有 ring
  的一个引用，域内 fd 的增减不影响 ring 的生命周期；ring 失去最后一个域才失去 writer。
- 第一版以 fd 为单位维护 writer（`nr_fds`）并要求 creator fd 交接，正是把同一组里本来等价的 fd
  区别对待的结果（1.4）。

**event 分量只决定域内的哪个 fd：它与共享和生命周期都无关。**

- 同一个 (c,t) 下不同 evsel 的 fd，写同一个 ring（K4 只看 c、t），随同一个 task 退出（K7），被
  同一次写入唤醒（K6）。对 ring 管理而言它们完全等价，只在 sample id 和 thread map 槽位上有区别，
  那属于 evsel 的事。

**域再挂到线程上：因为 HUP 和 ptrace 都以线程为单位。**

- 一个线程的域分布在多个 ring 上（各 cpu ring 的 (c,t)、自己的 (-1,t)），但 K7 让它们在同一时刻
  失效：收到其中任何一个 fd 的 HUP，就知道全部失效。`th->domain_list` 让一次 HUP 处理线程的所有域，
  而不必等每个 ring 各自 HUP。
- ptrace 报告的也是"线程 t 退出"。两个信号的状态机（7.2）因此放在线程记录上。

**监听 fd 落在域里：逐条由内核约束推出。**

1. *候选只能是域内的 fd。* fd 接入 ring（mmap 或 `SET_OUTPUT`）之前，poll 它立即返回 EPOLLHUP，
   也收不到写入唤醒（K13）。所以能监听的只有已经接入 ring 的 writer fd，而接入 ring 的 fd 必然已按
   t 分量放进了某个域。监听 fd 因此只能从域内选，注册必须发生在 fd 入域之后。
2. *要监听两件事，单位不同。* 读数据要 EPOLLIN，唤醒以 ring 为单位：ring 上任一 writer 写入会唤醒
   ring 上所有 event（K6），每个 ring 一个 fd 就够。得知退出要 EPOLLHUP，HUP 以线程为单位：线程的
   所有 event 同时 EXIT（K7），每个线程一个 fd 就够。只按 ring 选会漏掉线程：(event,c,-1) 与
   (event,c,t) 共存时数据 fd 在 cpu 域上，它永不 HUP，线程 t 的退出就收不到。
3. *选择的粒度是域，不是 fd。* 同一域内的 fd 同 ring（对 K6 等价）且同 task（对 K7 等价），对两种
   事件都完全等价；不同域则至少在一种事件上不等价。所以 ring 与线程都只需要选域，域内任取一个 fd。
4. *结果记在域上，不记在 ring 或线程上。* ring 的域集合与线程的域集合的交点是域，同一个域可能同时被
   ring 和线程选中。把注册的 fd 记在域上（`dom->poll_fd`），一个 fd 就能兼任两种角色，不会因为两个
   选择各注册一次而重复；注册的生死也随域：域死亡注销，域内 fd 关闭只在域内换一个 fd。

由此得到几条推论：

- 只有域死亡（线程 HUP，或域内最后一个 fd 离开）才转移角色；fd 的增减几乎不触发 epoll 操作。
- 数据角色优先 cpu 域：它永不 HUP，选中后不会转移。HUP 角色优先与数据角色重合，减少注册数。
- 每个存活线程至少占一个注册 fd（HUP 角色），退出时它的域全部死亡，至少注销一个；数据角色只会转移到
  已注册或新注册的域上而不增加总数。所以没有 cpu 域时，注册数归 0 就等于被跟踪的线程全部退出，
  不依赖 ptrace 即可主动结束（6.3）。

### 3.3 关联一览

| 关联 | 正向 | 反向 |
|---|---|---|
| fd ↔ 域 | `entry->domain` | `dom->fds[]` |
| 域 ↔ ring | `dom->map`（每个存活域持有 ring 的一个引用） | `map->domain_list`、`map->cpu_domain` |
| 域 ↔ 线程（t ≥ 0） | `dom->thread` | `th->domain_list` |
| 数据角色（ring → 域） | `map->poll_domain` | 域上的 `dom->poll_fd` |
| HUP 角色（线程 → 域） | `th->hup_domain` | 同上 |
| 监听 fd（域 → fd → epoll） | `dom->poll_fd`、`dom->poll_events`，`epoll.poll_fd_heads[]` 按 fd 反查域 | `perf_evlist_poll__find(evlist, fd)` |
| ring 登记 | evlist 查找哈希 `mmap_heads[]` + 遍历链表 `mmap_list` / `mmap_ovw_list` | |
| 线程登记 | evlist `thread_heads[]`（`struct perf_evlist_thread`） | |
| fd ↔ evsel | `evsel->fd_heads[]` 哈希（按 (c,t) 值）；按 cpu 挂在 `struct perf_evsel_cpu` 的 `fd_list`、按线程挂在 `struct perf_evsel_thread` 的 `fd_list`；内嵌的 sample id（挂 evlist `id_heads[]`）；`entry->leader` | `perf_evsel__fd_find(evsel, c, t)` |
| cpu / 线程 ↔ evsel | `evsel->cpu_list`（按 cpu 升序）+ `cpu_heads[]`、`evsel->thread_list` + `thread_heads[]`；evsel 对每条记录持有一个引用，每个 fd 也持有一个，见 7.5 | `perf_evsel__cpu_find()` / `perf_evsel__thread_find()` |

### 3.4 抽象与源码的对应

**对象**

| 抽象 | 源码 |
|---|---|
| fd (event,c,t) | `struct perf_evsel_fd`：event = `entry->evsel`，c = `entry->cpu`，t = `entry->tid`（cgroup fd 时为 cgroup fd，`entry->cgroup` 置位），按值用 `perf_evsel__fd_find(evsel, c, t)` 查到 |
| ring (c,-1) / (-1,t) | `struct perf_mmap`，键 `map->cpu` / `map->tid` / `map->overwrite`；`perf_mmap__oncpu()` 区分两类 |
| 域 (c,t) | `struct perf_mmap_domain`：c = `dom->map->cpu`（thread ring 为 -1），t = `dom->tid` |
| cpu 域 (-1) | `map->cpu_domain`，`dom->tid == -1`，`dom->thread == NULL` |
| 线程 t | `struct perf_evlist_thread`，`th->tid`；退出状态 `th->hup` / `th->del` |
| 数据角色 / HUP 角色 | `map->poll_domain` / `th->hup_domain`；注册的 fd 与事件掩码 `dom->poll_fd` / `dom->poll_events` |
| 域的生死 | `dom->dead`；死亡后 `dom->map == NULL`，但 `dom->fds[]`、`dom->thread` 保留到 fd 关闭 |
| ring 是否登记 | `map->registered`（哈希 + 链表），`hlist_unhashed(&map->hnode)`（是否可被新 writer 找到） |

**映射步骤**（`perf_evlist__mmap_attach_fd()`）

| 抽象步骤 | 源码 |
|---|---|
| c 分量 → ring 键 | `(bind_cpu, bind_tid)`：c ≥ 0 为 (c,-1)，c = -1 为 (-1,t) |
| 找到 / 创建 ring | `perf_evlist__find_mmap()` → `SET_OUTPUT` 到 `perf_evlist__mmap_output_fd()`；或 `perf_evlist__mmap_create()` + `perf_evlist__mmap_register()` |
| t 分量 → 域 | `tid = (entry->tid < 0 \|\| entry->cgroup) ? -1 : entry->tid`，`perf_evlist__mmap_domain_get(evlist, map, tid)`；t ≥ 0 时经 `perf_evlist__thread_get()` 挂到线程 |
| event 分量 → fd 入域 | `perf_mmap_domain__add_fd()`，`entry->domain = dom` |
| 选择角色 | `perf_evlist_poll__update()` → `perf_evlist_poll__pick_data()` / `perf_evlist_poll__pick_hup()` → `perf_evlist_poll__sync()` → `perf_evlist_poll__add()` / `perf_evlist_poll__del()` |

**解除关联**

| 抽象 | 源码 |
|---|---|
| 线程 HUP（7.3） | `perf_evlist__hup()`：`th->hup = true`，对每个存活域 `perf_evlist__reap_domain()` → `perf_evlist__mmap_domain_put(evlist, dom, &m->map)`；`perf_evlist__reap_done()` 放引用 |
| 域离开 ring | `perf_evlist__mmap_domain_put()`：`perf_evlist_poll__del()`、`list_del_init(&dom->map_node)`；ring 没有域时 `perf_evlist__mmap_unregister()` |
| del 先到（7.2） | `perf_evlist__del_thread()`：`perf_evsel__disable_thread()`、`perf_evlist__mmap_unhash()`、`th->del = true` |
| 关闭 fd（7.4） | `perf_evlist__thread_close()` → `perf_evsel__del_thread()` → `perf_evsel__close_one()` → `perf_evlist__mmap_detach_fd()`：`perf_evlist__id_del()`、`perf_mmap_domain__del_fd()`，死域空了 `perf_evlist__mmap_domain_free()`；线程记录 `perf_evlist__thread_free()` |
| 无 HUP 时最后一个 fd 离开存活域 | `perf_evlist__mmap_detach_fd()` 中 `!dom->dead && dom->nr_fds == 1` → `perf_evlist__mmap_domain_put(evlist, dom, NULL)` |
| cpu 删除：停止 + 收割（7.6） | `perf_evlist__del_cpu()`：`c->del = true`、`perf_evsel__disable_cpu()`；`perf_evlist__reap_mmap()` 对 (c,-1) ring 用 `perf_evlist__mmap_stopped()` 计数，停止的 fd 是全部 writer 时 `perf_evlist__mmap_retire()` |
| cpu 删除：完成（7.6） | `perf_evlist__reap_done()` → `perf_evlist__cpu_close()` → `perf_evsel__del_cpu()` → `perf_evsel__close_one()` → `perf_evlist__mmap_detach_fd()` |
| evsel 增加（7.7） | `perf_evsel__set_leader()` → `perf_evsel__open()`；`perf_evlist__add_evsel()`：`evsel->joined`、`perf_evlist__mmap_attach_fd()`、`perf_evsel__enable()` |
| evsel 删除（7.8） | `perf_evlist__del_evsel()`：`evsel->del = true`、DISABLE、`perf_evlist__reap_mmap()`；`perf_evlist__reap_done()` → `perf_evlist__evsel_leave()` → `perf_evlist__mmap_detach_fd()`、`perf_evlist__remove()` |
| munmap | `perf_evlist__munmap()`：`evlist->munmapping = true`，先 `perf_evlist_poll__del()` 全部域，再逐个 `perf_evlist__mmap_detach_fd()` |
| ring 释放 | `perf_mmap__put()` 归 0 → munmap + free（`lib/perf/mmap.c`） |

---

## 4 绑定与共享

- evsel 的 cpus / threads 可以覆盖 evlist 的默认值（`perf_evsel__set_own_cpus()` /
  `perf_evsel__set_own_threads()`），决定这个 evsel 的 fd 落在 fd 空间的哪些 (c,t) 上。
- `perf_evlist__mmap_attach_fd()` 处理一个 fd (event,c,t)：
  1. 由 c 得到 ring 键：查到 ring 就 `ioctl(fd, PERF_EVENT_IOC_SET_OUTPUT, ring 上任一存活 writer)`
     接入（K4）；查不到就 `perf_mmap__new()` + `perf_mmap__mmap(fd)` 创建并登记，fd 成为 owner；
  2. 由 t 得到域：取 ring 上的域 (t)，不存在就创建（取 ring 的一个引用，t ≥ 0 时挂到线程上）；
  3. fd 加入域，然后更新 ring 的数据角色、线程的 HUP 角色。
- `rb->watermark` 由 owner 的 attr 决定，所以 `perf_evlist__mmap()`、`perf_evlist__add_thread()` 与
  `perf_evlist__add_cpu()` 都分两轮：先处理 `attr.watermark` 的 evsel（创建 ring），再处理其余 evsel（`SET_OUTPUT` 接入）。

---

## 5 引用与生命周期

### 5.1 ring 的引用

`perf_mmap__new()` 返回时引用为 1（创建者持有，ring 的第一个域建立后释放）。之后的持有者：

| 持有者 | 获取 | 释放 |
|---|---|---|
| 每个存活的域 | 域创建 | 域死亡：线程 HUP、cpu 或 evsel 删除使 ring retired，或域内最后一个 fd detach |
| `struct perf_evlist_reap` 的 `maps[]` | 收割：`perf_evlist__hup()`（由死亡的域转交）、`perf_evlist__del_cpu()` / `del_evsel()`（另取一个引用） | `perf_evlist__reap_done()` |
| perf-prof order 堆 | `order_mmap_add()` | `order_mmap_del()` / `order_deinit()` |

`perf_mmap__put()` 归 0 时 munmap 并 free，**不访问 evlist**：ring 失去 writer 时已经从 evlist
摘除，此后 evlist 可能先于 ring 销毁。

### 5.2 失去 writer 与释放

两者分开：

- **失去 writer**：ring 的最后一个域死亡 → 从 evlist 登记表（哈希 + 链表）摘除（retired）。之后
  不会再有新事件，也不会被新 writer 找到，但只要还有引用就仍可读。
- **释放**：最后一个引用放下 → munmap。

ring 的域有三种死法：线程 HUP；cpu 或 evsel 删除时 ring 的 writer 全部被停止，整个 ring retired
（7.6、7.8）；或域内最后一个 fd 被 detach（`perf_evsel__del_thread()` / `del_cpu()`、
`perf_evsel__close()`、`perf_evlist__del_evsel()` 的完成、`perf_evlist__munmap()`）。

### 5.3 owner 与 fd 的关系

VMA 持有 owner 的 file（K1），ring 的映射不依赖任何 fd 保持打开：

- owner fd 被 close 后 owner event 仍由 VMA 撑住；它在 close 前已 DISABLE，不再写入；
- 新 writer 的 `SET_OUTPUT` 目标取 ring 上任一存活 writer（K4），不需要 owner fd；
- 因此不需要 creator 交接（K5 使其不可行），也不需要在 libperf 里 dup 一个 fd。

owner 可能是一个已退出线程的 event，它只持有 task_struct 的内存（K11），不阻碍进程回收，数量
不超过 ring 数。owner 若是 group leader，close 后仍由 VMA 撑住，组成员留在它的组里。

---

## 6 epoll

### 6.1 数据角色与 HUP 角色

**fd 只能在接入 ring 之后注册。** 一个 writer fd 在 mmap（成为 owner）或 `SET_OUTPUT`（接入已有
ring）之前没有 ring：此时 poll 它立即返回 EPOLLHUP，也收不到任何写入唤醒（K13）。若先注册再接入，
EPOLLHUP 会被误当作线程退出。所以注册只发生在 `perf_evlist__mmap_attach_fd()` 把 fd 放进域之后，
由 `perf_evlist_poll__update()` 完成；`perf_evlist__open()` 打开 fd 时不注册。外部 loop 的回调参数
`map` 也只有此时才确定。


- **数据角色（每个 ring 一个域）**：注册 EPOLLIN。由 K6，一个 fd 足以收到 ring 上所有 writer 的
  唤醒。优先 cpu 域（不会 HUP），没有 cpu 域时选一个 task 域，优先选同时承担 HUP 角色的那个。
- **HUP 角色（每个线程一个域）**：只等 EPOLLHUP。由 K7，线程的所有 event 同时 EXIT，一个 fd 足以
  得知线程退出。优先选已承担数据角色的域，使一个 fd 兼任两者。

一个域承担任一角色就注册一个 fd，只有承担数据角色时带 EPOLLIN；overwrite ring 不带 EPOLLIN。
只有 task 域的注册带 EPOLLHUP。EPOLLHUP 无法屏蔽（K8），写进事件掩码不改变内核行为，它只是标记：
告诉外部 loop 这个 fd 会在线程退出时 HUP，cpu 域（pid=-1、cgroup）的 fd 永远不会（6.3）。

system wide 的 evsel 也参与角色选择，它的 fd 落在 cpu 域，可以承担数据角色。只有 system wide
evsel 写入的 ring 因此也会被 poll。上游 perf 最初不 poll system wide 的 fd，原因是它们永不 POLLHUP，
会妨碍"全部任务退出"的判断：f90d194a867a ("perf evlist: Do not poll events that use the
system_wide flag"), in v3.19；后来改为 poll，但不计入存活数：6cc447964555 ("libperf evlist: Fix
polling of system-wide events"), in v6.0。这里由 EPOLLHUP 标记让外部 loop 做同样的区分。

HUP 角色不能省：(event,c,-1) 与 (event,c,t) 共存时数据角色在 cpu 域上，task 域的 fd 若不单独注册，
线程 t 退出的 EPOLLHUP 永远收不到。

| 场景 | 注册的 fd |
|---|---|
| 只有 (event,-1,t) | 每个线程 1 个，兼任 (-1,t) 的数据角色和 t 的 HUP 角色 |
| 只有 (event,c,-1) | 每个 cpu ring 1 个 |
| 只有 (event,c,t)，N 个线程 | 每个 cpu ring 1 个数据 fd + 每个线程 1 个 HUP fd（尽量与数据 fd 重合） |
| (event,c,-1) 或 system wide 与 (event,c,t) 共存 | 每个 cpu ring 1 个 cpu 域数据 fd（不带 EPOLLHUP）+ 每个线程 1 个只等 EPOLLHUP 的 fd |
| 再加 (event,-1,t) | 线程 t 的 HUP 角色优先落在 (-1,t) 上，与该 thread ring 的数据 fd 共用 |

### 6.2 角色的维护

域的 fd 增删、域死亡时，`perf_evlist_poll__update(evlist, map, th, dom)` 重选 `map` 的数据域
（`perf_evlist_poll__pick_data()`）与 `th` 的 HUP 域（`perf_evlist_poll__pick_hup()`），当前选择
仍有效就保持；只对获得或失去角色的域增删注册（`perf_evlist_poll__sync()`）。

- 承担数据角色的域死亡时，另一个存活域接替；若它原本只以 HUP 角色注册（无 EPOLLIN），先注销再
  带 EPOLLIN 重新注册。例如 (event,c,t1) 创建的 (c,-1) ring，t1 退出、t2 仍存活：数据角色转到
  (c,t2)。t1 的 fd 关闭后 ring 照常工作：VMA 撑住 owner（K1），t2 写入唤醒新的数据 fd（K6），
  后来的线程 `SET_OUTPUT` 到任一存活 writer（K4）。
- 被注册的 fd 关闭而域仍存活时，换同一个域的另一个 fd：同域的 fd 同时 HUP，这不是角色转移。
- 线程 HUP 时先置 `th->hup`，它的域依次死亡时不再为它重选 HUP 域。
- `perf_evlist__munmap()` 先一次性注销全部 fd 并置 `evlist->munmapping`，之后 detach fd 时只删
  不加——否则每 detach 一个 fd 就把角色交给下一个 fd，与 evsel 的添加、拆除顺序无关。
- 注销必须发生在 fd 关闭之前：VMA 可能撑住 file，fd 关了 epitem 仍在（K9）。

### 6.3 EPOLLHUP 注册数与全部线程退出

每个存活线程至少有一个带 EPOLLHUP 的注册（HUP 角色），线程 HUP 时它的所有域死亡，这些注册随之
注销；数据角色可能转移到另一个线程已注册的 fd 上，也不会多出带 EPOLLHUP 的注册。所以**最后一个
线程退出后，带 EPOLLHUP 的注册数归 0**，与有没有 cpu 域（(event,c,-1)、system wide）无关：cpu 域的
注册不带 EPOLLHUP。

外部 loop 从 add 回调的 `events` 得知一个 fd 是否带 EPOLLHUP，del 回调收到同一个 `events`，据此计数
就能判断被跟踪的线程是否全部退出，**不需要 ptrace**。perf-prof 的 `dev->nr_pollfd` 按跟踪方式计数
（`prof_dev_pollfd_counts()`）：

- 跟踪任务（`-p`、`-t`、workload）：只计带 EPOLLHUP 的 fd，任务全部退出时归 0；
- 跟踪 cpu 或 cgroup：所有 fd 都计，没有会退出的任务，注册数不会归 0。

perf-prof 据此主动关闭 prof_dev（8.1）：

| 跟踪方式 | 关闭依据 |
|---|---|
| 不用 ptrace（`-p`、无 `--ptrace` 的 workload） | EPOLLHUP 处理后 `dev->nr_pollfd` 归 0 → `prof->hangup()` + `prof_dev_close()` |
| 用 ptrace（`--ptrace`） | 每个被跟踪线程通过 ptrace 链接持有一次 `prof_dev_use()`，线程退出时 `prof_dev_unuse()`；`dev->dev_users` 归 0 时关闭。此时 `dev->ptrace_list` 非空，不按 `nr_pollfd` 关闭 |

### 6.4 external epoll 接口

```c
typedef int  (*perf_evlist_poll_add_t)(void *external, int fd, unsigned events,
				       struct perf_mmap *map, bool new_map);
typedef void (*perf_evlist_poll_del_t)(void *external, int fd, unsigned events,
				       struct perf_mmap *map);

void perf_evlist_poll__external(struct perf_evlist *evlist, void *external,
				perf_evlist_poll_add_t add, perf_evlist_poll_del_t del);
```

- 注册哪些 fd 完全由 libperf 决定，从 `perf_evlist__mmap()` 起直接调用 add/del；外部 loop 不需要
  遍历 fd。
- `new_map` 在某个 ring 第一次被报告时为 true，perf-prof 在此把 ring 加入 order 堆。
- `events` 只在 task 域的 fd 上带 EPOLLHUP；del 收到该 fd 注册时的 `events`，外部 loop 可以据此
  维护 EPOLLHUP fd 的计数（6.3）。
- 外部 loop 收到 EPOLLHUP 时调用 `perf_evlist__hup()`。
- 没有设置 external 时 evlist 用自己的 epoll，`perf_evlist__poll_mmap()` 内部处理 HUP。

---

## 7 线程、cpu 与 evsel 增删

`perf_evlist__open()` 之后 evlist 与 evsel 的输入 cpu map / thread map 都不再变化，之后只能逐个
增删线程、cpu 或 evsel。删除 writer——线程、cpu、evsel——统一按 **停止 → 收割 → 完成** 三步：

1. **停止**：被删除的 writer 不再产生事件：`DISABLE`，或内核已停止它们（task 退出）；
2. **收割**：libperf 把它们写过的每个 ring 连同一个引用交给调用方（`struct perf_evlist_reap`），
   此时 fd 仍未关闭，调用方把 ring 读到末尾，sample id 仍可解析出 evsel；
3. **完成**：`perf_evlist__reap_done()` 释放收割得到的 ring 引用，再由 libperf 释放剩下的结构
   （关闭 fd、释放线程记录等）。

线程的停止是 `perf_evlist__del_thread()`（或线程退出本身），收割是 EPOLLHUP 触发的
`perf_evlist__hup()`，二者顺序不定（7.2）；cpu 的停止与收割都在 `perf_evlist__del_cpu()` 里一次完成
（7.6），evsel 的在 `perf_evlist__del_evsel()` 里（7.8）。

### 7.1 增加

`perf_evlist__add_thread(evlist, tid)`：对每个跟随 evlist 线程表的 evsel 打开 fd（按
`perf_evlist__open_order()`，group leader 在前），按 watermark 优先的两轮 attach（第 4 节），最后统一 enable。若同一 tid 还有一个尚未完成
退出的旧线程（tid 复用），先把旧线程彻底关闭（7.4）。

### 7.2 退出：两个信号

线程退出有两个信号，到达顺序不确定：

- **del**：ptrace 看到线程退出，调用 `perf_evlist__del_thread()`；
- **HUP**：内核让该线程的 fd 返回 EPOLLHUP，外部 loop 调用 `perf_evlist__hup()`。

HUP 解除"域 ↔ ring"一侧的关联（7.3），关闭 fd 解除"fd ↔ 域 ↔ 线程、fd ↔ evsel"一侧的关联
（7.4）。**fd 由后到者关闭**：

| 调用 \ 状态 | 未标记 | 已有 HUP | 已有 del |
|---|---|---|---|
| `perf_evlist__hup()` | 7.3；不关 fd | 返回 `PERF_EVLIST_REAP__NONE` | 7.3，并置 `complete`；`perf_evlist__reap_done()` 执行 7.4 |
| `perf_evlist__del_thread()` | 对该线程所有 fd `DISABLE`；(-1,t) ring 移出查找哈希（tid 可能复用，旧 ring 上 `SET_OUTPUT` 会 `EINVAL`），仍留在遍历链表中可读；置 `th->del`；HUP fd 留在 epoll；返回 0 | 7.4，返回 1 | 返回 0 |

- 线程没有任何存活的 (·,t) 域（例如 task-state + `-p`，全是 (event,c,-1)）：del 直接执行 7.4 并
  返回 1。
- fd 在两个信号都到之前不关闭，epoll 里注册的始终是有效的 fd，不存在 fd 号复用或 HUP 丢失。
- DISABLE 不影响 HUP（K7）。当前 ptrace 未开 `PTRACE_O_TRACEEXIT`，del 到来时 event 已经 EXIT，
  DISABLE 是保险；开启 TRACEEXIT 后就是必需的。

### 7.3 线程 HUP：收割，解除域与 ring 的关联

线程退出时它的所有 event 同时 EXIT（K7），第一个 HUP 就处理线程 t 的全部 (c,t)、(-1,t) 域：

1. 置 `th->hup`，此后不为 t 重选 HUP 角色；
2. 对每个存活域：
   - 注销 `dom->poll_fd`，从 `map->poll_domain` / `th->hup_domain` 中清除；
   - 从 `map->domain_list` 摘下，域标记为 dead；它的 ring 引用不释放，转入 `reap->maps[]`；
   - ring 上没有域了 → ring 从 evlist 登记表摘除（retired）。(-1,t) 一定 retired；(c,-1) 只在没有
     其他线程的域、也没有 cpu 域时 retired；
   - 否则为该 ring 重选数据角色。

**保留**：`dom->fds[]` / `entry->domain`（fd 未关闭）、`th->domain_list`（死亡的域仍挂在线程上）、
sample id（收割事件时仍要映射回 evsel）、thread map 槽位。

`perf_evlist__hup()` 的结果是一次收割：

```c
enum perf_evlist_reap_type {
	PERF_EVLIST_REAP__NONE,     /* 没有收割：该 fd 不属于退出中的线程，如第二次 HUP */
	PERF_EVLIST_REAP__THREAD,   /* 线程 tid 退出 */
};

struct perf_evlist_reap {
	enum perf_evlist_reap_type type;
	pid_t tid;          /* PERF_EVLIST_REAP__THREAD 的线程，否则 -1 */
	bool complete;      /* 删除已完整：perf_evlist__reap_done() 后不复存在（线程：del 已先到） */
	int nr_maps;
	struct perf_evlist_reap_map {
		struct perf_mmap *map;  /* 被删除的 writer 写过的 ring，带一个引用 */
		bool retired;           /* 已没有 writer，已从 evlist 摘除 */
	} *maps;
};
int  perf_evlist__hup(struct perf_evlist *evlist, int fd, struct perf_evlist_reap *reap);
void perf_evlist__reap_done(struct perf_evlist *evlist, struct perf_evlist_reap *reap);
```

调用方在 `reap_done()` 之前把 `maps[]` 读到末尾：此时 fd 未关闭、sample id 仍可解析出 evsel；
`retired` 的 ring 不会再有新事件，读完就是全部。`reap_done()` 释放 `maps[]` 的引用，ring 是否
munmap 取决于 order 堆是否还持有它；再按 `type` 完成删除（线程：`complete` 时执行 7.4）。
`PERF_EVLIST_REAP__NONE` 的结果也交给 `reap_done()`。后续删除 cpu、evsel 时新增 `type`，
沿用同一个结构与 `reap_done()`。

### 7.4 关闭 fd：解除 fd、域、线程、evsel 的关联

两个信号都到达后，`perf_evlist__thread_close()` 对每个跟随 evlist 线程表的 evsel 调用
`perf_evsel__del_thread()` → `perf_evsel__close_one()` → `perf_evlist__mmap_detach_fd()`。对线程 t
的每个 fd (event,c,t)：

1. 从 id 哈希删除它内嵌的 sample id；
2. 清 `entry->domain`，fd 从 `dom->fds[]` 删除；
3. 域已因 HUP 死亡且 `dom->fds[]` 空了 → 释放域，从 `th->domain_list` 摘下；
4. 它若是 group leader，同 (c,t) 上组员的 `entry->leader` 置 NULL，按 evsel 的状态重新 enable；
5. close(fd)，从 cpu 与线程的 `fd_list`、`evsel->fd_heads[]` 摘下并释放 `struct perf_evsel_fd`，放下
   它对 cpu 与线程记录的引用；`perf_evsel__del_thread()` 最后放下 evsel 对线程记录的引用，记录随之
   释放。evsel 的输入 thread map 不变，`perf_evsel__threads()` 返回的快照随之失效。

最后释放线程记录（从 `thread_heads[]` 摘除）。

**不经过 HUP 直接关闭 fd**（`perf_evsel__del_thread()` 直接调用、`perf_evsel__close()`、
`perf_evlist__munmap()`）时域还存活。域的最后一个 fd detach 时，7.3 中域与 ring 的关联在这里一并
解除，区别是 ring 引用直接释放、不转交调用方；被注册的 fd 若还有同域的 fd 可用，就换成它。

不论哪条路径，关闭的 fd 即使是 ring 的 owner 也无妨：VMA 撑住 event（5.3）。

### 7.5 增加 cpu

evsel 的 cpu 记录与线程记录对称：evsel 从 `perf_evsel__open()` 或 add 起对每条记录持有一个引用，到
del 或 `perf_evsel__close()` 放下；每个 fd 也各持有一个。所以某个 cpu 上一个 fd 都没有时，线程记录
仍在，之后加回的 cpu 照样为每个线程开 fd，反之亦然。cpu 记录按 cpu 升序排列，与 cpu map 一致。

`perf_evsel__add_cpu(evsel, c)` 为 evsel 的每个线程打开 (event,c,t)，与线程增加共用
`perf_evsel__open_new()`（新 fd 以 disabled 打开，补上 evsel 记下的 filter、BPF 程序，
`perf_evsel__mmap()` 的 ring，再 attach）。opened 时 cpu == -1（any cpu）的 evsel 不能加 cpu，返回
`-EINVAL`，与 any thread 不能加线程同理。已退出但还未 del 的线程 `perf_event_open()` 返回 `-ESRCH`，
它在新 cpu 上没有 fd，不算失败。

`perf_evlist__add_cpu(evlist, c)` 只驱动跟随 evlist cpu map 的 evsel：自带 cpu map 的、system wide
的、opened 时 any cpu 的 evsel 不动，需要时直接调用 `perf_evsel__add_cpu()`。已有 c 的 evsel 跳过，
不计入回滚；c 的删除未完成时返回 `-EBUSY`。其余步骤与 7.1 相同：按 `perf_evlist__open_order()` 打开、watermark 优先两轮 attach、最后
统一 enable（leader 带 `PERF_IOC_FLAG_GROUP`）；中途失败时把已加上的 cpu 删掉。新 fd 按 (c,t) 落入
(c,-1) ring：已有就 `SET_OUTPUT` 接入，没有就创建，经 poll add 回调以 `new_map` 报告给 order。

evsel 的输入 cpu map 不变，增删之后 `perf_evsel__cpus()` 返回按升序的快照，到下一次变化前有效。

### 7.6 删除 cpu：停止 → 收割 → 完成

```c
int perf_evlist__del_cpu(struct perf_evlist *evlist, int cpu, struct perf_evlist_reap *reap);
```

c 的 fd 只写入 c 的 ring，每种 overwrite 至多一个：(c,-1,false) 与 (c,-1,true)。

1. **停止**：对每个跟随 evlist cpu map 的 evsel，置 `c->del` 并 `DISABLE` c 上的全部 fd。cpu 绑定的
   event 的 DISABLE 在其 cpu 上执行，返回时 event 已被调度出去，之后不再写入。
2. **收割**：对 c 的每个 ring，数一数被停止的 fd 与全部 writer（各域 `dom->fds[]` 之和；死域的 fd
   已不计入）：
   - 停止的就是全部 writer → **retired**：`perf_evlist__mmap_retire()` 先把 ring 的所有域置 dead 并
     注销监听 fd（免得逐个摘域时把角色转给马上要死的域），再逐个 `perf_evlist__mmap_domain_put()`，
     ring 从 evlist 摘除，task 域死亡的线程把 HUP 角色转到它在其他 cpu 上的域；
   - 还有其他 writer（自带 cpu 的 evsel、system wide 的 evsel）→ ring 照常留在 evlist，只交给调用方读，
     不 retired；被停止的 fd 在完成时 detach。

   两种情况都另取一个引用放进 `reap->maps[]`。`reap->type = PERF_EVLIST_REAP__CPU`、`reap->cpu = c`、
   `complete` 恒为 true。
3. **完成**：调用方把 `maps[]` 读到末尾（retired 的 ring 从 order 堆删除），调用
   `perf_evlist__reap_done()`：放下引用，`perf_evlist__cpu_close()` 按逆打开顺序对每个 `c->del` 的
   evsel 调用 `perf_evsel__del_cpu()` 关闭 fd（7.4 的 1–5 步），最后放下 evsel 对 cpu 记录的引用。

停止与完成之间：再次 add/del 同一 cpu 返回 `-EBUSY`；新增的线程跳过 `c->del` 的 cpu。没有任何
evsel 有 c 时返回 `-ENOENT`，`reap->type` 为 `PERF_EVLIST_REAP__NONE`。

`perf_evsel__del_cpu()` 直接关闭 fd，不经过收割，与 `perf_evsel__del_thread()` 相同：失去 writer 的
ring 立即释放，调用方须先读完。

### 7.7 增加 evsel

evsel 整体加入已打开的 evlist，配置都用它自己的 `perf_evsel__` 函数在加入前完成：

1. `perf_evsel__new()`；作为组成员时 `perf_evsel__set_leader(evsel, leader)`；
   `perf_evsel__open(evsel, cpus, threads)`，用 evsel 自己的 cpu map 与 thread map（跟随 evlist 的
   线程或 cpu 时传当前的集合）。`attr.disabled = 1` 打开，加入时再 enable；
2. 可选 `perf_evsel__apply_filter()`、`perf_evsel__set_bpf()`；
3. `perf_evlist__add_evsel(evlist, evsel)`：`perf_evlist__add()` 加入链表，置 `evsel->joined`；
   evlist 已 mmap 时每个 fd 经 `perf_evlist__mmap_attach_fd()` 落入 (c,t) 的 ring——已有就
   `SET_OUTPUT` 接入，没有就创建，经 poll add 回调以 `new_map` 报告给 order；注册监听 fd，登记 sample
   id；最后 `perf_evsel__enable()`（`keep_disable` 时不 enable）。失败时摘掉已 attach 的 fd 并移出
   evlist，evsel 仍由调用方关闭。evlist 未 mmap 时只登记 sample id，之后的 `perf_evlist__mmap()` 一并映射。

`perf_evsel__set_leader(evsel, leader)` 的合法性检查，不合法返回 `-EINVAL`：

| 条件 | 原因 |
|---|---|
| `evsel` 尚未打开，否则 `-EBUSY` | 内核在 `perf_event_open()` 时取 group_fd，之后不能改 |
| `leader` 已打开 | 成员的 fd 以 leader 同 (c,t) 的 fd 为 group_fd |
| `leader` 自己是 leader（`leader->leader == leader`），且没有任何 fd 是别人的组成员 | group_fd 必须是组 leader（`perf_evsel__group_leader()`） |
| `leader` 不在移出 evlist 的过程中（`!leader->del`） | 它的 fd 将被关闭，组成员会被拆散 |
| `evsel` 自己没有成员（`nr_members == 1`） | 不支持嵌套组 |
| `perf_evsel__can_group(leader, evsel)` | 同一 clock；成员不能 pinned / exclusive；`keep_disable` 不入组 |

`leader` 为 NULL 或 evsel 自身时解除组关系。leader 在某个 (c,t) 没有 fd 时，成员在那里单独打开。

不在 evlist 里时成员 fd 按 evsel 自己的 `attr.disabled` 打开，不像 `perf_evlist__open()` 那样以
enabled 打开、跟随 leader：leader 已在运行，enabled 打开就会在加入 evlist 之前开始写入。
`perf_evlist__add_evsel()` 要求 leader 已在同一个 evlist 中，否则 `-EINVAL`。

grouped evlist（`perf_evlist__set_leader()`）里，open 之后加入的 evsel 只按显式 leader 分组：它不按
rank 参与组的选择，也不成为已有 fd 的 leader。`perf_evlist__open_order()` 里 joined evsel 不按 rank
排序，按加入顺序排在后面，显式 leader 总是先加入。之后的 `perf_evlist__add_thread()` /
`add_cpu()` 照常驱动它（跟随 evlist 的线程 / cpu 时）。

### 7.8 删除 evsel：停止 → 收割 → 完成

```c
int perf_evlist__del_evsel(struct perf_evlist *evlist, struct perf_evsel *evsel,
                           struct perf_evlist_reap *reap);
```

evlist 里任一 evsel 都可以删除，包括 open 时就在的。

1. **停止**：置 `evsel->del`，对 evsel 的全部 fd `DISABLE`。evsel 的 fd 还是别的 evsel 的组 leader 时返回
   `-EBUSY`：DISABLE leader 会把成员一起停掉，须先删除成员。
2. **收割**：对 evsel 的 fd 所在的每个 ring（去重），与 7.6 相同地数被停止的 fd 与全部 writer，
   被停止的 fd 包括 `c->del` 的 cpu 与 `evsel->del` 的 evsel 上的：全部停止则 retired，否则只交给
   调用方读。`reap->type = PERF_EVLIST_REAP__EVSEL`、`reap->evsel`，`complete` 恒为 true。
3. **完成**：`perf_evlist__reap_done()` 放下引用，`perf_evlist__evsel_leave()` 把 evsel 的 fd 逐个
   detach（死域随最后一个 fd 释放；未 retired 的 ring 上它的域离开），移出 evlist，清 `joined` /
   `del`，并退出显式组（清 `entry->leader`，`evsel->leader` 指回自身）：它的 leader 可能先于它被删除、
   关闭，evlist 之外没有人再清这些指针；内核里 fd 仍在组内，disabled，直到关闭。fd 保持打开。调用方
   随后 `perf_evsel__close()`、`perf_evsel__delete()`。

停止与完成之间：再次删除返回 `-EBUSY`；`perf_evlist__add_thread()` / `add_cpu()` 跳过该 evsel；它不
被选为新 fd 的组 leader。

---

## 8 与 perf-prof 的接口

### 8.1 external epoll（`src/monitor.c`）

```
perf_event_handle(fd, revents, map):
    设备 ACTIVE 时读 map（不增减引用）
    EPOLLHUP（任何非 EXIT 状态都处理，HUP 只报告一次）:
        prof_dev_hup(dev, fd)

prof_dev_hup(dev, fd):
    perf_evlist__hup(evlist, fd, &reap)
    for m in reap.maps:
        设备 ACTIVE 时把 m.map 读到当前 head（order_drain 或直接读）
        m.retired 且启用 order: order_mmap_del(m.map)
    reap.type == THREAD 且 reap.complete: prof_dev_thread_done(tid)
    perf_evlist__reap_done(evlist, &reap)
    nr_pollfd == 0 且没有 ptrace 链接: hangup + prof_dev_close()   // 6.3

__poll_add(ext, fd, events, map, new_map):
    main_epoll_add(); new_map 且启用 order: order_mmap_add(map)
    prof_dev_pollfd_counts(events): nr_pollfd++                     // 6.3
__poll_del(ext, fd, events, map):
    main_epoll_del(); prof_dev_pollfd_counts(events): nr_pollfd--
```

读 ring 不取引用的前提：ring 只会在 HUP 处理或 `perf_evlist__munmap()` 时释放，二者都不会在读
ring 的过程中发生。

### 8.2 order

堆排序器保证参与排序的所有 ring 上的事件按时间有序。堆持有 ring 的引用：`new_map` 时加入；
ring 失去 writer 时由 HUP 先收割其事件再 `order_mmap_del()`。ptrace 不操作 order。

### 8.3 ptrace

```
新线程:   prof_dev_add_thread() → perf_evlist__add_thread()
线程退出: prof_dev_del_thread() → perf_evlist__del_thread() == 1 时 prof_dev_thread_done()
```

### 8.4 prof_dev_thread_done

```
prof_dev_thread_done(dev, tid):
    设备 ACTIVE 时把所有 cpu ring 读到当前 head
    prof->del_thread(dev, tid)
```

两个信号都到达时线程已退出，它的 task-bound 事件都已写入（K10）；thread ring 已在 HUP 时收割，
cpu ring 在此读到 head，保证 `prof->del_thread()` 之后不再出现该线程的 task-bound 事件。

---

## 9 边界情况与已知限制

1. **只有 HUP 没有 del**：`-p` 未开 `--ptrace`。ring 引用照常释放；fd 与 `prof->del_thread()`
   留到设备关闭，线程数是初始集合，有上界。
2. **只有 del 没有 HUP**：线程没有 task-bound fd，del 直接完成（7.2）。
3. **ptrace detach 而线程未退出**：等不到 HUP。设备关闭时 `perf_evlist__munmap()` /
   `perf_evlist__close()` 无条件关闭全部 fd、释放全部 ring。
4. **tid 复用**：del 时 thread ring 移出查找哈希；`add_thread` 遇到退出未完成的同名 tid 先关闭旧线程。
5. **非 leader 线程 exec**（`de_thread`）：旧 leader 退出触发 `(-1, tgid)` 的 HUP；执行 exec 的
   线程 tid 变为 tgid，原 `(-1, T)` ring 继续写入。需 ptrace 处理 `PTRACE_EVENT_EXEC`，不在本设计内。
6. **inherit**：有子 event 时不 HUP，直到子 event 全部退出（K7）。
7. **overwrite ring**：登记键含 overwrite，`SET_OUTPUT` 要求 write_backward 一致；数据角色不带
   EPOLLIN，只等 HUP。
8. **EPOLLERR**：每个 ring / 线程只注册一个 fd，看不到其他 writer 的 EPOLLERR（pinned 硬件事件
   调度失败）。tracepoint / 软件事件不会出现。
9. **pid=-1 writer 产生的、关于退出线程的最后几条事件**（如它最后一次 `sched_switch`，发生在
   `exit_notify()` 之后）可能晚于 `prof->del_thread()` 到达，由 profiler 自行兜底。
10. **不支持 HUP 的内核（< v3.18）**：task 域等不到 HUP，`del_thread` 一直等待，fd 留到设备关闭。
    目前未处理。
11. **del 已到、HUP 未到的线程遇上 add_cpu**：新 cpu 上为它打开的 fd 会被 enable。当前 ptrace 未开
    `PTRACE_O_TRACEEXIT`，del 到来时 task 已退出，`perf_event_open()` 返回 `-ESRCH`，不会发生；开启
    TRACEEXIT 后需要处理。
12. **perf-prof 尚未使用 cpu 与 evsel 增删**：`perf_evlist__add_cpu()` / `del_cpu()`、
    `perf_evlist__add_evsel()` / `del_evsel()` 目前只有 libperf 实现与测试。
13. **加入运行中的组**：成员从下一次调度起随 leader 运行；PMU 放不下整个组时整个组都不运行，leader
    也受影响。tracepoint / 软件事件不受限。
14. **evsel 的 leader 先删**：返回 `-EBUSY`，须先删成员。不在 evlist 里、用 `perf_evsel__set_leader()`
    组成的组，`perf_evsel__close()` 不检查成员，由调用方先关成员。

---

## 10 为过滤器换代预留

perf 的 trace event 过滤器只能设置一次（K12）。若以后要为绑 cpu 的 evsel 随线程增删更换内核过滤器，
需要"开新一代 fd、设新过滤器、关旧一代"。本设计保证接入时不必返工：

1. `perf_evlist__mmap_attach_fd()` / `detach_fd()` 以 fd 为单位，同一槽位可以短暂并存新旧两个 fd，
   ring 只关心域内的 fd 集合。
2. owner 不依赖任何具体 writer fd（5.3）：旧一代即使是 owner，close 后也由 VMA 撑住。
3. 先 attach 新一代再 detach 旧一代，cpu 域不会变空，数据角色不变，不触发 epoll 操作。
4. 去重可按 sample id 在读路径完成，ring 层不感知。

---

## 11 代码位置

| 内容 | 位置 |
|---|---|
| ring 基础操作 | `lib/perf/mmap.c`：`perf_mmap__new/mmap/get/put` |
| 数据结构 | `lib/perf/include/internal/evlist.h`：`perf_mmap_domain`、`perf_evlist_thread`；`internal/mmap.h`：`perf_mmap` |
| 登记、线程记录、epoll、HUP 域、writer fd、收割、线程退出、cpu 增删、evsel 增删、mmap/munmap/poll | `lib/perf/evlist.c` 中 "Ring buffer management" 一节，按此顺序分组 |
| evsel 的 cpu / 线程记录与单个 fd 的增删，显式 leader | `lib/perf/evsel.c`：`perf_evsel__add/del_thread()`、`perf_evsel__add/del_cpu()`、`perf_evsel__open_new()`、`perf_evsel__set_leader()` |
| 对外接口 | `lib/perf/include/perf/evlist.h` |
| perf-prof 侧 | `src/monitor.c`：`prof_dev_hup()`、`prof_dev_thread_done()`、`prof_dev_add/del_thread()`、`__poll_add/del()`、`prof_dev_pollfd_counts()` |
| 测试 | `lib/perf/tests/test-bind.c`；`tests/test_dynamic_bindings.py` |
