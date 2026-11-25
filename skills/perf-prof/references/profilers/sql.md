# sql - SQL聚合分析

将采样事件存储为SQL表并执行聚合查询分析。

## 概述
- **主要用途**: 将采样的事件存储到SQLite数据库表中，通过SQL查询语句对事件数据进行聚合分析、统计计算、多维度数据透视等复杂分析操作。
- **适用场景**: 需要对大量事件进行复杂的统计分析（如分组计数、聚合计算、多表关联）、需要灵活的数据查询和分析、需要持久化事件数据以供后续分析等场景。
- **功能分类**: 自定义事件类，数据分析与工具，聚合分析
- **最低内核版本**: Linux 2.6.31+（需要perf_event子系统）
- **平台支持**: x86, ARM, ARM64, RISC-V, PowerPC
- **特殊限制**:
  - 需要 root 权限或 CAP_PERFMON 能力
  - 必须指定 `--query` 或 `--output2` 之一（或同时指定）
  - 使用 `--query` 时如无 `--output2`，必须指定 `-i` 周期间隔
- **参与联合分析**: 不参与联合分析，作为独立的数据分析工具使用

## 基础用法
```bash
perf-prof sql -e EVENT [--query 'SQL_STATEMENT'] [--output2 DB_FILE] [-i INT]
```

### OPTION
- `-i, --interval <ms>`: 周期性执行SQL查询并清理数据（默认：无间隔，仅在退出时执行一次）
- `-m, --mmap-pages <pages>`: 环形缓冲区大小，默认8页，高频事件可增大
- `--order`: 按时间戳排序事件后再插入数据库（通常不需要）

### FILTER OPTION
sql分析器支持trace event过滤器，在内核态过滤事件：
- 通过事件选择器语法指定：`-e 'sched:sched_wakeup/pid>1000 && prio<10/'`
- 支持数值字段运算符：`==`, `!=`, `<`, `<=`, `>`, `>=`, `&`
- 支持字符串字段运算符：`==`, `!=`, `~`（通配符）

### PROFILER OPTION
- `-e, --event <EVENT,...>`: 事件选择器，事件名称作为SQL表名（必需）
  - 支持 `alias=` 属性指定表别名：`-e 'event//alias=table_name/'`
  - 支持 `index=` 属性手动选择索引字段（内存模式）
- `--query <SQL>`: 执行的SQL查询语句，支持多条语句用 `;` 分隔
  - 查询语句包含字符串，则需要使用单引号，对应的--query则使用双引号。如；`--query "SELECT symbolic('vec', vec) as name"`
- `--output2 <file>`: 指定SQLite数据库文件路径（未指定时使用内存数据库）
- `--verify`: 验证Virtual Table实现的正确性（仅内存模式有效）

## 核心原理

### 基本定义

**事件映射**: 每个事件自动映射为一张SQL表，表名为事件名（如 `sched_wakeup`）
- 支持通过 `alias=` 属性指定表别名
- 有别名时使用别名作为表名，否则使用事件名
- 多个相同事件必须使用不同别名区分

**字段映射**:
- 事件字段 → SQL列（数值→INTEGER，字符串→TEXT，数组→BLOB）
- 系统列: `_pid`, `_tid`, `_time`, `_cpu`, `_period`
- trace event通用字段: `common_flags`, `common_preempt_count`, `common_pid`
- BPF事件（`bpf:name`）的表没有 `common_*`，事件字段按 BTF 映射：`char[]`→TEXT，其他数组→BLOB，其余→INTEGER

**元数据表**: `event_metadata`（存储事件元信息和可用函数列表）
  - `table_name`: 事件表名（主键）
  - `event_system`: 事件系统（如 sched）
  - `event_name`: 事件名称（如 sched_wakeup）
  - `event_id`: 事件ID（对应事件 common_type 字段）
  - `filter_expression`: 过滤器表达式
  - `has_stack`: 是否启用堆栈
  - `max_stack`: 最大堆栈深度
  - `field_count`: 字段总数
  - `created_time`: 表创建时间（Unix时间戳，秒）
  - `sample_count`: 采样总数
  - `first_sample_time`: 首次采样时间（perf时间戳，纳秒，非Unix时间）
  - `last_sample_time`: 最后采样时间（perf时间戳，纳秒，非Unix时间）
  - `function_list`: 该事件可用的SQL内置函数列表（逗号分隔），例如: `"symbolic('softirq_entry.vec', vec), ksymbol(function)"`

### 数据模型
```
事件采样 → [过滤器] → 批量插入SQL表 → [周期间隔] → 执行SQL查询 → 清空表
```

### 事件源
- **自定义事件**: 通过 `-e` 选项指定tracepoint、kprobe、uprobe、BPF事件（`bpf:name`）

### 过滤器层次
1. **trace event过滤器（内核态）**: 通过事件选择器语法 `/filter/` 指定
2. **Virtual Table约束下推（用户态）**: WHERE子句约束自动优化

### 事件处理
- **排序依赖**: 无排序依赖，事件按到达顺序直接插入
- **丢事件处理**: 无特殊处理，丢失事件不会插入数据库

### 存储模式
- **文件模式**（`--output2`）: 传统SQLite表存储，适合数据持久化
- **内存模式**（无`--output2`）: Virtual Table实现，支持索引和约束下推优化

### 内存模式

- **Virtual Table约束下推**：把WHERE查询子句转换为 **trace event过滤器**，在内核态直接过滤事件。
  BPF事件只下推整数字段，生成 C 表达式编译进 BPF 程序；`_cpu` 和字符串约束不下推（只在用户态过滤）。
