# python - Python脚本事件处理
使用Python脚本或模块处理perf事件，将事件转换为PerfEvent对象进行灵活分析。

## 概述
- **主要用途**: 将perf事件转换为PerfEvent对象，通过自定义Python脚本进行灵活的事件分析和处理，适合快速原型开发和复杂数据处理
- **适用场景**: 自定义事件处理逻辑、快速验证分析思路、复杂数据聚合、与Python生态集成、联合分析多个profiler事件
- **功能分类**: 自定义事件类，数据分析与工具，脚本处理，联合分析
- **最低内核版本**: 支持perf_event的Linux内核
- **依赖库**: libpython3 (python3-devel/python3-dev)
- **平台支持**: 所有支持perf_event的CPU架构
- **特殊限制**:
  - 需要编译时启用Python支持 (CONFIG_LIBPYTHON)
  - 需要 root 权限或 CAP_PERFMON
- **参与联合分析**: 作为分析主体，可通过 `-e profiler` 语法嵌入其他分析器（profile、page-faults等）作为事件源

## 基础用法
```bash
perf-prof python -e EVENT[,EVENT...] [选项] [--] module [args...]
```

### OPTION
- `-C, --cpus`: 指定CPU列表
- `-p, --pids`: 指定进程ID列表
- `-i, --interval <ms>`: 周期性调用`__interval__()`的间隔
- `-m, --mmap-pages`: 环形缓冲区页数
- `-N, --exit-N <N>`: 采样N个事件后退出
- `--order`: 按时间戳排序事件

### FILTER OPTION
- `-g, --call-graph`: 启用堆栈采样（默认选中内核态和用户态堆栈）
- `--user-callchain[=dwarf[,size]]`: 启用用户态堆栈，`no-` 前缀排除。`=dwarf` 启用DWARF栈回溯，比帧指针更可靠
- `--kernel-callchain`: 启用内核态堆栈，`no-` 前缀排除
- `--python-callchain`: 包含Python调用栈

### PROFILER OPTION
- `-e, --event <EVENT,...>`: **[必需]** 事件选择器
  - **tracepoint**: `sys:name[/filter/ATTR/.../]` - 系统tracepoint事件
  - **profiler**: `profiler[/option/ATTR/.../]` - 嵌入其他分析器作为事件源
  - **kprobe/kretprobe**: `kprobe:func[/filter/ATTR/.../]`
  - **uprobe/uretprobe**: `uprobe:func@"file"[/filter/ATTR/.../]`
- `module`: **[必需]** Python脚本或模块（位置参数）
- `args...`: 传递给脚本的参数（通过`sys.argv`访问，用`--`分隔）

## 核心原理

### 数据模型
```
perf事件 → PerfEvent对象 → 惰性字段解析 → 回调函数 → 用户处理
```

### 事件源

**Tracepoint事件** (`-e sys:name`):
- sample_type: TID、TIME、ID、CPU、PERIOD、RAW、CALLCHAIN（使用`-g`或`stack`属性时）
- 支持事件过滤器、通配符匹配、stack属性

**Profiler事件源** (`-e profiler`):
- 通过`-e profiler[/option/]`指定已有profiler作为事件源
- 事件字段由源profiler的sample_type决定
- 常见profiler事件源：profile、page-faults、breakpoint等

### 回调函数

| 函数名 | 调用时机 | 参数 |
|--------|----------|------|
| `__init__()` | 事件处理开始前 | 无 |
| `__exit__()` | 程序退出前 | 无 |
| `__interval__()` | 每个`-i`间隔 | 无 |
| `__print_stat__(indent)` | SIGUSR2信号 | indent: 缩进级别 |
| `__lost__(lost_start, lost_end)` | 事件丢失 | 时间戳 |
| `__sample__(event)` | 默认事件处理器 | PerfEvent对象 |
| `sys__event_name(event)` | 事件特定处理器 | PerfEvent对象 |

**处理器命名规则**:
- Tracepoint: `{sys}__{name}`（如 `sched__sched_wakeup`）
- Profiler: `{profiler_name}`（如 `profile`、`kvm_exit`，`-`转`_`）
- 使用`alias=`属性时: `{sys}__{alias}` 或直接 `{alias}`

