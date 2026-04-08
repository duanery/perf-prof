# profile - CPU采样分析器
以指定频率对CPU进行采样，分析CPU利用率高的问题，定位内核态/用户态的热点函数。

## 概述
- **主要用途**: 通过定期采样CPU执行状态，识别消耗CPU时间最多的函数和代码路径，支持内核态和用户态的性能剖析
- **适用场景**: CPU利用率高、系统响应慢、热点函数定位、性能瓶颈分析、火焰图生成
- **功能分类**: 内建事件类，CPU性能分析，采样分析
- **最低内核版本**: 需要支持 perf_event 子系统（Linux 2.6.32+）
- **依赖库**: libelf（必需）、libtraceevent（进程名解析）
- **平台支持**: x86、x86_64、ARM、ARM64、RISC-V、PowerPC 等所有支持 perf_event 的架构
- **特殊限制**:
  - Guest环境默认使用软件时钟（PERF_COUNT_SW_CPU_CLOCK）
  - 需要 root 权限或 CAP_PERFMON 能力
  - Intel 平台可利用 TSC 实现精确频率控制
- **参与联合分析**: 参与multi-trace分析延迟根因，可采样大延迟
- **核心技术**: 基于硬件 PMU 的 CPU_CYCLES 计数器或软件时钟进行周期性采样

## 基础用法
```bash
perf-prof profile -F <freq> [选项]
```

### OPTION
以下选项具有特殊的默认值或行为：
- `-m, --mmap-pages`: 默认为 2，启用 `-g` 时自动翻倍为 4
- `-i, --interval`: 不指定时实时输出每个采样事件

### FILTER OPTION
支持所有标准过滤器选项，常用组合：
- `--exclude-user`: 只分析内核态热点
- `--exclude-kernel`: 只分析用户态热点
- `--exclude-guest`/`-G`: Host/Guest 隔离分析
- `--irqs_disabled`: 只采样中断关闭的代码段
- `--tif_need_resched`: 只采样需要调度但未调度的代码段
- `--exclude_pid <pid>`: 排除指定的进程
- `--nr_running_min`/`--nr_running_max`: 按 runqueue 长度过滤
- `--sched_policy`: 按调度策略过滤（0:NORMAL, 1:FIFO, 2:RR, 3:BATCH, 5:IDLE, 6:DEADLINE）
- `--prio`: 按优先级过滤（0-139，0-99为实时优先级，100-139为普通优先级）

### PROFILER OPTION
- `-F, --freq <n>`: **[必需]** 采样频率（Hz），推荐值：99-999，0表示不采样
- `--than <n>`: 过滤阈值，百分比，只输出采样次数占总采样次数超过该百分比的事件
- `-g, --call-graph`: 启用调用栈记录，用于分析调用链
- `--flame-graph <file>`: 生成火焰图折叠栈文件，空字符串("")表示只生成火焰图不输出事件
- `--prio <prio[-prio],...>`: eBPF 过滤器，按调度优先级过滤（0-139）

### 示例
```bash
# 以 997Hz 采样指定进程，记录调用栈并生成火焰图
perf-prof profile -F 997 -p <pid> -g --flame-graph cpu.folded

# 采样 CPU 0-3，过滤掉低于 30% 的热点，生成火焰图
perf-prof profile -F 997 -C 0-3 --than 30 -g --flame-graph cpu.folded

# 只采样内核态，实时输出热点函数
perf-prof profile -F 499 --exclude-user -g
```

## 核心原理

### 基本定义
- **采样频率（Sampling Frequency）**: 每秒触发采样中断的次数（Hz）
- **CPU Cycles**: CPU 时钟周期计数，表示 CPU 实际执行的指令周期数
- **Reference Cycles**: Intel 平台的参考时钟周期（REF_CPU_CYCLES），不受频率调节影响
- **采样周期（Sample Period）**: 两次采样之间的时间间隔或周期数
- **热点函数（Hot Function）**: 被采样到次数最多的函数
- **CPU 利用率**: 在 Intel 平台可通过 cycles 与 TSC 的比值计算

### 数据模型
```
PMU中断触发 → [eBPF过滤] → 采样数据记录 → [ringbuffer] → [用户态处理] → [阈值过滤] → 输出/火焰图
```

### 事件源

