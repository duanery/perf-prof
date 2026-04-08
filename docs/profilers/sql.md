# sql - SQL聚合分析

将采样事件存储为SQL表并执行聚合查询分析。

## 概述
- **主要用途**: 将采样的事件存储到SQLite数据库表中，通过SQL查询语句对事件数据进行聚合分析、统计计算、多维度数据透视等复杂分析操作。
- **适用场景**: 需要对大量事件进行复杂的统计分析（如分组计数、聚合计算、多表关联）、需要灵活的数据查询和分析、需要持久化事件数据以供后续分析等场景。
- **功能分类**:
  - 按事件依赖程度：**自定义事件类** - 需要用户通过 `-e` 指定事件
  - 按功能领域：**数据分析与工具** - 通用事件数据分析工具
  - 按分析技术：**聚合分析** - 支持事件统计和数据挖掘
- **最低内核版本**: 需要内核支持 perf_event 子系统 (Linux 2.6.31+)
- **依赖库**:
  - libtraceevent (事件解析)
  - libelf (符号解析)
- **平台支持**: x86, ARM, ARM64, RISC-V, PowerPC 等主流架构
- **特殊限制**:
  - 需要 root 权限或 CAP_PERFMON 能力
  - 必须指定 `--query` 或 `--output2` 之一（或同时指定）
  - 使用 `--query` 时如无 `--output2`，必须指定 `-i` 周期间隔
- **参与联合分析**: 不参与联合分析，作为独立的数据分析工具使用
- **核心技术**:
  - SQLite 3.51.1 嵌入式数据库
  - 事件字段自动映射为SQL列
  - 双存储模式：文件模式和内存模式
  - 事务批处理优化
  - 预编译SQL语句复用
  - Virtual Table约束下推
  - 基于colUsed的自适应存储模式选择
  - 单字段红黑树索引
  - ORDER BY索引优化
  - 自动注册SQL内置函数

## 基础用法
```bash
perf-prof sql -e EVENT [--query 'SQL_STATEMENT'] [--output2 DB_FILE] [-i INT]
```

OPTION:
- `-i, --interval <ms>` - 间隔输出，周期性执行SQL查询并清理数据 (默认：无间隔，仅在退出时执行一次)
- `--order` - 按时间戳排序事件后再插入数据库（通常不需要）
- `-m, --mmap-pages <pages>` - 环形缓冲区大小，默认8页，高频事件可增大

PROFILER OPTION:
- `-e, --event <EVENT,...>` - 事件选择器，事件名称作为SQL表名（必需）
  - 支持 `alias=` 属性指定表别名：`-e 'event//alias=table_name/'` 有多个相同事件时必须使用别名区分
  - 支持 `index=` 属性手动选择索引字段，必须为INTEGER类型
  - 示例：`-e 'sched:sched_wakeup//alias=wakeup1/index=pid/',sched:sched_wakeup//alias=wakeup2/'`

- `--query <SQL>` - 执行的SQL查询语句，支持多条语句用 `;` 分隔
  - 查询语句将在周期间隔（`-i`）或程序退出时执行
  - 查询完成后会清空表数据（内存模式）或重建表（文件模式）
  - 支持标准SQL语法：SELECT, GROUP BY, ORDER BY, JOIN等

- `--output2 <file>` - 指定SQLite数据库文件路径
  - 未指定时使用内存数据库（`:memory:`）
  - 文件数据库可持久化数据，供后续分析
  - 文件数据库性能优化更激进（128MB缓存，512MB内存映射）

- `--verify` - 验证Virtual Table实现的正确性
  - 同时创建内存Virtual Table和临时文件数据库
  - 相同事件同时插入两个数据库
  - 执行查询时比较两个数据库的结果，验证列数、类型、值是否一致
  - 不一致的结果输出到stderr，便于调试
  - 仅在内存模式下有效（无`--output2`时）
  - 指定`--output2`时自动禁用并提示

### 使用场景矩阵

| `-i` | `--query` | `--output2` | 行为说明 |
|------|-----------|-------------|---------|
| ✗ | ✗ | ✗ | **禁止** - 事件存储在内存中但从不使用 |
| ✗ | ✗ | ✓ | 事件持续保存到数据库文件（无查询） |
| ✗ | ✓ | ✗ | **禁止** - 内存累积直到程序退出 |
| ✗ | ✓ | ✓ | 程序退出时执行查询（数据持久化） |
| ✓ | ✗ | ✗ | **禁止** - 周期性无操作 |
| ✓ | ✗ | ✓ | 周期性保存事件到数据库文件 |
| ✓ | ✓ | ✗ | **推荐** - 周期性查询，内存清空 |
| ✓ | ✓ | ✓ | 周期性查询，表重建（适合长期监控） |

### 示例
```bash
# 统计每个进程的唤醒次数，按次数降序排列
perf-prof sql -e sched:sched_wakeup -i 1000 \
  --query 'SELECT comm, COUNT(*) as count FROM sched_wakeup GROUP BY comm ORDER BY count DESC'

# 多事件联合查询
perf-prof sql -e sched:sched_wakeup,sched:sched_switch -i 1000 \
  --query 'SELECT * FROM sched_wakeup WHERE pid > 1000'

# 保存事件到文件供后续分析
perf-prof sql -e sched:sched_wakeup --output2 events.db -i 10000

# 多条查询语句（用分号分隔）
perf-prof sql -e sched:sched_wakeup -i 1000 \
  --query 'SELECT comm, COUNT(*) FROM sched_wakeup GROUP BY comm; SELECT AVG(prio) FROM sched_wakeup'

# 使用过滤器和事件字段
perf-prof sql -e 'sched:sched_wakeup/prio<10/' -i 1000 \
  --query 'SELECT target_cpu, COUNT(*) FROM sched_wakeup GROUP BY target_cpu'

# 使用别名区分多个相同事件（不同过滤条件）
perf-prof sql -e 'sched:sched_wakeup/prio<10/alias=high_prio/,sched:sched_wakeup/prio>=10/alias=low_prio/' -i 1000 \
  --query 'SELECT "high_prio" as type, COUNT(*) FROM high_prio UNION SELECT "low_prio", COUNT(*) FROM low_prio'
```

## 核心原理

**基本定义**

- **事件映射**: 每个事件自动映射为一张SQL表，表名为事件名（如 `sched_wakeup`）
  - 支持通过 `alias=` 属性指定表别名
  - 有别名时使用别名作为表名，否则使用事件名
  - 多个相同事件必须使用不同别名区分
- **字段映射**: 事件的所有字段自动映射为表的列
  - 数值字段 → INTEGER 类型
  - 字符串字段 → TEXT 类型
  - 数组字段 → BLOB 类型
- **系统列**: 每张表包含固定的系统列
  - `_pid`: 进程ID
  - `_tid`: 线程ID
  - `_time`: 事件时间戳
  - `_cpu`: CPU编号
  - `_period`: 采样周期
  - `common_flags`, `common_preempt_count`, `common_pid`: trace event通用字段
- **元数据表**: 自动创建 `event_metadata` 表存储事件元信息
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

**数据模型**
```
事件采样 → [过滤器] → 批量插入SQL表 → [周期间隔] → 执行SQL查询 → 清空表
```

