# ring 生命周期与 HUP 域设计

Status: Done — implemented in commits cda0d45/014d12f (libperf/src), 02fa996 (tests), f1a7107 (docs)
Date: 2026-11-05
Base: 分支 `pr-20`（b4a8973 libperf rework / 3bfd098 profiler 适配）

本文统一称 `struct perf_mmap` 为 **ring**（一个 perf ring buffer）。"增加/解除 ring 引用"、
"处理 ring 的事件"都指它。

**ring 只是文档内的约定。** 源码中的类型、函数、变量一律沿用 mmap 命名：`struct perf_mmap`、
`perf_mmap__*`、`perf_evlist__mmap_*`、`struct perf_mmap_domain`，变量名用 `map`。

---

## 1 背景与问题

b4a8973 让 evsel 可以动态增删线程，按 `(cpu, tid, overwrite)` 共享 ring（`SET_OUTPUT`），
3bfd098 在 perf-prof 侧用 `dead_threads` + end 快照在线程退出后收割 ring。现状的问题：

1. **管理不内聚**。ring 的分配、引用、`SET_OUTPUT`、epoll 注册、creator 交接散落在
   `evlist.c` 的 `__perf_evlist__mmap_fd()` / `perf_evlist__unmap_evsel_fd()` /
   `perf_evlist__prepare_remove_fd()` 里；`evsel.c` 还有一套 evsel 私有的
   `perf_evsel__mmap_one()`；perf-prof 的 `prof_dev_reap_threads()` 需要了解 libperf 内部的
   creator 交接细节，并在交接失败时 `continue` 重试。
2. **creator 交接在新内核上不可用**。`perf_evlist__prepare_remove_fd()` 用另一个被
   `SET_OUTPUT` 重定向的 writer fd 重新 mmap 同一个 ring，再 munmap creator 的映射。被重定向的
   event 自身 `mmap_count == 0`：
   - 448f97fba901 ("perf: Convert mmap() refcounts to refcount_t"), in v6.18，起该路径触发
     refcount WARN；
   - d06bf78e55d5 ("perf: Fix refcount warning on event->mmap_count increment"), in v6.19，
     改为 `perf_mmap_rb()` 返回 `-EBUSY`（带 Fixes，会回合到 stable）。

   在这些内核上 `prof_dev_reap_threads()` 永远 `continue`，dead_threads 无限堆积。
3. **每个 writer fd 都注册 epoll**。一个 ring 有 N 个 writer 时，每次唤醒内核要对 N 个
   waitq 调 `ep_poll_callback`，`rb->poll` 是 xchg 语义，只有一个 fd 能拿到 EPOLLIN。
   epitem 内存、线程增删时的 `epoll_ctl` 也随 writer 数线性增长。
4. **线程退出由 ptrace 单方面驱动**，用 end 快照近似"该线程事件已全部写入"的边界。

## 2 内核事实（设计依据）

| 事实 | 位置 / 引入 |
|---|---|
| VMA 持有 `vma->vm_file`。`close(fd)` 不释放 event，event 未 DISABLE 时继续写 | `perf_mmap_close()` 只在 munmap 时调用 |
| ring 最后一个 VMA 被 munmap 时，所有 `SET_OUTPUT` 过来的 writer 被 `ring_buffer_attach(event, NULL)` 摘掉，之后静默丢事件 | `perf_mmap_close()` 的 `detach_rest` |
| 被重定向的 writer 随时可关，已写数据留在 ring 里 | `free_event()` 只摘自己 |
| `SET_OUTPUT` 目标可以是 ring 上任一 event：同 cpu；cpu==-1 时须同一 task；clock、write_backward 一致 | `perf_event_set_output()` |
| 任一 writer 写入都会唤醒 ring 上**所有** event 的 waitq；`perf_poll()` 读的是 `rb->poll` | `ring_buffer_wakeup()`，10c6db110d0e ("perf: Fix loss of notification with multi-event"), in v3.2 |
| task 退出时，其 task ctx 内所有 event（含已 DISABLE 的）被置为 `PERF_EVENT_STATE_EXIT`，`perf_poll()` 返回 EPOLLHUP | `perf_event_exit_event()` → `DETACH_EXIT`；179033b3e064 ("perf: Add PERF_EVENT_STATE_EXIT state for events with exited task"), dc633982ff3f ("perf: Do not POLLHUP event if it has children"), in v3.18 |
| EPOLLHUP 无法用事件掩码屏蔽，fd 留在 epoll 里会一直就绪 | epoll 语义 |
| 已 close 但 file 仍被引用的 fd 留在 epoll 里；file 最终释放时 epitem 自动摘除 | epoll 在 `__fput` 时清理 |
| `do_exit()` 中 `perf_event_exit_task()` 早于 `exit_notify()`：ptrace（未开 `PTRACE_O_TRACEEXIT`）看到线程退出时，该线程的 task-bound 事件已全部写入 ring | `kernel/exit.c` |