**采样配置**：
- **Guest 环境**: 使用软件时钟 `PERF_COUNT_SW_CPU_CLOCK`，`exclude_idle=1`
- **Intel Host**: 优先使用 `PERF_COUNT_HW_REF_CPU_CYCLES`（参考时钟周期），通过 TSC 频率精确控制采样周期
- **其他平台**: 使用硬件周期计数器 `PERF_COUNT_HW_CPU_CYCLES`，通过 `freq=1` 启用频率模式

**采样周期计算（Intel 平台）**：
```
sample_period = tsc_khz * 1000 / freq
例如: freq=997Hz, tsc_khz=2500000
     sample_period = 2500000 * 1000 / 997 = 2506265
```

- **sample_type**:
  - `PERF_SAMPLE_TID`: 进程/线程 ID（始终启用）
  - `PERF_SAMPLE_TIME`: 时间戳（始终启用）
  - `PERF_SAMPLE_CPU`: CPU 编号（始终启用）
  - `PERF_SAMPLE_READ`: 读取计数器值（始终启用）
  - `PERF_SAMPLE_CALLCHAIN`: 调用栈（`-g` 选项启用）

- **内建事件**:
  - `PERF_COUNT_HW_CPU_CYCLES`: CPU 时钟周期（默认事件）
  - `PERF_COUNT_HW_REF_CPU_CYCLES`: Intel 平台参考时钟周期（优先使用）
  - `PERF_COUNT_SW_CPU_CLOCK`: Guest 环境软件时钟

#### 过滤器

**PMU 过滤器（硬件层）**：
- `exclude_user`: 不采样用户态指令指针
- `exclude_kernel`: 不采样内核态指令指针
- `exclude_guest`: 不采样 Guest 模式
- `exclude_host`: 不采样 Host 模式（`-G` 选项）
- `exclude_callchain_user`: 调用栈中排除用户态栈帧
- `exclude_callchain_kernel`: 调用栈中排除内核态栈帧

**eBPF 过滤器（软件层）**：
通过 `bpf_filter` 在采样点过滤，支持：
- 中断状态过滤（`--irqs_disabled`）
- 调度标记过滤（`--tif_need_resched`）
- 进程 PID 过滤（`--exclude_pid`）
- runqueue 长度过滤（`--nr_running_min`/`--nr_running_max`）
- 调度策略过滤（`--sched_policy`）
- 优先级范围过滤（`--prio`）

**用户态阈值过滤**：
`--than <n>` 参数实现百分比阈值过滤，算法如下：
```c
// 每秒重置统计
if (current_time - last_reset >= 1s) {
    sample_count = 1;
    last_reset = current_time;
    print = false;
} else {
    sample_count++;
    threshold_count = (freq * than + 99) / 100;
    if (sample_count >= threshold_count)
        print = true;
}
```

### 事件处理

**处理流程**：
1. **采样中断**: PMU 计数器溢出触发中断或软件定时器到期
2. **内核记录**: 记录 PID、TID、CPU、时间戳、指令指针（IP）、调用栈（可选）
3. **eBPF 过滤**: 执行 eBPF 程序，决定是否继续记录（如果配置了 eBPF 过滤器）
4. **写入 ringbuffer**: 通过 perf_event ringbuffer 传递到用户态
5. **用户态解析**: 读取采样数据，解析各字段
6. **阈值过滤**: 应用 `--than` 参数进行统计过滤
7. **符号解析**: 将指令指针转换为函数名（调用栈模式）
8. **输出**: 实时打印或累积到火焰图

**不依赖排序**: profile 是实时采样分析，每个采样事件独立处理，不需要 `--order` 排序

**丢事件处理**: 当 ringbuffer 满时可能丢失采样点，但不影响整体分析结果，只是采样精度略有下降

### 状态统计

**周期性输出**：
- 使用 `-i, --interval` 参数时，profile 按周期输出
- 对于火焰图模式（`--flame-graph`），每个周期会：
  - 生成当前周期的时间戳标签（格式：`YYYY-MM-DD;HH:MM:SS`）
  - 输出累积的折叠栈到文件
  - 重置火焰图统计数据

**信号处理**：
- profile 不响应 SIGUSR1/SIGUSR2 信号
- 使用 Ctrl+C (SIGINT) 正常退出，退出时会输出最终的火焰图数据

## 输出

### 输出格式

**实时模式（无 `-i` 参数）**：
```
YYYY-MM-DD HH:MM:SS.microsec      comm   tid [cpu] timestamp: profile: <counter> cpu-cycles
                <调用栈（如果启用 -g）>
```

