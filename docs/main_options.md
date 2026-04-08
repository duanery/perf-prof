# perf-prof 选项参数

perf-prof 工具提供了丰富的选项参数，用于控制分析器的行为和输出格式。

该文档提供所有的选项参数定义，一个分析单元只支持其中的一部分，查看帮助`-h`列出具体支持的选项

## 选项参数定义规范

```bash
- `-s, --long <value>` 描述 # 短选项(-开头，可能为空), 长选项(--开头，必须存在), 值定义（boolean类型为空），描述（用途和含义）
  - 变量名: `struct env`结构体内的变量名，未说明则无对应的变量名
    # 类型和约束
  - 类型: 参数的数据类型：string, integer(整形, 允许16进制), boolean
  - 必选: 是否为必选参数 true/false, 未说明默认为false
  - 默认值: 参数默认值, 未说明默认为0（integer, boolean）、空（string）
  - 可选值: [可选值列表]
  - 最小值: 最小值
  - 最大值: 最大值
  - 单位: 参数的单位, 未说明默认无单位
  - no前缀: 允许"no-"前缀反选 true/false, 未说明默认为false
  - 值模式: 使用正则表达式描述值（string类型）
  - 可重复：允许多次指定的选项 true/false
    # 语义信息
  - 值描述: 值的描述
  - 示例: ["示例值1", "示例值2"]
  - 状态: 参数的当前状态，废弃、试验性、不常用、用途等
  - 附加:
    # 关系信息
  - 互斥参数: [互斥参数列表]
  - 依赖参数: [依赖参数列表]
```

## 1 OPTION 公共选项

### 1.1 CPU和进程附加选项

- `-C, --cpus <cpu[-cpu],...>`    Attach到CPU列表，如果未指定`-p`、`-t`、`--cgroups`，则默认Attach到系统所有的CPU
  - **变量名**: `cpumask`
  - **类型**: string
  - **默认值**: 系统所有的CPU
  - **值描述**: 支持格式：单个数字、连续范围、不连续列表、混合格式
  - **值模式**: `^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$`
  - **示例**: ["5", "1-5", "1-5,7,9"]
  - **互斥参数**: ["-p", "-t", "--cgroups"]

- `-p, --pids <pid,...>`          Attach到PID列表，跟踪指定进程的所有线程
  - **变量名**: `pids`
  - **类型**: string
  - **值描述**: 支持格式：单个pid，多个pid（逗号分隔）
  - **值模式**: `^[0-9]+(,[0-9]+)*$`
  - **示例**: ["345", "578,3489"]
  - **互斥参数**: ["-C", "-t", "--cgroups"]

- `-t, --tids <tid,...>`          Attach到TID列表，跟踪指定进程的部分线程
  - **变量名**: `pids`
  - **类型**: string
  - **值描述**: 支持格式：单个tid，多个tid（逗号分隔）
  - **值模式**: `^[0-9]+(,[0-9]+)*$`
  - **示例**: ["345", "578,3489"]
  - **互斥参数**: ["-C", "-p", "--cgroups"]

- `--cgroups <cgroup,...>`        Attach到cgroup，先把进程加到 `perf_event` cgroup 内，再使用该选项
  - **变量名**: `cgroups`
  - **类型**: string
  - **值描述**: 支持格式：单个cgroup，多个cgroup（逗号分隔）
  - **值模式**: `^[a-zA-Z0-9]+(,[a-zA-Z0-9]+)*$`
  - **示例**: ["test", "test,test1"]
  - **互斥参数**: ["-C", "-p", "-t"]

### 1.2 性能和缓冲区选项

- `--watermark <0-100>`           配置perf_event ringbuffer的水位，内核写入到达水位时唤醒perf-prof，水位越低唤醒越频繁
  - **变量名**: `watermark`, `watermark_set`
  - **类型**: integer
  - **默认值**: 每个profiler不一样，参与源码及分析器文档 ([profilers/](profilers/))
  - **最小值**: 0
  - **最大值**: 100
  - **值描述**: ringbuffer的大小由`-m`指定，watermark表示ringbuffer的百分比（如：-m 32, --watermark 50，则内核写入16个页的数据则唤醒perf-prof）

