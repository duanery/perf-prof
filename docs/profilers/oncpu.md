# oncpu - CPU运行进程监控
实时监控在CPU上运行的进程及其运行时间统计。

## 概述
- **主要用途**: 监控每个CPU上运行的进程及其运行时间，分析CPU资源在不同进程之间的分配情况。支持两种监控模式：按CPU监控进程（tid-to-cpumap）和按进程监控CPU（thread-to-cpumap）。
- **适用场景**: CPU资源竞争分析、进程调度行为观察、实时任务监控、多线程负载均衡分析、CPU亲和性验证。
- **功能分类**: 内建事件类，CPU性能分析，状态监控
- **最低内核版本**: 需要支持perf_event和sched tracepoint
- **依赖库**: libtraceevent, libbpf（可选）
- **平台支持**: 所有支持perf_event的Linux架构
- **特殊限制**: 需要root权限
- **参与联合分析**: 不参与
- **核心技术**: tracepoint采样、红黑树聚合、按优先级过滤、运行时统计

## 基础用法
```bash
perf-prof oncpu [OPTION...] [--detail] [--filter filter] [--only-comm] [--prio n]
```

OPTION:
- `-C, --cpus <cpu[-cpu],...>`: 监控指定的CPU列表（默认监控所有CPU）
- `-p, --pids <pid,...>`: Attach到指定进程
- `-t, --tids <tid,...>`: Attach到指定线程
- `-i, --interval <ms>`: 统计输出间隔，默认为1000毫秒
- `-m, --mmap-pages <pages>`: mmap缓冲区页数，默认为4页
- `--cgroups <cgroup,...>`: Attach到cgroup，支持正则表达式

FILTER OPTION:
- `--filter <filter>`: 事件过滤器，适用于tracepoint事件

PROFILER OPTION:
- `--detail`: 输出更详细的信息，包括切换次数(sws)和最大运行时间(max_ms)
- `--only-comm`: 只显示进程名（comm），不显示线程ID（tid）
- `--prio <prio[-prio],...>`: 指定监控的优先级范围（实时调度和普通调度）

### 示例
```bash
# 监控指定进程的CPU使用情况
perf-prof oncpu -p 2347

# 监控指定CPU上运行的进程
perf-prof oncpu -C 0-3

# 监控实时优先级进程（1-99）
perf-prof oncpu --prio 1-99

# 详细监控，包含切换次数和最大运行时间
perf-prof oncpu -C 0-3 --detail

# 只显示进程名，不显示线程ID
perf-prof oncpu -C 0-3 --only-comm
```

## 核心原理

**基本定义**
- **sched_stat_runtime**: 调度器统计运行时事件，记录进程在CPU上的运行时间
- **sched_switch**: 进程切换事件，记录从prev进程切换到next进程
- **tid-to-cpumap模式**: Attach到线程时，使用sched_stat_runtime事件，统计线程在各个CPU上的运行时间
- **cpu-to-tidmap模式**: Attach到CPU时，使用sched_switch事件，统计CPU上各个进程的运行时间
- **runtime**: 进程在CPU上的运行时间（纳秒）
- **nr_run**: 进程运行的次数（调度次数）
- **prio**: 调度优先级，0-99为实时优先级，100-139为普通优先级（nice -20到19）

**数据模型**

**tid-to-cpumap模式（按线程监控）**:
```
线程 → 在多个CPU上运行 → 统计每个CPU的运行时间
sched_stat_runtime事件 → 红黑树聚合[thread][cpu] → 总运行时间统计
```

**cpu-to-tidmap模式（按CPU监控）**:
```
CPU → 运行多个进程 → 统计每个进程的运行时间
sched_switch事件 → 计算切换间隔 → 红黑树聚合[cpu][tid/comm] → 按运行时间排序
```

事件 → 过滤器 → 运行时统计 → 红黑树聚合 → 间隔输出

### 事件源

- **sample_type**: `PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW`
  - `PERF_SAMPLE_TID`: 采样线程ID
  - `PERF_SAMPLE_TIME`: 采样时间戳
  - `PERF_SAMPLE_CPU`: 采样CPU编号
  - `PERF_SAMPLE_PERIOD`: 采样周期（用于sched_stat_runtime的runtime字段）
  - `PERF_SAMPLE_RAW`: 原始事件数据