**处理优先级**: 事件特定处理器 > `__sample__`

### PerfEvent对象

**Tracepoint事件字段**:

| 字段 | 类型 | 描述 |
|------|------|------|
| `_pid`, `_tid` | int | 进程/线程ID |
| `_time` | int | 事件时间戳(ns) |
| `_cpu` | int | CPU编号 |
| `_period` | int | 采样周期 |
| `common_type` | int | 事件类型ID |
| `_realtime` | int | 真实时间(ns，有偏差) |
| `_callchain` | list | 调用栈列表 |
| `_event` | str | 事件名称（仅`__sample__`） |
| `<field>` | 各类型 | 事件特定tep字段 |

**Profiler事件字段**:

| 字段 | 类型 | 描述 |
|------|------|------|
| `_pid`, `_tid` | int | 进程/线程ID |
| `_time` | int | 事件时间戳(ns) |
| `_cpu` | int | CPU编号 |
| `_realtime` | int | 真实时间(ns) |
| `_event` | str | profiler名称或alias |
| `<field>` | 各类型 | 由sample_type决定 |

**Profiler事件sample_type字段映射**:

| sample_type | 成员名 | Python类型 |
|-------------|--------|------------|
| PERF_SAMPLE_IP | ip | int |
| PERF_SAMPLE_TID | pid, tid | int |
| PERF_SAMPLE_TIME | time | int |
| PERF_SAMPLE_ADDR | addr | int |
| PERF_SAMPLE_CPU | cpu | int |
| PERF_SAMPLE_PERIOD | period | int |
| PERF_SAMPLE_CALLCHAIN | callchain | list |
| PERF_SAMPLE_RAW | raw | bytes |
| PERF_SAMPLE_READ | read | dict |
| PERF_SAMPLE_REGS_USER | regs_user | dict |
| PERF_SAMPLE_REGS_INTR | regs_intr | dict |
| PERF_SAMPLE_BRANCH_STACK | branch_stack | bytes |
| PERF_SAMPLE_STACK_USER | stack_user | bytes |
| PERF_SAMPLE_WEIGHT | weight | int |
| PERF_SAMPLE_DATA_SRC | data_src | int |
| PERF_SAMPLE_PHYS_ADDR | phys_addr | int |
| PERF_SAMPLE_CGROUP | cgroup | int |
| PERF_SAMPLE_AUX | aux | bytes |

**read字段格式**（按`attr->read_format`解码）:
- 非GROUP: `{'value': int, 'time_enabled': int, 'time_running': int, 'id': int, 'lost': int}`
- GROUP: `{'nr': int, 'time_enabled': int, 'time_running': int, 'cntr': [{'value': int, 'id': int, 'lost': int}, ...]}`

**regs_user/regs_intr字段格式**: `{'abi': int, 'reg_name': int, ...}`
- x86_64: ax, bx, cx, dx, si, di, bp, sp, ip, flags, cs, ss, r8-r15
- arm64: x0-x28, x29, lr, sp, pc

**`_callchain`/`callchain`字段格式**:
```python
[{'addr': int, 'symbol': str, 'offset': int, 'kernel': bool, 'dso': str}, ...]
```

### PerfEvent访问方式

```python
event._pid                          # 属性访问（推荐）
event['field']                      # 字典风格访问
event.get('field', default)         # 带默认值
'field' in event                    # 检查字段存在
event.keys() / values() / items()   # 字典遍历
event.print()                       # perf-prof格式打印
event.to_dict()                     # 转为普通字典
```

### 脚本模板生成
```bash
perf-prof python -e sched:sched_wakeup help              # 生成模板
perf-prof python -e sched:sched_wakeup help > script.py   # 保存模板
```

### 模块类型

| 类型 | 示例 |
|------|------|
| Python脚本 | `myscript.py` |
| Cython模块 | `mymodule.cpython-36m-x86_64-linux-gnu.so` |
| 模块名 | `mymodule`（在sys.path搜索） |

## 输出

### 输出格式
- 输出完全由Python脚本控制
- `event.print()` 以perf-prof格式输出事件
- 建议在`__interval__()`中输出周期性统计
- 建议在`__exit__()`中输出最终汇总

