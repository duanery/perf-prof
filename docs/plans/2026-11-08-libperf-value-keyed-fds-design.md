# libperf：fd 按值组织，去掉下标

Status: Done — implemented in commits a2bbbed/c2f3e61/479559f/6c4292e (src), 81f32fa (tests), 7d522c9 (docs)
Date: 2026-11-08
Base: main 56c0f2f

范围：只做"去下标、只用值"。切片增删（event / cpu 维度）、reap 接口留到后续。

---

## 1 问题

evsel 的 fd 及其附属数据按 cpu map / thread map 的**下标**存放：

| 数据 | 存储 |
|---|---|
| fd、group_fd、domain | `evsel->fd`：xyarray[cpu_idx][thread_idx] of `struct perf_evsel_fd` |
| 私有 mmap（`perf_evsel__mmap()`） | `evsel->mmap`：xyarray |
| sample id | `evsel->sample_id`：xyarray of `struct perf_sample_id`，节点挂在 evlist id 哈希 |
| id 集合 | `evsel->id[]` / `evsel->ids` |

而 (event,c,t) 是按**值**确定、会增删的稀疏集合。为让下标在线程增删后仍然对齐，引入了一串补丁：
`PERF_THREAD_MAP_HOLE` 空洞与复用、`perf_thread_map__realloc/dup`、`make_threads_private` /
`threads_private`（COW）、`xyarray__grow_y`、`perf_evsel__grow_threads()` 里 realloc 前后
unlink/relink sample id、`perf_thread_map__valid()`、所有循环跳过空洞。每多一个可变维度（cpu）
就要再来一套。另外：

- 下标与值反复互转：`perf_evsel__mmap_bind()`、`get_group_fd()` 把下标换成值再换回 leader 的下标；
- 对外接口暴露下标：`perf_evsel__read/get_id/is_group_leader/mmap_base(evsel, cpu_idx, thread_idx)`，
  调用方必须先拿 evsel 的 map 换算；
- thread map 同时充当"请求的集合"和"当前存在的集合"，动态线程让它变成可变对象，被 evlist、
  evsel、prof_dev 多方共享，只能靠 COW 维持。

## 2 原则

1. **内部只用值**。一个 fd 由 (evsel, cpu, tid) 唯一确定，cpu、tid 都是值；cgroup fd 的 tid 位置是
   cgroup fd，另带 cgroup 标志。
2. **map 只在接口边界**。cpu map / thread map 是不可变的值集合：
   - 输入：`perf_evlist__set_maps()`、`perf_evsel__set_own_cpus/threads()`、`perf_evsel__open()`
     接收 map，libperf 遍历它打开 fd，之后不再依赖它的下标；
   - 输出：需要 map 的调用方（`perf_evsel__threads()` 等）拿到的是由内部表示生成的快照。
3. **对象不移动**。每个 fd 一个独立分配的对象，挂在链表/哈希上；增删不 realloc 任何数组。

## 3 内部表示

```c
struct perf_evsel_fd {
	struct perf_evsel	*evsel;
	int			 cpu;      /* 值，-1 表示不绑 cpu */
	pid_t			 tid;      /* 值，-1 表示 pid=-1；cgroup 时为 cgroup fd */
	bool			 cgroup;
	int			 fd;
	struct perf_evsel_fd	*leader;   /* 取代 group_fd */
	struct perf_mmap_domain	*domain;   /* 已有 */
	struct perf_mmap	*mmap;     /* perf_evsel__mmap() 的私有 ring */
	struct perf_sample_id	 sid;      /* 内嵌：id、evlist 哈希节点、period */
	struct list_head	 cpu_node;    /* perf_evsel_cpu::fd_list */
	struct list_head	 thread_node; /* perf_evsel_thread::fd_list */
	struct hlist_node	 hnode;       /* evsel->fd_heads[]，按 (cpu, tid) 查找 */
};

struct perf_evsel {
	...
	struct perf_cpu_map	*cpus, *own_cpus;      /* 输入：请求的集合，不可变 */
	struct perf_thread_map	*threads, *own_threads;
	struct list_head	 fds;
	struct hlist_head	 fd_heads[...];
	int			 nr_fds;
	struct perf_thread_map	*threads_snap;          /* 输出快照，变化时失效 */
};
```

- `perf_evsel__fd_find(evsel, cpu, tid)`：按值查找；`perf_evsel__for_each_fd(evsel, pos)`：遍历。
- sample id 内嵌后：`perf_evlist__id_to_evsel()` 命中 `sid` 即得 `container_of(...)->evsel`；
  id 哈希节点永不移动。`evsel->id[]` / `ids` 删除（只有 libperf 测试在用）。
- 下标全部去掉：`perf_mmap.idx` / `perf_mmap__idx()`、`perf_evlist__mmap_idx()`、`perf_sample_id.idx`、
  只为它们服务的 `evlist->all_cpus` 一并删除（perf-prof 不用）。
