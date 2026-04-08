---
layout: default
title: "python - 脚本分析"
---

# python - Python脚本事件处理
使用Python脚本或模块处理perf事件，将事件转换为PerfEvent对象进行灵活分析。

## 概述
- **主要用途**: 将perf事件转换为PerfEvent对象，通过自定义Python脚本或模块进行灵活的事件分析和处理。适合快速原型开发、自定义分析逻辑、复杂数据处理场景。
- **适用场景**: 需要自定义事件处理逻辑、快速验证分析思路、复杂数据聚合、与Python生态集成、使用Cython加速处理、联合分析多个profiler的事件
- **功能分类**: 自定义事件类，数据分析与工具，脚本处理，联合分析
- **最低内核版本**: 支持perf_event的Linux内核
- **依赖库**: libpython3 (python3-devel/python3-dev)
- **平台支持**: 所有支持perf_event的CPU架构
- **特殊限制**: 需要编译时启用Python支持 (CONFIG_LIBPYTHON)
- **核心技术**: Python C API嵌入、事件字段惰性解析、回调函数机制

## 基础用法
```
perf-prof python -e EVENT[,EVENT...] [--] module [args...]
```

OPTION:
- `-C, --cpus`: 指定CPU列表
- `-p, --pids`: 指定进程ID列表
- `-i, --interval`: 周期性调用`__interval__()`的间隔(ms)
- `-g, --call-graph`: 启用堆栈采样（默认只采样内核态堆栈）
- `--user-callchain[=dwarf[,size]]`: 启用用户态堆栈采样。`=dwarf` 启用DWARF栈回溯，比帧指针更可靠
- `--no-kernel-callchain`: 禁用内核态堆栈采样

PROFILER OPTION:
- `-e, --event`: 指定tracepoint事件或profiler事件源（必需）
- `module`: Python脚本或模块（必需，位置参数）
- `args...`: 传递给脚本的参数，通过`sys.argv`访问

### 模块类型

支持多种Python模块类型：

| 类型 | 示例 | 说明 |
|------|------|------|
| Python脚本 | `myscript.py` | 标准Python脚本文件 |
| 脚本路径 | `/path/to/myscript.py` | 带路径的Python脚本 |
| Cython模块 | `mymodule.cpython-36m-x86_64-linux-gnu.so` | Cython编译的扩展模块 |
| 模块名 | `mymodule` | 在sys.path和当前目录搜索 |
| 共享库 | `mymodule.so` | 其他Python扩展模块 |

**注意**: 成功加载模块后，会输出模块的实际文件路径：
```
Loaded module: /path/to/mymodule.cpython-36m-x86_64-linux-gnu.so
```

### 示例
```bash
# 基础用法：统计sched_wakeup事件
perf-prof python -e sched:sched_wakeup counter.py

# 使用Cython编译的模块（高性能场景）
perf-prof python -e sched:sched_wakeup myanalyzer.cpython-36m-x86_64-linux-gnu.so

# 只指定模块名（在当前目录和sys.path中搜索）
perf-prof python -e sched:sched_wakeup myanalyzer

# 多事件分析
perf-prof python -e sched:sched_wakeup,sched:sched_switch -i 1000 analyzer.py

# 带过滤器的事件
perf-prof python -e 'sched:sched_wakeup/pid>1000/' -C 0-3 filter_events.py

# 启用堆栈采样
perf-prof python -e sched:sched_wakeup -g callstack.py

# 仅对特定事件启用堆栈（使用stack属性）
perf-prof python -e 'sched:sched_wakeup//stack/' analyzer.py

# 传递参数给脚本（使用--分隔perf-prof选项和脚本参数）
perf-prof python -e sched:sched_wakeup -i 1000 -- analyzer.py --threshold 100 --output result.txt

# 使用profiler事件源：处理profile采样事件
perf-prof python -e profile -i 1000 profile_analyzer.py

# 联合分析：同时处理tracepoint和profiler事件
perf-prof python -e sched:sched_wakeup,profile -i 1000 combined.py
```

### 脚本参数

脚本可以接收自己的命令行参数，通过`sys.argv`访问：

```bash
perf-prof python -e sched:sched_wakeup -- script.py --foo bar -n 10
```

在脚本中：
```python
import sys
print(sys.argv)  # ['script.py', '--foo', 'bar', '-n', '10']

# 使用argparse解析参数
import argparse
parser = argparse.ArgumentParser()
parser.add_argument('--foo', default='default')
parser.add_argument('-n', type=int, default=1)
args = parser.parse_args()
print(args.foo)  # 'bar'
print(args.n)    # 10
```

**注意**: 使用`--`分隔perf-prof选项和脚本参数，避免脚本参数被perf-prof解析。

