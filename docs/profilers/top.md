# top - 键值统计

top分析器是一个多维度键值统计工具，通过采样事件，构建(key, [values], name)统计矩阵，并按指定列排序显示。

## 概述
- **主要用途**: 通过采样事件进行多维度统计，支持自定义键值聚合和排序，用于分析系统事件分布和热点
- **适用场景**: 各种计数统计场景，如：进程调度统计、中断频率分析、内存分配统计、IO性能分析、网络协议统计等
- **功能分类**: 自定义事件类，聚合分析，采样分析
- **最低内核版本**: 3.10 (支持perf_event)
- **依赖库**: libtraceevent (trace事件解析), libperf (性能事件)
- **平台支持**: x86, ARM
- **特殊限制**: 需要root权限或CAP_SYS_ADMIN权限

## 基础用法

```bash
perf-prof top [OPTION...] -e "event[/filter/key=EXPR/top-by=EXPR/top-add=EXPR/comm=EXPR/alias=STR/printkey=EXPR/][,event2...]" [-k EXPR] [--only-comm]
```

OPTION:
- `--watermark <0-100>`     未指定该选项则默认50
- `-i, --interval <ms>`     未指定该选项则默认1000ms
- `-m, --mmap-pages <N>`    未指定该选项则默认4页

PROFILER OPTION:
- `-e, --event <EVENT,...>` 事件选择器，多个事件使用','分隔，不支持多个-e选项
- `-k, --key <EXPR>`        设置键表达式，默认使用线程ID(tid)
- `--only-comm`             仅显示进程名列，隐藏键列

### 示例
```bash
# 基础用法 - 统计每个中断的次数
perf-prof top -e irq:irq_handler_entry//key=irq/

# 进程运行时间统计，按运行时间排序
perf-prof top -e 'sched:sched_stat_runtime//key=pid/comm=comm/top-by="runtime/1000"/alias=run(us)/' -i 1000

# 按进程名统计唤醒次数
perf-prof top -e sched:sched_wakeup//comm=comm/ --only-comm -m 64
```

## 核心原理

**数据模型**

事件 → (key, [value1, value2, ..., valuen], name) → 聚合 → 排序显示

### 事件源
- **sample_type**: `PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW`
- **自定义事件**:
  - `-e` 选项指定多个事件，','分隔
  - `/filter/`        过滤器
  - `/key=EXPR/`      指定键列
  - `/printkey=EXPR/` 定制键列输出格式
  - `/top-by=EXPR/`   增加值列，优先排序
  - `/top-add=EXPR/`  增加值列，默认排序
  - `/comm=EXPR/`     键的对应名称
  - `/alias=str/`     事件的别名

### 事件处理

**三大核心组件**

1. 键(key) - 键列

- 作用：行标识，确定数据聚合的行
- 提取规则：
  - 指定 key=EXPR 或 -k EXPR → 提取表达式值作为键（EXPR不能使用数组，只支持数值）
  - 未指定 → 默认使用线程ID(tid)
  - 不允许混合情况：部分事件指定key，部分未指定。要么全指定key，要么全部不指定
  - `(key, name)` 组合作为行的唯一标识，相同则聚合。目前只有2种组合：
    - `(0, name)`: 启用--only-comm，隐藏键列，name作为唯一标识，key强制为0，对name进行字符串匹配
    - `(key, NULL)`: 未启用--only-comm，key作为唯一标识，不使用name
- 显示标题：
  - 任意事件指定 key=EXPR 或 指定 -k EXPR → 表达式大写字母
  - 否则 → "PID"字符串

2. 值(values) - 值列

- 作用：每列的累计统计值
- 更新规则：
  - 指定 top-by=EXPR → 增加值列，提取表达式值并累计到值列（EXPR不能使用数组，只支持数值）
  - 指定 top-add=EXPR → 增加值列，提取表达式值并累计到值列（EXPR不能使用数组，只支持数值）
  - 未指定top-by/top-add → 增加一个值列，对事件计数(每次+1)
  - 一个事件可指定多个值列（top-by/top-add），按定义顺序增加
  - 只有值列参与排序，排序优先级：
    a. top-by列（按top-by定义顺序）
    b. top-add列 + 默认值列（同一优先级，按定义顺序）
- 显示标题：
  - 指定top-by=EXPR或top-add=EXPR → alias属性大写字母(仅首个top-by/top-add)或EXPR表达式大写字母
  - 未指定 → alias属性的大写字母或事件名大写字母
  - 显示顺序：按事件顺序，按事件内top-by/top-add定义顺序

3. 键名(name) - 名列

