# kmemprof - 内存分配分析

multi-trace的特化版本，专用于分析内存分配的生命周期，统计内存分配/释放的字节数和堆栈信息。

## 概述
- **主要用途**: 分析内存分配事件到释放事件的完整生命周期，统计分配/释放的字节数，输出分配/释放最多的前N个堆栈
- **适用场景**: 内存分配热点分析、内存使用模式分析、内存分配器性能评估
- **功能分类**: 自定义事件类，内存分析，multi-trace派生
- **最低内核版本**: 3.10+ (支持kmem tracepoints)
- **平台支持**: x86, ARM, RISC-V, PowerPC
- **特殊限制**:
  - 需要root权限运行
  - 分配事件必须指定`size`属性
  - 需要`stack`属性才能输出堆栈信息
- **参与联合分析**: 不支持

## 基础用法
```bash
perf-prof kmemprof [OPTION...] -e alloc_event -e free_event [-k str]
```

### OPTION
- `--watermark <0-100>`: 默认50
- `-m, --mmap-pages <N>`: 默认64页，内存分配事件频率高，建议128或更大
- `--order`: 跨CPU关联时必需

### FILTER OPTION
- `--user-callchain[=dwarf[,size]]`: 包含用户态调用栈。`=dwarf` 启用DWARF栈回溯
- `--kernel-callchain`: 包含内核态调用栈
- `--python-callchain`: 包含Python调用栈
- trace event过滤器: 在事件后使用`/filter/`语法

### PROFILER OPTION
- `-e, --event`: 事件选择器
  - 第一个`-e`: 内存分配事件（必须指定`size`属性）
  - 第二个`-e`: 内存释放事件
- `-k, --key <str>`: 关联键（通常使用`ptr`）
- `-i, --interval <ms>`: 输出间隔（毫秒）

## 核心原理

### 数据模型
```
alloc事件 → [key=ptr关联] → free事件 → 按堆栈聚合 → 输出top N
```

### 事件源

**内存分配事件（第一个`-e`）**：
- 支持任意产生内存分配的事件
- **必须属性**: `size=EXPR`（指定分配大小的计算表达式）
- **建议属性**: `stack`（启用调用栈采样）、`key=EXPR`（指定关联键）

**内存释放事件（第二个`-e`）**：
- 支持任意产生内存释放的事件
- **建议属性**: `stack`（启用调用栈采样）、`key=EXPR`（指定关联键）

**常用事件组合**：

| 场景 | 分配事件 | 释放事件 | Key |
|------|---------|---------|-----|
| **slab分配** | kmem:kmalloc | kmem:kfree | ptr |
| **页面分配** | kmem:mm_page_alloc | kmem:mm_page_free | pfn |
| **vmalloc** | kmem:vmalloc | kmem:vfree | ptr |

### 过滤器层次
1. **trace event过滤器（内核态）**: `/filter/`语法高效过滤
2. **Key关联**: 通过ptr或其他字段关联分配和释放事件

### 事件处理

**统计方式**：
- 按调用栈聚合分配字节数
- 输出分配最多的top N堆栈（默认10个）
- 同时统计释放事件的堆栈分布

**未配对处理**：
- 分配但未释放的事件在周期结束或程序退出时输出

## 输出

### 输出格式

**周期性输出**：
```
时间戳

alloc_event => free_event
alloc_event total alloc 12345678 bytes on 1234 objects
Allocate 5678901 (46.0%) bytes on 567 (46.0%) objects:
    [调用栈1]
Allocate 3456789 (28.0%) bytes on 345 (28.0%) objects:
    [调用栈2]
...
Skipping alloc numbered 11..50

free_event total free 11234567 bytes on 1123 objects
Free 4567890 (40.7%) bytes on 456 (40.6%) objects:
    [调用栈1]
...
```

**未配对输出**：
```
alloc_event total alloc 1111111 bytes on 111 objects but not freed
```

### 输出字段
| 字段 | 说明 |
|------|------|
| total alloc N bytes | 总分配字节数 |
| on M objects | 分配对象数 |
| Allocate X (Y%) bytes | 该堆栈分配的字节数及占比 |
| on Z (W%) objects | 该堆栈分配的对象数及占比 |
| total free N bytes | 总释放字节数 |
| but not freed | 标记未释放的分配 |

### 关键指标

