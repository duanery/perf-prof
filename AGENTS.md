# perf-prof 项目 — 编码指南

本文档面向 **改动 perf-prof 源码** 的场景（增加分析器、修改解析器、维护 lib/ 等）。
使用 perf-prof 工具做性能分析请看 [docs/tool_usage.md](docs/tool_usage.md) 与 [skills/perf-prof/SKILL.md](skills/perf-prof/SKILL.md)。

## 项目概述
**perf-prof**是一个linux系统级的分析工具，可以长期运行，且安全可靠，兼容性广，性能良好。

### 工具特色
- 基于`libperf`和`libtraceevent`、`libbpf`库。
- 采样事件不写文件，在内存中处理后直接丢弃。
- 兼容旧linux内核，需要支持perf_event。
- 用户态实现，安全，可以快速迭代。

### 工具定位

perf-prof的目标是为问题分析提供更多可能，在性能、灵活性、跟踪时效、兼容性之间权衡。

**Linux跟踪工具优先级**（类比编程语言）：

| 优先级 | 跟踪工具 | 类比语言 | 适用场景 |
|--------|----------|----------|----------|
| 1 | bcc/bpftrace/bpftime | C/C++/Rust | 性能影响最小，内核态聚合，高频事件 |
| 2 | perf-prof | JavaScript/TypeScript | 兼容3.10旧内核、延迟根因分析、紧急问题快速拼命令 |
| 3 | perf/ftrace | Python | 短时采样后离线分析，无长期运行需求 |
| 4 | 其他 | Shell | 特定场景 |

**perf-prof的核心优势**：
- **兼容性**：支持3.10等旧内核（仅需perf_event），bcc/bpftrace需要4.1+内核
- **延迟分析**：multi-trace提供延迟根因分析能力（--detail还原中间细节），bcc需要自行编写复杂逻辑
- **快速上手**：内建分析器覆盖常见场景，紧急问题可直接拼命令，无需编写代码
- **实时处理**：事件在内存中实时处理后丢弃，可长期运行

**python分析器的独特价值**：
- bcc需要掌握eBPF C代码（不同prog类型参数不同、可调用的bpf helper不同）、perf_event、tracepoint、kprobe、poll等底层细节
- bpftrace需要学习一套新的跟踪语言、大量新函数，且限制较大
- perf-prof python把采样事件转换为PerfEvent对象，用户不需要关注如何采样、不需要关注内核逻辑，只需了解事件成员即可开始分析（使用help生成模板后修改）


## 项目代码结构

**改动源码前先按此地图定位——修改分析器、事件解析、栈回溯、时间戳转换等常见任务都会来这里查。**

### 顶层目录

```
perf-prof/
├── src/                  # 项目源码：框架 + 所有分析器（52 个 .c 文件）
│   ├── bpf-skel/        # BPF skeleton 程序（bpf:kvm_exit 用）
│   ├── filter/          # 三层事件过滤器实现
│   └── sqlite/          # SQLite 引擎和扩展（sql profiler 用）
├── lib/                  # 第三方基础库（多来自 Linux 内核）
│   ├── perf/            # libperf：perf_event 封装、evlist、mmap、cpu_map、thread_map
│   ├── traceevent/      # libtraceevent：tracepoint 二进制格式解析
│   ├── bpf/             # libbpf：BPF 程序加载/卸载
│   ├── subcmd/          # 子命令解析、parse-options
│   ├── api/             # sysfs / debugfs / tracing_path 等 API 封装
│   ├── symbol/          # ELF 符号解析
│   └── lockdep/         # 锁依赖检查
├── arch/                 # 体系结构相关（x86, ARM, RISC-V, PowerPC 等）
├── include/              # 公共头文件（uapi 等）
├── build/                # 构建脚本、fixdep、Makefile 片段
├── tests/                # pytest 测试（每个分析器一个测试文件）
├── docs/                 # 项目文档
├── tools/                # 独立分析工具（脚本 + python 模块 + 设计文档）
├── packages/             # 打包脚本、standalone python 下载脚本
└── skills/               # AI 辅助分析技能包（skills/perf-prof/）
```

