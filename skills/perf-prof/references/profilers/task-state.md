# task-state - 进程状态耗时分析

task-state用来跟踪整个系统或进程状态（`R,S,D,T,t,I,RD`）的耗时分布。

## 概述
- **主要用途**: 诊断进程调度延迟、IO等待、睡眠等问题，跟踪进程状态的耗时分布，分析业务特征，业务画像。
- **适用场景**: CPU利用率高但业务性能差、进程响应慢、系统卡顿、IO等待时间长、调度器性能分析
- **功能分类**: 内建事件类，进程调度分析，状态监控
- **最低内核版本**: Linux 3.0+ (支持tracepoint事件)
- **依赖库**: libtraceevent, libperf
- **平台支持**: x86_64, ARM64
- **特殊限制**:
  - 需要CAP_SYS_ADMIN权限或root用户运行
  - `--ptrace`会带来停顿，对于频繁创建线程的业务慎用
  - `--ptrace`不能和`--filter`一起使用
- **参与联合分析**: 不参与联合分析，但可与profile、blktrace、multi-trace配合使用进行多阶段分析

## 基础用法
```bash
perf-prof task-state [OPTION...] [-S] [-D] [--than ns] [--filter comm] [--perins] [--ptrace] [-g [--flame-graph file]] [-- workload workload_options]
```

如果使用workload，`--`前面是perf-prof的选项，后面是workload及其选项。

### OPTION
- `--watermark <0-100>`: 默认值50（不同于通用默认值）
- `-m, --mmap-pages <pages>`: 默认值8，启用`-g`时为16
- `--order`: 默认启用（不同于通用默认值关闭）

### FILTER OPTION
- `--user-callchain[=dwarf[,size]]`: 启用-g时默认打开，"no-"前缀关闭。`=dwarf` 启用DWARF栈回溯
- `--kernel-callchain`: 启用-g时默认打开，"no-"前缀关闭
- `--python-callchain`: 启用-g时默认关闭

### PROFILER OPTION
- `--filter <comm>`: 指定一个或多个comm，多个comm使用','分隔，comm支持通配符(`*?[]`)、不支持正则表达式。跟`-p`、`-t`同时使用时，会优先使用`--filter`选项。
- `-S, --interruptible`: 监控TASK_INTERRUPTIBLE状态，no-前缀排除
- `-D, --uninterruptible`: 监控TASK_UNINTERRUPTIBLE状态
- `--than <n>`: 输出超过阈值的事件信息，包括起始事件，单位支持s/ms/us/ns
- `--perins`: 按每个线程输出统计状态
- `-g, --call-graph`: 启用调用栈采集
- `--flame-graph <file>`: 输出火焰图折叠栈文件
- `--ptrace`: 使用ptrace跟踪新创建的线程

## 核心原理

### 数据模型
```
事件 → 排序 → 各状态延迟测量 → 统计聚合 → 周期显示
```

### 状态定义

task-state监控以下七种进程状态：

| 状态 | 内核定义 | 描述 | 符号 |
|------|----------|------|------|
| TASK_RUNNING | 0 | 运行状态 | R |
| TASK_INTERRUPTIBLE | 1 | 可中断睡眠状态 | S |
| TASK_UNINTERRUPTIBLE | 2 | 不可中断睡眠状态 | D |
| __TASK_STOPPED | 4 | 停止状态 | T |
| __TASK_TRACED | 8 | 跟踪状态 | t |
| TASK_REPORT_IDLE | 0x80 | 空闲状态（Linux 4.14+） | I |
| RUNDELAY | TASK_STATE_MAX << 1 | 调度延迟（自定义） | RD |

### 事件源
- **内建事件**:
  - `sched:sched_switch`: prev_comm, prev_pid, prev_state, next_comm, next_pid
  - `sched:sched_wakeup`: comm, pid
  - `sched:sched_wakeup_new`: comm, pid

### 过滤器层次

为了使task-state采样的事件尽可能少，在使用不同的选项参数时，会选择不同的事件及过滤器。

1. **状态过滤（内核态）**: `-S`、`-D`选项过滤prev_state字段
2. **进程过滤（内核态）**: `-p`、`-t`、`--filter`选项过滤pid/comm等字段

综合后形成4种工作模式：

**模式0：全局状态监控**
- 状态过滤：不过滤S/D状态，监控所有状态
- 监控范围：整个系统
- 使用事件：3个（sched_switch、sched_wakeup、sched_wakeup_new无过滤器）
- 适用场景：系统级全面监控

**模式1：指定进程全状态监控**
- 状态过滤：不过滤S/D状态，监控所有状态
- 监控范围：进程类（进程、线程、workload）、进程名
- 使用事件：4个（sched_switch有2个实例，分别过滤prev和next；sched_wakeup、sched_wakeup_new过滤pid/comm）
- 适用场景：监控特定进程的所有状态变化

**模式2：全局S/D状态监控**
- 状态过滤：只监控S/D状态
- 监控范围：整个系统
- 使用事件：2个（sched_switch过滤prev_state；sched_wakeup无过滤）
- 适用场景：分析系统级睡眠、IO延迟等问题

**模式3：指定进程S/D状态监控**
- 状态过滤：只监控S/D状态
- 监控范围：进程类（进程、线程、workload）、进程名
- 使用事件：2个（sched_switch过滤prev_state和prev_pid/prev_comm；sched_wakeup过滤pid/comm）
- 适用场景：分析特定进程的睡眠、IO延迟等问题

### 事件处理

task-state使用状态机模型处理事件序列，通过三个关键事件的配对来计算各种状态的延迟时间：

- **排序依赖**: 默认启用排序，保证事件顺序性
- **丢事件处理**: 丢事件放弃已统计的所有进程状态，重新开始统计