**周期模式（有 `-i` 参数）**：
```
YYYY-MM-DD HH:MM:SS.microsec      comm   tid [cpu] timestamp: profile: <counter> cpu-cycles
                <调用栈（如果启用 -g）>
[... 多个采样事件 ...]
```

**火焰图模式（`--flame-graph file`）**：
生成 Brendan Gregg 火焰图格式的折叠栈文件：
```
comm;func1;func2;func3 count
comm;func1;func4;func5 count
...
```
使用 `flamegraph.pl` 转换为 SVG：
```bash
flamegraph.pl cpu.folded > cpu.svg
```

**火焰图周期模式（`--flame-graph file -i INT`）**：
如果 `file` 为空字符串 `""`，则只输出火焰图，不打印事件：
```
YYYY-MM-DD;HH:MM:SS;comm;func1;func2;func3 count
YYYY-MM-DD;HH:MM:SS;comm;func1;func4;func5 count
...
```

### 输出字段说明

**表头含义**：
- `YYYY-MM-DD HH:MM:SS.microsec`: 事件发生的时间戳
- `comm`: 进程/线程名称
- `tid`: 线程 ID
- `[cpu]`: 采样时所在的 CPU 编号（3位，补零对齐）
- `timestamp`: 事件时间戳（秒.微秒格式）
- `cpu-cycles`: 本次采样周期累计的 CPU 周期数

**调用栈格式**（`-g` 启用时）：
```
            ffffffff81234567 function_name+0x12 ([kernel.kallsyms])
            00007f8901234567 library_func+0x34 (/lib64/libc.so.6)
            0000000000401234 main+0x56 (/path/to/binary)
```
每行包含：
- 指令指针地址（十六进制）
- 函数名+偏移量
- 所属模块（内核符号、共享库或二进制文件）

**数据单位**：
- 时间戳：纳秒（nanoseconds）
- CPU cycles：周期数（无单位）

**排序规则**：
- 实时输出按采样发生的时间顺序
- 火焰图按调用栈折叠后的计数排序

### 关键指标

**采样计数（counter）**：
- **计算方法**: 累积的采样次数，每次采样增加当前计数器值与上次的差值
- **正常范围**: 与采样频率相关，理论值 ≈ `freq * elapsed_time`
- **异常阈值**: 如果某个函数的采样计数远超其他函数，说明该函数是热点

**CPU 周期数（cycles）**：
- **计算方法**: 从 PMU 计数器读取的累积 CPU 周期数
- **正常范围**: 与 CPU 频率和运行时间相关
- **异常阈值**: 周期数过高表示该时间段 CPU 消耗大

**热点函数采样占比**：
- **计算方法**: `函数采样次数 / 总采样次数 * 100%`
- **正常范围**: 取决于应用特性
- **异常阈值**:
  - 单个函数 `> 30%`: 明显热点，优先优化
  - 单个函数 `> 50%`: 严重瓶颈，必须优化

### 阈值建议

**采样频率选择**：
- **低开销（99-199Hz）**: 生产环境长期监控，开销 < 1%
- **平衡（499-999Hz）**: 常规性能分析，开销约 1-3%
- **高精度（1000-4000Hz）**: 详细剖析，短期使用，开销 5-10%
- **极限（> 4000Hz）**: 微观分析，可能影响系统性能

**过滤阈值（--than）**：
- **粗筛（> 50%）**: 快速定位最严重的瓶颈
- **常规（> 10-30%）**: 识别主要热点函数
- **细致（> 1-5%）**: 全面分析性能分布
- **无过滤（0%）**: 记录所有采样点

**ringbuffer 大小（-m）**：
- **低频采样（2-8 pages）**: 采样频率 < 1000Hz
- **中频采样（16-64 pages）**: 采样频率 1000-4000Hz
- **高频采样（128-512 pages）**: 采样频率 > 4000Hz 或启用调用栈

## 分析方法

### 基础分析流程

**1. 确定分析范围**：
```bash
# 分析整个系统
perf-prof profile -F 997 -g

# 分析特定进程
perf-prof profile -F 997 -p <pid> -g

# 分析特定 CPU
perf-prof profile -F 997 -C 0-3 -g
```

**2. 选择内核态或用户态**：
```bash
# 只分析内核态热点
perf-prof profile -F 997 --exclude-user -g

# 只分析用户态热点
perf-prof profile -F 997 --exclude-kernel -g
```

**3. 设置过滤阈值**：
```bash
# 只关注占用 > 30% 的热点
perf-prof profile -F 997 --than 30 -g
```

