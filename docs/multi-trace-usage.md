# perf-prof multi-trace 使用指南

## 1. 命令总体功能

`multi-trace` 是 perf-prof 中的**多事件关系分析器**，核心能力：

- 把多个事件按照 `key` 关联成配对（event1 → event2）
- 统计两个事件之间的**延迟分布**
- 当延迟超过阈值时，打印中间发生的所有相关事件的**时间线**

适用场景：进程调度延迟分析、系统调用延迟、内存分配生命周期追踪、中断/软中断延迟分析。

---

## 2. 事件表达式格式 (`-e`)

每个 `-e` 指定一组事件，逗号分隔：

```
-e 'EVENT1,EVENT2,EVENT3,...'
```

### 单个事件语法

```
sys:name/filter/ATTR/ATTR/.../
```

用 `/` 分隔三部分：

| 部分 | 说明 | 示例 |
|------|------|------|
| `sys:name` | tracepoint 事件名 | `sched:sched_wakeup` |
| `/filter/` | 内核过滤条件，留空 `//` 表示不过滤 | `pid==11731`、`prev_state==0&&prev_pid>0` |
| `/ATTR/.../` | 属性列表，每个属性用 `/` 分隔 | `key=pid/stack/untraced/` |

### 事件属性（ATTR）一览

| 属性 | 说明 |
|------|------|
| `key=EXPR` | 事件关联键，用于匹配 event1 和 event2。EXPR 是事件字段表达式 |
| `stack` | 采集该事件的内核调用栈 |
| `untraced` | **不参与配对**，仅作为 `--detail` 时间线上的辅助事件 |
| `trigger` | 每次触发此事件时输出一次统计 |
| `alias=NAME` | 给事件起别名 |
| `role=EXPR` | 控制事件角色：bit0(1)=可作 event2，bit1(2)=可作 event1，3=都可 |
| `top-by=FIELD` | 按此字段排序 top 输出 |
| `top-add=FIELD` | 在 top 输出中添加此字段 |
| `comm=FIELD` | 使用指定字段作为进程名 |
| `ptr=EXPR` | kmemprof 用，内存分配指针表达式 |
| `size=EXPR` | kmemprof 用，内存分配大小表达式 |
| `num=EXPR` | 数值表达式 |
| `printkey=EXPR` | 自定义 key 的打印格式 |
| `vm=UUID` | KVM 虚拟机 UUID，用于 Guest/Host 事件转换 |
| `push=NAME` | 向指定通道广播事件 |
| `pull=NAME` | 从指定通道接收事件 |
| `exec=EXPR` | 执行表达式 |
| `cpus=CPULIST` | 限制此事件在指定 CPU 上采集 |

### 嵌入子 profiler

事件位置还可以放一个完整的 profiler（而非 tracepoint）：

```
profiler_name/options/ATTR/
```

例如嵌入 CPU 采样：

```
profile/-F 200 --watermark 50 -m 16/untraced/
```

含义：
- `profile` — CPU 采样 profiler
- `-F 200` — 采样频率 200Hz
- `--watermark 50` — ringbuffer 写入 50% 时唤醒读取
- `-m 16` — 独立使用 16 pages 的 ringbuffer
- `untraced` — 不参与配对，仅补充时间线上下文

---

## 3. 两个 `-e` 的关系

```
-e 'event1 组'  -e 'event2 组'
     ↓                ↓
   起始事件         结束事件
```

**第一个 `-e` 定义 event1（起始事件），第二个 `-e` 定义 event2（结束事件）**。

工作流程：
1. event1 发生 → 按 key 备份 (backup)
2. event2 发生 → 查找同 key 的 event1
3. 计算延迟 = `event2.time - event1.time`
4. 如果延迟 > `--than` 阈值 → 打印时间线

标记了 `untraced` 的事件不参与配对，只出现在 `--detail` 的时间线输出中。

