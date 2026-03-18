# rundelay - 调度延迟分析

multi-trace的特化版本，预配置了调度相关事件和过滤器，专用于分析进程调度延迟。

## 概述
- **主要用途**: 分析从进程唤醒到开始运行的延迟，以及进程被抢占后再次运行的延迟
- **适用场景**: 进程调度延迟分析、调度器性能评估、调度延迟问题诊断
- **功能分类**: 自定义事件类，延迟分析，multi-trace派生
- **最低内核版本**: 3.10+ (支持sched tracepoints)
- **平台支持**: x86, ARM, RISC-V, PowerPC
- **特殊限制**:
  - 需要root权限运行
  - sched事件不适合绑定到特定线程，使用`-p`/`-t`时自动切换到全局CPU模式
- **参与联合分析**: 支持添加`untraced`属性事件进行联合分析

## 基础用法
```bash
perf-prof rundelay [OPTION...] -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch [--filter comm] [--than ns] [--detail] [--perins]
```

### OPTION
- `--watermark <0-100>`: 默认50
- `-m, --mmap-pages <N>`: 默认64页，高频场景建议128或更大
- `--order`: 自动启用，无需手动指定

### FILTER OPTION
- `--user-callchain[=dwarf[,size]]`: 包含用户态调用栈。`=dwarf` 启用DWARF栈回溯
- `--kernel-callchain`: 包含内核态调用栈
- `--filter <comm>`: 过滤进程名，支持通配符，自动应用到所有事件
- `-p, --pids <pid,...>`: 附加到进程，自动过滤线程id
- `-t, --tids <tid,...>`: 附加到线程，自动过滤线程id

### PROFILER OPTION
- `-e, --event`: 事件选择器
  - 第一个`-e`: 起点事件，使用`sched:sched_wakeup*,sched:sched_switch`
  - 第二个`-e`: 终点事件，使用`sched:sched_switch`
  - 任何一个`-e`都支持添加`untraced`属性事件
- `--than <ns>`: 延迟阈值，只输出超过阈值的情况，单位：s/ms/us/ns
- `--detail[=<samecpu,samepid,sametid,samekey>]`: 详细输出模式，显示事件链
- `--perins`: 按实例（进程）统计延迟分布
- `--heatmap <file>`: 生成延迟热图文件

## 核心原理

### 数据模型
```
唤醒/抢占事件 → [排序] → key关联(pid) → 延迟统计 → 输出
```

### 事件源

**起点事件（第一个`-e`，使用通配符自动匹配）**：
- `sched:sched_wakeup`: 进程被其他进程唤醒，自动设置key=pid
- `sched:sched_wakeup_new`: 新创建的进程被唤醒，自动设置key=pid
- `sched:sched_switch`: 进程Running态被抢占，自动设置key=prev_pid
  - 自动过滤器：`prev_state==0 && prev_pid>0`（筛选Running态，排除swapper）

**终点事件（第二个`-e`）**：
- `sched:sched_switch`: 进程开始运行，自动设置key=next_pid

### 两类延迟分析

| 延迟类型 | 起点事件 | 终点事件 | 含义 |
|---------|---------|---------|------|
| **唤醒延迟** | sched_wakeup/sched_wakeup_new | sched_switch(next) | 从唤醒到开始运行 |
| **抢占延迟** | sched_switch(prev) | sched_switch(next) | 从被抢占到再次运行 |

### 过滤器层次
1. **`--filter`选项（内核态）**:
  - 起点事件：`sched:sched_wakeup*`自动转换为comm字段过滤器；`sched:sched_switch`自动转换为prev_comm过滤器
  - 终点事件：`sched:sched_switch`自动转换为next_comm过滤器
2. **`-p`/`-t`选项（内核态）**:
  - 起点事件：`sched:sched_wakeup*`自动转换为pid字段过滤器；`sched:sched_switch`自动转换为prev_pid过滤器
  - 终点事件：`sched:sched_switch`自动转换为next_pid过滤器

### 事件处理

