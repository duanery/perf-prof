---
layout: default
title: "multi-trace - 多事件分析"
---

# multi-trace - 多事件关系分析

multi-trace是一个基于key关联的多事件关系分析工具，通过时间排序和状态管理实现复杂事件链的延迟分析和配对跟踪。

## 概述
- **主要用途**: 将复杂的多事件关系转换为两两事件关系进行分析，支持延迟分析、事件配对、内存分配跟踪、系统调用延迟等多种分析模式
- **适用场景**: 进程调度延迟分析、软中断处理延迟、内存分配生命周期跟踪、系统调用性能分析、复杂事件因果关系链分析
- **功能分类**: 自定义事件类，延迟分析，联合分析，状态机驱动的事件关联
- **最低内核版本**: 3.10+ (支持trace events)
- **依赖库**: libtraceevent, libperf
- **平台支持**: x86, ARM, RISC-V, PowerPC
- **特殊限制**: 需要root权限运行，支持guest环境，依赖事件时间戳排序
- **参与联合分析**: 作为联合分析的主体，可以组合多个分析单元作为事件源
- **核心技术**: Timeline红黑树事件排序、Backup红黑树状态管理、Key表达式事件关联

## 基础用法
perf-prof multi-trace [OPTION...] -e EVENT [-e ...] [-k EXPR] [--impl impl] [--than|--only-than ns] [--detail] [--perins] [--heatmap file] [--cycle]

事件格式: "event[/filter/key=EXPR/role=EXPR/untraced/trigger/alias=str/stack/max-stack=int/][,event...]"

OPTION:
- `--watermark <0-100>`     未指定该选项：默认50
- `-m, --mmap-pages <N>`    未指定该选项：默认64页
- `--order`                 未指定该选项：可根据场景动态决定是否启用（详见性能优化章节）

PROFILER OPTION:
- `-e, --event <EVENT,...>`   指定事件，支持多种用法：
  - 单个事件：`-e sched:sched_wakeup`
  - 多个事件（同位置）：`-e 'sched:sched_wakeup,sched:sched_wakeup_new'`
  - 多位置事件：`-e event1 -e event2`（构建分析路径）
  - 复杂事件：`-e 'event/filter/key=EXPR/role=EXPR/stack/'`
- `-k, --key <str>`           事件关联键，给未使用`key=EXPR`属性的事件提供默认key
- `--impl <impl>`             分析实现类型
    - `delay`: 延迟分析（默认）
    - `pair`: 事件配对分析
    - `kmemprof`: 内存分配分析
    - `syscalls`: 系统调用延迟分析
    - `call`: 函数调用分析（仅nested-trace）
    - `call-delay`: 调用+延迟分析（仅nested-trace）
- `--than <n>`                超过指定的阈值输出，单位：s/ms/us/ns，不指定默认是ns
- `--only-than <ns>`          只有在超过指定的阈值才输出，单位：s/ms/us/ns，不指定默认是ns
- `--lower <ns>`              低于指定的阈值输出，单位：s/ms/us/ns，不指定默认是ns（如：--lower 1ms）
- `--perins`                  每个实例统计
- `--heatmap <file>`          指定输出延迟热图文件，file会自动加".lat"后缀
- `--detail[=<-N,+N,1,2,hide<N,same*>]`: 详细信息输出，依赖`--than`、`--lower`选项
    - `-N`: 在event1之前，打印N纳秒内的事件，N支持时间单位：s、ms、us、ns，无单位默认ns
    - `+N`: 在event2之后，打印N纳秒内的事件，N支持时间单位：s、ms、us、ns，无单位默认ns
    - `1`: 只显示与event1相同的事件
    - `2`: 只显示与event2相同的事件
    - `hide<N`: 隐藏小于N纳秒的事件间隔，N支持时间单位：s、ms、us、ns，无单位默认ns
    - `samecpu`: 只显示与event1或event2相同CPU的事件
    - `samepid`: 只显示与event1或event2相同PID的事件
    - `sametid`: 只显示与event1或event2相同TID的事件
    - `samekey`: 只显示与event1或event2相同key的事件
- `--cycle`                   环形事件模式，建立从最后一个`-e`回到第一个`-e`的关系（详见三种核心关系）

### 示例
```bash
# 软中断处理延迟分析
perf-prof multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ -i 1000 --than 100us

# 事件配对分析
perf-prof multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ -i 1000 --impl pair
```

## 核心原理

multi-trace将复杂的多事件关系转换为高效的两两事件关系进行分析，通过timeline红黑树和backup红黑树实现事件配对和状态管理。

**数据模型**

事件 → [排序] → 事件关联 → 两事件关系处理 → 统计输出

### 事件关系

#### 1. 事件关系转换机制

multi-trace的核心思想是将**多事件关系**转换为**两事件关系**来分析：

**转换示例**：
```
输入命令: perf-prof multi-trace -e A,B,C -e D,E -e F
多事件关系: A,B,C → D,E → F
转换为两事件关系:
- A→D, A→E, B→D, B→E, C→D, C→E  (起点到中间)
- D→F, E→F                      (中间到终点)
- F→A, F→B, F→C                 (终点到起点, 依赖`--cycle`选项)
```

**事件位置定义**：
- **起点事件(A,B,C)**: 3种可能性，只需备份等待后续事件
- **中间事件(D,E)**: 2种可能性，既要查找前序事件，又要备份等待后续事件
- **终点事件(F)**: 1个终点，只需查找前序事件

**特殊说明**：
- `--cycle` 表示所有事件皆为**中间事件**
- `untraced`属性事件不参与两事件关系转换，不是起点事件、不是中间事件、不是终点事件

这种转换使复杂的多事件分析变成了可管理的两两事件分析。

#### 2. 多事件关系：三种核心关系约束

**1) 因果关系**：事件按时间顺序发生
- 时序保证：有A,B,C才会有D,E；有D,E才会有F
- 实现方式：通过事件时间戳和排序机制保证时序关系
- 命令行映射：多个`-e`选项的顺序定义了事件发生的因果顺序
- 举例：内存分配 → 释放；文件打开 → 关闭；sys_enter → sys_exit

**2) 选择关系**：事件集合内的互斥选择
- **互斥性**：A,B,C在同一个分析实例中只有一个会发生
- **完备性**：A,B,C共同构成了所有可能性的完整集合
- **选择性**：通过逗号分隔符定义"多选一"的事件组合
- 命令行映射：单个`-e`选项内用逗号分隔的事件定义一个"事件可能性集合"
- 举例：打开文件描述符(open,socket,accept)，内存分配(kmalloc,kmalloc_node)

**3) 关联关系**：事件通过key值关联
- **关联机制**：相同key值的事件属于同一分析实例
- **字段灵活性**：不同事件的key字段名可以不同，但含义必须一致
- **实现方式**：通过backup红黑树按key索引，实现高效事件关联

**事件路径构建说明**：
- **位置顺序**：第一个`-e`是起点位置，最后一个是终点位置，中间的是中间位置
- **事件集合**：每个`-e`定义一个"事件可能性集合"，集合内事件互斥
- **路径完整性**：所有`-e`选项构成完整的分析路径，用于延迟分析


#### 3. 两事件关系

本文档中使用`event1→event2`表示两个关联事件的关系，代码中使用`two(event1, event2)`表示，输出时使用`event1 => event2`表示。

**关系定义**：
- **因果关系**：有event1（前序事件）才会有event2（后续事件），表示时间和因果顺序
- **选择关系**：event1是A,B,C中的一个，event2是D,E中的一个，来自不同的事件可能性集合
- **关联关系**：event1和event2通过相同的key值关联，属于同一个分析实例

**事件角色说明**：
- **event1（前序事件）**：触发者、原因、影响者
  - 角色：起点或中间事件
  - 行为：需要备份，等待后续事件 (need_backup=1)
  - 作用：作为延迟分析的起始点

- **event2（后续事件）**：接收者、结果、被影响者
  - 角色：终点或中间事件
  - 行为：需要查找前序事件 (need_find_prev=1)
  - 作用：作为延迟分析的结束点

**关联过程**：
1. event1 → 以key为索引创建备份到backup红黑树
2. event2 → 以key为索引查找备份
3. 找到备份 → `event1→event2`关系建立，执行两事件关系分析

**中间事件的双重角色**：
在 A → B → C 事件链中，中间事件B具有双重身份：
- **在 A → B 关系中**：B 作为 event2，是 A 的结果
- **在 B → C 关系中**：B 作为 event1，是 C 的原因
- **处理机制**：中间事件先作为event2存在，查找前序事件；再作为event1存在，备份等待后续事件

**符号使用说明**：
- **文档表示**：`event1→event2` (Unicode 箭头，清晰直观)
- **代码实现**：`two(event1, event2)` (函数调用形式)
- **输出显示**：`event1 => event2` (ASCII 兼容格式)

**四种实现方式**：

| 实现类型 | 函数名 | 用途 | 典型场景 |
|---------|--------|------|---------|
| delay | `delay_two(event1, event2, )` | 延迟分析 | 进程调度延迟、中断处理延迟 |
| pair | `pair_two(event1, event2, )` | 配对分析 | open-close配对、alloc-free配对 |
| kmemprof | `mem_profile_two(event1, event2, )` | 内存分配分析 | alloc事件到free事件的生命周期 |
| syscalls | `syscalls_two(event1, event2, )` | 系统调用分析 | sys_enter到sys_exit的延迟 |