**4. 生成火焰图**：
```bash
# 采样 60 秒生成火焰图
perf-prof profile -F 997 -g --flame-graph cpu.folded -- sleep 60
flamegraph.pl cpu.folded > cpu.svg
```

### 数据驱动分析

**不预设任何业务特征**：
1. 先用 `top` 或 `mpstat` 确认 CPU 利用率异常
2. 使用 `profile` 全系统采样识别进程和函数热点
3. 根据热点分布选择内核态或用户态分析
4. 用 `--than` 参数逐步聚焦最严重的瓶颈

**完全基于实际数据**：
```bash
# 第一步：全系统采样 30 秒，识别热点进程
perf-prof profile -F 997 -g -- sleep 30

# 第二步：针对热点进程详细分析
perf-prof profile -F 997 -p <hot_pid> -g --flame-graph cpu.folded -- sleep 60

# 第三步：如果是内核热点，排除用户态噪声
perf-prof profile -F 997 -p <hot_pid> --exclude-user -g --flame-graph kernel.folded

# 第四步：设置阈值过滤，聚焦最热的代码路径
perf-prof profile -F 997 -p <hot_pid> --than 10 -g
```

## 应用示例

### 基础示例

**示例 1：分析系统级 CPU 热点**
```bash
# 以 997Hz 采样整个系统，记录调用栈，生成火焰图
perf-prof profile -F 997 -g --flame-graph system_cpu.folded

# 转换为 SVG 火焰图
flamegraph.pl system_cpu.folded > system_cpu.svg
```

**示例 2：分析特定进程的用户态热点**
```bash
# 只采样进程 2347 的用户态代码
perf-prof profile -F 997 -p 2347 --exclude-kernel -g --flame-graph user_cpu.folded
```

**示例 3：分析内核态热点并实时输出**
```bash
# 排除用户态，只看内核态热点函数
perf-prof profile -F 499 --exclude-user -g
```

**示例 4：按优先级过滤实时进程**
```bash
# 只采样实时优先级进程（0-99）
perf-prof profile -F 997 --prio 0-99 -g --flame-graph rt_cpu.folded
```

### 高级技巧

**技巧 1：周期性火焰图对比**
```bash
# 每 5 秒生成一次火焰图快照，输出到标准输出
perf-prof profile -F 997 -g --flame-graph "" -i 5000

# 重定向到文件后分割成多个火焰图
perf-prof profile -F 997 -g --flame-graph "" -i 5000 -o snapshots.folded
awk '/^[0-9]{4}-[0-9]{2}-[0-9]{2};/{n++}{print > "snapshot_"n".folded"}' snapshots.folded
```

**技巧 2：只采样中断关闭的代码段**
```bash
# 定位关中断时间过长的代码
perf-prof profile -F 997 --irqs_disabled -g --flame-graph irqoff.folded
```

**技巧 3：只采样 runqueue 长的 CPU**
```bash
# 只在 runqueue >= 3 时采样
perf-prof profile -F 997 --nr_running_min 3 -g --flame-graph busy_cpu.folded
```

**技巧 4：Guest 和 Host 隔离分析**
```bash
# 在 Host 上分析，排除 Guest 模式
perf-prof profile -F 997 --exclude-guest -g --flame-graph host_only.folded

# 在 Host 上只分析 Guest 模式
perf-prof profile -F 997 -G -g --flame-graph guest_only.folded
```

**技巧 5：采样延迟调度的代码段**
```bash
# 只采样 TIF_NEED_RESCHED 标记设置但未调度的代码
perf-prof profile -F 997 --tif_need_resched -g --flame-graph sched_delay.folded
```

### 性能优化

**缓冲区大小调优**：
- **默认（2 pages = 8KB）**: 适用于低频采样（< 500Hz）且无调用栈
- **启用调用栈（自动翻倍到 4 pages）**: 记录完整调用链需要更多空间
- **高频采样（-m 64）**: 避免 ringbuffer 溢出导致丢失采样点
  ```bash
  perf-prof profile -F 4000 -g -m 64 --flame-graph high_freq.folded
  ```
- **极限采样（-m 256）**: 最高采样频率（> 10000Hz）
  ```bash
  perf-prof profile -F 10000 -g -m 256 --flame-graph extreme.folded
  ```

**采样频率优化**：
- **生产环境（99Hz）**: 极低开销，长期监控
  ```bash
  perf-prof profile -F 99 -g --flame-graph production.folded -- sleep 300
  ```