- **内建事件**:
  - **tid-to-cpumap模式**: `sched:sched_stat_runtime`
    - 关键字段: `comm`, `pid`, `runtime`, `vruntime`
    - 触发时机: 进程运行时间片结束或被抢占时
  - **cpu-to-tidmap模式**: `sched:sched_switch`
    - 关键字段: `prev_comm`, `prev_pid`, `prev_prio`, `next_comm`, `next_pid`, `next_prio`
    - 触发时机: 每次进程切换时

- **采样配置**:
  - `sample_period = 1`: 每个事件都采样
  - `disabled = 1`: 初始化时禁用，待过滤器设置完成后启用
  - `pinned = 1`: 固定到CPU，保证精确采样
  - `wakeup_watermark = (pages << 12) / 2`: 使用水位线唤醒，默认50%

#### 过滤器

**自动生成的优先级过滤器**（仅cpu-to-tidmap模式）:
- 使用`--prio`参数时，自动生成基于prev_prio和next_prio的过滤器
- 例如：`--prio 1-99` 生成：`(prev_prio>=1 && prev_prio<=99) || (next_prio>=1 && next_prio<=99)`

**用户自定义过滤器**:
- 使用`--filter`参数手动指定过滤条件
- 与`--prio`互斥，不能同时使用

**特殊过滤**:
- cpu-to-tidmap模式下，自动过滤掉swapper进程（tid=0）
- sched_stat_runtime模式下，过滤掉tid不匹配的异常事件（进程唤醒时的跨CPU统计）

### 事件处理

**tid-to-cpumap模式（按线程监控）**:
1. **直接统计**：从sched_stat_runtime事件的runtime字段直接读取运行时间
2. **红黑树索引**：使用`[thread_instance][cpu]`作为键，聚合同一线程在不同CPU上的运行时间
3. **SMT检测**（可选）：使用`--detail`时，检测CPU的超线程兄弟，统计co-running时间
4. **输出格式**：按线程分组，显示在各个CPU上的运行时间

**cpu-to-tidmap模式（按CPU监控）**:
1. **间接计算**：通过sched_switch事件的时间差计算进程运行时间
2. **状态跟踪**：记录每个CPU上最后一次切换的时间戳和进程ID
3. **运行时计算**：
   ```
   runtime = next_switch_time - prev_switch_time
   进程runtime = 从被调度到CPU，到被切换出CPU的时间间隔
   ```
4. **红黑树索引**：使用`[cpu_instance][tid/comm]`作为键，聚合同一进程的运行时间
5. **排序输出**：按运行时间从大到小排序，显示CPU上运行时间最长的进程

**异常处理**:
- **丢事件恢复**：cpu-to-tidmap模式下，丢事件后重置switch_time状态，从下次切换重新开始统计
- **跨CPU统计过滤**：tid-to-cpumap模式下，过滤掉进程唤醒时在其他CPU上触发的sched_stat_runtime事件（tid != runtime.pid）


### 状态统计

**统计维度**:
- **tid-to-cpumap模式**：
  - 每个线程的总运行时间
  - 线程在每个CPU上的运行时间
  - 可选：co-running时间和百分比
- **cpu-to-tidmap模式**：
  - 每个CPU上所有进程的总运行时间
  - 每个进程的运行时间、切换次数、最大连续运行时间

**信号处理**:
- 无特殊信号处理，使用默认行为

## 输出

### 输出格式

**tid-to-cpumap模式（-p/-t参数）**:
```
[时间戳]

THREAD COMM             SUM(ms) CPUS(ms)
------ ---------------- ------- ---------
2347   my_process       1234    0(123ms) 2(234ms) 4(567ms) 8(310ms)
```

- **表头含义**:
  - `THREAD`: 线程ID
  - `COMM`: 进程/线程名称
  - `SUM(ms)`: 在所有CPU上的总运行时间（毫秒）
  - `CPUS(ms)`: 在每个CPU上的运行时间，格式为`cpu(ms)`

**cpu-to-tidmap模式（-C参数）**:

不带`--detail`参数:
```
[时间戳]

CPU SUM(ms) COMM:TID(ms)
--- ------- --------------------------------------------
000 956     systemd:1(12.3) kworker/0:1(45.6) sshd:1234(898.1)
001 1024    nginx:5678(512.4) nginx:5679(511.6)
```