### Shebang方式执行脚本

可以使用 shebang 方式直接执行 Python 脚本，无需显式调用 `perf-prof python`：

```python
#!/usr/bin/env -S perf-prof python -e sched:sched_wakeup -i 1000

def __init__():
    print("Script started")

def __sample__(event):
    event.print()
```

```bash
chmod +x script.py
./script.py
```

**工作原理**：
- `env -S` 会拆分 perf-prof 的选项参数
- perf-prof 自动区分哪些是 python 分析单元的参数，哪些是脚本的参数

#### 参数分隔规则

根据是否在 shebang 行末尾使用 `--` 分隔符，有不同的参数处理行为：

**场景1：不使用 `--` 分隔符**

```python
#!/usr/bin/env -S perf-prof python -e sched:sched_wakeup

def __sample__(event):
    pass
```

执行方式：
```bash
./script.py -i 1000 --threshold 50
```

此时：
- `-i 1000` 会被 perf-prof 解析为 python 分析单元的 interval 参数
- `--threshold 50` 会传递给脚本（前提是不与 python 分析单元参数重叠）
- **约束**：脚本参数不能与 python 分析单元的参数名重叠

**场景2：使用 `--` 分隔符**

```python
#!/usr/bin/env -S perf-prof python -e sched:sched_wakeup --

def __sample__(event):
    pass
```

执行方式：
```bash
./script.py -i 1000 --threshold 50
```

此时：
- 所有命令行参数（`-i 1000 --threshold 50`）都传递给脚本
- 脚本参数可以与 python 分析单元参数重名（如 `-i`）
- python 分析单元的参数只能在 shebang 行中指定

#### 使用建议

| 场景 | shebang 末尾 | 说明 |
|------|-------------|------|
| 脚本参数与分析单元参数无重叠 | 不加 `--` | 命令行可混合指定分析单元参数和脚本参数 |
| 脚本参数可能与分析单元参数重叠 | 加 `--` | 命令行参数全部传给脚本 |
| 需要在命令行调整分析单元参数 | 不加 `--` | 如动态调整 `-i`, `-C`, `-p` 等 |
| 分析单元参数固定 | 加 `--` | 参数在 shebang 行写死，脚本参数更灵活 |


## 核心原理

**基本定义**
- **PerfEvent对象**: 每个perf事件转换为PerfEvent对象，支持惰性字段解析，提高性能
- **回调函数**: Python脚本中定义的特定函数，在相应时机被调用
- **事件处理器**: 可以定义通用处理器或事件特定处理器

**数据模型**
```
perf事件 → PerfEvent对象 → 惰性字段解析 → 回调函数 → 用户处理
```

### 事件源

python profiler 支持两类事件源：

#### Tracepoint事件源

- **sample_type**:
  - `PERF_SAMPLE_TID`: 进程/线程ID
  - `PERF_SAMPLE_TIME`: 事件时间戳
  - `PERF_SAMPLE_ID`: 事件ID（用于识别事件类型）
  - `PERF_SAMPLE_CPU`: CPU编号
  - `PERF_SAMPLE_PERIOD`: 采样周期
  - `PERF_SAMPLE_RAW`: 原始事件数据
  - `PERF_SAMPLE_CALLCHAIN`: 调用栈（使用`-g`或`stack`属性时启用）

- **自定义事件**:
  - 通过`-e`选项指定tracepoint事件
  - 支持事件过滤器: `-e 'sched:sched_wakeup/pid>1000/'`
  - 支持多个事件: `-e event1,event2,event3`
  - 支持stack属性启用堆栈: `-e 'event//stack/'`

#### Profiler事件源（dev_tp）

通过`-e profiler`指定一个已有的profiler作为事件源，profiler产生的事件通过PERF_RECORD_DEV转发到python进行处理。

- **指定方式**: `-e profiler[/option/ATTR/...]`
  - `profiler` 为已注册的profiler名称（如 `profile`, `kvm-exit`, `task-state` 等）
  - `/option/` 指定profiler支持的选项参数
  - `ATTR` 指定属性（如 `alias=`, `key=` 等）

