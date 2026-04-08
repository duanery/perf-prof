---
layout: default
title: "trace - 事件跟踪"
---

# trace - 事件跟踪与打印

跟踪并打印系统事件，支持调用栈记录和火焰图生成。

## 概述
- **主要用途**: 实时跟踪和显示内核/用户空间事件，记录调用栈，支持联合分析，是最基础和灵活的事件分析工具。
- **适用场景**: 需要实时查看事件发生的详细信息、分析事件序列、记录调用栈、生成火焰图、进行多事件联合分析等场景。
- **功能分类**:
  - 按事件依赖程度：**自定义事件类** - 需要用户通过 `-e` 指定事件
  - 按功能领域：**调试与工具** - 通用事件跟踪和调试工具
  - 按分析技术：**追踪分析**、**联合分析** - 支持多事件组合分析
- **最低内核版本**: 需要内核支持 perf_event 子系统 (Linux 2.6.31+)
- **依赖库**: libtraceevent (事件解析), libelf (符号解析), liblzma (可选，用于 MiniDebugInfo)
- **平台支持**: x86, ARM, ARM64, RISC-V, PowerPC 等主流架构
- **特殊限制**:
  - 需要 root 权限或 CAP_PERFMON 能力
  - kprobe/uprobe 需要内核 CONFIG_KPROBES 支持
  - BPF 过滤器需要较新内核支持 (4.1+)
- **参与联合分析**:
  - 作为联合分析主体，可以嵌入其他分析器作为事件源
  - 支持通过 `profiler[/option/]` 语法引用其他分析器
- **核心技术**:
  - 基于 perf_event 事件采样
  - trace event 内核态过滤
  - 用户态表达式过滤
  - 调用栈解析和符号化
  - 火焰图生成

## 基础用法
```bash
perf-prof trace -e EVENT [--overwrite] [-g [--flame-graph file [-i INT]]]
```

OPTION:
- `-i, --interval <ms>` - 间隔输出，配合 `--flame-graph` 周期性输出火焰图 (默认：无间隔)
- `--order` - 按时间戳排序事件，确保事件输出的时序正确性
- `-N, --exit-N <N>` - 采样 N 个事件后退出
- `--inherit` - 子任务继承计数器，跟踪创建的子进程/线程，实验性功能

FILTER OPTION:
- `--user-callchain[=dwarf[,size]]` - 包含用户态调用栈，`no-` 前缀排除 (默认：包含)。`=dwarf` 启用DWARF栈回溯，可选指定栈数据大小
- `--kernel-callchain` - 包含内核态调用栈，`no-` 前缀排除 (默认：包含)
- `--python-callchain` - 包含 Python 调用栈

PROFILER OPTION:
- `-e, --event <EVENT,...>` - 事件选择器，支持多种事件源类型
  - **tracepoint**: `sys:name[/filter/ATTR/.../]` - 系统 tracepoint 事件
  - **profiler**: `profiler[/option/ATTR/.../]` - 嵌入其他分析器作为事件源
  - **kprobe**: `kprobe:func[/filter/ATTR/.../]` - 内核函数探针
  - **kretprobe**: `kretprobe:func[/filter/ATTR/.../]` - 内核函数返回探针
  - **uprobe**: `uprobe:func@"file"[/filter/ATTR/.../]` - 用户态函数探针
  - **uretprobe**: `uretprobe:func@"file"[/filter/ATTR/.../]` - 用户态函数返回探针
  - **通配符**: 支持 `*, ?, []` 通配符匹配多个事件

- `-g, --call-graph` - 启用调用图记录，为所有事件记录调用栈
- `--flame-graph <file>` - 指定折叠栈输出文件，生成火焰图格式数据
- `--overwrite` - 使用覆盖模式，避免在高负载下丢失最新事件，实验性功能
- `--ptrace` - 使用 ptrace 跟踪新创建的线程，确保子线程也被监控

