# libperf 动态线程支持：ptrace + 单 evlist 原生增删线程

Status: 设计中 — 已批准，实现从 Phase 1 起（libperf 存储地基）。落地后回填
`Done — implemented in commits <src>/<tests>/<docs>`。

## Context（为什么做）

perf-prof 在 `-p PID` 模式下需要跟踪目标进程运行期间新建/退出的线程。当前
`perf_evlist` 关联的 `perf_thread_map` 是**静态**的：`perf_evsel.{fd,mmap,sample_id}`
是一次性分配的 `[ncpus][nthreads]` 固定矩阵（`xyarray`），线程一旦确定就钉死。
新线程无法再 `perf_event_open + mmap`，退出线程无法 `close + munmap`。

现状用 ptrace 捕获 `PTRACE_EVENT_CLONE`，对每个新线程 `prof_dev_clone()` **克隆一整个
prof_dev**（`src/ptrace.c:377`，全项目唯一功能调用点）。粒度过粗：每线程一份 evlist +
mmap + order + timer + convert + profiler private（Python 解释器 / SQLite 引擎会被重复
初始化，聚合语义错乱）。

`docs/thread-tracking-design.md` 是评估 **inherit 方案**可行性的讨论稿（结论：inherit 在
768 核上放大 fork 延迟 ~10ms，不可行），**不是最终方案**。最终方案 = **ptrace +
libperf 库原生支持动态线程**：单个 prof_dev 的单个 evlist 在运行期动态增删 per-thread
ringbuffer，order/timer/convert/profiler 状态保持单点。

**目标产出**：新线程 → 在既有 evlist 上就地 open+mmap+登记；线程退出 → 排空后 close+unmap；
零漏采样保持；768 核下 fd 数 = 线程数（非 CPU×线程）。触发源仅 **ptrace CLONE/EXIT**
（本次不做 PERF_RECORD_FORK；不破坏既有 `--inherit` 路径，但动态增删仅在 `!inherit` 的
thread-attach 模式启用）。

## 关键约束（已用真实代码核实，file:line 见下）

1. **动态性只有 thread 一维**：thread-attach 模式下 cpu 恒为 dummy（`ncpus==1`），
   system-wide 模式 `nthreads==1`（dummy）不动。所以增长永远是 y 维、x==1 的连续追加。
   `xyarray__entry` 偏移 `x*row_size + y*entry_size`（`xyarray.h:23`），x==1 时是扁平
   `[y]`，追加即尾部扩展。**`xyarray__reallocy` 必须断言 `max_x==1`**。
2. **`evlist->mmap[]` 是值数组，含大量长生命周期内部裸指针**：`mmap_first`/`->next` 链
   （`evlist.c:561,826`）、`map->evlist` 自引用（`evlist.c:363`）、order 的
   `perf_mmap_event.map`（`order.c:251,462`）、epoll `data[].mmap`（`evlist.c:419`）。
   `realloc` 移动会全部悬空 → **必须指针化**为 `struct perf_mmap **`（每个 mmap 独立
   `zalloc`，地址稳定）。
3. **order 存裸指针、只对 dev 持引用**：`heap_event->event` 直接指向 ring（`order.c:502`，
   无 memcpy）；`order_heap_init` 只 `prof_dev_get(heap_event->dev)`（`order.c:689`），
   不对 mmap 持引用。现状靠 per-thread dev 被 order 引用而存活；单 evlist 模型丢了这个安全网，
   **必须给 mmap 加引用计数**。`perf_mmap__get/put` 已存在（`mmap.c:69-80`，put→munmap at 0）。
4. **`perf_thread_map` 是柔性数组**（`threadmap.h:15`），`realloc` 会移动对象
   （`threadmap.c:18`）；`evlist->threads`、`dev->threads`、每个 `evsel->threads` 都指向
   同一对象 → 移动全悬空。
5. **epoll `data[]` 固定大小**：`nr_alloc = nr_entries*ncpus*nthreads` 一次性分配
   （`evlist.c:387`），`perf_evlist_poll__add` 满了返回 `-ENOMEM`（`evlist.c:414`），
   **无扩容路径**。epoll 为 external（`monitor.c:2328`），fd 由 monitor `__addfn/__delfn`
   → `main_epoll_add/del` 挂载（`monitor.c:2175-2186`）。
