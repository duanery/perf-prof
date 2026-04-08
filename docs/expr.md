# perf-prof 表达式

perf-prof 表达式编译器和模拟器，使用基于C4的表达式系统，支持完整的C语言表达式语法，专门用于事件属性中的复杂计算。

## 核心语法架构

### 基本语法格式

```bash
sys:event_name[/filter/ATTR/ATTR/...]
```

### EXPR属性语法

```bash
key=EXPR        # 计算键字段
printkey=EXPR   # 打印键字段, 只能用`key`作为变量，如：printkey=printf("%d",key)
top-by=EXPR     # 排序字段表达式
top-add=EXPR    # 显示字段表达式
comm=EXPR       # 进程名表达式
ptr=EXPR        # 指针字段表达式
size=EXPR       # 大小字段表达式
num=EXPR        # 数值分布表达式
role=EXPR       # 事件角色表达式
```

## 支持的操作符（优先级从高到低）

| 优先级 | 操作符 | 描述 |
|--------|--------|------|
| 1 | `++ --` | 后缀递增/递减 |
| 1 | `() []` | 函数调用、数组下标 |
| 2 | `++ --` | 前缀递增/递减 |
| 2 | `+ - ! ~` | 一元正负、逻辑非、位非 |
| 2 | `(type) & * sizeof` | 类型转换、取地址、解引用、大小 |
| 3 | `* / %` | 乘除余 |
| 4 | `+ -` | 加减 |
| 5 | `<< >>` | 位移 |
| 6 | `< <= > >= ~` | 关系比较 |
| 7 | `== !=` | 相等比较 |
| 8 | `&` | 位与 |
| 9 | `^` | 位异或 |
| 10 | `|` | 位或 |
| 11 | `&&` | 逻辑与 |
| 12 | `||` | 逻辑或 |
| 13 | `?:` | 三元条件 |
| 14 | `=` | 赋值 |
| 15 | `,` | 逗号 |

## 事件字段作为变量

在EXPR中，可以直接使用事件的字段名作为变量，此外还可以使用`_cpu`/`_pid`变量。

- `_cpu` 事件发生的CPU
- `_pid` 事件发生的进程ID（非线程ID）

## 实际工作流程

```c
// 1. 编译表达式
struct expr_prog *prog = expr_compile(expression, declare);

// 2. 加载事件数据
expr_load_data(prog, raw_data, raw_size);

// 3. 执行表达式
long result = expr_run(prog);
```

## 内置函数

```c
int printf(char *fmt, args...)       // 格式化输出，支持最多6个参数，返回打印的字符数
char *ksymbol(long addr)             // 根据地址获取内核符号名，返回字符串
int ntohl(int netlong)               // 网络字节序转主机字节序
short ntohs(short netshort)          // 网络字节序转主机字节序
int strncmp(const char *s1, const char *s2, long n) // 字符串比较，返回<0, =0, >0的值
char *comm_get(int pid)              // 获取pid对应的进程名
char *syscall_name(int id)           // 获取id对应的系统调用名
char *exit_reason_str(int isa, int val) // 根据isa和val获取虚拟化退出原因字符串
int system(const char *format, ...)  // 格式化并执行命令
```

## 扩展操作符

### 通配符运算符

```c
~ (const char *str, const char *pattern)    // 通配符匹配操作符，返回1表示匹配，0表示不匹配
```

#### 功能说明

- 重载位非操作符 (~)，变成一个二元操作符，左操作数为字符串，右操作数为匹配的模式
- 支持标准的 trace event filter 通配符语法
- 优先级与关系运算符 (>,>=,<,<=) 相同
- 仅支持 char* 类型操作数

#### 支持的通配符

- `*` - 匹配任意数量字符（包括零个）
- `?` - 匹配单个字符
- `[abc]` - 匹配字符集中的任意字符
- `[a-z]` - 匹配指定范围内的字符
- `[^abc]` 或 `[!abc]` - 匹配不在字符集中的任意字符

#### 使用场景