**简化假设**：默认内核支持 HUP（v3.18+）。遇到不支持的内核再修复。

## 3 HUP 域

ring 的 writer 按"会因为谁退出而 HUP"分组，每组称为一个 **HUP 域**：

- pid=-1 的 writer（cpu-wide、cgroup）：永远不 HUP，同一 ring 上的所有 pid=-1 writer 是**一个域**；
- task-bound 的 writer：**每个 task 一个域**。同一线程在同一 ring 上的多个 evsel 的 fd 同属一个
  域，线程退出时一起 HUP。

各类 ring：

| ring | 可能的 writer | 说明 |
|---|---|---|
| `(-1, t)` thread ring | 全部绑在 t 上 | 只有一个域，HUP 即 ring 失去全部 writer |
| `(c, -1)` cpu ring，含 pid=-1 writer | pid=-1 writer，可能混有 task-bound writer（`cpus=`+`-p`、`-C`+`-p`） | 不会因 HUP 失去全部 writer |
| `(c, -1)` cpu ring，只有 task-bound writer | 多个线程的 `cpu=c, pid=t` fd | 最后一个线程 HUP 后失去全部 writer，需回收 |

注意：`cpu=c, pid=t` 的 fd 写入 `(c, -1)` ring，不存在单独的 `(c, t)` ring。

### 3.1 epoll 注册集合：数据角色与 HUP 角色

注册的 fd 承担两种角色，二者独立选择：

- **数据角色（每个 ring 一个）**：注册 EPOLLIN，用来读 ring 的数据。任一 writer 写入都会经
  `ring_buffer_wakeup()` 唤醒 ring 上所有 event，所以一个 fd 足够。优先选 pid=-1 域（它不会因
  HUP 消失），没有 pid=-1 域时选一个 task 域。
- **HUP 角色（每个线程一个）**：只关心 EPOLLHUP（不注册 EPOLLIN）。线程退出时它的所有 event
  同时进入 EXIT，所以一个 fd 足够。优先选已经是某个 ring 数据 fd 的域，这样一个 fd 兼任两种角色。

一个域只要承担其中任一角色就注册一个 fd，EPOLLIN 只在承担数据角色时设置。

为什么需要两种角色：`(c,-1)` ring 同时有 pid=-1 writer 和 task-bound writer 时（task-state 这类
cpu 事件 + `-p` 的 task-bound 事件），数据角色在 pid=-1 域上；如果只按 ring 注册一个 fd，
task-bound writer 的 fd 都不在 epoll 里，线程退出的 EPOLLHUP 永远收不到，`del_thread` 一直等待、
fd 永不关闭。HUP 角色保证每个线程至少有一个 fd 在 epoll 里。

各类 ring 的注册结果：

| 场景 | 注册的 fd |
|---|---|
| thread ring `(-1,t)` | 1 个，兼任该 ring 的数据角色和 t 的 HUP 角色 |
| `(c,-1)`，只有 pid=-1 writer | 每个 ring 1 个 |
| `(c,-1)`，只有 task-bound writer，N 个线程 | 每个 ring 1 个数据 fd + 每个线程 1 个 HUP fd（尽量与数据 fd 重合） |
| `(c,-1)`，pid=-1 与 task-bound 混合 | 每个 ring 1 个 pid=-1 数据 fd + 每个线程 1 个 HUP fd（仅 EPOLLHUP） |

角色变化时（域失去最后一个 fd、线程 HUP、pid=-1 writer 加入/离开），只重选该 ring 的数据域和
该线程的 HUP 域，当前选择仍然有效就保持不变；只对获得或失去角色的域增删注册，复杂度 O(1)，
只有当前选择失效时才遍历域链表。一个线程 HUP 时它的所有域同时死亡，不为它重选 HUP 域。

## 4 ring 的结构与引用

```c
struct perf_mmap {                 /* ring */
	void		*base;
	int		 cpu;              /* (cpu, tid, overwrite) 为登记键 */
	pid_t		 tid;
	bool		 overwrite;
	refcount_t	 refcnt;
	int		 owner_fd;         /* VMA 依附的 event 的 fd，见 4.2 */
	int		 nr_domains;       /* 存活 HUP 域数（含 pid=-1 域） */
	bool		 registered;       /* 仍在 evlist 登记表里 */
	struct perf_evlist *evlist;
	...
};
```

