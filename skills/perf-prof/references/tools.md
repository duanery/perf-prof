# tools/ - 独立分析工具

基于 perf-prof 构建的现成分析脚本。它们不是分析器（profiler），而是**已经拼好命令的成品工具**：
把 `-e` 事件串、`--order`、`-m` 等选项固化在 shebang 或 shell 封装里，用户只需补上 `-p PID` 之类的观测范围。

## 概述

- **主要用途**: 覆盖三类"用内建分析器拼不出来、或拼起来很啰嗦"的场景：函数调用树耗时、事件触发时的进程上下文快照、QEMU userspace exit 耗时。
- **适用场景**: 用户态/内核函数耗时与调用关系；"谁在做这件事"（谁卸载模块、谁写 sysctl、谁反复拉起短命进程）；虚拟机 IO/MMIO 退出被 QEMU 处理得慢。
- **与分析器的关系**: 全部基于 `python` 分析器（见 [python.md](profilers/python.md)），事件采集由 perf-prof 完成，聚合与输出逻辑在 Python 脚本里。
- **最低内核版本**: 跟随所用事件。uprobe 需 3.5+，kprobe/tracepoint 3.10 可用。
- **特殊限制**: 需要 root；`perf-prof` 需编译带 `CONFIG_LIBPYTHON`（`perf-prof python -h` 能出帮助即可用）。

## 工具位置

| 环境 | 路径 |
|---|---|
| 源码仓库 | `tools/` |
| RPM 安装 | `/usr/share/perf-prof/tools/` |

RPM 只安装 git 仓库里跟踪的脚本，设计文档（`*_design.md`）不打包。
脚本自带 `#!/usr/bin/env -S perf-prof python -e <EVENT> ...` shebang 并且是可执行的，
可以直接 `./exec_trace.py` 运行；也可以显式写完整命令覆盖默认事件。

**找工具：**
```bash
ls /usr/share/perf-prof/tools/     # 已安装
ls tools/                          # 源码树

# 每个工具的完整帮助（选项、EXAMPLES、Notes）都在脚本头部和 -h 里
/usr/share/perf-prof/tools/exec_trace.py -h
```

> 使用新工具前，先执行该工具的 `-h`，与"使用新分析器前先 `perf-prof <profiler> -h`"同理。

## 工具索引

| 工具 | 解决的问题 | 分析技术 |
|---|---|---|
| `func_latency.sh` | 某个函数下调用了哪些子函数、各自耗时多少（P50/P95/P99） | 调用树 + 延迟分布 |
| `exec_trace.py` | 事件触发那一刻，CPU 上的进程是谁、完整上下文是什么 | 事件触发 + /proc 快照 |
| `kvm_userspace_exit_latency.py` | QEMU 处理 KVM userspace exit 花了多久，哪类 exit 最耗时 | 事件配对 + 延迟分布 |

---

## 1. func_latency.sh - 函数调用树耗时分析

用 uprobe/kprobe 追踪一组函数，**按完整调用路径**聚合耗时，输出树形的
`N / TOTAL / MIN / AVG / P50 / P95 / P99 / MAX`。

### 何时使用

- 想知道某个 root 函数下实际调用了哪些子函数、各调用几次。
- 同一个子函数在不同调用链下耗时是否有系统性差异（路径独立统计能回答）。
- 用户态函数和内核函数需要在**同一棵调用树**里对比（内核函数用 `|→` 标记）。

不要用它做火焰图或高频函数追踪——每秒调用 >10k 次时 uprobe 开销显著，改用 `profile`。

### 基础用法

```bash
./func_latency.sh -b BINARY [-u FUNC]... [-k FUNC]... [perf-prof options]
```

### 脚本自有选项

| 选项 | 说明 |
|---|---|
| `-b BINARY` | 目标二进制路径（用 `-u` 时必填） |
| `-u FUNC` | 二进制内的用户态函数，展开成 `uprobe` + `uretprobe` 一对（可重复） |
| `-k FUNC` | 内核函数，展开成 `kprobe` + `kretprobe` 一对（可重复） |
| `-n` | dry-run：只打印组装出来的 perf-prof 命令，不执行（排错用） |
| `-h` | 帮助 |

