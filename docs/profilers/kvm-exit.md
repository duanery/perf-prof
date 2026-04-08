# kvm-exit - KVM虚拟化退出延迟分析

kvm-exit 统计从虚拟机退出(VM-Exit)到重新进入(VM-Entry)之间的延迟，用于分析虚拟化性能开销。

## 概述
- **主要用途**: 监控并分析KVM虚拟机退出到重新进入之间的延迟，统计不同退出原因的延迟分布，识别虚拟化性能瓶颈
- **适用场景**: 虚拟机性能问题排查、虚拟化开销分析、特定VM-Exit事件的延迟监控、Guest性能优化
- **功能分类**: 内建事件类，虚拟化分析，延迟分析，状态配对
- **最低内核版本**: 3.10+ (支持 kvm tracepoints)
- **依赖库**: libtraceevent, libperf
- **平台支持**: x86 (VMX/SVM), ARM64
- **特殊限制**: 需要root权限，需要在Host端运行，不能在Guest内运行
- **参与联合分析**: 不参与联合分析，独立分析器
- **核心技术**: 基于kvm:kvm_exit和kvm:kvm_entry事件配对，按vcpu线程分实例统计延迟分布

## 基础用法
```
perf-prof kvm-exit [OPTION...] [--perins] [--than ns] [--heatmap file] [--filter filter]
```

OPTION:
- `-i, --interval <ms>`      输出间隔，单位：毫秒 (默认：1000ms)
- `-m, --mmap-pages <N>`     环形缓冲区页数 (默认：64页)

FILTER OPTION:
- `--filter <filter>`        事件过滤器，只应用于 kvm:kvm_exit 事件

PROFILER OPTION:
- `--perins`                 按实例统计，每个vcpu线程独立显示统计信息
- `--than <n>`               输出大于指定阈值的事件，单位：s/ms/us/ns (默认ns)
- `--heatmap <file>`         输出延迟热图数据到指定文件(自动添加.lat后缀)

### 示例
```bash
# 监控所有vcpu的kvm-exit延迟，每秒输出一次
perf-prof kvm-exit -p <qemu-pid> -i 1000

# 按vcpu实例分别统计
perf-prof kvm-exit -C 1-4 -i 1000 --perins

# 只监控延迟超过1ms的VM-Exit事件
perf-prof kvm-exit -p <qemu-pid> --than 1ms -i 1000

# 过滤特定的退出原因(例如EPT violation)
perf-prof kvm-exit -p <qemu-pid> --filter 'exit_reason==48' -i 1000
```

## 核心原理

**基本定义**

- **VM-Exit**: 虚拟机退出，Guest执行某些特殊指令或遇到特定事件时，CPU控制权从Guest转移到Host
- **VM-Entry**: 虚拟机重新进入，Host处理完退出原因后，恢复Guest执行
- **退出原因(exit_reason)**: 导致VM-Exit的具体原因，如IO访问、EPT页错误、MSR访问等
- **ISA类型**: 指令集架构类型，x86平台包括VMX(Intel)和SVM(AMD)，ARM平台为ARM架构
- **vcpu线程**: 每个虚拟CPU对应一个QEMU用户态线程

**数据模型**

事件 → 线程配对 → 延迟计算 → 按退出原因聚合 → 统计输出

### 事件源

kvm-exit 使用内核内建的 KVM tracepoint 事件，无需用户自定义。

- **sample_type**: `PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW`
  - `PERF_SAMPLE_TID`: 线程ID，用于区分不同vcpu
  - `PERF_SAMPLE_TIME`: 事件时间戳，用于计算延迟
  - `PERF_SAMPLE_CPU`: CPU编号
  - `PERF_SAMPLE_RAW`: 原始事件数据，包含退出原因等详细信息

- **内建事件**:
  - `kvm:kvm_exit`: VM-Exit事件，关键字段：
    - `exit_reason`: 退出原因编码
    - `isa`: 指令集架构类型(VMX/SVM/ARM)
    - `guest_rip`: Guest指令指针(PC)
  - `kvm:kvm_entry`: VM-Entry事件，关键字段：
    - `vcpu_id`: 虚拟CPU编号

#### 过滤器

kvm-exit 的 `--filter` 选项仅应用于 `kvm:kvm_exit` 事件，在内核态执行，高效过滤特定的退出原因。

**常见退出原因过滤示例**:

