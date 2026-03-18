# syscalls - 系统调用耗时分析

multi-trace的特化版本，预配置了系统调用相关事件，专用于分析系统调用的延迟和性能。

## 概述
- **主要用途**: 分析从系统调用进入(sys_enter)到退出(sys_exit)的延迟，统计每个系统调用的性能和错误率
- **适用场景**: 系统调用性能分析、IO延迟诊断、进程阻塞原因分析
- **功能分类**: 自定义事件类，延迟分析，multi-trace派生
- **最低内核版本**: 3.10+ (支持raw_syscalls tracepoints)
- **平台支持**: x86, ARM, RISC-V, PowerPC（系统调用编号因平台而异）
- **特殊限制**:
  - 需要root权限运行
  - 强制使用`common_pid`作为key
  - 不支持`--detail`和`untraced`属性事件
- **参与联合分析**: 不支持

## 基础用法
```bash
perf-prof syscalls [OPTION...] -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit \
                   [--than ns] [--perins] [--heatmap file]
```

### OPTION
- `--watermark <0-100>`: 默认50
- `-m, --mmap-pages <N>`: 默认64页，高频场景建议128或更大
- `--order`: 根据attach方式自动决定

### FILTER OPTION
- `--user-callchain[=dwarf[,size]]`: 包含用户态调用栈。`=dwarf` 启用DWARF栈回溯
- `--kernel-callchain`: 包含内核态调用栈
- `-p, --pids <pid,...>`: 附加到进程
- `-t, --tids <tid,...>`: 附加到线程
- `-C, --cpus <cpu,...>`: 监控指定CPU
- trace event过滤器: 在事件后使用`/filter/`语法过滤系统调用

### PROFILER OPTION
- `-e, --event`: 事件选择器
  - 第一个`-e`: `raw_syscalls:sys_enter`（可添加过滤器和`stack`属性）
  - 第二个`-e`: `raw_syscalls:sys_exit`（可添加过滤器和`stack`属性）
- `--than <ns>`: 延迟阈值，只输出超过阈值的系统调用，单位：s/ms/us/ns
- `--perins`: 按线程统计系统调用延迟分布
- `--heatmap <file>`: 生成系统调用延迟热图文件

## 核心原理

### 数据模型
```
sys_enter → [key=common_pid关联] → sys_exit → 按(线程,syscall_id)统计 → 输出
```

### 事件源

**起点事件（第一个`-e`）**：
- `raw_syscalls:sys_enter`: 系统调用进入事件
  - `id`字段: 系统调用编号
  - `args`数组: 系统调用参数

**终点事件（第二个`-e`）**：
- `raw_syscalls:sys_exit`: 系统调用退出事件
  - `id`字段: 系统调用编号
  - `ret`字段: 系统调用返回值（用于检测错误）

**额外监听事件（自动配置）**：
- `sched:sched_process_free`: 进程退出事件
  - 作用: 清理退出进程未完成的系统调用（如`exit`、`exit_group`）

### 过滤器层次
1. **trace event过滤器（内核态）**: `/id==0/`过滤特定系统调用
2. **Key关联（强制）**: 使用`common_pid`关联sys_enter和sys_exit

### 事件处理

**统计分组**：
- **分组键**: `(common_pid, sys_enter.id)`
- **统计指标**: calls、total、min、avg、max、err

**错误检测**：
- 统计`sys_exit.ret < 0`的次数作为错误计数

**进程退出处理**：
- 监听`sched:sched_process_free`事件
- 自动清理该进程未完成的系统调用

## 输出

### 输出格式

**全局统计（无`--perins`）**：
```
          syscalls                calls        total(us)      min(us)      avg(us)      max(us)    err
------------------------- ------------ ---------------- ------------ ------------ ------------ ------
read(0)                          1234         5678.901        0.123        4.567       100.234     10
write(1)                          567         1234.567        0.234        2.178        50.123      5
```

**进程/线程统计（`--perins`）**：
```
thread comm             syscalls                calls        total(us)      min(us)      avg(us)      max(us)    err
------ ---------- ------------------------- ------------ ---------------- ------------ ------------ ------------ ------
1234   java       read(0)                          123          456.789        0.123        3.712        50.123      5
1234   java       write(1)                          56          123.456        0.234        2.204        25.678      2
```