- **事件字段**: profiler事件的字段由源profiler的`sample_type`决定，sample_type各bit映射为以下成员：

  | sample_type bit | 成员名 | 类型 | 说明 |
  |-----------------|--------|------|------|
  | `PERF_SAMPLE_IDENTIFIER` | identifier | u64 | 样本标识符 |
  | `PERF_SAMPLE_IP` | ip | u64 | 指令指针 |
  | `PERF_SAMPLE_TID` | pid, tid | u32, u32 | 进程ID和线程ID |
  | `PERF_SAMPLE_TIME` | time | u64 | 事件时间戳 |
  | `PERF_SAMPLE_ADDR` | addr | u64 | 内存地址 |
  | `PERF_SAMPLE_ID` | id | u64 | 样本ID |
  | `PERF_SAMPLE_STREAM_ID` | stream_id | u64 | 流ID |
  | `PERF_SAMPLE_CPU` | cpu | u32 | CPU编号 |
  | `PERF_SAMPLE_PERIOD` | period | u64 | 采样周期 |
  | `PERF_SAMPLE_CALLCHAIN` | callchain | list | 调用栈（与tracepoint的`_callchain`格式相同） |
  | `PERF_SAMPLE_RAW` | raw | bytes | 原始数据 |
  | `PERF_SAMPLE_READ` | read | dict | 计数器读取值（见下方说明） |
  | `PERF_SAMPLE_BRANCH_STACK` | branch_stack | bytes | 分支栈 `{ u64 nr; lbr[nr]; }` |
  | `PERF_SAMPLE_REGS_USER` | regs_user | dict | 用户态寄存器 `{'abi': int, 'reg': int, ...}` |
  | `PERF_SAMPLE_STACK_USER` | stack_user | bytes | 用户态栈数据 `{ u64 size; data[size]; }` |
  | `PERF_SAMPLE_WEIGHT` | weight | u64 | 采样权重 |
  | `PERF_SAMPLE_WEIGHT_STRUCT` | weight | u64 | 采样权重结构（与WEIGHT互斥） |
  | `PERF_SAMPLE_DATA_SRC` | data_src | u64 | 数据来源 |
  | `PERF_SAMPLE_TRANSACTION` | transaction | u64 | 事务 |
  | `PERF_SAMPLE_REGS_INTR` | regs_intr | dict | 中断时寄存器 `{'abi': int, 'reg': int, ...}` |
  | `PERF_SAMPLE_PHYS_ADDR` | phys_addr | u64 | 物理地址 |
  | `PERF_SAMPLE_AUX` | aux | bytes | AUX数据 `{ u64 size; data[size]; }` |
  | `PERF_SAMPLE_CGROUP` | cgroup | u64 | Cgroup ID |
  | `PERF_SAMPLE_DATA_PAGE_SIZE` | data_page_size | u64 | 数据页大小 |
  | `PERF_SAMPLE_CODE_PAGE_SIZE` | code_page_size | u64 | 代码页大小 |

  寄存器字段以字典形式返回，`abi`表示ABI版本，其余key为架构相关的寄存器名：
  - x86_64: `ax,bx,cx,dx,si,di,bp,sp,ip,flags,cs,ss,ds,es,fs,gs,r8-r15`
  - i386: `ax,bx,cx,dx,si,di,bp,sp,ip,flags,cs,ss,ds,es,fs,gs`
  - arm64: `x0-x28,x29,lr,sp,pc`

  `read`字段按`attr->read_format`解码为字典，格式取决于是否使用`PERF_FORMAT_GROUP`：
  - 非GROUP: `{'value': int, 'time_enabled': int, 'time_running': int, 'id': int, 'lost': int}`
  - GROUP: `{'nr': int, 'time_enabled': int, 'time_running': int, 'cntr': [{'value': int, 'id': int, 'lost': int}, ...]}`

  其中`time_enabled`/`time_running`/`id`/`lost`仅在对应`PERF_FORMAT_*`标志启用时出现。

- **示例**:
  ```bash
  # 使用profile作为事件源，处理CPU采样事件
  perf-prof python -e profile -i 1000 analyzer.py

  # profile事件源指定选项参数
  perf-prof python -e 'profile/-F 997 -g/' -i 1000 analyzer.py

  # 联合分析tracepoint和profiler事件
  perf-prof python -e sched:sched_wakeup,profile -i 1000 combined.py
  ```

### 事件处理

**回调函数**

| 函数名 | 调用时机 | 参数 |
|--------|----------|------|
| `__init__()` | 事件处理开始前调用一次 | 无 |
| `__exit__()` | 程序退出前调用一次 | 无 |
| `__interval__()` | 每个`-i`间隔调用 | 无 |
| `__print_stat__(indent)` | 收到SIGUSR2信号时调用 | indent: 缩进级别 |
| `__lost__(lost_start, lost_end)` | 事件丢失时调用 | lost_start: lost前最后一个采样时间戳<br>lost_end: lost后第一个采样时间戳<br>（未启用--order时均为0） |
| `__sample__(event)` | 默认事件处理器 | event: PerfEvent对象 |
| `sys__event_name(event)` | 事件特定处理器 | event: PerfEvent对象 |

