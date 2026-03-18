# breakpoint - 硬件断点分析
利用CPU调试寄存器的硬件断点功能，跟踪指定地址的读、写、执行操作。

## 概述
- **主要用途**: 使用PERF_TYPE_BREAKPOINT PMU在内核态建立硬件断点，跟踪指定内存地址的访问行为。采样断点触发时的通用寄存器值，用于配合反汇编分析断点位置的指令。对于x86平台内核地址的写断点，可以通过解码指令计算写入的值。
- **适用场景**:
  - 跟踪特定内存地址的读写访问
  - 分析自旋锁加解锁路径
  - 跟踪引用计数的加减变化
  - 分析内核数据结构的修改
- **功能分类**: 内建事件类，硬件性能监控，断点分析
- **最低内核版本**: 2.6.33+（perf_event支持）
- **平台支持**: x86/x86_64（完整功能）, ARM64（基础功能）
- **特殊限制**:
  - 每个进程最多4个硬件断点（CPU调试寄存器限制）
  - 写入值解码仅支持x86平台的内核地址
  - XCHG、XADD指令暂不支持写入值解码
- **参与联合分析**: 不支持联合分析

## 基础用法
```bash
perf-prof breakpoint <addr>[/len][:type] [-g] [--filter EXPR]
```

### 地址格式
```
<addr>[/len][:type]

addr: 十进制或十六进制地址（0x前缀）
len:  断点长度，可选值：1（默认）、2、4、8字节
type: 断点类型
      r - 读（自动包含写）
      w - 写（默认）
      x - 执行（长度固定为sizeof(long)）
```

### OPTION
- `-m, --mmap-pages`: 默认1页，开启-g时自动翻倍
- `-i, --interval`: 不支持周期输出

### FILTER OPTION
- `--user-callchain[=dwarf[,size]]`: 包含用户态调用栈，`no-`前缀排除。`=dwarf` 启用DWARF栈回溯
- `--kernel-callchain`: 包含内核态调用栈，`no-`前缀排除
- `--exclude-user`: 过滤用户态断点触发
- `--exclude-kernel`: 过滤内核态断点触发

### PROFILER OPTION
- `-g, --call-graph`: 采样调用栈
- `--flame-graph <file>`: 输出火焰图到文件
- `--filter <EXPR>`: 过滤写入值，使用'data'变量引用写入的值（仅x86内核地址写断点）

## 核心原理

### 数据模型
```
断点触发 → [排序(写断点)] → 寄存器采样 → [指令解码(x86写断点)] → 输出/过滤
```

### 硬件断点机制
- **PERF_TYPE_BREAKPOINT**: 使用perf_event接口创建硬件断点
- **CPU调试寄存器**: 利用x86的DR0-DR3（4个断点地址寄存器）和DR7（控制寄存器）
- **断点类型**:
  - 数据写断点（Trap）：指令执行完成后触发
  - 数据读断点（Trap）：与写断点相同机制
  - 指令执行断点（Fault）：指令执行前触发

### 寄存器采样
配置`PERF_SAMPLE_REGS_INTR`采样断点触发时的通用寄存器：
- **x86_64**: RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP, R8-R15, RIP, RFLAGS, CS, SS
- **ARM64**: X0-X29, LR, SP, PC

### 写入值解码（x86内核地址）
对于x86平台的内核地址写断点：
1. 通过`kcore`读取断点位置的指令字节
2. 使用Intel指令解码器解析指令
3. 结合采样的寄存器值，从指令源操作数计算写入值
4. 跟踪连续写入，记录值的变化规律

**支持的指令类型**:
| 类别 | 指令 | 说明 |
|------|------|------|
| MOV | MOV, MOVABS | 直接赋值 |
| 原子操作 | CMPXCHG | 比较交换 |
| 算术运算 | ADD, SUB, INC, DEC, NEG | 加减操作 |
| 逻辑运算 | OR, AND, XOR, NOT | 位操作 |
| 移位/旋转 | SHL, SHR, SAL, SAR, ROL, ROR | 移位操作 |
| 位测试 | BTS, BTR, BTC | 位设置/清除/翻转 |

**不支持的指令**: XCHG, XADD（需要读取内存原值）

### 安全性考虑
- **数据安全标记**: 跟踪指令类型判断写入值是否可靠
- **MOV类指令**: 安全（直接写入，不依赖原值）
- **ADD类指令**: 需要前序安全指令建立上下文

## 输出

### 输出格式

**基础输出（无-g）**:
```
           <comm> <pid> [cpu] timestamp: breakpoint: 0x<addr>/len:type ip <rip>
      寄存器信息...
```

**带调用栈输出（-g）**:
```
           <comm> <pid> [cpu] timestamp: breakpoint: 0x<addr>/len:type
      寄存器信息...
      调用栈...
```