6. **`evsel->id[]` 是独立扁平数组**（`evsel.c:525`，`zalloc(ncpus*nthreads)`），
   `id_add` 用追加计数 `evsel->id[evsel->ids++]`（`evlist.c:284`）——**非 xyarray，要单独扩**。
7. **order 的 `perf_mmap_event`/heap 在 `order_init` 按 `nr_mmaps` 一次性定容**
   （`order.c:202,223,246-253`）；`order_init` 之后加入的线程没有 `perf_mmap_event`、不会被排序。
8. **profiler per-instance 状态按 `ins` 索引、init 时按 `prof_dev_nr_ins`(=`perf_thread_map__nr`)
   定容**（`monitor.c:76-85`）。`ins` = mmap idx = thread slot（`monitor.c:2145,2253`；
   `order.c:463`）。线程增长使 `ins` 越界 → profiler OOB。**libperf 修不了，必须 profiler 层协调。**

## 目标架构

**thread-major、grow-only、slot 复用**：
- thread_map 单调增长（high-water mark），退出线程的 slot 标记为空闲（`pid=-1, fd=-1`），
  由下一个新线程复用。**永不压缩** → order 的 `ins`/mmap 链/索引全部稳定。
- 每个 evsel 的 `fd`、`sample_id`、`id[]` 与 `evlist->mmap[]`（指针化后）、`thread_map`、
  epoll `data[]` 在增删时锁步伸缩。
- order 与 profiler 通过显式 API 同步线程增删。

## 实施阶段

### Phase 1 — libperf 存储地基（无行为变化，仅使可增长）
- **`lib/perf/xyarray.c`**：新增 `int xyarray__reallocy(struct xyarray **xyp, size_t new_ylen)`，
  grow-only，断言 `max_x==1`；realloc 后更新 `max_y`/`entries`/`row_size` 三者，新尾清零。
- **`lib/perf/threadmap.c` + `include/internal/threadmap.h`**（约束 4 修复，推荐方案）：
  把 `struct perf_thread_map` 改成**稳定 header + 独立分配的可增长 `map` 指针**
  （`struct thread_map_data *map` 取代柔性数组）。header 地址永久稳定，
  `evlist->threads`/`dev->threads`/`evsel->threads`/瞬时缓存全部免于悬空。
  改动波及所有 `map->map[i]` 访问点（threadmap.c、`perf_thread_map__pid/__idx/__set_pid`、
  `evlist.c:528`），均为字段访问改写，非生命周期改写。
- **`lib/perf/evlist.c`**：`evlist->mmap`/`mmap_ovw` 指针化为 `struct perf_mmap **`
  （约束 2）；改 `perf_evlist__alloc_mmap`、`perf_evlist__mmap_cb_get`、`perf_evlist__munmap`、
  `perf_evlist__next_mmap`。每个 `perf_mmap` 独立 `zalloc`，地址稳定。
- **`lib/perf/evlist.c`**：新增 `int perf_evlist_poll__grow(struct perf_evlist *evlist, int extra)`
  —— realloc `epoll->data[]`、更新 `nr_alloc`（约束 5）。

### Phase 2 — libperf 动态增删 API
- **`lib/perf/evsel.c`**：
  - `int perf_evsel__grow_thread(struct perf_evsel *evsel, int slot)`：`xyarray__reallocy`
    扩 `fd`(置 -1) 与 `sample_id`(清零)，realloc 扩 `id[]`。
  - `void perf_evsel__release_thread(struct perf_evsel *evsel, int slot)`：`close` fd 置 -1、
    `hlist_del` 该 slot 的 `sample_id.node`、清零 sid。
  - **`id[]` 按 slot 索引**（`id[slot]` 而非追加计数 `ids`），使复用原地覆盖、无需压缩（约束 6/E）。