其余参数**原样透传** perf-prof：`-p`、`-C`、`-i`、`-m`、`-o`、`-v` 等。

**脚本自动注入 `--order`**（用户已显式传入时不重复添加）：同一次调用的 entry 和 return
可能落在不同 CPU 的 ringbuffer 上，不做全局时间排序会导致栈匹配错乱、耗时为负。

### 示例

```bash
# 追踪二进制内一组用户态函数，每 5s 输出一次
./func_latency.sh -b /path/to/target_binary \
    -u root_func_A -u child_func_1 -u child_func_2 \
    -p $(pgrep -x target_binary | head -1) -i 5000

# 用户态 + 内核函数混合追踪（内核函数以 |→ 标记）
./func_latency.sh -b /path/to/target_binary -u root_func_A -k some_kernel_func -i 3000

# 只看组装出来的命令，不真跑
./func_latency.sh -b /path/to/target_binary -u root_func_A -n
```

### 输出解读

```
2026-07-15 10:32:07
function                                     N  TOTAL(us)  MIN(us)  AVG(us)  P50(us)  P95(us)  P99(us)  MAX(us)
---------------------------------------------------------------------------------------------------------------
root_func_A                                 10    5000.00   200.00   500.00   480.00   890.00   990.00  1000.00
  |- child_func_1                            5    2100.00   350.00   420.00   410.00   500.00   500.00   500.00
  |  |→ kernel_helper                       20    1600.00    50.00    80.00    75.00   150.00   180.00   200.00
  |- child_func_2                            5    2900.00   500.00   580.00   570.00   700.00   700.00   700.00
```

- 缩进表示调用嵌套；`|-` 用户态函数，`|→` 内核函数。
- 每个 `-i` 窗口**独立统计**，历史不累积；孩子按首次出现顺序打印。
- 分位数为精确的 nearest-rank 值（保留原始样本排序计算），不是近似算法。
- `[anomalies] unmatched-returns=N pending-frames=M` 行只在有值时打印：
  - `unmatched-returns` 偏高 → 事件丢失，增大 `-m`。
  - `pending-frames` 有值通常是正常的跨窗口调用（耗时归属到"完成"窗口）。

### 限制

| 项 | 缓解方案 |
|---|---|
| 高频函数下 ringbuffer 溢出 | 增大 `-m`；`-o FILE` 避免终端成为瓶颈；缩小 `-p` 范围 |
| 尾调用优化（`foo(){return bar();}` 被优化成 jmp） | 目标程序编译时加 `-fno-optimize-sibling-calls` |
| inline 函数挂不上 uprobe | 关键函数加 `__attribute__((noinline))` |
| uretprobe 漏采会污染栈顶几层 | 脚本用"就近同名匹配 + 清除上方帧"控制爆炸半径，通过 `pending-frames` 观测 |
| `-b` 只支持一个二进制 | 分多次运行 |

---

## 2. exec_trace.py - 事件触发的进程上下文快照

在每个采样事件发生的瞬间，打印当时 CPU 上进程的 cmdline 和 `/proc` 上下文。
默认（shebang）挂 `sched:sched_process_exec`，即一个开箱可用的 exec 追踪器；
覆盖 `-e` 即可变成**任意事件**的"是谁在干这件事"快照工具。

### 何时使用

- 谁卸载/加载了内核模块？谁写了某个 sysctl？谁 open 了可疑文件？
- 有短命进程被反复拉起（`ps` 轮询抓不到），想看每次的完整命令行和父进程。
- 怀疑 fork/exec 风暴导致 CPU 尖峰，需要实时命令行 + cwd + 环境变量。
- 排查 setuid 提权、`LD_PRELOAD` 库劫持、fileless/memfd/deleted 二进制。

相比 `bpftrace execsnoop`：支持 3.10 老内核，且父进程/cwd/uid/env/stdio/cgroup/调度参数都是开关式选项，无需改脚本。