### 事件源

- **sample_type**:
  - `PERF_SAMPLE_TID`: 采集进程和线程ID
  - `PERF_SAMPLE_TIME`: 采集事件时间戳
  - `PERF_SAMPLE_ID`: 事件ID用于区分不同事件
  - `PERF_SAMPLE_CPU`: 采集CPU编号
  - `PERF_SAMPLE_PERIOD`: 采集周期
  - `PERF_SAMPLE_RAW`: 采集原始trace事件数据

- **自定义事件**: 通过 `-e` 选项指定
  - 支持所有tracepoint事件: `sys:name`
  - 支持通配符: `sched:*`, `irq:irq_*`
  - 支持kprobe/uprobe: `kprobe:function`, `uprobe:func@"file"`
  - **过滤器**: 支持trace event过滤器语法，在内核态过滤事件
    - 示例: `-e 'sched:sched_wakeup/pid>1000 && prio<10/'`
    - trace event过滤器失败时在用户态执行过滤
  - **属性**:
    - `alias=name`: 设置表别名（默认使用事件名）
    - `index=field`: 指定索引字段（覆盖自动选择的索引字段，仅内存模式有效）

### 事件处理

事件处理分为**文件模式**和**内存模式**两种，由`--output2`决定。

#### 文件模式（`--output2`）

文件模式使用传统的SQLite表存储事件，适合数据持久化和离线分析。

**初始化流程**:
1. 调用`sql_tp_common_init()`完成通用初始化（字段解析、函数注册）
2. 通过`tep_event_fields()`获取所有字段定义
3. 根据字段类型（数值/字符串/数组）生成CREATE TABLE语句
4. 预编译INSERT语句并标记为PERSISTENT复用

**数据插入流程**:
1. 接收到perf事件样本
2. 解析样本数据，提取系统字段和事件字段
3. 绑定参数到预编译语句（零拷贝绑定）
4. 执行INSERT语句
5. 批量提交优化（2000条/事务）

**查询执行与清理**:
1. 到达周期间隔或程序退出时触发
2. 提交未完成的事务
3. 执行用户指定的SQL查询语句
4. 输出查询结果（表格格式，自适应列宽）
5. 清理数据：DROP TABLE + CREATE TABLE（数据保留在文件中）

#### 内存模式（无`--output2`）

内存模式使用Virtual Table实现零拷贝事件访问，支持查询优化自动推导。

**初始化流程**（`sql_tp_mem()`）:
1. 调用`sql_tp_common_init()`完成通用初始化
2. 为所有事件创建Virtual Table（启用`xBestIndex`调用）
3. 设置`priv->init = 1`开启优化数据收集
4. 在空表上执行`--query`触发查询规划（`sql_tp_mem_try_exec()`）:
   - `sqlite3_prepare_v3()`触发`xBestIndex`：收集`colUsed`、WHERE约束、ORDER BY
   - `sqlite3_step()`触发`xFilter`：从约束生成ftrace filter表达式
5. 设置`priv->init = 0`停止收集
6. 分析收集的数据，选择优化策略:
   - 选择存储模式（Virtual Table vs Regular Table）
   - 选择索引字段（约束中引用最多的字段）
   - 将ftrace filter应用到`tp->filter`实现内核态过滤
7. 为`MEM_REGULAR_TABLE_MODE`的事件创建普通表

**存储模式选择**:
- `MEM_VIRTUAL_TABLE_MODE`: 事件存储在链表中，按需提取字段
- `MEM_REGULAR_TABLE_MODE`: 只存储`colUsed`指定的列，通过INSERT插入

**模式选择逻辑**:
1. **默认选择**（基于`colUsed`字段使用比例）:
   - `colUsed > 50%` 字段 → `MEM_REGULAR_TABLE_MODE`（INSERT更高效）
   - `colUsed <= 50%` 字段 → `MEM_VIRTUAL_TABLE_MODE`（按需提取更高效）
2. **强制使用Virtual Table**:
   - 有ftrace filter可用时（利用内核态过滤）
   - 有index可用时（利用索引加速查询）

**数据插入流程**（`sql_tp_mem_sample()`）:
- `MEM_VIRTUAL_TABLE_MODE`: 将原始事件存入链表，如有索引则同时加入红黑树
- `MEM_REGULAR_TABLE_MODE`: 解析事件并INSERT所需列到普通表

**查询执行与清理**:
1. 到达周期间隔或程序退出时触发
2. 执行用户指定的SQL查询语句
3. 清理数据：释放链表中所有事件，清空索引树

#### 性能优化策略

- **事务批处理**:
  - 内存数据库：不限制事务
  - 文件数据库：2000条插入/事务
- **预编译语句**: INSERT语句预编译并标记为PERSISTENT复用
- **零拷贝绑定**: 字符串和BLOB使用SQLITE_STATIC标志，避免数据拷贝
- **无排序依赖**: 事件按到达顺序直接插入，无需排序
- **colUsed收集**: 支持`;`分隔的多条SQL语句，所需列会累积
  ```bash
  --query "SELECT pid FROM sched_wakeup; SELECT comm FROM sched_wakeup"
  # colUsed 会包含 pid 和 comm 两列
  ```

#### Virtual Table约束下推

内存模式的Virtual Table支持WHERE子句约束下推，实现两级过滤优化：

**两级过滤架构**:
1. **内核态过滤（ftrace filter）**: 在perf_event采样时过滤，最高效
2. **用户态过滤（op_table）**: 在Virtual Table遍历时过滤，次高效

**内核态过滤：ftrace filter生成**（初始化阶段）:
- 初始化时（`priv->init=1`），`xFilter`将约束转换为ftrace filter表达式
- 只有内核支持的字段和运算符才能生成ftrace filter
  - `INTEGER` 列: 支持 `=`, `>`, `<`, `>=`, `<=`, `!=`
  - `TEXT` 列：支持 `=`, `!=`, `GLOB`，分别转换为 `==`, `!=`, `~` 内核运算符。
- 单次`xFilter`内的约束用`&&`组合：`WHERE pid>1000 AND prio<10` → `pid>1000&&prio<10`
- 多次`xFilter`调用的filter用`||`组合：`(pid>1000)||(comm~"perf*")`
- 生成的filter存入`priv->ftrace_filter`，后续应用到`tp->filter`实现内核态过滤
- **多查询语句限制**: 当`--query`包含多条SQL语句时，只有当某个表的**所有**查询语句都有WHERE约束时，才会应用ftrace filter。否则，没有WHERE约束的查询会因为内核过滤而丢失事件。
  ```sql
  -- 示例：以下情况不会应用ftrace filter
  SELECT * FROM sched_wakeup WHERE pid > 1000;  -- 有WHERE
  SELECT * FROM sched_wakeup;                    -- 无WHERE，需要全部事件
  -- 因为第二条查询需要全部事件，所以不能在内核态过滤pid>1000
  ```

**用户态过滤**:
- **支持的运算符**:
  - `INTEGER` 列: `=`, `>`, `<`, `>=`, `<=`, `!=`
  - `TEXT` 列: `=`, `>`, `<`, `>=`, `<=`, `!=`, `GLOB`