### `src/` — 核心框架 + 分析器

**核心框架文件（所有分析器都会用到）：**

| 文件 | 职责 |
|-----|-----|
| `src/monitor.c/h` | 核心监控框架：`profiler` 注册、`prof_dev` 生命周期、evlist 打开、事件分发 |
| `src/tep.c/h` | trace 事件解析器：解析 `-e` 事件字符串，识别 tracepoint / kprobe / uprobe / profiler 头部 |
| `src/trace_helpers.c/h` | trace 事件辅助工具：符号解析、栈回溯 |
| `src/stack_helpers.c/h` | 栈遍历、火焰图、键值栈聚合 |
| `src/latency_helpers.c/h` | 延迟统计（tdigest 分布） |
| `src/count_helpers.c/h` | 计数分析辅助 |
| `src/tp_struct.h` | tracepoint 结构体定义 |
| `src/order.c` | 多 CPU ringbuffer 事件时间序合并 |
| `src/two-event.c` | 两两事件配对分析（multi-trace 底层） |

**基础服务源码：**

| 文件 | 职责 |
|-----|-----|
| `src/comm.c` | pid → 进程名映射，跟踪进程创建/改名 |
| `src/convert.c` | 时间戳转换（perf → tsc / kvmclock / monotonic） |
| `src/expr.c` | 基于 c4 的表达式编译器（事件属性 EXPR、trace event 用户态过滤器 fallback） |
| `src/event-spread.c` | 事件传播：Guest → Host 联合分析 |
| `src/net.c` | 事件传播的 TCP 承载 |
| `src/pystack.c` | 持续跟踪 Python 堆栈 |
| `src/vcpu_info.c` | 基于 virsh 输出获取虚拟机 vcpu 信息 |
| `src/ptrace.c` | ptrace 跟踪目标进程新建的线程/子进程 |
| `src/dwarf_unwind.c` | DWARF 用户态栈回溯（需 `CONFIG_LIBUNWIND`） |
| `src/perfeval.c` | perf 事件 evaluation（辅助功能开销评估） |
| `src/help.c` `src/list.c` `src/misc.c` | 帮助、列出事件、杂项工具 |

**分析器源码（每个分析器一个 .c 文件，通过 `PROFILER_REGISTER()` / `MONITOR_REGISTER()` 注册）：**

| 分类 | 文件 |
|-----|-----|
| CPU 性能 | `profile.c`, `top.c`, `oncpu.c`, `cpu-util.c` |
| 内存 | `kmemleak.c`, `kmemprof.c`, `page-faults.c` |
| 调度进程 | `task-state.c`, `sched.c` |
| 延迟/多事件 | `multi-trace.c`, `nested-trace.c`（依赖 `two-event.c`） |
| 虚拟化 | `kvm-exit.c`, `kvmmmu.c`（`vcpu_info.c` 提供支撑） |
| I/O / 存储 | `blktrace.c`, `llcstat.c`, `tlbstat.c` |
| 硬件性能 | `hwstat.c`, `ldlat-loads.c`, `ldlat-stores.c`, `split-lock.c` |
| 中断/定时器 | `hrtimer.c`, `hrcount.c`, `irq-off.c`, `watchdog.c` |
| 事件跟踪 | `trace.c`, `event-care.c`, `stat.c`, `percpu-stat.c`, `num-dist.c` |
| 断点/内存读 | `breakpoint.c`, `kcore.c` |
| 探针管理 | `usdt.c` |
| 脚本/查询 | `python.c`（需 `CONFIG_LIBPYTHON`）, `sql.c`（+ `src/sqlite/`） |
| BPF 事件 | `bpf_kvm_exit.c`（+ `src/bpf-skel/kvm_exit.bpf.c`） |