**事件特定处理器命名规则**
- **Tracepoint事件**: 格式 `{sys}__{name}`，其中`sys`是事件类别，`name`是事件名称
  - 特殊字符`-`、`.`、`:`转换为`_`
  - 示例: `sched:sched_wakeup` → `sched__sched_wakeup(event)`
  - 示例: `sched:sched-migrate` → `sched__sched_migrate(event)`
- **Profiler事件**: 格式 `{profiler_name}`，直接使用profiler名称
  - 特殊字符`-`、`.`、`:`转换为`_`
  - 示例: `profile` → `profile(event)`
  - 示例: `kvm-exit` → `kvm_exit(event)`
  - 示例: `task-state` → `task_state(event)`

**使用alias区分相同事件**
- 当通过`-e`指定多个相同事件时，使用`alias=`属性区分
- 处理器函数名使用`{sys}__{alias}`格式
- `_event`字段也使用`{sys}:{alias}`格式
- 示例:
  ```bash
  # 指定两个相同事件，使用不同alias
  perf-prof python -e 'sched:sched_wakeup//alias=wakeup1/,sched:sched_wakeup//alias=wakeup2/' script.py
  ```
  对应的Python处理器:
  ```python
  def sched__wakeup1(event):
      """处理第一个sched_wakeup事件"""
      pass

  def sched__wakeup2(event):
      """处理第二个sched_wakeup事件"""
      pass

  def __sample__(event):
      # event._event 将是 "sched:wakeup1" 或 "sched:wakeup2"
      pass
  ```

**处理优先级**
1. 优先调用事件特定处理器 `sys__event_name(event)`
2. 如果未定义，调用默认处理器 `__sample__(event)`
3. 默认处理器的event对象包含额外的`_event`字段

**PerfEvent对象字段**

PerfEvent是一个惰性求值的事件对象，直接访问字段比字典更高效。两种事件类型的字段集不同：

**Tracepoint事件字段** (`-e sys:name`)

| 字段名 | 类型 | 描述 | 求值方式 |
|--------|------|------|----------|
| `_pid` | int | 进程ID | 直接访问 |
| `_tid` | int | 线程ID | 直接访问 |
| `_time` | int | 事件时间戳(ns)，用于延迟计算 | 直接访问 |
| `_cpu` | int | CPU编号 | 直接访问 |
| `_period` | int | 采样周期 | 直接访问 |
| `common_type` | int | trace_entry的common_type（事件类型ID） | 从raw数据读取 |
| `common_flags` | int | trace_entry的common_flags | 从raw数据读取 |
| `common_preempt_count` | int | trace_entry的preempt_count | 从raw数据读取 |
| `common_pid` | int | trace_entry的common_pid | 从raw数据读取 |
| `_realtime` | int | 真实时间(ns，Unix纪元)，仅用于显示，有偏差不可用于延迟计算 | 惰性计算 |
| `_callchain` | list | 调用栈列表（使用`-g`或`stack`属性时） | 惰性计算 |
| `_event` | str | 事件名称（仅`__sample__`） | 惰性计算 |
| `<field>` | 各类型 | 事件特定tep字段 | 惰性解析 |

**Profiler事件字段** (`-e profiler`)

| 字段名 | 类型 | 描述 | 求值方式 |
|--------|------|------|----------|
| `_pid` | int | 进程ID | 直接访问 |
| `_tid` | int | 线程ID | 直接访问 |
| `_time` | int | 事件时间戳(ns) | 直接访问 |
| `_cpu` | int | CPU编号 | 直接访问 |
| `_realtime` | int | 真实时间(ns，Unix纪元) | 惰性计算 |
| `_event` | str | profiler名称或alias | 惰性计算 |
| `<field>` | 各类型 | 由源profiler的sample_type决定的字段 | 惰性解析 |

Profiler事件的 `<field>` 字段取决于源profiler的sample_type配置，常见字段包括：`ip`（指令指针）、`period`（采样周期）、`callchain`（调用栈）等。

**PerfEvent访问方式**

PerfEvent对象支持多种访问方式：

```python
# 属性访问（推荐，更高效）
pid = event._pid
cpu = event._cpu
comm = event.comm

# 字典风格访问
pid = event['_pid']
comm = event['comm']

# 使用get方法，支持默认值
comm = event.get('comm', '<unknown>')  # 字段不存在时返回默认值
pid = event.get('nonexistent', -1)     # 返回 -1

# 检查字段是否存在
if 'comm' in event:
    print(event.comm)

# 获取字段数量
print(len(event))

# 获取所有字段名
print(event.keys())

# 获取所有字段值
print(event.values())

# 获取所有字段键值对
print(event.items())

# 迭代所有字段
for name, value in event:
    print(f"{name}: {value}")

# 转换为普通字典
d = event.to_dict()

# 打印事件（perf-prof格式）
event.print()                           # 完整输出
event.print(timestamp=False)            # 不显示时间戳
event.print(callchain=False)            # 不显示调用栈

# 字符串表示
print(str(event))    # 字典风格输出
print(repr(event))   # <PerfEvent sched:sched_wakeup cpu=0 pid=1234 time=...>
                     # 或 <PerfEvent profile cpu=0 pid=1234 time=...>（profiler事件）

# 计算哈希（可用于去重）
h = hash(event)
```