### 4.1 引用来源

| 持有者 | 获取 | 释放 |
|---|---|---|
| task 域 | 初始 `perf_evlist__mmap()` 或 `perf_evlist__add_thread()` 时该线程首次在此 ring 上出现 | 该线程 HUP（`perf_evlist__hup()`） |
| pid=-1 域 | 第一个 pid=-1 writer 加入 | `perf_evlist__munmap()` / `close_cpu` |
| order 堆 | `order_mmap_add()` | `order_mmap_del()` |

- `nr_domains` 归 0 → ring **没有 writer**：从 evlist 登记表、epoll 中移除（`registered = false`）。
- `refcnt` 归 0 → munmap、关闭 owner_fd、free。**最后一次 put 不得访问 evlist**（retire 后
  evlist 可能已经销毁）。
- **只有 HUP 才会让 ring 没有 writer**。例外：设备关闭（`munmap` / `close`）无条件释放全部。
- 处理 ring 事件（`perf_event_handle_mmap()`）**不**增减引用。前提：ring 只会在 HUP 处理或设备
  关闭时释放，设备关闭必须延迟到当前 ring 处理完成之后（见 9.3）。

### 4.2 owner fd

ring 的 VMA 依附在创建它的 event 上。该 event 必须活到 ring 释放：

- 创建 ring 的 fd 记为 `owner_fd`。它所属的 evsel 槽位要关闭这个 fd 时，若 ring 仍有引用，
  则只 DISABLE、不 close，交给 ring 在最后一次 put 时关闭（evsel 槽位置为 -1）。
- owner 可能是一个已退出线程的 event（`(c,-1)` ring 只有 task-bound writer 时），它已 EXIT、
  不再写入，只撑住 VMA。它只持有 `event->hw.target` 的 task_struct 引用（`perf_event_alloc()`
  中 `get_task_struct()`，`_free_event()` 中释放），不影响进程回收：waitpid 照常走
  `release_task()`，线程从 pid 哈希、线程组和 `/proc` 中摘除，pid 号可被复用（event 只持有
  pid namespace，不持有 `struct pid`）。代价只是一个 task_struct 的内存；mm、files、内核栈
  在退出时已经各自释放。每个 ring 只有一个 owner，这类 event 的数量不超过 ring 数。
- 新 writer `SET_OUTPUT` 的目标用 owner_fd（cpu ring 同 cpu 即可；thread ring 的 owner 与
  writer 同 task）。

## 5 分层

### 5.1 mmap.c — ring 的基础操作（不认识 evlist）

```c
struct perf_mmap *perf_mmap__new(int cpu, pid_t tid, bool overwrite);
int  perf_mmap__mmap(struct perf_mmap *map, struct perf_mmap_param *mp, int fd); /* 设 owner_fd */
int  perf_mmap__attach(struct perf_mmap *map, int fd);   /* SET_OUTPUT(fd -> owner_fd) */
void perf_mmap__get(struct perf_mmap *map);
void perf_mmap__put(struct perf_mmap *map);              /* 归 0：munmap、close(owner_fd)、free */
bool perf_mmap__registered(struct perf_mmap *map);
```

### 5.2 evlist.c — 登记、HUP 域、epoll、线程状态

- `(cpu, tid, overwrite)` 哈希 + 链表；查找、创建、attach。
- watermark evsel 先建 ring 的两轮顺序保留（`rb->watermark` 由 creator 的 attr 决定）。
- 维护每个 ring 的 HUP 域与注册集合（3.1）；调用 external 的 add/del。
- 维护每个线程的退出状态机（第 7 节）。
- 删除：`perf_evlist__prepare_remove_fd()`、`map->fd`/`mmap_fd` 并存、`nr_fds`、
  `unmap_evsel_fd()` 中对全部 evsel×cpu×thread 的遍历、`perf_evsel__mmap_one()`
  在 `add_thread` 中的使用（evsel 私有 mmap 若保留，改用 mmap.c 的同一组函数）。
- `map->idx` / `next_mmap_idx` 不再随线程增删增长（perf-prof 已按 `(cpu, tid)` 存状态）。

### 5.3 evlist.c 的数据结构与函数

#### 数据结构