- **约束传递机制**:
  - `xBestIndex`: 分析WHERE约束，构建`struct index_info`结构体传递给`xFilter`
    - 结构体头部为字符串`"perf_tp:<address>"`，EXPLAIN时可显示
    - 包含约束表（字段、运算符、RHS值）、ORDER BY标志、distinct、colUsed等信息
  - `xFilter`: 从`idxStr`获取`index_info`指针，复制约束表并绑定argv[]运行时值
  - `xNext`: 遍历时检查所有约束（AND逻辑），跳过不匹配的事件

- **约束示例**:
  ```sql
  -- 约束下推到内核过滤器
  SELECT * FROM sched_wakeup WHERE pid = 1234; SELECT * FROM sched_wakeup WHERE prio < 10
  -- pid = 和 prio < 是ftrace支持的字段和运算符，生成内核过滤器"pid==1234||prio<10"
  -- 同时在Virtual Table遍历时，为每条查询进行不同的用户态过滤。

  -- 混合整数和字符串约束（字符串范围约束仅用户态过滤）
  SELECT * FROM sched_wakeup WHERE pid > 10 AND comm GLOB 'perf*'
  -- pid > 和 comm GLOB 是内核支持的字段和运算符，生成的内核过滤器'pid>10&&comm~"perf*"'
  ```

- **不支持下推的情况**:
  - BLOB类型列的约束（由SQLite在`xColumn`返回后过滤）
  - LIKE等模式匹配运算符（GLOB 对字符串索引字段支持下推）
  - 复杂表达式（如`pid + 1 > 100`）
  - 内核不支持的字段（仅用户态过滤）

#### 单字段索引优化（支持整数和字符串）

内存模式支持对单个字段建立索引，将 WHERE 查询从 O(n) 全表扫描优化为 O(log n) 索引查找。支持所有 `INTEGER` 和 `TEXT` 类型的字段，以及所有比较运算符：`=`、`>`、`>=`、`<`、`<=`、`!=`、`GLOB`。同时支持 `ORDER BY` 和 `GROUP BY` 优化，利用索引的有序性直接提供排序结果，避免 SQLite 的额外排序开销。

- **索引选择机制**:
  1. 初始化时在空表上执行 `--query`，触发 SQLite 查询规划。
  2. `xBestIndex` 收集所有 `WHERE` 约束条件以及 `ORDER BY` 和 `GROUP BY` 子句中的列。
     - `INTEGER` 字段支持 `EQ`、`GT`、`LE`、`LT`、`GE`、`NE` 运算符。
     - `TEXT` 字段支持 `EQ`、`GT`、`LE`、`LT`、`GE`、`NE`、`GLOB` 运算符。
     - 通过内部的 `TEXT` 位标记区分字符串和整数运算，支持混合过滤条件（如 `WHERE pid > 10 AND comm GLOB 'perf*'`）。
  3. 统计每个 `INTEGER` 和 `TEXT` 字段被引用的次数（`col_refs`），选择引用最多的字段作为索引字段。
     - 单字段 `ORDER BY` 或 `GROUP BY` 也会增加该字段的引用计数。
  4. 运行时为该字段建立红黑树索引：
     - `INTEGER` 字段：直接存储整数值，使用整数比较。
     - `TEXT` 字段：存储字符串指针（转换为 `int64_t`），使用 `strcmp` 比较。字符串来自 perf_event 内部，无需拷贝。
  5. 用户可通过 `index=field` 属性覆盖自动选择的索引字段。

- **手动指定索引字段**:
  ```bash
  # 手动为字符串字段 comm 建立索引
  perf-prof sql -e 'sched:sched_wakeup//index=comm/' -i 1000 \
    --query "SELECT comm, COUNT(*) FROM sched_wakeup WHERE comm GLOB 'perf*' GROUP BY comm"
  ```
  - 如果指定的字段不存在，或不是 `INTEGER` 或 `TEXT` 类型，会输出警告并退回到自动选择。

- **ftrace 过滤器集成**:
  初始化阶段在空表上执行 `--query` 时，会触发 `xFilter` 调用。`xFilter` 内部调用 `perf_tp_ftrace_filter()` 尝试为支持的约束生成 ftrace 过滤器。
  - Linux 内核对字符串字段的 ftrace filter 仅支持 `EQ`、`NE`、`GLOB` 运算符。
  - 不支持的运算符（如字符串的 `GT`、`LT` 等）只能在用户态过滤。

- **核心算法：边界运算**:
  为了同时支持整数和字符串等无法简单执行 `+1`/`-1` 运算的类型，索引采用了一种更通用的边界 (`struct boundary`) 运算算法，由 `query_op_boundary()` 函数实现。
  - **边界表示**: 每个边界由 **值** (`value`) 和 **运算符** (`op`) 共同定义。初始边界 `valid=0` 表示无穷大（无约束）。
    - `(10, GE)` 表示 `>= 10`，即 `[10, ...)`
    - `(10, GT)` 表示 `> 10`，即 `(10, ...)`
    - `(20, LE)` 表示 `<= 20`，即 `(..., 20]`
    - `(20, LT)` 表示 `< 20`，即 `(..., 20)`
    - `('perf', LT)` 表示 `< 'perf'`，即 `(..., 'perf')`
    - 这种设计取代了过去对整数值执行 `+1`/`-1` 来表示开闭区间的做法。
  - **两遍算法**:
    - **Pass 1: 建立基础范围** - 处理 `EQ/GE/LE/GT/LT` 约束：
      - `EQ`: 同时设置左边界为 `GE` 和右边界为 `LE`（即单点 `[value, value]`）
      - `GE/GT`: 更新左边界，仅当新值更大，或值相同但更严格（`GT` 覆盖 `GE`）
      - `LE/LT`: 更新右边界，仅当新值更小，或值相同但更严格（`LT` 覆盖 `LE`）
    - **冲突检测**: Pass 1 后检查范围是否为空：
      - 左边界值 > 右边界值：冲突，无结果
      - 左右边界值相同，但非闭区间 `[value, value]`：冲突，无结果
    - **Pass 2: 应用 NE 约束** - 按遍历方向扫描已排序的约束表：
      - `NE` 在范围外：忽略
      - `NE` 在边界上：将闭合边界转换为开放边界（`GE` → `GT` 或 `LE` → `LT`）
      - `NE` 在范围内：成为新边界，将范围切分为两段
  - **类型无关**: 整个算法不直接修改边界值（无 `+1`/`-1` 运算），只操作边界的运算符，从而天然支持字符串和未来可能的浮点数等类型。
  - **查询流程** (`perf_tp_do_index`):
    1. 使用 `struct boundary left, right` 初始化左右边界（`valid=0` 表示无穷）。
    2. 调用 `query_op_boundary()` 按升/降序从 `op_index` 表查询约束，确定左右边界。
    3. 调用 `find_IndexNode()` 在红黑树中查找边界范围内的事件。