- map 只在两处出现：`perf_evsel__open()` 遍历输入 map 打开 fd；`perf_evsel__cpus()` / `perf_evsel__threads()`
  把 map 交还调用方。打开之后 evsel 只用自己的记录：`evsel->cpu_list`（`struct perf_evsel_cpu`，打开时
  从输入 cpu map 记下的 cpu 值，保留到 close）、`evsel->thread_list` / `thread_heads[]`
  （`struct perf_evsel_thread`，随该线程第一个 fd 创建、最后一个 fd 释放），fd 同时挂在所属 cpu 与线程的
  `fd_list` 上，增删线程只走该线程的 `fd_list`、`evsel->any_thread`
  （以 pid=-1 打开，不能再加线程）、`evsel->rank_cpus/rank_threads`（见 4.1）。
- `cpus_bound` 改名为 `opened`：从 `perf_evsel__open()` 到 `perf_evsel__close()`，fd 已打开，输入 map
  不再改变。

## 4 各处改法

| 位置 | 现状 | 改为 |
|---|---|---|
| `perf_evsel__open()` | 分配 xyarray，按下标 `open_one(cpu_idx, thread_idx)` | 遍历输入 map 的值，`open_one(cpu, tid, cgroup)` 分配 entry |
| `get_group_fd()` | 值→leader 下标→fd，要求 map 完全一致 | 按 4.1：在同 (cpu, tid, cgroup) 上取排名最高的 evsel 的 fd 作 leader |
| enable/disable/filter/bpf/`run_ioctl` | `for cpu_idx: for thread_idx` 跳空洞 | `perf_evsel__for_each_fd()`；`*_cpu(evsel, cpu)` 的参数改为 cpu 值，过滤 `pos->cpu == cpu` |
| `perf_evsel__add_thread()` | 找空洞或 grow，COW thread map | 对 `evsel->cpus` 的每个 cpu 值 `open_one(cpu, tid)`；不修改任何 map |
| `perf_evsel__del_thread()` | 关 fd、槽位置 HOLE | 对 `tid` 的所有 entry close 并释放 |
| `perf_evlist__mmap_attach/detach_fd()` | `(evsel, cpu_idx, thread_idx)` | `(evlist, struct perf_evsel_fd *)`；ring 键直接取 `pos->cpu` / `pos->tid` |
| `perf_evsel__mmap_bind()` | 下标→值 | 删除 |
| `perf_evlist__id_add_fd()` 等 | `(evsel, cpu_idx, thread_idx, fd)` | `(evlist, struct perf_evsel_fd *)` |
| `__perf_evlist__propagate_maps()` | 不覆盖 `threads_private` 的 map | 只设置输入 map；打开后不再改 |
| `perf_evlist__open()` 的 RLIMIT 估算 | `nr cpus × nr threads` | 不变（按输入 map 估算） |

### 4.1 group leader

内核的 group 只要求成员与 leader 在同一个 (cpu, task/cgroup) 上，并不要求两个 evsel 的 map 相同。去下标后
group 按 fd 决定，不再按 evsel：

- **排名**：`perf_evsel__rank_cmp(a, b)` 把排名当作值比较，a 比 b 高返回 > 0：先比输入 cpu map 的 cpu 数
  （dummy 记 1），再比输入 thread map 的线程数，越大越高；相同时先加入 evlist 的高。cpu 数、线程数由
  `perf_evsel__set_rank()` 在 open 时按**输入** map 记下（`perf_evlist__open()` 在排打开顺序前先记），
  动态增删线程不改变排名。
- **能否成组**：`perf_evsel__can_group(leader, member)`，按内核约束：只有 group leader 能 pinned /
  exclusive，所以 member 不能是，leader 可以是；同组 clock 相同（`use_clockid` / `clockid`）。
  `keep_disable` 的 evsel 不参与分组。
- **每个 fd 的 leader**：同一个 (cpu, tid, cgroup) 上，排名最高且在该处有 fd 的 evsel 的 fd 是 leader，其余
  是成员（`pos->leader` 指向 leader entry）。一个 evsel 可以在一些 cpu 上是 leader、另一些 cpu 上是成员。
  例：event1 绑 cpu 0-7，event2 绑 cpu 0-3。event1 排名高，在 cpu 0-7 都是 leader；cpu 0-3 上 event2 是它的
  成员，cpu 0 的组有 2 个事件，cpu 7 只有 1 个。若再有 event3 绑 cpu 4-11，它在 8-11 是 leader，4-7 是
  event1 的成员。
- **打开顺序**：内核要求 leader 先打开，open 与 add_thread 都按排名顺序遍历 evsel。
- **enable / disable**：`*_group` 只对 `leader == NULL`（自己是 leader）的 fd 发
  `PERF_IOC_FLAG_GROUP`；成员 fd 以 `attr.disabled = 0` 打开、随 leader 调度。