```c
/* 一个 ring 上的一个 HUP 域 */
struct perf_mmap_domain {
	struct perf_mmap	*map;
	pid_t			 tid;          /* -1：pid=-1 域 */
	int			 nr_fds;       /* 本域的 writer fd 数 */
	int			 poll_fd;      /* 已注册 epoll 的 fd，-1 表示未注册 */
	bool			 dead;         /* 已因 HUP 释放 ring 引用 */
	struct list_head	 map_node;     /* map->domains */
	struct list_head	 thread_node;  /* perf_evlist_thread->domains */
	struct hlist_node	 poll_node;    /* evlist->poll_heads，poll_fd -> 域 */
};

/* 一个线程的退出状态（第 7 节） */
struct perf_evlist_thread {
	pid_t			 tid;
	unsigned		 state;        /* THREAD_HUP | THREAD_DEL */
	struct perf_mmap_domain	*hup_domain;   /* HUP 角色 */
	struct list_head	 domains;      /* 该线程在各 ring 上的 task 域 */
	struct hlist_node	 node;         /* evlist->thread_heads */
};

struct perf_mmap {         /* 新增 */
	struct list_head	 domains;
	struct perf_mmap_domain	*cpu_domain;   /* pid=-1 域，可为 NULL */
	struct perf_mmap_domain	*poll_domain;  /* 数据角色 */
	int			 nr_domains;   /* 未 dead 的域数 */
	bool			 hashed;       /* 在查找哈希中 */
	bool			 registered;   /* 在 evlist ring 链表中 */
};

struct perf_evsel_fd {     /* mmap 指针换成域指针，map = domain->map */
	int fd, group_fd;
	struct perf_mmap_domain	*domain;
};

struct perf_evlist {       /* 新增 */
	struct hlist_head	 thread_heads[...];
	struct hlist_head	 poll_heads[...];
};
```

#### 函数

源码布局：下面 A–F 每一组函数在 `evlist.c` 内连续放在一起，按 A→F 的顺序排列（被调用者在前），
组与组之间不交叉；每组前加一段注释说明本组职责。evsel.c 的配套函数同样集中放在一处。

**A. ring 登记（static）**

| 函数 | 作用 |
|---|---|
| `perf_evlist__mmap_hash(cpu, tid, ovw)` | 登记键哈希 |
| `perf_evlist__find_mmap(evlist, cpu, tid, ovw)` | 只查 `hashed` 的 ring |
| `perf_evlist__mmap_create(evlist, evsel, cpu, thread)` | `perf_mmap__new()` + `perf_mmap__mmap()`（owner_fd = 当前 fd），登记 |
| `perf_evlist__mmap_register(evlist, map)` | 加入查找哈希和 ring 链表 |
| `perf_evlist__mmap_unhash(evlist, map)` | 只从查找哈希摘除（del 时防 tid 复用），仍在链表中供读取 |
| `perf_evlist__mmap_unregister(evlist, map)` | 从哈希和链表摘除，`registered = false`；`nr_domains` 归 0 时调用 |

**B. HUP 域（static）**

| 函数 | 作用 |
|---|---|
| `perf_evlist__mmap_domain_get(evlist, map, tid)` | 查找或创建域；创建时 `perf_mmap__get(map)`、`nr_domains++`，task 域挂到 `perf_evlist_thread`，然后 `perf_evlist_poll__update()` |
| `perf_evlist__mmap_domain_put(evlist, dom)` | 置 `dead`，注销 poll fd，`nr_domains--`，`perf_mmap__put(map)`；`nr_domains` 归 0 时 unregister，返回 true（ring 失去 writer） |
| `perf_evlist__mmap_domain_free(dom)` | 域的 fd 全部关闭后释放结构体 |

**C. epoll 注册集合（static）**

| 函数 | 作用 |
|---|---|
| `perf_evlist_poll__update(evlist, map, th, dom)` | 按 3.1 重选 ring 的数据域、线程的 HUP 域（`pick_data` / `pick_hup`），只同步获得或失去角色的域 |
| `perf_evlist_poll__add(evlist, dom, fd, new_map)` | 内部 epoll 或 external add，加入 `poll_heads` |
| `perf_evlist_poll__del(evlist, dom)` | 内部 epoll 或 external del，从 `poll_heads` 摘除 |
| `perf_evlist_poll__find(evlist, fd)` | fd → 域，供 `perf_evlist__hup()` |

**D. writer fd（库内，evsel.c 调用）**