### 事件源

multi-trace支持自定义tracepoint、kprobe和uprobe事件作为分析源。

- **sample_type**: `PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW`
  - `PERF_SAMPLE_TIME`: 核心采样类型，用于计算事件间时间差
  - `PERF_SAMPLE_CALLCHAIN`: 可选，通过`stack`属性为事件启用堆栈采样
- **事件类型**:
  - tracepoint事件：`sys:name`格式
  - kprobe事件：`kprobe:func`格式
  - uprobe事件：`uprobe:func@"file"`格式
- **过滤器**: 每个事件`filter`指定过滤器，支持trace event过滤器语法
- **事件属性**:
  | 属性 | 格式 | 说明 |
  |------|------|------|
  | `key=EXPR` | 表达式 | 指定键，用于关联不同事件 |
  | `printkey=EXPR`| 表达式 | `--perins`输出键值时，可以定制输出 |
  | `role=EXPR` | 表达式 | 计算事件角色，作为event1、event2 |
  | `untraced` | 标志 | 辅助事件，不参与两事件关联 |
  | `trigger` | 标志 | 由事件触发输出，而非周期性输出 |
  | `alias=str` | 字符串 | 事件别名，输出时替代事件名 |
  | `stack` | 标志 | 启用调用栈采样 |
  | `max-stack=int` | 整数 | 指定堆栈深度 |
- **`-e`选项使用规则**：
  - 多个`-e`选项按顺序定义事件路径，每个`-e`选项对应一个位置
  - 单个`-e`选项内用逗号分隔的事件，属于同一位置的可能性集合（互斥发生）
  - **核心约束**：每个`-e`选项必须包含有效事件（未标记`untraced`的事件），不允许全部指定`untraced`属性。
- **untraced 属性约束原因**：
  - **保证分析有效性**：每个`-e`选项代表事件路径中的一个位置，必须能参与到前后事件的关系中。如果某个位置的所有事件都是untraced，则该位置无法参与任何事件关联，导致整个延迟分析失效。
  - **维护逻辑完整性**：事件路径需要完整的"位置1→位置2→位置3"链条。如果中间位置完全untraced，则前序事件无法找到后续事件，后续事件无法找到前序事件，整个路径断裂。


### 事件处理

![multi-trace-design-diagram](../images/multi-trace-design-diagram.png)

#### 1. 核心数据结构

**timeline_node结构**：
```c
struct timeline_node {
    struct rb_node timeline_node; // timeline红黑树节点
    u64    time;                  // 事件时间戳
    struct rb_node key_node;      // backup红黑树节点
    u64    key;                   // 关联键值
    u32 unneeded : 1,             // 标记可释放
        need_find_prev : 1,       // 需要查找前序事件（中间、终点事件）
        need_backup : 1;          // 需要备份（起点、中间事件）
    union perf_event *event;      // 事件数据
};
```

**两个红黑树**：

| 红黑树 | 索引方式 | 用途 | 特点 |
|--------|---------|------|------|
| timeline | 按时间戳排序 | 恢复事件顺序，保证因果关系 | 中序遍历获得时间有序序列 |
| backup | 按key值索引 | 临时存储前序事件，等待后续事件 | 快速查找相同key的事件 |

**关键特性**：同一个timeline_node可以同时挂入两个红黑树，实现高效的时序管理和关联查找。

#### 2. 详细处理流程

**⚠️ 注意：以下是有 `--detail` 选项时的完整处理流程**

**如果没有 `--detail` 选项，multi-trace 使用简化的流式处理模式，详见 [4.2 内存优化](#42-内存优化) 章节。**

---

**阶段1：事件收集与排序**

1. **事件收集**：从多个ringbuffer读取事件
2. **排序处理**：通过order堆排序，生成按时间排序的事件序列
3. **节点创建**：为每个事件分配timeline_node对象，并进行以下处理：

   a) **提取key值**（按优先级）：
   - 事件有`key=EXPR`属性 → key为EXPR表达式值
   - 指定`-k EXPR`选项（给未指定key属性的事件提供默认的key） → key为EXPR表达式值
   - 指定`-p`或`-t`选项 → key为tid（事件发生的线程）
   - 默认情况 → key为cpu（事件发生的CPU）

   b) **确定事件位置**（根据`-e`选项顺序）：
   - 起点事件(A,B,C)：`need_find_prev=0, need_backup=1`，不需要查找前序事件，但需要备份
   - 中间事件(D,E)：`need_find_prev=1, need_backup=1`，需要查找前序事件，需要备份
   - 终点事件(F)：`need_find_prev=1, need_backup=0`，需要查找前序事件，不需要备份
   - `untraced`属性事件：`need_find_prev=0, need_backup=0`，不需要查找前序事件，不需要备份

   c) **处理role属性**（如果指定）：
   - 计算role表达式值
   - Bit1与事件位置一同决定事件是否需要查找前序事件（作为event2）：`need_find_prev &= Bit1`
   - Bit0与事件位置一同决定事件是否需要备份（作为event1）：`need_backup &= Bit0`

   d) **设置**：
   - key、time、event(拷贝的事件)
   - unneeded: 不需要备份直接标记可释放(!need_backup)，或`untraced`属性事件

4. **插入timeline**：将timeline_node按时间戳插入timeline红黑树

**阶段2：事件关联**

按时间顺序（中序遍历timeline红黑树）处理每个事件curr：

1. **查找前序事件**（`curr->need_find_prev == 1`）：
   - 使用curr.key在backup红黑树查找前序事件prev
   - 查找成功：
     - 执行两事件关系分析 `two(prev->event, curr->event)`
     - 从backup红黑树删除prev，仅标记事件`prev->unneeded = 1`，待回收

2. **备份当前事件**（`curr->need_backup == 1`）：
   - **特殊情况**：已备份但未被关联，删除现有备份事件
     - 使用curr.key在backup红黑树查找已备份事件back
     - 查找成功 → 从backup红黑树删除back（保证key唯一），并标记`back->unneeded = 1`，待回收
   - 将curr加入backup红黑树，等待后续事件关联

每一个事件都先尝试作为event2存在（在backup红黑树查找前序事件），再作为event1存在（备份到backup红黑树）

**阶段3：内存回收**

按时间顺序（中序遍历timeline红黑树）扫描，处理已标记事件：
- 删除`unneeded = 1`的节点（直到遇到`unneeded = 0`为止）
- 释放timeline_node对象，回收内存


对于每一个事件，都要顺序执行阶段1、阶段2、阶段3。

#### 3. 两事件关系处理

**事件对定义**：
- `(event1,event2)`表示一个事件对，分配唯一id(two_id)
- event1→event2属于事件对集合：(A→D, A→E, B→D, B→E, C→D, C→E, D→F, E→F) + `--cycle`: (F→A, F→B, F→C)
- event1、event2都不是`untraced`属性事件

**实现1: delay_two(event1, event2, ) - 延迟分析（默认）**

- **初始化**：为所有事件对建立公共延迟统计（latency_dist）
- **处理流程**：
  1. 根据(event1,event2)匹配事件对，获取two_id
  2. 计算延迟 `delta = event2.time - event1.time`
  3. 以(key, two_id)为键，将delta加入latency_dist
  4. 统计延迟分布：min/max/p50/p95/p99等分位数

- **延迟根因分析**：
  - `--than <n>`：输出延迟超过阈值的event1（前序事件）和event2（后续事件）
  - `--detail`：按时间顺序，详细输出event1→event2之间的相关事件，包括`untraced`属性事件。依赖`--than`

    | --detail选项 | 作用 |
    |-------------|------|
    | `-N` | 输出event1前N时间内的事件（前置事件） |
    | `+N` | 输出event2后N时间内的事件（后置事件） |
    | `hide<N` | 隐藏间隔小于N的中间事件 |
    | `1` | 只显示与event1匹配的事件（与same*配合） |
    | `2` | 只显示与event2匹配的事件（与same*配合） |
    | `samecpu` | 显示相同CPU的事件（受`1`/`2`控制） |
    | `samepid` | 显示相同PID的事件（受`1`/`2`控制） |
    | `sametid` | 显示相同TID的事件（受`1`/`2`控制） |
    | `samekey` | 显示相同key的事件（受`1`/`2`控制） |
  - `untraced`属性事件：对延迟根因至关重要，其不参与两事件关系处理，用于还原event1→event2的中间细节。依赖`--detail`
  - `stack`属性事件：输出事件堆栈

- **选项参数**：
  - `--perins`：统计(key, two_id)的延迟分布
    - 未指定：按(0, two_id)统计，key=0，所有实例合并，只为不同的事件对统计延迟分布

**实现2: pair_two(event1, event2, ) - 配对分析**

分析event1和event2是否成对出现，用于检测资源泄漏（如：open-close、alloc-free配对检查）

**实现3: mem_profile_two(event1, event2, ) - 内存分配分析**

专用于内存分配生命周期分析，event1为alloc事件（前序事件），event2为free事件（后续事件）

**实现4: syscalls_two(event1, event2, ) - 系统调用分析**

- **事件约束**：event1必须是`raw_syscalls:sys_enter`（前序事件），event2必须是`raw_syscalls:sys_exit`（后续事件），key只能是"common_pid"
- **初始化**：为事件对(sys_enter, sys_exit)建立一个公共延迟统计（latency_dist）
- **处理流程**：
  1. 计算延迟 `delta = sys_exit.time - sys_enter.time`
  2. 以(common_pid, sys_enter.id)为键，将delta加入latency_dist
  3. 按(线程, 系统调用)统计延迟分布：min/avg/max等
  4. 统计系统调用出错的次数：`sys_exit.ret < 0`
