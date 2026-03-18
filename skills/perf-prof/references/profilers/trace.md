# trace - 事件跟踪与打印
跟踪并打印系统事件，支持调用栈记录和火焰图生成。

## 概述
- **主要用途**: 实时跟踪和显示内核/用户空间事件，记录调用栈，支持联合分析，是最基础和灵活的事件分析工具
- **适用场景**: 需要实时查看事件发生的详细信息、分析事件序列、记录调用栈、生成火焰图、进行多事件联合分析
- **功能分类**: 自定义事件类，调试与工具，追踪分析/联合分析
- **最低内核版本**: Linux 2.6.31+ (需要 perf_event 子系统)
- **平台支持**: x86, ARM, ARM64, RISC-V, PowerPC 等主流架构
- **特殊限制**:
  - 需要 root 权限或 CAP_PERFMON 能力
  - kprobe/uprobe 需要内核 CONFIG_KPROBES 支持
  - BPF 过滤器需要较新内核支持 (4.1+)
- **参与联合分析**: 作为联合分析主体，可通过 `profiler[/option/]` 语法嵌入其他分析器作为事件源

## 基础用法
```bash
perf-prof trace -e EVENT [--overwrite] [-g [--flame-graph file [-i INT]]]
```

### OPTION
- `-i, --interval <ms>`: 间隔输出，配合 `--flame-graph` 周期性输出火焰图 (默认：无间隔)
- `--order`: 按时间戳排序事件，确保事件输出的时序正确性
- `-N, --exit-N <N>`: 采样 N 个事件后退出
- `--inherit`: 子任务继承计数器，跟踪创建的子进程/线程 (实验性)

### FILTER OPTION
- `--user-callchain[=dwarf[,size]]`: 包含用户态调用栈，`no-` 前缀排除 (默认：包含)。`=dwarf` 启用DWARF栈回溯
- `--kernel-callchain`: 包含内核态调用栈，`no-` 前缀排除 (默认：包含)
- `--python-callchain`: 包含 Python 调用栈

### PROFILER OPTION
- `-e, --event <EVENT,...>`: 事件选择器，支持多种事件源类型
  - **tracepoint**: `sys:name[/filter/ATTR/.../]` - 系统 tracepoint 事件
  - **profiler**: `profiler[/option/ATTR/.../]` - 嵌入其他分析器作为事件源
  - **kprobe**: `kprobe:func[/filter/ATTR/.../]` - 内核函数探针
  - **kretprobe**: `kretprobe:func[/filter/ATTR/.../]` - 内核函数返回探针
  - **uprobe**: `uprobe:func@"file"[/filter/ATTR/.../]` - 用户态函数探针
  - **uretprobe**: `uretprobe:func@"file"[/filter/ATTR/.../]` - 用户态函数返回探针
  - **通配符**: 支持 `*, ?, []` 通配符匹配多个事件
- `-g, --call-graph`: 启用调用图记录，为所有事件记录调用栈
- `--flame-graph <file>`: 指定折叠栈输出文件，生成火焰图格式数据
- `--overwrite`: 使用覆盖模式，避免在高负载下丢失最新事件 (实验性)
- `--ptrace`: 使用 ptrace 跟踪新创建的线程

## 核心原理

### 数据模型
```
事件源 → [内核过滤] → 环形缓冲区 → [用户态过滤] → [排序] → [栈解析] → 显示/保存
```

### 事件源
- **tracepoint**: 使用 `perf-prof list` 列出所有可用事件，支持通配符匹配
- **kprobe/kretprobe**: 动态探针，需要内核支持
- **uprobe/uretprobe**: 用户态探针，需要指定可执行文件路径
- **profiler**: 引用其他分析器作为事件源，进行联合分析

### 过滤器层次
1. **trace event 过滤器（内核态）**: 第一个 `/` 之后、第二个 `/` 之前，效率高
   - 支持: `==, !=, <, <=, >, >=, &`（数值）, `==, !=, ~`（字符串）
   - 扩展字段: `_cpu`（事件发生的 CPU）, `_pid`（进程 ID）
2. **用户态表达式过滤器**: 内核态过滤器设置失败时使用，支持完整 C 表达式

### 事件属性
- `stack`: 启用调用栈记录
- `max-stack=int`: 最大栈深度 (默认: 127)
- `alias=str`: 事件别名
- `cpus=cpu[-cpu]`: 指定 CPU 范围
- `exec=EXPR`: 执行表达式
- `push/pull`: 事件传播（跨主机/跨虚拟机）

