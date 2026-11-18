# task-state - 进程状态耗时分析

task-state用来跟踪整个系统或进程状态（`R,S,D,T,t,I,RD`）的耗时分布。

## 概述
- **主要用途**: 诊断进程调度延迟、IO等待、睡眠等问题，跟踪进程状态的耗时分布，分析业务特征，业务画像。
- **适用场景**: CPU利用率高但业务性能差、进程响应慢、系统卡顿、IO等待时间长、调度器性能分析
- **功能分类**: 内建事件类，进程调度分析，状态监控
- **最低内核版本**: Linux 3.0+ (支持tracepoint事件)
- **依赖库**: libtraceevent, libperf
- **平台支持**: x86_64, ARM64
- **特殊限制**: 需要CAP_SYS_ADMIN权限或root用户运行

## 基础用法
perf-prof task-state [OPTION...] [-S] [-D] [--than ns] [--filter comm] [--perins] [--ptrace] [-g [--flame-graph file]] [-- workload workload_options]

如果使用workload，`--`前面是perf-prof的选项，后面是workload及其选项。

OPTION:
- `--watermark <0-100>`       未指定该选项：默认50
- `-m, --mmap-pages <pages>`  未指定该选项：默认为8，启用`-g`为16
- `--order`                   未指定该选项：默认启用

FILTER OPTION:
- `--user-callchain[=dwarf[,size]]`  启用-g时默认打开，"no-"前缀关闭。`=dwarf` 启用DWARF栈回溯
- `--kernel-callchain`    启用-g时默认打开，"no-"前缀关闭
- `--python-callchain`    启用-g时默认关闭

PROFILER OPTION:
- `--filter <comm>`       指定一个或多个comm，多个comm使用','分隔，comm支持通配符(`*?[]`)、不支持正则表达式。
                          跟`-p`、`-t`同时使用时，会优先使用`--filter`选项。
- `-S, --interruptible`   TASK_INTERRUPTIBLE状态，no-前缀排除
- `-D, --uninterruptible` TASK_UNINTERRUPTIBLE状态
- `--than <n>`            输出超过阈值的事件信息，包括起始事件
- `--perins`              按每个线程输出统计状态
- `-g, --call-graph`
- `--flame-graph <file>`
- `--ptrace`              使用ptrace跟踪新创建的线程

### 示例
```bash
# 系统整体状态监控
perf-prof task-state -i 1000

# 监控特定进程的S/D状态
perf-prof task-state -p 2347 -SD --than 20ms -g

# 按进程名监控Java和Python进程
perf-prof task-state --filter 'java,python*' -S --than 100ms -g

# 监控workload
perf-prof task-state -- ip link show eth0
```

## 核心原理

**状态定义**

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

**数据模型**

事件 → 排序 → 各状态延迟测量 → 统计聚合 → 周期显示

### 事件源
- **sample_type**：`PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW`
  - `PERF_SAMPLE_CALLCHAIN`: `-g`
- **内建事件**:
    - `sched:sched_switch`: prev_comm, prev_pid, prev_state, next_comm, next_pid
    - `sched:sched_wakeup`: comm, pid
    - `sched:sched_wakeup_new`: comm, pid

#### 过滤器

为了使task-state采样的事件尽可能的少，在使用不同的选项参数时，会选择不同的事件及过滤器。
- 过滤S/D状态：只需要 sched_switch 和 sched_wakeup 事件，且需要过滤 prev_state
- 监控范围：
  - 进程类（进程、线程、workload），需要过滤pid；进程名，需要过滤comm。这两种方式的处理逻辑相同
  - 整个系统：不需要设置过滤

综合后形成4种工作模式：

**模式0：全局状态监控**

- 状态过滤：不过滤S/D状态，监控所有状态
- 监控范围：整个系统
- 使用事件：3个
  - `sched:sched_switch`：无过滤器
  - `sched:sched_wakeup`：无过滤器
  - `sched:sched_wakeup_new`：无过滤器
- 适用场景：系统级全面监控，分析所有进程的所有状态

**模式1：指定进程全状态监控**

