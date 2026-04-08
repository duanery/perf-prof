---
layout: default
title: "perf-prof 文档"
---

# perf-prof

基于 perf_event 的 Linux 实时性能分析框架。事件在内存中实时处理后直接丢弃，可长期运行，安全可靠。

![perf-prof 框架](images/perf-prof_framework.png)

## 快速导航

| 文档 | 说明 |
|------|------|
| [框架概述](Readme) | 事件模型、profiler 架构、事件关系 |
| [选项参考](main_options) | 完整的命令行选项参数（60+ 参数） |
| [表达式系统](expr) | EXPR 属性语法、运算符、内置函数 |
| [事件过滤](Event_filtering) | 内核 trace event 过滤表达式语法 |

---

## 分析器 (Profilers)

### 采样与剖析

| 分析器 | 功能 |
|--------|------|
| **[profile](profilers/profile)** | CPU采样分析：热点函数定位，火焰图生成，支持内核/用户态分析 |

### 事件跟踪与关联

| 分析器 | 功能 |
|--------|------|
| **[trace](profilers/trace)** | 通用事件跟踪：打印事件内容、调用栈，支持 kprobe/uprobe |
| **[multi-trace](profilers/multi-trace)** | 多事件关系分析：事件配对、延迟分析、`--detail` 还原中间细节 |

### 统计与聚合

| 分析器 | 功能 |
|--------|------|
| **[top](profilers/top)** | 键值聚合统计：按进程/线程/自定义键排序 |
| **[sql](profilers/sql)** | SQL 聚合查询：基于 SQLite 的灵活事件聚合分析 |

### 进程与调度

| 分析器 | 功能 |
|--------|------|
| **[task-state](profilers/task-state)** | 进程状态监控：R/S/D/T 状态耗时分布与根因分析 |
| **[oncpu](profilers/oncpu)** | CPU 运行监控：实时显示各 CPU 上运行的进程及运行时间 |

### IO 分析

| 分析器 | 功能 |
|--------|------|
| **[blktrace](profilers/blktrace)** | 块设备 IO 跟踪：IO 全生命周期延迟、毛刺检测 |

### 虚拟化

| 分析器 | 功能 |
|--------|------|
| **[kvm-exit](profilers/kvm-exit)** | KVM VM-Exit/VM-Entry 延迟分析与退出原因统计 |

### 内存

| 分析器 | 功能 |
|--------|------|
| **[kmemleak](profilers/kmemleak)** | 内存泄漏检测：内核/用户态内存分配追踪 |

### 脚本分析

| 分析器 | 功能 |
|--------|------|
| **[python](profilers/python)** | Python 脚本分析：自定义 Python 脚本处理 perf 事件 |

---

## 按问题类型查找

| 问题类型 | 推荐分析器 | 说明 |
|----------|-----------|------|
| CPU 使用率高 | [profile](profilers/profile) | 采样分析热点函数 |
| 调度延迟 | [multi-trace](profilers/multi-trace), [task-state](profilers/task-state) | 唤醒→运行延迟、D/S 状态分析 |
| IO 性能 | [blktrace](profilers/blktrace) | 块设备 IO 延迟全链路 |
| 内存泄漏 | [kmemleak](profilers/kmemleak) | 分配/释放配对追踪 |
| 虚拟化开销 | [kvm-exit](profilers/kvm-exit) | VM-Exit 延迟与原因 |
| 进程状态异常 | [task-state](profilers/task-state) | D/S 状态耗时与堆栈 |
| 系统调用慢 | [multi-trace](profilers/multi-trace) | syscalls 系统调用延迟 |
| 事件聚合统计 | [top](profilers/top), [sql](profilers/sql) | 按键值聚合排序 |

---

## 其他文档

| 文档 | 说明 |
|------|------|
| [kmemleak 概述](kmemleak) | kmemleak 分析概述 |
| [perf 时钟转换](perf_clock_to_guest_clock) | perf 时钟到 Guest 时钟的转换 |
| [virtio-ports](virtio-ports) | virtio-ports 事件传播 |
| [watchdog](watchdog) | 硬锁/软锁检测 |

---

## 工作流程示例

### 1. CPU 性能分析
```bash
# 采样分析内核态 CPU 热点
perf-prof profile -F 997 -g --exclude-user --than 30

# 生成火焰图
perf-prof profile -F 997 -g --flame-graph cpu.folded
```

### 2. 延迟分析
```bash
# 调度延迟分析
perf-prof multi-trace -e sched:sched_wakeup,sched:sched_wakeup_new \
  -e 'sched:sched_switch//key=next_pid/' -k pid --order --than 4ms --detail

# 软中断延迟
perf-prof multi-trace -e irq:softirq_entry/vec==1/ \
  -e irq:softirq_exit/vec==1/ -i 1000 --than 100us
```

### 3. 内存泄漏检测
```bash
perf-prof kmemleak --alloc "kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/" \
  --free "kmem:kfree//ptr=ptr/" --order -m 128 -g
```

### 4. IO 跟踪
```bash
perf-prof blktrace -d /dev/sda -i 1000 --than 10ms
```

---

## 外部资源

- [GitHub 项目主页](https://github.com/OpenCloudOS/perf-prof)
- [Linux perf_event 文档](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)
- [Linux tracepoint 文档](https://www.kernel.org/doc/html/latest/trace/events.html)
