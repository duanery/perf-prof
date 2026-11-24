# evlist 维护跟随集合，add/del 在任何阶段都可调用

Status: Done — implemented in commits d6d912c/b479fed/e0f204f (src), d9194c5 (tests); docs not committed (per user)

## 1 问题

1. 从空 evlist 起步、之后不断加入 evsel 时，调用方拿不到"新 evsel 该在哪些 cpu、线程上打开"。
   `user_requested_cpus/threads` 在 open 之后不再变，`add/del_cpu`、`add/del_thread` 不更新它。
2. open 之前调用 `perf_evlist__add_cpu()` / `add_thread()`：没有已打开的 evsel，返回 0，什么也没做，
   之后的 open 仍按原来的 map 打开——静默丢失。
3. open 之前用 `perf_evlist__add()` 加入已打开的 evsel：`perf_evlist__open()` 对它再次
   `perf_evsel__open()`，`-EBUSY`，整个 open 失败并关闭所有 evsel。
4. open 之前 `perf_evlist__del_evsel()` 一个有成员的 leader：`perf_evsel__leads_members()` 只看 fd，
   这时没有 fd，不报 `-EBUSY`；成员的 `evsel->leader` 仍指向被删除的 evsel。
5. open 之后、`perf_evlist__enable()` 之前 `perf_evlist__add_evsel()`：新 evsel 立即 enable，比其他
   evsel 先开始计数。
6. 空 evlist 上 `perf_evlist__open()`：`err` 未初始化，不进打开循环就被读取。

## 2 原则

把"集合"与"fd"分开：

- **集合**：跟随 evlist 的 evsel 默认应在哪些 cpu、线程上打开，由 evlist 维护。`add/del_cpu`、
  `add/del_thread` 首先修改集合。它是请求的集合，不是实际状态：跟随的 evsel 可以用子集打开，实际状态
  仍由 `perf_evsel__cpus()` / `threads()` 给出；自带 map、system wide、any cpu / any thread 的 evsel
  不受它约束。
- **fd**：evlist 打开之后，修改集合时同时给跟随的、已打开的 evsel 补开或关闭 fd（现有逻辑）。

于是 add/del 的语义与阶段无关，只是"open 之后多做一步 fd"；open 用当前集合打开；mmap 只决定 fd 是否
落入 ring。

## 3 跟随集合

### 3.1 数据结构

```c
/* 跟随集合中的一项：线程，或 thread map 是 cgroup 时的 cgroup fd */
struct perf_evlist_member {
	union {
		pid_t		 tid;          /* !@cgroup */
		int		 cgroup_fd;    /* @cgroup */
	};
	bool			 cgroup;
	struct list_head	 node;   /* perf_evlist::member_list，加入顺序 */
	struct hlist_node	 hnode;  /* perf_evlist::member_heads */
};

struct perf_evlist {
	...
	unsigned long		*cpu_bits;       /* 跟随集合中的 cpu */
	int			 nr_cpu_bits;    /* 按需扩大 */
	bool			 cpus_changed;
	struct perf_cpu_map	*cpus_snap;
	struct list_head	 member_list;
	struct hlist_head	 member_heads[PERF_EVLIST__THREAD_HLIST_SIZE];
	int			 nr_members;
	bool			 threads_changed;
	struct perf_thread_map	*threads_snap;
};
```

- **cpu 用位图**：cpu 号小而稠密，置位、测试 O(1)；快照要求升序，按位扫描天然有序，不需要排序
  链表。cpu 在集合层面没有别的状态（删除进行中的 `del` 在 evsel 的 cpu 记录上），不需要每 cpu 一个
  结构。cpu 号超出当前位数时 realloc 扩大（热插拔可能加入初始 map 之外的 cpu）。
- **线程用链表 + 哈希**：tid 稀疏、上限可达 pid_max，不适合位图；哈希供增删查找，链表保持加入顺序，
  初始 map 的顺序在前，快照的顺序与之一致。与 `struct perf_evsel_thread` 相同的做法，但没有引用计数和
  fd 链表。