- 状态过滤：不过滤S/D状态，监控所有状态
- 监控范围：进程类（进程、线程、workload）、进程名
- 使用事件：4个（其中sched_switch有2个实例，设置的过滤器不同）
  - `sched:sched_switch`：进程类：过滤prev_pid；进程名：过滤prev_comm
  - `sched:sched_switch`：进程类：过滤next_pid；进程名：过滤next_comm
  - `sched:sched_wakeup`：进程类：过滤pid；进程名：过滤comm
  - `sched:sched_wakeup_new`：进程类：过滤pid；进程名：过滤comm
- 使用范围：监控特定进程的所有状态变化
- 特殊情况：监控进程类，需要`--ptrace`控制新建线程

**模式2：全局S/D状态监控**

- 状态过滤：只监控S/D状态
- 监控范围：整个系统
- 使用事件：2个
  - `sched:sched_switch`：过滤prev_state
  - `sched:sched_wakeup`：不设置过滤
- 使用范围：分析系统级睡眠、IO延迟等问题

**模式3：指定进程S/D状态监控**

- 状态过滤：只监控S/D状态
- 监控范围：进程类（进程、线程、workload）、进程名
- 使用事件：2个
  - `sched:sched_switch`：进程类：过滤prev_state和prev_pid；进程名：过滤prev_state和prev_comm
  - `sched:sched_wakeup`：进程类：过滤pid；进程名：过滤comm
- 使用范围：分析特定进程的睡眠、IO延迟等问题
- 特殊情况：监控进程类，需要`--ptrace`控制新建线程

**特殊情况**

针对进程类（进程、线程、workload）
- **特殊情况1**：事件需要转换为Attach到所有CPU + 设置过滤器。
  - **原因**：事件Attach到进程或线程后，linux内核会在进程睡眠时关闭perf_event，导致采样的事件不完整。
    - `sched:sched_switch`：Attach到线程A后，线程A在睡眠时可以采样到“sched_switch:prev_pid=A”事件，此时线程A绑定的sched_switch事件被关闭，无法采样到“sched_switch:next_pid=A”事件，会影响RUNDELAY的测量和TASK_RUNNING的测量
    - `sched:sched_wakeup`、`sched:sched_wakeup_new`：Attach到线程A后，只能采样到线程A唤醒别的线程的事件，无法采样到唤醒线程A的事件（sched_wakeup:pid=A），会影响睡眠状态延迟测量。
    - 所以，事件需要转换成Attach到所有CPU，设置过滤器，`sched:sched_switch`过滤prev_pid或next_pid字段；`sched:sched_wakeup`、`sched:sched_wakeup_new`过滤pid字段。
  - **举例**：`-p 234`进程234有2个线程"234"和"685"
    - `sched:sched_switch` 事件设置过滤器"prev_pid==234||prev_pid==685||next_pid==234||next_pid==685"
    - `sched:sched_wakeup`、`sched:sched_wakeup_new` 事件设置过滤器"pid==234||pid==685"，就能筛选出与进程234相关的所有事件，才能正常测量各状态的延迟。
  - 设置过滤器，要先找到进程下的所有线程。
- **特殊情况2**：内核过滤器有两个限制
    - 长度不超过一页（4KB，内核`perf_event_set_filter()`用`strndup_user(arg, PAGE_SIZE)`读取），线程多（几百个）时放不下。
    - 一个事件只能设置一次（再设返回`-EEXIST`），进程新建的线程无法加进已设置的过滤器。
    - 所以过滤分两层：**内核过滤器放过线程集合的超集，用户态过滤器精确匹配线程**（sample与转发都经过它）。
      - 内核过滤器由排好序的tid生成：连续的tid合并成区间`(prev_pid>=a&&prev_pid<=b)`；超过长度时从最小的缝隙开始合并相邻区间，直到放得下。缝隙里的其他任务进入用户态后被丢弃。