**自动处理机制**：
- **Key关联**: 自动设置`-k pid`，无需手动指定
- **排序**: 自动启用`--order`，保证跨CPU事件的时序正确
- **过滤器**: 自动为不同事件设置正确的字段名

**内核版本兼容**：

| 内核版本 | prev_state值 | 说明 |
|---------|-------------|------|
| >= 4.14 | `TASK_REPORT_MAX` (0x100) | 新版本使用特殊标记 |
| < 4.14 | `0` | 旧版本使用0表示Running态 |

## 输出

### 输出格式

**标准输出（无`--perins`）**：
```
        start => end             calls        total(us)      min(us)      p50(us)      p95(us)      p99(us)      max(us)
-------------    ------------ -------- ---------------- ------------ ------------ ------------ ------------ ------------
sched_wakeup => sched_switch      1234         5678.901        0.123        2.345        10.234       25.678       100.234
```

**实例统计（`--perins`）**：
```
THREAD  comm         start => end             calls        total(us)      min(us)      p50(us)      p95(us)      p99(us)      max(us)
------ ---------- -------------    ------------ -------- ---------------- ------------ ------------ ------------ ------------ ------------
1234   java       sched_wakeup => sched_switch      100          234.567        0.123        1.234        5.678        12.345       50.123
```

### 输出字段
| 字段 | 说明 |
|------|------|
| THREAD | 进程/线程ID（仅`--perins`时显示） |
| comm | 进程名（仅`--perins`时显示） |
| start | 起点事件类型（sched_wakeup、sched_wakeup_new或sched_switch） |
| end | 终点事件（sched_switch） |
| calls | 配对成功的事件数 |
| total(us) | 总延迟时间（微秒） |
| min/p50/p95/p99/max(us) | 延迟分位数统计（微秒） |

### 关键指标

| 指标 | 计算方法 | 异常阈值 |
|------|----------|----------|
| p50 | 50%的延迟在此之下 | >1ms需关注 |
| p95 | 95%的延迟在此之下 | >10ms需关注 |
| p99 | 99%的延迟在此之下 | >50ms需关注 |
| max | 最大延迟 | >100ms需深入分析 |

## 应用示例

### 基础示例
```bash
# 1. 全局统计所有进程的调度延迟
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch -i 1000

# 2. 按进程统计调度延迟分布
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch -i 1000 --perins

# 3. 分析特定进程的调度延迟
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch -p 1234 -i 1000 --than 4ms
```

### 高级技巧
```bash
# 使用进程名过滤
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch --filter python -i 1000 --than 4ms

# 通配符匹配多个进程
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch --filter "java,pyth*" -i 1000

# 详细输出，只显示相同key的事件
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch --filter python \
                   -i 1000 --than 4ms --detail=samekey
```

### 性能优化
```bash
# 高频场景增大缓冲区
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e sched:sched_switch -m 128 -i 1000
```

### 组合使用
```bash
# 添加迁移事件辅助分析
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e 'sched:sched_switch,sched:sched_migrate_task//untraced/stack/' \
                   --filter python -i 1000 --than 4ms --detail=samecpu

# 联合profile采样分析长循环
perf-prof rundelay -e sched:sched_wakeup*,sched:sched_switch \
                   -e 'sched:sched_switch,profile/-F 500 -g/untraced/' \
                   --filter python -i 1000 --than 100ms --detail=samecpu
```

## 与multi-trace的区别

| 特性 | rundelay | multi-trace |
|------|----------|------------|
| **事件配置** | 使用通配符`sched:sched_wakeup*`自动匹配 | 需手动指定所有事件 |
| **Key关联** | 自动设置（无需`-k pid`） | 需手动指定`-k`或`key=`属性 |
| **排序** | 自动启用`--order` | 需手动指定`--order` |
| **过滤器** | 支持`--filter comm`自动应用 | 需手动编写filter表达式 |
| **适用场景** | 专用于调度延迟分析 | 通用的多事件关系分析 |
| **使用复杂度** | 简单 | 灵活但需更多配置 |

## 相关资源
- [multi-trace核心文档](multi-trace.md)
- [事件过滤语法参考](Event_filtering.md)
- [task-state进程状态分析](task-state.md)