| 指标 | 计算方法 | 异常阈值 |
|------|----------|----------|
| alloc_bytes | 周期内总分配字节数 | 持续增长需关注 |
| nr_alloc | 分配对象数 | 高频率需优化 |
| 堆栈占比 | 该堆栈字节数/总字节数 | >50%是热点 |
| not freed | 未释放的分配 | >0需检查是否泄漏 |

## 应用示例

### 基础示例
```bash
# 1. 分析kmalloc/kfree的内存分配
perf-prof kmemprof -e 'kmem:kmalloc//size=bytes_alloc/stack/' -e kmem:kfree \
    -m 128 --order -k ptr

# 2. 同时监控kmalloc和kmalloc_node
perf-prof kmemprof \
    -e 'kmem:kmalloc//size=bytes_alloc/stack/,kmem:kmalloc_node//size=bytes_alloc/stack/' \
    -e kmem:kfree \
    --order -k ptr

# 3. 分析页面分配
perf-prof kmemprof \
    -e 'kmem:mm_page_alloc//size=4096<<order/key=pfn/stack/' \
    -e 'kmem:mm_page_free//key=pfn/stack/' \
    -m 256 --order
```

### 高级技巧
```bash
# 只分析大于1KB的分配
perf-prof kmemprof \
    -e 'kmem:kmalloc/bytes_alloc>1024/size=bytes_alloc/stack/' \
    -e kmem:kfree \
    -m 128 --order -k ptr

# 只分析特定调用路径的内存分配
perf-prof kmemprof \
    -e 'kmem:kmalloc/call_site==__kmalloc_cache_noprof/size=bytes_alloc/stack/' \
    -e kmem:kfree \
    -m 128 --order -k ptr -i 5000

# 使用page作为key分析页面分配（低版本内核）
perf-prof kmemprof \
    -e 'kmem:mm_page_alloc//size=4096<<order/key=page/stack/' \
    -e 'kmem:mm_page_free//key=page/stack/' \
    -m 256 --order
```

### 性能优化
```bash
# 高频分配场景增大缓冲区
perf-prof kmemprof -e 'kmem:kmalloc//size=bytes_alloc/stack/' -e kmem:kfree \
    -m 256 --order -k ptr

# 延长输出间隔减少开销
perf-prof kmemprof -e 'kmem:kmalloc//size=bytes_alloc/stack/' -e kmem:kfree \
    -m 128 --order -k ptr -i 10000
```

## 与kmemleak的区别

| 特性 | kmemprof | kmemleak |
|------|----------|----------|
| **分析目标** | 内存分配热点分析 | 内存泄漏检测 |
| **输出内容** | 周期性输出分配/释放统计 | 只输出未释放的内存 |
| **堆栈输出** | top N分配最多的堆栈 | 未释放内存的分配堆栈 |
| **适用场景** | 了解内存分配模式、热点路径 | 检测内存泄漏 |
| **性能开销** | 较高（统计所有分配） | 较低（只跟踪未释放） |

## 与multi-trace的区别

| 特性 | kmemprof | multi-trace |
|------|----------|------------|
| **事件配置** | 专用于alloc/free事件对 | 支持任意事件组合 |
| **实现类型** | 固定使用`--impl kmemprof` | 支持delay/pair等多种实现 |
| **输出格式** | 内存分配专用统计格式 | 通用的延迟统计格式 |
| **堆栈分析** | 按堆栈聚合字节数，输出top N | 需要手动配置 |
| **size属性** | 必须指定 | 可选 |
| **适用场景** | 内存分配热点分析 | 通用的多事件关系分析 |

## 技术要点

1. **size属性必需**: 分配事件必须指定`size`属性，用于计算分配的字节数
2. **stack属性建议**: 启用`stack`属性才能输出分配/释放的调用栈
3. **key一致性**: 分配和释放事件的key计算结果必须相同才能正确配对
4. **排序需求**: 跨CPU的内存分配需要`--order`保证时序正确
5. **缓冲区大小**: 内存分配事件频率较高，建议使用`-m 128`或更大
6. **堆栈聚合**: 按调用栈聚合分配字节数，输出top N（默认10个）

## 相关资源
- [multi-trace核心文档](multi-trace.md)
- [kmemleak内存泄漏检测](kmemleak.md)
- [事件过滤语法参考](Event_filtering.md)