- **问题诊断（997Hz）**: 平衡精度和开销，推荐默认值
  ```bash
  perf-prof profile -F 997 -g --flame-graph diagnosis.folded -- sleep 60
  ```
- **深度剖析（4999Hz）**: 高精度，短期使用
  ```bash
  perf-prof profile -F 4999 -g -m 128 --flame-graph deep.folded -- sleep 10
  ```

**过滤器优化**：
- **优先使用 PMU 过滤器**: 硬件层过滤，零开销
  ```bash
  perf-prof profile -F 997 --exclude-user -g  # 硬件过滤用户态
  ```
- **谨慎使用 eBPF 过滤器**: 每次采样执行 eBPF 程序，有额外开销
  ```bash
  perf-prof profile -F 997 --irqs_disabled -g  # 每次采样检查中断状态
  ```
- **用户态阈值过滤**: 不减少采样开销，只减少输出量
  ```bash
  perf-prof profile -F 997 --than 50 -g  # 采样开销不变，输出减少
  ```

### 参数调优

**采样频率选择建议**：
- **CPU bound 应用**: 使用较高频率（997-4999Hz），捕获更多热点细节
- **IO bound 应用**: 使用较低频率（99-499Hz），避免过度采样空闲等待
- **微服务架构**: 使用中等频率（499-997Hz），平衡多个进程的采样精度
- **实时系统**: 使用低频率（49-99Hz）且短时采样，减少对延迟的影响

**火焰图优化**：
- **单次生成（默认）**: 适合固定时间窗口分析
  ```bash
  perf-prof profile -F 997 -g --flame-graph cpu.folded -- sleep 60
  ```
- **周期生成（-i）**: 适合观察时间序列变化
  ```bash
  perf-prof profile -F 997 -g --flame-graph "" -i 10000  # 每 10 秒一次
  ```
- **纯火焰图模式**: 不输出事件，减少 I/O 开销
  ```bash
  perf-prof profile -F 997 -g --flame-graph "" -i 5000 -o cpu.folded
  ```

**调用栈深度优化**：
虽然 profile 本身不提供 `--max-stack` 参数，但可通过内核参数调整：
```bash
# 查看当前最大栈深度
cat /proc/sys/kernel/perf_event_max_stack

# 临时调整（需要 root）
echo 32 > /proc/sys/kernel/perf_event_max_stack
perf-prof profile -F 997 -g --flame-graph deep_stack.folded
```

### 组合使用

**与 top 分析器配合**：
```bash
# 第一步：用 top 统计进程被唤醒的次数
perf-prof top -e sched:sched_wakeup//comm=comm/ --only-comm -i 1000 -- sleep 60

# 第二步：用 profile 分析唤醒最频繁的进程的 CPU 热点
perf-prof profile -F 997 -p <hot_pid> -g --flame-graph wakeup_hot.folded
```

**与 oncpu 分析器配合**：
```bash
# 第一步：用 oncpu 观察 CPU 上运行的进程
perf-prof oncpu -C 0-3 --detail

# 第二步：用 profile 采样特定 CPU 的热点
perf-prof profile -F 997 -C 0 -g --flame-graph cpu0.folded
```

**与 task-state 分析器配合**：
```bash
# 第一步：用 task-state 统计进程运行时间
perf-prof task-state -p <pid> -i 1000 -- sleep 60

# 第二步：如果发现 R 状态时间长，用 profile 分析在做什么
perf-prof profile -F 997 -p <pid> -g --flame-graph running.folded
```

**多阶段分析示例**：
```bash
# 阶段 1：全系统扫描（低频，低开销）
perf-prof profile -F 99 -g --flame-graph phase1.folded -- sleep 60

# 阶段 2：针对热点进程（中频）
perf-prof profile -F 997 -p <pid> -g --flame-graph phase2.folded -- sleep 60

# 阶段 3：内核态深度剖析（高频）
perf-prof profile -F 4999 -p <pid> --exclude-user -g -m 128 --flame-graph phase3.folded -- sleep 30

# 阶段 4：用户态深度剖析（高频）
perf-prof profile -F 4999 -p <pid> --exclude-kernel -g -m 128 --flame-graph phase4.folded -- sleep 30
```

## 相关资源
- [sample_type 采样类型说明](../sample_type.md)
- [选项参数完整参考](../main_options.md)
- [火焰图生成工具](https://github.com/brendangregg/FlameGraph)
- [Brendan Gregg 的 CPU 火焰图指南](http://www.brendangregg.com/flamegraphs.html)
- [perf_event_open 系统调用手册](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)