### 示例
```bash
# 基础事件跟踪
perf-prof trace -e sched:sched_wakeup -C 0

# 多事件跟踪，带过滤器
perf-prof trace -e 'sched:sched_wakeup/prio<10/,sched:sched_switch'

# 记录调用栈并生成火焰图
perf-prof trace -e sched:sched_wakeup -C 0 -g --flame-graph wakeup.folded

# 通配符匹配多个事件
perf-prof trace -e 'sched:sched_wak*' -C 0

# kprobe 跟踪内核函数
perf-prof trace -e 'kprobe:try_to_wake_up' -C 0

# uprobe 跟踪用户态函数
perf-prof trace -e 'uprobe:printf@"/lib64/libc.so.6"' -m 64

# 联合分析：嵌入其他分析器
perf-prof trace -e 'task-state,page-faults/-N 10/,raw_syscalls:sys_enter' --order

# 覆盖模式，保留最新事件
perf-prof trace -e 'sched:sched_wakeup,sched:sched_switch' --overwrite
```

## 核心原理

**基本定义**
- **事件 (Event)**: 内核或用户空间发生的可观测活动点，如系统调用、进程调度、中断等
- **tracepoint**: 内核预定义的静态跟踪点，性能开销极低
- **kprobe/uprobe**: 动态插入的探针，可跟踪任意内核/用户函数
- **调用栈 (Call Chain)**: 函数调用序列，记录代码执行路径
- **火焰图 (Flame Graph)**: 可视化调用栈的统计图表，展示性能热点

**数据模型**
```
事件源 → [内核过滤] → 环形缓冲区 → [用户态过滤] → [排序] → [栈解析] → 显示/保存
```

- **事件源**: tracepoint、kprobe、uprobe 产生原始事件
- **内核过滤**: trace event filter 在内核态执行，减少数据传输
- **环形缓冲区**: per-CPU 无锁缓冲区，高效收集事件数据
- **用户态过滤**: ftrace_filter 表达式过滤，处理复杂逻辑
- **排序**: 基于时间戳的多 CPU 事件排序 (`--order`)
- **栈解析**: 符号化内核和用户态调用栈
- **输出**: 实时打印或保存为火焰图格式

### 事件源

- **sample_type**: 默认采样类型
  - `PERF_SAMPLE_TID`: 线程 ID (PID/TID)
  - `PERF_SAMPLE_TIME`: 事件时间戳
  - `PERF_SAMPLE_ID`: 事件唯一标识
  - `PERF_SAMPLE_CPU`: 发生事件的 CPU
  - `PERF_SAMPLE_PERIOD`: 采样周期
  - `PERF_SAMPLE_RAW`: 原始事件数据
  - `PERF_SAMPLE_CALLCHAIN`: 调用栈 (当 `-g` 或 `//stack/` 时启用)

- **自定义事件**: trace 分析器需要用户通过 `-e` 参数指定事件
  - **tracepoint 事件**: `sys:name[/filter/ATTR/.../]`
    - 使用 `perf-prof list` 列出所有可用事件
    - 支持通配符: `sched:sched_wak*` 匹配 `sched_wakeup`, `sched_wakeup_new` 等
  - **kprobe/kretprobe**: 动态探针，需要内核支持
  - **uprobe/uretprobe**: 用户态探针，需要指定可执行文件路径
  - **profiler 嵌入**: 引用其他分析器作为事件源，进行联合分析

#### 过滤器

trace 支持两种过滤机制：

**1. trace event 过滤器 (内核态)**
- **语法**: 第一个 `/` 之后，第二个 `/` 之前
- **示例**: `sched:sched_wakeup/prio<10/`
- **特点**:
  - 在内核态执行，效率高
  - 支持数值比较: `==, !=, <, <=, >, >=, &`
  - 支持字符串匹配: `==, !=, ~` (通配符)
  - 支持逻辑组合: `&&, ||, ()`
- **扩展字段**:
  - `_cpu`: 事件发生的 CPU
  - `_pid`: 事件发生的进程 ID (非线程 ID)

**2. 用户态表达式过滤器 (ftrace_filter)**
- 当内核态过滤器设置失败时，在用户态执行
- 支持完整 C 表达式语法
- 可调用内置函数: `printf()`, `ksymbol()`, `comm_get()` 等
- 示例: `pid<prio` (比较两个字段)

#### 属性 (Attributes)