- **特殊情况3**：进程新建的线程
    - 新增`--ptrace`选项，用来解决此问题
      - 启用后，利用ptrace的能力主动控制新线程的创建，进程在调用`fork`、`vfork`，`clone`系统调用后，新线程默认处于STOPPED状态，并通知perf-prof进程，perf-prof把新线程加入线程集合，再通知新线程继续运行。
      - 新线程不在内核过滤器内时，**替换事件**：先打开带新过滤器的事件并启用，再停止、读完、关闭旧事件，不会漏掉事件。新旧事件同时启用的短暂时间内，同一条记录会写两次（同一cpu的ring buffer里前后相邻、内容相同），用户态丢掉其中一条。各事件分别替换，某个替换失败时保留旧事件，1秒后重试。
      - 启动时过滤器只覆盖已知线程；第一个新线程到来时转换为**开放末尾**`prev_pid>=新tid`。之后新建线程的tid更大，直接落在范围内，不必再替换。
      - pid回绕后，末尾起点落在已有区间的前面或中间：末尾只延伸到上面下一个被跟踪的tid之前（如`(prev_pid>=300&&prev_pid<=499)`），上面的区间保持不变，不会把一大段无关的pid都放进来。新线程越过这一段时，末尾起点移到它的tid，再替换一次。
      - 之后由一个定时器**延迟重新生成**过滤器，按紧急程度取到期时间（每个事件每秒最多替换一次）：
        - 用户态丢弃的记录数达到1000（开放末尾放进了其他新建的任务）：立即到期，末尾起点前移到最近分配的pid（`/proc/loadavg`第5个字段）。
        - 新线程已在过滤器内：1秒后到期，每来一个新线程往后推。到期时说明1秒内没有新线程，**关闭末尾**，过滤器回到只覆盖已跟踪的线程。再有新线程时重新开放。
        - 线程退出：10秒后到期，每次退出往后推，从第一次退出起最多60秒。已退出的tid在过滤器里多留这段时间；pid不会很快回绕，期间tid复用由用户态过滤挡住。
    - **用户态过滤器按需启用**：内核过滤器只放进跟踪的线程（不带末尾、没合并缝隙、生成后没有线程退出）时，不再执行用户态过滤；有线程退出时立即启用，替换事件期间（去重）也启用。每次重新生成过滤器时重新判断。
      - `-v`打印每次替换后的过滤器、用户态过滤器的启用/关闭（`ftrace_filter on/off`），结束时打印替换次数、失败次数、丢弃的记录数与重复记录数。
    - `--ptrace`会带来停顿，对于频繁创建线程的业务慎用此选项。
    - `--ptrace`不适用于监控进程名，进程创建的新线程有相同的进程名。不能和`--filter`一起使用。

**各类过滤器**

`-p pid` 指定一个或多个pid，在设置过滤器时，先找出每个pid的所有线程，按tid生成区间（见特殊情况2）。
`-t tid` 指定一个或多个tid，在设置过滤器时，按tid生成区间。
`--filter <comm>` 指定一个或多个comm，对每个comm设置过滤器。comm使用通配符(包含`*?[`任意字符)，过滤条件需要使用 `~` 运算符；否则使用 `==` 运算符。

- 过滤prev_pid：`prev_pid==11||(prev_pid>=20&&prev_pid<=22)||prev_pid>=30`（单个tid、连续的tid、开放末尾）
- 过滤prev_comm：`prev_comm=="xx" || prev_comm~"yy*"`
- 过滤next_pid：`next_pid==11||(next_pid>=20&&next_pid<=22)`
- 过滤next_comm：`next_comm=="xx" || next_comm~"yy*"`
- 过滤pid：`pid==11||(pid>=20&&pid<=22)`
- 过滤comm：`comm=="xx" || comm~"yy*"`
- 过滤prev_state：
  - `-S`选项：`prev_state==1`
  - `-D`选项：`prev_state==2`
  - `-SD`选项：`prev_state==1 || prev_state==2`
- 过滤prev_state和prev_pid：
  - `-S`选项：`(prev_state==1) && (prev_pid==11 || prev_pid==22)`
  - `-D`选项：`(prev_state==2) && (prev_pid==11 || prev_pid==22)`
  - `-SD`选项：`(prev_state==1 || prev_state==2) && (prev_pid==11 || prev_pid==22)`