### `src/filter/` — 三层过滤器

- `bpf_filter.c` + `perf_event.bpf.c`：eBPF 过滤器（内核态最强大）
- `tp_filter.c`：tracepoint（ftrace）事件过滤器
- PMU 过滤器散落在各分析器 attr 设置里（`--exclude-user/kernel/guest/host`）

改过滤器逻辑一定会碰这个目录。

### `src/bpf-skel/` — BPF Skeleton

- `kvm_exit.bpf.c`：目前唯一的 BPF 程序（`bpf:kvm_exit` 分析器用）
- `vmlinux.h`：内核类型定义（构建时通过 `bpftool btf dump` 生成）
- `kvm_exit.skel.h`：libbpf 生成的 skeleton 头文件（构建时生成）

### `src/sqlite/` — SQL 引擎

- `sqlite3.c` + `perf_tp.c`：`sql` 分析器把事件流通过 SQLite 虚拟表暴露给 SQL 查询
- **amalgamation 完全独立、每 3 个月更新一次**：`sqlite3.c` 是 SQLite 官方发布的 amalgamation 单文件源码，与 perf-prof 其它代码零耦合。升级时整体替换 `sqlite3.c` / `sqlite3.h`（和 `shell.c`），**不要**在里面打补丁——本地补丁会在下次同步时全部丢失。仅项目自身的胶水层写在 `perf_tp.c`。

### 核心数据结构（记住这四个）

- **`struct env`**（`src/monitor.h`）— 全部命令行选项参数的落点
- **`profiler`（即 `struct monitor`）**（`src/monitor.h`）— 一个分析器的接口定义（`init` / `deinit` / `interval` / `read` / `sample` / `filter` / `reinit` 等回调）
- **`struct prof_dev`**（`src/monitor.h`）— 分析器**实例**：Attach 的 cpus/threads、`perf_evlist`、order、时间戳转换器、当前 profiler 指针
- **`struct tp`**（`src/tep.h`）— 单个事件（`sys:name` / kprobe / uprobe / profiler）解析后的表示，含 kprobe_func / uprobe_path / probe_offset / filter / attr 等

### 事件处理管道
```
事件源 → 内核过滤器 → perf ringbuffer → order 排序 → 分析器 sample() 回调 → 输出
```
- 三层过滤：eBPF、PMU、ftrace（trace event 过滤器）
- Attach 维度：CPU、PID、TID、workload、cgroups
- 栈处理模式：perf top 风格、火焰图、键值栈

### `lib/` — 基础库

大多数改动都是**读**这些库；很少改。

- `lib/perf/`：`perf_event_open` 封装、evlist、evsel、mmap ringbuffer、cpu_map、thread_map
- `lib/traceevent/`：tracepoint 事件二进制 → 字段名/类型 解析；plugins/ 是 kmem/kvm/sched 等特殊格式化
- `lib/bpf/`：libbpf（本仓库有本地补丁；见"未提交的历史修改"）
- `lib/subcmd/`：`parse-options`、`exec-cmd`、`run-command`
- `lib/api/`：`fs/`（sysfs/debugfs/tracing_path）、`fd/array`、`cpu/debug`
- `lib/symbol/`：ELF 符号、demangle
- 独立小工具：`rbtree.c` `rblist.c` `strlist.c` `thread_map.c` `cgroup.c` `epoll.c` `tdigest.c`（P99 分位）、`rpmalloc.c`、`demangle-cxx.cpp` `demangle-java.c` `demangle-rust.c`

### `tools/` — 独立工具集

基于 perf-prof 构建的独立分析工具，每个工具自包含（脚本 + python 模块 + 设计文档），不编译进主二进制。

- `func_latency.sh` + `func_latency.py`：uprobe/kprobe 函数调用路径延迟分析
- `*.py`：各类专项分析脚本（kmemleak、rundelay、syscall_latency、softirq 等）
- `*.bt`：bpftrace 脚本（hw_irq、sched_switch、task_state 等）