- **`lib/perf/evlist.c`**：两个公共入口（回调式，把 epoll 所有权留在 monitor）：
  ```c
  typedef int (*perf_evlist_thread_fd_cb)(int fd, unsigned events,
                                          struct perf_mmap *mmap, void *arg);
  int perf_evlist__add_thread(struct perf_evlist *evlist, pid_t tid,
                              perf_evlist_thread_fd_cb add, void *arg);   /* 返回 slot 或 -errno */
  int perf_evlist__remove_thread(struct perf_evlist *evlist, pid_t tid,
                                 perf_evlist_thread_fd_cb del, void *arg);
  ```
  - `add_thread`：找空闲 slot 或增长全部数组 1 格 → 先 `poll__grow` → **按 leader-first
    顺序**为每个 evsel `perf_event_open` (cpu0, slot) fd（`get_group_fd` 依赖 leader 先开，
    约束/风险 1）→ 复用 `mmap_per_evsel` 两遍逻辑（watermark 遍 + SET_OUTPUT 遍；overwrite
    则再来一组，风险 8）建/映射该线程 ring → `id_add_fd` → 对每个新 fd 调 `add` 回调
    （monitor `__addfn` → `main_epoll_add`）。
  - `remove_thread`：定位 slot → 对每个 fd 调 `del` 回调（`__delfn` → `main_epoll_del`）→
    `close` fd → `hlist_del` id → **不立即 munmap**，`perf_mmap__put` 掉 base 引用（order 仍
    持引用则延迟）→ 标记 slot 空闲（`pid=-1`，保 high-water `nr`）。

### Phase 3 — order 协调（协调 order 排序逻辑一起设计）
- **`src/order.c`**：新增 `order_add_mmap(dev, map, ins)` / `order_remove_mmap(dev, map, ins)`：
  - add：为新 ring 追加一个 `perf_mmap_event` 到 `heap_event_list`，按需扩 `order.data`/heap
    （沿用 `order_heap_init`/`order_register` 已有的按需扩容，`order.c:333,677`）。
  - remove：把该 map 的 `perf_mmap_event` 标记为退出中，待其 ring 排空/被 pop 后移出。
- **mmap 引用计数（约束 3，placement 2）**：在 `order.c:689` `prof_dev_get(heap_event->dev)`
  旁，对 `type==PERF_MMAP_EVENT` 加 `perf_mmap__get(map)`；在每处配对的
  `prof_dev_put`（`order.c:782,959,977`）旁加 `perf_mmap__put(map)`。这样退出线程的 ring
  被 order 引用期间不会 munmap，order pop 最后一个引用时 refcnt→0 自动 munmap。
- 未提交的 `mmap.c` 改动（移除 `consume` 里的自动 put）与本设计一致：改为显式生命周期控制，保留。

### Phase 4 — profiler 协调（增删线程时同步到 profiler 内）
- **`src/monitor.h`**：`struct monitor` 新增可选回调
  `int (*thread_add)(struct prof_dev *dev, int ins, int tid);`
  `void (*thread_del)(struct prof_dev *dev, int ins, int tid);`
  并新增 profiler 能力标志 `bool dynamic_threads;`（是否支持就地增删线程）。
- **`src/monitor.c`**：提供 per-instance 状态增长辅助（`dev->values` 等按 `nr_ins` 定容的
  结构随 slot 扩容）；`interval_handle` 计数读循环跳过空闲 slot（fd==-1 时 `perf_evsel__read`
  返回 `-EINVAL`，`monitor.c:2252` 的 `==0` 守卫已安全，仅需确认 high-water 遍历无副作用）。
- **迁移策略**：仅 `dynamic_threads==true` 的 profiler 走新增删路径；其余暂时保留
  `prof_dev_clone` 兜底。先转换单点状态强依赖者（python/sql/multi-trace），全部转换后再退休
  `prof_dev_clone`。需审计所有按 `ins` 索引 per-instance 数组的 profiler。

### Phase 5 — ptrace 接线
- **`src/ptrace.c`**：
  - `__fork__new`（360-391）：对 `dynamic_threads` 的 dev，改 `prof_dev_clone` 为
    `prof_dev_add_thread(link->dev, child->pid)`（monitor 封装：调 `perf_evlist__add_thread`
    + `order_add_mmap` + `prof->thread_add` + 同步 `dev->threads`）。
  - `ptrace_exited`（528）：对 `dynamic_threads` 的 dev，调
    `prof_dev_remove_thread(dev, pid)`（封装：`prof->thread_del` + `order_remove_mmap` +
    `perf_evlist__remove_thread`）。