- 过滤prev_state和prev_comm：
  - `-S`选项：`(prev_state==1) && (prev_comm=="xx" || prev_comm~"yy*")`
  - `-D`选项：`(prev_state==2) && (prev_comm=="xx" || prev_comm~"yy*")`
  - `-SD`选项：`(prev_state==1 || prev_state==2) && (prev_comm=="xx" || prev_comm~"yy*")`

### 事件处理
task-state使用状态机模型处理事件序列，通过三个关键事件的配对来计算各种状态的延迟时间：

1. **RUNDELAY计算**: sched_wakeup(被唤醒) → sched_switch(获得CPU)
2. **TASK_RUNNING计算**: sched_switch(获得CPU) → sched_switch(失去CPU)
3. **睡眠状态计算**: sched_switch(失去CPU) → sched_wakeup(被唤醒)

默认启用排序，才可以保证事件的顺序性，才能安全的计算延迟。

#### 1. RUNDELAY（调度延迟）计算

**定义**：进程被唤醒后在运行队列里的等待时间，衡量调度器性能

**事件序列**：
```
时间轴: T1------------------------T2
事件:   sched_wakeup:pid=A        sched_switch:next_pid=A
动作:   进程A被唤醒                进程A获得CPU
状态:   S/D/T/t/I → RUNNING       RUNNING → RUNNING
延迟:   RUNDELAY_TIME = T2 - T1
```

#### 2. TASK_RUNNING（运行时间）计算

**定义**：进程实际在CPU上运行的时间

**事件序列**：
```
时间轴: T1-------------------------T2
事件:   sched_switch:next_pid=A    sched_switch:prev_pid=A
动作:   进程A开始运行               进程A被切换出CPU
状态:   RUNNING → RUNNING          RUNNING → S/D/T/t/I
延迟:   RUNNING_TIME = T2 - T1
```

#### 3. 睡眠状态（S/D/T/t/I）计算

**定义**：进程在各种睡眠状态的持续时间

**事件序列**：
```
时间轴: T1-------------------------T2
事件:   sched_switch:prev_pid=A    sched_wakeup:pid=A
动作:   进程A切换出CPU(睡眠)        进程被唤醒
状态:   RUNNING → S/D/T/t/I        S/D/T/t/I → RUNNING
延迟:   SLEEP_TIME = T2 - T1
状态：  S/D/T/t/I来自sched_switch的prev_state字段
```

#### 状态转换表

| 当前状态 | 触发事件 | 下一状态 | 计算延迟 | 记录时间戳 |
|----------|----------|----------|----------|---------|
| RUNNING | sched_switch(prev_pid) | S/D/T/t/I(prev_state) | 计算prev_pid的RUNNING_TIME | prev_pid切换出CPU的时间 |
| S/D/T/t/I | sched_wakeup(pid) | RUNDELAY | 计算pid的各个状态的SLEEP_TIME | 记录pid被唤醒的时间 |
| RUNDELAY | sched_switch(next_pid) | RUNNING | 计算next_pid的RUNDELAY_TIME | 记录next_pid获得CPU的时间 |

#### 特殊情况
- idle进程(pid=0)不参与状态统计

#### 丢事件和恢复处理逻辑
- 丢事件放弃已统计的所有进程状态，重新开始统计

### 状态统计
- **信号处理**
  - SIGUSR1: 输出统计信息
  - SIGUSR2: 打印内建事件的过滤器，并输出统计信息

## 输出

### 输出格式
```
thread comm    St    calls        total(us)      min(us)      p50(us)      p95(us)      p99(us)      max(us)
```
- **表头含义**:
  - thread/comm: 线程ID和进程名(--perins时显示)
  - St: 状态类型(R/S/D/T/t/I/RD)
  - calls: 状态出现次数
  - total(us): 总延迟时间(微秒)
  - min/p50/p95/p99/max(us): 延迟分位数统计(微秒)
- **行索引**:
  - 启用`--perins`，按（thread，St）聚合数据，每个线程的状态会单独统计
  - 未启用，按（St）聚合数据，所有线程按状态累计到一起
  - 每次输出后清空统计信息，重新开始统计
