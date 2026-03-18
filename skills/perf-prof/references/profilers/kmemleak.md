# kmemleak - 内存泄漏分析器
用于检测用户态和内核态内存分配器的内存泄漏问题。

## 概述
- **主要用途**: 通过跟踪内存分配和释放事件，检测未释放的内存，识别内存泄漏问题
- **适用场景**: 内存使用持续增长、怀疑存在内存泄漏、需要定位内存泄漏来源
- **功能分类**: 自定义事件类，内存分析，延迟分析
- **最低内核版本**: 支持 perf_event 和 tracepoint 的内核版本（通常 2.6.32+）
- **平台支持**: x86_64, ARM, RISC-V 等所有支持 perf_event 的架构
- **特殊限制**:
  - 需要root权限
  - 用户态内存分配器需要加uprobe点
- **参与联合分析**: 否（独立分析器）

## 基础用法
```bash
perf-prof kmemleak --alloc EVENT[...] --free EVENT[...] [OPTION]
```

### OPTION
- `-C, --cpus`: Attach到指定CPU列表
- `-p, --pids`: Attach到指定进程
- `-t, --tids`: Attach到指定线程
- `-m, --mmap-pages`: ringbuffer大小，页数，默认值: 4，**建议128或更大**
- `--order`: 启用事件时间戳排序，**强烈推荐启用**，提高分析准确性

### FILTER OPTION
- `--user-callchain[=dwarf[,size]]`  堆栈开关，选中用户态堆栈，`no-`前缀反选。`=dwarf` 启用DWARF栈回溯
- `--kernel-callchain`               堆栈开关，选中内核态堆栈，`no-`前缀反选
- 支持标准的 trace event filter 语法，在事件定义的第一个 `/` 后指定
- 示例：`--alloc kmem:kmalloc/bytes_alloc>1048576/ptr=ptr/` 只跟踪大于1MB的分配

### PROFILER OPTION
- `--alloc <EVENT,...>`: 指定内存分配事件，必须包含 `ptr` 属性，可选 `size` 和 `stack` 属性
- `--free <EVENT,...>`: 指定内存释放事件，必须包含 `ptr` 属性
- `--than <n>`: 过滤内存分配超过指定时间的泄漏，单位: s/ms/us/*ns
- `-g, --call-graph`: 启用调用栈记录，用于定位泄漏位置
- `--flame-graph <file>`: 生成火焰图文件
- `--comm`: 在泄漏字节报告中显示每个调用栈的分配进程名列表（需配合 `-g` 使用）

## 核心原理

### 数据模型
```
分配事件 → [排序] → [存入alloc链表] → [指针匹配] → [未匹配=泄漏] → 报告
释放事件 → [排序] → [指针查找] → [从alloc链表删除] → 已释放
```

### 事件源
- **分配事件** (`--alloc`):
  - 格式: `sys:name[/filter/ptr=EXPR/size=EXPR/stack/]`
  - `ptr=EXPR`: **必需**，计算分配返回的指针
  - `size=EXPR`: **可选**，计算分配的字节数（启用泄漏字节报告）
  - `stack`: **可选**，为该事件启用调用栈
  - 示例事件: `kmem:kmalloc`, `kmem:kmalloc_node`, `kmem:mm_page_alloc`

- **释放事件** (`--free`):
  - 格式: `sys:name[/filter/ptr=EXPR/]`
  - `ptr=EXPR`: **必需**，计算要释放的指针
  - 示例事件: `kmem:kfree`, `kmem:mm_page_free`

### 过滤器层次
1. **trace event filter（内核态）**: 在事件定义的第一个 `/` 后指定，高效减少数据量
2. **时间过滤（用户态）**: `--than` 选项，过滤长期未释放的内存

### 事件处理
- **排序依赖**: 强烈建议启用 `--order`，保证事件按时间戳顺序处理，避免因乱序导致误报
- **丢事件处理**: 检测到事件丢失时，立即报告当前泄漏，清空跟踪数据重新开始

## 输出

### 输出格式

**泄漏事件报告**（默认或 `-v`）:
```
KMEMLEAK REPORT: 15
      305.266631  4518 [000] kmalloc: call_site=c09f56b9 ptr=f59a7600 bytes_req=1024 bytes_alloc=1024 gfp_flags=208
    ffffffff81234567 kmalloc
    ffffffff81abcdef some_function+0x123
    ffffffff81fedcba caller_function+0x45
```

**泄漏字节报告**（启用 `size` 属性且 `-g`）:
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

**统计信息**（程序退出或 SIGUSR1）:
```
KMEMLEAK STATS:
ALLOC LIST num 128 mem 65536
TOTAL alloc 10245 free 10117
```

### 关键指标

| 指标 | 计算方法 | 异常阈值 |
|------|----------|----------|
| 泄漏事件数 | ALLOC LIST num | 持续增长表示泄漏 |
| 泄漏字节数 | Leak of X bytes | 根据系统内存判断 |
| alloc/free比 | TOTAL alloc / free | 显著大于1表示泄漏 |

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
```

### 高级技巧
```bash
# 多个分配函数联合跟踪
perf-prof kmemleak \
    --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/,kmem:kmalloc_node//ptr=ptr/size=bytes_alloc/stack/ \
    --free kmem:kfree//ptr=ptr/ --order -m 256 -g

# 过滤大内存分配 (>1MB)
perf-prof kmemleak \
    --alloc 'kmem:kmalloc/bytes_alloc>1048576/ptr=ptr/size=bytes_alloc/' \
    --free kmem:kfree//ptr=ptr/ --order -g

# 检测长时间未释放的内存 (>30秒)
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/ \
                   --free kmem:kfree//ptr=ptr/ --order -g --than 30s

# 泄漏字节报告中显示分配进程名
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/stack/ \
                   --free kmem:kfree//ptr=ptr/ --order -m 128 -g --comm
```

### 性能优化
```bash
# 优化缓冲区大小（根据分配频率调整）
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/ \
                   --free kmem:kfree//ptr=ptr/ --order -m 256  # 高频分配

# 使用过滤器减少事件量
perf-prof kmemleak \
    --alloc 'kmem:kmalloc/bytes_alloc>=1024 && bytes_alloc<=4096/ptr=ptr/' \
    --free kmem:kfree//ptr=ptr/ --order -g
```

### 组合使用
```bash
# 与 top 配合：先找热点再定位泄漏（新内核支持call_site.function过滤器）
perf-prof top -e 'kmem:kmalloc//key=call_site/printkey=printf("%lx",key)/' -i 1000
perf-prof kmemleak --alloc kmem:kmalloc/call_site.function==hotspot_func/ptr=ptr/ \
                   --free kmem:kfree//ptr=ptr/ --order -g

# 多阶段分析
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/ \
                   --free kmem:kfree//ptr=ptr/ --order -m 128  # 阶段1: 快速扫描
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/ \
                   --free kmem:kfree//ptr=ptr/ --order -m 256 -g  # 阶段2: 详细分析
```

## 相关资源
- [表达式系统文档](expr.md)
- [过滤器语法文档](Event_filtering.md)
- [kmemprof 分析器](kmemprof.md) - 内存分配分析