- `-i, --interval <ms>`           输出间隔，ms单位
  - **变量名**: `interval`
  - **类型**: integer
  - **单位**: ms(毫秒)
  - **默认值**: 每个profiler不一样，参与源码及分析器文档 ([profilers/](profilers/))

- `-m, --mmap-pages <pages>`      设置ringbuffer的大小，页数，一个页4k字节
  - **变量名**: `mmap_pages`
  - **类型**: integer
  - **默认值**: 每个profiler不一样的，参考源码及分析器文档 ([profilers/](profilers/))
  - **值描述**: 2的幂次方
  - **示例**: ["1", "8", "32", "256"]

### 1.3 输出和控制选项

- `-N, --exit-N <N>`              采样N个事件后退出，用于限定采样量
  - **变量名**: `exit_n`
  - **类型**: integer

- `-o, --output <file>`           输出到文件，stdout、stderr都会被重定向到该文件
  - **变量名**: `output`
  - **类型**: string

- `--order`                       启用事件排序，在严格保证事件顺序时启用。默认对`blktrace`、`kvmmmu`、`task-state`是启用排序的
  - **变量名**: `order`
  - **类型**: boolean
  - **默认值**: 对于`blktrace`、`kvmmmu`、`task-state`为true，其他为false

### 1.4 时间戳转换选项

- `--tsc`                         把perf时间戳转换为tsc时间戳
  - **变量名**: `tsc`
  - **类型**: boolean
  - **互斥参数**: ["--kvmclock"]

- `--kvmclock <uuid>`             把perf时间戳转换为虚拟机的时间戳，由uuid指定虚拟机，`virsh list`列出所有的虚拟机
  - **变量名**: `kvmclock`
  - **类型**: string
  - **值描述**: 标准UUID格式，32位十六进制数字，以连字符分隔：8-4-4-4-12
  - **值模式**: `^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$`
  - **示例**: ["123e4567-e89b-12d3-a456-426614174000"]
  - **互斥参数**: ["--tsc"]

- `--clock-offset <n>`            转换后的时钟加上偏移量
  - **变量名**: `clock_offset`
  - **类型**: integer
  - **依赖参数**: ["--tsc", "--kvmclock"]

- `--monotonic`                   使用CLOCK_MONOTONIC作为perf时间戳
  - **变量名**: `monotonic`
  - **类型**: boolean

### 1.5 监控和调试选项

- `--usage-self <ms>`             周期性输出perf-prof自身的cpu利用率，ms单位
  - **变量名**: `usage_self`
  - **类型**: integer
  - **单位**: ms(毫秒)

- `-v, --verbose`                 更原始输出，-v输出profiler内部调试信息，-vv 输出原始事件，-vvv 全部输出
  - **变量名**: `verbose`
  - **类型**: boolean
  - **可重复**: true
  - **值描述**:
    - `-v`: 输出profiler内部调试信息
    - `-vv`: 输出原始事件（输出量极大，谨慎使用）
    - `-vvv`: 全部输出
  - **互斥参数**: ["-q"]

- `-q, --quiet`                   更安静，减少输出
  - **变量名**: `verbose`
  - **类型**: boolean
  - **可重复**: true
  - **互斥参数**: ["-v"]

### 1.6 版本和帮助选项

- `-V, --version`                 显示版本信息
  - **类型**: 无返回值

- `-h, --help`                    输出帮助信息
  - **类型**: 无返回值

## 2 FILTER OPTION 过滤器选项

### 2.1 PMU过滤器选项

- `-G, --exclude-host`            pmu过滤器，过滤掉host，只监控guest
  - **变量名**: `exclude_host`
  - **类型**: boolean