> 可以使用超过 2 个 `-e`，形成链式配对：`-e A -e B -e C` 表示 A→B 和 B→C 两对延迟分析。

---

## 4. 全局参数详解

### 核心参数

| 参数 | 说明 |
|------|------|
| `-e EVENT` | 事件表达式，可多次指定 |
| `-k FIELD` | 全局 key。对没有单独指定 `key=` 的事件，使用此字段作为关联键 |
| `-i MS` | 统计间隔（毫秒），每隔此时间输出一次延迟分布 |
| `-m PAGES` | mmap 页数，perf ringbuffer 大小。数据量大时需要增大 |
| `--order` | 启用跨 CPU 事件按时间戳排序。**使用 key 关联不同 CPU 上的事件时必须启用** |
| `--impl NAME` | 选择分析实现：`delay`(默认)、`pair`、`kmemprof`、`syscalls`、`call`、`call-delay` |

### 延迟过滤

| 参数 | 说明 |
|------|------|
| `--than N` | 延迟阈值，只统计+打印延迟 > N 的事件对。支持单位：`s/ms/us/ns` |
| `--only-than N` | 同 `--than`，但统计中也**只计入**超过阈值的事件 |
| `--lower N` | 只统计延迟 < N 的事件对 |

### 详细输出 (`--detail`)

| 参数 | 说明 |
|------|------|
| `--detail` | 启用时间线详细输出（需配合 `--than`） |
| `--detail=samecpu` | 只显示与 event1/event2 同 CPU 的事件 |
| `--detail=samepid` | 只显示与 event1/event2 同 pid 的事件 |
| `--detail=sametid` | 只显示与 event1/event2 同 tid 的事件 |
| `--detail=samekey` | 只显示与 event1/event2 同 key 的事件 |
| `--detail=-1ms` | event1 之前 1ms 范围内也显示事件 |
| `--detail=+1ms` | event2 之后 1ms 范围内也显示事件 |
| `--detail=hide<100us` | 隐藏间隔 < 100us 的事件 |

可以组合：`--detail=samecpu,-1ms,+1ms`

### 其他参数

| 参数 | 说明 |
|------|------|
| `--perins` | 按 instance (CPU/线程) 分别输出统计 |
| `--heatmap FILE` | 输出延迟热力图数据到文件 |
| `--cycle` | 循环配对，从最后一个 -e 回到第一个 |
| `-C CPULIST` | 指定 CPU 列表 |
| `-p PID` | 指定进程 PID |
| `-t TID` | 指定线程 TID |
| `--watermark N` | ringbuffer 唤醒水位 (0-100) |
| `-g` | 启用调用栈采集 |
| `-v` / `-vv` | verbose 输出 |

---

## 5. 如何查看两个事件之间发生了什么

**使用 `--than` + `--detail` + `untraced` 辅助事件**。

### 基本模式

```bash
perf-prof multi-trace \
  -e '起始事件/filter/key=xxx/' \
  -e '结束事件/filter/key=xxx/,辅助事件1//untraced/,辅助事件2//untraced/' \
  -k KEY --order -i 1000 \
  --than 阈值 \
  --detail=samecpu
```

### 步骤说明

1. **第一个 `-e`**：定义起始事件（event1）
2. **第二个 `-e`**：定义结束事件（event2），加上标记为 `untraced` 的辅助事件
3. **`key=`**：让 event1 和 event2 通过相同字段关联（如 pid）
4. **`--than`**：设置延迟阈值，超过才触发详细输出
5. **`--detail`**：启用时间线输出，用 `samecpu`/`samepid` 过滤中间事件
6. **`untraced` 事件**：时间线的"填充物"，不触发配对，但出现在 event1→event2 之间

---

## 6. 典型用例

### 用例 1：调度延迟分析（rundelay）

分析指定进程从被唤醒到被调度上 CPU 的延迟，超过 10ms 时显示同 CPU 上中间事件：