- 作用：键的可读名称，显示在行末
- 更新规则：
  - 获取name：
    - 指定 comm=EXPR → 提取comm表达式值，必须返回字符串(`char *`类型)
    - 未指定 comm → common_pid获取进程名(global_comm_get)
  - 启用 --only-comm 以`(0, name)`为键在统计矩阵查找或新建行，新建时即更新name
  - 未启用 --only-comm 以`(key, NULL)`为键在统计矩阵查找或新建行，对应行的name为空则更新
    - 所有事件有key属性 + 任意事件有comm属性 → 只从comm属性的事件提取并更新对应行的name
    - 所有事件有key属性 + 所有事件无comm属性 → 不显示键名，不需要更新name
    - 所有事件无key属性 + 所有事件无comm属性 → 更新对应行的name（进程名）
    - 所有事件无key属性 + 任意事件有comm属性 → 更新对应行的name（comm=EXPR或进程名）
- 显示标题：
  - 所有事件有key属性 + 任意事件有comm属性 → 显示键名，显示标题：首个comm属性大写字母
  - 所有事件有key属性 + 所有事件无comm属性 → 不显示键名，隐藏名列
  - 所有事件无key属性 + 所有事件无comm属性 → 显示键名，显示标题："COMM"字符串（与键列显示标题 "PID"字符串相匹配）
  - 所有事件无key属性 + 任意事件有comm属性 → 显示键名，显示标题：首个comm属性大写字母
- --only-comm限制：
  - 必须要显示键名，否则报错


#### 统计与聚合

对每个采样的事件：
- 提取事件对应的 `key`, `values`, `name`，每个事件的values只是一个子集（如：[value1, value2]）
- 按(key, name)为索引在统计矩阵内查找对应的行（未找到则新建一行）
- 把`values`累加到对应的`[value1, value2, ..., valuen]`内

随着对大量事件的采样，不断更新values，最终每个值列都累积有值。

#### 排序显示

对统计矩阵(key, [value1, value2, ..., valuen], name)内的每一行，按照值列进行排序，输出排序后的每一行
排序优先级：
- top-by列（按top-by定义顺序）
- top-add列 + 默认值列（同一优先级，按定义顺序）

#### 演示示例

`sys:A//key=fieldA/top-by=value1/top-add=value2/,sys:B//key=fieldB/,sys:C//key=fieldC/top-by=value3/comm=comm1/,...`

统计矩阵: (key, [value1, value2, B, value3, ...], name)

1. 事件采样

- 事件A：
  - 提取：key=fieldA、values=[value1,value2]，name=进程名
  - 更新：查找(fieldA,name)（未找到则新建一行），累加[value1,value2]
- 事件B：
  - 提取：key=fieldB，values=[B]（事件自身），name=进程名
  - 更新：查找(fieldB,name)（未找到则新建一行），累加[B]
- 事件C：
  - 提取：key=fieldC、values=[value3]、name=comm1
  - 更新：查找(fieldC,name)（未找到则新建一行），累加[value3]、更新name=comm1
- ...

当一个key值对应的事件A、事件B、事件C都发生过时，([value1, value2, B, value3, ...], name) 才会都有值。

2. 排序显示

演示示例按`value1`,`value3`,`value2`,`B`顺序进行排序

**显示标题**
|键列|值列(优先排序)|值列|值列|值列(优先排序)|...|键名|
|---|---|---|---|---|---|---|
| FIELDA | VALUE1 | VALUE2 | B | VALUE3 | ... | COMM1 |

### 过滤器支持

- 内核态trace event过滤器
- 用户态ftrace过滤器，当内核态过滤器失败时，会自动降级到用户态执行过滤表达式。支持扩展的表达式语法，包括：
  - 使用 `_cpu` 和 `_pid` 全局变量
  - 调用 `comm_get()` 获取进程名
  - 使用 `~` 操作符进行通配符匹配

### 状态统计
- **信号处理**:
  - SIGWINCH: 随终端窗口尺寸变化调整显示，自动切换全屏/非全屏模式

## 输出

### 输出格式

#### 标题行格式
```
perf-prof - HH:MM:SS  sample N events
键显示标题 值列1标题 值列2标题 ... 键名标题
```

#### 数据行格式
```
key值 value1累计值 value2累计值 ... name值
```

### 输出配置

#### 1. 键列显示
- **默认显示**: 显示key的数值
- **printkey定制**: 使用 `printkey=EXPR` 属性定制输出格式
- **隐藏键列**: 使用 `--only-comm` 选项

**printkey示例**:
```bash
# 显示复合键的多个维度
perf-prof top -e 'irq:softirq_entry//key=(_cpu<<32)|vec/printkey=printf("  %03d      %d",key>>32,(int)key)/'
```

#### 2. 键名列显示
- 有key属性但无comm属性，不显示该列
- **进程名显示**: 无key属性，自动显示对应的进程名
- **comm属性**: 使用事件字段或表达式计算键名
  - **ksymbol函数**: 将函数地址转换为符号名
  - **comm_get函数**: 获取进程名

#### 3. 值列显示
- 每列显示累计统计值
- 按排序优先级组织显示顺序

### 决策指南

**选择printkey还是键名列？**

1. **使用printkey的场景**:
   - key是复合键（如：`_cpu*1000+vector`）
   - 需要分解多个维度显示
   - 需要自定义格式化输出

2. **使用键名列的场景**:
   - key是单维度（如：PID、IRQ号）
   - 需要显示key的可读名称
   - key有明确的含义映射（如：PID→进程名）

