# blktrace - 块设备IO延迟跟踪
跟踪块设备上的IO延迟，分析从IO请求创建到完成的整个生命周期。

## 概述
- **主要用途**: 跟踪和统计块设备上的IO延迟，精确测量IO请求在不同阶段的耗时。通过四个关键tracepoint（block_getrq、block_rq_insert、block_rq_issue、block_rq_complete）追踪IO请求的完整生命周期。
- **适用场景**: 磁盘IO性能分析、存储子系统瓶颈定位、IO延迟排查、异常慢IO诊断。
- **功能分类**: 内建事件类，I/O性能分析，延迟分析，状态监控
- **最低内核版本**: 需要支持perf_event和trace event
- **依赖库**: libtraceevent, libbpf（可选）
- **平台支持**: 所有支持perf_event的Linux架构
- **特殊限制**: 需要root权限，只能监控实际物理设备或分区
- **参与联合分析**: 不参与
- **核心技术**: tracepoint采样、基于sector的请求跟踪、红黑树索引、排序保证、事件丢失恢复

## 基础用法
```bash
perf-prof blktrace [OPTION...] -d device [--than ns]
```

OPTION:
- `-i, --interval <ms>`: 统计输出间隔，默认为1000毫秒
- `-m, --mmap-pages <pages>`: mmap缓冲区页数，默认为8页
- `--order`: 按时间戳排序事件（默认已启用）
- `--tsc`: 将perf时钟转换为tsc周期计数
- `--kvmclock <uuid>`: 转换为Guest的kvmclock时间
- `--clock-offset <n>`: 时钟偏移量，用于时间校准

FILTER OPTION:
- 内建过滤器：自动根据设备和分区范围生成过滤器（`dev==N [&& sector>=start && sector<=end]`）