- **不并入 `struct perf_evlist_thread`**：它的生命周期跟随 ring——随第一个域创建，在 del 完成或
  `perf_evlist__munmap()` 时释放；而集合要在 open 之前、没有 ring 时就存在，munmap 之后仍保留。
  合并就得让那条记录同时服从两种生命周期，`perf_evlist__thread_free()` 的三个调用点都要改。两张表
  各管各的，每次增删多一次哈希查找，代价可忽略。删除进行中的判断（`th->del`、`c->del`）仍在原来的
  记录上，不变。

`perf_evlist__set_maps()`（仍只允许 open 之前）由 map 的内容重建位图与线程表，并保存 map 本身。

- `perf_evlist__cpus()` / `perf_evlist__threads()`（新增，LIBPERF_API）：集合没变时返回
  `user_requested_*` 本身；变了返回按需生成并缓存的快照，到下一次变化前有效，长期持有须
  `perf_*_map__get()`。与 `perf_evsel__cpus()` / `threads()` 相同。
- 从不原地修改 map：跟随的 evsel 以它为输入 map，perf-prof 的 `dev->cpus/threads` 也可能是同一个
  对象。

### 3.2 any 与 cgroup

- cpu：any cpu（-1）不是成员，NULL 和 dummy map 得到空集合；集合变化后为空时快照是 dummy map（-1）。
- 线程：集合可混合线程、cgroup（`perf_evlist_member` 的 `tid`/`cgroup_fd` union）和 any thread
  （tid=-1，绑定 cpu 的 pid=-1）。`nr_tids` 只计 tid>=0 的线程：大于 0 时去掉 -1 项；降为 0 时，
  初始 map 有 -1（`map_any_thread`，NULL 与空 map 也算）才加回 -1。集合永不为空：删掉最后一项（线程，
  或以后的 cgroup）时换成 -1。换回 -1 都是就地改写被删项，不分配（`perf_evlist__member_del()`）。
  `perf_evlist__set_maps()`：map 有线程时去掉 -1；cgroup 且 pid<0 返回 `-EINVAL`。
  例：`(-1),(cg)` 加删线程后为 `(cg),(-1)`；`(cg)` 加删线程后仍为 `(cg)`；`(a)` 删 a 后为 `(-1)`。
- `add_thread`/`del_thread` 只增删线程，cgroup 只来自 `perf_evlist__set_maps()`。
- open 之前随时可以增删；open 之后某个 evsel 是否接收新的 cpu、线程由它自己的 `any_cpu`/`any_thread`
  决定，集合照样记录。

### 3.3 更新时机

| 调用 | 集合 | fd（evlist 已 open） |
|---|---|---|
| `add_cpu/add_thread` | 成功后记入；失败回滚时不记入 | 现有逻辑：给跟随的已打开 evsel 补开 fd |
| `del_cpu/del_thread` | 调用时（停止那一步）移出 | 现有逻辑：停止、收割，`reap_done()` 关闭 |
| `perf_evlist__hup()` | 移出该线程（先到的信号） | 现有逻辑 |

删除在停止时就移出集合：删除进行中的 cpu、线程不应再给新 evsel 打开，与 `add_thread` 跳过 `c->del`
的 cpu 一致。HUP 也移出：否则新 evsel 在已退出的 tid 上打开得到 `-ESRCH`。tid 复用后再 add 会重新
记入。

删除进行中（`c->del`、`th->del`，到 `reap_done()`）再 add/del 仍返回 `-EBUSY`，判断依据不变。

### 3.4 返回值（修改 cbe5e05 定下的部分）

- `add_cpu/add_thread`：已在集合中且所有跟随的 evsel 都已有它 → 0；没有 evsel 接收（空 evlist、
  未 open）→ 仍记入集合，返回 0。
- `del_cpu/del_thread`：既不在集合中、也没有 evsel 有它 → `-ENOENT`。在集合中但没有 fd 要关（空
  evlist、未 open）→ 移出集合，返回不带 ring 的 `complete` 收割。调用方的处理与有 fd 时相同，例如
  perf-prof 照样调用 `prof_dev_thread_done()`。