- **GLOB 运算符优化**:
  `GLOB` 运算符通过与 `GE` 和 `LT` 协同工作实现高效索引。当 SQLite 遇到 `comm GLOB 'perf*'` 这样的约束时，它会自动生成三个独立的约束：
  1. `comm GE 'perf'`：确立查询的起始边界。
  2. `comm LT 'perg'`：确立查询的结束边界（`g` 是 `f` 的下一个字符）。
  3. `comm GLOB 'perf*'`：真正的模式匹配，由 `xNext` 阶段处理。

  索引利用前两个约束 `[GE 'perf', LT 'perg')` 快速锁定一个很小的扫描范围，然后 `xNext` 阶段仅对这个范围内的事件应用 `GLOB` 模式匹配，极大地提高了查询效率。

- **NE (!=) 分段迭代**:
  当查询包含 `!=` 约束时，`query_op_boundary()` 会将搜索范围分割为多个不连续的段，通过多次调用依次返回每个段。

  示例：`WHERE pid != 5 AND pid > 10 AND pid != 20 AND pid < 100 AND pid != 200`
  ```
  Pass 1: 建立基础范围 (GT 10, LT 100)，即 (10, 100)
  Pass 2 (升序): NE 5 在范围外忽略，NE 20 在范围内切分
    → 第一次调用返回 (GT 10, LT 20)，即 (10, 20)
    → 下一次调用从 (GT 20, LT 100) 继续
  Pass 2 (继续): NE 200 在范围外忽略
    → 第二次调用返回 (GT 20, LT 100)，即 (20, 100)
  ```

- **ORDER BY / GROUP BY 优化**:
  当 `ORDER BY` 或 `GROUP BY` 的字段与索引字段相同时，索引可以直接提供排序结果，避免 SQLite 的排序开销。
  - **升序 (ASC)**: 从红黑树左侧向右遍历，先输出最小值。
  - **降序 (DESC)**: 从红黑树右侧向左遍历，先输出最大值。
  - 这同样适用于字符串字段，会按字典序排序。

  示例：
  ```sql
  -- 按进程名升序排序，利用 comm 字段的索引
  SELECT comm, COUNT(*) FROM sched_wakeup GROUP BY comm ORDER BY comm ASC

  -- 按时间戳降序，利用 _time 字段的索引
  SELECT * FROM sched_wakeup WHERE _time > 1000000 ORDER BY _time DESC
  ```

  **限制**:
  - 仅支持单字段 `ORDER BY`（多字段排序无法优化）。
  - `ORDER BY` 字段必须与索引字段相同。

- **索引使用示例**:
  ```sql
  -- 假设 comm 字段被选为索引字段
  SELECT * FROM sched_wakeup WHERE comm = 'perf-prof'        -- 使用索引 O(log n)
  SELECT * FROM sched_wakeup WHERE comm GLOB 'perf*'          -- 使用索引（范围扫描 + GLOB）
  SELECT * FROM sched_wakeup WHERE comm > 'bash'              -- 使用索引（范围扫描）
  SELECT * FROM sched_wakeup WHERE comm != 'systemd'          -- 使用索引（分段迭代）

  -- 混合类型查询
  SELECT * FROM sched_wakeup WHERE pid > 100 AND comm = 'bash'
    -- 若 comm 是索引，则先用索引找到 'bash'，再过滤 pid > 100
    -- 若 pid 是索引，则先用索引找到 pid > 100 的范围，再过滤 comm = 'bash'
  ```

- **Cost 模型**（用于查询规划器选择最优方案）:
  | 约束类型 | Cost | 说明 |
  |---------|------|------|
  | ftrace 兼容约束 | 10 | 可下推到内核过滤，最优 |
  | EQ/NE 整数约束 | 50 | 适合索引精确查找 |
  | EQ/NE 字符串约束 | 50 | 适合索引精确查找 |
  | 范围约束 (GT/LT/GE/LE) 整数 | 200 | 需要范围扫描 |
  | 范围约束 (GT/LT/GE/LE) 字符串 | 200 | 需要范围扫描 |
  | GLOB 约束 | 200 | 字符串模式匹配，需要范围扫描 |
  | 非整数/字符串或不支持约束 | 1000 | 无法优化 |

- **运行时统计**（使用 `-v` 选项查看）:
  ```
  SQL query cost: 1 ms
  SQL stmt status: fullscan_step 0 sort 0 vm_step 6753
  sched_wakeup: xFilter 2 xEof 1002 xNext 1002 xColumn 1061 xRowid 0 scan_list 1 do_index 1 do_filter 0
  ```
  - `SQL query cost`: 查询语句执行耗时
  - `SQL stmt status`: 查询语句的性能统计
  - `xFilter, xEof, xNext, xColumn, xRowid`: 对应函数的执行次数
  - `scan_list`: 全表扫描的次数
  - `do_index`: 使用索引查找的次数
  - `do_filter`: 用户态过滤器执行次数


### 参数调优

**SQLite PRAGMA优化**:
- `page_size = 65536`: 64KB大页面，减少页面切换
- `journal_mode = OFF`: 关闭日志，最大化写入速度
- `synchronous = OFF`: 关闭同步写入，不等待fsync
- `locking_mode = EXCLUSIVE`: 独占锁，避免锁竞争
- `cache_size`: 内存数据库64MB，文件数据库128MB
- `mmap_size = 512MB`: 文件数据库启用内存映射I/O

### 状态统计
- **信号处理**
  - SIGUSR1: 无特殊处理
  - SIGUSR2: 无特殊处理
- **统计信息**
  - Total Inserts: 总插入事件数
  - Total Commits: 总事务提交次数
  - Total SQL Cost: 总查询耗时

## SQL 内置函数

所有内置函数在事件的 `print_fmt` 包含对应格式符时**自动注册**，可通过 `event_metadata.function_list` 查询可用函数。

### 函数一览

| 函数 | 功能 | 触发格式 | 参数类型 | 返回值 |
|------|------|----------|----------|--------|
| `symbolic(value)` | 数值转符号字符串（单参数） | `__print_symbolic()` | INTEGER | 符号名或 `"UNKNOWN"` |
| `symbolic(field, value)` | 数值转符号字符串（双参数） | `__print_symbolic()` | TEXT, INTEGER | 符号名或 `"UNKNOWN"` |
| `ksymbol(addr)` | 内核地址转符号 | `%pS`, `%ps`, `%pF`, `%pf` | INTEGER | 符号名或 `"??"` |
| `syscall(nr)` | 系统调用号转名称 | `raw_syscalls:*`, `syscalls:*` | INTEGER | 系统调用名或 `"??"` |
| `ipv4_str(blob)` | IPv4地址转换（网络序） | `%pI4`, `%pi4` | BLOB(4) | `"x.x.x.x"` 或 `"??"` |
| `ipv4_hstr(blob)` | IPv4地址转换（主机序） | `%pI4h`, `%pi4h` | BLOB(4) | `"x.x.x.x"` 或 `"??"` |
| `ipv6_str(blob)` | IPv6地址转换 | `%pI6`, `%pi6` | BLOB(16) | IPv6字符串或 `"??"` |
| `ipsa_str(blob)` | sockaddr转换（网络序） | `%pIS`, `%piS` | BLOB | `"IP:port"` 或 `"??"` |
| `ipsa_hstr(blob)` | sockaddr转换（主机序） | `%pISh`, `%piSh` | BLOB | `"IP:port"` 或 `"??"` |
| `uuid_str(blob)` | UUID转换（大端序） | `%pU`, `%pUB` | BLOB(16) | UUID字符串或 `"??"` |
| `guid_str(blob)` | GUID转换（小端序） | `%pUL`, `%pUl` | BLOB(16) | GUID字符串或 `"??"` |
| `mac_str(blob)` | MAC地址转换 | `%pM`, `%pm` | BLOB(6) | `"xx:xx:xx:xx:xx:xx"` 或 `"??"` |