### 事件处理
- **排序依赖**: 使用 `--order` 时按时间戳排序，多 CPU 事件归并
- **丢事件处理**: 使用 `--overwrite` 覆盖旧数据，避免丢失最新事件
- **调用栈处理**: 不启用火焰图时实时打印，启用时累积到火焰图结构

## 输出

### 输出格式

**标准输出（默认）**:
```
[UNIX时间戳] [进程名/TID] [CPU] [时间戳] [事件名称] [字段=值 ...]
    ffffffff81xxxxxx function_name+0xoffset (kernel)
    7fxxxxxxxxxxxx function_name+0xoffset (/path/to/library.so)
```

**火焰图格式（`--flame-graph`）**:
```
comm;func1;func2;func3 count
```

### 输出字段
| 字段 | 说明 |
|------|------|
| UNIX时间戳 | YYYY-MM-DD HH:MM:SS.microsec，精度有损失 |
| 进程名/TID | 进程名称和线程 ID |
| CPU | 事件发生的 CPU 编号 (0-based) |
| 时间戳 | 事件精确时间，支持 `--tsc`, `--kvmclock`, `--monotonic` |
| 事件名称 | tracepoint、kprobe 等事件名称 |
| 调用栈 | 地址 函数名+偏移 (模块) |

### 关键指标

| 指标 | 计算方法 | 异常阈值 |
|------|----------|----------|
| 事件频率 | 单位时间内事件发生次数 | 根据业务确定基线 |
| 调用路径 | 通过调用栈识别 | 死循环/递归深度异常 |
| 事件乱序 | 未使用 `--order` 时可能出现 | 时间戳逆序 |

## 应用示例

### 基础示例
```bash
# 1. 跟踪进程调度事件
perf-prof trace -e 'sched:sched_wakeup,sched:sched_switch' -C 0 -N 100

# 2. 分析高优先级进程的调度路径
perf-prof trace -e 'sched:sched_wakeup/prio<10/' -g --flame-graph high_prio.folded

# 3. 跟踪特定进程的系统调用
perf-prof trace -e 'syscalls:sys_enter_*' -p 1234 --order

# 4. 监控内存分配热点
perf-prof trace -e 'kmem:kmalloc/bytes_alloc>1024/stack/' -m 128
```

### 高级技巧
```bash
# 使用通配符批量跟踪事件
perf-prof trace -e 'sched:*' -C 0 -N 1000 | grep -i wakeup

# 动态跟踪内核函数
perf-prof trace -e 'kprobe:schedule' -g --flame-graph schedule.folded

# 跟踪用户态库函数
perf-prof trace -e 'uprobe:malloc@"/lib64/libc.so.6"' -p 1234

# 周期性生成火焰图
perf-prof trace -e 'sched:sched_wakeup' -g --flame-graph wakeup.folded -i 5000

# 多层过滤组合
perf-prof trace -e 'sched:sched_wakeup/target_cpu==0 && prio<10/stack/max-stack=16/' -C 0

# 使用表达式计算自定义字段
perf-prof trace -e 'sched:sched_stat_runtime//exec=printf("runtime: %llu us\n", runtime/1000)/' -C 0

# 跨主机事件传播
perf-prof trace -e 'sched:sched_wakeup//push=192.168.1.100:8888/' -C 0   # 发送端
perf-prof trace -e 'sched:sched_wakeup//pull=0.0.0.0:8888/' --order      # 接收端
```

### 性能优化
```bash
# 高频事件增大缓冲区
perf-prof trace -e 'irq:*' -m 128

# 低频事件减少内存占用
perf-prof trace -e 'block:*' -m 4

# 覆盖模式保留最新事件
perf-prof trace -e 'sched:sched_switch' --overwrite -m 64
```

### 组合使用
```bash
# 与 task-state 联合分析
perf-prof trace -e 'task-state,profile/-F 5000/' --order -p 1234

# 多阶段分析
perf-prof stat -e 'sched:*' -i 1000              # 阶段1: 识别高频事件
perf-prof trace -e 'sched:sched_wakeup' -g -N 1000      # 阶段2: 跟踪调用栈
perf-prof trace -e 'sched:sched_wakeup/prio<10/' -g --flame-graph high_prio.folded  # 阶段3: 过滤分析
flamegraph.pl high_prio.folded > high_prio.svg   # 阶段4: 生成火焰图
```

## 相关资源
- [事件过滤文档](Event_filtering.md)
- [表达式系统文档](expr.md)
- [multi-trace 联合分析文档](multi-trace.md)
- [top 键值统计文档](top.md)