## 分析方法

### 基础分析方法
1. 根据分析任务，选定待分析的事件
2. 选定每个事件的键、值、键名
   - 选定键：多个事件时，每个键要有相同的含义，键名要跟键的含义匹配
   - 选定值：top-by/top-add会对选定的字段值做累加；如果仅计数事件次数，不指定top-by/top-add属性。
   - 键名：所有事件不指定key属性，也不指定comm属性，默认就会显示进程名，键是"PID"
3. 可选：设定每个事件的过滤器
4. 跟踪、分析输出结果

### 数据驱动分析
- 通过采集数据来确定下一步动作
  - 调整过滤器
  - 增加新事件
  - 选择其他分析器进一步分析

## 真实案例

目标：分析每个进程的切换次数，以及运行时间，按运行时间排序显示。

### 选定待分析的事件

- 选择`sched:sched_switch`事件，用于累计进程切换次数
- 选择`sched:sched_stat_runtime`事件，累计进程运行时间

分析事件的各个字段含义。可以利用末尾的`help`来查看事件的字段。

```bash
perf-prof top -e sched:sched_switch,sched:sched_stat_runtime help
```

- `sched:sched_switch`事件:
  - prev_pid：换出进程(prev进程)的pid
  - prev_comm：换出进程的进程名
  - next_pid：换入进程(next进程)的pid
  - next_comm：换入进程的进程名
- `sched:sched_stat_runtime`事件：
  - pid：进程pid
  - comm: 进程名
  - runtime：进程的运行时间

### 选定键

选定2个事件都具有pid含义的字段作为key。

- `sched:sched_switch`事件：key=prev_pid
- `sched:sched_stat_runtime`事件：key=pid

### 选定值

- `sched:sched_switch`事件：值选择默认值，对事件自身计数
- `sched:sched_stat_runtime`事件：值选择runtime，以累计进程运行时间；同时按运行时间排序，使用`top-by=runtime`

### 选定键名

由于键值表示进程id，且2个事件内无论哪个事件，都能把进程id跟进程名关联起来。
键名使用comm属性，加到2个事件内的任一事件。

### 最终命令

```bash
perf-prof top -e sched:sched_switch//key=prev_pid/comm=prev_comm/,sched:sched_stat_runtime//key=pid/top-by=runtime/ -i 1000

# 输出
2025-10-29 19:50:25.067825 perf-prof - 19:50:25  sample 9022 events
PREV_PID SCHED_SWITCH      RUNTIME PREV_COMM
    2831          933     12801931 sap1008
   32729          102      6203671 barad_agent
    6476          189      4759857 main
```


## 更多示例

```bash
# 统计每个中断的频率，显示中断名
perf-prof top -e irq:irq_handler_entry//key=irq/comm=name/

# 统计每个pid的sched_wakeup次数
perf-prof top -e sched:sched_wakeup//key=pid/

# 默认key：线程pid。按runtime来计数，不断累加runtime字段的值。
perf-prof top -e sched:sched_stat_runtime//top-by=runtime/

# 按comm统计进程唤醒次数
perf-prof top -e sched:sched_wakeup//comm=comm/ --only-comm -m 64

# 按comm统计进程IO
perf-prof top -e block:block_rq_issue//top-by=nr_sector/comm=comm/ --only-comm -m 32

# 过滤写IO，且是小IO
perf-prof top -e 'block:block_rq_issue/rwbs=="W"&&nr_sector<4/top-by=nr_sector/comm=comm/' --only-comm -i 1000

# 按退出原因统计虚拟化退出次数
perf-prof top -e kvm:kvm_exit//key=exit_reason/ -i 1000

# 统计进程的执行时间和进程切换次数
perf-prof top -e sched:sched_stat_runtime//key=pid/top-by=runtime/,sched:sched_switch//key=prev_pid/comm=prev_comm/

# 按进程名统计进程的执行时间和进程切换次数
perf-prof top -e sched:sched_stat_runtime//key=pid/comm=comm/top-by=runtime/,sched:sched_switch//key=prev_pid/comm=prev_comm/ --only-comm

# 统计可执行程序的执行次数
perf-prof top -e 'sched:sched_process_exec//key=pid/alias=num/comm=filename/' --only-comm

# 按网络协议统计网络丢包的函数
perf-prof top -e 'skb:kfree_skb//key=protocol/comm=ksymbol(location)/' -m 32

# 使用printkey显示复合键
perf-prof top -e 'kvm:kvm_msi_set_irq//key=(target_cpu*1000+vector)/printkey=printf("cpu%d->vec%d",key/1000,key%1000)/'

# 使用用户态过滤器过滤Python进程
perf-prof top -e 'sched:sched_wakeup/comm_get(_pid) ~ "python*"/key=pid/' -i 1000

# 过滤特定CPU上的调度事件
perf-prof top -e 'sched:sched_switch/_cpu<4/key=prev_pid/comm=prev_comm/' -i 1000
```
