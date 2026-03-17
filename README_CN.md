# perf-prof

**perf-prof** 是一款 Linux 系统级性能分析工具，专为长期性能监控设计，具有低开销、广泛兼容性和高可靠性的特点。

[![许可证](https://img.shields.io/badge/license-GPLv2-blue.svg)](LICENSE)
[![构建状态](https://img.shields.io/badge/build-passing-brightgreen.svg)](https://github.com/OpenCloudOS/perf-prof)

## 目录

- [项目概述](#项目概述)
- [核心特性](#核心特性)
- [安装](#安装)
- [快速入门](#快速入门)
- [分析器](#分析器)
- [文档](#文档)
- [开发](#开发)
- [贡献](#贡献)
- [许可证](#许可证)

---

## English Version

您正在阅读中文版文档。[English version available](README.md).

---

## 项目概述

perf-prof 是一款用户态性能分析工具，基于 `libperf`、`libtraceevent` 和 `libbpf` 构建。它提供实时分析能力，无需将事件数据写入磁盘，在内存中处理并立即丢弃。

### 核心特性

- **内存处理**：事件在内存中处理并立即丢弃，无持久化存储开销
- **广泛兼容**：支持旧版 Linux 内核（需要 perf_event 支持）
- **用户态实现**：安全执行，快速迭代
- **模块化架构**：30+ 专用分析器应对不同场景
- **低开销**：内核级过滤减少用户空间数据传输
- **实时分析**：实时处理事件，即时反馈

## 安装

### 前置依赖

```bash
# 安装必需依赖
yum install -y xz-devel elfutils-libelf-devel

# 可选：安装 eBPF 依赖
yum install -y llvm bpftool
```

### 从源码构建

```bash
# 克隆仓库
git clone https://github.com/OpenCloudOS/perf-prof.git
cd perf-prof

# 构建项目
make

# 详细构建输出
make V=1

# 清理构建产物
make clean
```

### 交叉编译

```bash
# 使用 CROSS_COMPILE
make CROSS_COMPILE=aarch64-linux-gnu-

# 使用 LLVM
make LLVM=1
```

### 使用 Standalone Python 构建

无需安装系统 python3-devel，可使用 [python-build-standalone](https://github.com/astral-sh/python-build-standalone) 启用 Python 分析器：

```bash
# 下载（自动检测架构，默认 Python 3.12）
packages/download-python-standalone.sh

# 解压
tar xzf cpython-*.tar.gz

# 使用 standalone Python 构建
make PYTHON=python/bin/python3
```

`PYTHON=` 选项会自动检测 standalone Python 的路径，设置 `PYTHONHOME`，并添加 `-Wl,-rpath` 链接 bundled libpython。可通过 `PYTHON_HOME=` 覆盖安装路径。

## 快速入门

### 列出可用分析器

```bash
# 列出所有分析器
./perf-prof -h

# 列出所有 tracepoint 事件
./perf-prof list
```

### CPU 性能分析

```bash
# 以 997Hz 采样分析 CPU 使用情况，包含调用栈
./perf-prof profile -F 997 -g

# 生成火焰图
./perf-prof profile -F 997 -g --flame-graph cpu.folded

# 仅分析用户态，指定 CPU，显示超过 30ms 的采样点
./perf-prof profile -F 997 -C 0-3 --exclude-kernel --than 30
```

### 内存泄露检测

```bash
# 检测内核内存泄露
./perf-prof kmemleak --alloc "kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/" \
                     --free "kmem:kfree//ptr=ptr/" --order -m 128 -g
```

### 进程调度分析

```bash
# 监控任务状态（R, S, D, T, I）
./perf-prof task-state -i 1000
```

### 事件跟踪

```bash
# 跟踪特定事件
./perf-prof trace -e sched:sched_wakeup,sched:sched_switch -i 1000

# 跟踪并过滤
./perf-prof trace -e "sched:sched_wakeup/prio<10/" -i 1000
```

## 分析器

![perf-prof框架](docs/images/perf-prof_framework.png)

perf-prof 提供 30+ 专用分析器，按类别组织：

### CPU 性能分析
- **profile** - CPU 性能采样分析
- **oncpu** - 监控 CPU 上运行的进程

### 内存分析
- **kmemleak** - 内存泄露检测
- **kmemprof** - 内存分配分析
- **page-faults** - 页面异常跟踪

### 调度与进程
- **task-state** - 进程状态监控（R, S, D, T, I）
- **rundelay** - 调度运行延迟分析

### I/O 性能
- **blktrace** - 块设备 I/O 跟踪

### 虚拟化
- **kvm-exit** - KVM 退出延迟分析
- **kvmmmu** - KVM MMU 映射观察

### 硬件监控
- **hwstat** - 硬件状态监控（cycles, IPC）
- **llcstat** - 最后一级缓存监控
- **tlbstat** - dTLB 监控
- **ldlat-loads** - Intel 加载延迟计数
- **ldlat-stores** - Intel 存储指令计数
- **split-lock** - x86 分裂锁检测

### 中断与定时器
- **hrtimer** - 高精度条件采样
- **irq-off** - 中断关闭检测
- **watchdog** - 硬锁和软锁检测

### 数据分析
- **sql** - SQL 聚合分析（基于 SQLite）
- **top** - 键值统计分析
- **multi-trace** - 多事件关系分析
- **syscalls** - 系统调用延迟分析
- **python** - Python 脚本事件处理

### 工具类
- **trace** - 事件跟踪
- **list** - 列出 tracepoint 事件
- **expr** - 表达式测试工具
- **usdt** - 用户静态定义跟踪
- **breakpoint** - 内核/用户空间硬件断点
- **kcore** - 读取内核内存
- **misc** - 杂项跟踪

## 事件选择

perf-prof 遵循三层事件选择规范：

### 1. 获取系统事件

```bash
# 列出所有事件
./perf-prof list

# 按类别过滤
./perf-prof list | grep -E "^(sched:|kmem:|timer:|irq:)"
```

### 2. 查看事件帮助

```bash
# 查看事件字段
./perf-prof trace -e sched:sched_wakeup help

# 多个事件
./perf-prof trace -e sched:sched_wakeup,sched:sched_switch help
```

### 3. 事件语法

```
EVENT: sys:name[/filter/ATTR/ATTR/.../]
       kprobe:func[/filter/ATTR/ATTR/.../]
       kretprobe:func[/filter/ATTR/ATTR/.../]
       uprobe:func@"file"[/filter/ATTR/ATTR/.../]
       uretprobe:func@"file"[/filter/ATTR/ATTR/.../]
```

#### 过滤器语法（内核态执行）

```bash
# 数值比较
./perf-prof trace -e "sched:sched_wakeup/pid>1000/"
./perf-prof trace -e "sched:sched_wakeup/prio<10/"

# 字符串匹配
./perf-prof trace -e 'sched:sched_wakeup/comm=="java"/'
./perf-prof trace -e 'sched:sched_wakeup/comm~"pyth*"/'

# 逻辑组合
./perf-prof trace -e "sched:sched_wakeup/pid>1000 && prio<10/"
```

#### 属性（用户态执行）

```bash
stack                     # 启用调用栈
alias=str                # 事件别名
max-stack=int            # 最大栈深度
key=EXPR                 # 事件关联键
top-by=EXPR              # 排序字段
comm=EXPR                # 进程名显示
ptr=EXPR                 # 指针字段
size=EXPR                # 大小字段
num=EXPR                 # 数值分布字段
```

## 帮助系统

```bash
# 分析器帮助（包含示例）
./perf-prof trace -h
./perf-prof task-state -h

# 事件帮助（包含字段信息）
./perf-prof trace -e sched:sched_wakeup help
./perf-prof kmemleak --alloc kmem:kmalloc --free kmem:kfree help
```

## 文档

### 主文档

- [主要选项参考](docs/main_options.md) - 完整命令行选项

### 分析器文档

- [profile](docs/profilers/profile.md) - CPU 性能分析
- [task-state](docs/profilers/task-state.md) - 进程状态监控
- [multi-trace](docs/profilers/multi-trace.md) - 多事件分析
- [sql](docs/profilers/sql.md) - SQL 聚合分析
- [top](docs/profilers/top.md) - 键值统计
- [kmemleak](docs/profilers/kmemleak.md) - 内存泄露检测
- [kvm-exit](docs/profilers/kvm-exit.md) - KVM 退出分析
- [blktrace](docs/profilers/blktrace.md) - 块设备 I/O 跟踪
- [trace](docs/profilers/trace.md) - 事件跟踪
- [python](docs/profilers/python.md) - Python 脚本事件处理

### 高级主题

- [事件过滤](docs/Event_filtering.md) - Trace event 过滤器语法
- [表达式](docs/expr.md) - 表达式语言参考

## 测试

```bash
# 运行所有测试
cd tests
pytest

# 运行特定测试文件
pytest test_profile.py

# 使用自定义运行时和内存泄漏检查
pytest --runtime=20 --memleak-check=2000
```

## 开发

### 项目结构

```
perf-prof/
├── *.c                   # 核心分析器模块（30+ 个分析器）
├── lib/                  # 基础库（libperf, libtraceevent, libbpf）
├── arch/                 # 架构相关代码
├── bpf-skel/             # BPF skeleton 程序
├── filter/               # 事件过滤器（BPF, tracepoint, PMU）
├── sqlite/               # SQLite源码和扩展模块
├── include/              # 包含头文件
├── tests/                # 测试套件
├── docs/                 # 文档
└── skills/               # AI 辅助分析技能包
```

### 核心组件

**监控框架：**
- `monitor.c/h` - 核心框架
- `tep.c/h` - Trace 事件解析器
- `trace_helpers.c/h` - Trace 事件工具
- `stack_helpers.c/h` - 栈遍历和符号解析

**分析单元：**
- 每个分析器都是独立的 `.c` 文件
- 通过 `PROFILER_REGISTER()` 宏注册
- 支持 `init`, `deinit`, `interval`, `read`, `sample` 回调

### 事件处理管道

```
事件源 → 过滤器 → 环形缓冲区 → 排序 → 分析器 → 输出
```

### 添加新分析器

1. 创建源文件 `new_profiler.c`
2. 实现 `profiler` 结构体及所需回调
3. 定义 `name`, `desc`, `argc`, `option`
4. 使用 `PROFILER_REGISTER()` 注册
5. 在 `Build` 文件中添加：`perf-prof-y += new_profiler.o`
6. 在 `tests/` 目录中添加测试

## 贡献

欢迎贡献！请遵循以下准则：

1. Fork 仓库
2. 创建特性分支
3. 为新功能编写测试
4. 确保所有测试通过
5. 提交 Pull Request

## 许可证

本项目采用 GPLv2 许可证。详见 [LICENSE](LICENSE)。

## 代码规范

- 遵循 Linux 内核编码风格
- 编写清晰、可维护的代码
- 为新功能包含测试
- 编写公共 API 和接口文档

## 相关链接

- [GitHub 仓库](https://github.com/OpenCloudOS/perf-prof)
- [问题追踪](https://github.com/OpenCloudOS/perf-prof/issues)
- [文档](docs/)
- [Readme - English](README.md)

## 致谢

基于 Linux内核 组件构建：
- libperf
- libtraceevent
- libbpf
- 其他实用库