**提交规则**：每个工具的源码、脚本、设计文档作为一个整体一起提交（见"提交拆分"第 4 条）。

### `docs/` — 项目文档

改分析器/事件语法/选项时，同步修改文档。

- **`profilers/`**：每个分析器一份 `.md`（`profile` / `top` / `multi-trace` / `task-state` / `kmemleak` / `blktrace` / `kvm-exit` / `oncpu` / `python` / `sql` / `trace` 等），配 `template.md` 供新增分析器套用。
- **`images/`**：文档配图（`multi-trace-*.png`、`perf-prof_framework.png`）
- **`plans/`**：设计计划稿，如 `2026-07-01-kprobe-uprobe-format-redesign.md`
- 站点资产：`index.html` / `style.css` / `.nojekyll`
- 权威文档：`perf-prof User Guide.pdf`

## 构建系统

perf-prof 使用类似 Linux 内核 kbuild 的构建框架。

### 构建项目
```bash
# 安装依赖包
yum install -y xz-devel elfutils-libelf-devel libunwind-devel python3-devel

# 安装ebpf依赖包
yum install llvm bpftool

# 构建整个项目
make

# 详细构建
make V=1

# 清理构建产物
make clean
```

关键依赖：
- `libelf` (必需)
- `liblzma` (可选，用于 MiniDebugInfo 解压缩)
- `zlib` (可选)
- `libunwind` (可选，用于 DWARF 用户态栈回溯 → `dwarf_unwind.o`)
- `libpython3-dev` (可选，用于 python 分析器 → `python.o`)
- BTF 支持 (可选，用于 BPF 功能)
- 符号反混淆支持 (可选)
- 内存分配器 (tcmalloc 或 rpmalloc，可选)

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

- `PYTHON=` 选项自动检测 standalone Python 路径，设置 `PYTHONHOME`，添加 `-Wl,-rpath`
- `PYTHON_HOME=` 可覆盖安装路径（RPM 打包时使用，如 `PYTHON_HOME=/usr/lib64/perf-prof/python`）
- 下载源：https://github.com/astral-sh/python-build-standalone/releases
- 包格式：`cpython-{ver}+{tag}-{arch}-unknown-linux-gnu-install_only_stripped.tar.gz`

### 构建文件层次

```
Makefile (主入口)
├── Build (定义顶层目标: lib/ arch/ src/)
├── src/Build (定义perf-prof源文件和子目录)
├── build/Makefile.bin (处理二进制目标构建)
├── build/Makefile.build (处理源文件编译和链接)
├── build/Makefile.include (通用构建规则)
└── scripts/Makefile.include (脚本和工具)
```

**关键规则：**
- 主 Makefile 包含 `build/Makefile.include` 和 `scripts/Makefile.include`
- 功能检测在 `Makefile.config` 中，包含 `build/Makefile.feature`
- 功能检测定义 `HAVE_feature` 用于源码文件，`CONFIG_feature` 用于 `Build` 文件
- 功能检测输出到 `.config-detected`
- 通过 `CROSS_COMPILE` / `LLVM` 变量支持交叉编译
- 可以使用 `O=path` 指定输出目录
- `Build` 文件语法：
  - `bin-y` 指定构建的二进制
  - `perf-prof-y` 指定 perf-prof 二进制链接的 .o 文件与目录
  - `CFLAGS_xx.o` 指定 .o 文件追加的编译选项

**目录 Build 文件机制：**
- 根目录 `Build`：定义主二进制 perf-prof，引用 lib/ arch/ src/
- `src/Build`：定义所有分析器源文件，引用 filter/ bpf-skel/ sqlite/ 子目录
- `lib/*/Build`、`arch/*/Build` 定义各自的源文件

### 构建时配置