通过 `/ATTR/ATTR/.../` 配置事件处理行为：

- `stack`: 为该事件启用调用栈记录
- `max-stack=int`: 设置最大栈深度 (默认: 127)
- `alias=str`: 事件别名，简化输出显示
- `cpus=cpu[-cpu]`: 指定该事件仅在特定 CPU 上采样
- `exec=EXPR`: 执行表达式，用于自定义处理逻辑，如：`exec=system("/proc/%d/stat", pid)`
- `push=[IP:]PORT`: 推送事件到远程服务器 (事件传播)
- `pull=[IP:]PORT`: 从远程拉取事件 (事件传播)

### 事件处理

**事件接收流程**:
1. **环形缓冲区读取**: 从 per-CPU mmap 缓冲区读取原始事件
2. **事件识别**: 根据 `event->header.type` 识别事件类型
   - `PERF_RECORD_SAMPLE`: 标准采样事件
   - `PERF_RECORD_DEV`: 设备转发事件 (联合分析)
3. **evsel 查找**: 通过 `id` 字段查找对应的 `perf_evsel`
4. **事件匹配**: 匹配 `tp_list` 中的 tracepoint 定义

**用户态过滤 (ftrace_filter)**:
```c
long trace_ftrace_filter(struct prof_dev *dev, union perf_event *event, int instance)
```
- 在 `sample()` 之前执行，返回 0 则丢弃事件
- 解析原始事件数据，执行表达式程序
- 使用 `tp_prog_run()` 运行编译后的表达式

**事件打印**:
```c
void trace_sample(struct prof_dev *dev, union perf_event *event, int instance)
```
- 解析原始数据: `__raw_size()` 提取事件字段
- 打印事件: `tp_print_event()` 格式化输出
- 执行 exec 表达式: `tp_prog_run(tp->exec_prog)`
- 打印调用栈: `__print_callchain()`

**调用栈处理**:
- **不启用火焰图**: 使用 `callchain_ctx` 实时打印栈
  ```c
  print_callchain_common(ctx->cc, &callchain, pid)
  ```
- **启用火焰图**: 累积到火焰图结构
  ```c
  flame_graph_add_callchain_at_time(ctx->flame, &callchain, pid, comm, time, time_str)
  ```

**排序处理**:
- 使用 `--order` 时，框架自动按时间戳排序
- 多 CPU 事件按时间戳归并，确保输出顺序正确
- 需要额外缓冲区延迟，适合离线分析

**事件广播**:
- 通过 `tp_broadcast_event()` 推送到远程
- 支持 TCP、字符设备、文件三种方式
- 用于跨主机/跨虚拟机联合分析

**覆盖模式 (overwrite)**:
- 环形缓冲区写满后覆盖旧数据
- 避免高负载下丢失最新事件
- 适合保留最近事件的场景

### 状态统计
trace 分析器不处理信号，由框架统一处理：
- **SIGUSR1**: 无特殊操作
- **SIGUSR2**: 无特殊操作

## 输出

### 输出格式

**标准输出格式**:
```
[UNIX时间戳] [进程名/TID] [CPU] [时间戳] [事件名称] [字段=值 ...]
[调用栈...]
```

**字段说明**:
- **UNIX时间戳**: YYYY-MM-DD HH:MM:SS.microsec，由事件的perf clock转换而来，精度有损失
- **进程名/TID**: 进程名称和线程 ID
- **CPU**: 事件发生的 CPU 编号 (0-based)
- **时间戳**: 事件发生的精确时间，来自内核采样，可选格式:
  - 默认: perf clock (纳秒)
  - `--tsc`: TSC 时间戳计数器
  - `--kvmclock <uuid>`: Guest kvmclock
  - `--monotonic`: CLOCK_MONOTONIC
  - `--clock-offset <n>`: 时间偏移
- **事件名称**: tracepoint、kprobe 等事件名称
- **字段=值**: 事件特定字段及其值

**调用栈格式**:
```
    ffffffff81xxxxxx function_name+0xoffset (kernel)
    7fxxxxxxxxxxxx function_name+0xoffset (/path/to/library.so)
```