| 函数 | 作用 |
|---|---|
| `perf_evlist__mmap_attach_fd(evlist, evsel, cpu, thread)` | 取代 `__perf_evlist__mmap_fd()`：算登记键，find 或 create，`perf_mmap__attach()`，`perf_evlist__mmap_domain_get()`，`nr_fds++`，`id_add`；域还没有 poll fd 且需要时由 `perf_evlist_poll__update()` 选用此 fd |
| `perf_evlist__mmap_detach_fd(evlist, evsel, cpu, thread)` | 取代 `perf_evlist__unmap_evsel_fd()`：删 sample id，`nr_fds--`；若是本域 poll fd，换本域另一个 fd（同域不会 HUP 不同步）；`nr_fds` 归 0 且未 dead 时 `perf_evlist__mmap_domain_put()`；若是 ring 的 owner_fd 且 ring 仍有引用，所有权交给 ring，返回 false 告诉调用方不要 close |

**E. 线程状态（static）**

| 函数 | 作用 |
|---|---|
| `perf_evlist__thread_find/new/free(evlist, tid)` | `thread_heads` 维护 |
| `perf_evlist__thread_hup(evlist, th, hup)` | 对线程所有未 dead 的域 `perf_evlist__mmap_domain_put()`，收集失去 writer 的 ring 到 `hup->retired` |
| `perf_evlist__thread_disable(evlist, tid)` | 对各 evsel 调用 `perf_evsel__disable_thread()` |
| `perf_evlist__thread_close(evlist, tid)` | 对各 evsel 调用 `perf_evsel__del_thread()`（关 fd、收缩 thread_map，内部走 `perf_evlist__mmap_detach_fd()`），释放 `perf_evlist_thread` |

**F. 对外接口**

| 函数 | 作用 |
|---|---|
| `perf_evlist__mmap(evlist, pages)` | 两轮（watermark 优先）对所有 fd 调用 `perf_evlist__mmap_attach_fd()` |
| `perf_evlist__munmap(evlist)` | 无条件：所有 fd `perf_evlist__mmap_detach_fd()`，所有域 `perf_evlist__mmap_domain_put()`，pid=-1 域在此释放 |
| `perf_evlist__add_thread(evlist, tid)` | 各 evsel `perf_evsel__add_thread()`（open + `perf_evlist__mmap_attach_fd()`），再统一 enable |
| `perf_evlist__del_thread(evlist, tid)` | 状态机：无 HUP 域 → close 并返回 1；已 HUP → close 并返回 1；否则 disable、unhash thread ring、标记 DEL、返回 0 |
| `perf_evlist__hup(evlist, fd, hup)` | `perf_evlist_poll__find()` → 线程；已 HUP 返回 `tid = -1`；否则 `perf_evlist__thread_hup()`，若已 DEL 则 `perf_evlist__thread_close()` 并置 `complete` |
| `perf_evlist_poll__external(evlist, ext, add, del)` | 设置外部 epoll |
| `perf_evlist__poll_mmap(evlist, timeout, handle)` | 内部 epoll：EPOLLIN 调 handle；EPOLLHUP 先 handle 读空，再 `perf_evlist__hup()`，对 retired 读空后 put |
| `perf_evlist__next_mmap()` / `perf_evlist__for_each_mmap()` | 遍历 `registered` 的 ring |

删除：`perf_evlist__prepare_remove_fd()`、
`perf_evlist__mmap_unmap_cb()`、`perf_evlist__link_mmap()`、
`perf_evlist__mmap_idx()`、`perf_evlist_poll__set_ops()`、`perf_evlist_poll__foreach_fd()`、
`perf_evlist_poll__del_fd()`。

evsel.c 配套：新增 `perf_evsel__disable_thread()`；`perf_evsel__close_one()` 改为调用
`perf_evlist__mmap_detach_fd()` 并按返回值决定是否 close；`perf_evsel__add_thread()` 不再调用
`perf_evsel__mmap_one()`。

## 6 external epoll 接口

```c
typedef int  (*perf_evlist_poll_add_t)(void *external, int fd, unsigned events,
				       struct perf_mmap *map, bool new_map);
typedef void (*perf_evlist_poll_del_t)(void *external, int fd, struct perf_mmap *map);

/* 合并原 perf_evlist_poll__external() 与 perf_evlist_poll__set_ops() */
void perf_evlist_poll__external(struct perf_evlist *evlist, void *external,
				perf_evlist_poll_add_t add, perf_evlist_poll_del_t del);
```

- 删除 `perf_evlist_poll__foreach_fd()`：注册哪些 fd 完全由 libperf 决定，ring 创建或注册集合
  变化时直接调用 add。