**`_callchain`字段格式**

当使用`-g`选项或`stack`属性启用堆栈采样时，事件会包含`_callchain`字段。它是一个列表，每个元素是一个栈帧字典：

```python
_callchain = [
    {
        'addr': 0xffffffff81234567,  # 指令地址 (int)
        'symbol': 'schedule',        # 函数名 (str)
        'offset': 0x42,              # 函数内偏移 (int)
        'kernel': True,              # 是否内核态 (bool)
        'dso': '[kernel.kallsyms]'   # DSO名称 (str)
    },
    # ... 更多栈帧，从栈顶到栈底
]
```

栈帧字典字段说明：

| 字段 | 类型 | 描述 |
|------|------|------|
| `addr` | int | 指令指针地址 |
| `symbol` | str | 函数名，未知时为"Unknown" |
| `offset` | int | 相对于函数起始地址的偏移 |
| `kernel` | bool | True表示内核态，False表示用户态 |
| `dso` | str | DSO名称，内核为"[kernel.kallsyms]"，用户态为库/可执行文件路径 |

**字段类型映射**

Tracepoint事件：
- 数值字段 → Python int
- 字符串字段 → Python str
- 数组字段 → Python bytes
- 动态字符串 → Python str

Profiler事件（基于member size）：
- u64/u32/u16/u8 字段 → Python int
- callchain字段 → Python list（与tracepoint的`_callchain`格式相同）
- raw字段 → Python bytes（原始trace数据）
- branch_stack字段 → Python bytes（`{ u64 nr; lbr[nr]; }`）
- stack_user字段 → Python bytes（`{ u64 size; data[size]; }`）
- aux字段 → Python bytes（`{ u64 size; data[size]; }`）
- regs_user/regs_intr字段 → Python dict（`{'abi': int, 'reg_name': int, ...}`）
- read字段 → Python dict（按read_format解码，见上方说明）

### 内建模块: perf_prof

python profiler 提供了一个内建的 `perf_prof` 模块，包含 PerfEvent 类型定义。该模块会自动导入，用户脚本无需显式导入即可使用 PerfEvent 对象。

**查看帮助**
```python
import perf_prof
help(perf_prof.PerfEvent)  # 查看PerfEvent类型的完整文档
```

**PerfEvent类型**

`perf_prof.PerfEvent` 是事件处理器接收的事件对象类型。主要特性：

| 方法/属性 | 描述 |
|-----------|------|
| `event._pid`, `event._tid`, ... | 直接访问字段（高效） |
| `event['field']` | 字典风格访问 |
| `event.get(field, default=None)` | 获取字段值，不存在时返回默认值 |
| `'field' in event` | 检查字段是否存在 |
| `len(event)` | 获取字段数量 |
| `event.keys()` | 获取所有字段名列表 |
| `event.values()` | 获取所有字段值列表 |
| `event.items()` | 获取所有(字段名, 值)元组列表 |
| `for name, value in event` | 迭代所有字段 |
| `event.print(timestamp=True, callchain=True)` | 以perf-prof格式打印事件 |
| `event.to_dict()` | 转换为普通Python字典 |
| `str(event)`, `repr(event)` | 字符串表示 |
| `hash(event)` | 计算事件哈希值 |

**event.print() 方法**

```python
event.print(timestamp=True, callchain=True)
```

参数：
- `timestamp`: 是否打印时间戳（默认 True）
- `callchain`: 是否打印调用栈（默认 True，需要事件包含 `_callchain` 字段）

对于tracepoint事件，输出格式：
```
YYYY-MM-DD HH:MM:SS.uuuuuu            comm   pid .... [cpu] time.us: sys:name: fields
    addr symbol+offset (dso)
    ...
```

对于profiler事件，调用源profiler的打印方法（`prof_dev_print_event`），输出格式由源profiler决定。

**使用示例**
```python
def sched__sched_wakeup(event):
    # 完整输出：时间戳 + 事件 + 调用栈
    event.print()

def sched__sched_switch(event):
    # 只输出事件，不显示时间戳
    event.print(timestamp=False)

def __sample__(event):
    # 输出事件和时间戳，不显示调用栈
    event.print(callchain=False)
```