### 基础用法

```bash
# 默认：exec 追踪器（用 shebang 里的 -e sched:sched_process_exec//batch=1/ --order -m 64）
./exec_trace.py [options]

# 换成任意事件
perf-prof python -e <EVENT> --order -m 64 -- exec_trace.py [options]
```

处理器是 `__sample__`（所有事件的默认回调），所以**任何** tracepoint / kprobe / uprobe 都能用。

### 脚本自有选项

上下文默认全部关闭，按需打开（输出越多行越长）：

| 选项 | 说明 |
|---|---|
| `--name PAT` | 按 `basename(event.filename)` 过滤，shell 通配符，可重复 |
| `--path PAT` | 按完整 `event.filename` 过滤，通配符，可重复 |
| `--parent` | 父进程 PID + cmdline |
| `--tree` | 向上递归 PPid 直到 PID 1，逐层打印祖先 cmdline（覆盖 `--parent`，深度上限 32） |
| `--cwd` | 工作目录（`/proc/<pid>/cwd`） |
| `--uid` | real/effective uid/gid + loginuid |
| `--env KEY[,KEY..]` | 选定的环境变量（`/proc/<pid>/environ`），可重复 |
| `--exe` | 真实二进制路径；与事件 `filename` 不一致时追加 `filename:` 行 |
| `--std` | stdin/stdout/stderr 指向（`/proc/<pid>/fd/{0,1,2}`） |
| `--cgroup-path [SUBSYS,..]` | 打印 `/proc/<pid>/cgroup`；无参数为全部，带列表则按控制器子串过滤（`unified` 匹配 cgroup v2 空 controllers 行） |
| `--sched` | 调度参数：policy（SCHED_NORMAL/FIFO/RR/BATCH/IDLE/DEADLINE）、nice、rtprio、Cpus_allowed_list |

**注意**：选项名是 `--cgroup-path` 而不是 `--cgroup`——perf-prof python 自己已经有 `--cgroups`，长选项前缀匹配会把两者混在一起。

**`--name` / `--path` 只对携带 `filename` 字段的事件有意义**
（`sched:sched_process_exec`、`syscalls:sys_enter_execve` 等）；其它事件启用这两个选项时事件会被丢弃（无从匹配）。

### 示例

```bash
# 默认：捕获所有 exec
./exec_trace.py
./exec_trace.py --name 'python*'
./exec_trace.py --path '/usr/local/bin/*'

# 谁在哪儿跑 sudo：父进程 + cwd + 身份
./exec_trace.py --name sudo --parent --cwd --uid

# 完整祖先链直到 init —— 最终责任方是谁
./exec_trace.py --tree

# 排查 LD_PRELOAD 劫持 / setuid 提权
./exec_trace.py --uid --env LD_PRELOAD,LD_LIBRARY_PATH

# 排查 fileless / 已删除二进制（filename 与真实 exe 不一致时会额外打印）
./exec_trace.py --exe --std

# 容器上下文
./exec_trace.py --cgroup-path memory,cpu

# 换成 fork 事件（无 filename 字段，--name/--path 不适用）
perf-prof python -e 'sched:sched_process_fork' \
    --order -m 64 -- exec_trace.py --parent --cwd --uid

# 谁卸载了内核模块？打印完整祖先链 + 身份
perf-prof python -e 'kprobe:free_module' \
    --order -m 64 -- exec_trace.py --tree --uid

# 高频事件：把过滤下推到内核（远比 --name 便宜）
perf-prof python -e 'sched:sched_process_exec/filename~"*/ps"/batch=1/' \
    --order -m 64 -- exec_trace.py
```

### 输出解读

```
[2026-07-17 10:23:11.045123] CPU:2   PID:20481   [sched:sched_process_exec] /bin/bash /tmp/deploy.sh --stage=1
    parent:  PPID:1234    /usr/bin/sudo /tmp/deploy.sh --stage=1
    uid:     uid=0/0 gid=0/0 loginuid=1000
    exe:     /usr/bin/bash
    filename: /tmp/deploy.sh  (!= exe)
```