- add 的语义是"libperf 新增一个 poll fd"，不仅限于新建 ring。`new_map == true` 表示该 ring 首次
  出现，perf-prof 在此做 `order_mmap_add()`，ptrace 不再自己遍历 ring。
- del 在 HUP 处理、注册集合切换、设备关闭时调用。

## 7 线程退出：两信号状态机

线程退出有两个信号：**HUP**（`perf_evlist__hup()`）与 **del**（ptrace 调
`perf_evlist__del_thread()`）。到达顺序不确定，两者各管一半，**fd 由后到者关闭**。

| 事件 \ 状态 | 未标记 | 已有 HUP | 已有 del |
|---|---|---|---|
| `perf_evlist__hup()` | 从 epoll 删除该线程注册的 fd；释放该线程所有域对 ring 的引用；收集失去 writer 的 ring；**不关 fd**；标记 HUP | 无操作（同线程后续 HUP） | 同"未标记"，然后**关闭 fd、收缩 thread_map**，返回 complete |
| `perf_evlist__del_thread()` | 对该线程所有 fd `DISABLE`；从查找哈希摘掉该线程的 thread ring（tid 可能被复用，旧 ring 上 `SET_OUTPUT` 会 `EINVAL`）；**不关 fd**；标记 del | **关闭 fd、收缩 thread_map**，返回 complete | 无操作 |

要点：

- HUP 只管 ring（epoll、引用、去登记），del 只管 evsel（disable、thread_map），两者隔离。
- fd 在两个信号都到之前不关闭：epoll 里注册的始终是有效的 evsel fd，不存在 fd 号复用或 HUP
  因 file 释放而丢失。
- del 中的 `DISABLE`：当前 `src/ptrace.c` 未开 `PTRACE_O_TRACEEXIT`，ptrace 看到退出时 event
  已 EXIT，DISABLE 只是保险；但将来开启 TRACEEXIT 或 del 用于非退出场景时是必需的。
  DISABLE 不影响 HUP（2 节）。
- 一个线程有多个 HUP 域（自己的 thread ring + 各 cpu ring）。收到**第一个** HUP 即处理该线程的
  全部域：线程退出时其所有 event 同时 EXIT，未注册 epoll 的 writer 也已停止写入。
- 线程在本设备没有 task-bound fd（如 task-state + `-p`，全部是 pid=-1 事件）：没有 HUP 域，
  `del_thread` 直接返回 complete。

### 7.1 返回值

```c
/* 1：两个信号都已到达，可以执行 prof->del_thread；0：等待另一个；<0：错误 */
int perf_evlist__del_thread(struct perf_evlist *evlist, pid_t tid);

struct perf_evlist_hup {
	pid_t tid;                  /* 退出的线程；-1 表示该 fd 已处理过 */
	bool  complete;             /* 同 del_thread 返回 1 */
	int   nr_retired;
	struct perf_mmap **retired; /* 没有 writer 的 ring，已去登记，每个附带一份引用交给调用方 */
};
int perf_evlist__hup(struct perf_evlist *evlist, int fd, struct perf_evlist_hup *hup);

int perf_evlist__add_thread(struct perf_evlist *evlist, pid_t tid); /* 新域取 ring 引用并注册 poll fd */
```

## 8 perf-prof 侧

### 8.1 external epoll 处理

```
EPOLLIN(fd, map):
    处理 map 的事件（不增减引用）

EPOLLHUP(fd, map):
    处理 map 的事件（读到当前 head）
    perf_evlist__hup(evlist, fd, &hup)
    for map in hup.retired:
        收割 map 剩余事件（order_drain 到 head）
        order_mmap_del(map)        // 释放 order 的引用
        perf_mmap__put(map)        // 释放 retired 附带的引用
    if (hup.complete) thread_done(dev, hup.tid)
```

失去 writer 的 ring 在 HUP 时已是最终状态，直接收割全部事件，不需要 end 快照。

### 8.2 order

默认的堆排序器保证所有参与排序的 ring 上的事件按时间有序。堆持有 ring 引用：

- `new_map` 的 add 回调中 `order_mmap_add()`；
- ring 失去 writer 时由 HUP 通知收割其事件并 `order_mmap_del()`，释放引用。
- ptrace 不通知 order 删除 ring。

### 8.3 ptrace（回收 ring 的唯一开关）

```
新线程:   perf_evlist__add_thread(evlist, tid)     // order 的添加由 add 回调完成
线程退出: if (perf_evlist__del_thread(evlist, tid) == 1)
              thread_done(dev, tid)
```

### 8.4 thread_done