- `--exclude-guest`               pmu过滤器，过滤掉guest，只监控host
  - **变量名**: `exclude_guest`
  - **类型**: boolean

- `--exclude-user`                pmu过滤器，过滤掉用户态事件
  - **变量名**: `exclude_user`
  - **类型**: boolean

- `--exclude-kernel`              pmu过滤器，过滤掉内核态事件
  - **变量名**: `exclude_kernel`
  - **类型**: boolean

### 2.2 eBPF过滤器选项

- `--irqs_disabled[=<0|1>]`       ebpf过滤器，根据irq是否关闭筛选事件，用于采样关中断的代码段
  - **变量名**: `irqs_disabled`
  - **类型**: boolean
  - **默认值**: 1（当不指定值时）
  - **可选值**: [0, 1]
  - **值描述**:
    - `--irqs_disabled` 或 `--irqs_disabled=1`: 中断关闭继续采样，中断打开终止采样
    - `--irqs_disabled=0`: 中断打开继续采样，中断关闭终止采样

- `--tif_need_resched[=<0|1>]`    ebpf过滤器，根据进程`TIF_NEED_RESCHED`标记是否设置筛选事件，用于采样进程延迟调度（需要调度但未走到调度点）的代码段
  - **变量名**: `tif_need_resched`
  - **类型**: boolean
  - **默认值**: 1（当不指定值时）
  - **可选值**: [0, 1]
  - **值描述**:
    - `--tif_need_resched` 或 `--tif_need_resched=1`: 标记设置继续采样，标记未设置终止采样
    - `--tif_need_resched=0`: 标记未设置继续采样，标记设置终止采样

- `--exclude_pid <pid>`           ebpf过滤器，过滤掉pid的事件
  - **变量名**: `exclude_pid`
  - **类型**: integer
  - **值描述**: 当前进程等于PID终止采样，否则继续采样

- `--nr_running_min <n>`          ebpf过滤器，cpu runqueue长度大于n则选中事件，用于采样运行队列拥堵的问题
  - **变量名**: `nr_running_min`
  - **类型**: integer
  - **值描述**: 满足nr_running_min <= nr_running条件继续采样

- `--nr_running_max <n>`          ebpf过滤器，cpu runqueue长度小于n则选中事件，用于采样运行队列拥堵的问题
  - **变量名**: `nr_running_max`
  - **类型**: integer
  - **值描述**: 满足nr_running <= nr_running_max条件继续采样

- `--sched_policy <n>`            ebpf过滤器，根据进程的调度策略筛选事件，采样指定调度策略的事件
  - **变量名**: `sched_policy`
  - **类型**: integer
  - **可选值**: [0, 1, 2, 3, 5, 6]
  - **值描述**:
    - 0: NORMAL
    - 1: FIFO
    - 2: RR
    - 3: BATCH
    - 5: IDLE
    - 6: DEADLINE
  - **示例**: `--sched_policy 2` 采样RR实时进程的事件

### 2.3 堆栈开关选项

- `--user-callchain[=dwarf[,size]]`  堆栈开关，选中用户态堆栈，`no-`前缀反选（如：`--no-user-callchain`）
  - **变量名**: `user_callchain`
  - **类型**: boolean 或 string
  - **默认值**: 根据分析器而不同
  - **no前缀**: true（支持`--no-user-callchain`）
  - **依赖参数**: ["-g"] 或 `/stack/`属性
  - **dwarf模式**: `--user-callchain=dwarf` 启用基于DWARF的用户态栈回溯，使用libunwind解析.eh_frame
    unwind表，比帧指针（FP）方式更可靠。`--user-callchain=dwarf,8192` 可指定采集的栈数据大小（字节），
    默认为内核perf_event_attr的默认值。内核采样用户寄存器（PERF_SAMPLE_REGS_USER）和栈快照
    （PERF_SAMPLE_STACK_USER），用户态通过libunwind远程解析栈帧
  - **示例**:
    - `--user-callchain` — FP模式，使用帧指针回溯用户态堆栈
    - `--user-callchain=dwarf` — DWARF模式，使用.eh_frame unwind表回溯
    - `--user-callchain=dwarf,8192` — DWARF模式，采集8192字节栈数据

