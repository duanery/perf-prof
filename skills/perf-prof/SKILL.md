---
name: perf-prof
metadata:
  version: "0.1"
  source: https://github.com/OpenCloudOS/perf-prof.git
description: 使用perf-prof进行Linux系统问题分析。perf-prof是基于perf_event的系统级分析工具，事件在内存中实时处理，可长期运行。触发场景：(1) CPU使用率高、热点分析 (2) 进程状态异常(D/S状态多) (3) 延迟抖动、响应慢 (4) 内存泄露或增长异常 (5) 块设备IO慢 (6) 虚拟机性能问题 (7) 事件聚合统计 (8) 自定义脚本分析。核心分析器：profile(CPU采样)、task-state(进程状态)、multi-trace(延迟分析)、kmemleak(内存泄露)、blktrace(IO延迟)、top/sql(聚合统计)、kvm-exit(虚拟化退出)、rundelay(调度延迟)、syscalls(系统调用耗时)、python(自定义脚本分析)。适用于：性能问题定位、内核/应用开发调试、学习理解Linux内核机制（调度、内存、IO、中断等）。English: Linux system performance analysis with perf-prof, a perf_event-based tool for real-time in-memory event processing. Triggers: high CPU usage, hotspot profiling, process state issues (D/S states), latency spikes, slow response, memory leak detection, memory growth, block device IO slowness, VM performance issues, event aggregation, custom script analysis. Key profilers: profile (CPU sampling), task-state (process state), multi-trace (latency analysis), kmemleak (memory leak), blktrace (IO latency), top/sql (aggregation), kvm-exit (virtualization exit), rundelay (scheduling delay), syscalls (syscall latency), python (custom scripting).
---

# perf-prof 系统性能分析

## 概述

perf-prof 是一个Linux系统级性能分析工具，基于perf_event在内存中实时处理事件。本技能提供使用perf-prof分析系统问题的工作流程和方法。

### 安装

使用前先检查 perf-prof 是否已安装：

```bash
which perf-prof && perf-prof --version
```

如果系统中没有 perf-prof 二进制文件，**需要询问用户确认**是否可以下载源码并编译安装，然后执行以下步骤：

```bash
# 1. 下载源码
git clone https://github.com/OpenCloudOS/perf-prof.git
cd perf-prof

# 2. 安装依赖包
yum install -y xz-devel elfutils-libelf-devel libunwind-devel python3-devel

# 3. 编译
make

# 4. 验证安装
./perf-prof --version
```

## 核心分析流程

### 第一步：确定问题类型

- **确定系统级问题**：使用mpstat、top、vmstat等系统工具确认问题类型
- **确定分析领域**：CPU性能分析、内存分析、进程调度分析、I/O性能分析、虚拟化分析、硬件性能监控、中断与死锁、事件跟踪
- **确定分析技术**：采样分析、计数分析、聚合分析、延迟分析、进程状态分析、断点分析、联合分析

**操作步骤：**

1. **使用系统工具初步诊断**（可选）：
   ```bash
   top -1                    # 查看CPU使用率、进程状态
   mpstat -P ALL 1           # 查看各CPU利用率
   vmstat 1                  # 查看内存、IO、CPU综合状态
   iostat -x 1               # 查看磁盘IO
   ```

2. **根据决策树选择分析器**：
   ```
   系统问题
   ├── CPU相关
   │   ├── CPU使用率高 → profile (采样分析)
   │   ├── 进程运行时间统计 → top (sched_stat_runtime)
   │   └── CPU上运行进程监控 → oncpu
   ├── 调度相关
   │   ├── 进程状态分析(R/S/D) → task-state
   │   ├── 调度延迟分析 → rundelay
   │   └── 唤醒延迟分析 → multi-trace
   ├── 内存相关
   │   ├── 内核内存泄露 → kmemleak
   │   ├── 内存分配统计 → kmemprof
   |   ├── 缺页异常 → page-faults
   |   ├── 跟踪内存写入 → breakpoint
   |   └── 读取内核内存 → kcore
   ├── IO相关
   │   └── 块设备延迟 → blktrace
   ├── 系统调用
   │   └── 系统调用耗时 → syscalls
   ├── 虚拟化
   │   └── KVM退出延迟 → kvm-exit
   ├── 硬件相关
   |   ├── 断点分析 → breakpoint
   |   └── 硬件状态 → tlbstat、llcstat、hwstat
   ├── 事件计数
   |   ├── 高频计数、微突发检测 → hrcount
   |   └── 低频计数 → stat
   ├── 通用事件追踪 → trace
   └── 自定义脚本分析 → python
   ```