- **排序规则**:
  - 第一，thread从小到大（启用`--perins`时）
  - 第二，St从小到大
- **详细输出**:
  - `--than`：输出超过阈值的事件信息，包括起始事件
  - `-g`：打开堆栈，输出事件时会输出堆栈，`--flame-graph`输出堆栈为折叠栈，用于生成火焰图
  - `--perins`：会显示每个线程的状态统计
  - 选项关系：指定`--than`则`-g`生效；指定`-g`则`--flame-graph`生效

### 关键指标
- total(us):
  - R：进程总运行时间，可以粗略计算cpu利用率
  - S：可中断睡眠总时间，值越大说明没有合理的利用cpu
  - D：不可中断睡眠总时间，值越大说明长时间在内核里等待资源，越需要重点分析
  - RD：调度延迟总时间，反映调度器的繁忙程度，值越大越需要重点分析
- max(us):
  - R：进程单次在cpu上的最大执行时间，值越大说明其他进程有越大的调度延迟
  - S：进程单次睡眠的最大时间，值越大说明进程睡眠时间越长，使用`-g`打开堆栈分析，使用`-S`选项过滤分析
  - D：进程单次不可中断的最大时间，值越大说明在内核等待资源的时间越长，使用`-g`打开堆栈分析、使用`-D`过滤、使用`--no-interruptible`排除S状态、使用`--than`选择合理的阈值
  - RD：进程单次调度延迟最大值，值越大需要分析调度器的行为


## 分析方法

### 基础分析方法
1. 确定监控范围，分析整体情况
   - 确定监控的是进程、线程、workload、整个系统
   - 不加状态过滤，不加详细输出参数
   - 分析哪些状态的`p99(us)`、`max(us)`值比较大，确定状态的阈值
2. 按状态过滤，确定重点
   - 高优分析D、RD状态、中优分析S、R状态、低优分析T、t、I状态
   - 使用`-S`、`-D`、`--no-interruptible`筛选
3. 打开详细输出
   - 设定状态阈值，由于每个系统、每个进程的特征都不同，不能预设通用的阈值。必须基于实际数据来选择合理的监控阈值。
   - 打开堆栈，分析延迟原因，确定根因
   - 火焰图分析

### 数据驱动分析
- 每个系统的D状态、S状态、RD延迟特征都不同
- 通过采集数据来确定最优阈值


## 应用示例

```bash
# 监控所有进程的整体情况
perf-prof task-state --perins -i 5000

# 监控D状态进程，通常表示IO等待问题
perf-prof task-state -D -i 1000

# 监控PID 2347的S/D状态延迟，超过20ms的详细事件，包含调用栈
perf-prof task-state -p 2347 -SD --than 20ms -g

# 监控Java和Python进程的睡眠状态
perf-prof task-state --filter 'java,python' -S --than 100ms -g
```

### 高级技巧
```bash
# 使用通配符监控相似进程名
perf-prof task-state --filter 'java*,python*' -D

# 监控workload并自动跟踪新线程
perf-prof task-state -- /usr/bin/stress --cpu 4

# 监控进程3479，启用ptrace来跟踪新建线程
perf-prof task-state --ptrace -p 3479
```

### 性能优化
- **缓冲区大小**: 默认8个页，高频监控时使用`-m 256`提升性能
- **采样频率**: 系统监控建议1-2秒间隔，问题诊断时500ms间隔
- **过滤器优化**: 使用进程名过滤比pid过滤开销更小

### 参数调优
- **--ptrace调优**: 仅在监控不频繁创建线程的进程时使用，降低影响
- **状态过滤优化**: 只关注D状态时使用-D选项，可减少事件处理量

### 组合使用
- **与profile配合**: task-state发现R状态问题后，用profile分析CPU热点
- **与blktrace配合**: task-state发现IO等待后，用blktrace分析具体IO设备
- **多阶段分析**: 先用task-state定位问题状态，再用multi-trace分析根因

## 相关资源
- [事件基础文档](../events/)
- [sample_type采样类型说明](../sample_type.md)
- [选项参数完整参考](../main_options.md)
- [实际案例分析](../examples/)