#### 1. RUNDELAY（调度延迟）计算

**定义**：进程被唤醒后在运行队列里的等待时间，衡量调度器性能

**事件序列**：
```
时间轴: T1------------------------T2
事件:   sched_wakeup:pid=A        sched_switch:next_pid=A
动作:   进程A被唤醒                进程A获得CPU
延迟:   RUNDELAY_TIME = T2 - T1
```

#### 2. TASK_RUNNING（运行时间）计算

**定义**：进程实际在CPU上运行的时间

**事件序列**：
```
时间轴: T1-------------------------T2
事件:   sched_switch:next_pid=A    sched_switch:prev_pid=A
动作:   进程A开始运行               进程A被切换出CPU
延迟:   RUNNING_TIME = T2 - T1
```

#### 3. 睡眠状态（S/D/T/t/I）计算

**定义**：进程在各种睡眠状态的持续时间

**事件序列**：
```
时间轴: T1-------------------------T2
事件:   sched_switch:prev_pid=A    sched_wakeup:pid=A
动作:   进程A切换出CPU(睡眠)        进程被唤醒
延迟:   SLEEP_TIME = T2 - T1
状态:   来自sched_switch的prev_state字段
```

#### 特殊情况
- idle进程(pid=0)不参与状态统计
- 监控进程类时，事件需要Attach到所有CPU并设置过滤器
- `--ptrace`用于跟踪新创建的线程

## 输出

### 输出格式

**标准输出（周期性）**:
```
thread comm    St    calls        total(us)      min(us)      p50(us)      p95(us)      p99(us)      max(us)
```

**详细输出（`--than`触发）**:
```
[时间戳] 进程名 状态 延迟时间
    调用栈（如果启用-g）
```

### 输出字段
| 字段 | 说明 |
|------|------|
| thread | 线程ID（启用`--perins`时显示） |
| comm | 进程名（启用`--perins`时显示） |
| St | 状态类型(R/S/D/T/t/I/RD) |
| calls | 状态出现次数 |
| total(us) | 总延迟时间(微秒) |
| min(us) | 最小延迟时间(微秒) |
| p50(us) | 50分位延迟(微秒) |
| p95(us) | 95分位延迟(微秒) |
| p99(us) | 99分位延迟(微秒) |
| max(us) | 最大延迟时间(微秒) |

### 关键指标

| 指标 | 计算方法 | 异常阈值 |
|------|----------|----------|
| R状态total | 进程总运行时间 | 用于计算CPU利用率 |
| S状态total | 可中断睡眠总时间 | 值越大说明没有合理利用CPU |
| D状态total | 不可中断睡眠总时间 | 值越大需要重点分析，通常表示IO等待 |
| RD状态total | 调度延迟总时间 | 反映调度器繁忙程度 |
| R状态max | 单次最大运行时间 | 值越大需关注，通常会导致其他进程产生大的调度延迟 |
| D状态max | 单次最大不可中断时间 | >10ms需要关注，>100ms严重问题 |
| RD状态max | 单次最大调度延迟 | >4ms需要关注调度器行为 |

## 应用示例

### 基础示例
```bash
# 1. 系统整体状态监控
perf-prof task-state -i 1000                      # 每秒输出所有状态统计
perf-prof task-state --perins -i 5000             # 按线程输出，5秒间隔

# 2. 监控特定进程
perf-prof task-state -p 2347 -i 1000              # 监控进程2347所有状态
perf-prof task-state -t 2347,2348 -i 1000         # 监控指定线程

# 3. 监控D状态（IO等待）
perf-prof task-state -D -i 1000                   # 系统级D状态监控
perf-prof task-state -p 2347 -D --than 10ms -g    # 进程D状态详细分析
```

### 高级技巧
```bash
# 使用通配符监控相似进程名
perf-prof task-state --filter 'java*,python*' -D

# 监控workload并自动跟踪新线程
perf-prof task-state -- /usr/bin/stress --cpu 4

# 监控进程并启用ptrace跟踪新建线程
perf-prof task-state --ptrace -p 3479

# 排除S状态，只关注D状态（更精确）
perf-prof task-state -D --no-interruptible --than 20ms -g
```

### 性能优化
```bash
# 高频监控时增大缓冲区
perf-prof task-state -m 256 -i 500

# 使用进程名过滤（比pid过滤开销更小）
perf-prof task-state --filter 'mysql*' -D

# 只监控S/D状态减少事件量
perf-prof task-state -SD -i 1000
```

### 组合使用
```bash
# 与profile配合：task-state发现R状态问题后，用profile分析CPU热点
perf-prof task-state -p 2347 -i 1000              # 发现R状态耗时高
perf-prof profile -p 2347 -F 997 -g               # 分析CPU热点

# 与blktrace配合：task-state发现IO等待后，用blktrace分析具体IO
perf-prof task-state -D --than 10ms               # 发现D状态延迟
perf-prof blktrace -d /dev/sda -i 1000            # 分析块设备IO

# 多阶段分析
perf-prof task-state -i 1000                      # 阶段1：定位问题状态
perf-prof task-state -D --than 20ms -g            # 阶段2：详细分析原因
```

## 分析方法

### 数据驱动分析流程
1. **确定监控范围**：分析整体情况，不加状态过滤
2. **确定状态阈值**：分析哪些状态的`p99(us)`、`max(us)`值比较大
3. **按状态过滤**：高优D/RD、中优S/R、低优T/t/I
4. **打开详细输出**：设定阈值，打开堆栈分析延迟原因

### 阈值选择建议
- 每个系统的D状态、S状态、RD延迟特征都不同
- 必须基于实际数据来选择合理的监控阈值
- 不能预设通用的阈值