**火焰图格式** (`--flame-graph`):
```
进程名;函数1;函数2;函数3 采样次数
进程名;函数1;函数2 采样次数
```
- 折叠格式，每行代表一条调用路径
- 使用 `flamegraph.pl` 生成 SVG 可视化图表
- 支持时间切片: `-i` 参数周期性输出

### 关键指标

trace 分析器主要用于定性观察事件序列，不提供聚合统计指标。关注点：

- **事件频率**: 单位时间内事件发生次数，高频事件可能是性能瓶颈
- **事件延迟**: 配合 `multi-trace` 分析事件对延迟
- **调用路径**: 通过调用栈识别热点函数
- **事件序列**: 多事件间的时序关系和因果关系

### 阈值建议

trace 本身不设阈值，建议根据业务需求判断：

- **正常范围**: 根据具体事件类型和系统负载确定基线
- **异常检测**:
  - 事件频率突增/突降
  - 调用栈异常 (如死循环)
  - 事件乱序 (未使用 `--order`)
- **性能影响**:
  - 启用调用栈会增加约 2-5% 开销
  - 过滤器可显著降低数据量

## 分析方法

### 基础分析方法

**1. 确定分析目标**
- 明确要跟踪的事件类型
- 确定是否需要调用栈
- 评估是否需要过滤减少数据量

**2. 选择事件源**
```bash
# 查看可用事件
perf-prof list

# 按类别过滤
perf-prof list | grep sched:

# 查看事件字段
perf-prof trace -e sched:sched_wakeup help
```

**3. 设置过滤器**
- 优先使用内核态过滤器 (效率高)
- 复杂逻辑使用用户态表达式
- 注意过滤器语法: 数值用 `<, >, ==` 等，字符串用 `~` 通配符

**4. 配置输出方式**
- 实时查看: 标准输出
- 生成火焰图: `--flame-graph` + `-i` 间隔输出
- 离线分析: `--order` 排序 + `-o` 输出文件

**5. 调整采样范围**
- `-C, --cpus`: 限定 CPU 范围
- `-p, --pids`: 限定进程
- `-N, --exit-N`: 限制采样数量
- `--watermark`: 调整唤醒阈值

## 应用示例

```bash
# 示例 1: 跟踪进程调度事件
perf-prof trace -e 'sched:sched_wakeup,sched:sched_switch' -C 0 -N 100

# 示例 2: 分析高优先级进程的调度路径
perf-prof trace -e 'sched:sched_wakeup/prio<10/' -g --flame-graph high_prio.folded

# 示例 3: 跟踪特定进程的系统调用
perf-prof trace -e 'syscalls:sys_enter_*' -p 1234 --order

# 示例 4: 监控内存分配热点
perf-prof trace -e 'kmem:kmalloc/bytes_alloc>1024/stack/' -m 128

# 示例 5: 覆盖模式保留最新事件
perf-prof trace -e 'irq:*' --overwrite -m 64

# 示例 6: 跟踪内核函数调用
perf-prof trace -e 'kprobe:do_sys_open' -g -N 50

# 示例 7: 联合分析任务状态和页错误
perf-prof trace -e 'task-state,page-faults/-N 10/' --order -- ./myapp
```

### 高级技巧

```bash
# 技巧 1: 使用通配符批量跟踪事件
perf-prof trace -e 'sched:*' -C 0 -N 1000 | grep -i wakeup

# 技巧 2: 动态跟踪任意内核函数
perf-prof trace -e 'kprobe:schedule' -g --flame-graph schedule.folded

# 技巧 3: 跟踪用户态库函数调用
perf-prof trace -e 'uprobe:malloc@"/lib64/libc.so.6"' -p 1234

# 技巧 4: 周期性生成火焰图，观察时间变化
perf-prof trace -e 'sched:sched_wakeup' -g --flame-graph wakeup.folded -i 5000

# 技巧 5: 多层过滤组合
perf-prof trace -e 'sched:sched_wakeup/target_cpu==0 && prio<10/stack/max-stack=16/' -C 0

# 技巧 6: 使用表达式计算自定义字段
perf-prof trace -e 'sched:sched_stat_runtime//exec=printf("runtime: %llu us\n", runtime/1000)/' -C 0

# 技巧 7: 跨主机事件传播
# 主机 A (发送端)
perf-prof trace -e 'sched:sched_wakeup//push=192.168.1.100:8888/' -C 0

# 主机 B (接收端)
perf-prof trace -e 'sched:sched_wakeup//pull=0.0.0.0:8888/' --order

# 技巧 8: ptrace 跟踪子进程
perf-prof trace -e 'syscalls:sys_enter_*' --ptrace -- ./myapp

# 技巧 9: 时间戳转换
perf-prof trace -e 'sched:sched_wakeup' --tsc -C 0  # 使用 TSC 时间戳
```