- **选项参数**：
  - `--than`：输出超过阈值的系统调用（不支持`--detail`、`untraced`属性事件）
  - `--perins`：统计(线程, 系统调用)的延迟分布
    - 未指定：只按系统调用统计延迟分布


#### 4. 性能优化策略

multi-trace提供多层次的性能优化机制，可根据实际场景灵活选择。

##### 4.1 排序优化

**order堆排序机制**：
- **工作原理**：利用每个ringbuffer内事件天然有序的特性，使用堆排序从多个ringbuffer中取出全局有序的事件流
- **性能优势**：每次只需从各ringbuffer取一个事件参与堆排序，时间复杂度O(N log M)，其中N为事件总数，M为ringbuffer数量
- **流式处理**：支持事件流式输出，无需在排序器内缓存所有事件，降低内存峰值

**何时需要--order**：
- **必须使用场景**：多个ringbuffer且需要跨ringbuffer进行事件关联（如多CPU的事件关联同一个pid）
- **可以省略场景**：
  1. 单ringbuffer场景（`-C`指定单CPU / `-t`指定单线程 / `-p`指定单线程进程）
  2. 同时满足以下两个条件：
     - 未指定`-k`且所有事件无key属性（按CPU/TID关联，不需要跨ringbuffer关联事件）
     - 未指定`--detail`（不需要event1到event2的中间细节，只配对event1和event2）
     - 同时满足，不需要保证多ringbuffer事件的严格有序


##### 4.2 内存优化

是否有--detail参数对内存占用有不同的影响

无--detail，可以简化处理流程：

**阶段1：事件接收与准备**

1. **事件收集**：从ringbuffer读取事件（通过order堆排序或直接读取）
2. **节点创建**：创建**栈上的临时** `timeline_node` 对象：

   a) **提取key值**（按优先级）：同有 --detail 模式

   b) **确定事件位置**（根据`-e`选项顺序）：
   - 起点事件：`need_find_prev=0, need_backup=1`
   - 中间事件：`need_find_prev=1, need_backup=1`
   - 终点事件：`need_find_prev=1, need_backup=0`
   - `untraced`属性事件：直接跳过，不处理

   c) **处理role属性**（如果指定）：同有 --detail 模式

3. **不插入timeline**：临时对象不插入timeline红黑树

**阶段2：事件关联（流式处理）**

立即处理当前事件curr，无需等待其他事件：

1. **查找前序事件**（`need_find_prev == 1`）：
   - 使用key在backup红黑树查找前序事件prev
   - 查找成功：
     - 执行两事件关系分析 `two(prev->event, curr.event)`
     - 从backup红黑树删除prev
     - **立即释放prev内存**

2. **备份当前事件**（`need_backup == 1`）：
   - **检查旧备份**：如果已有相同key的备份（旧事件未被关联）
     - 删除旧备份，**立即释放**内存
   - **创建新备份**：
     - 分配新的 `timeline_node` 对象（堆内存），复制栈上的对象
     - 复制事件数据：`memdup(event, event->header.size)`
     - 插入backup红黑树，等待后续事件关联

**阶段3：内存回收（自动完成）**

- **终点事件**（`need_backup=0`）：栈上临时对象自动释放
- **已配对的前序事件**：在阶段2查找时立即释放
- **未配对的前序事件**：保留在backup红黑树中，等待后续事件或程序退出时释放

**处理模式对比**：

| 处理步骤 | 有 `--detail`（完整模式） | 无 `--detail`（简化模式） |
|---------|--------------------------|-------------------------|
| **事件对象** | 堆内存分配 | 栈上临时对象 |
| **timeline红黑树** | 使用，保存所有事件 | 不使用 |
| **backup红黑树** | 使用，标记unneeded延迟释放 | 使用，立即释放 |
| **untraced事件** | 插入timeline，用于详细分析 | 直接跳过 |
| **前序事件回收** | 标记`unneeded=1`，延迟批量释放 | 配对成功后立即释放 |
| **内存占用** | 高（保存所有事件） | 低（只保存未配对前序事件） |
| **处理速度** | 较慢（需维护timeline） | 快（流式处理） |
| **适用场景** | 需要详细事件链分析 | 只需统计数据 |


##### 4.3 Key选择策略

选择合适的key对分析至关重要：

| Key类型 | 设置方式 | 适用场景 | 性能特点 |
|---------|---------|---------|---------|
| **CPU关联** | 默认（不指定任何key相关参数） | 分析CPU维度的事件关系 | 最优，CPU隔离 |
| **进程/线程关联** | `-p pid` / `-t tid` / `-k common_pid` | 分析进程/线程维度的事件关系 | 优秀，按TID隔离 |
| **自定义字段关联** | `-k EXPR` / `key=EXPR` | 复杂关联场景（如按指针、优先级等） | 取决于key值分布 |

##### 4.4 从事件上下文理解Key选择

**Key选择的本质问题**：你希望哪个event1和哪个event2关联在一起？

从"事件发生的上下文"角度理解key选择，比从"字段名"入手更本质。

**事件的基本上下文**

每个事件发生时，都有两个基本上下文：
- **CPU上下文**：事件发生在哪个CPU上
- **线程上下文**：事件发生在哪个线程上（common_pid）

```
事件发生 → [CPU=2, TID=1234, 事件字段...]
```

**核心问题：延迟路径上，上下文是否变化？**

| 上下文变化情况 | Key选择 | 典型场景 |
|---------------|----------|----------|
| **同CPU、同线程** | 默认（按CPU）或不指定 | 软中断、hardirq |
| **可能跨CPU、同线程** | `-k common_pid` 或 `-p`/`-t` | 系统调用（线程可能迁移） |
| **跨线程、可能跨CPU** | `key=资源标识` | 调度延迟、跨进程通信 |

**三种典型路径模式**

**模式A：同上下文路径**（最简单）

```
softirq_entry [CPU=0, TID=100]
    ↓ (同CPU、同线程)
softirq_exit  [CPU=0, TID=100]

→ Key：默认按CPU，无需指定
```

**模式B：线程内路径，可能迁移**

```
sys_enter [CPU=0, TID=1234]
    ↓ (同线程，但CPU可能变化)
sys_exit  [CPU=2, TID=1234]

→ Key：common_pid（线程ID）
→ 需要 --order（跨CPU排序）
```

**模式C：跨线程路径**（最复杂）

```
sched_wakeup  [CPU=0, TID=100]  唤醒 pid=1234
    ↓ (被唤醒的进程在另一个CPU开始运行)
sched_switch  [CPU=2, TID=1234] next_pid=1234

→ event1的TID=100（唤醒者），event2的TID=1234（被唤醒者）
→ Key不能用common_pid！要用业务字段 pid/next_pid
```

**Key选择判断流程**

```
问：event1到event2，线程上下文是否相同？

是 → 问：CPU上下文是否相同？
     │
     ├── 是 → 不需要key（默认按CPU）
     │        例：软中断、本地定时器
     │
     └── 否 → key=common_pid，需要 --order
              例：系统调用（线程可能迁移）

否 → 必须用"业务字段"作为key，需要 --order
     问：什么东西从event1"传递"到event2？
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

**具体例子对比**

**例1：软中断延迟（同上下文）**
```bash
# 软中断不会跨CPU，不会切换线程
# event1和event2的CPU、TID完全相同
perf-prof multi-trace \
    -e irq:softirq_entry/vec==3/ \
    -e irq:softirq_exit/vec==3/ \
    -i 1000
# 不需要 -k，不需要 --order
```

**例2：系统调用延迟（同线程，可能跨CPU）**
```bash
# 系统调用期间线程可能被迁移到其他CPU
# event1.TID == event2.TID，但CPU可能不同
perf-prof multi-trace \
    -e raw_syscalls:sys_enter \
    -e raw_syscalls:sys_exit \
    -k common_pid --order -i 1000
```

**例3：调度延迟（跨线程）**
```bash
# 唤醒者和被唤醒者是不同线程
# event1 [TID=A] 唤醒 pid=B
# event2 [TID=B] 开始运行
# 两个事件的common_pid不同！
perf-prof multi-trace \
    -e 'sched:sched_wakeup//key=pid/' \
    -e 'sched:sched_switch//key=next_pid/' \
    --order -i 1000
```

**Key选择的本质总结**

```
Key的作用 = 在"上下文可能变化"的情况下，找到"不变的关联标识"