x86平台 (Intel VMX):
```bash
# IO指令 (EXIT_REASON_IO_INSTRUCTION = 30)
--filter 'exit_reason==30'

# EPT页错误 (EXIT_REASON_EPT_VIOLATION = 48)
--filter 'exit_reason==48'

# 排除HLT指令 (EXIT_REASON_HLT = 12)
--filter 'exit_reason!=12'
```

**注意**: HLT指令导致的VM-Exit在统计时会被特殊处理：
- 因为HLT是Guest空闲状态，不代表实际的内核态工作负载
- --than选项不显示HLT的VM-Exit/VM-Entry事件

### 事件处理

**配对机制**:

1. **per-instance状态维护**: 
- `-p pid` 实例是vcpu线程。`-C cpus` 实例是CPU，需要保证每个vcpu线程都绑定独立的CPU上的。
- 每个实例有独立的ringbuffer，其kvm_exit事件和kvm_entry事件是有序的
   ```c
   ctx->perins_kvm_exit[instance]        // 存储每个实例最近的kvm_exit事件
   ctx->perins_kvm_exit_valid[instance]  // 标记是否有待配对的kvm_exit
   ```

2. **事件配对流程**:
   ```
   收到kvm:kvm_exit事件:
   └─> 解析退出原因和ISA类型
   └─> 存储到 perins_kvm_exit[instance]
   └─> 设置 perins_kvm_exit_valid[instance] = 1

   收到kvm:kvm_entry事件:
   └─> 检查 perins_kvm_exit_valid[instance] == 1
   └─> 验证线程ID匹配: entry.tid == exit.tid
   └─> 验证时间顺序: entry.time > exit.time
   └─> 计算延迟: delta = entry.time - exit.time
   └─> 延迟统计
   └─> 清除 perins_kvm_exit_valid[instance] = 0
   ```

3. **异常处理**:
   - **线程ID不匹配**: 输出WARN信息(需要 -v 选项)，丢弃该配对
   - **时间戳逆序**: 静默丢弃该配对
   - **事件丢失**: 标记该instance的缓存无效，等待下一个完整配对

**延迟统计**:

使用 `latency_dist` 延迟分布统计模块，支持：
- 按退出原因分类统计 (通过key: `(isa<<32)|exit_reason`)
- 记录最小值、最大值、平均值
- 使用t-digest算法计算p99延迟
- 统计Guest内核态比例 (%gsys): 根据 `guest_rip >= START_OF_KERNEL` 判断

**热图输出**:

当指定 `--heatmap` 选项时，每个配对成功的延迟都会写入热图文件：
```c
heatmap_write(ctx->heatmap, exit_time, delta)
```
格式: `<时间戳> <延迟纳秒>`

**无需排序的特殊设计**:

kvm-exit 默认不需要 `--order` 选项，因为：
- 事件配对是基于线程的，每个vcpu线程的事件天然有序
- per-instance的状态管理避免了跨实例的顺序依赖
- 只需要同一线程内的exit和entry时间戳正确即可

## 输出

### 输出格式

**表头含义**:

不使用 `--perins` 时:
```
exit_reason       calls   total(us)     min(us)     avg(us)     p99(us)     max(us)  %gsys
```

使用 `--perins` 时: 
``` bash
# -C cpus
[CPU] exit_reason       calls   total(us)     min(us)     avg(us)     p99(us)     max(us)  %gsys

# -p pid
[THREAD] exit_reason       calls   total(us)     min(us)     avg(us)     p99(us)     max(us)  %gsys
```
--------------------------------
**各列说明**:
- `[CPU]` / `[THREAD]`: 实例标识，CPU编号或线程ID
- `exit_reason`: 退出原因字符串，Intel平台显示20字符宽度，其他平台显示32字符宽度
- `calls`: 该退出原因发生的总次数
- `total(us)`: 总延迟时间(微秒)或总周期数(kcyc)，取决于 `--tsc` 选项
- `min(us)`: 最小延迟
- `avg(us)`: 平均延迟 = total / calls
- `p99(us)`: 99分位延迟，表示99%的事件延迟小于等于此值
- `max(us)`: 最大延迟
- `%gsys`: Guest内核态占比，表示这些VM-Exit发生时Guest在内核态的比例

**数据单位**:
- 默认使用微秒 (us)，小数点后保留3位
- 使用 `--tsc` 时显示为kcyc (千周期)

**行索引**:
- 使用 `--perins` 时，以（实例，exit_reason）为索引，为每个实例独立显示一组统计
- 不使用 `--perins` 时，以（exit_reason）为索引，为所有实例合并统计