```bash
perf-prof multi-trace \
  -e 'sched:sched_wakeup/pid==11731/,sched:sched_wakeup_new/pid==11731/,sched:sched_switch/prev_state==0&&prev_pid==11731/key=prev_pid/' \
  -e 'sched:sched_switch//key=next_pid/,sched:sched_migrate_task//untraced/key=pid/,profile/-F 200 --watermark 50 -m 16/untraced/' \
  -k pid -m 256 -i 1000 --order --than 10ms --detail=samecpu
```

**简化版**（使用 `rundelay` 派生命令）：

```bash
perf-prof rundelay \
  -e 'sched:sched_wakeup*,sched:sched_switch' \
  -e 'sched:sched_switch,profile/-F 500 --watermark 50/untraced/' \
  -p 11731 -i 1000 --than 10ms --detail=samecpu
```

### 用例 2：Java 进程调度延迟 + 迁移分析

```bash
perf-prof multi-trace \
  -e 'sched:sched_wakeup/comm~"java"/key=pid/' \
  -e 'sched:sched_switch/next_comm~"java"/key=next_pid/,sched:sched_migrate_task/comm~"java"/untraced/stack/' \
  -i 1000 --order --only-than 10ms --detail=samepid
```

- `comm~"java"` — 过滤进程名包含 "java"
- `--only-than 10ms` — 只统计并打印延迟 > 10ms 的
- `--detail=samepid` — 只显示与 java 进程同 pid 的中间事件
- `sched_migrate_task` 带 `stack/` — 可以看到迁移时的调用栈

### 用例 3：软中断延迟分析

```bash
perf-prof multi-trace \
  -e irq:softirq_entry/vec==1/ \
  -e irq:softirq_exit/vec==1/ \
  -i 1000 --than 100us --order --detail=-1ms
```

- `vec==1` — TIMER 软中断
- `--than 100us` — 超过 100us 的软中断
- `--detail=-1ms` — 同时显示 entry 之前 1ms 的事件

### 用例 4：全量调度延迟分析 + CPU 采样

```bash
perf-prof multi-trace \
  -e 'sched:sched_wakeup,sched:sched_wakeup_new,sched:sched_switch/prev_state==0&&prev_pid>0/key=prev_pid/' \
  -e 'sched:sched_switch//key=next_pid/,profile/-F 200 --watermark 50 -m 16/untraced/' \
  -k pid -m 128 -i 1000 --order --than 20ms --detail=samecpu
```

- 不限定具体进程，分析所有线程的调度延迟
- `profile` 嵌入 200Hz CPU 采样，在时间线上能看到谁占了 CPU
- `prev_pid>0` — 排除 idle 线程

### 用例 5：sched_switch 自循环分析（--cycle）

```bash
perf-prof multi-trace \
  -e 'sched:sched_switch//role="(next_pid?1:0)|(prev_pid?2:0)"/' \
  --cycle -i 1000
```

- 只有一个 `-e`，使用 `--cycle` 让同一事件自己配对
- `role` 表达式：`next_pid != 0` 时可作为 event2（bit0=1），`prev_pid != 0` 时可作为 event1（bit1=2）
- 分析每个线程从被调度上 CPU 到被切走的运行时间

### 用例 6：软中断延迟分析（事件配对模式）

```bash
perf-prof multi-trace \
  -e irq:softirq_entry/vec==1/ \
  -e irq:softirq_exit/vec==1/ \
  -i 1000 --impl pair
```

- `--impl pair` — 使用配对模式，统计事件是否正确配对（而非延迟分布）
- 适用于 alloc/free、open/close 等配对检测

### 用例 7：内存分配热点分析（kmemprof）

```bash
perf-prof kmemprof \
  -e 'kmem:kmalloc//size=bytes_alloc/stack/' \
  -e kmem:kfree \
  -m 128 --order -k ptr
```