**输出示例**
```
2024-01-15 10:30:45.123456           mysqld  12345 d... [003] 1705298445.123456: sched:sched_wakeup: comm=worker pid=12346 prio=120 target_cpu=003
    ffffffff81234567 try_to_wake_up+0x42 ([kernel.kallsyms])
    ffffffff81234890 wake_up_process+0x15 ([kernel.kallsyms])
    ffffffff812789ab worker_thread+0x123 ([kernel.kallsyms])
```

### 状态统计
- **信号处理**
  - SIGUSR2: 调用`__print_stat__(indent)`函数

## 输出

### 输出格式
- 输出完全由Python脚本控制
- 脚本可以使用`print()`输出任意格式
- 建议在`__interval__()`中输出周期性统计
- 建议在`__exit__()`中输出最终汇总

## 分析方法

### 脚本模板生成

使用 `help` 关键字可以生成Python脚本模板，包含所有回调函数和事件处理器的框架代码：

```bash
# 生成基础脚本模板
perf-prof python -e sched:sched_wakeup help

# 生成多事件脚本模板
perf-prof python -e sched:sched_wakeup,sched:sched_switch help

# 使用alias区分相同事件的模板
perf-prof python -e 'sched:sched_wakeup//alias=wakeup1/,sched:sched_wakeup//alias=wakeup2/' help

# 保存模板到文件
perf-prof python -e sched:sched_wakeup help > my_script.py
```

**模板特性**：
- **可选函数标记**: 标记为 `[OPTIONAL]` 的函数可以安全删除
- **模块导入**: 支持 `import` 导入任意Python模块
- **异常处理**: 函数中抛出的异常会被打印，但不会中断事件处理

**生成的模板包含**：
- **通用回调函数**: `__init__()`, `__exit__()`, `__interval__()`, `__print_stat__()`, `__lost__(lost_start, lost_end)` （均为可选）
- **事件处理器**: 根据指定的事件自动生成 `sys__event_name()` 或 `sys__alias()` 函数
- **字段文档**: 每个事件处理器的docstring中列出所有可用字段及其类型
- **示例代码**: 展示如何访问常用字段和导入模块

**模板结构示例**:
```python
# =============================================================================
# Python Script Template for perf-prof python
# =============================================================================
#
# Save this template to a .py file and customize as needed.
# Functions marked [OPTIONAL] can be safely deleted if not needed.
# Exceptions raised in functions will be printed but won't stop processing.
#
# PerfEvent object fields:
#
#   Tracepoint events (-e sys:name):
#   _pid, _tid    : Process/thread ID (int)
#   _time         : Event timestamp in nanoseconds (int)
#   _cpu          : CPU number (int)
#   _period       : Sample period (int)
#   common_type, common_flags, common_preempt_count, common_pid : trace_entry fields
#   _realtime     : Wall clock time in ns since Unix epoch (int, lazy computed)
#                   Note: Has drift, only for display, not for latency calc
#   _callchain    : Call stack list (when -g or stack attribute is set, lazy computed)
#   _event        : Event name with alias if set (str, only in __sample__, lazy computed)
#   <field>       : Event-specific fields (int/str/bytes, lazy computed)
#
#   Profiler events (-e profiler):
#   _pid, _tid    : Process/thread ID (int)
#   _time         : Event timestamp in nanoseconds (int)
#   _cpu          : CPU number (int)
#   _realtime     : Wall clock time in ns since Unix epoch (int, lazy computed)
#   _event        : Event name with alias if set (str, only in __sample__, lazy computed)
#   <field>       : Profiler-specific fields based on sample_type (lazy computed)
#
# PerfEvent access methods:
#   event.field or event['field']  - Access field value
#   event.get(field, default=None) - Get field with default fallback
#   'field' in event               - Check if field exists
#   len(event)                     - Number of fields
#   event.keys(), event.values(), event.items()  - Dict-like access
#   for field, value in event      - Iterate over fields
#   event.print(timestamp=True, callchain=True)  - Print in perf-prof format
#   event.to_dict()                - Convert to regular Python dict
#   str(event), repr(event)        - String representations
#   hash(event)                    - Hash of entire perf event
#
# =============================================================================

# Import other modules as needed (examples)
# import json
# import time
# from collections import defaultdict, Counter

# Global variables for statistics
event_count = 0

# [OPTIONAL] Delete if no initialization needed
def __init__():
    """Called once before event processing starts."""
    pass

# [OPTIONAL] Delete if no cleanup/summary needed
def __exit__():
    """Called once before program exit."""
    pass

# [OPTIONAL] Delete if -i interval not used
def __interval__():
    """Called at each -i interval."""
    pass

# Event-specific handler (higher priority than __sample__)
def sched__sched_wakeup(event):
    """
    Handler for sched:sched_wakeup
    event is a PerfEvent object with lazy field evaluation.

    Event-specific fields:
        comm : str
        pid : int
        prio : int
        target_cpu : int
    """
    global event_count
    event_count += 1

    # Access common fields
    pid = event._pid
    time_ns = event._time

    # Access event-specific fields
    # comm = event.comm
    # target_cpu = event.target_cpu

    # Example: print event
    # event.print()

# [OPTIONAL] Delete if using event-specific handlers for all events
def __sample__(event):
    """
    Default handler for events without specific handlers.
    event is a PerfEvent object. The _event field has format 'sys:name' or 'sys:alias'.
    """
    global event_count
    event_count += 1

    event_name = event._event
    cpu = event._cpu

    # Example: print event
    # event.print()
```