**排序规则**:
- 按 `total(us)` 从大到小显示每一行

**详细输出**:
当使用 `--than` 选项时，每当延迟超过阈值，会立即打印kvm_exit和kvm_entry事件：
```
<timestamp> kvm_exit: vcpu=X exit_reason=xxx guest_rip=0xXXXXXXXX
<timestamp> kvm_entry: vcpu=X
```
需要 `--verbose` 选项查看更详细的事件信息。

### 关键指标

- **calls**：退出次数，越高，虚拟机的性能越差
- **total**：每个退出原因的总延迟。所有退出原因的total值之和，衡量Host侧整体损耗
- **p99延迟**：衡量退出的抖动情况，值越大，虚拟机内的业务就会有感知
- **最大延迟**：重点关注大的退出延迟
- **Guest内核态占比(%gsys)**：衡量Guest业务处于内核态还是用户态

## 分析方法

### 基础分析方法

**第一步: 整体监控**
```bash
# 监控所有vcpu的综合情况
perf-prof kvm-exit -p <qemu-pid> -i 1000
```
观察整体延迟水平和退出原因分布，根据业务状况重点关注
- 业务整体高负载：关注退出次数多、总耗时长的。
- 业务有抖动：关注p99分位延迟、最大延迟。

**第二步: 按vcpu分析**
```bash
# 如果发现异常，按vcpu分别查看
perf-prof kvm-exit -p <qemu-pid> --perins -i 1000
```
确定是所有vcpu的普遍问题，还是某个vcpu的特定问题。

**第三步: 延迟分析**
使用multi-trace等分析器，重点分析延迟原因


### 数据驱动分析

**不预设任何业务特征**:
- 先用整体监控了解退出原因分布
- 根据calls最多的退出原因，决定下一步方向

## 应用示例

### 基本监控

```bash
# 监控虚拟机的KVM退出延迟
perf-prof kvm-exit -p $(pidof qemu-system-x86_64) -i 1000

# 输出示例:
# exit_reason                         calls        total(us)      min(us)      avg(us)      p99(us)      max(us)  %gsys
# -------------------------------- -------- ---------------- ------------ ------------ ------------ ------------ ------
# EXIT_HLT                              911      3916645.237        0.620     4299.281    20121.135    38825.427 100.00
# EXIT_MSR                             2655         2412.115        0.320        0.908        2.029        8.450 100.00
# EXIT_IOIO                              62          444.878        1.120        7.175       29.691       29.691 100.00
```

## 退出原因参考

### x86 (Intel VMX)

常见退出原因及其含义：

| exit_reason | 名称 | 描述 | 
|-------------|------|------|
| 0 | EXCEPTION_NMI | 异常或NMI |
| 1 | EXTERNAL_INTERRUPT | 外部中断 |
| 10 | CPUID | CPUID指令 |
| 12 | HLT | HLT指令 |
| 28 | CR_ACCESS | 控制寄存器访问 |
| 30 | IO_INSTRUCTION | IO指令 |
| 31 | MSR_READ | 读MSR |
| 32 | MSR_WRITE | 写MSR |
| 48 | EPT_VIOLATION | EPT页错误 |
| 54 | WBINVD | WBINVD指令 |
| 55 | XSETBV | XSETBV指令 |

### x86 (AMD SVM)

| exit_code | 名称 | 描述 |
|-----------|------|------|
| 0x78 | SVM_EXIT_HLT | HLT指令 |
| 0x7B | SVM_EXIT_IOIO | IO指令 |
| 0x7C | SVM_EXIT_MSR | MSR访问 |
| 0x400 | SVM_EXIT_NPF | 嵌套页错误 |

### ARM64

| esr_ec | 名称 | 描述 |
|--------|------|------|
| 1 | WFI_WFE | WFI/WFE指令 |
| 24 | DABT_LOW | 数据中止(低异常级别) |
| 32 | IABT_LOW | 指令中止(低异常级别) |
| 36 | DABT_CURRENT | 数据中止(当前异常级别) |

## 相关资源

- [事件基础文档](../events/)
- [sample_type采样类型说明](../sample_type.md)
- [选项参数完整参考](../main_options.md)
- [KVM虚拟化架构](https://www.linux-kvm.org/page/Documents)
- [Intel VT-x规范](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [ARM虚拟化扩展](https://developer.arm.com/documentation/)
