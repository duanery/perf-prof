# evsel 级 cpu/线程绑定 与 mmap 共享模型

## 1 背景

### 1.1 现状

`libperf` 的绑定关系挂在 `evlist` 上：`evlist->user_requested_cpus` 和 `evlist->threads`
在 `__perf_evlist__propagate_maps()` 里被复制到每一个 `evsel`。ringbuffer 的分配也是以
evlist 为单位：`mmap_per_cpu()` / `mmap_per_thread()` 先按 cpu（或线程）循环，再在内层遍历
所有 evsel，第一个 evsel 建立映射，其余 evsel 用 `PERF_EVENT_IOC_SET_OUTPUT` 复用。

于是 `nr_mmaps == nr_cpus`（或 `nr_threads`），mmap 的下标 `map->idx` 就等于 perf-prof 里的
`instance`。整个 perf-prof 的 profiler 接口都建立在这个下标之上。

### 1.2 两个问题

**（1）ebpf 事件不能绑定线程。**

perf 事件可以绑定 cpu，也可以绑定线程。但 ebpf 事件（`bpf_perf_event_output()` 写入
perf ringbuffer）只能绑定 cpu —— 全部 cpu 或部分 cpu。当一个 evlist 里同时有 perf 事件和
ebpf 事件时，evlist 级的统一绑定就无法表达："这个 evsel 绑 cpu，那个 evsel 绑线程"。

**结论：cpu/线程绑定必须跟随 evsel，而不是 evlist。**

**（2）inherit 模式太重。**

要跟踪一个多线程进程，现在有两条路：

- `attr.inherit = 1`：内核自动继承到子线程。但 inherit 与 per-cpu ringbuffer 不能很好共存，
  且无法在用户态感知线程的创建/退出。
- `--ptrace`：`ptrace.c` 通过 `PTRACE_O_TRACECLONE` 捕获 clone，然后
  **给每个新线程 clone 一个完整的 `prof_dev`** —— 新的 evlist、新的 evsel、新的 ringbuffer、
  新的 order 堆。一个 1000 线程的进程会产生 1000 个 prof_dev。

**结论：libperf 需要支持"往已经打开的 evsel 上动态增删线程"，ptrace 只负责通知。**

---

## 2 libperf 改造

### 2.1 绑定关系下沉到 evsel

```
struct perf_evsel {
        struct perf_cpu_map     *cpus;         /* 生效的 cpu 绑定 */
        struct perf_cpu_map     *own_cpus;     /* evsel 自己指定的 cpu 绑定，可为 NULL */
        struct perf_thread_map  *threads;      /* 生效的线程绑定 */
        struct perf_thread_map  *own_threads;  /* evsel 自己指定的线程绑定，可为 NULL */
        ...
};
```

传播规则（`__perf_evlist__propagate_maps()`）：

| evsel 自己 | 生效值 |
| --- | --- |
| `own_cpus != NULL` | `evsel->cpus = own_cpus` |
| `own_cpus == NULL` | `evsel->cpus = evlist->user_requested_cpus` |
| `own_threads != NULL` | `evsel->threads = own_threads` |
| `own_threads == NULL` | `evsel->threads = evlist->threads` |

evlist 里的是"默认值"，evsel 可以覆盖。

**cpu 绑定一次确定，之后不变。** `perf_evsel__open()` 成功后置 `evsel->cpus_bound = true`，
后续的 `perf_evlist__set_maps()` 不再改写它。不同 evsel 可以绑不同的 cpu 子集。

**线程绑定可以动态增删。** 见 2.4。

### 2.2 evsel 的绑定维度

一个 evsel 只有两种绑定维度，由它自己的 cpu map 决定：

```
oncpu = !perf_cpu_map__empty(evsel->cpus)      /* cpu map 不是 dummy(-1) */
```

- **cpu 维度**（`oncpu == true`）：`evsel->cpus` 里每个 cpu 一个 ringbuffer。
  即使同时指定了多个线程（cgroup / inherit），所有线程的 fd 都 `SET_OUTPUT` 到本 cpu 的
  ringbuffer 上。这也是 ebpf 事件唯一可用的维度。
- **线程维度**（`oncpu == false`）：`evsel->threads` 里每个线程一个 ringbuffer。

### 2.3 mmap 的绑定与全局复用

`struct perf_mmap` 记录自己的绑定：

```
struct perf_mmap {
        int      cpu;   /* cpu 维度：cpu 号；线程维度：-1 */
        pid_t    tid;   /* 线程维度：tid；   cpu 维度：-1 */
        bool     overwrite;
        ...
};
```