### 第一步(续)：问题定界 - 用户态 vs 内核态 - Guest vs Host

如有必要，在选择具体分析器之前，先通过定界分析确定问题发生在用户态还是内核态，Guest还是Host：

**定界分析路径：**
```
问题定界
├── 方法1：syscalls分析系统调用耗时
│   ├── 系统调用耗时高 → 内核态问题 → 进入内核分析路径
│   └── 系统调用耗时低 → 用户态问题 → 进入用户态分析路径
│
├── 方法2：task-state分析进程运行状态
|   ├── R状态(Running)占比高 → 用户态CPU消耗 → 进入用户态分析路径
|   ├── S状态(Sleeping)占比高 → 等待唤醒 → 分析唤醒源
|   ├── D状态(Disk Sleep)占比高 → IO/锁等待 → 进入内核分析路径
|   └── RD状态(Runnable)占比高 → 调度延迟 → 进入内核分析路径
|
└── 方法3：kvm-exit分析虚拟化退出耗时
    ├── 虚拟化退出耗时高 → Host问题 → 进入内核分析路径
    └── 虚拟化退出耗时低 → Guest问题 → 进入Guest内分析
```

**内核态问题分析路径：**
```
# 1. 调度延迟分析
# 2. 系统调用深入分析：哪个系统调用慢，系统调用内发生的进程切换等
# 3. IO延迟分析
# 4. 内核热点采样
# 5. 虚拟化退出耗时分析
```

**用户态问题分析路径：**
```
# 1. 用户态CPU热点采样
# 2. 用户态函数追踪（添加uprobe点）
# 3. 用户态函数耗时分析
# 4. 业务层面分析：结合业务日志、应用metrics等
```

### 第二步：选择分析器（可多选）

根据问题类型选择对应分析器，必要时选择多个分析器相互验证。

以下分析器有文档，参考`references/`目录下的详细文档，优先通过第三步获取概要信息及示例：
- [profile](references/profilers/profile.md) - CPU采样分析：采样分析内核态/用户态CPU利用率
- [top](references/profilers/top.md) - 键值聚合统计分析：基于事件的键值聚合分析，用于分析系统事件分布和热点
- [sql](references/profilers/sql.md) - SQL聚合分析：基于SQLite的事件聚合查询、统计计算、多维度数据透视等复杂分析操作，用于分析系统事件分布和热点。优先使用
- [hrcount](references/profilers/hrcount.md) - 高精度事件计数分析：统计事件发生的频率，利用hrtimer实现毫秒/微秒级计数粒度，仅支持CPU级监控
- [stat](references/profilers/hrcount.md) - 事件计数分析：统计事件发生的频率，支持CPU和进程级监控，与hrcount互补
- [task-state](references/profilers/task-state.md) - 进程状态分析：跟踪整个系统或进程状态（`R,S,D,T,t,I,RD`）的耗时分布
- [oncpu](references/profilers/oncpu.md) - CPU进程监控，2种模式：实时监控CPU上运行的进程及其运行时间统计；实时监控进程在哪些CPU上运行及其运行时间统计。
- [multi-trace](references/profilers/multi-trace.md) - 多事件关系分析：将复杂的多事件关系转换为两两事件关系进行分析，支持延迟分析(delay)、事件配对分析(pair)
- [rundelay](references/profilers/rundelay.md) - 调度延迟分析：专用于分析进程调度延迟
- [syscalls](references/profilers/syscalls.md) - 系统调用耗时分析：专用于分析系统调用的延迟和错误率
- [kmemprof](references/profilers/kmemprof.md) - 内存分配分析：专用于分析内存分配的生命周期，统计内存分配/释放的字节数和堆栈信息
- [kmemleak](references/profilers/kmemleak.md) - 内存泄露分析：检测用户态和内核态内存分配器的内存泄漏问题
- [blktrace](references/profilers/blktrace.md) - 块设备IO分析：分析从IO请求创建到完成的整个生命周期，跟踪块设备上的IO延迟
- [kvm-exit](references/profilers/kvm-exit.md) - KVM虚拟化退出延迟分析：分析KVM虚拟机退出(VM-Exit)到重新进入(VM-Entry)之间的延迟，统计不同退出原因的延迟分布
- [trace](references/profilers/trace.md) - 事件追踪与打印：实时跟踪和显示内核/用户空间事件，是最基础和灵活的事件分析工具
- [python](references/profilers/python.md) - Python脚本事件处理：将perf事件转换为PerfEvent对象，通过自定义Python脚本进行灵活分析，支持tracepoint和profiler事件源的联合分析
- [breakpoint](references/profilers/breakpoint.md) - 硬件断点分析：利用CPU调试寄存器跟踪指定地址的读写执行，支持x86内核地址写入值解码