### 基础分析方法
1. 确定要分析的tracepoint事件或profiler事件源
2. 使用`perf-prof python -e EVENT help`生成脚本模板
3. 根据模板编写Python脚本实现分析逻辑
4. 运行分析器收集数据

### 数据驱动分析
- **事件探索**: 先用简单脚本打印事件内容，了解数据结构
- **增量开发**: 从简单统计开始，逐步增加复杂逻辑
- **实时验证**: 利用`-i`选项实时查看中间结果

## 应用示例

### 基础示例：事件计数
```python
# counter.py - 统计事件数量
count = 0

def __sample__(event):
    global count
    count += 1

def __interval__():
    global count
    print(f"Events: {count}")
    count = 0

def __exit__():
    print("Done")
```

```bash
perf-prof python -e sched:sched_wakeup -i 1000 counter.py
```

### 事件特定处理器示例
```python
# wakeup_analyzer.py - 分析唤醒事件
wakeups = {}

def sched__sched_wakeup(event):
    """处理sched:sched_wakeup事件"""
    comm = event.get('comm', '<unknown>')
    pid = event.pid
    target_cpu = event.target_cpu

    key = (comm, pid)
    if key not in wakeups:
        wakeups[key] = {'count': 0, 'cpus': set()}
    wakeups[key]['count'] += 1
    wakeups[key]['cpus'].add(target_cpu)

def __interval__():
    print(f"{'COMM':<16} {'PID':>8} {'COUNT':>8} {'CPUS'}")
    for (comm, pid), data in sorted(wakeups.items(), key=lambda x: -x[1]['count'])[:10]:
        cpus = ','.join(map(str, sorted(data['cpus'])))
        print(f"{comm:<16} {pid:>8} {data['count']:>8} {cpus}")
    print()
    wakeups.clear()
```

### 多事件关联分析
```python
# latency.py - 计算唤醒到运行的延迟
pending = {}  # pid -> wakeup_time
latencies = []

def sched__sched_wakeup(event):
    pid = event.pid
    pending[pid] = event._time

def sched__sched_switch(event):
    next_pid = event.next_pid
    if next_pid in pending:
        latency = event._time - pending[next_pid]
        latencies.append(latency / 1000)  # 转换为微秒
        del pending[next_pid]

def __interval__():
    if latencies:
        avg = sum(latencies) / len(latencies)
        max_lat = max(latencies)
        print(f"Latency: avg={avg:.1f}us max={max_lat:.1f}us samples={len(latencies)}")
        latencies.clear()
```

```bash
perf-prof python -e sched:sched_wakeup,sched:sched_switch -i 1000 latency.py
```

### 堆栈采样分析
```python
# callstack_analyzer.py - 分析唤醒事件的调用栈
from collections import Counter

wakeup_stacks = Counter()

def sched__sched_wakeup(event):
    """分析谁唤醒了进程"""
    callchain = event.get('_callchain', [])
    if not callchain:
        return

    # 构建调用栈字符串（只取内核部分）
    stack = []
    for frame in callchain:
        if frame['kernel']:
            stack.append(frame['symbol'])

    if stack:
        # 使用调用栈作为key进行聚合
        stack_key = ' <- '.join(stack[:5])  # 只取前5层
        wakeup_stacks[stack_key] += 1

def __interval__():
    print("\n=== Top Wakeup Stacks ===")
    for stack, count in wakeup_stacks.most_common(10):
        print(f"{count:>6}  {stack}")
    wakeup_stacks.clear()

def __exit__():
    print("\n=== Final Wakeup Stack Summary ===")
    for stack, count in wakeup_stacks.most_common(20):
        print(f"{count:>6}  {stack}")
```

```bash
# 启用堆栈采样
perf-prof python -e sched:sched_wakeup -g -i 1000 callstack_analyzer.py

# 或使用stack属性
perf-prof python -e 'sched:sched_wakeup//stack/' -i 1000 callstack_analyzer.py

# 同时采样用户态堆栈（FP模式）
perf-prof python -e sched:sched_wakeup -g --user-callchain -i 1000 callstack_analyzer.py

# 使用DWARF模式采样用户态堆栈（更可靠）
perf-prof python -e sched:sched_wakeup -g --user-callchain=dwarf -i 1000 callstack_analyzer.py
```