### 输出字段
| 字段 | 说明 |
|------|------|
| thread | 进程/线程ID（仅`--perins`时显示） |
| comm | 进程名（仅`--perins`时显示） |
| syscalls | 系统调用名(系统调用编号) |
| calls | 系统调用总次数 |
| total(us) | 系统调用总耗时（微秒） |
| min(us) | 最小耗时（微秒） |
| avg(us) | 平均耗时（微秒） |
| max(us) | 最大耗时（微秒） |
| err | 系统调用出错次数（ret < 0） |

### 关键指标

| 指标 | 计算方法 | 异常阈值 |
|------|----------|----------|
| avg | total / calls | 取决于系统调用类型 |
| max | 最大耗时 | 远大于avg需关注 |
| err | ret < 0的次数 | > 0 需检查错误原因 |

## 应用示例

### 基础示例
```bash
# 1. 统计所有进程的系统调用性能
perf-prof syscalls -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit -i 1000

# 2. 统计特定进程的系统调用
perf-prof syscalls -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit -p 1234 -i 1000

# 3. 按线程统计系统调用性能
perf-prof syscalls -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit \
                   -p 1234 -i 1000 --perins
```

### 高级技巧
```bash
# 找出耗时超过1ms的系统调用
perf-prof syscalls -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit \
                   -p 1234 -i 1000 --perins --than 1ms

# 只分析read系统调用（x86_64: id=0）
perf-prof syscalls -e 'raw_syscalls:sys_enter/id==0/' \
                   -e 'raw_syscalls:sys_exit/id==0/' -i 1000

# 只分析文件操作相关系统调用（read、write、open、close）
perf-prof syscalls -e 'raw_syscalls:sys_enter/id>=0&&id<=3/' \
                   -e 'raw_syscalls:sys_exit/id>=0&&id<=3/' -i 1000

# 启用调用栈采样
perf-prof syscalls -e 'raw_syscalls:sys_enter//stack/' \
                   -e 'raw_syscalls:sys_exit//stack/' \
                   -p 1234 -i 1000 --than 10ms
```

### 性能优化
```bash
# 高频系统调用场景增大缓冲区
perf-prof syscalls -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit \
                   -m 256 -i 1000

# 过滤掉高频系统调用减少开销
perf-prof syscalls -e 'raw_syscalls:sys_enter/id!=0&&id!=1/' \
                   -e 'raw_syscalls:sys_exit/id!=0&&id!=1/' -i 1000
```

## 平台差异

**系统调用编号因平台而异**：

| 平台 | read | write | open | close |
|------|------|-------|------|-------|
| x86_64 | 0 | 1 | 2 | 3 |
| ARM64 | 63 | 64 | - | 57 |
| RISC-V | 63 | 64 | - | 57 |

**注意**：ARM64和RISC-V没有`open`系统调用，使用`openat`替代。

## 限制和注意事项

### 不支持的功能

| 功能 | 是否支持 | 原因 |
|------|---------|------|
| `--detail` | 不支持 | 系统调用内部事件无法通过tracepoint捕获 |
| `-k, --key` | 不支持 | 强制使用`common_pid` |
| `untraced`属性 | 不支持 | 实现限制 |
| 自定义key表达式 | 不支持 | 会导致分类统计失效 |

### 性能注意事项

**高频系统调用的影响**：
- read、write等系统调用频率极高
- 可能导致ringbuffer满、事件丢失
- 建议使用filter过滤或增大缓冲区

## 与multi-trace的区别

| 特性 | syscalls | multi-trace |
|------|----------|------------|
| **事件配置** | 固定使用sys_enter/sys_exit | 支持任意事件组合 |
| **实现类型** | 固定使用`--impl syscalls` | 支持delay/pair等多种实现 |
| **Key关联** | 强制使用`common_pid` | 支持任意表达式 |
| **系统调用分类** | 自动按系统调用类型分组 | 不支持 |
| **错误检测** | 自动统计错误（ret<0） | 不支持 |
| **进程退出处理** | 自动清理未完成的系统调用 | 无此机制 |
| **详细输出** | 不支持`--detail` | 支持 |
| **适用场景** | 专用于系统调用性能分析 | 通用的多事件关系分析 |

## 相关资源
- [multi-trace核心文档](multi-trace.md)
- [事件过滤语法参考](Event_filtering.md)
- [task-state进程状态分析](task-state.md)