`(cpu, tid, overwrite)` 三元组是 mmap 的**全局唯一键**，evlist 上挂一张哈希表做查找。

`perf_evlist__mmap()` 彻底重写为**按 evsel 顺序**处理：

```
for each evsel in evlist:                  /* 外层：evsel */
    for each (cpu_idx, thread_idx) of evsel:
        key = evsel->oncpu ? (cpu, -1) : (-1, tid)
        map = lookup(evlist, key, evsel->attr.write_backward)
        if map == NULL:
            map = new_mmap(key)            /* 首个使用该绑定的 evsel 负责创建 */
            mmap(fd)                       /* refcnt = 1 */
            link into evlist
        else:
            ioctl(fd, PERF_EVENT_IOC_SET_OUTPUT, map->fd)
            perf_mmap__get(map)            /* 复用，加引用 */
        epoll_add(fd, map)
        id_add(evsel, cpu_idx, thread_idx, fd)
```

第一个 evsel 决定自己的绑定关系并创建 mmap，后面的 evsel 只要绑定键相同就复用。
"全局复用"意味着复用不再局限于同一个 cpu 循环内 —— 任意两个 evsel，只要绑定键相同就共享。

> **watermark 的次序**：`rb->watermark` 是 ringbuffer 的属性，在 `mmap()` 那一刻由**创建者
> fd** 的 `attr.wakeup_watermark` 决定；而 `wakeup_events` 是 per-event 属性。所以 evsel 的
> 遍历分两趟：第一趟只处理 `attr.watermark` 的 evsel（由它们创建 ringbuffer，水位正确），
> 第二趟处理 `wakeup_events` 的 evsel（一律 `SET_OUTPUT` 复用）。这保留了改造前
> `mmap_per_evsel()` 里 `wakeup_events_only` 两趟扫描的语义。

**epoll 必须加入每一个 fd，而不只是创建 mmap 的那个 fd。** 内核 `perf_output_wakeup()`
唤醒的是**写入者** event 的 `waitq`（`handle->event->pending_irq`），只是把 `EPOLLIN` 记在
共享的 `rb->poll` 上。只 poll 属主 fd 会漏唤醒。

### 2.4 线程的动态增删

```
int  perf_evsel__add_thread(struct perf_evsel *evsel, pid_t pid);
int  perf_evsel__del_thread(struct perf_evsel *evsel, pid_t pid);
int  perf_evlist__add_thread(struct perf_evlist *evlist, pid_t pid);
int  perf_evlist__del_thread(struct perf_evlist *evlist, pid_t pid);
```

**写时复制**：如果 `evsel->threads == evlist->threads`（还在共用 evlist 的默认值），
第一次动态增删时先深拷贝成 `own_threads`，之后只动自己的。这样 `evlist->threads`
（以及 perf-prof 侧持有它的 `dev->threads`）指针始终有效，不会被 realloc 移动。

**空洞而非压缩**：删除线程时**不压缩**下标，把 slot 标记为空洞
（`thread_map_data.pid == PERF_THREAD_MAP_HOLE`），fd 置 -1。原因：

- `evsel->sample_id` 是一个 xyarray，里面的 `struct perf_sample_id` 直接挂在 evlist 的
  id 哈希链表上。压缩会移动这些 `hlist_node`，链表立刻损坏。
- `evsel->fd` / `evsel->mmap` 都按线程下标寻址。

新增线程优先复用空洞，没有空洞才把 xyarray 的 y 维扩容（`xyarray__grow_y()`）。

**mmap 的生命周期**：

- 增加线程 → 查找 `(-1, tid)` 的 mmap，没有就新建并 `mmap()`；有就 `SET_OUTPUT` + `perf_mmap__get()`。
- 删除线程 → 关 fd，`perf_mmap__put()`。引用归零时立即 `munmap()` 并从 evlist 摘链释放。

  调用方（perf-prof 的 ptrace 路径）**必须先把 ringbuffer 里的残留事件读完**再调用
  `perf_evsel__del_thread()`，否则事件丢失。

### 2.5 事件 id → evsel → 外部关联

多个事件写到同一个 mmap，靠 `PERF_SAMPLE_ID` 区分。这条链路已经存在，改造后继续复用：

```
id --(evlist->heads[] 哈希)--> struct perf_sample_id --> evsel --> evsel->external --> tp
```

`perf_evlist__id_to_evsel()` 之上，perf-prof 的 `perf_evsel_tp()` / `perf_evsel_dev()`
拿到外部关联。改造后 mmap 的共享面变大（跨 evsel 全局复用），这条链路的重要性也随之上升。