### Profiler事件源示例

**处理profile采样事件**
```python
# profile_analyzer.py - 分析CPU采样的热点函数
from collections import Counter

hot_functions = Counter()

def profile(event):
    """处理profile profiler的采样事件"""
    ip = event.get('ip', 0)
    callchain = event.get('callchain', [])

    # 统计热点函数（取调用栈顶部）
    if callchain:
        top_frame = callchain[0]
        hot_functions[top_frame['symbol']] += 1

def __interval__():
    print(f"\n{'FUNCTION':<40} {'COUNT':>8}")
    print('-' * 50)
    for func, count in hot_functions.most_common(10):
        print(f"{func:<40} {count:>8}")
    hot_functions.clear()
```

```bash
perf-prof python -e 'profile/-F 997 -g/' -i 1000 profile_analyzer.py
```

**联合分析tracepoint和profiler事件**
```python
# combined.py - 同时分析调度事件和CPU采样
wakeup_count = 0
sample_count = 0

def sched__sched_wakeup(event):
    """处理tracepoint事件"""
    global wakeup_count
    wakeup_count += 1

def profile(event):
    """处理profiler事件"""
    global sample_count
    sample_count += 1
    # 打印事件（使用源profiler的格式）
    # event.print()

def __interval__():
    print(f"Wakeups: {wakeup_count}, CPU samples: {sample_count}")

def __sample__(event):
    """未定义特定处理器的事件走这里"""
    # event._event 可用于区分事件类型
    pass
```

```bash
perf-prof python -e sched:sched_wakeup,profile -i 1000 combined.py
```

### 高级技巧

**与Python库集成**
```python
# 使用collections进行高效统计
from collections import defaultdict, Counter

events = Counter()

def __sample__(event):
    events[event._event] += 1

def __interval__():
    for name, count in events.most_common():
        print(f"{name}: {count}")
    events.clear()
```

**状态持久化**
```python
import json

stats = {'total': 0, 'by_cpu': {}}

def __sample__(event):
    stats['total'] += 1
    cpu = str(event._cpu)
    stats['by_cpu'][cpu] = stats['by_cpu'].get(cpu, 0) + 1

def __exit__():
    with open('stats.json', 'w') as f:
        json.dump(stats, f, indent=2)
    print(f"Stats saved to stats.json")
```

### 性能优化
- **缓冲区大小**: 使用`-m`参数增大缓冲区，减少事件丢失
- **过滤器优化**: 在事件级别使用过滤器，减少进入Python的事件数量
- **批量处理**: 在`__interval__()`中处理聚合数据，而非每个事件都输出
- **Cython加速**: 对性能敏感的处理逻辑，可使用Cython编译为扩展模块

### 使用Cython模块

对于高性能场景，可以使用Cython将Python脚本编译为C扩展模块，获得更好的执行效率。

**创建Cython模块**

1. 创建 `.pyx` 文件（与普通Python脚本语法相同）:
```python
# myanalyzer.pyx
cdef int count = 0

def __sample__(event):
    global count
    count += 1

def __interval__():
    global count
    print(f"Events: {count}")
    count = 0
```

2. 创建 `setup.py`:
```python
from setuptools import setup
from Cython.Build import cythonize

setup(
    ext_modules=cythonize("myanalyzer.pyx")
)
```

3. 编译模块:
```bash
python setup.py build_ext --inplace
```

4. 使用编译后的模块:
```bash
# 指定完整文件名
perf-prof python -e sched:sched_wakeup myanalyzer.cpython-36m-x86_64-linux-gnu.so

# 或只指定模块名（自动搜索）
perf-prof python -e sched:sched_wakeup myanalyzer
```

**Cython优势**:
- 执行速度比纯Python快数倍
- 可使用静态类型声明进一步优化
- 支持直接调用C库
- 与纯Python脚本完全兼容

### 参数调优
- **间隔调优**: `-i`设置合适的间隔，平衡实时性和性能
- **CPU绑定**: 使用`-C`限制监控范围，减少事件量

### 组合使用
- **与trace配合**: 先用`perf-prof trace -e EVENT help`了解事件字段
- **与top配合**: 用`perf-prof top`快速定位热点，再用python深入分析
- **多阶段分析**: 第一阶段收集数据到文件，第二阶段离线分析

## 相关资源
- [事件基础文档](../events/)
- [sample_type采样类型说明](../sample_type.md)
- [选项参数完整参考](../main_options.md)
- [trace分析器](./trace.md) - 了解事件字段