- 线程只因 HUP 移出集合、之后 del：evsel 仍有它的 fd，照现在处理。

### 3.5 open 与传播

- `perf_evlist__open()`：未打开的跟随 evsel 用 `perf_evlist__cpus()` / `threads()` 打开，而不是
  `user_requested_*`。open 之前的 add/del 由此生效（问题 2）。
- open 之后 `perf_evlist__add()` 一个未打开的 evsel：`needs_map_propagation` 传播的也是当前集合。
- 修复空 evlist 下 `err` 未初始化（问题 6）。

## 4 evsel 增删

### 4.1 add_evsel 在任何阶段可调用

`perf_evlist__add_evsel()` 是加入**已打开** evsel 的唯一入口：

- open 之前：`perf_evlist__add()`，置 `joined`，不 attach（还没有 ring）。`perf_evlist__open()` 跳过
  已打开的 evsel；joined evsel 本来就不按 rank 排序，按加入顺序排在后面。`PERF_FORMAT_ID` 的 id 在
  open 时与其他 evsel 一起登记。
- open 之后、mmap 之前：只登记 id（现状）；mmap 时一并映射。
- mmap 之后：现状。
- 显式 leader 必须已在同一个 evlist 中（现有检查）。

`perf_evlist__open()` 遇到已打开但不是 joined 的 evsel（用 `perf_evlist__add()` 加入），在打开任何
evsel 之前返回 `-EBUSY`，不再打开一半再全部关闭（问题 3）。

### 4.2 enable 跟随 evlist 的状态

evlist 增加 `enabled` 状态，由 `perf_evlist__enable()` / `disable()` 设置。`add_evsel()` 只在
evlist 已 enable 时 enable 新 evsel；否则保持它打开时的状态，由之后的 `perf_evlist__enable()`
统一 enable（问题 5）。`keep_disable` 不变。

`add_cpu/add_thread` 新 fd 的 enable 已经按 evsel 自己的 `enabled` 判断，不变。

### 4.3 del_evsel 的组检查

`-EBUSY` 的条件增加：evlist 中还有 evsel 以它为显式 leader（`pos->leader == evsel`）。现有的 fd
检查保留：grouped evlist 里按 rank 形成的组只体现在 fd 的 `entry->leader` 上。这样 open 之前删除
leader 同样被拒绝（问题 4）。

open 之前 del_evsel：没有 fd，收割不带 ring，`complete`；`reap_done()` 移出 evlist，与
`perf_evlist__remove()` 等价。

## 5 各阶段汇总

| 调用 \ 阶段 | open 之前 | open 之后、mmap 之前 | mmap 之后 |
|---|---|---|---|
| `add/del_cpu`、`add/del_thread` | 只改集合 | 改集合 + 开关 fd，登记/注销 id | 改集合 + 开关 fd + ring |
| `add_evsel` | 加入，open 时跳过 | 加入，登记 id | 加入，attach |
| `del_evsel` | 移出（leader 有成员时 `-EBUSY`） | 停止、完成，无 ring | 停止、收割、完成 |
| enable | — | 随 evlist 的 enabled 状态 | 同左 |

## 6 不在本方案内

- rlimit：`perf_evlist__open()` 按初始集合放宽 `RLIMIT_NOFILE`，之后增加的 fd 不计入。
- perf-prof 的接入：`dev->cpus/threads` 保持初始 map（per-cpu 数组依赖它的 idx）；打开新 evsel、
  开子设备时取 `perf_evlist__cpus()` / `threads()`。另立方案。

## 7 提交

1. libperf：空 evlist 下 `perf_evlist__open()` 的 `err` 初始化。
2. libperf：evlist 维护跟随集合（3 节）。
3. libperf：add/del evsel 在任何阶段可调用，enable 跟随 evlist（4 节）。
4. libperf tests：test-bind 覆盖 open 之前、mmap 之前的 add/del，空 evlist，`-ENOENT`/`-EINVAL`
   的新语义，open 之前删除 leader。
5. docs：`libperf_ring_lifetime.md` 第 7 节与 9.12，`lib/perf/Documentation/libperf.txt`。