带`--detail`参数:
```
[时间戳]

CPU SUM(ms/sws) COMM:TID(ms/sws/max_ms)
--- ----------- ---------------------------------------------------------
000 956ms/142   systemd:1(12.3ms/23/1.2ms) kworker/0:1(45.6ms/67/5.3ms) sshd:1234(898.1ms/52/120.5ms)
001 1024ms/98   nginx:5678(512.4ms/49/35.2ms) nginx:5679(511.6ms/49/34.8ms)
```

- **表头含义**:
  - `CPU`: CPU编号
  - `SUM(ms)`: 该CPU在统计周期内所有进程的总运行时间（毫秒）
  - `SUM(ms/sws)`: 总运行时间/总切换次数（仅--detail时显示）
  - `COMM:TID(ms)`: 进程名:线程ID(运行时间)，按运行时间降序排列
  - `COMM:TID(ms/sws/max_ms)`: 运行时间/切换次数/最大连续运行时间（仅--detail时显示）
  - 使用`--only-comm`时，不显示`:TID`部分，只显示进程名

- **数据单位**:
  - 所有时间默认单位：毫秒(ms)
  - 实际存储单位：纳秒，显示时除以1000000转换

- **行索引**:
  - tid-to-cpumap模式：每行代表一个线程
  - cpu-to-tidmap模式：每行代表一个CPU

- **排序规则**:
  - tid-to-cpumap模式：按线程ID排序（红黑树自然顺序）
  - cpu-to-tidmap模式：按CPU编号排序，同一CPU内按运行时间降序排列

- **详细输出**: `--detail`参数控制
  - tid-to-cpumap模式：显示SMT兄弟CPU和co-running统计
  - cpu-to-tidmap模式：显示切换次数和最大连续运行时间

### 关键指标

- **SUM(ms) - 总运行时间**:
  - **计算方法**:
    - tid-to-cpumap: 所有CPU上的runtime之和
    - cpu-to-tidmap: 所有进程的runtime之和
  - **正常范围**: 接近统计周期（`--interval`参数）
  - **异常阈值**:
    - 远小于interval：CPU利用率低，可能空闲或大量时间在idle
    - 远大于interval：多核CPU，正常现象

- **COMM:TID(ms) - 进程运行时间**:
  - **计算方法**:
    - tid-to-cpumap: sched_stat_runtime.runtime累加
    - cpu-to-tidmap: sched_switch事件的时间差累加
  - **正常范围**: 取决于业务特征
  - **异常阈值**: 某个进程长期占用大部分CPU时间（如：>80%）

- **sws - 切换次数**（仅--detail）:
  - **计算方法**: 进程被调度到CPU的次数
  - **正常范围**: 取决于进程类型
    - IO密集型：高切换次数（>100/秒）
    - CPU密集型：低切换次数（<10/秒）
  - **异常阈值**:
    - 非常高（>1000/秒）：可能有锁竞争或频繁睡眠唤醒
    - 非常低（<1/秒）：可能长时间运行或被挂起

- **max_ms - 最大连续运行时间**（仅--detail）:
  - **计算方法**: 单次运行的最大时间片
  - **正常范围**:
    - 实时进程：可能很大（几十到几百毫秒）
    - 普通进程：通常<10ms（时间片大小）
  - **异常阈值**:
    - 普通进程max_ms > 100ms：可能禁用了抢占或在长时间内核态操作
    - 实时进程max_ms过小：可能被频繁抢占

## 分析方法

### 基础分析方法

1. **选择监控模式**：
   - **已知目标进程**：使用`-p <pid>`或`-t <tid>`，分析进程在各CPU上的分布
   - **已知问题CPU**：使用`-C <cpu-list>`，分析CPU上运行的所有进程
   - **全局监控**：不指定参数，监控所有CPU上的进程（cpu-to-tidmap模式）

2. **选择输出级别**：
   - **简单统计**：不加`--detail`，只看运行时间
   - **详细分析**：加`--detail`，查看切换次数和最大运行时间
   - **进程名聚合**：加`--only-comm`，按进程名聚合（不区分线程）

3. **设置统计间隔**：
   - 快速定位：`-i 1000`（1秒），快速发现异常
   - 精细分析：`-i 100`（100ms），捕捉瞬时变化
   - 长期监控：`-i 5000`（5秒），减少输出量

