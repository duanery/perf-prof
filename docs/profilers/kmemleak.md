---
layout: default
title: "kmemleak - 内存泄漏"
---

# kmemleak - 内存泄漏分析器
用于检测用户态和内核态内存分配器的内存泄漏问题。

## 概述
- **主要用途**: 通过跟踪内存分配和释放事件，检测未释放的内存，识别内存泄漏问题
- **适用场景**: 内存使用持续增长、怀疑存在内存泄漏、需要定位内存泄漏来源
- **功能分类**: 自定义事件类，内存分析，延迟分析
- **最低内核版本**: 支持 perf_event 和 tracepoint 的内核版本（通常 2.6.32+）
- **依赖库**: libtraceevent, libelf
- **平台支持**: x86_64, ARM, RISC-V 等所有支持 perf_event 的架构
- **特殊限制**: 需要root权限
- **参与联合分析**: 否（独立分析器）
- **核心技术**: 
  - 基于 tracepoint/kprobe/uprobe 的事件关联分析，通过指针匹配 alloc 和 free 事件
  - 支持内核态内存分配器，用户态内存分配器

## 基础用法
```bash
perf-prof kmemleak --alloc EVENT[...] --free EVENT[...] [OPTION]
```

OPTION:
- `-C, --cpus`: Attach到指定CPU列表
- `-p, --pids`: Attach到指定进程
- `-t, --tids`: Attach到指定线程
- `-m, --mmap-pages`: ringbuffer大小，页数，默认值: 4
- `--order`: 启用事件时间戳排序（强烈推荐），提高分析准确性

FILTER OPTION:
- `--user-callchain[=dwarf[,size]]`  堆栈开关，选中用户态堆栈，`no-`前缀反选。`=dwarf` 启用DWARF栈回溯
- `--kernel-callchain`               堆栈开关，选中内核态堆栈，`no-`前缀反选