### 2.6 mmap 绑定关系对外暴露

perf-prof 从每个 mmap 读事件，需要知道这个 mmap 代表哪个 cpu / 哪个线程：

```
int  perf_mmap__cpu(struct perf_mmap *map);     /* -1 表示不是 cpu 绑定 */
pid_t perf_mmap__tid(struct perf_mmap *map);    /* -1 表示不是线程绑定 */
bool perf_mmap__oncpu(struct perf_mmap *map);
```

这是 perf-prof 侧去掉 `instance` 的基础：事件的 cpu/线程归属直接来自 mmap，而不是数组下标。

### 2.7 分组

绑定关系相同（同 cpu 或同线程）的 evsel 才可能组成一个 group —— 内核要求 group 内所有
event 在同一个 `(cpu, pid)` 上。首个 evsel 作为 group leader，同时启用、同时禁用。
`get_group_fd()` 已经处理了 leader 与成员 cpu map 不一致的下标换算，改造后按绑定键分组即可。

同一 `(cpu, pid)` 上不一定所有 evsel 都打开成功，group 样本里的 `PERF_SAMPLE_READ`
只包含实际成员。解析样本时按 read 数据中的 `nr` 计算其实际长度；READ 之后的
`CALLCHAIN`、`RAW` 等字段必须在这个实际长度之后继续解析。

---

## 3 perf-prof 改造

### 3.1 去掉 instance

现状：`instance` = mmap 下标 = `dev->cpus` 或 `dev->threads` 的下标，`prof_dev_nr_ins()`
给出总数。线程动态增删之后这个下标不再稳定，必须换掉。

profiler 的回调统一从 `int instance` 换成 cpu/线程：

```
- void (*sample)(struct prof_dev *dev, union perf_event *event, int instance);
+ void (*sample)(struct prof_dev *dev, union perf_event *event, int cpu, int tid);
```

`cpu == -1` 表示事件来自线程绑定的 ringbuffer，`tid == -1` 表示来自 cpu 绑定的 ringbuffer。
两者由 `perf_mmap__cpu()` / `perf_mmap__tid()` 直接给出。

聚合状态不再用映射下标寻址。固定 cpu 的 profiler 可以在 profiler 内部把真实 cpu 换算成
自己的槽位；线程相关的 profiler 用 `(cpu, tid)` 键。

原来用 `instance` 做数组下标的 profiler（`profile.c`、`hrcount.c`、`multi-trace.c`、
`kvm-exit.c` 等）改成按 cpu/tid 查哈希表。

### 3.2 ptrace 驱动线程增删

`ptrace.c` 收到 `PTRACE_EVENT_CLONE` / `FORK` / `VFORK` 时，不再 `prof_dev_clone()`，
而是 `perf_evlist__add_thread(dev->evlist, new_tid)`。

线程退出时：先 `perf_evlist__find_mmap(evlist, -1, tid, overwrite)` 找到 ringbuffer，
把残留事件处理完，再 `perf_evlist__del_thread()` 让 libperf 销毁它。

### 3.3 order 全局排序

改成对每个 mmap 建一个堆元素。mmap 是动态的，`order.c` 里按 `nr_mmaps` 预分配的
`permap_event` 数组换成随 mmap 创建/销毁而增删的链表节点。

### 3.4 后续

逐步去掉子 `prof_dev` 向父 `prof_dev` 转发（`forward.target` / `PERF_RECORD_DEV`）的功能：
evsel 已经可以各自绑定 cpu/线程，多数需要"子设备"的场景可以直接在同一个 evlist 里用
多个 evsel 表达。

---

## 4 实施顺序

| 阶段 | 内容 | 影响面 |
| --- | --- | --- |
| S1 | evsel 独立 cpu/thread 绑定；mmap 记录绑定；`perf_evlist__mmap` 重写 | libperf，perf-prof 保持兼容（`map->idx` 语义不变） |
| S2 | 线程动态增删，mmap 引用计数创建/销毁 | libperf |
| S3 | 按绑定键自动分组 | libperf |
| S4 | profiler 回调 `instance` → `cpu/tid` | perf-prof 全量 profiler |
| S5 | ptrace 驱动线程增删，去掉 per-thread `prof_dev` clone | `ptrace.c`、`monitor.c` |
| S6 | order 按 mmap 堆排序 | `order.c` |
| S7 | 去掉 prof_dev 转发 | `monitor.c`、`trace.c`、`multi-trace.c` |