- `--kernel-callchain`            堆栈开关，选中内核态堆栈，`no-`前缀反选（如：`--no-kernel-callchain`）
  - **变量名**: `kernel_callchain`
  - **类型**: boolean
  - **默认值**: 根据分析器而不同
  - **no前缀**: true（支持`--no-kernel-callchain`）
  - **依赖参数**: ["-g"] 或 `/stack/`属性

- `--python-callchain`            堆栈开关，选中python堆栈
  - **变量名**: `python_callchain`
  - **类型**: boolean
  - **依赖参数**: ["-g"]

### 2.4 特殊过滤器选项

- `--prio <prio[-prio],...>`      指定进程的优先级列表，对`profile`是个ebpf过滤器
  - **变量名**: `prio`
  - **类型**: string
  - **值描述**: 支持格式：单个数字、连续范围、不连续列表、混合格式
  - **值模式**: `^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$`
  - **示例**: `--prio 1-99` 实时进程优先级

## 3 PROFILER OPTION 分析单元选项

- `-e, --event <EVENT,...>`      指定事件，参考"perf-prof 事件格式"，可以是kprobe事件、uprobe事件
  - **变量名**: `events`
  - **类型**: string
  - **可重复**: 对于`multi-trace`、`syscalls`、`kmemprof`、`nested-trace`、`rundelay`为true，其他为false
  - **值描述**: 支持多个事件，逗号分隔，参考"perf-prof 事件格式"
  - **示例**: ["sched:sched_wakeup", "kprobe:try_to_wake_up", "sched:sched_wakeup,sched:sched_switch"]

- `-F, --freq <n>`               指定采样频率
  - **变量名**: `freq`
  - **类型**: integer
  - **最小值**: 1
  - **单位**: Hz

- `-k, --key <str>`              指定键字段，同`key=EXPR`属性
  - **变量名**: `key`
  - **类型**: string
  - **值描述**: 支持C表达式，用于事件关联

- `--filter <filter>`            指定事件的过滤器，用于内建事件的profiler
  - **变量名**: `filter`
  - **类型**: string
  - **值描述**: 用于内建事件类分析器，参考每个分析器的描述

- `--period <ns>`                指定采样周期，单位：s/ms/us/ns，不指定默认是ns
  - **变量名**: `period`
  - **类型**: string
  - **值描述**: 不支持浮点数，支持时间单位：s、ms、us、ns，无单位默认ns
  - **值模式**: `^[0-9]+(s|ms|us|ns)?$`
  - **示例**: ["10ms", "1000000", "1s", "500us"]

- `--impl <impl>`                指定two-event分析类型。不指定默认是 delay
  - **变量名**: `impl`
  - **类型**: string
  - **默认值**: delay
  - **可选值**: ["delay", "pair", "kmemprof", "syscalls", "call", "call-delay"]
  - **值描述**:
    - `delay`: 延迟分析（默认）
    - `pair`: 事件配对分析
    - `kmemprof`: 内存分配释放分析
    - `syscalls`: 系统调用延迟分析
    - `call`: 函数调用分析（仅nested-trace）
    - `call-delay`: 调用+延迟分析（仅nested-trace）

- `-S, --interruptible`          选择S状态进程，用于`task-state`选择进程状态，`no-`前缀反选（如：`--no-interruptible`）
  - **变量名**: `interruptible`
  - **类型**: boolean
  - **no前缀**: true（支持`--no-interruptible`）

- `-D, --uninterruptible`        选择D状态进程，用于`task-state`选择进程状态
  - **变量名**: `uninterruptible`
  - **类型**: boolean