### symbolic() - 符号转换函数

- **功能**: 将事件字段的数值转换为对应的符号字符串，基于内核 `__print_symbolic()` 宏定义。
- **语法**:
  ```sql
  symbolic(field_value)                           -- 单参数形式（仅当只有一个 __print_symbolic 定义时）
  symbolic('field_name', field_value)             -- 双参数形式
  symbolic('table_name.field_name', field_value)  -- 带表名的双参数形式
  ```
- **参数**:
  - `field_value`: 需要转换的数值（单参数形式）
  - `field_name`: 字段名称（支持所有表）
  - `table_name.field_name`: 完整字段名（指定特定表）
- **返回值**:
  - 成功：返回对应的符号字符串
  - 失败：返回 `"UNKNOWN"`

- **单参数形式**:
  - 当所有事件中只有一个 `__print_symbolic` 定义时，可以省略字段名参数
  - 系统自动使用唯一的符号表进行查找
  - 可通过 `event_metadata.function_list` 查看可用的单参数形式（如 `symbolic(exit_reason)`）
  - 示例场景：只监控 `kvm:kvm_exit` 事件时，可直接使用 `symbolic(exit_reason)`

- **工作原理**:
  1. 启动时自动解析事件的 `print_fmt` 格式定义
  2. 提取 `__print_symbolic()` 宏中的值到字符串映射
  3. 构建内存查找表（红黑树）：`(event_id, field_offset, value) -> string`
  4. SQL 查询时实时转换数值为符号字符串

- **支持的事件类型**:
  - ✅ 简单字段引用：`__print_symbolic(REC->vec, {0, "HI"}, {1, "TIMER"}, ...)`
  - ✅ 条件表达式（KVM）：`(REC->isa == 1) ? __print_symbolic(...) : __print_symbolic(...)`
  - ❌ 复杂表达式：`__print_symbolic((REC->dm >> 8 & 0x7), ...)` （不支持）
  - ⚠️  每个事件的每个字段只能有一个 `__print_symbolic` 定义

- **注意事项**:
  1. **自动注册**：只有包含 `__print_symbolic` 的事件才会注册该函数
  2. **平台差异**：KVM 事件会根据 CPU vendor（Intel/AMD/Hygon）自动选择正确的符号表

### ksymbol() - 内核符号解析函数

- **功能**: 将内核函数指针地址转换为人类可读的内核符号名称。
- **语法**: `ksymbol(kernel_address)`
- **参数**: `kernel_address`: 内核地址（INTEGER类型）
- **返回值**:
  - 成功：返回内核符号名称（如 `schedule`, `do_sys_open`）
  - 失败：返回 `"??"`（未知地址）

- **工作原理**:
  1. 启动时自动解析事件的 `print_fmt` 格式定义
  2. 检测使用 `%pS`、`%ps`、`%pF`、`%pf` 格式的指针字段
  3. 自动注册 ksymbol() 函数（仅在需要时）
  4. 运行时调用 `function_resolver()` 解析内核符号
  5. 使用 `/proc/kallsyms` 和内核符号表进行地址到符号映射

- **支持的指针格式**:
  - `%pS`: 带偏移的符号（如 `schedule+0x10/0x50`）
  - `%ps`: 不带偏移的符号（如 `schedule`）
  - `%pF`: 带偏移的函数符号（已弃用，等同于 `%pS`）
  - `%pf`: 不带偏移的函数符号（已弃用，等同于 `%ps`）

- **自动检测**: 只有事件的 print_fmt 包含上述格式时，ksymbol() 函数才会被注册。

- **注意事项**:
  1. **自动注册**：只有包含 `%pS/%ps/%pF/%pf` 格式的事件才会注册该函数
  2. **性能**：符号解析有一定开销，建议配合 GROUP BY 减少解析次数
  3. **权限**：需要读取 `/proc/kallsyms`，可能需要 root 权限

### syscall() - 系统调用名称转换函数

- **功能**: 将系统调用号转换为人类可读的系统调用名称。
- **语法**: `syscall(syscall_nr)`
- **参数**: `syscall_nr`: 系统调用号（INTEGER类型）
- **返回值**:
  - 成功：返回系统调用名称（如 `read`, `write`, `openat`）
  - 失败：返回 `"??"`（无效的系统调用号）

- **自动注册**: 该函数为以下事件系统自动注册：
  - `raw_syscalls` 事件：使用 `syscall(id)` 字段
  - `syscalls` 事件：使用 `syscall(__syscall_nr)` 字段

- **注意事项**:
  1. **O(1) 查找**：使用内置的 syscalls_table 数组进行快速查找
  2. **架构相关**：系统调用号与 CPU 架构相关，当前支持 x86, arm 架构

### IP地址转换函数

**概述**: 一组用于处理IP地址和网络地址的标量函数，支持IPv4、IPv6以及sockaddr结构体的格式化输出。

#### ipv4_str() - IPv4地址转换（网络字节序）

- **功能**: 将IPv4地址从网络字节序（大端）的BLOB格式转换为点分十进制字符串。
- **语法**: `ipv4_str(ipv4_blob)`
- **参数**: `ipv4_blob`: IPv4地址BLOB（必须为4字节）
- **返回值**:
  - 成功：返回点分十进制格式（如 `"192.168.1.1"`）
  - 失败：返回 `"??"`（无效输入或大小错误）
- **支持的格式**: `%pI4`, `%pi4`

#### ipv4_hstr() - IPv4地址转换（主机字节序）

- **功能**: 将IPv4地址从主机字节序（小端）的BLOB格式转换为点分十进制字符串。
- **语法**: `ipv4_hstr(ipv4_blob)`
- **参数**: `ipv4_blob`: IPv4地址BLOB（必须为4字节）
- **返回值**:
  - 成功：返回点分十进制格式（如 `"192.168.1.1"`）
  - 失败：返回 `"??"`（无效输入或大小错误）
- **支持的格式**: `%pI4h`, `%pI4l`, `%pi4h`, `%pi4l`

#### ipv6_str() - IPv6地址转换

- **功能**: 将IPv6地址的BLOB格式转换为标准IPv6字符串表示。
- **语法**: `ipv6_str(ipv6_blob)`
- **参数**: `ipv6_blob`: IPv6地址BLOB（必须为16字节）
- **返回值**:
  - 成功：返回IPv6地址字符串（如 `"2001:db8::1"`, `"fe80::1"`）
  - 失败：返回 `"??"`（无效输入或大小错误）
- **支持的格式**: `%pI6`, `%pi6`, `%pI6c`

#### ipsa_str() - sockaddr地址转换（网络字节序）

- **功能**: 将sockaddr结构体转换为 "IP:port" 格式字符串，自动识别IPv4和IPv6。
- **语法**: `ipsa_str(sockaddr_blob)`
- **参数**: `sockaddr_blob`: sockaddr结构体BLOB
- **返回值**:
  - 成功（IPv4）：返回 `"192.168.1.1:8080"`
  - 成功（IPv6）：返回 `"[2001:db8::1]:8080"`
  - 失败：返回 `"??"`（无效输入或不支持的地址族）