```
thread_done(dev, tid):
    把所有 cpu ring 读到当前 head      // 该线程留在共享 cpu ring 里的 task-bound 事件
    prof->del_thread(dev, tid)
```

两个信号都到达时线程已退出，其 task-bound 事件都已写入 ring；thread ring 已在 HUP 时收割，
cpu ring 在此读到 head，保证 `prof->del_thread` 之后不再出现该线程的 task-bound 事件。

已知限制（与现状相同）：pid=-1 writer 产生的、关于该线程的最后几条事件（如它最后一次
`sched_switch`，发生在 `exit_notify()` 之后）可能晚于 `prof->del_thread` 到达，由 profiler
自行兜底（task-state 的 `state_dead` / comm notify）。

### 8.5 删除

- `struct dead_thread`、`dev->dead_threads`、end 快照、`prof_dev_reap_threads()` 中的
  `prepare_remove_fd` 交接与重试；
- `__addfn` / `__delfn` 与 `perf_evlist_poll__foreach_fd` 的配合；
- `perf_event_handle_mmap()` 中的 `perf_mmap__get/put`（前提见 9.3）。

`prof_dev_pollfd_gone()` 中"`nr_pollfd` 归 0 即关闭设备"保留，`nr_pollfd` 的含义变为"已注册的
poll fd 数"。

## 9 边界情况

1. **只有 HUP 没有 del**：`-p` 未开 `--ptrace`。ring 引用照常释放；fd 与 `prof->del_thread`
   留到设备关闭。线程数是初始集合，有上界。
2. **只有 del 没有 HUP**：线程没有 task-bound fd，del 直接 complete（第 7 节）。
3. **ptrace detach 而线程未退出**（`src/ptrace.c` unlink 路径、设备关闭）：等不到 HUP。设备关闭时
   `perf_evlist__munmap()` / `close()` 无条件关闭所有 fd、释放所有 ring。若某条 detach 路径不会
   随后关闭设备，`del_thread` 需要 force 参数自行关 fd、释放域引用。
4. **tid 复用**：del 时把 thread ring 从查找哈希摘除；若 HUP 先到，ring 已去登记。新线程总是建新 ring。
5. **非 leader 线程 exec**（`de_thread`）：旧 leader 退出触发 `(-1, tgid)` 的 HUP；exec 的线程
   tid 变为 tgid，其原 `(-1, T)` ring 继续写入。需要 ptrace 处理 `PTRACE_EVENT_EXEC` 时重新映射，
   不在本设计范围。
6. **inherit**：有子 event 时不 HUP，直到子 event 全部退出（dc633982ff3f），语义正确。
7. **overwrite ring**：登记键含 overwrite，`SET_OUTPUT` 要求 write_backward 一致；以 events=0
   注册只等 HUP，规则相同。
8. **EPOLLERR**：只轮询每个域的一个 fd 后，看不到其他 writer 的 EPOLLERR（pinned 硬件事件调度
   失败）。tracepoint / 软件事件不会出现；以后用于硬件事件时可在 interval 中 read 补查。
9. **system_wide evsel**：不进 poll，按 pid=-1 域处理，不影响注册集合以外的逻辑。
10. **group**：关闭 leader 会让组成员脱离分组，按原逻辑处理，与 ring 生命周期无关。

### 9.3 处理 ring 事件期间关闭设备

sample 回调可能触发 `prof_dev_close()`（`-N`、hangup）。去掉 `perf_event_handle_mmap()` 的
get/put 后，必须确认 evlist munmap 被延迟到当前 ring 处理结束（`dev->inclose` /
`prof_dev_get/put` 的延迟释放）。否则保留 get/put。

## 10 给过滤器换代预留

过滤器换代（同一 evsel 的同一 cpu 槽位开新一代 fd、设新过滤器、关旧一代）是独立功能，本次不实现。
本设计保证接入时不必返工：

1. `perf_mmap__attach()` 以 fd 为参数，不以 evsel 槽位为参数；同一槽位可短暂并存新旧两个 fd。
2. owner 不依赖具体 writer：旧一代若是 owner，只 DISABLE 不 close，由 ring 持有（4.2）。
3. cpu ring 的 pid=-1 域在换代期间不会归 0（先 attach 新一代、再 detach 旧一代），注册集合不变，
   不触发 epoll 操作。
4. `struct perf_sample_id` 预留 generation 字段，读路径按 id 去重；ring 层不感知。

## 11 测试计划（lib/perf/tests/test-bind.c）