### 输出字段
| 字段 | 说明 |
|------|------|
| PerfEvent字段 | 取决于事件类型和sample_type配置 |
| _callchain | 调用栈（使用-g或stack属性时） |
| _realtime | 真实时间（有偏差，仅用于显示） |

## 应用示例

### 基础示例
```bash
# 1. 生成脚本模板
perf-prof python -e sched:sched_wakeup help > wakeup.py

# 2. 统计事件数量
perf-prof python -e sched:sched_wakeup -i 1000 counter.py

# 3. 带过滤器和堆栈
perf-prof python -e 'sched:sched_wakeup/pid>1000/' -g -i 1000 analyzer.py

# 4. 多事件分析
perf-prof python -e sched:sched_wakeup,sched:sched_switch -i 1000 latency.py

# 5. 传递参数给脚本
perf-prof python -e sched:sched_wakeup -i 1000 -- analyzer.py --threshold 100

# 6. Profiler事件源
perf-prof python -e 'profile/-F 997 -g/' -i 1000 profile_analyzer.py

# 7. 联合分析tracepoint和profiler
perf-prof python -e sched:sched_wakeup,profile -i 1000 --order combined.py
```

### 高级技巧
```bash
# 使用alias区分相同事件
perf-prof python -e 'sched:sched_wakeup//alias=wakeup1/,sched:sched_wakeup//alias=wakeup2/' script.py

# Shebang方式执行脚本
#!/usr/bin/env -S perf-prof python -e sched:sched_wakeup -i 1000

# 使用Cython编译的模块（高性能）
perf-prof python -e sched:sched_wakeup myanalyzer

# 仅对特定事件启用堆栈
perf-prof python -e 'sched:sched_wakeup//stack/' analyzer.py

# page-faults事件源（分析内存访问）
perf-prof python -e 'page-faults/-g/' -i 1000 -N 100 pagefault.py

# 处理profiler事件的read字段（计数器值）
perf-prof python -e profile -i 1000 read_counter.py
```

### 脚本编写模式
```python
# 事件计数
count = 0
def __sample__(event):
    global count
    count += 1
def __interval__():
    global count
    print(f"Events: {count}")
    count = 0

# 多事件关联（唤醒到运行延迟）
pending = {}
def sched__sched_wakeup(event):
    pending[event.pid] = event._time
def sched__sched_switch(event):
    if event.next_pid in pending:
        latency = event._time - pending.pop(event.next_pid)
        print(f"latency: {latency/1000:.1f}us")

# Profiler事件处理
def profile(event):
    callchain = event.get('callchain', [])
    if callchain:
        print(f"top: {callchain[0]['symbol']}")

# 联合分析
def sched__sched_wakeup(event):
    pass  # 处理tracepoint
def profile(event):
    pass  # 处理profiler事件
def __sample__(event):
    pass  # 未定义特定处理器的事件
```

### 性能优化
```bash
# 增大缓冲区减少事件丢失
perf-prof python -e sched:sched_wakeup -m 128 -i 1000 analyzer.py

# 使用过滤器减少事件量
perf-prof python -e 'sched:sched_wakeup/pid>1000/' -C 0-3 -i 1000 analyzer.py

# Cython加速
python setup.py build_ext --inplace
perf-prof python -e sched:sched_wakeup myanalyzer
```

### 组合使用
```bash
# 先用trace了解事件字段
perf-prof trace -e sched:sched_wakeup help

# 再用python深入分析
perf-prof python -e sched:sched_wakeup help > analyzer.py
# 编辑 analyzer.py
perf-prof python -e sched:sched_wakeup -i 1000 analyzer.py

# 多阶段分析
perf-prof top -e 'sched:sched_wakeup//key=pid/' -i 1000      # 阶段1: 定位热点
perf-prof python -e 'sched:sched_wakeup/pid==1234/' deep.py   # 阶段2: 深入分析
```

## 相关资源
- [事件过滤文档](Event_filtering.md)
- [表达式系统文档](expr.md)
- [trace 事件跟踪](trace.md)
- [top 键值统计](top.md)
- [profile CPU采样](profile.md)