- **支持的格式**: `%pIS`, `%piS`, `%pISc`, `%pISpc`

#### ipsa_hstr() - sockaddr地址转换（主机字节序）

- **功能**: 将sockaddr结构体转换为 "IP:port" 格式字符串，使用主机字节序。
- **语法**: `ipsa_hstr(sockaddr_blob)`
- **参数**: `sockaddr_blob`: sockaddr结构体BLOB
- **返回值**:
  - 成功（IPv4）：返回 `"192.168.1.1:8080"`
  - 成功（IPv6）：返回 `"[2001:db8::1]:8080"`
  - 失败：返回 `"??"`（无效输入或不支持的地址族）
- **支持的格式**: `%pISh`, `%pISl`, `%piSh`, `%piSl`

#### 注意事项

1. **自动注册**: 只有包含相应格式符（%pI4/%pI6/%pIS）的事件才会注册对应函数
2. **边界检查**: 所有函数都进行严格的输入大小验证
3. **字节序**:
   - 网络字节序函数（ipv4_str, ipsa_str）: 用于直接从网络抓取的数据
   - 主机字节序函数（ipv4_hstr, ipsa_hstr）: 用于内核处理后的数据
4. **端口显示**: sockaddr函数自动包含端口号，格式为 "IP:port"
5. **IPv6格式**: IPv6地址自动使用方括号包围，如 `[::1]:8080`

### UUID/GUID转换函数

**概述**: 用于将16字节的UUID/GUID二进制数据转换为标准字符串格式。

#### uuid_str() - UUID转换（大端序）

- **功能**: 将16字节的UUID BLOB转换为标准UUID字符串格式（大端序/网络字节序）。
- **语法**: `uuid_str(uuid_blob)`
- **参数**: `uuid_blob`: UUID BLOB（必须为16字节）
- **返回值**:
  - 成功：返回标准UUID格式（如 `"550e8400-e29b-41d4-a716-446655440000"`）
  - 失败：返回 `"??"`（无效输入或大小错误）
- **支持的格式**: `%pU`, `%pUB`, `%pUb`
- **字节序说明**:
  - 大端序（Big-endian）：字节按原始顺序输出
  - 适用于RFC 4122标准UUID

#### guid_str() - GUID转换（小端序）

- **功能**: 将16字节的GUID BLOB转换为标准GUID字符串格式（小端序/Intel字节序）。
- **语法**: `guid_str(guid_blob)`
- **参数**: `guid_blob`: GUID BLOB（必须为16字节）
- **返回值**:
  - 成功：返回标准GUID格式（如 `"00844e55-9be2-d441-a716-446655440000"`）
  - 失败：返回 `"??"`（无效输入或大小错误）
- **支持的格式**: `%pUL`, `%pUl`
- **字节序说明**:
  - 小端序（Little-endian）：前3组字节反转
  - 适用于Windows/UEFI使用的GUID格式
  - 字节转换规则：
    - 第1组（4字节）：反转
    - 第2组（2字节）：反转
    - 第3组（2字节）：反转
    - 第4、5组（2+6字节）：保持原序

#### 注意事项

1. **自动注册**: 只有包含 `%pU` 格式符的事件才会注册对应函数
2. **大小验证**: 输入必须恰好为16字节，否则返回 `"??"`
3. **格式选择**:
   - `uuid_str()`: 用于标准UUID（大端序，如Linux内核、网络协议）
   - `guid_str()`: 用于Windows/UEFI GUID（小端序）
4. **输出格式**: 统一输出为小写十六进制，格式为 `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`

### mac_str() - MAC地址转换函数

- **功能**: 将6字节的MAC地址BLOB转换为标准MAC地址字符串格式。
- **语法**: `mac_str(mac_blob)`
- **参数**:
  - `mac_blob`: MAC地址BLOB（必须为6字节）
- **返回值**:
  - 成功：返回标准MAC地址格式（如 `"00:1a:2b:3c:4d:5e"`）
  - 失败：返回 `"??"`（无效输入或大小错误）
- **支持的格式**: `%pM`, `%pm`
- **注意事项**:
1. **自动注册**: 只有包含 `%pM` 或 `%pm` 格式符的事件才会注册该函数
2. **大小验证**: 输入必须恰好为6字节，否则返回 `"??"`
3. **输出格式**: 小写十六进制，冒号分隔，格式为 `xx:xx:xx:xx:xx:xx`

### 综合示例

```bash
# 示例1：软中断和定时器统计（symbolic + ksymbol）
perf-prof sql -e irq:softirq_entry,timer:hrtimer_expire_entry -i 1000 \
  --query "
    SELECT 'softirq' as type, symbolic('vec', vec) as name, COUNT(*) as count
    FROM softirq_entry GROUP BY vec
    UNION ALL
    SELECT 'timer' as type, ksymbol(function) as name, COUNT(*) as count
    FROM hrtimer_expire_entry GROUP BY function
    ORDER BY count DESC LIMIT 10"

# 示例2：KVM退出原因统计（单参数 symbolic）
# 当只有一个 __print_symbolic 定义时，可省略字段名参数
perf-prof sql -e kvm:kvm_exit -i 1000 \
  --query "
    SELECT symbolic(exit_reason) as reason, COUNT(*) as count
    FROM kvm_exit GROUP BY exit_reason ORDER BY count DESC"

# 示例3：TCP连接统计（ipsa_str）
perf-prof sql -e tcp:tcp_probe -i 2000 \
  --query "
    SELECT ipsa_str(saddr) as src, ipsa_str(daddr) as dst,
           COUNT(*) as packets, AVG(srtt) as avg_rtt
    FROM tcp_probe
    GROUP BY src, dst ORDER BY packets DESC LIMIT 10"

# 查询事件可用的内置函数（包括单参数 symbolic 的可用性）
perf-prof sql -e kvm:kvm_exit -i 1000 \
  --query "SELECT table_name, function_list FROM event_metadata"
# 输出示例: kvm_exit | symbolic(exit_reason)
```

## SQLite版本

perf-prof 内嵌 SQLite 3.51.1 源码，无需外部依赖库。

**编译优化配置**:
- `SQLITE_THREADSAFE=0`: 单线程模式，禁用互斥锁
- `SQLITE_DEFAULT_MEMSTATUS=0`: 禁用内存统计
- `SQLITE_OMIT_DEPRECATED`: 移除废弃接口
- `SQLITE_OMIT_PROGRESS_CALLBACK`: 移除进度回调
- `SQLITE_OMIT_SHARED_CACHE`: 移除共享缓存
- `SQLITE_DEFAULT_LOCKING_MODE=1`: 默认独占锁模式
- `SQLITE_DEFAULT_SYNCHRONOUS=0`: 默认关闭同步写入

**调试信息**: 使用 `-v` 选项可显示 SQLite 版本信息。

## 输出

### 输出格式

**查询结果表格**:
```
=== SQL查询语句 ===
column1  | column2  | column3
---------|----------|----------
value1   | value2   | value3
value1   | value2   | value3
```