PROFILER OPTION:
- `--alloc <EVENT,...>`: 指定内存分配事件，必须包含 `ptr` 属性，可选 `size` 和 `stack` 属性
- `--free <EVENT,...>`: 指定内存释放事件，必须包含 `ptr` 属性
- `--than <n>`: 过滤内存分配超过指定时间的泄漏，单位: s/ms/us/*ns
- `-g, --call-graph`: 启用调用栈记录，用于定位泄漏位置
- `--flame-graph <file>`: 生成火焰图文件
- `--comm`: 在泄漏字节报告中显示每个调用栈的分配进程名列表（需配合 `-g` 使用）

### 示例
```bash
# 内核态内存泄漏检测 (kmalloc/kfree)
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/ \
                   --free kmem:kfree//ptr=ptr/ --order -m 128 -g

# 多个分配事件 (kmalloc + kmalloc_node)
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/,kmem:kmalloc_node//ptr=ptr/size=bytes_alloc/stack/ \
                   --free kmem:kfree//ptr=ptr/ --order -m 128 -g

# 分析特定进程的内存泄漏
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/ \
                   --free kmem:kfree//ptr=ptr/ -p <pid> --order -m 128 -g

# 检测超过10秒未释放的内存
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/ \
                   --free kmem:kfree//ptr=ptr/ --order -m 128 -g --than 10s

# 泄漏字节报告中显示分配进程名
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/ \
                   --free kmem:kfree//ptr=ptr/ --order -m 128 -g --comm
```

## 核心原理

**基本定义**
- **内存泄漏**: 已分配但未释放的内存
- **分配事件**: 通过 tracepoint/kprobe/uprobe 捕获内存分配操作
- **释放事件**: 通过 tracepoint/kprobe/uprobe 捕获内存释放操作
- **指针关联**: 通过分配返回的指针 (`ptr`) 将 alloc 和 free 事件关联
- **泄漏判定**: 分配事件没有对应的释放事件，即为泄漏

**数据模型**
```
分配事件 → [排序] → [存入alloc链表] → [指针匹配] → [未匹配=泄漏] → 报告
释放事件 → [排序] → [指针查找] → [从alloc链表删除] → 已释放
```

### 事件源

- **sample_type**:
  - `PERF_SAMPLE_TID`: 记录线程ID
  - `PERF_SAMPLE_TIME`: 记录事件时间戳
  - `PERF_SAMPLE_ID`: 记录事件ID（用于区分alloc/free）
  - `PERF_SAMPLE_CPU`: 记录CPU编号
  - `PERF_SAMPLE_RAW`: 记录原始事件数据
  - `PERF_SAMPLE_CALLCHAIN`: 启用调用栈（由 `-g` 或 `stack` 属性控制）

- **自定义事件**:
  - **分配事件** (`--alloc`):
    - 格式: `sys:name[/filter/ptr=EXPR/size=EXPR/stack/]`
    - `ptr=EXPR`: **必需**，计算分配返回的指针
    - `size=EXPR`: **可选**，计算分配的字节数（启用泄漏字节报告）
    - `stack`: **可选**，为该事件启用调用栈
    - `-g` 选项: 只为alloc事件启用调用栈
    - 示例事件: `kmem:kmalloc`, `kmem:kmalloc_node`, `kmem:mm_page_alloc`
    - 用户态的内存分配事件，需要加uprobe点

  - **释放事件** (`--free`):
    - 格式: `sys:name[/filter/ptr=EXPR/]`
    - `ptr=EXPR`: **必需**，计算要释放的指针
    - 示例事件: `kmem:kfree`, `kmem:mm_page_free`
    - 用户态的内存释放事件，需要加uprobe点

#### 过滤器
支持标准的 trace event filter 语法，在事件定义的第一个 `/` 后指定：
```bash
# 只跟踪大于1MB的分配
--alloc kmem:kmalloc/bytes_alloc>1048576/ptr=ptr/size=bytes_alloc/
```

### 事件处理

**处理流程**:
1. **事件接收**: 从 perf ringbuffer 接收 alloc 和 free 事件
2. **指针提取**: 通过 `ptr` 表达式从事件中提取指针值
3. **分配处理**:
   - 提取 `ptr` 和可选的 `size` 值
   - 将事件备份到 `alloc` 红黑树（以 ptr 为键）
   - 如果 ptr 已存在，替换旧事件（重新分配）
4. **释放处理**:
   - 从 `alloc` 树中查找对应的 ptr
   - 找到则删除（已正常释放）
   - 未找到则忽略

**依赖排序**:
- **强烈建议启用 `--order`**: 保证事件按时间戳顺序处理
- **不启用排序**: 可能因乱序导致误报（释放事件早于分配事件到达）
- **事件丢失处理**:
  - 检测到事件丢失时，立即报告当前泄漏
  - 清空 alloc 树，重新开始跟踪
  - 避免因丢失事件导致误报

**数据结构**:
- `alloc` 红黑树: 存储未释放的分配事件（按 ptr 排序）
- 每个节点保存完整的 perf_event 数据（包括调用栈）

### 状态统计
- **信号处理**
  - **SIGUSR1**: 输出内存泄漏统计信息
    - `ALLOC LIST`: 当前 alloc 树的事件数和内存占用
    - `TOTAL`: 累计处理的 alloc 和 free 事件数
  - **SIGUSR2**: 未使用

## 输出

### 输出格式

**1. 泄漏事件报告** (默认或 `-v`)
```
KMEMLEAK REPORT: 15
      305.266631  4518 [000] kmalloc: call_site=c09f56b9 ptr=f59a7600 bytes_req=1024 bytes_alloc=1024 gfp_flags=208
    ffffffff81234567 kmalloc
    ffffffff81abcdef some_function+0x123
    ffffffff81fedcba caller_function+0x45
```

- **表头含义**:
  - `时间戳`: 事件发生时间（perf clock）
  - `线程ID`: 分配线程的 TID
  - `CPU编号`: 分配所在的 CPU
  - `事件名`: tracepoint 事件名
  - `字段值`: 事件的各字段值
- **调用栈**: 显示分配发生的调用链（由 `-g` 启用）

**2. 泄漏字节报告** (启用 `size` 属性且 `-g`)
```
LEAKED BYTES REPORT:
Leak of 524288 bytes in 512 objects allocated from:
    ffffffff81234567 kmalloc
    ffffffff81abcdef some_function+0x123
    ffffffff81fedcba caller_function+0x45
```

使用 `--comm` 时，每个调用栈会显示分配进程名列表，按泄漏字节数降序排列：
```
LEAKED BYTES REPORT:
Leak of 524288 bytes in 512 objects allocated from:
    comms: kworker/0:1(409600/300) systemd(81920/150) bash(32768/62)
    ffffffff81234567 kmalloc
    ffffffff81abcdef some_function+0x123
    ffffffff81fedcba caller_function+0x45
```
- 格式: `进程名(泄漏字节数/分配次数)`

- **表头含义**:
  - `Leak of X bytes in Y objects`: X 字节泄漏，分布在 Y 个对象中
  - 按泄漏字节数降序排列
  - 相同调用栈的泄漏聚合显示

**3. 统计信息** (程序退出或 SIGUSR1)
```
KMEMLEAK STATS:
ALLOC LIST num 128 mem 65536
TOTAL alloc 10245 free 10117
```

- **数据单位**:
  - `num`: 事件数量
  - `mem`: 事件在kmemleak内缓存所占用的内存（字节）
  - `alloc/free`: 累计处理的事件数
- **行**:
  - `ALLOC LIST`: 当前未匹配的分配事件


## 分析方法

### 基础分析方法

**1. 选择合适的事件**
```bash
# 查看可用的内存分配和释放事件
./perf-prof list | grep -E "^kmem:"

# 查看事件字段
./perf-prof trace -e kmem:kmalloc help
```

**2. 配置指针和大小属性**
- 分配事件必须有 `ptr` 属性
- 建议添加 `size` 属性以获取泄漏字节报告
- 示例: `--alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/`

**3. 启用排序和调用栈**
```bash
# 使用 --order 保证事件顺序
# 使用 -g 获取调用栈
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/ \
                   --free kmem:kfree//ptr=ptr/ --order -g -m 128
```

**4. 调整缓冲区大小**
- 默认 4 页，内存泄漏分析建议 128 页或更大
- 使用 `-m 128` 或 `-m 256` 减少事件丢失

**5. 设置运行时间**
- 使用 `-i` 设置定期输出（如 `-i 10000` 每10秒）
- 或运行固定时间后 Ctrl+C 退出查看结果

### 数据驱动分析

**1. 不预设业务特征**
- 通过/proc/meminfo，slabtop等识别可能存在内存泄漏的分配器。
- 支持用户态内存分配器。
- 根据泄漏点，选择事件

**2. 完全基于实际数据**
- 使用过滤器缩小范围:
  ```bash
  # 只跟踪特定函数的分配
  perf-prof kmemleak --alloc 'kmem:kmalloc/bytes_alloc>1024/ptr=ptr/size=bytes_alloc/' \
                     --free kmem:kfree//ptr=ptr/ --order -g
  ```

## 应用示例

### 基础示例

```bash
# 1. 内核 kmalloc/kfree 泄漏检测
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/ \
                   --free kmem:kfree//ptr=ptr/ --order -m 128 -g

# 2. 页面分配泄漏检测（低内核内核版本使用ptr=page）
perf-prof kmemleak --alloc 'kmem:mm_page_alloc//ptr=pfn/size=4096<<order/stack/' \
                   --free kmem:mm_page_free//ptr=pfn/stack/ -m 256 --order

# 3. 特定进程的内存泄漏
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/ \
                   --free kmem:kfree//ptr=ptr/ -p 1234 --order -g -i 5000

# 4. 生成火焰图
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/ \
                   --free kmem:kfree//ptr=ptr/ --order -g --flame-graph leak.folded
flamegraph.pl leak.folded > leak.svg
```

### 高级技巧

```bash
# 1. 多个分配函数联合跟踪
perf-prof kmemleak \
    --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/,kmem:kmalloc_node//ptr=ptr/size=bytes_alloc/stack/ \
    --free kmem:kfree//ptr=ptr/ --order -m 256 -g

# 2. 过滤大内存分配 (>1MB)
perf-prof kmemleak \
    --alloc 'kmem:kmalloc/bytes_alloc>1048576/ptr=ptr/size=bytes_alloc/' \
    --free kmem:kfree//ptr=ptr/ --order -g

# 3. 检测长时间未释放的内存 (>30秒)
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/ \
                   --free kmem:kfree//ptr=ptr/ --order -g --than 30s

# 4. 使用 kprobe 跟踪自定义函数
perf-prof kmemleak \
    --alloc 'kprobe:my_alloc_func/filter/ptr=$retval/size=bytes/' \
    --free 'kprobe:my_free_func//ptr=ptr/' --order -g

# 5. 用户态内存泄漏 (需要 uprobe 支持)
perf-prof kmemleak \
    --alloc 'uprobe:malloc@"/lib64/libc.so.6"//ptr=$retval/' \
    --free 'uprobe:free@"/lib64/libc.so.6"//ptr=ptr/' -p <pid> --order -g

# 6. 实时监控泄漏趋势
while true; do
    echo "=== $(date) ==="
    timeout 30 perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/ \
                                   --free kmem:kfree//ptr=ptr/ --order -m 128
    sleep 60
done
```

### 性能优化

- **缓冲区大小**:
  - 小型系统: `-m 64`
  - 中型系统: `-m 128`
  - 大型系统: `-m 256` 或更大
  - 内存分配频繁系统: `-m 512`

- **过滤器优化**:
  - 优先使用内核态过滤器（第一个 `/` 后）
  - 使用精确过滤减少事件量:
    ```bash
    # 只跟踪特定大小范围
    --alloc kmem:kmalloc/bytes_alloc>=1024 && bytes_alloc<=4096/ptr=ptr/
    ```

- **采样限制**:
  - 使用 `--sampling-limit <N>` 限制采样率
  - 高频分配场景建议: `--sampling-limit 10000`

### 参数调优

- **`--order` 调优**:
  - 必需参数，提高准确性
  - 代价: 轻微性能开销和内存占用
  - 建议: 始终启用

- **`-m` 优化**:
  - 根据分配频率调整
  - 观察是否有 `lost events` 警告
  - 逐步增大直到无丢失

- **`--than` 调优**:
  - 过滤短暂分配，关注长期泄漏
  - 建议值: 10s ~ 60s
  - 分析缓存类泄漏用较小值（1s ~ 5s）

### 组合使用

- **与其他分析器配合**:
  ```bash
  # 1. 先用 top 找热点
  perf-prof top -e 'kmem:kmalloc//key=call_site/printkey=printf("%lx",key)/' -i 1000

  # 2. 再用 kmemleak 分析特定函数（新内核支持call_site.function过滤器）
  perf-prof kmemleak --alloc kmem:kmalloc/call_site.function==hotspot_func/ptr=ptr/ \
                     --free kmem:kfree//ptr=ptr/ --order -g

  # 3. 用 kmemprof 分析分配大小分布
  perf-prof kmemprof -e kmem:kmalloc//size=bytes_alloc/stack/ -e kmem:kfree --order
  ```

- **多阶段分析**:
  ```bash
  # 阶段1: 快速扫描（无调用栈）
  perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/ \
                     --free kmem:kfree//ptr=ptr/ --order -m 128 -i 10000

  # 阶段2: 详细分析（启用调用栈）
  perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/ \
                     --free kmem:kfree//ptr=ptr/ --order -m 256 -g -v

  # 阶段3: 生成报告（火焰图）
  perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/ \
                     --free kmem:kfree//ptr=ptr/ --order -g --flame-graph leak.folded
  ```

## 相关资源
- [事件基础文档](../events/)
- [sample_type采样类型说明](../sample_type.md)
- [选项参数完整参考](../main_options.md)
- [表达式系统文档](../expr.md)
- [过滤器语法文档](../Event_filtering.md)
- [实际案例分析](../examples/)
- [kmemprof 分析器](kmemprof.md) - 内存分配分析
- [multi-trace 分析器](multi-trace.md) - 多事件关系分析
