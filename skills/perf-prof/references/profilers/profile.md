# profile - CPU采样分析
以指定频率对CPU进行采样，分析CPU利用率高的问题，定位内核态/用户态的热点函数。

## 概述
- **主要用途**: 通过定期采样CPU执行状态，识别消耗CPU时间最多的函数和代码路径
- **适用场景**: CPU利用率高、系统响应慢、热点函数定位、性能瓶颈分析、火焰图生成
- **功能分类**: 内建事件类，CPU性能分析，采样分析
- **最低内核版本**: Linux 2.6.32+（需要 perf_event 子系统）
- **平台支持**: x86、x86_64、ARM、ARM64、RISC-V、PowerPC
- **特殊限制**:
  - Guest 环境默认使用软件时钟
  - 需要 root 权限或 CAP_PERFMON
- **参与联合分析**: 可作为 multi-trace 的事件源，采样大延迟期间的热点

## 基础用法
```bash
perf-prof profile -F <freq> [选项]
```

### OPTION
- `-m, --mmap-pages`: 默认为 2，启用 `-g` 时自动翻倍为 4
- `-i, --interval`: 不指定时实时输出每个采样事件

### FILTER OPTION
- `--user-callchain[=dwarf[,size]]`: 包含用户态调用栈，`no-`前缀排除。`=dwarf` 启用DWARF栈回溯
- `--kernel-callchain`: 包含内核态调用栈，`no-`前缀排除
- `--exclude-user`: 只分析内核态热点
- `--exclude-kernel`: 只分析用户态热点
- `--exclude-guest`/`-G`: Host/Guest 隔离分析
- `--irqs_disabled`: 只采样中断关闭的代码段（eBPF）
- `--tif_need_resched`: 只采样需要调度但未调度的代码段（eBPF）
- `--exclude_pid <pid>`: 排除指定的进程（eBPF）
- `--nr_running_min`/`--nr_running_max`: 按 runqueue 长度过滤（eBPF）
- `--sched_policy`: 按调度策略过滤（eBPF，0:NORMAL, 1:FIFO, 2:RR, 3:BATCH, 5:IDLE, 6:DEADLINE）
- `--prio <prio[-prio],...>`: 按优先级过滤（eBPF，0-139）

### PROFILER OPTION
- `-F, --freq <n>`: **[必需]** 采样频率（Hz），推荐 99-999
- `--than <n>`: 百分比阈值，只输出超过该占比的事件
- `-g, --call-graph`: 启用调用栈记录
- `--flame-graph <file>`: 生成火焰图折叠栈文件，`""`表示只生成火焰图

## 核心原理

### 数据模型
```
PMU中断 → [eBPF过滤] → 采样记录 → [ringbuffer] → [用户态处理] → [阈值过滤] → 输出/火焰图
```

### 事件源
- **Guest 环境**: `PERF_COUNT_SW_CPU_CLOCK`（软件时钟）
- **Intel Host**: `PERF_COUNT_HW_REF_CPU_CYCLES`（参考时钟，精确频率控制）
- **其他平台**: `PERF_COUNT_HW_CPU_CYCLES`（硬件周期计数器）

### 过滤器层次
1. **PMU 过滤器（硬件层，零开销）**: `--exclude-user`, `--exclude-kernel`, `--exclude-guest`, `-G`
2. **eBPF 过滤器（软件层）**: `--irqs_disabled`, `--tif_need_resched`, `--nr_running_*`, `--sched_policy`, `--prio`
3. **用户态阈值过滤**: `--than`（不减少采样开销，只减少输出）

### 事件处理
- **不依赖排序**: 实时采样，每个事件独立处理
- **丢事件处理**: ringbuffer 满时丢失采样点，不影响整体分析

## 输出

### 输出格式

**实时模式（无 `-i`）**:
```
YYYY-MM-DD HH:MM:SS.microsec     comm   tid [cpu] timestamp: profile: <counter> cpu-cycles
      <调用栈>
```

**火焰图模式（`--flame-graph file`）**:
```
comm;func1;func2;func3 count
```
转换为 SVG: `flamegraph.pl cpu.folded > cpu.svg`

**周期火焰图模式（`--flame-graph "" -i INT`）**:
```
YYYY-MM-DD;HH:MM:SS;comm;func1;func2;func3 count
```

### 输出字段
| 字段 | 说明 |
|------|------|
| comm | 进程/线程名 |
| tid | 线程 ID |
| [cpu] | CPU 编号（3位补零） |
| timestamp | 事件时间戳 |
| cpu-cycles | 累计 CPU 周期数 |

### 关键指标

| 指标 | 计算方法 | 异常阈值 |
|------|----------|----------|
| 热点函数占比 | 函数采样次数/总采样次数×100% | >30% 明显热点，>50% 严重瓶颈 |
| CPU 周期数 | PMU 计数器累积值 | 相对比较 |

## 应用示例

### 基础示例
```bash
# 1. 确定分析范围
perf-prof profile -F 997 -g                    # 整个系统
perf-prof profile -F 997 -p <pid> -g           # 特定进程
perf-prof profile -F 997 -C 0-3 -g             # 特定 CPU

# 2. 选择内核态或用户态
perf-prof profile -F 997 --exclude-user -g     # 只看内核态
perf-prof profile -F 997 --exclude-kernel -g   # 只看用户态

# 3. 设置过滤阈值
perf-prof profile -F 997 --than 30 -g          # 只关注 >30% 的热点

# 4. 生成火焰图
perf-prof profile -F 997 -g --flame-graph cpu.folded -- sleep 60
flamegraph.pl cpu.folded > cpu.svg
```

### 高级技巧
```bash
# 周期性火焰图（每5秒一次）
perf-prof profile -F 997 -g --flame-graph "" -i 5000

# 只采样中断关闭的代码
perf-prof profile -F 997 --irqs_disabled -g --flame-graph irqoff.folded

# 只采样 runqueue >= 3 的 CPU
perf-prof profile -F 997 --nr_running_min 3 -g --flame-graph busy.folded

# 不只采样 pid=234 的进程
perf-prof profile -F 997 --exclude_pid 234 -g --flame-graph rr.folded

# 只采样 RR 进程
perf-prof profile -F 997 --sched_policy 2 -g --flame-graph rr.folded

# 只采样实时优先级进程（0-99）
perf-prof profile -F 997 --prio 0-99 -g --flame-graph rt.folded

# Host/Guest 隔离分析
perf-prof profile -F 997 --exclude-guest -g --flame-graph host.folded
perf-prof profile -F 997 -G -g --flame-graph guest.folded

# 采样延迟调度的代码
perf-prof profile -F 997 --tif_need_resched -g --flame-graph sched_delay.folded
```

### 性能优化
```bash
# 高频采样增加 ringbuffer
perf-prof profile -F 4000 -g -m 64 --flame-graph high_freq.folded

# 极限采样
perf-prof profile -F 10000 -g -m 256 --flame-graph extreme.folded

# 生产环境低开销
perf-prof profile -F 99 -g --flame-graph production.folded -- sleep 300
```

## 相关资源
- [火焰图生成工具](https://github.com/brendangregg/FlameGraph)