4. **分析输出**：
   - **tid-to-cpumap模式**：
     - 检查线程是否按预期分布到不同CPU（CPU亲和性验证）
   - **cpu-to-tidmap模式**：
     - 查看各CPU的负载均衡情况
     - 识别占用时间最多的进程（排在前面）
     - 通过切换次数判断进程的IO/CPU密集度

5. **使用过滤器**：
   - 只关注实时进程：`--prio 1-99`
   - 只关注普通进程：`--prio 100-139`
   - 自定义过滤：`--filter "prev_prio<120 || next_prio<120"`（优先级高于nice 0的进程）

### 数据驱动分析

- **不预设任何业务特征**：
  - 先使用`perf-prof oncpu -C 0-3 -i 1000`观察各CPU的负载分布
  - 使用`perf-prof oncpu -C 0-3 --detail -i 1000`查看切换次数，判断进程类型
  - 使用`perf-prof top -e sched:sched_switch//key=next_pid/`分析进程调度频率

- **完全基于实际数据**：
  - 根据观察到的进程分布，决定是否需要调整CPU亲和性
  - 根据切换次数判断是否有异常的频繁唤醒
  - 根据max_ms判断是否有调度延迟问题

## 应用示例

```bash
# 基础监控：监控指定进程在各CPU上的运行情况
perf-prof oncpu -p 2347 -i 1000

# 全局监控：监控所有CPU上运行的进程
perf-prof oncpu -C 0-15 -i 1000

# 实时任务监控：只监控实时优先级进程
perf-prof oncpu -C 0-3 --prio 1-99 -i 1000

# 详细监控：查看切换次数和最大运行时间
perf-prof oncpu -C 0-3 --detail -i 1000

# 进程名聚合：按进程名聚合，不区分线程
perf-prof oncpu -C 0-3 --only-comm -i 1000

# 多线程程序分析：监控多线程应用的CPU分布
perf-prof oncpu -p $(pidof nginx) -i 1000 --detail
```

### 高级技巧

```bash
# CPU亲和性验证：检查进程是否按预期绑定到CPU
perf-prof oncpu -p 2347 -i 1000
# 预期输出：只在绑定的CPU上有运行时间

# 负载均衡分析：查看多个CPU的负载分布
perf-prof oncpu -C 0-15 --only-comm -i 1000 | grep -E "CPU|nginx"
# 分析nginx进程在各CPU上的分布是否均衡

# 调度延迟分析：结合--detail查看最大连续运行时间
perf-prof oncpu -C 0-3 --detail -i 1000
# 筛选出max_ms > 50ms的异常情况

# 实时任务优先级分析：监控不同优先级的实时任务
# 终端1：高优先级（1-50）
perf-prof oncpu -C 0-3 --prio 1-50 -i 1000 --only-comm
# 终端2：低优先级（51-99）
perf-prof oncpu -C 0-3 --prio 51-99 -i 1000 --only-comm

# 长时间监控并记录：持续监控并保存到文件
perf-prof oncpu -C 0-7 --detail -i 5000 -o oncpu.log &
# 分析日志找出异常时段
grep "THREAD\|CPU" oncpu.log -A 5 | grep -B 1 -E "[0-9]{4,}"

# 结合系统命令综合分析
# 查看CPU利用率
mpstat -P ALL 1 &
# 查看进程CPU使用
top -d 1 -b -n 10 &
# 启动oncpu分析
perf-prof oncpu -C 0-3 --detail -i 1000

# 动态调整优先级并观察
# 终端1：监控
perf-prof oncpu -C 0 --detail -i 1000
# 终端2：修改进程优先级
chrt -p 50 <pid>  # 改为实时优先级50
# 在终端1观察变化

# 周期性监控：每小时记录一次
while true; do
    date >> oncpu_hourly.log
    timeout 60s perf-prof oncpu -C 0-7 --only-comm -i 1000 >> oncpu_hourly.log
    sleep 3600
done
```

### 性能优化

- **缓冲区大小**:
  - 默认4页通常足够，高负载系统可能需要增加
  - cpu-to-tidmap模式下，切换频繁时增加到8或16页：`-m 16`
  - tid-to-cpumap模式下，多核系统建议增加缓冲区：`-m 32`