以下分析器无文档，必须通过第三步获取概要信息及示例：
- kcore - 读取内核内存：通过/proc/kcore读取内核虚拟地址或符号对应的内存内容，支持hex dump和字符串输出
- expr - 表达式编译器和模拟器：支持复杂的表达式计算，用于事件属性中的复杂计算
- tlbstat - dTLB状态监控：监控数据TLB的命中和缺失情况，用于分析内存访问性能
- llcstat - 最后一级缓存监控：监控LLC（Last Level Cache）的命中和缺失统计
- hwstat - 硬件状态监控：监控CPU cycles和IPC（每周期指令数）等硬件性能指标
- usdt - 用户态静态探针：管理ELF文件中的USDT（User Statically-Defined Tracing）探针，支持list/add/del操作
- kvmmmu - KVM MMU页表观察：观察x86平台KVM虚拟机的MMU页表映射，包括SPTE设置和MMIO标记
- irq-off - 中断关闭检测：使用周期性hrtimer检测中断被关闭的情况，--than阈值应大于--period周期
- hrtimer - 高分辨率条件采样：基于hrtimer周期性采样，根据事件计数表达式条件决定是否输出采样
- page-faults - 缺页异常跟踪：跟踪进程或CPU的缺页异常，支持用户态寄存器采样辅助堆栈分析
- ldlat-stores - Intel存储指令计数：统计Intel平台写内存指令的延迟（MEM_INST_RETIRED.ALL_STORES）
- ldlat-loads - Intel加载延迟统计：统计Intel平台读内存指令的延迟（MEM_TRANS_RETIRED.*），可设置延迟阈值
- num-dist - 数值分布分析：分析事件字段数值的分布情况，支持热图输出和调用图
- percpu-stat - 精选事件统计：统计预置的系统关键事件（上下文切换、中断、软中断、定时器、KVM退出、网络收发、内存、文件系统、系统调用、CPU空闲等）
- watchdog - Hard Lockup和Soft Lockup检测：基于NMI watchdog检测系统硬死锁和软死锁情况
- cpu-util - CPU利用率报告：报告CPU在Guest/Host、用户态/内核态的利用率分布，基于profile实现 
- split-lock - x86分裂锁检测：检测跨缓存行的锁操作（Super Queue lock splits），会严重影响性能
- misc - 杂项跟踪：跟踪内核符号注册/注销、BPF程序加载/卸载、cgroup事件、内核代码自修改等
- nested-trace - 嵌套事件分析：分析嵌套事件（如函数调用、中断等），基于multi-trace实现
- bpf:kvm_exit - KVM退出延迟BPF分析：在内核态处理kvm_exit和kvm_entry事件生成bpf:kvm_exit事件，提供详细的延迟分解
- event-care - 事件丢失和乱序关注：监控事件的丢失和乱序情况

#### 功能分类

**按功能领域分类**
- CPU性能分析：`profile`,`cpu-util`,`oncpu`
- 内存分析：`kmemleak`,`kmemprof`,`page-faults`
- 进程调度分析：`task-state`,`rundelay`,`oncpu`
- I/O性能分析：`blktrace`
- 虚拟化分析：`kvm-exit`,`kvmmmu`,`bpf:kvm_exit`
- 硬件性能监控：`hwstat`,`llcstat`,`tlbstat`,`split-lock`,`ldlat-stores`,`ldlat-loads`,`breakpoint`
- 中断与死锁：`irq-off`,`watchdog`
- 数据分析：`sql`,`python`,`trace`
- 辅助工具：`expr`,`misc`,`kcore`,`list`,`usdt`,`help`