PROFILER OPTION:
- `-d, --device <device>`: **必需**。指定要监控的块设备，格式为`/dev/sdx`或`/dev/nvmeXnYpZ`
- `--than <n>`: 设置延迟阈值，统计并高亮显示超过该值的请求。单位：s/ms/us/*ns（默认ns）

### 示例
```bash
# 基础用法：监控/dev/sda的IO延迟
perf-prof blktrace -d /dev/sda -i 1000

# 检测慢IO：统计超过10ms的IO请求
perf-prof blktrace -d /dev/sda -i 1000 --than 10ms

# 分区监控：监控特定分区的IO延迟
perf-prof blktrace -d /dev/sda1 -i 1000 --than 5ms

# 高精度监控：使用更大的缓冲区和详细输出
perf-prof blktrace -d /dev/nvme0n1 -i 500 -m 128 -v
```

## 核心原理

**基本定义**
- **block_getrq**: IO请求创建事件，标志着IO请求的开始
- **block_rq_insert**: IO请求插入队列事件，请求被加入调度队列
- **block_rq_issue**: IO请求下发事件，请求被下发到硬件驱动
- **block_rq_complete**: IO请求完成事件，硬件完成IO操作
- **sector**: 磁盘扇区号（512字节），用于唯一标识IO请求
- **nr_sector**: IO请求涉及的扇区数量，表示IO大小
- **dev**: 设备号，主设备号和次设备号的组合

**数据模型**
```
IO请求创建 → [跟踪] → 插入队列 → [排队延迟] → 下发硬件 → [设备延迟] → 完成
   ↓           ↓        ↓          ↓          ↓           ↓        ↓
getrq  →  红黑树索引 → insert  →  统计延迟  →  issue  →  统计延迟 → complete
```

事件 → 设备过滤器 → 排序 → 红黑树跟踪 → 延迟统计 → 间隔输出

### 事件源

- **sample_type**: `PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW`
  - `PERF_SAMPLE_TID`: 采样线程ID
  - `PERF_SAMPLE_TIME`: 采样时间戳
  - `PERF_SAMPLE_CPU`: 采样CPU编号
  - `PERF_SAMPLE_RAW`: 原始事件数据（包含dev、sector、nr_sector等字段）

- **内建事件**:
  - `block:block_getrq`: IO请求创建，字段：dev, sector, nr_sector
  - `block:block_rq_insert`: IO请求插入队列，字段：dev, sector, nr_sector
  - `block:block_rq_issue`: IO请求下发硬件，字段：dev, sector, nr_sector
  - `block:block_rq_complete`: IO请求完成，字段：dev, sector, nr_sector, bytes

- **采样配置**:
  - `sample_period = 1`: 每个事件都采样
  - `disabled = 1`: 初始化时禁用，待过滤器设置完成后启用
  - `pinned = 1`: 固定到CPU，保证精确采样

#### 过滤器

**自动生成的设备过滤器**：
- 整盘设备：`dev==N`
- 分区设备：`dev==N && sector>=start && sector<=end`

例如：
```bash
# /dev/sda：只过滤设备号
dev==2048

# /dev/sda1：同时过滤设备号和扇区范围
dev==2048 && sector>=2048 && sector<=20971519
```

**特殊请求过滤**：
- flush请求（sector == -1）会被自动跳过，因为flush请求没有扇区信息

### 事件处理

**请求跟踪机制**：
1. **红黑树索引**：使用`(dev, sector, nr_sector)`作为键，在红黑树中快速查找IO请求
2. **状态转换**：跟踪每个IO请求的状态：getrq → insert → issue → complete
3. **延迟计算**：计算相邻状态之间的时间差
4. **完整性检查**：检测丢失的中间状态，标记异常（LOST、BYPASS_INSERT、EXIST）

**扇区范围匹配算法**：
```c
// 两个请求有重叠时认为是同一个请求
if (rq->sector >= r->sector + r->nr_sector)
    return 1;  // rq在r之后
else if (rq->sector + rq->nr_sector <= r->sector)
    return -1; // rq在r之前
else
    return 0;  // 有重叠，是同一个请求
```

**依赖排序**：
- **默认启用**：`order = true`，保证事件按时间戳顺序处理
- **必要性**：多CPU采样时，事件可能乱序到达用户态，排序保证正确的状态转换

**丢事件恢复**：
1. **检测**：当接收到LOST事件时，记录丢失时间范围 `[lost_start, lost_end]`
2. **清理**：清空红黑树中的所有请求跟踪状态
3. **恢复**：等待时间超过`lost_end`后，重新开始正常跟踪
4. **保护**：丢失范围内的事件被标记为不安全，不参与统计

**异常状态标记**：
- **EXIST**: getrq事件重复出现，表示前一个请求未完成
- **BYPASS_INSERT**: 跳过insert状态，直接从getrq到issue
- **LOST**: 中间状态丢失，状态跳跃不连续
- **GREATER_THAN**: 延迟超过阈值，高亮显示

### 状态统计

**统计维度**：
- 三个延迟阶段：getrq→insert, insert→issue, issue→complete
- 每个阶段统计：请求数(n)、总耗时(sum)、最小值(min)、平均值(avg)、最大值(max)
- 可选统计：超过阈值的请求数(than)及其百分比

**信号处理**：
- 无特殊信号处理，使用默认行为

## 输出

### 输出格式

```
[时间戳]

       start => end              reqs       total(us)      min(us)      avg(us)      max(us)  than(reqs)
-------------- -------------- -------- ---------------- ------------ ------------ ------------ ------------
  block_getrq => block_rq_insert     142          512.345        0.123        3.608       45.234    5 ( 3%)
block_rq_insert => block_rq_issue     142         2145.678        1.234       15.112      234.567   12 ( 8%)
 block_rq_issue => block_rq_complete  142        45678.901       45.678      321.683    12345.678   45 (31%)
```

- **表头含义**:
  - `start => end`: IO请求的起始状态和结束状态
  - `reqs`: 统计周期内完成该阶段的请求数量
  - `total(us)`: 该阶段所有请求的总耗时（微秒或kcyc）
  - `min(us)`: 最小延迟
  - `avg(us)`: 平均延迟
  - `max(us)`: 最大延迟
  - `than(reqs)`: 超过阈值的请求数及百分比（仅--than参数时显示）

- **数据单位**:
  - 默认单位：微秒(us)
  - `--tsc`参数时：千周期(kcyc)
  - 实际存储单位：纳秒，显示时除以1000转换

- **行索引**: 三行分别对应IO生命周期的三个阶段：
  1. **getrq → insert**: 请求创建到插入队列的延迟（软件层面）
  2. **insert → issue**: 队列排队等待下发的延迟（调度层面）
  3. **issue → complete**: 硬件执行IO的延迟（硬件层面）

- **排序规则**: 输出顺序固定为IO请求的生命周期顺序，不支持自定义排序

- **详细输出**: `-v`参数时，实时打印每个事件的详细信息，包括：
  - 异常状态标记（EXIST、LOST、BYPASS_INSERT）
  - 超过阈值的请求（GREATER_THAN，红色高亮）
  - 事件的原始字段信息

### 关键指标

- **getrq→insert延迟（请求创建到插入队列）**:
  - **计算方法**: insert事件时间戳 - getrq事件时间戳
  - **正常范围**: < 10us（软件操作，通常很快）
  - **异常阈值**: > 100us（可能有锁竞争或内存分配延迟）

- **insert→issue延迟（队列排队延迟）**:
  - **计算方法**: issue事件时间戳 - insert事件时间戳
  - **正常范围**: < 1ms（取决于IO调度器和队列深度）
  - **异常阈值**: > 10ms（队列过长或调度策略不当）

- **issue→complete延迟（硬件执行延迟）**:
  - **计算方法**: complete事件时间戳 - issue事件时间戳
  - **正常范围**: 取决于设备类型
    - SSD/NVMe: < 1ms
    - HDD: 5-15ms（随机读写）
  - **异常阈值**:
    - SSD/NVMe: > 10ms
    - HDD: > 100ms（可能有硬件故障）

- **than百分比（超过阈值请求占比）**:
  - **计算方法**: (超过阈值的请求数 / 总请求数) × 100%
  - **正常范围**: < 1%（偶尔的慢IO可以接受）
  - **异常阈值**: > 5%（表示持续性能问题）

### 阈值建议

- **轻微异常**:
  - insert→issue > 5ms: 检查IO调度器配置，考虑调整队列深度
  - issue→complete > 设备正常延迟的3倍: 监控硬件健康状态
  - than百分比 1-5%: 增加监控频率，观察趋势

- **严重异常**:
  - insert→issue > 50ms: 立即检查系统负载，可能需要限流
  - issue→complete > 1s: 硬件故障风险，紧急检查设备状态
  - than百分比 > 10%: 存储子系统严重过载，需立即介入

- **正常范围**:
  - getrq→insert < 10us
  - insert→issue < 1ms（SSD）或 < 5ms（HDD）
  - issue→complete < 1ms（SSD）或 < 15ms（HDD）
  - than百分比 < 1%

## 分析方法

### 基础分析方法

1. **确定监控目标**：
   - 使用 `lsblk` 或 `df -h` 确定要监控的设备或分区
   - 区分整盘（/dev/sda）和分区（/dev/sda1）

2. **选择合适的间隔**：
   - 初步排查：`-i 1000`（1秒），快速定位问题时段
   - 详细分析：`-i 100`（100ms），捕捉瞬时波动
   - 长期监控：`-i 5000`（5秒），减少输出量

3. **设置延迟阈值**：
   - 先不设置阈值，观察正常的avg和max值
   - 根据业务需求设置合理阈值（例如：P99延迟的2倍）
   - 使用`--than`参数统计超过阈值的请求比例

4. **分析输出**：
   - 观察三个阶段的延迟分布，定位瓶颈在哪一层
   - getrq→insert高：软件层问题（锁竞争、内存分配）
   - insert→issue高：调度层问题（队列过长、调度策略）
   - issue→complete高：硬件层问题（设备性能、硬件故障）

5. **启用详细输出**：
   - 使用`-v`参数查看异常事件的详细信息
   - 关注GREATER_THAN标记的事件，分析慢IO的特征
   - 检查是否有LOST或BYPASS_INSERT等异常状态

### 数据驱动分析

- **不预设任何业务特征**：
  - 先使用`perf-prof blktrace -d /dev/sdX -i 1000`运行一段时间，观察延迟分布
  - 使用`perf-prof top -e block:block_rq_complete//top-by=bytes/`分析IO大小分布
  - 使用`perf-prof top -e block:block_rq_complete//key=rwbs/`分析读写比例

- **完全基于实际数据**：
  - 根据观察到的max值设置合理的`--than`阈值
  - 根据输出的请求数(reqs)判断是否需要调整缓冲区大小（`-m`参数）
  - 根据than百分比判断问题严重程度，决定下一步分析方向

## 应用示例

```bash
# 基础监控：每秒输出/dev/sda的IO延迟统计
perf-prof blktrace -d /dev/sda -i 1000

# 慢IO检测：监控超过10ms的IO请求
perf-prof blktrace -d /dev/sda -i 1000 --than 10ms

# 分区监控：只监控特定分区的IO
perf-prof blktrace -d /dev/sda1 -i 500 --than 5ms

# NVMe设备监控：使用更严格的阈值
perf-prof blktrace -d /dev/nvme0n1 -i 1000 --than 1ms

# 详细调试：实时查看所有慢IO事件
perf-prof blktrace -d /dev/sda -i 1000 --than 10ms -v

# 高频监控：捕捉瞬时波动
perf-prof blktrace -d /dev/sda -i 100 -m 128
```

### 高级技巧

```bash
# 使用TSC时钟提高精度（适用于性能测试）
perf-prof blktrace -d /dev/sda -i 1000 --than 10ms --tsc

# 长时间监控并记录到文件
perf-prof blktrace -d /dev/sda -i 5000 --than 50ms -o io_stat.log

# 多设备同时监控（使用多个终端）
# 终端1：
perf-prof blktrace -d /dev/sda -i 1000 --than 10ms
# 终端2：
perf-prof blktrace -d /dev/sdb -i 1000 --than 10ms

# 结合系统命令综合分析
# 查看设备IO统计
iostat -x 1 &
# 查看进程IO使用情况
iotop -o &
# 启动blktrace分析
perf-prof blktrace -d /dev/sda -i 1000 --than 10ms -v

# 捕捉特定时间段的慢IO（运行10秒后退出）
timeout 10s perf-prof blktrace -d /dev/sda -i 1000 --than 20ms -v > slow_io.log

# 分析虚拟化环境的Guest IO（需要在Guest内运行）
perf-prof blktrace -d /dev/vda -i 1000 --than 10ms --kvmclock <uuid>
```

### 性能优化

- **缓冲区大小**:
  - 默认8页通常足够，高IO负载下可能丢失事件
  - 监控时看到LOST事件时，增加到32或64页：`-m 64`
  - NVMe等高IOPS设备建议使用更大缓冲区：`-m 128`

- **采样开销**:
  - blktrace对每个IO请求采样4次，开销相对较高
  - 生产环境长期监控建议结合`iostat`等低开销工具
  - 仅在排查问题时启用blktrace，问题解决后及时关闭

- **过滤器优化**:
  - 监控分区比监控整盘更精确，过滤掉无关IO
  - 分区过滤器在内核态执行，不增加用户态开销
  - flush请求（sector=-1）被自动过滤，减少无用采样

### 参数调优

- **interval调优**:
  - **快速定位**：`-i 100`，每100ms输出一次，快速发现问题时段
  - **正常监控**：`-i 1000`，每秒输出一次，平衡输出量和及时性
  - **长期监控**：`-i 5000`，每5秒输出一次，减少日志量

- **than阈值优化**:
  - **初步探测**：不设置`--than`，观察max和avg值
  - **SSD设备**：`--than 5ms`（正常延迟的5-10倍）
  - **HDD设备**：`--than 50ms`（正常延迟的3-5倍）
  - **严格监控**：`--than 1ms`（捕捉所有异常延迟）
  - **业务驱动**：根据业务SLA设置（如：P99延迟的2倍）

### 组合使用

- **与iostat配合**：
  ```bash
  # 终端1：系统级IO统计
  iostat -x 1

  # 终端2：精确延迟分析
  perf-prof blktrace -d /dev/sda -i 1000 --than 10ms
  ```
  - iostat提供宏观视角（IOPS、带宽、%util）
  - blktrace提供微观视角（每个阶段的精确延迟）

- **与top分析器配合**：
  ```bash
  # 第一步：使用blktrace定位到有慢IO问题
  perf-prof blktrace -d /dev/sda -i 1000 --than 10ms

  # 第二步：分析具体的IO模式（读写、大小）
  perf-prof top -e block:block_rq_complete//key=rwbs/ -i 1000
  perf-prof top -e block:block_rq_complete//top-by=nr_sector/ -i 1000
  ```

- **多阶段分析**：
  ```bash
  # 阶段1：宏观分析 - 确定是否有IO性能问题
  perf-prof blktrace -d /dev/sda -i 5000

  # 阶段2：定位时段 - 找出问题发生的时间段
  perf-prof blktrace -d /dev/sda -i 1000 --than 20ms

  # 阶段3：详细分析 - 查看具体的慢IO事件
  perf-prof blktrace -d /dev/sda -i 1000 --than 20ms -v

  # 阶段4：根因分析 - 结合其他工具定位根本原因
  perf-prof top -e block:block_getrq//key=_pid/ -i 1000
  ```

- **与trace分析器配合**：
  ```bash
  # 追踪特定进程的IO调用栈
  perf-prof trace -e block:block_rq_issue//stack/ -p <pid> -g

  # 同时运行blktrace统计延迟
  perf-prof blktrace -d /dev/sda -i 1000 --than 10ms
  ```

## 相关资源
- [事件基础文档](../events/)
- [sample_type采样类型说明](../sample_type.md)
- [选项参数完整参考](../main_options.md)
- [实际案例分析](../examples/)
- [trace分析器文档](trace.md)
- [top分析器文档](top.md)
