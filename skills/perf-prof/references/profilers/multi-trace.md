# multi-trace - 多事件关系分析

基于key关联的多事件关系分析工具，通过时间排序和状态管理实现复杂事件链的延迟分析和配对跟踪。

## 概述
- **主要用途**: 将复杂的多事件关系转换为两两事件关系进行分析，支持延迟分析(delay)、事件配对(pair)分析模式
- **适用场景**: 进程调度延迟分析、软中断处理延迟、系统调用延迟、资源配对检查(open-close、alloc-free)、复杂事件因果关系链分析
- **功能分类**: 自定义事件类，延迟分析，联合分析
- **最低内核版本**: 3.10+ (支持trace events)
- **平台支持**: x86, ARM, RISC-V, PowerPC
- **特殊限制**:
  - 需要root权限运行
  - 依赖事件时间戳排序
- **参与联合分析**: 作为联合分析的主体，可以组合多个分析单元作为事件源

## 基础用法
```bash
perf-prof multi-trace [OPTION...] -e EVENT [-e ...] [-k EXPR] [--impl impl] [--than ns] [--detail] [--perins]
事件格式："event[/filter/key=EXPR/role=EXPR/untraced/trigger/alias=str/stack/max-stack=int/][,event...]"
```

### OPTION
- `--watermark <0-100>`: 默认50
- `-m, --mmap-pages <N>`: 默认64页
- `--order`: 根据场景动态决定是否启用（详见性能优化章节）

### FILTER OPTION
- `--user-callchain[=dwarf[,size]]`: 包含用户态调用栈。`=dwarf` 启用DWARF栈回溯
- `--kernel-callchain`: 包含内核态调用栈
- `--exclude-user`: 排除用户态事件
- `--exclude-kernel`: 排除内核态事件
- trace event过滤器: 在事件后使用`/filter/`语法

### PROFILER OPTION
- `-e, --event <EVENT,...>`: 指定事件，支持多种用法：
  - 单个事件：`-e sched:sched_wakeup`
  - 多个事件（同位置）：`-e 'sched:sched_wakeup,sched:sched_wakeup_new'`
  - 多位置事件：`-e event1 -e event2`（构建分析路径）
  - 复杂事件：`-e 'event/filter/key=EXPR/role=EXPR/stack/'`
- `-k, --key <str>`: 事件关联键，给未使用`key=EXPR`属性的事件提供默认key
- `--impl <impl>`: 分析实现类型
  - `delay`: 延迟分析（默认）
  - `pair`: 事件配对分析
- `--than <n>`: 超过指定的阈值输出，单位：s/ms/us/ns，不指定默认是ns
- `--only-than <ns>`: 只有在超过指定的阈值才输出，单位：s/ms/us/ns，不指定默认是ns
- `--lower <ns>`: 低于指定的阈值输出，单位：s/ms/us/ns，不指定默认是ns
- `--perins`: 每个实例统计
- `--heatmap <file>`: 指定输出延迟热图文件，file会自动加".lat"后缀
- `--detail[=<-N,+N,1,2,hide<N,same*>]`: 详细信息输出，依赖`--than`、`--lower`选项
  - `-N`: 在event1之前，打印N纳秒内的事件，N支持时间单位：s、ms、us、ns
  - `+N`: 在event2之后，打印N纳秒内的事件
  - `1`: 只显示与event1相同的事件
  - `2`: 只显示与event2相同的事件
  - `hide<N`: 隐藏小于N纳秒的事件间隔
  - `samecpu`: 只显示与event1或event2相同CPU的事件
  - `samepid`: 只显示与event1或event2相同PID的事件
  - `sametid`: 只显示与event1或event2相同TID的事件
  - `samekey`: 只显示与event1或event2相同key的事件
- `--cycle`: 环形事件模式，建立从最后一个`-e`回到第一个`-e`的关系

## 核心原理

### 数据模型
```
事件 → [排序] → 事件关联 → 两事件关系处理 → 统计输出
```

### 事件源
- **sample_type**: `PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW`
  - `PERF_SAMPLE_TIME`: 核心采样类型，用于计算事件间时间差
  - `PERF_SAMPLE_CALLCHAIN`: 可选，通过`stack`属性为事件启用堆栈采样
- **事件类型**:
  - tracepoint事件：`sys:name`格式
  - kprobe事件：`kprobe:func`格式
  - uprobe事件：`uprobe:func@"file"`格式
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

#### 2. 多事件关系：三种核心关系约束

**1) 因果关系**：事件按时间顺序发生
- 时序保证：有A,B,C才会有D,E；有D,E才会有F
- 命令行映射：多个`-e`选项的顺序定义了事件发生的因果顺序

**2) 选择关系**：事件集合内的互斥选择
- **互斥性**：A,B,C在同一个分析实例中只有一个会发生
- **完备性**：A,B,C共同构成了所有可能性的完整集合
- 命令行映射：单个`-e`选项内用逗号分隔的事件定义一个"事件可能性集合"