**按分析技术分类**
- 采样分析：`profile`
- 计数分析：`hrcount`,`stat`,`percpu-stat`,`hrtimer`,`num-dist`
  - 高精度计数：`hrcount`
  - 低精度计数：`stat`
- 聚合分析：`top`,`sql`
- 延迟分析：`multi-trace`,`nested-trace`
  - 调度延迟：`rundelay`
  - 系统调用耗时：`syscalls`
  - IO延迟：`blktrace`
  - 虚拟化退出耗时：`kvm-exit`
- 状态监控：`task-state`,`oncpu`
  - 进程状态：`task-state`
- 追踪分析：`trace`
- 脚本分析：`python`
- 断点分析：`breakpoint`
- 联合分析：`multi-trace`,`trace`,`python`

**按事件依赖程度分类**
- 无事件依赖类：`misc`,`kcore`,`list`,`usdt`,`help`
- 内建事件类：`tlbstat`,`llcstat`,`hwstat`,`breakpoint`,`kvmmmu`,`irq-off`,`page-faults`,`ldlat-stores`,`ldlat-loads`,`oncpu`,`blktrace`,`kvm-exit`,`percpu-stat`,`watchdog`,`task-state`,`cpu-util`,`profile`,`split-lock`
- 自定义事件类：`expr`,`stat`,`hrcount`,`event-care`,`hrtimer`,`rundelay`,`nested-trace`,`syscalls`,`kmemprof`,`multi-trace`,`top`,`num-dist`,`kmemleak`,`trace`,`sql`,`python`

**ebpf类**
`bpf:kvm_exit`

### 第三步：查看帮助与文档

**操作步骤：**

1. **查看分析器帮助**：
   ```bash
   perf-prof <profiler> -h
   ```
   帮助信息包含：
   - 概要信息（关键原理）
   - EXAMPLES示例（关键参考）
   - 支持的选项参数及其含义
   - 是否需要`-e`事件选项

2. **阅读分析器文档**：
   参考`references/`目录下对应的md文档，获取详细用法和典型场景。

3. **判断事件需求**：
   ```
   分析器类型判断：
   ├── 内建事件类（无需-e选项）→ 直接跳转第六步
   │   例如：profile, task-state, blktrace, kvm-exit, oncpu
   └── 自定义事件类（需要-e选项，或其他需要事件的选项）→ 继续确定事件选项要求
       例如：trace, top, multi-trace, kmemleak, sql
   ```

4. **确定事件选项要求**
   - 使用 `-e, --event` 选项指定事件
     - 只允许一个-e选项，多个事件使用逗号分隔，事件顺序无要求。
     - 允许多个-e选项，多个-e选项的顺序有不同含义。例如：multi-trace, rundelay, syscalls, kmemprof
     - -e选项使用固定事件（属性可调，可扩充untraced事件）。例如：rundelay
     - -e选项使用固定事件（过滤器可调）。例如：syscalls
   - 使用 `--alloc/--free` 选项指定事件。只有kmemleak使用 

**示例：**
```bash
# 查看profile分析器帮助
perf-prof profile -h

# 查看top分析器帮助（需要-e选项）
perf-prof top -h
```

### 第四步：选择事件

**操作步骤：**

1. **列出系统事件**：
   ```bash
   # 列出所有tracepoint事件
   perf-prof list

   # 按类别筛选事件
   perf-prof list | grep "^sched:"    # 调度相关
   perf-prof list | grep "^kmem:"     # 内存相关
   perf-prof list | grep "^block:"    # 块设备相关
   perf-prof list | grep "^syscalls:" # 系统调用相关
   ```

2. **查看事件字段**：
   ```bash
   # 查看事件的字段定义（用于配置过滤器和表达式）
   perf-prof trace -e <sys:name> help

   # 示例：查看sched_wakeup事件字段
   perf-prof trace -e sched:sched_wakeup help
   ```

3. **常用事件速查**：
   | 分析场景 | 推荐事件 | 关键字段 |
   |---------|---------|---------|
   | 进程唤醒 | sched:sched_wakeup | pid, comm, prio, target_cpu |
   | 进程切换 | sched:sched_switch | prev_pid, next_pid, prev_comm, next_comm |
   | 运行时间 | sched:sched_stat_runtime | pid, comm, runtime |
   | 内存分配 | kmem:kmalloc | ptr, bytes_alloc, call_site |
   | 内存释放 | kmem:kfree | ptr |
   | 软中断 | irq:softirq_entry/exit | vec |
   | 系统调用 | raw_syscalls:sys_enter/exit | id |