上下文相同 → 用默认（CPU）
上下文不同 → 找"传递的东西"作为key
```


#### 5. 关键算法特性

1. **时间序列构建**：通过timeline红黑树构建全局时间有序的事件序列，保证因果关系
2. **事件快速关联**：通过backup红黑树按key值实现O(log n)时间复杂度的事件匹配，还原关联关系
3. **两事件关系分析**：将复杂多事件关系转换为可管理的两两事件关系（delay、pair等）
4. **流式处理能力**：依赖堆排序，事件可以流式处理，避免大量内存占用


### 状态统计

multi-trace支持运行时状态查询和监控：

**信号处理**：
- `SIGUSR1`：输出当前统计信息（不中断运行）
- `SIGUSR2`：输出当前统计信息（不中断运行）


## 输出

multi-trace的输出格式根据`--impl`指定的实现类型而有所不同。

### 输出格式

#### 实现1: delay - 延迟分析（默认）

**标准输出格式**：

```
[键名] [comm]    start => end      calls   total(us)      min(us)      p50(us)      p95(us)      p99(us)      max(us)
[键值] [进程名]  event1 => event2  统计数据...
```

**字段说明**：
- **键名**：仅`--perins`时显示，根据key来源确定表头名称（见下表）
  | 场景 | 键名显示 | 说明 |
  |------|---------|------|
  | 指定`-k EXPR`或任意事件有`key=EXPR` | EXPR字符串 | 显示第一个key表达式 |
  | 指定`-p`或`-t`（进程/线程附加） | THREAD | 固定显示"THREAD" |
  | 默认情况（无上述选项） | CPU | 固定显示"CPU" |
- **键值**：仅`--perins`时显示，具体的key值（如果事件指定`printkey=EXPR`属性，则执行表达式来定制输出，表达式用`key`作为变量，使用printf内建函数，如：printkey=printf("%d",key)）
- **comm**: 仅`--perins`选项，且`-p`或`-t`，显示该列，显示进程名
- **start**: 事件对的event1（前序事件），显示事件名（去除"sys:"前缀）或事件alias属性值
- **end**: 事件对的event2（后续事件），显示事件名（去除"sys:"前缀）或事件alias属性值
- **calls**: event1→event2统计的次数
- **total(us)**: event1→event2总延迟时间(微秒)
- **min/p50/p95/p99/max(us)**: event1→event2延迟分位数统计(微秒)

**行索引**：

| `--perins`选项 | 行索引 | 输出行数 | 说明 |
|---------------|--------|---------|------|
| **使用** | (key, two_id) | 多行 | 每个(key值, 事件对)一行 |
| **未使用** | (0, two_id) | 少行 | 每个事件对一行，合并所有key |

**详细输出模式**：

当使用`--than`或`--lower`阈值过滤时，额外输出：

1. **基础信息**：
   - 输出触发阈值的event1和event2的详细信息
   - 如果事件有`stack`属性，输出调用栈

2. **完整事件链**（`--detail`）：
   - 输出event1→event2之间的所有相关事件（包括`untraced`属性事件），事件有`stack`属性，也输出调用栈
   - 支持时间范围扩展（`-N`前置事件，`+N`后置事件）
   - 支持事件过滤（`samecpu`、`samepid`、`sametid`、`samekey`）
   - 支持事件隐藏（`hide<N`隐藏间隔小的事件）

**输出示例**：

```bash
# 示例1：不使用--perins，合并统计
$ perf-prof multi-trace -e irq:softirq_entry -e irq:softirq_exit -i 1000
        start => end             calls        total(us)      min(us)      p50(us)      p95(us)      p99(us)      max(us)
-------------    ------------ -------- ---------------- ------------ ------------ ------------ ------------ ------------
softirq_entry => softirq_exit     1698         3761.098        0.351        1.807        5.422        8.876       68.879

# 示例2：使用--perins，按CPU统计
$ perf-prof multi-trace -e irq:softirq_entry -e irq:softirq_exit -i 1000 --perins
CPU         start => end             calls        total(us)      min(us)      p50(us)      p95(us)      p99(us)      max(us)
--- -------------    ------------ -------- ---------------- ------------ ------------ ------------ ------------ ------------
0   softirq_entry => softirq_exit      234          672.741        0.451        1.055        6.840       11.071      138.790
1   softirq_entry => softirq_exit      175          457.090        0.491        1.566        6.833       10.841       14.908
```

#### 实现2: pair - 配对分析

输出未配对的event1（缺少event2）或event2（缺少event1），用于检测资源泄漏。

**输出格式**：
```
未配对的event1事件列表
未配对的event2事件列表
```

#### 实现3: kmemprof - 内存分配分析

输出内存分配的生命周期统计，格式类似delay实现，但专注于内存分配特性。

#### 实现4: syscalls - 系统调用分析

输出系统调用的延迟统计，按系统调用类型分组。

**输出格式**：
```
thread comm             syscalls                calls        total(us)      min(us)      avg(us)      max(us)    err
```
**字段说明**：
- **thread/comm**：线程id/线程名（仅`--perins`时显示）
- **syscalls**: 系统调用名(系统调用号)
- **calls**: 系统调用总次数
- **total(us)**: 系统调用总耗时(微秒)
- **min/avg/max(us)**: 系统调用耗时的最小值、平均值、最大值(微秒)
- **err**: 系统调用出错次数
**行索引**：
| `--perins`选项 | 行索引 | 输出行数 | 说明 |
|---------------|--------|---------|------|
| **使用** | (common_pid, sys_enter.id) | 多行 | 每个(线程, 系统调用)一行 |
| **未使用** | (0, sys_enter.id) | 少行 | 每个系统调用一行，合并所有线程 |


### 关键指标解读

multi-trace提供丰富的统计指标，帮助全面评估系统性能。

#### 1. 延迟分布指标

| 指标 | 含义 | 应用场景 | 关注点 |
|------|------|---------|--------|
| **min** | 最小延迟 | 评估最佳性能 | 理论性能下限 |
| **p50（中位数）** | 50%的延迟在此之下 | 评估典型性能 | 大部分情况的表现 |
| **p95** | 95%的延迟在此之下 | 识别异常值 | 少数慢请求的影响 |
| **p99** | 99%的延迟在此之下 | SLA保障 | 长尾延迟问题 |
| **max** | 最大延迟 | 识别性能瓶颈 | 最坏情况分析 |

#### 2. 统计量指标

| 指标 | 计算方式 | 适用场景 |
|------|---------|---------|
| **calls** | 配对成功的事件数 | 评估事件频率和配对完整性 |
| **total** | 所有延迟之和 | 计算平均延迟（total/calls） |



## 分析方法论

### 标准分析流程

multi-trace分析遵循"选事件 → 定关系 → 配参数 → 看结果 → 深挖根因"的五步法：

#### 第1步：确定分析目标和事件选择

根据分析任务，确认分析目标，以及选择哪些事件，设定过滤器。

**目标分类**：

| 分析目标 | 典型场景 | 推荐事件组合 | 选项参数 |
|---------|---------|-------------|---------|
| **延迟分析** | 调度延迟、中断延迟、系统调用耗时分析 | 起点事件 + [中间事件] + 终点事件 | `--impl delay`或不指定 |
| **配对分析** | open-close配对 | 打开事件 + 关闭事件 | `--impl pair` |
| **系统调用分析** | 统计系统调用耗时 | `raw_syscalls:sys_enter` + `raw_syscalls:sys_exit` | `--impl syscalls` |

**事件选择原则**：
1. **精确性**：事件能准确反映分析目标
2. **完整性**：覆盖完整的事件链路，确定哪些是起点事件、终点事件
3. **低开销**：优先选择低频事件

**过滤器设计原则**：
1. 根据分析目标，确定初始的事件过滤器
2. 根据后续分析，调整过滤器，缩小分析范围

#### 第2步：设计key关联表达式

**key设计原则**：

| 关联维度 | Key设计 | 示例 | 适用场景 |
|---------|--------|------|---------|
| **CPU维度** | 默认（不指定key） | - | 分析CPU级别的事件关系 |
| **进程/线程维度** | `-k common_pid` 或 `-p`/`-t` | `common_pid` | 跟踪进程/线程的事件链 |
| **单维度** | `-k 事件字段` / key属性 | `pid`、`ptr` | 跟踪资源的生命周期 |
| **复合维度** | `-k 表达式` / key属性 | `(ctrl_id<<48)+(qid<<32)+cid` | 多维度关联分析 |

单维度、复合维度，优先使用key属性，每个事件可以指定不同的key属性。

#### 第3步：配置选项参数

1. `--order`       决策是否需要排序
2. `--perins`      决策是否按实例统计
3. `--cycle`       决策是否需要组成环形事件模式

#### 第4步：运行并解读结果

**初步分析检查清单**：

1. **配对率检查**：calls数量是否合理？
2. **延迟分布检查**：p50、p95、p99是否正常？
3. **异常值检查**：max是否远大于p99？

#### 第5步：深入分析根因

1. 由p99，max等数值，决定`--than`参数
2. 决策`--detail`如何筛选event1→event2中间的事件
3. 决策是否进一步调整事件过滤器，缩小范围
4. 决策是否需要添加更多`untraced`事件，是否需要打开堆栈

### 数据驱动分析方法

multi-trace支持完全数据驱动的分析方法，不预设业务特征

**迭代分析流程**

```
第1轮：基础统计
  ↓
获取整体延迟分布 → 识别异常区间
  ↓
第2轮：实例级统计（--perins）
  ↓
定位异常的key值 → 找出问题实例
  ↓
第3轮：详细分析（--detail）
  ↓
分析事件链细节 → 缩小问题范围
  ↓
第4轮：还原细节
  ↓