- **读**：`PERF_FORMAT_GROUP` 从 leader fd 读出的成员数随 (cpu, tid) 变化（上例 cpu 0 为 2、cpu 7 为 1），
  `perf_evsel__read_size()` 以 evlist 的 evsel 数为上限，短读是合法的（现状已如此），调用方按读出的 `nr`
  解析。
- **显式 group**（非 grouped evlist，`evsel->leader` 由调用方指定）：leader 在某个 (cpu, tid) 上没有 fd 时，
  成员在那里单独打开，不再报 `-EBADF`。
- **leader fd 关闭**：线程退出时同一 (·,t) 的 fd 一起关闭，不影响别处；leader 若是某个 ring 的 owner，
  close 后仍被 VMA 撑住，成员留在它的组里（见 `docs/libperf_ring_lifetime.md` 5.3），后续 evsel 维度的删除
  需要先处理这一点。

查找 leader 现在是遍历 evlist、对每个 evsel `__perf_evsel__fd_find(pos, cpu, tid, cgroup)` 取排名最高者，
O(evsel 数)；后续引入 cell (c,t) 后降为 O(1)。`perf_evlist__open_order()` 给出打开顺序（grouped 按排名从高到低，否则 evlist 顺序），close 与线程关闭
按其逆序。

- **leader 关闭**：同 (cpu, tid, cgroup) 上 `leader` 指向它的成员置 NULL，按各自 evsel 的状态 enable 或
  disable（成员以 enabled 打开、只随 leader 调度，leader 释放后成为独立 event）。
- **`__perf_evlist__set_leader()`** 无使用者，删除。
- `struct perf_evsel_cpu` / `struct perf_evsel_thread` 各有链表与哈希（`cpu_list`/`cpu_heads`、
  `thread_list`/`thread_heads`），带引用计数：每个 fd 持有所属 cpu 与线程记录的引用；evsel 在
  open 到 close 期间持有每个 cpu 的引用（cpu 没有 fd 时仍保留，供后加的线程打开）；打开过程中调用方
  临时持有线程记录的引用。`perf_evsel__open_one(evsel, c, t, out)` 统一接收两条记录，新 fd 取引用；
  `perf_evsel__close_one()` 释放引用，最后一个引用释放时记录随之释放。
- evlist 的用户 map（`user_requested_cpus` / `user_requested_threads`，后者由 `threads` 改名）从
  `perf_evlist__open()` 到 `perf_evlist__close()` 固定，期间 `perf_evlist__set_maps()` 返回 -EBUSY（改为返回 int）。
  删除未使用的 `has_user_cpus`。
- 链表头统一 `_list` 后缀、哈希表头 `_heads` 后缀：`evlist->entries` → `evsel_list`，`evlist->heads` →
  `id_heads`，`epoll.heads` → `poll_fd_heads`，`map->domains` / `th->domains` → `domain_list`，
  `map->list` → `node`，`perf_evlist_thread::node` → `hnode`。每个节点注释它挂在哪个表头，每个表头注释
  其中的结构体与成员。
- 有内核约束的函数，在函数前的注释里直接写明约束（内核函数、错误码、引入的 commit），不引用文档。
- **`perf_evlist__set_leader()`** 只置 `grouped`，各 evsel 的 `leader` 指向自己；`perf_evlist__nr_groups()`
  在 grouped 时数"至少在一处带成员的 leader evsel"，需在 open 之后调用。

删除：`evsel->fd`/`mmap`/`sample_id` xyarray、`evsel->id[]`、`perf_evsel__alloc_fd/alloc_id/free_id`
的下标版本、`perf_evsel__grow_threads()`、`perf_evsel__relink_ids()`、`perf_evsel__thread_slot()`、
`perf_evsel__make_threads_private()`、`threads_private`、`xyarray__grow_y()`、
`PERF_THREAD_MAP_HOLE`、`perf_thread_map__dup()`、`perf_thread_map__valid()`。
`perf_thread_map__realloc()` 保留：它是 thread map 的分配器，`lib/thread_map.c`、`lib/cgroup.c` 在用。
xyarray 的唯一外部使用者是 `src/bpf_kvm_exit.c`（取 fd），改用新增的 `perf_evsel__fd(evsel, cpu, tid)` 后一并删除。

## 5 对外接口