**3) 关联关系**：事件通过key值关联
- **关联机制**：相同key值的事件属于同一分析实例
- **实现方式**：通过backup红黑树按key索引，实现高效事件关联

#### 3. 两事件关系

本文档中使用`event1→event2`表示两个关联事件的关系，输出时使用`event1 => event2`表示。

**关系定义**：
- **因果关系**：有event1（前序事件）才会有event2（后续事件）
- **选择关系**：event1是A,B,C中的一个，event2是D,E中的一个
- **关联关系**：event1和event2通过相同的key值关联

**事件角色说明**：
- **event1（前序事件）**：触发者、原因、影响者
  - 行为：需要备份，等待后续事件 (need_backup=1)
- **event2（后续事件）**：接收者、结果、被影响者
  - 行为：需要查找前序事件 (need_find_prev=1)

### 过滤器层次
1. **trace event过滤器（内核态）**: `/filter/`语法，在内核态高效过滤
2. **用户态属性过滤**: key/role表达式在用户态计算

### 事件处理

#### 1. 核心数据结构

**两个红黑树**：

| 红黑树 | 索引方式 | 用途 | 特点 |
|--------|---------|------|------|
| timeline | 按时间戳排序 | 恢复事件顺序，保证因果关系 | 中序遍历获得时间有序序列 |
| backup | 按key值索引 | 临时存储前序事件，等待后续事件 | 快速查找相同key的事件 |

#### 2. 详细处理流程

**阶段1：事件收集与排序**

1. **事件收集**：从多个ringbuffer读取事件
2. **排序处理**：通过order堆排序，生成按时间排序的事件序列
3. **节点创建**：为每个事件分配timeline_node对象，提取key值，确定事件位置
4. **插入timeline**：将timeline_node按时间戳插入timeline红黑树

**阶段2：事件关联**

按时间顺序处理每个事件curr：
1. **查找前序事件**：使用curr.key在backup红黑树查找，返回prev
2. **执行两事件分析**：找到后执行`two(prev->event, curr->event)`两事件关系处理；从backup红黑树移除prev
3. **备份当前事件**：将curr加入backup红黑树

**阶段3：内存回收**

按时间顺序扫描，删除已处理的节点，释放内存

#### 3. 两事件关系处理

**实现1: delay - 延迟分析（默认）**

- **初始化**：为所有事件对建立公共延迟统计（latency_dist）
- **处理流程**：
  1. 根据(event1,event2)匹配事件对，获取two_id（唯一标识一个事件对）
  2. 计算延迟 `delta = event2.time - event1.time`
  3. 以(key, two_id)为键，将delta加入latency_dist
  4. 统计延迟分布：min/max/p50/p95/p99等分位数

- **延迟根因分析**：
  - `--than <n>`：输出延迟超过阈值的event1和event2
  - `--detail`：详细输出event1→event2之间的相关事件
  - `untraced`属性事件：用于还原event1→event2的中间细节

- **选项参数**：
  - `--perins`：统计(key, two_id)的延迟分布
    - 未指定：按(0, two_id)统计，所有实例合并

**实现2: pair - 配对分析**

分析event1和event2是否成对出现，用于检测资源泄漏（如：open-close、alloc-free配对检查）

#### 4. 性能优化策略

**何时需要--order**：
- **必须使用场景**：多个ringbuffer且需要跨ringbuffer进行事件关联
- **可以省略场景**：
  1. 单ringbuffer场景（`-C`指定单CPU / `-t`指定单线程）
  2. 同时满足：未指定`-k`且所有事件无key属性、未指定`--detail`

**内存优化**：
- 有`--detail`：使用timeline红黑树保存所有事件，内存占用高
- 无`--detail`：流式处理，只保存未配对前序事件，内存占用低

**Key选择策略**：

| Key类型 | 设置方式 | 适用场景 |
|---------|---------|---------|
| **CPU关联** | 默认 | 分析CPU维度的事件关系 |
| **进程/线程关联** | `-p pid` / `-k common_pid` | 分析进程/线程维度 |
| **自定义字段关联** | `-k EXPR` / `key=EXPR` | 复杂关联场景 |

## 输出

### 输出格式

#### delay - 延迟分析（默认）