`--tree` 时：

```
[2026-07-17 10:24:52.117008] CPU:0   PID:20732   [kprobe:free_module] rmmod nf_conntrack
    tree:
        PID:1       /sbin/init splash
          `- PID:987     /usr/lib/systemd/systemd --user
            `- PID:5011    sshd: alice [priv]
              `- PID:5013    -bash
                `- PID:20732   rmmod nf_conntrack
```

关键语义：
- 用于 `/proc` 查询的 PID 始终是 `event._pid`（perf 采样头里的 PID，即事件触发时 on-CPU 的任务），**不是** `event.pid` 字段——很多事件的 `pid` 字段含义不同（如 `sched:sched_wakeup` 的 `pid` 是被唤醒者）。
- `_pid == 0`（idle 任务）的事件被丢弃。
- 进程已退出/命名空间不匹配时，主行固定标 `(cmdline unavailable)`，上下文行标 `(unavailable)`。

### 限制

| 项 | 缓解方案 |
|---|---|
| 短命进程可能在读 `/proc` 前已退出 | shebang 已带 `batch=1`（每个事件立即处理）尽量抢先；仍失败时输出标记 `(unavailable)` |
| `environ`/`cwd` 事后可被程序自己改 | 只反映事件到达时读到的值 |
| procfs 视图受 mnt/pid namespace 影响 | 在同一 ns 内运行，或从宿主命名空间观测 |
| 换到 `openat`/`sys_enter_read` 等高频事件会刷屏 | 用内核态过滤器（`event/filter/`）或 `-C`/`-p`/`-G` 收窄 |

---

## 3. kvm_userspace_exit_latency.py - QEMU userspace exit 处理耗时

测量 KVM 退出到用户态后，QEMU 处理这次退出花了多久：
从 `kvm:kvm_userspace_exit` 到**同一 vcpu 线程**上的下一次 `ioctl(KVM_RUN)`。

### 何时使用

- 虚拟机卡顿，`kvm-exit` 显示退出耗时高且退出原因是 `IO` / `MMIO`（需要 QEMU 参与），怀疑瓶颈在 QEMU 侧而非 KVM 侧。
- 想知道哪类 userspace exit（IO / MMIO / INTR / HLT …）总体最耗时。
- 想定位是哪个 vcpu 线程被拖慢（`--per-tid`）。

与 `kvm-exit` 分析器的分工：`kvm-exit` 量的是 VM-Exit → VM-Entry 的整段延迟（含内核处理）；
本工具专门切出"返回用户态后 QEMU 处理"这一段。

### 基础用法

```bash
# shebang 已固化 -e/--order/-m，直接跑
./kvm_userspace_exit_latency.py -p <qemu_pid> [-i 1000] [options]

# 或写完整命令
perf-prof python \
    -e 'kvm:kvm_userspace_exit,syscalls:sys_enter_ioctl/cmd==0xAE80/' \
    --order -m 16 -p <qemu_pid> [-i 1000] \
    -- kvm_userspace_exit_latency.py [options]
```

事件说明：
- `KVM_RUN` 的 ioctl 号是 `_IO(KVMIO=0xAE, 0x80)` = `0xAE80`，用内核态过滤器 `cmd==0xAE80` 挑出来，避免采集进程的每一次 ioctl。
- 配对以 `common_pid`（vcpu 线程）为 key；线程在两个事件之间可能被调度走/迁移 CPU，所以**必须 `--order`**（shebang 已带）。

### 脚本自有选项

| 选项 | 说明 |
|---|---|
| `--than <dur>` | 延迟超过阈值时，额外打印起点/终点两条原始事件（全字段）。支持 `ns/us/ms/s` 后缀；裸数字按 us 解释 |
| `--top <n>` | 只显示 top N 行，默认 20 |
| `--sort <field>` | 排序字段：`total`（默认）/ `count` / `max` / `p99` |
| `--per-tid` | 按 `(vcpu-tid, reason)` 分组，而非仅按 `reason` |