4. **动态探针**（当系统事件不满足需求时）：
   ```bash
   # kprobe - 内核函数探针
   -e 'kprobe:try_to_wake_up'

   # kretprobe - 内核函数返回探针
   -e 'kretprobe:try_to_wake_up'

   # uprobe - 用户态函数探针，二进制路径带有'/'，必须使用双引号
   -e 'uprobe:func@"/path/to/binary"'

   # uretprobe - 用户态函数返回探针，二进制路径带有'/'，必须使用双引号
   -e 'uretprobe:func@"/path/to/binary"'
   ```

5. **新增动态探针**（当系统事件不满足要求且动态探针无法使用或需要增加参数时）：

   通过 `/sys/kernel/debug/tracing/kprobe_events` 和 `/sys/kernel/debug/tracing/uprobe_events` 文件手动新增探针。

   判断新增的动态探针是内核态的还是用户态的。

   详细文档：
   - [kprobe_events.md](references/kprobe_events.md) - 内核态探针
   - [uprobe_events.md](references/uprobe_events.md) - 用户态探针

6. **延迟分析：选择事件原则**：
   - 确定延迟路径的起点事件、中间事件（可以没有）、终点事件
   - 确定延迟路径的上下文：所选事件是在线程上下文还是CPU上下文执行

### 第五步：配置事件过滤器和属性