添加更多辅助事件，调节过滤器 → 进一步缩小范围，直到定位根因
```

## 应用示例

### 场景1：进程调度延迟分析

- 任务分解：调度延迟分为2类：从进程唤醒到进程开始运行的时间，进程Running态被抢占到再次开始运行的时间
- 目标分类：延迟分析，`--impl delay`或不指定
- 选事件：只需要起点事件、终点事件
  - 起点事件：3种可能性：
    - sched:sched_wakeup（进程唤醒，通过pid字段获取唤醒的进程id）
    - sched:sched_wakeup_new（新创建的进程被唤醒，通过pid字段获取唤醒的进程id）
    - sched:sched_switch（进程Running态被抢占时发生进程切换，通过prev_pid获取被抢占的进程id）
      - 过滤器：需要筛选Running态发生的进程切换，要排除swapper进程（swapper也会发生Running态的切换，我们并不需要特殊分析swapper）
  - 终点事件：1种可能性：sched:sched_switch（进程开始运行，通过next_pid获取开始运行的进程id）
- 初始过滤器：根据分析任务，确定是否需要筛选进程名，进程id等。
  - sched:sched_wakeup：comm字段过滤被唤醒进程的进程名，pid字段过滤其进程id
  - sched:sched_wakeup_new：comm字段过滤被唤醒进程的进程名，pid字段过滤其进程id
  - sched:sched_switch：通过prev_state字段过滤Running态发生的进程切换，prev_comm字段过滤进程名，prev_pid字段过滤进程id
  - sched:sched_switch：next_comm字段过滤开始运行进程的进程名，next_pid字段过滤其进程id
- 选择key：以进程id来关联事件，分析每个进程的调度延迟
  - sched:sched_wakeup：key=pid（pid被唤醒）
  - sched:sched_wakeup_new：key=pid（pid被唤醒）
  - sched:sched_switch：key=prev_pid（prev_pid被抢占）
  - sched:sched_switch：key=next_pid（next_pid开始运行）
- 选项参数：需要--order

**命令行**：
```bash
# 基础分析，不筛选进程名、进程id
# 起点事件sched:sched_switch：prev_state==0&&prev_pid>0，筛选Running态被抢占，且排除swapper
perf-prof multi-trace \
    -e 'sched:sched_wakeup,sched:sched_wakeup_new,sched:sched_switch/prev_state==0&&prev_pid>0/key=prev_pid/' \
    -e 'sched:sched_switch//key=next_pid/' \
    -k pid --order -i 1000

# 深入分析，指定--than、--detail
perf-prof multi-trace \
    -e 'sched:sched_wakeup,sched:sched_wakeup_new,sched:sched_switch/prev_state==0&&prev_pid>0/key=prev_pid/' \
    -e 'sched:sched_switch//key=next_pid/' \
    -k pid --order -i 1000 --than 10ms --detail=samecpu

# 还原细节，增加untraced事件，及过滤器
perf-prof multi-trace \
    -e 'sched:sched_wakeup/comm~"CPU*"/,sched:sched_switch/prev_state==0&&prev_comm~"CPU*"/key=prev_pid/' \
    -e 'sched:sched_switch/next_comm~"CPU*"/key=next_pid/,sched:sched_migrate_task/comm~"CPU*"/untraced/stack/key=pid/' \
    -k pid --order -i 1000 --than 10ms --detail=samecpu
```

**参数说明**：
- **第一个`-e`**：三个起点事件（唤醒或切出）
- **第二个`-e`**：一个终点事件（切入运行）
- **`-k pid`**：sched:sched_wakeup和sched:sched_wakeup_new的默认key
- **`--order`**：跨CPU排序（必需）
- **`--than 10ms`**：只输出延迟>10ms的情况
- **`--detail`**：输出完整事件链
- **`stack`**：记录调用栈
- 未指定`--impl` 默认delay

### 场景2：软中断处理延迟分析

- 任务分解：软中断函数的执行，无论是在irq_exit时，还是在ksoftirqd线程内，都不会跨cpu
- 目标分类：延迟分析，`--impl delay`或不指定
- 选事件：只需要起点事件、终点事件
  - 起点事件：1种可能性：irq:softirq_entry（软中断进入）
  - 终点事件：1种可能性：irq:softirq_exit（软中断退出）
- 初始过滤器：是否要分析特定的软中断（如：NET_RX）过滤vec字段
- 选择key：以CPU关联
- 选项参数：不需要--order

**命令行**：
```bash
# 基础统计
perf-prof multi-trace -e 'irq:softirq_entry' -e 'irq:softirq_exit' -i 1000

# 详细分析，筛选NET_RX（vec值因内核版本而异）
perf-prof multi-trace -e 'irq:softirq_entry/vec==3/stack/'-e 'irq:softirq_exit/vec==4/stack/' \
    --than 100us --detail --perins -i 1000
```

**参数说明**：
- 不需要`--order`：单CPU内事件天然有序
- **`--than 100us`**：关注>100微秒的延迟

### 场景3：内存分配生命周期跟踪

**分析目标**：追踪内存分配到释放的完整过程

**命令行**：
```bash
# 使用kmemprof（multi-trace的特化版本）
perf-prof kmemprof \
    -e 'kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/' \
    -e 'kmem:kfree//ptr=ptr/' \
    -k ptr --order -m 128

# 或使用multi-trace直接分析
perf-prof multi-trace \
    -e 'kmem:kmalloc//key=ptr/alias=alloc/stack/' \
    -e 'kmem:kfree//key=ptr/alias=free/' \
    -k ptr --impl pair --order
```

**参数说明**：
- **`ptr=ptr`**：提取指针字段作为key
- **`--impl pair`**：配对分析模式，检测未释放的内存
- **`-m 128`**：增大缓冲区，应对高频事件

### 场景4：系统调用延迟分析

**分析目标**：统计各类系统调用的延迟分布

**命令行**：
```bash
# 所有系统调用统计
perf-prof syscalls -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit \
    -p 1234 -i 1000 --perins

# 特定系统调用详细分析
perf-prof multi-trace -e 'raw_syscalls:sys_enter/id==0/stack/' -e 'raw_syscalls:sys_exit/id==0/stack/' \
    -k common_pid --than 1ms --detail --order