### 性能优化

- **缓冲区大小**:
  - 默认: 2 pages (不启用调用栈)，4 pages (启用调用栈)
  - 高频事件: `-m 128` 或更大，避免丢失事件
  - 低频事件: `-m 4` 减少内存占用
  - 覆盖模式: `-m 1 --overwrite` 最小化内存

- **采样频率**:
  - trace 使用 `sample_period = 1`，记录每个事件
  - 通过过滤器减少事件量，而非降低采样率
  - 使用 `-N` 限制采样总数，用于快速验证

- **过滤器优化**:
  - 优先使用内核态过滤器 (`/filter/`)
  - 简化过滤表达式，避免复杂计算
  - 多条件过滤使用 `&&` 组合，提前剔除不符合的事件
  - 字符串匹配使用精确匹配 (`==`) 而非通配符 (`~`)

### 参数调优

- **--watermark 调优**:
  - 默认: 0 (使用 `wakeup_events = 1`)
  - 高频事件: `--watermark 50` 降低唤醒频率，减少上下文切换
  - 实时性要求高: 保持默认值

- **--order 优化**:
  - 离线分析必选，确保事件时序正确
  - 实时跟踪不推荐，增加延迟和内存占用
  - 多 CPU 高频事件慎用，性能开销大

- **-m, --mmap-pages 优化**:
  - 必须是 2 的幂次
  - 高频事件: 64-256 pages
  - 中频事件: 16-64 pages
  - 低频事件: 4-8 pages
  - 监控多 CPU: 适当增大 (per-CPU 分配)

- **-N, --exit-N 优化**:
  - 快速验证: `-N 100`
  - 短期分析: `-N 10000`
  - 长期运行: 不设置 (持续运行)

### 组合使用

- **与其他分析器配合**:
  ```bash
  # trace 嵌入 task-state 和 profile 进行联合分析
  perf-prof trace -e 'task-state,profile/-F 5000/' --order -p 1234

  # 先用 top 统计，再用 trace 详细跟踪
  perf-prof top -e 'sched:sched_wakeup//comm=comm/' --only-comm -i 1000
  # 基于 top 结果，跟踪特定进程
  perf-prof trace -e 'sched:sched_wakeup/comm=="myapp"/' -g

  # 与 multi-trace 配合分析延迟
  perf-prof trace -e 'sched:sched_wakeup,sched:sched_switch' --order | head -100
  perf-prof multi-trace -e 'sched:sched_wakeup' -e 'sched:sched_switch//key=next_pid/' -k pid --order
  ```

- **多阶段分析**:
  ```bash
  # 阶段 1: 识别高频事件
  perf-prof stat -e 'sched:*' -i 1000

  # 阶段 2: 跟踪高频事件的调用栈
  perf-prof trace -e 'sched:sched_wakeup' -g -N 1000

  # 阶段 3: 过滤特定场景深入分析
  perf-prof trace -e 'sched:sched_wakeup/prio<10/' -g --flame-graph high_prio.folded

  # 阶段 4: 生成火焰图可视化
  flamegraph.pl high_prio.folded > high_prio.svg
  ```

## 相关资源
- [事件过滤文档](../Event_filtering.md)
- [表达式系统文档](../expr.md)
- [sample_type 采样类型说明](../sample_type.md)
- [选项参数完整参考](../main_options.md)
- [multi-trace 联合分析文档](./multi-trace.md)
- [top 键值统计文档](./top.md)
- [task-state 进程状态文档](./task-state.md)