1. 多 writer 的 ring 只注册一个 poll fd（含 pid=-1 writer）。
2. 只有 task-bound writer 的 `(c,-1)` ring：每线程一个 poll fd；owner 线程先退出后，其他线程的
   数据仍可读到；最后一个线程 HUP 后 ring 失去 writer、收割、释放。
3. 同一线程的多个 evsel、多个 HUP 只处理一次。
4. 两信号状态机：del 先到（fd 被 DISABLE 不关，HUP 后关闭，返回 complete）；HUP 先到（del 关闭
   并返回 1）；无 task-bound fd 的线程 del 直接返回 1。
5. tid 复用：旧 thread ring 未释放时新线程建新 ring。
6. pid=-1 writer 加入/离开时注册集合正确切换。
7. thread ring HUP 后的引用：order 持有时 ring 保持映射，order 释放后 munmap。

perf-prof 侧：`tests/` 中多线程 workload 下 `--ptrace` 的 trace / task-state / multi-trace，
线程频繁创建退出，`--memleak-check` 无泄漏、fd 数不增长。

## 12 实施与提交

1. libperf 源码：mmap.c 基础操作、evlist.c 登记/HUP 域/状态机/接口合并，删除 prepare_remove_fd；
   附 test-bind 用例（lib/perf/tests 随 libperf 源码提交）。
2. perf-prof 源码：monitor.c 的 external epoll、HUP 处理、thread_done，ptrace.c、order.c 适配；
   删除 dead_threads。
3. tests：perf-prof 侧测试。
4. docs：**不改旧文档**（`docs/evsel_binding_design.md` 保持原样）。实现完成后新写一篇
   `docs/libperf_ring_lifetime.md`，完整描述 libperf 库 ring 生命周期管理的整体逻辑，独立成文，
   不依赖本 plan 和旧文档。大纲：
   1. 背景与问题——**从 origin/main 开始阐述**。核心问题只有一个：**如何跟踪新创建的线程**。
      - 内核 `attr.inherit` 能自动跟到子线程，但有性能问题（写文档时对照内核源码给出具体开销与
        commit：fork 时逐个复制 event 与 context、每次上下文切换的 ctx 切换开销、子 event 写入
        父 event 的 ring 带来的跨 cpu 竞争等），且用户态感知不到线程的创建与退出；
      - 因此改用 ptrace（`PTRACE_O_TRACECLONE` 等）在用户态跟踪线程的创建与退出；
      - origin/main 的 libperf 在打开后 fd、ring 都是固定的，无法随线程增删调整，只能给每个新
        线程 clone 一个完整 prof_dev（evlist、evsel、ring、order 堆全部复制）；
      - libperf 重构要做的，就是让 evlist/evsel 在打开之后**动态调节**：随线程创建打开 fd、
        接入或创建 ring，随线程退出收割 ring、关闭 fd；evsel 级绑定、按 `(cpu, tid, overwrite)`
        共享 ring 都是为此服务；
      - 由此引出 ring 生命周期的各个子问题：ring 何时创建、谁撑住映射、何时回收、epoll 注册什么、
        线程退出时如何保证事件收割完整；
      - 第一版实现（creator 交接、每 writer 注册 epoll、end 快照）的问题及其被替换的原因。
   2. 内核事实——第 2 节全部表项，附 commit 与版本（重点：VMA 持有 file、最后一个 VMA 解除时
      摘除全部 writer、`SET_OUTPUT` 约束、`ring_buffer_wakeup()` 唤醒全部 event、退出时 EXIT/HUP
      对已 DISABLE 的 event 也成立、HUP 不可屏蔽、v6.18/v6.19 禁止对重定向 event mmap、
      `perf_event_exit_task()` 早于 `exit_notify()`、owner 只持有 task_struct 不阻碍回收）。
   3. 概念：ring、writer、owner、HUP 域、注册集合。
   4. 绑定与共享：evsel 级绑定、登记键、watermark 优先的两轮 mmap。
   5. 引用与生命周期：三类引用来源、失去 writer 与释放的区别、owner fd 延后关闭。
   6. epoll：数据角色（每 ring 一个）与 HUP 角色（每线程一个）、external 接口、`new_map`。
   7. 线程增删：add_thread、两信号状态机、返回值、tid 复用。
   8. 与 perf-prof 的接口：HUP 处理、order、ptrace、thread_done。
   9. 边界情况与已知限制。
   10. 为过滤器换代预留的设计点。

按 CLAUDE.md 的提交拆分规则：若改动回填到 b4a8973 / 3bfd098 的未推送提交，用
`--fixup` + `rebase --autosquash`。