```

**参数说明**：
- **syscalls**：multi-trace的特化版本，自动按系统调用分类
- **`id==0`**：过滤read系统调用（x86_64）
- **`-p 1234`**：只分析特定进程

### 高级用法

```bash
# 检测长时间占用CPU的进程（排除idle进程）
perf-prof multi-trace -e sched:sched_switch//role="(next_pid?1:0)|(prev_pid?2:0)"/ --cycle -i 1000
```


## 最佳实践总结

#### 性能优化实践

| 优化项 | 建议配置 | 适用场景 |
|--------|---------|---------|
| **缓冲区** | `-m 64/128/256` | 根据事件频率：低/中/高 |
| **采样间隔** | `-i 500~2000` | 高频事件使用长间隔 |
| **过滤器** | `/field>value/` | 在内核态过滤，减少数据传输 |
| **排序** | 按需使用`--order` | 仅跨ringbuffer关联时使用 |

#### 参数调优实践

| 参数 | 调优建议 | 注意事项 |
|------|---------|---------|
| **key表达式** | 简洁、高效、分布均匀 | 避免复杂计算，避免hash冲突 |
| **阈值设置** | 基于p99设置 | 过低产生噪音，过高遗漏问题 |
| **detail选项** | 按需使用 | 显著增加内存占用 |

## 相关资源
- [事件基础文档](../events/)
- [sample_type采样类型说明](../sample_type.md)
- [选项参数完整参考](../main_options.md)
- [表达式语法参考](../expressions.md)
- [实际案例分析](../examples/)



# multi-trace派生的分析器

## rundelay - 调度延迟分析

rundelay是multi-trace的特化版本，预配置了调度相关事件和过滤器，专用于分析进程调度延迟。

### 概述

rundelay分析两类调度延迟：
1. **唤醒延迟**：从进程被唤醒(sched_wakeup/sched_wakeup_new)到进程开始运行(sched_switch next_pid)的时间
2. **抢占延迟**：进程Running态被抢占(sched_switch next_pid)到再次开始运行(sched_switch next_pid)的时间

### 基础用法

```bash
perf-prof rundelay [OPTION...] -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch [--filter comm] [--than ns] [--detail] [--perins] [--heatmap file]
```

**与multi-trace的关系**：
- rundelay基于multi-trace实现，共享相同的核心机制
- 预配置了调度相关的事件组合，及事件的过滤器
- 自动处理key关联和事件排序，无需手动指定`-k`和`--order`
- 支持`--filter`选项直接过滤进程名（comm），无需手动编写filter表达式
- 使用通配符`sched:sched_wakeup*`自动匹配`sched_wakeup`和`sched_wakeup_new`事件

### 核心特性

#### 1. 预配置的事件组合

**起点事件（第一个 `-e` 指定，使用通配符 `sched:sched_wakeup*,sched:sched_switch`）**：
- `sched:sched_wakeup`：进程被其他进程唤醒，通过`pid`字段获取唤醒的进程id（通配符自动匹配）
- `sched:sched_wakeup_new`：新创建的进程被唤醒，通过`pid`字段获取唤醒的进程id（通配符自动匹配）
- `sched:sched_switch`：进程Running态被抢占发生进程切换，通过`prev_pid`获取被抢占的进程id
  - 自动过滤器：筛选Running态的进程切换(`prev_state==0`或`prev_state==TASK_REPORT_MAX`)，排除swapper进程(`prev_pid>0`)

**终点事件（第二个 `-e` 指定）**：
- `sched:sched_switch`：进程开始运行，通过`next_pid`获取开始运行的进程id

**Key关联（自动处理）**：
- rundelay自动按进程id关联事件，无需手动指定`-k pid`
- 自动为不同事件设置正确的key字段：
  - `sched:sched_wakeup`和`sched:sched_wakeup_new`：自动设置`key=pid`
  - 起点`sched:sched_switch`：自动设置`key=prev_pid`
  - 终点`sched:sched_switch`：自动设置`key=next_pid`

**排序优化（自动启用）**：
- rundelay自动启用`--order`排序，保证跨CPU事件的时序正确性

#### 2. 运行模式适配

rundelay根据attach方式自动优化性能：

**进程/线程模式**（使用`-p`或`-t`）：
- 问题：sched事件不适合绑定到特定线程
- 解决：自动切换到全局CPU模式，并过滤线程id

**CPU模式**（使用`-C`或默认）：
- 直接在指定CPU上采集事件

#### 3. 智能过滤器支持

multi-trace提供智能过滤器支持，根据选项不同自动应用不同的过滤策略。

**过滤器优先级**：
1. **优先使用 `--filter` 选项**：过滤对应的进程名
2. **其次使用 `-p`/`-t` 选项**：过滤指定的所有线程id

**过滤器设置规则**：

**1. 使用 `--filter` 选项时**：
- 指定一个或多个comm，逗号分隔，对每个comm设置过滤器。comm支持通配符(包含`*?[`任意字符)，过滤条件需要使用 `~` 运算符；否则使用 `==` 运算符。
```bash
# 过滤器设置
--filter "java,pyth*"  # 会自动转换为：
  # sched:sched_wakeup: comm=="java" || comm~"pyth*"
  # sched:sched_wakeup_new: comm=="java" || comm~"pyth*"
  # sched:sched_switch(起点): prev_state==0 && (prev_comm=="java" || prev_comm~"pyth*")
  # sched:sched_switch(终点): next_comm=="java" || next_comm~"pyth*"
```

**2. 使用 `-p`/`-t` 选项时**：
- 指定一个或多个进程/线程，逗号分隔。
```bash
# 进程234包含线程：234, 345
-p 234 # 自动转换为：
  # sched:sched_wakeup: pid==234 || pid==345
  # sched:sched_wakeup_new: pid==234 || pid==345
  # sched:sched_switch(起点): prev_state==0 && (prev_pid==234 || prev_pid==345)
  # sched:sched_switch(终点): next_pid==234 || next_pid==345
```

#### 4. 内核版本兼容性

针对不同内核版本的`prev_state`字段自动适配：

| 内核版本 | prev_state值 | 说明 |
|---------|-------------|------|
| >= 4.14 | `TASK_REPORT_MAX` (0x100) | 新版本使用特殊标记表示Running态 |
| < 4.14 | `0` | 旧版本使用0表示Running态 |

代码中通过`kernel_release()`动态检测并应用正确的过滤器。

### 选项参数

**核心选项**：
- `-e, --event`：事件选择器
  - 第一个`-e`：起点事件，使用`sched:sched_wakeup*,sched:sched_switch`
  - 第二个`-e`：终点事件，使用`sched:sched_switch`
  - 支持在事件后添加属性（如`//stack/`启用调用栈）
  - 每个`-e`均支持添加更多`untraced`属性事件，这些事件需要手动设置过滤器，手动指定key
- `--filter <comm>`：过滤进程名，支持通配符（自动应用到所有事件的相应comm字段）
- `--than <ns>`：延迟阈值，只输出超过阈值的情况，单位支持s/ms/us/ns
- `--detail`：详细输出模式，显示事件链（支持samecpu/samepid/sametid/samekey等过滤）
- `--perins`：按实例（进程）统计延迟分布
- `--heatmap <file>`：生成延迟热图文件

**Attach选项**：
- `-p, --pids <pid,...>`：附加到进程（会自动切换到全局CPU模式）
- `-t, --tids <tid,...>`：附加到线程（会自动切换到全局CPU模式）
- `-C, --cpus <cpu,...>`：监控指定CPU

**自动处理的选项**：
- `-k, --key`：自动设置为`pid`，无需手动指定
- `--order`：自动启用事件排序，无需手动指定

### 输出格式

#### 标准输出（无`--perins`）

```
        start => end             calls        total(us)      min(us)      p50(us)      p95(us)      p99(us)      max(us)
-------------    ------------ -------- ---------------- ------------ ------------ ------------ ------------ ------------
sched_wakeup => sched_switch      1234         5678.901        0.123        2.345        10.234       25.678       100.234
```

#### 实例统计（`--perins`）

```
THREAD  comm         start => end             calls        total(us)      min(us)      p50(us)      p95(us)      p99(us)      max(us)
------ ---------- -------------    ------------ -------- ---------------- ------------ ------------ ------------ ------------ ------------
1234   java       sched_wakeup => sched_switch      100          234.567        0.123        1.234        5.678        12.345       50.123
```

**字段说明**：
- **THREAD**：进程/线程ID（仅`--perins`时显示）
- **comm**：进程名（仅`--perins`时显示）
- **start**：起点事件类型（sched_wakeup、sched_wakeup_new或sched_switch）
- **end**：终点事件（sched_switch）
- **calls**：配对成功的事件数
- **total(us)**：总延迟时间（微秒）
- **min/p50/p95/p99/max(us)**：延迟分位数统计（微秒）

### 使用示例

#### 示例1：基础延迟统计

```bash
# 全局统计，所有进程的调度延迟
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch -i 1000

# 统计每个进程的调度延迟分布
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch -i 1000 --perins

# 分析进程1的调度延迟，超过4ms输出详情
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch -p 1 -i 1000 --than 4ms

# 使用进程名过滤，分析所有python进程
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch --filter python -i 1000 --than 4ms

# 根因分析：显示延迟超过4ms的完整事件链（只显示相同key的事件）
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch --filter python \
                   -i 1000 --than 4ms --detail=samekey

# 根因分析：显示相同CPU上的所有事件，判断是否有进程迁移（启用堆栈）
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch,sched:sched_migrate_task//untraced/stack/  \
                   --filter python -i 1000 --than 4ms --detail=samecpu

# 联合分析：追加采样，判断是否有长循环阻塞调度
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e 'sched:sched_switch,sched:sched_migrate_task//untraced/,profile/-F 500 --watermark 50 -g/untraced/'  \
                   --filter python -i 1000 --than 100ms --detail=samecpu
```

### 性能优化建议

| 场景 | 配置建议 | 原因 |
|------|---------|------|
| **全局分析** | `-m 128` 或更大 | 调度事件频率高，需要足够缓冲区 |
| **特定进程** | `--filter <comm>` 或 `-p <pid>` | 减少数据量，`--filter`在内核态过滤更高效 |

### 与multi-trace的区别

| 特性 | rundelay | multi-trace |
|------|----------|------------|
| **事件配置** | 使用通配符`sched:sched_wakeup*`，自动匹配相关事件 | 需手动指定所有事件 |
| **Key关联** | 自动设置key关联（无需`-k pid`） | 需手动指定`-k`或为每个事件设置`key=`属性 |
| **排序** | 自动启用`--order`排序 | 需手动指定`--order` |
| **过滤器** | 支持`--filter comm`自动应用到所有事件 | 需手动为每个事件编写filter表达式 |
| **适用场景** | 专用于调度延迟分析 | 通用的多事件关系分析 |
| **使用复杂度** | 简单，只需指定事件和过滤条件 | 灵活，但需要更多配置 |

### 技术要点

1. **自动事件配置**：使用通配符`sched:sched_wakeup*`自动匹配`sched_wakeup`和`sched_wakeup_new`事件
2. **自动Key关联**：自动为不同事件设置正确的key字段（pid/prev_pid/next_pid），无需手动指定`-k`选项
3. **自动排序**：自动启用`--order`排序，保证多CPU事件的全局有序性
4. **内核兼容**：自动适配不同内核版本的`prev_state`字段定义（4.14前后）
5. **性能优化**：非oncpu模式（使用`-p`/`-t`）下自动切换到全局CPU采样
6. **智能过滤**：根据事件类型自动选择正确的字段名（pid/prev_pid/next_pid, comm/prev_comm/next_comm）

### 相关资源

- [multi-trace核心文档](#multi-trace---多事件关系分析)
- [进程调度延迟分析示例](#场景1进程调度延迟分析)
- [事件过滤语法参考](../Event_filtering.md)


## syscalls - 系统调用耗时分析

syscalls是multi-trace的特化版本，预配置了系统调用相关事件，专用于分析系统调用的延迟和性能。

### 概述

syscalls分析从系统调用进入(sys_enter)到系统调用退出(sys_exit)的完整生命周期，统计每个系统调用的延迟分布和错误率。

**核心功能**：
- **延迟统计**：计算每个系统调用的延迟分布(min/avg/max)
- **错误检测**：统计系统调用的错误次数(`ret < 0`)
- **进程级统计**：支持按进程/线程统计系统调用性能

### 基础用法

```bash
perf-prof syscalls [OPTION...] -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit \
                   [--than ns] [--perins] [--heatmap file]
```

**与multi-trace的关系**：
- syscalls基于multi-trace实现，固定使用`--impl syscalls`
- 预配置了系统调用相关事件：`raw_syscalls:sys_enter`和`raw_syscalls:sys_exit`
- 自动按系统调用类型分组统计（通过sys_enter.id字段）
- 自动处理key关联（强制使用`common_pid`作为key）
- 自动监听进程退出事件(sched:sched_process_free)，用于清理无法完成的系统调用（`exit`、`exit_group`）

### 核心特性

#### 1. 预配置的事件组合

**起点事件（第一个 `-e` 指定）**：
- `raw_syscalls:sys_enter`：系统调用进入事件
  - `id`字段：系统调用编号（用于识别系统调用类型）
  - `args`数组：系统调用参数

**终点事件（第二个 `-e` 指定）**：
- `raw_syscalls:sys_exit`：系统调用退出事件
  - `id`字段：系统调用编号（与sys_enter.id对应）
  - `ret`字段：系统调用返回值（用于检测错误）

**额外监听事件（自动配置）**：
- `sched:sched_process_free`：进程退出事件
  - 作用：当进程退出时，清理该进程未完成的系统调用（避免内存泄漏）
  - 实现：调用`reclaim()`函数，从backup红黑树删除该进程`exit, exit_group`系统调用的sys_enter事件

#### 2. Key关联（强制使用common_pid）

syscalls实现默认使用`common_pid`作为key：
- **固定key字段**：只能使用`common_pid`（进程/线程ID）
- **原因**：syscalls需要按线程维度统计系统调用性能，无法使用其他key
- **实现限制**：如果使用其他key，系统调用分类统计将失效

#### 3. 系统调用分类统计

syscalls自动按系统调用类型分组统计：

**分组键（用于latency_dist统计）**：
- `(common_pid, sys_enter.id)`：按线程和系统调用分组
- `common_pid`：进程/线程ID（key值）
- `sys_enter.id`：系统调用编号（从sys_enter事件获取）

**统计指标**：
- **calls**：系统调用总次数（sys_enter→sys_exit配对成功的次数）
- **total(us)**：系统调用总耗时（微秒）
- **min/avg/max(us)**：系统调用耗时的最小值、平均值、最大值（微秒）
- **err**：系统调用出错次数（`sys_exit.ret < 0`的次数）

#### 4. 进程退出处理机制

syscalls通过监听`sched:sched_process_free`事件，及时清理退出进程的未完成系统调用：

**工作流程**：
1. **事件监听**：自动创建`sched:sched_process_free`事件监听器（evsel）
2. **事件处理**：当进程退出时，触发`syscalls_extra_sample()`函数
3. **资源回收**：调用`reclaim(pid, REMAINING_SYSCALLS)`，删除该进程的所有未完成系统调用
4. **统计处理**：对未完成的系统调用调用`remaining()`函数，进行必要的统计

### 选项参数

**核心选项**：
- `-e, --event`：事件选择器
  - 第一个`-e`：`raw_syscalls:sys_enter`（必需，可以添加过滤器，`stack`属性）
  - 第二个`-e`：`raw_syscalls:sys_exit`（必需，可以添加过滤器，`stack`属性）
  - 不支持添加更多事件（syscalls实现的限制）
- `--than <ns>`：延迟阈值，只输出超过阈值的情况，单位支持s/ms/us/ns
- `--perins`：按实例（线程）统计系统调用延迟分布
- `--heatmap <file>`：生成系统调用延迟热图文件

**Attach选项**：
- `-p, --pids <pid,...>`：附加到进程
- `-t, --tids <tid,...>`：附加到线程
- `-C, --cpus <cpu,...>`：监控指定CPU

**不支持的选项**：
- `--detail`：syscalls不支持详细事件链输出（系统调用内部事件无法捕获）
- `untraced`属性：syscalls不支持辅助事件（实现限制）
- `-k, --key`：key关联字段，默认`-k common_pid`（按进程/线程统计）

**自动处理的选项**：
- `--impl syscalls`：自动设置，无需手动指定
- `--order`：根据attach方式自动决定是否启用

### 输出格式

#### 全局统计（无`--perins`）

```
          syscalls                calls        total(us)      min(us)      avg(us)      max(us)    err
------------------------- ------------ ---------------- ------------ ------------ ------------ ------
read(0)                          1234         5678.901        0.123        4.567       100.234     10
```

#### 进程/线程统计（`--perins`）

```
thread comm             syscalls                calls        total(us)      min(us)      avg(us)      max(us)    err
------ ---------- ------------------------- ------------ ---------------- ------------ ------------ ------------ ------
1234   java       read(0)                          123          456.789        0.123        3.712        50.123      5
```

**字段说明**：
- **thread**：进程/线程ID（仅`--perins`时显示）
- **comm**：进程名（仅`--perins`时显示）
- **syscalls**：系统调用名(系统调用编号)
  - 格式：`syscall_name(id)`
  - 系统调用名通过syscall表查找（x86_64、ARM等平台各不相同）
- **calls**：系统调用总次数（sys_enter→sys_exit配对成功的次数）
- **total(us)**：系统调用总耗时（微秒）
- **min/avg/max(us)**：系统调用耗时的最小值、平均值、最大值（微秒）
- **err**：系统调用出错次数（`sys_exit.ret < 0`的次数）

**行索引**：
| `--perins`选项 | 行索引 | 输出行数 | 说明 |
|---------------|--------|---------|------|
| **使用** | (common_pid, sys_enter.id) | 多行 | 每个(线程, 系统调用)一行 |
| **未使用** | (0, sys_enter.id) | 少行 | 每个系统调用一行，合并所有线程 |

### 使用示例

#### 示例1：全局系统调用统计

```bash
# 统计所有进程的系统调用性能
perf-prof syscalls -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit -i 1000

# 只统计进程1的系统调用
perf-prof syscalls -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit -p 1 -i 1000

# 按进程统计系统调用性能
perf-prof syscalls -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit \
                   -p 1 -i 1000 --perins

# 找出耗时超过1ms的系统调用
perf-prof syscalls -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit \
                   -p 1 -i 1000 --perins --than 1ms

# 只分析read系统调用（x86_64: id=0）
perf-prof syscalls -e 'raw_syscalls:sys_enter/id==0/' \
                   -e 'raw_syscalls:sys_exit/id==0/' \
                   -i 1000

# 只分析文件操作相关系统调用（read、write、open、close）
perf-prof syscalls -e 'raw_syscalls:sys_enter/id>=0&&id<=3/' \
                   -e 'raw_syscalls:sys_exit/id>=0&&id<=3/' \
                   -i 1000
```

### 与multi-trace的区别

| 特性 | syscalls | multi-trace |
|------|----------|------------|
| **事件配置** | 固定使用`raw_syscalls:sys_enter`和`sys_exit` | 自由指定任意事件 |
| **实现类型** | 固定使用`--impl syscalls` | 支持delay/pair/kmemprof等多种实现 |
| **Key关联** | 强制要求`common_pid`作为key | 支持任意表达式作为key |
| **系统调用分类** | 自动按系统调用类型分组 | 不支持系统调用分类 |
| **错误检测** | 自动统计系统调用错误（ret<0） | 不支持错误检测 |
| **进程退出处理** | 自动监听进程退出事件，清理未完成的系统调用 | 无此机制 |
| **详细输出** | 不支持`--detail`（系统调用内部事件无法捕获） | 支持`--detail`详细事件链 |
| **辅助事件** | 不支持`untraced`属性事件 | 支持`untraced`属性事件 |
| **适用场景** | 专用于系统调用性能分析 | 通用的多事件关系分析 |
| **输出格式** | 特化的系统调用统计格式 | 通用的延迟统计格式 |


### 限制和注意事项

#### 1. 不支持的功能

| 功能 | 是否支持 | 原因 |
|------|---------|------|
| `--detail` | ❌ 不支持 | 系统调用内部事件无法通过tracepoint捕获 |
| `-k, --key` | ❌ 不支持 | 默认 `-k common_pid` |
| `untraced`属性 | ❌ 不支持 | syscalls实现不需要辅助事件 |
| 自定义key表达式 | ⚠️ 不推荐 | 会导致系统调用分类统计失效 |
| 多个事件对 | ❌ 不支持 | syscalls只处理sys_enter→sys_exit关系 |

#### 2. 平台差异

**系统调用编号差异**：
| 平台 | 系统调用表 | 示例 |
|------|----------|------|
| x86_64 | arch/x86/entry/syscalls/syscall_64.tbl | read=0, write=1, open=2 |
| ARM64 | arch/arm64/include/asm/unistd.h | read=63, write=64, openat=56 |
| RISC-V | arch/riscv/include/asm/unistd.h | read=63, write=64, openat=56 |

**兼容性处理**：
- syscalls自动识别当前平台，使用正确的syscall表
- 如果找不到系统调用名，只显示编号：`123`

#### 3. 性能注意事项

**高频系统调用的影响**：
- 某些系统调用频率极高（如read、write），可能导致：
  - ringbuffer满，丢失事件
  - backup红黑树过大，影响性能
  - 内存占用增加

**优化策略**：
1. 使用filter过滤高频系统调用：`/id!=0/`（排除read）
2. 增大缓冲区：`-m 256`或更大
3. 缩小监控范围：`-p <pid>`
4. 使用阈值过滤：`--than 1ms`（只关注慢调用）

### 相关资源

- [multi-trace核心文档](#multi-trace---多事件关系分析)
- [系统调用延迟分析示例](#场景4系统调用延迟分析)
- [事件过滤语法参考](../Event_filtering.md)
- [延迟分析方法论](#分析方法论)


## kmemprof - 内存分配分析

kmemprof是multi-trace的特化版本，专用于分析内存分配的生命周期，统计内存分配/释放的字节数和堆栈信息。

### 概述

kmemprof分析内存分配事件到内存释放事件的完整生命周期，统计分配/释放的字节数，并输出分配/释放最多的前N个堆栈。

**核心功能**：
- **分配统计**：统计内存分配的总字节数和对象数
- **释放统计**：统计内存释放的总字节数和对象数
- **堆栈分析**：输出分配/释放最多字节的前N个堆栈
- **生命周期追踪**：基于ptr（指针）关联分配和释放事件

### 基础用法

```bash
perf-prof kmemprof [OPTION...] -e alloc -e free [-k str]
```

**与multi-trace的关系**：
- kmemprof基于multi-trace实现，固定使用`--impl kmemprof`
- 第一个`-e`指定内存分配事件（必须指定`size`属性，建议指定`stack`属性）
- 第二个`-e`指定内存释放事件（建议指定`stack`属性）
- 利用multi-trace的两事件关系处理能力，实现内存分配和释放事件的配对
- 需要手动指定`-k`或为事件设置`key`属性（通常使用ptr作为key）

### 核心特性

#### 1. 事件配置

**内存分配事件（第一个 `-e` 指定）**：
- 支持任意产生内存分配的事件（如`kmem:kmalloc`、`kmem:kmalloc_node`、`kmem:mm_page_alloc`等）
- **必须属性**：
  - `size=EXPR`：指定内存分配大小的计算表达式
- **建议属性**：
  - `stack`：启用调用栈采样，用于输出分配最多的堆栈
  - `key=EXPR`：指定关联键（通常是ptr字段）

**内存释放事件（第二个 `-e` 指定）**：
- 支持任意产生内存释放的事件（如`kmem:kfree`、`kmem:mm_page_free`等）
- **建议属性**：
  - `stack`：启用调用栈采样，用于输出释放最多的堆栈
  - `key=EXPR`：指定关联键（通常是ptr字段，需与分配事件一致）

**多事件支持**：
- 同一个`-e`选项内可以用逗号分隔多个事件，用于处理多种分配/释放路径
- 例如：`kmem:kmalloc,kmem:kmalloc_node`（同时监控两种分配方式）

#### 2. Key关联

kmemprof使用key来关联分配和释放事件：

| 场景 | Key设置 | 示例 |
|------|--------|------|
| **内核slab分配** | `-k ptr` 或 `key=ptr` | 使用kmalloc返回的指针作为key |
| **页面分配** | `key=pfn` | 使用pfn页框作为key（低版本内核不同） |
| **自定义分配器** | `key=EXPR` | 根据分配器特性设置 |

**注意**：分配事件和释放事件的key表达式可以不同，但计算结果必须相同。

#### 3. 输出格式

kmemprof的输出格式由`mem_profile_print()`函数实现：

**周期性输出**：
```
时间戳

alloc_event => free_event
alloc_event total alloc N bytes on M objects
Allocate X (Y%) bytes on Z (W%) objects:
    [调用栈1]
Allocate X (Y%) bytes on Z (W%) objects:
    [调用栈2]
...
Skipping alloc numbered N..M (如果堆栈数超过first_n)

free_event total free N bytes on M objects
Free X (Y%) bytes on Z (W%) objects:
    [调用栈1]
Free X (Y%) bytes on Z (W%) objects:
    [调用栈2]
...
Skipping free numbered N..M (如果堆栈数超过first_n)
```

**字段说明**：
- **alloc_event => free_event**：分配事件和释放事件的名称
- **total alloc N bytes on M objects**：总分配字节数和对象数
- **Allocate X (Y%) bytes on Z (W%) objects**：该堆栈分配的字节数(占比)和对象数(占比)
- **total free N bytes on M objects**：总释放字节数和对象数
- **Free X (Y%) bytes on Z (W%) objects**：该堆栈释放的字节数(占比)和对象数(占比)
- **调用栈**：内存分配/释放的调用栈（需要`stack`属性）
- **Skipping ... numbered N..M**：跳过的堆栈序号范围（默认只输出前10个）

**未配对事件输出**：
当程序退出或周期结束时，如果有分配但未释放的事件，也会输出：
```
alloc_event total alloc N bytes on M objects but not freed
```

#### 4. 统计指标

| 指标 | 含义 | 用途 |
|------|------|------|
| **alloc_bytes** | 总分配字节数 | 评估内存分配量 |
| **nr_alloc** | 分配对象数 | 评估分配频率 |
| **free_bytes** | 总释放字节数 | 评估内存释放量 |
| **nr_free** | 释放对象数 | 评估释放频率 |
| **堆栈占比** | 该堆栈的字节数/总字节数 | 识别热点分配路径 |

### 选项参数

**核心选项**：
- `-e, --event`：事件选择器
  - 第一个`-e`：内存分配事件（必须指定`size`属性）
  - 第二个`-e`：内存释放事件
- `-k, --key <str>`：系列事件的关联键（通常使用`ptr`）
- `-i, --interval <ms>`：输出间隔（毫秒）
- `--order`：启用事件排序（跨CPU关联时必需）
- `-m, --mmap-pages <pages>`：环形缓冲区大小（高频事件需要增大）

**Attach选项**：
- `-C, --cpus <cpu,...>`：监控指定CPU
- `-p, --pids <pid,...>`：附加到进程
- `-t, --tids <tid,...>`：附加到线程

**过滤选项**：
- `--user-callchain[=dwarf[,size]]`：包含用户态调用栈。`=dwarf` 启用DWARF栈回溯，可选指定栈数据大小
- `--kernel-callchain`：包含内核态调用栈
- `--python-callchain`：包含Python调用栈

### 使用示例

#### 示例1：内核slab内存分析

```bash
# 分析kmalloc/kfree的内存分配
perf-prof kmemprof -e 'kmem:kmalloc//size=bytes_alloc/stack/' -e kmem:kfree \
    -m 128 --order -k ptr

# 同时监控kmalloc和kmalloc_node
perf-prof kmemprof \
    -e 'kmem:kmalloc//size=bytes_alloc/stack/,kmem:kmalloc_node//size=bytes_alloc/stack/' \
    -e kmem:kfree \
    --order -k ptr
```

**参数说明**：
- `size=bytes_alloc`：使用kmalloc事件的bytes_alloc字段作为分配大小
- `stack`：启用调用栈采样
- `-k ptr`：使用ptr字段作为关联键
- `--order`：启用事件排序（kmalloc可能跨CPU）
- `-m 128`：增大缓冲区（内存分配事件频率较高）

#### 示例2：页面分配分析

```bash
# 分析页面分配/释放
perf-prof kmemprof \
    -e 'kmem:mm_page_alloc//size=4096<<order/key=pfn/stack/' \
    -e 'kmem:mm_page_free//key=pfn/stack/' \
    -m 256 --order
```

**参数说明**：
- `size=4096<<order`：计算页面大小（4096 * 2^order）
- `key=pfn`：使用pfn页框作为关联键
- `-m 256`：页面分配频率较高，需要更大缓冲区

#### 示例3：特定模块内存分析

```bash
# 只分析特定调用路径的内存分配（新内核支持call_site.function过滤器）
perf-prof kmemprof \
    -e 'kmem:kmalloc/call_site.function==__kmalloc_cache_noprof/size=bytes_alloc/stack/' \
    -e kmem:kfree \
    -m 128 --order -k ptr -i 5000

# 只分析大于1KB的分配
perf-prof kmemprof \
    -e 'kmem:kmalloc/bytes_alloc>1024/size=bytes_alloc/stack/' \
    -e kmem:kfree \
    -m 128 --order -k ptr
```

### 与multi-trace的区别

| 特性 | kmemprof | multi-trace |
|------|----------|------------|
| **事件配置** | 专用于alloc/free事件对 | 支持任意事件组合 |
| **实现类型** | 固定使用`--impl kmemprof` | 支持delay/pair等多种实现 |
| **输出格式** | 内存分配专用统计格式 | 通用的延迟统计格式 |
| **堆栈分析** | 按堆栈聚合字节数，输出top N | 需要手动配置 |
| **size属性** | 必须指定（用于计算分配大小） | 可选 |
| **适用场景** | 内存分配热点分析 | 通用的多事件关系分析 |

### 与kmemleak的区别

| 特性 | kmemprof | kmemleak |
|------|----------|----------|
| **分析目标** | 内存分配热点分析 | 内存泄漏检测 |
| **输出内容** | 周期性输出分配/释放统计 | 只输出未释放的内存 |
| **堆栈输出** | top N分配最多的堆栈 | 未释放内存的分配堆栈 |
| **适用场景** | 了解内存分配模式、热点路径 | 检测内存泄漏 |

### 技术要点

1. **size属性必需**：分配事件必须指定`size`属性，用于计算分配的字节数
2. **stack属性建议**：启用`stack`属性才能输出分配/释放的调用栈
3. **key一致性**：分配和释放事件的key计算结果必须相同才能正确配对
4. **排序需求**：跨CPU的内存分配需要`--order`保证时序正确
5. **缓冲区大小**：内存分配事件频率较高，建议使用`-m 128`或更大
6. **堆栈聚合**：按调用栈聚合分配字节数，输出top N（默认10个）

### 相关资源

- [multi-trace核心文档](#multi-trace---多事件关系分析)
- [kmemleak内存泄漏检测](kmemleak.md)
- [内存分配生命周期跟踪示例](#场景3内存分配生命周期跟踪)
- [事件过滤语法参考](../Event_filtering.md)