**写入值解码输出（x86内核写断点）**:
```
           <comm> <pid> [cpu] timestamp: breakpoint: 0x<addr>/len:type ip <rip>
      INSN: <hex bytes>  <disassembly> ADDR: <addr>  DATA: <value>
      调用栈...
```

### 输出字段
| 字段 | 说明 |
|------|------|
| comm | 触发断点的进程名 |
| pid | 进程ID |
| cpu | 触发的CPU编号 |
| timestamp | 断点触发时间戳 |
| addr | 断点监控地址 |
| len | 断点长度 |
| type | 断点类型（R/W/X） |
| ip | 触发断点的指令地址 |
| INSN | 指令十六进制字节和反汇编 |
| ADDR | 实际写入的内存地址 |
| DATA | 写入的值 |

### 寄存器输出（x86_64）
```
      RIP: <value> RSP: <value> RFLAGS:<value>
      RAX: <value> RBX: <value> RCX: <value>
      RDX: <value> RSI: <value> RDI: <value>
      RBP: <value> R08: <value> R09: <value>
      R10: <value> R11: <value> R12: <value>
      R13: <value> R14: <value> R15: <value>
      CS: <value> SS: <value>
```

## 应用示例

### 基础示例
```bash
# 1. 用户态地址断点（需要进程附加）
perf-prof breakpoint 0x7ffd8c7dae28 -g                    # 监控栈地址写入

# 2. 内核地址写断点
perf-prof breakpoint 0xffffffff82345678/4:w -g            # 监控4字节内核变量写入

# 3. 内核地址读写断点
perf-prof breakpoint 0xffffffff82345678/8:rw -g           # 监控8字节读写

# 4. 指令执行断点
perf-prof breakpoint 0xffffffff81234560:x -g              # 监控函数入口执行
```

### 过滤写入值
```bash
# 过滤特定写入值
perf-prof breakpoint 0xffffffff82345678/4:w -g --filter 'data==1'    # 值等于1时输出
perf-prof breakpoint 0xffffffff82345678/4:w -g --filter 'data>0'     # 值大于0时输出
perf-prof breakpoint 0xffffffff82345678/4:w -g --filter 'data&0x1'   # 最低位为1时输出
```

### 自旋锁分析
```bash
# 1. 获取自旋锁地址
grep "spinlock_name" /proc/kallsyms

# 2. 监控锁的获取释放
perf-prof breakpoint <lock_addr>/4:w -g                   # 监控锁状态变化
perf-prof breakpoint <lock_addr>/4:w -g --filter 'data==1'  # 仅显示加锁操作
perf-prof breakpoint <lock_addr>/4:w -g --filter 'data==0'  # 仅显示解锁操作
```

### 引用计数分析
```bash
# 1. 获取对象引用计数字段地址
# 假设 struct kref 在对象偏移+0x10处

# 2. 监控引用计数变化
perf-prof breakpoint <object_addr+0x10>/4:w -g            # 监控计数变化
perf-prof breakpoint <object_addr+0x10>/4:w -g --filter 'data==0'  # 计数归零时输出
```

### 火焰图生成
```bash
# 生成断点触发的调用栈火焰图
perf-prof breakpoint 0xffffffff82345678/4:w -g --flame-graph bp

# 使用flamegraph.pl生成SVG
flamegraph.pl bp.folded > bp.svg
```

### 多断点同时监控
```bash
# 最多4个断点同时工作
perf-prof breakpoint 0xaddr1/4:w 0xaddr2/4:w 0xaddr3/8:w -g
```

### 性能优化
```bash
# 减少输出：使用过滤器减少不关心的事件
perf-prof breakpoint <addr>/4:w -g --filter 'data!=0'     # 过滤写入0的情况

# 仅内核态：过滤用户态触发
perf-prof breakpoint <addr>/4:w -g --exclude-user
```

## 注意事项

### 硬件断点限制
- x86/x86_64每个CPU有4个断点寄存器（DR0-DR3）
- 断点是进程级资源，同一进程最多4个断点
- 系统调试器可能占用部分断点寄存器

### 写入值解码限制
- 仅支持x86/x86_64平台
- 仅支持内核地址（需要kcore访问）
- 不支持XCHG、XADD指令（需要读取内存原值）
- 用户态地址不支持值解码（kcore无法读取用户空间）

### 性能影响
- 硬件断点开销较低
- 高频触发的断点可能影响系统性能
- 建议配合过滤器减少输出

## 相关资源
- Intel SDM Volume 3, Chapter 19: Debug, Branch Profile, TSC, and Intel Resource Director Technology
- Linux内核文档: Documentation/trace/hw_breakpoint.rst