- `--than <n>`                   超过指定的阈值输出，单位：s/ms/us/ns，不指定默认是ns
  - **变量名**: `than`
  - **类型**: string
  - **值描述**: 不支持浮点数，支持时间单位：s、ms、us、ns，无单位默认ns
  - **值模式**: `^[0-9]+(s|ms|us|ns)?$`
  - **示例**: ["10ms", "5000000", "1s"]

- `--only-than <ns>`             只有在超过指定的阈值才输出，单位：s/ms/us/ns，不指定默认是ns
  - **变量名**: `only_than`
  - **类型**: string
  - **值描述**: 不支持浮点数，支持时间单位：s、ms、us、ns，无单位默认ns
  - **值模式**: `^[0-9]+(s|ms|us|ns)?$`

- `--lower <ns>`                 低于指定的阈值输出，单位：s/ms/us/ns，不指定默认是ns（如：--lower 1ms）
  - **变量名**: `lower`
  - **类型**: string
  - **值描述**: 不支持浮点数，支持时间单位：s、ms、us、ns，无单位默认ns
  - **值模式**: `^[0-9]+(s|ms|us|ns)?$`

- `--alloc <EVENT>`              指定内存分配事件，可以是用户态内存分配器
  - **变量名**: `alloc_event`
  - **类型**: string
  - **值描述**: 支持多个事件，逗号分隔，参考"perf-prof 事件格式"

- `--free <EVENT>`               指定内存释放事件，可以是用户态内存分配器
  - **变量名**: `free_event`
  - **类型**: string
  - **值描述**: 支持多个事件，逗号分隔，参考"perf-prof 事件格式"

- `--syscalls`                   跟踪系统调用次数。用于 `percpu-stat`
  - **变量名**: `syscalls`
  - **类型**: boolean

- `--perins`                     输出每个实例的统计，如：Attach到CPU则实例是CPU，Attach到PID则实例是线程
  - **变量名**: `perins`
  - **类型**: boolean

- `-g, --call-graph`             采样堆栈
  - **变量名**: `call_graph`
  - **类型**: boolean

- `--flame-graph <file>`         堆栈输出为折叠的火焰图文件，输出 `file.folded` 文件，使用 `flamegraph.pl file.folded > file.svg` 输出火焰图
  - **变量名**: `flame_graph`
  - **类型**: string
  - **值描述**: 文件名，不加".folded"后缀

- `--heatmap <file>`            指定输出延迟热图文件，输出 `file.lat` 文件
  - **变量名**: `heatmap`
  - **类型**: string
  - **值描述**: 文件名，不加".lat"后缀

- `--detail[=<-N,+N,1,2,hide<N,same*>]`  输出详细信息。对于`multi-trace`有特定用法
  - **变量名**: `detail`
  - **类型**: string
  - **值描述**:
    - `-N`: 在event1之前，打印N纳秒内的事件，N支持时间单位：s、ms、us、ns，无单位默认ns
    - `+N`: 在event2之后，打印N纳秒内的事件，N支持时间单位：s、ms、us、ns，无单位默认ns
    - `1`: 只显示与event1相同的事件
    - `2`: 只显示与event2相同的事件
    - `hide<N`: 隐藏小于N纳秒的事件间隔，N支持时间单位：s、ms、us、ns，无单位默认ns
    - `samecpu`: 只显示与event1或event2相同CPU的事件
    - `samepid`: 只显示与event1或event2相同PID的事件
    - `sametid`: 只显示与event1或event2相同TID的事件
    - `samekey`: 只显示与event1或event2相同key的事件
  - **示例**: ["--detail=-1ms", "--detail=+500us", "--detail=1", "--detail=hide<100ns", "--detail=samecpu,samepid"]
 - **依赖参数**: ["--than", "--lower"]

- `-T, --triger <n>`             触发频率，每n个事件触发一次采样
  - **变量名**: `trigger`
  - **类型**: integer