**标准输出格式**：
```
[键名] [comm]    start => end      calls   total(us)      min(us)      p50(us)      p95(us)      p99(us)      max(us)
[键值] [进程名]  event1 => event2  统计数据...
```

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
```

#### pair - 配对分析

输出未配对的event1（缺少event2）或event2（缺少event1），用于检测资源泄漏。

### 输出字段
| 字段 | 说明 |
|------|------|
| 键名 | 仅`--perins`时显示，根据key来源确定（EXPR/THREAD/CPU） |
| 键值 | 仅`--perins`时显示，具体的key值 |
| comm | 仅`--perins`且`-p`或`-t`时显示，进程名 |
| start | event1事件名或alias |
| end | event2事件名或alias |
| calls | 配对成功的事件数 |
| total(us) | 总延迟时间(微秒) |
| min/p50/p95/p99/max(us) | 延迟分位数统计(微秒) |

### 关键指标

| 指标 | 计算方法 | 异常阈值 |
|------|----------|----------|
| p50 | 50%的延迟在此之下 | 取决于具体场景 |
| p95 | 95%的延迟在此之下 | 超过预期2-3倍需关注 |
| p99 | 99%的延迟在此之下 | 长尾延迟问题指标 |
| max | 最大延迟 | 远大于p99需深入分析 |

## 应用示例

### 基础示例
```bash
# 1. 软中断处理延迟分析
perf-prof multi-trace -e irq:softirq_entry -e irq:softirq_exit -i 1000
perf-prof multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ -i 1000  # 过滤NET_TX

# 2. 进程调度延迟分析
perf-prof multi-trace \
    -e 'sched:sched_wakeup,sched:sched_wakeup_new,sched:sched_switch/prev_state==0&&prev_pid>0/key=prev_pid/' \
    -e 'sched:sched_switch//key=next_pid/' \
    -k pid --order -i 1000

# 3. 事件配对分析（检测资源泄漏）
perf-prof multi-trace -e irq:softirq_entry/vec==1/ -e irq:softirq_exit/vec==1/ --impl pair -i 1000
```

### 高级技巧
```bash
# 使用role属性动态决定事件角色
perf-prof multi-trace -e sched:sched_switch//role="(next_pid?1:0)|(prev_pid?2:0)"/ --cycle -i 1000

# 使用untraced属性添加辅助事件
perf-prof multi-trace \
    -e 'sched:sched_wakeup/comm~"java"/key=pid/' \
    -e 'sched:sched_switch/next_comm~"java"/key=next_pid/,sched:sched_migrate_task/comm~"java"/untraced/stack/' \
    --order -i 1000 --than 10ms --detail
```

### 性能优化
```bash
# 单CPU场景，无需--order
perf-prof multi-trace -e irq:softirq_entry -e irq:softirq_exit -C 0 -i 1000

# 高频事件，增大缓冲区
perf-prof multi-trace -e sched:sched_switch -e sched:sched_wakeup -m 128 --order -i 1000
```

### 组合使用
```bash
# 与profile联合分析：追加采样判断是否有长循环
perf-prof multi-trace \
    -e 'sched:sched_wakeup,sched:sched_switch/prev_state==0/key=prev_pid/' \
    -e 'sched:sched_switch//key=next_pid/,profile/-F 500 -g/untraced/' \
    -k pid --order -i 1000 --than 100ms --detail=samecpu
```

## 分析方法论

### 标准分析流程

**第1步：确定分析目标和事件选择**

| 分析目标 | 推荐事件组合 | 选项参数 |
|---------|-------------|---------|
| **延迟分析** | 起点事件 + [中间事件] + 终点事件 | `--impl delay`或不指定 |
| **配对分析** | 打开事件 + 关闭事件 | `--impl pair` |

**第2步：设计key关联表达式**

| 关联维度 | Key设计 | 示例 |
|---------|--------|------|
| **CPU维度** | 默认 | - |
| **进程/线程维度** | `-k common_pid` | 跟踪进程事件链 |
| **单维度** | `-k 事件字段` | `pid`、`ptr` |
| **复合维度** | `-k 表达式` | `(ctrl_id<<48)+cid` |

**第3步：配置选项参数**

1. `--order` 决策是否需要排序
2. `--perins` 决策是否按实例统计
3. `--cycle` 决策是否需要环形事件模式

**第4步：运行并解读结果**

1. **配对率检查**：calls数量是否合理？
2. **延迟分布检查**：p50、p95、p99是否正常？
3. **异常值检查**：max是否远大于p99？

**第5步：深入分析根因**

1. 由p99，max等数值，决定`--than`参数
2. 决策`--detail`如何筛选中间事件
3. 决策是否添加更多`untraced`事件

## 派生分析器

multi-trace提供了多个专用的派生分析器，针对特定场景进行了预配置优化：

| 派生分析器 | 用途 | 详细文档 |
|-----------|------|----------|
| [rundelay](rundelay.md) | 调度延迟分析 | 预配置sched事件，自动处理key和排序 |
| [syscalls](syscalls.md) | 系统调用延迟分析 | 预配置sys_enter/sys_exit，按系统调用分类统计 |
| [kmemprof](kmemprof.md) | 内存分配分析 | 预配置alloc/free，按堆栈聚合统计 |

## 相关资源
- [事件过滤语法参考](Event_filtering.md)
- [表达式语法参考](expr.md)
- [rundelay - 调度延迟分析](rundelay.md)
- [syscalls - 系统调用分析](syscalls.md)
- [kmemprof - 内存分配分析](kmemprof.md)