根据分析需求，使用下方 [事件选择语法](#事件选择语法) 配置事件。

**配置流程：**

1. **确定事件格式**：参考 [基本格式](#基本格式) 选择事件类型
   - tracepoint事件：`sys:name`
   - 动态探针：`kprobe:func`、`uprobe:func@"file"`

2. **配置过滤器**（可选，内核态执行）：
   参考 [过滤器语法](#过滤器语法) 和 [Event_filtering.md](references/Event_filtering.md)
   ```bash
   # 格式：sys:name/filter/
   -e 'sched:sched_wakeup/pid>1000 && prio<10/'
   -e 'sched:sched_wakeup/comm~"java*"/'
   ```
   - 事件有过滤器时，-e选项必须要使用单引号。避免与bash的运算符冲突。

3. **配置属性**（可选，用户态处理）：
   参考 [事件属性](#事件属性) 选择所需属性
   ```bash
   # 格式：sys:name/filter/ATTR/ATTR/
   -e 'sched:sched_wakeup//stack/'                    # 无过滤器，有属性
   -e 'sched:sched_wakeup/pid>1000/stack/alias=wk/'   # 过滤器 + 属性
   ```

4. **配置表达式**（可选，复杂计算）：
   参考 [expr.md](references/expr.md)
   ```bash
   # 1. 查看expr帮助
   perf-prof expr -h

   # 2. 属性值使用表达式
   -e 'sched:sched_stat_runtime//top-by=(runtime/1000)/'
   -e 'sched:sched_wakeup//key=(prio<100?pid:0)/'
   ```
   - 事件属性使用表达式时，必须使用括号包含整个表达式部分。避免除法运算符被作为属性分隔符。

5. **延迟分析：配置key属性**（可选，仅用于multi-trace）：
   key属性用来关联延迟路径上的事件。只有相同key的事件才可以计算延迟。

   **Key选择的本质问题**：你希望延迟路径上的事件如何关联在一起？

   **从事件上下文理解Key选择**

   每个事件发生时，都有两个基本上下文：
   - **CPU上下文**：事件发生在哪个CPU上
   - **线程上下文**：事件发生在哪个线程上（common_pid）

   **核心问题：延迟路径上，上下文是否变化？**

   | 上下文变化情况 | Key选择 | 典型场景 |
   |---------------|----------|----------|
   | 同CPU、同线程 | 默认（按CPU）或不指定 | 软中断、hardirq |
   | 可能跨CPU、同线程 | `-k common_pid` 或 `-p`/`-t` | 系统调用（线程可能迁移） |
   | 跨线程、可能跨CPU | `key=资源标识` | 调度延迟、跨进程通信 |

   **Key的作用** = 在"上下文可能变化"的情况下，找到"不变的关联标识"

   **Key选择判断流程**
   ```
   问：延迟路径上的事件，线程上下文是否相同？

   是 → 问：CPU上下文是否相同？
        │
        ├── 是 → 不需要key（默认按CPU）
        │        例：软中断、本地定时器
        │
        └── 否 → key=common_pid，需要 --order
                 例：系统调用（线程可能迁移）

   否 → 必须用"业务字段"作为key，需要 --order
        问：什么东西从起点事件"传递"到终点事件？
        │
        ├── 进程ID → key=pid相关字段
        │   例：调度（pid → next_pid）
        │
        ├── 资源指针 → key=ptr/address
        │   例：内存分配（kmalloc.ptr → kfree.ptr）
        │
        └── 请求标识 → key=request_id/bio
            例：I/O请求（提交 → 完成）
   ```
   **注意**：
   - `rundelay`不需要设置key属性，内部会自动设置
   - `syscalls`不需要设置key属性，内部会自动设置
   - key值只能是一个u64的数值，需要多个字段组合成一个复合的key时，需要使用表达式

**常用属性速查：**
| 属性 | 用途 | 适用分析器 |
|-----|------|-----------|
| stack | 启用调用栈 | 所有支持堆栈的分析器 |
| alias=str | 事件别名 | top, multi-trace, sql |
| key=EXPR | 关联键 | multi-trace, top |
| top-by=EXPR | 排序字段 | top |
| ptr=EXPR | 指针字段 | kmemleak |
| size=EXPR | 大小字段 | kmemleak, kmemprof |

完整属性列表参考 [事件属性](#事件属性)。

### 第六步：执行分析与输出解读

**通用选项参数：**
```bash
-m, --mmap-pages    # perf ringbuffer大小(页数)
-i, --interval <ms> # 输出间隔
-C, --cpus          # 监控指定的cpu
-p, --pids          # 监控指定的进程
-o, --output <file> # 把stdout/stderr重定向到文件
--watermark <0-100> # 唤醒perf-prof的水位，设置可降低工具自身的cpu消耗
--perins            # 按每个实例输出，不同的分析器实例的含义可能不同。一般：-p/-t选项实例为线程; 其他（-C选项等）实例为CPU
```

**根据分析器类型选择执行流程：**

#### 6.1 延迟分析流程（multi-trace、syscalls、rundelay、blktrace、kvm-exit等）

延迟分析采用**渐进式分析**，从统计概览逐步深入到细节：

**步骤1：基础统计（了解延迟分布）**
```bash
# 先执行基础统计，周期性输出延迟分布
perf-prof <profiler> [事件选项] -i 1000 --order

# 示例：系统调用延迟统计
perf-prof syscalls -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit -i 1000

# 示例：调度延迟统计
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch -e sched:sched_switch -p <pid> -i 1000

# 示例：块设备IO延迟统计
perf-prof blktrace -d /dev/sda -i 1000
```
解读输出：关注延迟分布（min/avg/max）、P99等百分位数，确定异常阈值。重点关注max等大的延迟毛刺。

阈值设置原则：
- 先用p99的值作为阈值，避免大量输出，占用模型上下文。

**步骤2：阈值过滤（聚焦异常事件）**
```bash
# 根据步骤1的统计结果，设置--than参数过滤超过阈值的事件，避免设置的阈值过小造成大量输出。
perf-prof <profiler> [事件选项] -i 1000 --order --than <threshold>
```
解读输出：统计异常事件数量和分布，确认问题严重程度。

**步骤3：细节跟踪（定位根因）**
```bash
# 加上--detail参数，筛选并输出异常事件范围内的中间事件
perf-prof <profiler> [事件选项] -i 1000 --order --than <threshold> --detail=<samecpu>
```
解读输出：分析每个异常事件的时间戳、进程、堆栈等信息，定位根因。

**步骤4：丰富细节（定位根因）**
```bash
# 针对multi-trace、syscalls、rundelay：加上untraced事件，打开堆栈，还原延迟范围内的中间细节
perf-prof <profiler> -e <event> -e <event,event//untraced/stack/> -i 1000 --order --than <threshold> --detail=<samecpu>
```
解读输出：分析延迟范围的中间细节，定位根因。

#### 6.2 其他分析器执行流程

```bash
# 直接执行，周期性输出统计结果
perf-prof <profiler> [事件选项] -i 1000 [其他选项]
```

#### 6.3 输出解读与迭代

```
分析输出：
├── 问题定位清晰 → 完成分析
├── 出现事件丢失(stderr出现：lost xx events on) → 调整-m参数增加ringbuffer，重新执行
├── 延迟分析需要深入 → 按6.1流程逐步添加--than和--detail参数
├── 需要更细粒度 → 调整选项参数/过滤器/阈值，重新执行
├── 输出量太大 → 立即结束命令，调整选项参数/增加过滤器/调大阈值，重新执行
├── 需要不同视角 → 返回第二步，选择其他分析器
|   └── 分析延迟根因 → multi-trace
└── 需要关联分析 → 使用multi-trace进行联合分析
```

## 事件选择语法

### 基本格式
```bash
EVENT,EVENT,...
EVENT: sys:name[/filter/ATTR/ATTR/.../]
      profiler[/option/ATTR/ATTR/.../]
      kprobe:func[/filter/ATTR/ATTR/.../]
      kretprobe:func[/filter/ATTR/ATTR/.../]
      uprobe:func@"file"[/filter/ATTR/ATTR/.../]
      uretprobe:func@"file"[/filter/ATTR/ATTR/.../]
filter: trace events filter
```

### 过滤器语法
- 数值比较：`==`, `!=`, `<`, `<=`, `>`, `>=`, `&`
- 字符串匹配：`==`, `!=`, `~`(通配符)
- 逻辑组合：`&&`, `||`, `()`

### 事件属性
- `stack` - 为指定的事件打开堆栈
- `max-stack=int` - 指定堆栈的深度
- `alias=str` - 事件别名，只用于`hrcount`, `hrtimer`, `multi-trace`, `num-dist`, `top`, `sql`
- `cpus=cpu[-cpu]` - 指定事件Attach到单独的cpu列表，与`-C, --cpus`指定的cpu列表取交集
- `top-by=EXPR` - 增加top显示字段，参与输出排序，只用于`top`
- `top-add=EXPR` - 增加top显示字段，不用于输出排序，只用于`top`
- `comm=EXPR` - 计算显示的进程名，只用于`top`
- `ptr=EXPR` - 计算内存分配返回的指针，用于内存分配事件，只用于`kmemleak`
- `size=EXPR` - 计算内存分配的字节，用于内存分配事件，只用于`kmemleak`, `kmemprof`
- `num=EXPR` - 计算分析数值分布的数值字段，只用于`num-dist`
- `key=EXPR` - 设置关联键字段，只用于`multi-trace`, `top`
- `printkey=EXPR` - 打印key值，使用'key'作为变量，只用于`multi-trace`, `top`（如：printkey=printf("%d",key)）
- `role=EXPR` - 计算事件的角色，延迟关系中作为起始事件还是结束事件，Bit0置位作为起始事件，Bit1置位作为结束事件，同时置位作为中间事件，只用于`multi-trace`
- `vm=uuid` - 指定事件来着某个虚拟机，通过uuid指定，只用于事件传播
- `push=` - 指定事件广播的位置，只用于事件传播。`[IP:]PORT`指定事件广播的服务端，`chardev`事件写到字符设备（如：`/push="/dev/virtio-ports/g.|qemu.perf0"/`），`file`事件写入文件
- `pull=` - 指定事件接受的位置，只用于事件传播。`[IP:]PORT`指定接收事件的服务端，`chardev`接收事件的字符设备，`file`接收事件的文件
- `index=field` - 指定 SQL的索引字段，只用于 `sql`
- `EXPR` - 计算表达式，使用事件的字段作为变量，在用户态计算。执行`perf-prof expr -h`获得帮助


## 严格约束

- 使用新的分析器时，必须先执行`perf-prof <profiler> -h`查看帮助
- 新增动态探针，必须阅读对应的文档，`kprobe_events.md`或`uprobe_events.md`

## 参考文档

详细的分析器文档在 `references/` 目录：
- 分析器使用指南：profile.md, top.md, task-state.md, multi-trace.md, hrcount.md, breakpoint.md, python.md等
- 过滤器语法：Event_filtering.md
- 表达式系统：expr.md