**作用于 Build 文件（决定链接哪些 .o）：**
- `CONFIG_LIBELF`：elf 支持（必需）
- `CONFIG_LIBBPF`：启用 ebpf 支持
- `CONFIG_LZMA`：启用 MiniDebugInfo 支持
- `CONFIG_LIBTCMALLOC`：使用 tcmalloc 内存分配器
- `CONFIG_RPMALLOC`：使用 rpmalloc 内存分配器
- `CONFIG_LIBUNWIND`：链接 `src/dwarf_unwind.o`，启用 DWARF 用户态栈回溯
- `CONFIG_LIBPYTHON`：链接 `src/python.o`，启用 python 分析器

**作用于源码文件（决定编译哪些代码分支）：**
- `CONFIG_LIBBPF`：启用 ebpf 相关代码
- `HAVE_LZMA_SUPPORT`：MiniDebugInfo 相关代码
- `HAVE_LIBTCMALLOC`：tcmalloc hook
- `HAVE_RPMALLOC`：rpmalloc hook
- `HAVE_LIBUNWIND`：DWARF 栈回溯代码分支（`src/monitor.c`、`src/trace_helpers.c` 里可见）
- `HAVE_LIBPYTHON`：python 分析器代码
- `HAVE_CXA_DEMANGLE_SUPPORT`：C++ 符号 demangle

## 测试

```bash
# 使用pytest运行所有测试
cd tests
pytest

# 运行特定测试文件
pytest test_profile.py

# 使用自定义运行时和内存泄漏检查运行
pytest --runtime=20 --memleak-check=2000
```

**必须先 `cd tests` 再跑 pytest。** `tests/conftest.py` 在测试前后分别执行`make` 和 `make clean`，靠的是当前目录里的 `tests/Makefile`（只构建测试所需的辅助程序）。
若在项目根目录执行 pytest，命中的是顶层 Makefile：测试前重新编译整个 perf-prof，测试结束时 `make clean` 把 perf-prof 二进制和全部编译产物清掉。

- 使用 pytest 和自定义 fixture 进行运行时和内存泄漏检查
- 测试在运行前自动构建项目
- 每个分析器都有对应的测试文件
- 测试验证输出正确性和内存管理

**测试用例约定**（写新测试时必须遵守，参考 `tests/test_trace.py`、`tests/test_kmemleak.py`）：

1. 顶部固定 imports：
   ```python
   from PerfProf import PerfProf
   from conftest import result_check
   ```
2. 正向用例的模板一律是：
   ```python
   prof = PerfProf([...])
   for std, line in prof.run(runtime, memleak_check):
       result_check(std, line, runtime, memleak_check)
   ```
   **不能**只写 `prof.run(runtime, memleak_check)` 单行——那样不会消费 stdout/stderr，也不会让 `result_check` 触发失败。
3. **不要**直接 `subprocess.run(['./perf-prof', ...], capture_output=True)`。反证测试同样应通过 `PerfProf.run()` 迭代 + 扫 stderr 是否含预期关键字。

## 开发指南

### 开发新分析器
1. 创建分析器源文件（如 `src/new_profiler.c`）
2. 实现 `profiler` 结构体定义的 `init`、`deinit`、`interval`、`read`、`sample` 等回调
3. 定义分析器的 `name`、`desc` 描述、`argc` 支持的选项
4. 使用 `PROFILER_REGISTER()`（或 `MONITOR_REGISTER()`）注册
5. 在 `src/Build` 中添加 .o 文件（如：`perf-prof-y += new_profiler.o`）
6. 编译、自测：`-h` 查看帮助是否带 EXAMPLES 示例；测试各选项参数
7. 在 `tests/` 中添加对应的自动化测试文件（遵守上文"测试用例约定"）
8. 若涉及事件格式扩展，同步更新：
   - `src/monitor.c` 中 `-e/--event` 帮助字符串
   - `docs/main_options.md`、`docs/tool_usage.md`
   - `README.md`、`README_CN.md`
   - `skills/perf-prof/SKILL.md` 及 `skills/perf-prof/references/profilers/*.md`
   - `docs/profilers/<相关分析器>.md`