- 当内核态 trace event filter 失败时，提供用户态过滤实现
- 在表达式中进行复杂的字符串模式匹配
- 与其他操作符组合使用构建高级过滤条件
- 可能的路径：
  - `_pid`/事件字段 → `comm_get()` → `~` (如：`comm_get(_pid) ~ "pyth*"`)
  - 事件字段 → `ksymbol()` → `~` (如：`ksymbol(function) ~ "sched*"`)
  - 事件字段 → `syscall_name()` → `~`
  - 事件字段 → `~`

### 字符串比较运算符

#### == 和 != 运算符（字符串比较）

```c
==(const char *s1, const char *s2)
!=(const char *s1, const char *s2)
```

#### 功能说明

- 当两个操作数都是字符串指针（char *）时，== 和 != 运算符执行字符串内容比较而不是指针比较
- 使用 strcmp() 进行字符串比较
- 返回值：1 表示真（true），0 表示假（false）
- 自动类型检测：编译器在编译时检测操作数类型，只有当两个操作数都是 char* 类型时才使用字符串比较
- 字符串比较不影响现有的数值比较功能

#### 支持的通配符

- 无（执行精确匹配）
- 如需通配符匹配，请使用 ~ 运算符

#### 使用场景

- 精确匹配进程名、文件名等字符串字段
- 与条件运算符组合进行复杂过滤
- 作为用户态过滤器的一部分
- 与其他字符串运算符（~）配合使用

#### 与 ~ 运算符的区别

- `==` / `!=`: 精确匹配，完全相等才返回真
- `~`: 通配符匹配，支持 *, ?, [abc] 等模式

## 字段类型支持

perf-prof支持以下C语言类型：

- 基本类型：`char`, `short`, `int`, `long`, `unsigned`
- 指针类型：`*` 前缀表示指针
- 数组类型：自动支持数组访问

## 实际应用示例

```bash
# 简单的字段引用
perf-prof top -e sched:sched_wakeup//key=pid/
perf-prof kmemleak --alloc kmem:kmalloc//ptr=ptr/size=bytes_alloc/

# 数值计算
perf-prof num-dist -e 'sched:sched_stat_runtime//num=(runtime/1000)/alias=runtime(us)/'
perf-prof top -e 'sched:sched_stat_runtime//top-by=(runtime/1000)/'

# 位运算
perf-prof multi-trace -e 'kvm:kvm_msi_set_irq//key=(((address>>18)&1)?42:0) + ((address>>12)&0x3f)/'

# 三元条件运算
perf-prof multi-trace -e 'sched:sched_switch//role=(next_pid?1:0)|(prev_pid?2:0)/'

# 复杂条件
perf-prof top -e 'sched:sched_wakeup//key=(prio<10?pid*1000:pid)/'

# 使用ksymbol函数
perf-prof top -e 'workqueue:workqueue_execute_start//comm=ksymbol(function)/'

# 字符串指针操作
perf-prof expr -e sched:sched_process_exec 'printf("%s ", filename)'

# 多字段组合
perf-prof multi-trace -e 'sched:sched_switch//key=(target_cpu*10000+next_pid)/'

# 内存页大小计算
perf-prof kmemprof -e 'kmem:mm_page_alloc//size=(4096<<order)/key=page/'

# 通配符匹配 (~ 操作符)
perf-prof expr -e sched:sched_wakeup 'comm ~ "*sh"' -v
perf-prof expr -e workqueue:workqueue_execute_start 'ksymbol(function) ~ "*sched*"'

# 字符串比较
perf-prof expr -e sched:sched_wakeup 'comm == "systemd"'
perf-prof expr -e sched:sched_process_exec 'filename != "/bin/sh"'
```

**注意**：表达式可以使用`()`括起来，避免运算符被作为分隔符，同时避免双引号被误读，如：`/`除法运算符，`,`。如果表达式部分含义shell的特殊字符，使用单引号扩住整个选项。

上述示例所用部分字段：

- `pid` - 进程ID
- `comm` - 进程名
- `target_cpu` - 目标CPU
- `bytes_alloc` - 分配字节数
- `ptr` - 内存分配指针值