- **单字段索引**：内存模式支持自动建立单字段索引。
  - 统计每个 `INTEGER` 和 `TEXT` 字段被引用的次数，选择引用最多的字段作为索引字段。
  - 在自动选择的索引字段不是最优时，使用 `index=` 属性手动选择。

### SQL 内置函数

所有内置函数在事件的 `print_fmt` 包含对应格式符时**自动注册**，可通过 `event_metadata.function_list` 查询可用函数。

| 函数 | 功能 | 参数类型 | 返回值 |
|------|------|----------|--------|
| `symbolic(value)` | 数值转符号字符串（单参数） | INTEGER | 符号名或 `"UNKNOWN"` |
| `symbolic(field, value)` | 数值转符号字符串（双参数） | TEXT, INTEGER | 符号名或 `"UNKNOWN"` |
| `ksymbol(addr)` | 内核地址转符号 | INTEGER | 符号名或 `"??"` |
| `syscall(nr)` | 系统调用号转名称 | INTEGER | 系统调用名或 `"??"` |
| `ipv4_str(blob)` | IPv4地址转换（网络序） | BLOB(4) | `"x.x.x.x"` 或 `"??"` |
| `ipv4_hstr(blob)` | IPv4地址转换（主机序） | BLOB(4) | `"x.x.x.x"` 或 `"??"` |
| `ipv6_str(blob)` | IPv6地址转换 | BLOB(16) | IPv6字符串或 `"??"` |
| `ipsa_str(blob)` | sockaddr转换（网络序） | BLOB | `"IP:port"` 或 `"??"` |
| `ipsa_hstr(blob)` | sockaddr转换（主机序） | BLOB | `"IP:port"` 或 `"??"` |
| `uuid_str(blob)` | UUID转换（大端序） | BLOB(16) | UUID字符串或 `"??"` |
| `guid_str(blob)` | GUID转换（小端序） | BLOB(16) | GUID字符串或 `"??"` |
| `mac_str(blob)` | MAC地址转换 | BLOB(6) | `"xx:xx:xx:xx:xx:xx"` 或 `"??"` |

## 输出

### 输出格式

**查询结果表格**:
```
=== SQL查询语句 ===
column1  | column2  | column3
---------|----------|----------
value1   | value2   | value3
```

### 输出字段
| 字段 | 说明 |
|------|------|
| 列名 | 查询返回的列名 |
| 数据 | INTEGER左对齐，FLOAT 6位小数，TEXT左对齐，BLOB显示为`[BLOB:size]` |

### 关键指标

| 指标 | 计算方法 | 异常阈值 |
|------|----------|----------|
| COUNT(*) | 事件总数 | 根据场景判断 |
| AVG(field) | 字段平均值 | 根据场景判断 |
| SUM(field) | 字段求和 | 根据场景判断 |

## 应用示例

### 基础示例
```bash
# 1. 统计每个进程的唤醒次数
perf-prof sql -e sched:sched_wakeup -i 1000 \
  --query 'SELECT comm, COUNT(*) as count FROM sched_wakeup GROUP BY comm ORDER BY count DESC'

# 2. 统计各CPU的事件分布
perf-prof sql -e sched:sched_switch -i 1000 \
  --query 'SELECT _cpu, COUNT(*) as switches FROM sched_switch GROUP BY _cpu'

# 3. 约束下推，构建 "pid>1000" 内核态过滤器
perf-prof sql -e sched:sched_wakeup -i 1000 \
  --query 'SELECT * FROM sched_wakeup WHERE pid > 1000'

# 4. BPF事件：调度延迟>1ms的进程，"rundelay>1000000" 下推为内核态BPF过滤器
perf-prof sql -e bpf:sched_rundelay -i 1000 \
  --query 'SELECT comm, COUNT(*), MAX(rundelay) FROM sched_rundelay WHERE rundelay > 1000000 GROUP BY comm'
```

### 高级技巧
```bash
# 使用别名区分多个相同事件
perf-prof sql -e 'sched:sched_wakeup/prio<10/alias=high_prio/,sched:sched_wakeup/prio>=10/alias=low_prio/' -i 1000 \
  --query 'SELECT "high_prio" as type, COUNT(*) FROM high_prio UNION SELECT "low_prio", COUNT(*) FROM low_prio'

# 使用内置函数转换符号
perf-prof sql -e irq:softirq_entry -i 1000 \
  --query "SELECT symbolic('vec', vec) as name, COUNT(*) FROM softirq_entry GROUP BY vec"

# 使用ksymbol解析内核函数地址
perf-prof sql -e timer:hrtimer_expire_entry -i 1000 \
  --query 'SELECT ksymbol(function) as func, COUNT(*) FROM hrtimer_expire_entry GROUP BY function'
```

### 性能优化
```bash
# 内核态过滤减少开销
perf-prof sql -e 'sched:sched_wakeup/prio<10/' -i 1000 \
  --query 'SELECT comm, COUNT(*) FROM sched_wakeup GROUP BY comm'

# 高频事件增大缓冲区
perf-prof sql -e sched:sched_switch -m 64 -i 1000 \
  --query 'SELECT prev_comm, COUNT(*) FROM sched_switch GROUP BY prev_comm'

# 持久化数据供后续分析
perf-prof sql -e sched:sched_wakeup --output2 wakeup.db -i 60000
sqlite3 wakeup.db "SELECT comm, COUNT(*) FROM sched_wakeup GROUP BY comm"
```

## 相关资源
- [trace事件过滤器语法](Event_filtering.md)