### 编码规范
- 遵循 Linux 内核编码风格
- 使用 SPDX 许可证标识符
- 内存管理：使用具有泄漏检测的自定义分配器
- 符号解析支持内核和用户空间符号
- 火焰图生成需要外部 `flamegraph.pl` 脚本

### 设计文档（plan）
- **位置固定**：所有 plan / spec / 设计文档一律放在 `docs/plans/`，
  文件名 `YYYY-MM-DD-<topic>-design.md`。不要放到 `docs/superpowers/`
  或临时目录。
- **完成后回填状态**：一个 plan 落地（源码 + 测试 + 文档三个 commit
  都进仓）后，在 plan 文件顶部把 `Status:` 更新为
  `Done — implemented in commits <src>/<tests>/<docs>`，方便后续查
  阅时区分"设计中"与"已实现"。
- `docs/plans/` 目前是 untracked 目录，plan 文件默认不入库；只有用户
  明确要求时才 `git add`。

## 常见陷阱与经验教训

本节记录改动 perf-prof 时容易踩到的坑。

### 内核演进导致 tracepoint 消失或字段增删

内核升级会让 tracepoint 被合并/删除、字段被移除或描述改变。新内核上"找不到事件 / 未定义变量"先怀疑内核变更，而不是 perf-prof 的 bug。

必须查到对应的内核 commit 与引入版本，写进提交说明，格式统一为：

```
<12位SHA> ("<内核 commit subject>"), in v<版本>, ...
```

若改动落在 `src/` 源码，还要在相关代码的注释里标注同样的信息，说明为何做兼容处理：

```c
/*
 * <为什么这样处理>: since <12位SHA> ("<内核 commit subject>"), in v<版本>,
 * <内核的行为变化及其对本处代码的影响>。
 */
```

兼容优先运行时探测（事件/字段是否存在），不要比较内核版本号——改动可能被发行版 backport 或回退。

### 文档同步范围
- 事件语法或选项字符串一旦变更，**必须同步更新的地方**：
  - `src/monitor.c` 帮助字符串
  - `docs/main_options.md`
  - `docs/tool_usage.md`
  - `README.md` / `README_CN.md`
  - `docs/profilers/*.md` 里所有涉及该语法的示例
  - `skills/perf-prof/SKILL.md` 及 `skills/perf-prof/references/profilers/*.md`
- 拆过一次范围就要习惯性检查 `skills/`——它容易被忘掉。

### AI 协作提交标签政策

**默认使用 `git commit -s`** 自动追加 `Signed-off-by:`；`Co-Authored-By:` / `Reviewed-by:` 仍需手工写入。

三个标签的使用规则：

- `Co-Authored-By:` — 只用于 AI（AI 参与了代码编写，无论比例多少）
- `Signed-off-by:` — 只用于人类（对提交内容负最终责任），由 `git commit -s` 自动生成
- `Reviewed-by:` — 人类和 AI 都可用；AI 时格式与 `Co-Authored-By:` 一致

**三种场景：**

```
# 场景一：人类写代码，AI review
Reviewed-by: Claude Sonnet 4.6 <noreply@anthropic.com>
Signed-off-by: 你的姓名 <邮箱>

# 场景二：人类 + AI 共同写代码
Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
Signed-off-by: 你的姓名 <邮箱>

# 场景三：AI 写代码，人类 review 并给出修改建议
Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
Reviewed-by: 你的姓名 <邮箱>
Signed-off-by: 你的姓名 <邮箱>
```

模型名称跟随实际使用的模型，如 `Claude Opus 4.7 (1M context)`、`Claude Sonnet 4.6` 等。

### 提交拆分
按"变更种类"分成三类独立提交，顺序上通常是 **源码 → 测试 → 文档**：