常用透传参数：`-p`（QEMU 主进程，自动含所有 vcpu 线程）、`-t`（指定 vcpu 线程）、`-C`、`-i`、`-m`、`-o`。

**输出时机**：不给 `-i` 时只在退出（Ctrl-C）时打印一次全量统计；给了 `-i ms` 则每窗口打印并清零，不再单独输出累积值。

### 示例

```bash
# 一次性统计整个 qemu 进程，Ctrl-C 打印结果
./kvm_userspace_exit_latency.py -p $(pgrep -f 'qemu.*vm-name' | head -1)

# 每秒统计一次
./kvm_userspace_exit_latency.py -p 12345 -i 1000

# 只关心 >500us 的长尾，dump 每个长尾 pair 的原始事件
./kvm_userspace_exit_latency.py -p 12345 --than 500us -i 1000

# 按 vcpu 线程拆开，按 max 排序
./kvm_userspace_exit_latency.py -p 12345 --per-tid --sort max
```

推荐的渐进式流程（与延迟分析通用流程一致）：
1. `-i 1000` 看分布，确定 P99；
2. `--than <P99>` 聚焦长尾，看原始事件的时间戳/comm/cpu；
3. `--per-tid` 确认是否集中在个别 vcpu。

### 输出解读

```
2026-07-21 11:36:35
REASON              COUNT    TOTAL(us)    MIN(us)    P50(us)    P95(us)    P99(us)    MAX(us)
----------------------------------------------------------------------------------------------
IO                    120      3542.11      12.20      21.03      88.40     125.60     318.90
MMIO                   45      1120.55      18.50      22.10      67.80      92.30     140.20
INTR                    8       410.20      45.00      50.10      60.00      60.00      60.00
```

- `REASON` 是 KVM exit reason（见 `include/uapi/linux/kvm.h`）。`--per-tid` 时表头前置 `THREAD` 列。
- 所有延迟列固定 us、2 位小数，单位在列头。
- 默认按 `TOTAL` 排序——先回答"哪类退出总体最耗时"，而不是单次最慢。

### 限制

| 项 | 说明 |
|---|---|
| 事件丢失 | 丢事件会破坏配对，脚本在 `__lost__` 回调里清空 pending（宁可漏几个 pair，也不让过期条目错配到后续 ioctl）。stderr 出现 `lost N events` 时翻倍加大 `-m`（16 → 32 → 64） |
| 跨窗口的 pair | 耗时归属到"完成"窗口，与 `func_latency` 策略一致 |
| 只覆盖 userspace exit | 内核内处理的退出（大部分）不经过用户态，不在本工具范围，用 `kvm-exit` 看 |

---

## 常见组合用法

```bash
# 1) 先用分析器定界，再用工具深入
perf-prof kvm-exit -p <qemu_pid> -i 1000              # 退出耗时高？原因是 IO/MMIO？
./kvm_userspace_exit_latency.py -p <qemu_pid> -i 1000 # → 切出 QEMU 侧那一段

# 2) 定位到函数后，量它的调用树耗时
perf-prof profile -F 997 -g -p <pid>                  # 热点在哪个函数
./func_latency.sh -b /path/to/bin -u hot_func -u callee -p <pid> -i 5000

# 3) 任何"我关心的内核路径"补上"是谁在操作"
perf-prof python -e 'kprobe:<your_func>' --order -m 64 -- exec_trace.py --tree --uid
```

## 严格约束

- 使用新工具前，先执行该工具的 `-h`（脚本头部注释即完整帮助）。
- **控制运行时长用外部 `timeout N`**，不要用 `-- sleep N`：`--` 之后的参数会被脚本/perf-prof 当作 python 模块或 workload。
- 直接执行脚本（`./xxx.py`）依赖 shebang 里的 `env -S`，需要 coreutils 8.30+。老系统上改用完整命令：
  `perf-prof python -e '<shebang 里的事件串>' --order -m N -- /usr/share/perf-prof/tools/xxx.py [options]`
- 这些脚本都需要 `perf-prof` 在 `PATH` 里（shebang 直接写的是 `perf-prof`）。