| 旧 | 新 |
|---|---|
| `perf_evsel__read(evsel, cpu_idx, thread_idx, counts)` | `perf_evsel__read(evsel, cpu, tid, counts)` |
| `perf_evsel__get_id(evsel, cpu_idx, thread_idx)` | `perf_evsel__get_id(evsel, cpu, tid)` |
| `perf_evsel__is_group_leader(evsel, cpu_idx, thread_idx)` | `perf_evsel__is_group_leader(evsel, cpu, tid)` |
| `perf_evsel__mmap_base(evsel, cpu_idx, thread_idx)` | `perf_evsel__mmap_base(evsel, cpu, tid)` |
| `perf_evsel__enable_cpu/disable_cpu/close_cpu/apply_filter_cpu(evsel, cpu_idx)` | 参数改为 cpu 值 |
| — | `perf_evsel__for_each_fd(evsel, pos)` 遍历已打开的 fd，`perf_evsel_fd__cpu/tid(pos)` 取值 |
| `perf_evsel__cpus()` | 不变：返回输入 cpu map（cpu 维度暂不可变） |
| `perf_evsel__threads()` | 未增删过线程时返回输入 map；否则返回当前线程集合的快照（按打开顺序：输入 map 的线程在前、后加的在后，由 evsel 缓存，下次增删时失效） |
| — | `perf_evsel__fd(evsel, cpu, tid)`：取 fd 值，没有时返回 -1 |

未改变的：`perf_cpu_map` / `perf_thread_map` 本身的构造与查询接口（`__idx()`、`__cpu()`、
`__pid()`），它们是值集合上的普通工具，perf-prof 的 profiler 用它们索引自己的数组，与 libperf 内部
无关。

## 6 perf-prof 侧

| 位置 | 改法 |
|---|---|
| `src/monitor.c` interval 读计数 | 用 `perf_evsel__for_each_fd()` 取 (cpu, tid)，调用值接口；删掉 `perf_thread_map__valid()` |
| `src/event-spread.c` | `perf_evsel__get_id(evsel, cpu, tid)` 直接用值，删掉 `cpuidx` / `threadidx` 换算 |
| `src/watchdog.c` | `perf_evsel__apply_filter_cpu(evsel, filter, cpu)` 传 cpu 值 |
| `src/task-state.c`、`src/multi-trace.c` 的动态线程表 | 原来用 `PERF_THREAD_MAP_HOLE` + `perf_thread_map__realloc()` + COW 自己扩 thread map。改为 `lib/thread_map.c` 新增的 `struct tid_set`（rblist，按值增删查，sample 路径从线性 `perf_thread_map__idx()` 变为 O(log n)）；`ctx->thread_map` 只保留为输入 map（ptrace_attach、打印）。task-state 重建过滤器时用 `tid_set__map()` 生成 map；rundelay 的过滤器只在 init 生成，仍用输入 map |
| `src/event-spread.c` 的实例下标 | `block_event_convert()` 返回的 ins 是 event-spread 自己的实例编号（调用方用 evsel 的 map 换回值），保留；只把 `perf_evsel__get_id()` 改成按值，下标换值、找不到时取下标 0，与原逻辑一致 |
| `src/bpf_kvm_exit.c` | `FD(e, ins, 0)` 改为 `perf_evsel__fd(evsel, cpu, perf_thread_map__pid(perf_evsel__threads(evsel), 0))`，去掉 xyarray |
| `src/bpf_kvm_exit.c` | `perf_thread_map__comm(ctx->thread_map, idx)`：ctx 自己的输入 map，不变 |
| `llcstat`/`hwstat`/`tlbstat` 的 `perf_cpu_map__idx(dev->cpus, cpu)` | profiler 自己数组的下标，不变 |

## 7 实施顺序

每一步保持 `lib/perf/tests` 与 `tests/test_dynamic_bindings.py`、`test_evsel_cpus.py`、
`test_task-state.py` 通过：

1. 引入 `struct perf_evsel_fd` 链表 + 哈希，open/close/ioctl 遍历改走链表；xyarray 暂留作兼容
   （按下标查时先换成值）。
2. sample id 内嵌进 entry；删 `sample_id` xyarray、`id[]`、relink。
3. `perf_evlist__mmap_attach/detach_fd()`、id 接口、group leader 改为 entry 参数。
4. add/del_thread 不再修改 thread map；`perf_evsel__threads()` 改为快照；删 HOLE、COW、grow。
5. 对外接口换成值；perf-prof 侧适配（第 6 节）；删 xyarray 兼容层。

提交拆分：libperf（含 `lib/perf/tests`）独立成 commit，不依赖 perf-prof；perf-prof 适配单独
commit；测试、文档各自 commit。第 5 步对外接口变化时 libperf 与 perf-prof 两个 commit 之间
perf-prof 无法编译（同上一轮），若要保持可 bisect，在 libperf commit 中保留旧接口的兼容包装，
perf-prof commit 中删除。

## 8 后续（不在本次）

- cell (c,t) 聚合，group leader 查找改从 cell 取（规则不变，见 4.1）；
- cpu 记录、`add_cpu/remove_cpu`；
- 切片增删与 `perf_evlist_reap` / `reap_done` 接口。