- **采样开销**:
  - oncpu开销相对较低，每次调度/运行时采样一次
  - cpu-to-tidmap模式（sched_switch）开销略高于tid-to-cpumap模式
  - 生产环境长期监控建议interval设置为5秒以上

- **过滤器优化**:
  - 使用`--prio`过滤器在内核态执行，不增加用户态开销
  - 使用`--only-comm`减少红黑树节点数量，降低内存使用
  - 指定明确的CPU或进程，避免全局监控的高开销

### 参数调优

- **interval调优**:
  - **快速定位**：`-i 100`，每100ms输出一次，快速发现异常调度行为
  - **正常监控**：`-i 1000`，每秒输出一次，平衡输出量和及时性
  - **长期监控**：`-i 5000`或`-i 10000`，每5-10秒输出一次，减少日志量
  - **高精度分析**：`-i 50`，配合`--detail`参数，捕捉瞬时调度情况

- **监控范围优化**:
  - **已知进程**：使用`-p <pid>`精确监控，开销最小
  - **已知CPU**：使用`-C <cpu-list>`监控特定CPU，适合NUMA系统
  - **全局监控**：不指定参数，监控所有CPU，开销最大，仅排查阶段使用

- **输出级别优化**:
  - **生产环境**：不使用`--detail`，减少输出量和计算开销
  - **调试阶段**：使用`--detail`获取完整信息
  - **多线程应用**：使用`--only-comm`按进程名聚合，减少输出行数

### 组合使用

- **与profile配合**：
  ```bash
  # 第一步：使用oncpu找出CPU占用高的进程
  perf-prof oncpu -C 0-3 -i 1000 --only-comm

  # 第二步：使用profile分析该进程的热点函数
  perf-prof profile -p <pid> -F 997 -g --flame-graph cpu.folded
  ```

- **与task-state配合**：
  ```bash
  # oncpu显示进程运行时间
  perf-prof oncpu -p 2347 -i 1000 --detail

  # task-state显示进程状态分布（运行R/睡眠S/等待D）
  perf-prof task-state -p 2347 -i 1000

  # 综合分析：判断进程是CPU密集型还是IO密集型
  ```

- **与top分析器配合**：
  ```bash
  # oncpu统计运行时间
  perf-prof oncpu -C 0 -i 1000 --detail

  # top统计调度次数分布
  perf-prof top -e sched:sched_switch//key=next_pid/comm=comm/ -C 0 -i 1000

  # 对比分析：运行时间长但调度次数少=CPU密集型
  ```

- **多阶段分析**：
  ```bash
  # 阶段1：全局扫描 - 找出CPU利用率高的CPU
  mpstat -P ALL 1 10

  # 阶段2：CPU分析 - 找出该CPU上占用时间最多的进程
  perf-prof oncpu -C <cpu> -i 1000 --only-comm

  # 阶段3：进程分析 - 分析该进程在各CPU上的分布
  perf-prof oncpu -p <pid> -i 1000 --detail

  # 阶段4：根因分析 - 使用profile分析热点函数
  perf-prof profile -p <pid> -F 997 -g --flame-graph cpu.folded
  ```

- **与调度器配合分析**：
  ```bash
  # 查看实时进程的运行情况
  perf-prof oncpu -C 0-3 --prio 1-99 -i 1000 --detail

  # 同时跟踪调度延迟
  perf-prof rundelay -e sched:sched_wakeup,sched:sched_wakeup_new,sched:sched_switch//key=prev_pid/ \
                     -e sched:sched_switch//key=next_pid/ -k pid --order -i 1000 --than 1ms

  # 综合判断：实时任务是否有调度延迟问题
  ```

- **虚拟化环境分析**：
  ```bash
  # Host上：监控vCPU线程的物理CPU分布
  perf-prof oncpu -p $(pidof qemu-system-x86_64) -i 1000 --detail --only-comm

  # 配合kvm-exit分析vCPU退出延迟
  perf-prof kvm-exit -p $(pidof qemu-system-x86_64) -i 1000 --perins
  ```

## 相关资源
- [事件基础文档](../events/)
- [sample_type采样类型说明](../sample_type.md)
- [选项参数完整参考](../main_options.md)
- [实际案例分析](../examples/)
- [profile分析器文档](profile.md)
- [task-state分析器文档](task-state.md)
- [top分析器文档](top.md)