- **teardown 严格顺序**：epoll-del → close fd → hlist_del id → drop base ref（延迟 munmap）→
  标记 slot 空闲；munmap 必在 order 不再引用该 ring 之后（由 Phase 3 引用计数保证）。

### Phase 6 — 测试 + 文档
- 测试与文档独立成 commit（见下）。

## 修改文件清单（核心）
- `lib/perf/xyarray.c`、`lib/perf/threadmap.c`(+internal header)、`lib/perf/evlist.c`、
  `lib/perf/evsel.c`、`lib/perf/mmap.c`、`lib/perf/include/perf/evlist.h`（新 API 导出）、
  `lib/perf/libperf.map`（符号导出）
- `src/order.c`(+`order.h`)、`src/monitor.c`、`src/monitor.h`、`src/ptrace.c`
- 待审计/转换的 profiler（按 `ins` 索引 per-instance 状态者）

## Top 风险（实现时重点验证）
1. **`evlist->mmap[]` 指针化**（最高）：不做则 `->next`/`map->evlist`/order.map/epoll.mmap 全悬空。
2. **group_fd 顺序**：`get_group_fd`（`evsel.c:84`）要求 leader fd 先开；slot 复用时整组按
   leader-first 重开。
3. **epoll `data[]` 扩容**：不扩则 `add_thread` 超出初始 nthreads 即 `-ENOMEM`；
   `perf_evlist__munmap` 按 `nr_alloc` 遍历（`evlist.c:794`）需同步。
4. **order 定容**（约束 7）：`order_init` 后加入的线程需 `order_add_mmap` 补 `perf_mmap_event`。
5. **profiler `ins` 越界**（约束 8）：per-instance 状态必须随 slot 扩容。
6. **overwrite 模式**：每线程有 normal + ovw 两个 mmap slot，`mmap`/`mmap_ovw` 两数组都要扩/put。
7. **inherit 互斥**：`add_thread` 仅在 `!inherit` thread-attach 模式启用，避免与 inherit 双重计数。
8. **thread_map 移动**：采用稳定 header 方案（Phase 1）根除。

## 验证方法（端到端）
- 构建：`make`（必要时 `make V=1`）。
- 多线程 workload 压测新线程增删：
  `./perf-prof trace -e sched:sched_switch -p <多线程进程> --ptrace -i 1000`
  跑一个持续 `pthread_create`/线程退出的程序，确认新线程事件被采到、退出线程 ring 被回收、
  无 fd 泄漏（`ls /proc/self/fd` 计数稳定）、无 crash。
- `--order` 路径重点验证 teardown 安全：
  `./perf-prof multi-trace -e x:xx -e y:yy,task-state/untraced/ --order -i 1000 -N 50 --ptrace -- <多线程程序>`
  制造"线程退出但事件仍在 order 缓存"场景（对应 `monitor.c:2213` 注释），确认延迟 munmap 生效、
  无悬空指针（`valgrind` / ASan 若可用）。
- 内存泄漏：`cd tests && pytest --runtime=20 --memleak-check=2000`，新增
  `tests/test_ptrace_dynamic_thread.py`（遵守测试约定：`PerfProf(...).run()` 迭代 + `result_check`）。
- 回归：确认 system-wide/cpu 模式（`-C 0-5`）与 `--inherit` 路径不变。

## 提交拆分（遵守 CLAUDE.md）
1. **源码提交**（可分多个自洽 patch，libperf 地基 → 动态 API → order → profiler → ptrace）：
   `lib/perf/**`、`src/**`(.c/.h) + `src/monitor.c` 帮助字符串（若新增选项）+ `libperf.map`。
2. **测试提交**：`tests/test_*.py` 独立。
3. **文档提交**：`docs/**`、`README*.md`、`skills/**`；并在 `docs/plans/` 落一份
   `2026-08-03-libperf-dynamic-thread-design.md`（本方案），落地后回填 `Status: Done`。

## 待确认（可在实现中决策，非阻塞）
- profiler 迁移是否一次性全转 vs 能力标志渐进（本方案取渐进，保 `prof_dev_clone` 兜底至全转完）。