- `--test`                       触发总线索，用于自测，用于`split-lock`
  - **变量名**: `test`
  - **类型**: boolean
  - **状态**: 测试用途，禁止用于生产环境

- `--symbols`                    映射地址为对应的符号，或者找符号在二进制内的文件偏移
  - **变量名**: `symbols`
  - **类型**: boolean

- `--device <device>`            指定块设备
  - **变量名**: `device`
  - **类型**: string
  - **值描述**: 块设备路径
  - **示例**: ["/dev/sda", "/dev/nvme0n1"]

- `--ldlat <cycles>`             指定内存加载的延迟阈值，MSR_PEBS_LD_LAT_THRESHOLD MSR寄存器的值
  - **变量名**: `ldlat`
  - **类型**: integer
  - **单位**: CPU cycles

- `--overwrite`                  使用覆盖模式，用于`trace`
  - **变量名**: `overwrite`
  - **类型**: boolean
  - **状态**: 实验性质

- `--spte`                       启用kvm_mmu_set_spte事件，用于`kvmmmu`
  - **变量名**: `spte`
  - **类型**: boolean

- `--mmio`                       启用mark_mmio_spte事件，用于`kvmmmu`
  - **变量名**: `mmio`
  - **类型**: boolean

- `--only-comm`                  只显示comm，不显示pid，用于`top`
  - **变量名**: `only_comm`
  - **类型**: boolean

- `--cycle`                      环形事件，统计最后一个事件到首个事件的延迟，用于`multi-trace`
  - **变量名**: `cycle`
  - **类型**: boolean

- `--ptrace`                     启用ptrace控制并跟踪新建的线程
  - **变量名**: `ptrace`
  - **类型**: boolean

- `--string`                     输出字符串值，用于`kcore`
  - **变量名**: `string_output`
  - **类型**: boolean

- `-1, -2, -4, -8`              输出8位、16位、32位、64位的值，用于`kcore`
  - **变量名**: `bits`
  - **类型**: boolean
  - **可选值**: [1, 2, 4, 8]

- `--query <SQL>`                执行SQL查询语句，用于`sql`分析器
  - **变量名**: `query`
  - **类型**: string
  - **可重复**: false
  - **值描述**: SQL查询语句，支持标准SQL语法（SELECT, WHERE, GROUP BY, ORDER BY, JOIN等）
  - **示例**:
    - 单条查询: `--query 'SELECT comm, COUNT(*) FROM sched_wakeup GROUP BY comm'`
    - 多条查询: `--query 'SELECT COUNT(*) FROM sched_wakeup; SELECT AVG(prio) FROM sched_wakeup'`
  - **附加**:
    - 多条SQL语句用分号(`;`)分隔
    - 查询将在周期间隔(`-i`)或程序退出时执行
    - 查询完成后会清空表数据（内存模式）或重建表（文件模式）
  - **依赖参数**: 需要配合`-e`指定事件源

- `--output2 <file>`             附加输出文件，不同分析器有不同用途
  - **变量名**: `output2`
  - **类型**: string
  - **值描述**: 文件路径
  - **按分析器用途**:
    - `sql`: 指定SQLite数据库文件路径，未指定时使用内存数据库(`:memory:`)
    - 其他分析器: 根据具体分析器定义
  - **示例**: ["events.db", "output.sqlite", "/tmp/perf_data.db"]

- `--prio <prio[-prio],...>`     指定进程的优先级列表
  - **变量名**: `prio`
  - **类型**: string
  - **值描述**: 进程优先级列表，类似CPU列表，支持格式：单个数字、连续范围、不连续列表、混合格式
  - **值模式**: `^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$`
  - **示例**: `--prio 1-99` 对应实时进程

- `--inherit`                    子任务继承计数器，用于`trace`
  - **变量名**: `inherit`
  - **类型**: boolean
  - **状态**: 实验性质