- **表头**: 显示查询返回的列名
- **分隔符**: `|` 分隔各列，`-` 分隔表头和数据
- **列宽**: 自动调整，初始为列名长度，动态扩展以适应数据
- **数据格式**:
  - INTEGER: 左对齐十进制数
  - FLOAT: 左对齐，6位小数
  - TEXT: 左对齐字符串
  - BLOB: 显示为 `[BLOB:size]`
  - NULL: 显示为 `NULL`

**多查询输出**: 使用 `;` 分隔多条SQL语句时，每条查询单独输出一个表格

### 关键指标

SQL查询返回的指标完全由用户定义，常见的聚合函数包括：

- **COUNT(*)**: 事件总数
- **COUNT(DISTINCT field)**: 去重计数
- **SUM(field)**: 字段求和
- **AVG(field)**: 字段平均值
- **MIN(field)/MAX(field)**: 最小/最大值
- **GROUP BY**: 分组聚合

### 查询示例

```sql
-- 统计每个CPU上的事件数
SELECT _cpu, COUNT(*) FROM sched_wakeup GROUP BY _cpu

-- 计算平均优先级
SELECT AVG(prio) as avg_prio FROM sched_wakeup

-- 多维度分组统计
SELECT comm, target_cpu, COUNT(*) as count
FROM sched_wakeup
GROUP BY comm, target_cpu
ORDER BY count DESC

-- 时间范围查询（纳秒时间戳）
SELECT * FROM sched_wakeup
WHERE _time > 1000000000 AND _time < 2000000000

-- 多表关联（多事件）
SELECT w.comm, w.pid, COUNT(*)
FROM sched_wakeup w
JOIN sched_switch s ON w.pid = s.next_pid
GROUP BY w.comm, w.pid

-- 查询事件元数据信息
SELECT * FROM event_metadata

-- 关联元数据表查询事件ID和统计信息
SELECT m.event_system, m.event_name, m.event_id, m.sample_count
FROM event_metadata m
WHERE m.table_name = 'sched_wakeup'

-- 查询所有事件的采样统计
SELECT table_name, event_id, sample_count,
       (last_sample_time - first_sample_time) / 1000000000.0 as duration_sec
FROM event_metadata
ORDER BY sample_count DESC

-- 查询哪些事件支持内置函数
SELECT table_name, event_name, function_list
FROM event_metadata
WHERE function_list IS NOT NULL
```

## 分析方法

### 基础分析流程

1. **确定分析目标**: 明确需要统计什么指标（计数、平均值、分布等）
2. **选择事件**: 通过 `-e` 选择相关的事件
3. **设置过滤器**: 使用trace event过滤器减少数据量
4. **编写SQL查询**: 根据分析目标编写GROUP BY、聚合函数等
5. **设置间隔**: 使用 `-i` 设置合适的查询周期
6. **观察结果**: 分析输出的统计数据

### 数据驱动分析

**探索性分析**:
1. 查看事件元数据，了解可用函数
   ```sql
   SELECT table_name, function_list FROM event_metadata WHERE function_list IS NOT NULL
   ```
2. 用简单查询了解数据分布
   ```sql
   SELECT * FROM event_name LIMIT 10
   ```
3. 统计基本信息
   ```sql
   SELECT COUNT(*), MIN(field), MAX(field), AVG(field) FROM event_name
   ```
4. 识别热点
   ```sql
   SELECT field, COUNT(*) FROM event_name GROUP BY field ORDER BY COUNT(*) DESC
   ```

**渐进式过滤**:
1. 初步运行不加过滤器，观察整体情况
2. 根据查询结果识别关注点
3. 添加WHERE条件或trace event过滤器精确定位
4. 迭代优化查询语句

## 应用示例

### 基础统计分析

```bash
# 统计各进程的唤醒次数
perf-prof sql -e sched:sched_wakeup -i 1000 \
  --query 'SELECT comm, COUNT(*) as wakeups FROM sched_wakeup GROUP BY comm ORDER BY wakeups DESC LIMIT 10'

# 统计各CPU的事件分布
perf-prof sql -e sched:sched_switch -i 1000 \
  --query 'SELECT _cpu, COUNT(*) as switches FROM sched_switch GROUP BY _cpu'

# 计算平均运行时间
perf-prof sql -e sched:sched_stat_runtime -i 1000 \
  --query 'SELECT comm, AVG(runtime)/1000 as avg_us FROM sched_stat_runtime GROUP BY comm'
```

### 高级技巧

**多维度分组**:
```bash
# 按进程和优先级统计
perf-prof sql -e sched:sched_wakeup -i 1000 \
  --query 'SELECT comm, prio, COUNT(*) FROM sched_wakeup GROUP BY comm, prio ORDER BY COUNT(*) DESC'
```

**时间窗口分析**:
```bash
# 按时间窗口统计（需要计算时间窗口边界）
perf-prof sql -e sched:sched_wakeup -i 10000 \
  --query 'SELECT (_time/1000000000) as time_sec, COUNT(*) FROM sched_wakeup GROUP BY time_sec'
```

**多表关联**:
```bash
# 关联wakeup和switch事件
perf-prof sql -e sched:sched_wakeup,sched:sched_switch -i 1000 \
  --query 'SELECT w.comm, COUNT(DISTINCT w.pid) as processes
           FROM sched_wakeup w, sched_switch s
           WHERE w.pid = s.next_pid
           GROUP BY w.comm'
```

**使用别名对比不同条件**:
```bash
# 对比实时进程和普通进程的唤醒频率
perf-prof sql -e 'sched:sched_wakeup/prio<100/alias=rt_prio/,sched:sched_wakeup/prio>=100/alias=normal_prio/' -i 1000 \
  --query '
    SELECT "RT" as schedule_class, COUNT(*) as count
    FROM rt_prio
    GROUP BY schedule_class
    UNION ALL
    SELECT "NORMAL" as schedule_class, COUNT(*) as count
    FROM normal_prio
    GROUP BY schedule_class
    ORDER BY count DESC
    LIMIT 20'

# 对比不同CPU的事件分布
perf-prof sql -e 'sched:sched_switch/common_pid>0/cpus=0-3/alias=cpu_0_3/,sched:sched_switch/common_pid>0/cpus=4-7/alias=cpu_4_7/' -i 1000 \
  --query '
    SELECT "CPU 0-3" as cpu_group, COUNT(*) as switches FROM cpu_0_3
    UNION ALL
    SELECT "CPU 4-7" as cpu_group, COUNT(*) as switches FROM cpu_4_7'
```

**持久化数据供后续分析**:
```bash
# 先收集数据到文件
perf-prof sql -e sched:sched_wakeup --output2 wakeup.db -i 60000

# 后续使用sqlite3命令行工具分析
sqlite3 wakeup.db "SELECT comm, COUNT(*) FROM sched_wakeup GROUP BY comm"
```