- `size=bytes_alloc` — 使用 bytes_alloc 字段作为分配大小
- `stack` — 采集分配时的调用栈
- `-k ptr` — 按 ptr（内存地址）关联 alloc 和 free

### 用例 8：系统调用延迟分析（syscalls）

```bash
perf-prof syscalls \
  -e raw_syscalls:sys_enter \
  -e raw_syscalls:sys_exit \
  -p 1 --perins
```

- 自动使用 `common_pid` 作为 key
- `--perins` — 按线程分别统计
- 输出每种系统调用的延迟分布

### 用例 9：KVM 虚拟机退出延迟 + 辅助 profiler

```bash
perf-prof multi-trace \
  -e kvm:kvm_exit \
  -e 'kvm:kvm_entry,task-state/-m 256/untraced/' \
  -t 210673 -m 128 -i 1000 --than 80us --detail=sametid
```

- 分析 KVM vCPU 线程 (tid=210673) 的 kvm_exit → kvm_entry 延迟
- `task-state` 作为嵌入的辅助 profiler（`untraced`），在时间线上展示线程状态

### 用例 10：嵌套函数调用分析（nested-trace）

```bash
perf-prof nested-trace \
  -e irq:softirq_entry/vec==1/,irq:softirq_exit/vec==1/ \
  -e timer:timer_expire_entry,timer:timer_expire_exit \
  -i 1000 --impl call-delay
```

- 分析嵌套的函数调用关系：软中断 → 定时器
- `--impl call-delay` — 同时分析调用关系和执行时间

---

## 7. 派生命令速查

| 命令 | 等价于 | 用途 | 简化之处 |
|------|--------|------|----------|
| `rundelay` | `multi-trace` + 自动配 sched 事件 | 调度延迟分析 | 自动设置 key=pid/next_pid/prev_pid，支持 `--filter` |
| `syscalls` | `multi-trace --impl syscalls` | 系统调用延迟 | 自动配置 sys_enter/sys_exit，按 syscall ID 分组 |
| `kmemprof` | `multi-trace --impl kmemprof` | 内存分配热点 | 需要手动指定 `size=`、`ptr=` 属性 |
| `nested-trace` | `multi-trace` 嵌套模式 | 嵌套函数调用 | 每个 `-e` 必须包含成对的 entry/exit 事件 |

---

## 8. --impl 实现类型

| 实现 | 说明 | 典型场景 |
|------|------|----------|
| `delay` (默认) | 延迟分布分析 | 调度延迟、中断延迟、任意两事件间延迟 |
| `pair` | 事件配对检测 | alloc/free 配对、open/close 配对、泄漏检测 |
| `kmemprof` | 内存分配统计 | 内存分配热点、已分配/已释放字节统计 |
| `syscalls` | 系统调用延迟 | 按 syscall ID 分组的延迟统计 |
| `call` | 函数调用分析 | 仅用于 nested-trace |
| `call-delay` | 函数调用 + 延迟 | 仅用于 nested-trace，同时输出调用关系和耗时 |

---

## 9. 关键概念图

```
第一个 -e (event1 组)              第二个 -e (event2 组)
┌─────────────────────┐           ┌──────────────────────────────────────┐
│ sched_wakeup        │           │ sched_switch (key=next_pid)         │
│ sched_wakeup_new    │  key 配对  │ sched_migrate_task [untraced]       │
│ sched_switch        │ ────────> │ profile [untraced]                  │
│   (key=prev_pid)    │           │                                      │
└─────────────────────┘           └──────────────────────────────────────┘
         │                                    │
         │      延迟 = event2.time - event1.time
         │                                    │
         ▼                                    ▼
    event1 备份                          查找同 key 的 event1
                                         计算延迟
                                              │
                                    延迟 > --than ?
                                         ┌────┴────┐
                                        YES       NO
                                         │         │
                                  打印时间线    仅统计
                                  (--detail)
```