1. **源码提交** — C 源文件（`src/*.c`、`src/*.h`、`lib/**`）+ **同一 patch 内**必须包含：
   - `src/monitor.c` 的选项帮助字符串更新（帮助与代码语义强绑定，不可分开提交）
   - `src/Build` 里的 .o 增删
   - 若涉及构建时配置，同步 `build/` 与 `Makefile.config`

2. **测试提交** — `tests/**` 独立成一个 commit：
   - 新增 `tests/test_*.py`
   - 迁移已有测试到新语法/新参数
   - **不要**把测试塞进源码 commit；review 时通常需要单独看测试是否覆盖到位

3. **文档提交** — 所有 `.md` 独立成一个 commit：
   - `README.md`、`README_CN.md`
   - `docs/**`（`main_options.md`、`profilers/*.md`、设计文档等）
   - `skills/perf-prof/**`（SKILL.md 与 `references/profilers/*.md`）

4. **tools/ 提交** — `tools/` 下每个独立工具的源码、文档、配套脚本作为一个整体一起提交，不按上面三类拆分：
   - 例如 `tools/func_latency.sh` + `tools/func_latency.py` + `tools/func_latency_design.md` 合为一个 commit
   - 原因：tools/ 里的工具相互独立、自包含，拆开提交反而割裂了工具的完整性

**其他约定：**
- **测试暴露源码 bug 时，修补源码要合入到原源码 patch 里，不要另开一个"fix xxx"的补丁**：
  - 若原 patch 是**当前分支最近**的一次源码提交：修改源码 → `git add <文件>` → `git commit --fixup=<原源码 SHA>` → `git rebase -i --autosquash <原源码 SHA>^`。
  - 若原 patch 中间隔了测试或文档 commit：先 stash 掉不相关的工作树改动，再 rebase autosquash。
  - 目的：源码 patch 应始终自洽（可编译、可跑通配套测试），任何后续发现的 bug 都算作原实现不完整，回填到原 commit 才能保证 bisect / review 的语义清晰。
- 已推送到远端的 commit 不要用 `--amend` / `rebase --autosquash`；本地未推送的可按用户指示合并（参考本仓库 `db231e3` / `475b4a4` 的整理过程）。
- 用户在 review 时会要求按上面三类拆开，回炉重排比一开始就拆干净成本高。
- 一个功能可能涉及多次源码/测试/文档提交轮换（本会话 `db231e3 → 475b4a4 → b8a8977 → e07e3e7 → ec4ae9f` 就是范例）——按功能推进的自然节奏来，不是硬性一次三 commit 就结束。

### 未跟踪文件
- `docs/thread-tracking-design.md`、`docs/plans/*.md` 目前是 untracked（不在 git 版本库里）。改动它们不需要 `git add`，除非用户明确要求入库。
- `git add -A` 会一起吞掉临时构建产物（`.o.cmd`、`*.o`、`perf-prof` 二进制），务必用具体路径 `git add file1 file2 ...`。


## 相关文档

- [docs/tool_usage.md](docs/tool_usage.md) — perf-prof 工具使用指南（帮助、选项、功能分类、事件格式、过滤器、表达式、分析器概览、工作流程）
- [docs/main_options.md](docs/main_options.md) — 完整选项参数字典
- [docs/expr.md](docs/expr.md) — 表达式系统
- [docs/Event_filtering.md](docs/Event_filtering.md) — trace event 过滤器语法
- [docs/profilers/](docs/profilers/) — 各分析器详细说明
- [skills/perf-prof/SKILL.md](skills/perf-prof/SKILL.md) — 面向问题分析的工作流程

## 重要注意事项

- 所有分析器都支持实时处理，事件不会存储，处理完即丢弃
- 框架专为高性能监控场景设计，具有低开销和实时数据处理能力
- **`-vv` 选项警告**：使用 `-vv` 会显示原始事件，输出量极大，禁止使用