**使用元数据表分析**:
```bash
# 查询事件元数据信息
perf-prof sql -e sched:sched_wakeup,sched:sched_switch -i 1000 \
  --query 'SELECT table_name, event_id, sample_count FROM event_metadata'

# 关联元数据表查询事件详情
perf-prof sql -e sched:sched_wakeup -i 1000 \
  --query '
    SELECT m.event_system, m.event_name, m.event_id,
           w.comm, COUNT(*) as count
    FROM sched_wakeup w, event_metadata m
    WHERE m.table_name = "sched_wakeup"
    GROUP BY w.comm
    ORDER BY count DESC
    LIMIT 10'

# 查询事件采样时间跨度（perf时间，单位纳秒）
perf-prof sql -e sched:sched_wakeup --output2 wakeup.db -i 10000 \
  --query '
    SELECT table_name,
           first_sample_time,
           last_sample_time,
           (last_sample_time - first_sample_time) / 1000000000.0 as duration_sec,
           sample_count,
           sample_count / ((last_sample_time - first_sample_time) / 1000000000.0) as rate_per_sec
    FROM event_metadata'

# 查询事件可用的SQL内置函数
perf-prof sql -e irq:softirq_entry,sched:sched_wakeup -i 1000 \
  --query 'SELECT table_name, function_list FROM event_metadata WHERE function_list IS NOT NULL'

# 输出示例:
# table_name     | function_list
# ---------------|----------------------------------
# softirq_entry  | symbolic('softirq_entry.vec', vec)
```

**使用内置函数**:
```bash
# 查看事件的 print_fmt 定义（确认是否有 __print_symbolic 或指针格式）
perf-prof trace -e irq:softirq_entry help
perf-prof trace -e timer:hrtimer_expire_entry help

# 验证 symbolic 函数是否可用
perf-prof sql -e irq:softirq_entry -i 1000 \
  --query "SELECT vec, symbolic('vec', vec) FROM softirq_entry LIMIT 5"

# 验证 ksymbol 函数是否可用
perf-prof sql -e timer:hrtimer_expire_entry -i 1000 \
  --query 'SELECT function, ksymbol(function) FROM hrtimer_expire_entry LIMIT 5'

# 组合使用多个内置函数
perf-prof sql -e irq:softirq_entry,timer:hrtimer_expire_entry -i 1000 \
  --query "
    SELECT 'softirq' as type, symbolic('vec', vec) as name, COUNT(*) FROM softirq_entry GROUP BY vec
    UNION ALL
    SELECT 'timer' as type, ksymbol(function) as name, COUNT(*) FROM hrtimer_expire_entry GROUP BY function
    ORDER BY COUNT(*) DESC LIMIT 10"
```

### 性能优化

**缓冲区调优**:
- `-m` 参数: 高频事件（>100k/s）建议使用 `-m 64` 或更大
- 默认8页（32KB）适合中低频事件
- 避免事件丢失导致数据不完整

**事件过滤器优化**:
```bash
# 在内核态过滤，减少用户态处理开销
perf-prof sql -e 'sched:sched_wakeup/prio<10/' \
  --query 'SELECT comm, COUNT(*) FROM sched_wakeup GROUP BY comm'

# 使用多个过滤条件
perf-prof sql -e 'sched:sched_wakeup/pid>1000 && target_cpu<4/' \
  --query 'SELECT comm, target_cpu, COUNT(*) FROM sched_wakeup GROUP BY comm, target_cpu'
```

**查询间隔调优**:
- 短间隔（`-i 1000`）: 实时监控，但查询开销较大
- 长间隔（`-i 10000`）: 减少查询开销，适合长期趋势分析
- 无间隔: 仅在程序结束时查询一次，适合一次性分析

**数据库模式选择**:
- 内存数据库（无`--output2`）:
  - 优点: 最快性能（5000条/批次）
  - 缺点: 数据不持久化，占用内存
  - 适用: 实时监控，周期性清空数据

- 文件数据库（有`--output2`）:
  - 优点: 数据持久化，可后续分析
  - 缺点: 稍慢（2000条/批次），占用磁盘
  - 适用: 需要保存原始数据，离线分析

### 调试与验证

**使用 --verify 验证 Virtual Table 实现**:

`--verify` 选项用于验证内存模式下 Virtual Table 实现的正确性，通过对比 Virtual Table 和传统文件表的查询结果来检测潜在问题。

```bash
# 验证简单查询的正确性
perf-prof sql -e sched:sched_wakeup -i 1000 --verify \
  --query 'SELECT comm, COUNT(*) as count FROM sched_wakeup GROUP BY comm ORDER BY count DESC'

# 验证带索引的查询
perf-prof sql -e 'sched:sched_wakeup//index=pid/' -i 1000 --verify \
  --query 'SELECT * FROM sched_wakeup WHERE pid > 1000 ORDER BY pid'

# 验证字符串索引和 GLOB 查询
perf-prof sql -e 'sched:sched_wakeup//index=comm/' -i 1000 --verify \
  --query 'SELECT comm, COUNT(*) FROM sched_wakeup WHERE comm GLOB "perf*" GROUP BY comm'

# 验证多条查询语句
perf-prof sql -e sched:sched_wakeup -i 1000 --verify \
  --query 'SELECT COUNT(*) FROM sched_wakeup; SELECT comm, AVG(prio) FROM sched_wakeup GROUP BY comm'
```

**验证输出说明**:
- 正常情况：只输出查询结果表格
- 发现不一致时：在 stderr 输出错误信息
  - `Column count mismatch`: 列数不一致
  - `Column N type mismatch`: 第N列类型不一致
  - `Column N INTEGER/FLOAT/TEXT/BLOB mismatch`: 第N列值不一致

**工作原理**:
1. 创建临时文件数据库（`verify_temp.db`，立即 unlink）
2. 事件同时插入 Virtual Table（内存）和普通表（文件）
3. 对两个数据库执行相同的查询
4. 逐行逐列比较结果，报告不一致

**使用场景**:
- 开发新的 Virtual Table 优化功能后验证正确性
- 调试索引或约束下推问题
- 确认复杂查询的结果符合预期

### 组合使用

**与trace组合**:
```bash
# 先用trace探索事件字段
perf-prof trace -e sched:sched_wakeup -C 0 | head -20

# 再用sql进行统计分析
perf-prof sql -e sched:sched_wakeup -i 1000 \
  --query 'SELECT comm, COUNT(*) FROM sched_wakeup GROUP BY comm'
```

**与list组合**:
```bash
# 先列出可用事件
perf-prof list | grep sched

# 选择合适的事件进行SQL分析
perf-prof sql -e sched:sched_wakeup,sched:sched_switch -i 1000 \
  --query 'SELECT * FROM sched_wakeup LIMIT 10'
```

**分阶段分析**:
1. 第一阶段：收集全量数据到文件
   ```bash
   perf-prof sql -e sched:* --output2 all_sched.db
   ```
2. 第二阶段：离线多角度查询分析
   ```bash
   sqlite3 all_sched.db
   sqlite> SELECT name FROM sqlite_master WHERE type='table';
   sqlite> SELECT * FROM sched_wakeup LIMIT 5;
   sqlite> SELECT comm, COUNT(*) FROM sched_wakeup GROUP BY comm;
   ```

## 相关资源
- [事件基础文档](../events/)
- [sample_type采样类型说明](../sample_type.md)
- [选项参数完整参考](../main_options.md)
- [trace事件过滤器语法](../Event_filtering.md)
- [表达式系统文档](../expr.md)
