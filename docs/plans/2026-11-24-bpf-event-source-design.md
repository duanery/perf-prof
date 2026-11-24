# BPF 事件源：`-e bpf:xxx` 与 tracepoint / kprobe 并列

Status: Done — implemented in commits 1a38096 f2d1efb 8c0c552 668681a 277e00b 8ca79bb (source) / afda002 (tests) / cca590e (docs); 与本稿的差异见第 13 节
Date: 2026-11-24
Base: main 65efec1

## 0 已确认（用户，2026-11-24）

1. **不强制 `trace_entry` 头**：BPF 事件的 raw 就是 BTF 里的事件结构体本身。事件经 **evsel → tp** 关联到它的 tp，
   不靠 raw 里的 `common_type`。pid / tid / time / cpu 从 perf sample 头取。
2. **注册方式**：每个 `.bpf.c` 在构建时由同一模板生成 `xxx.event.o`，里面的 constructor 自动注册；
   不手写注册代码。
3. **重名**：`-e bpf:kvm_exit` 指向事件源；`perf-prof bpf:kvm_exit` 命令仍是现有分析器。
4. 按 `skills/refactor-design` 流程：本稿 → 独立审查 → 人确认 → 实现。
5. Q1 模块位置 `src/bpf_event.c/h`；`bpf-skel/` 只放 BPF 程序、模板与生成物。
6. Q2 加演示源，编进正式二进制；按用户要求做成真实的调度延迟事件 `bpf:sched_rundelay`（`src/bpf-skel/sched_rundelay.bpf.c`）：
   sched_wakeup / sched_wakeup_new / sched_switch（prev 仍可运行）→ sched_switch 切入，输出 comm、tgid、pid、prio、policy、rundelay，
   另加 reason（变为可运行的原因）与 runnable_cpu（变为可运行时所在 cpu）。
7. Q3 `tp->id` 每个源一个（`BPF_EVENT_BASE + tep 表下标`）；不支持 push / pull。
8. Q4 `tp_print_event()` 加 `tid` 参数，7 个调用点都传 sample tid，tracepoint 也统一用它。
9. Q5 取 (a)：只有 BPF 事件的进程 scope 下，目标进程退出检测不到，第 1 期不处理，文档写明。
10. **源的配置与输出都按源预留**：BPF 事件源内部会有更多配置参数，`ops->prepare` 是每个源单独设置的接口；
    输出同样按源预留 `ops->print`——事件结构体里带有自己采样的栈等内容时，需要源自己的打印函数。
11. **进程 scope 按 tgid**：BPF 事件源跟踪进程只需要主进程的 tgid（`-p` 给的是进程 id，`-t` 的线程换算成所属进程）；
    跟踪多个进程用一个 map。为它预留：scope 直接给出去重后的 tgid 列表，多进程的 map 约定留到以后实现。
    不使用 `pid_filter`——这里只需要一份静态的 tgid 集合，用不到它的跟随、增删延迟、丢弃计数。
12. **框架用一个公共函数启动 BPF 事件，查找仍是 evsel → tp → bpf**：evlist 打开之后由框架函数
    `prof_dev_bpf_start()` 启动 dev 上的所有 BPF 事件。实例挂在 tp 上（`tp->bpf`），过滤器来自 `tp->filter`
    （BPF 事件的 `/filter/`），所以经 tp 查找，不另在 evsel 私有数据里存一份。

## 1 问题

现在 BPF 程序只能写成一个分析器（`src/bpf_kvm_exit.c`）：

- 每个 BPF 程序都要手写一份 open → 注入过滤器 → load → 建 `PERF_COUNT_SW_BPF_OUTPUT` evsel →
  填 `perf_events` map → attach 的代码（bpf_kvm_exit.c 约 150 行），以及打印、帮助。
- `-e bpf:kvm_exit` 走"分析器作为事件源"的转发：用不了 `key=` / `top-by=` / `num=` / `/filter/` 等按字段
  工作的属性，`help` 看不到字段，不能被 trace / top / multi-trace / num-dist 按字段使用。
- `perf-prof list` 查不到 BPF 事件。

目标：写一个 `.bpf.c`、在 Build 里加一行，就得到一个 `bpf:xxx` 事件，与 `sys:name` 同等地被所有基于
`tp_list` 的分析器使用：

```
perf-prof list bpf
perf-prof trace -e bpf:xxx help
perf-prof trace -e 'bpf:xxx/pid>100 && lat>1000/key=pid/' -C 0-3
perf-prof multi-trace -e 'sched:sched_wakeup//key=pid/' -e 'bpf:xxx//key=pid/'
```

## 2 使用者

**现有使用者**（都经 `tep.c` / `monitor.c` 的适配，分析器本身不调用新模块）：

| 使用者 | 用到什么 |
|---|---|
| `tp_list_new()` 解析 | 按名字找源；字段（属性表达式编译）；大小 |
| `tp_evsel_new()` | 建实例与它的 BPF_OUTPUT evsel |
| `prof_dev_open` 流程（monitor.c） | evlist 打开之后启动实例（注入过滤器、加载、绑定输出、attach） |
| `tp_list_free()` | 释放实例 |
| `tp_print_event()` | 打印一条事件 |
| `order.c` `perf_sample_max_size()` | raw 大小 |
| `help.c` / `list.c` | 事件格式 / 列出全部源 |
| `tp_matcher_find()` | 默认 matcher 读 `cpu` / `pid` 字段 |
| `bpf_kvm_exit.c` | 定义 kvm_exit 源的 `prepare`，分析器自己的 init 也调用它 |

**未来使用者**（伪代码见 6.6）：

- F1 python / sql 按字段访问 BPF 事件（第 2 期）。
- F2 线程模式下由 `pid_filter` 在内核态收紧 pid。
- F3 `perf-prof bpf:kvm_exit` 分析器改为在事件源之上实现（第 3 期）。
- F4 某个 BPF 源带自己的 `tp_matcher`。

**约束**：perf-prof 编译一次跑在任意内核上。BPF 事件源实际需要 v5.2+ 且内核带 BTF（K9）；
更旧的内核上该事件报明确的错误，不影响其它事件。`CONFIG_LIBBPF` 关闭时没有任何 BPF 源，头文件提供桩，
`bpf:xxx` 回到现有的分析器 / tracepoint 解析。

## 3 分层

| 层 | 位置 | 职责 | 不管 |
|---|---|---|---|
| BPF 程序 | `src/bpf-skel/xxx.bpf.c`（+ 类型头 `xxx.h`） | 产生事件；`perf_output()` 输出整个定长结构体；`DEFINE_EXPR_FILTER(struct T)`；必须有 `BPF_EVENT_DESC()`；自己按 scope 过滤（pid、cgroup 等，见 K3、K12） | 用户态、绑定 |
| 注册胶水（生成） | `src/bpf-skel/bpf_event.tmpl.c` → `xxx.event.o` | 把 skeleton 的 open / obj / load / attach / destroy 包成类型无关的函数；从 `struct xxx_bpf` 的成员取描述与 `perf_events` map（缺了编译失败）；constructor 注册 | 字段、加载流程、ops |
| 源的用户态配置（可选） | 任意 `.c`，如 `bpf_kvm_exit.c` | `BPF_EVENT_OPS(xxx, &ops)`：`prepare` 按 scope 设置 rodata / bss / autoload | 何时调用 |
| **bpf_event 模块** | `src/bpf_event.c/h` | 注册表与 ops 绑定；源级：BTF 字段 / 大小 / 描述、打印、帮助；实例：建 BPF_OUTPUT evsel（`inherit=0`、按 cpu、dummy 线程、`sample_period≠0`）；启动：open → 注入过滤器 → prepare → load → 从 evsel 读 fd 写 `perf_events` → attach；过滤器装不进内核时（含 verifier 拒绝）重开不带过滤器的对象 | tp、tp_list、prof_dev、env、`tp->id`；要不要回退到用户态过滤；何时启动 |
| tep 适配 | `src/tep.c/h` | `EVT_HEAD_BPF`；`tp->id` 的分配与 id → 源的表；tp ↔ 源 / 实例；scope 从 dev 取；`tp_bpf_start()` 用 `tp->filter` 启动，回退时编译用户态过滤器；`tep__event_fields/size`、print、ftrace_filter 查找、默认 matcher 的 BPF 分支 | BTF、libbpf、skeleton、fd |
| 框架 | `src/monitor.c` | `perf_evlist__open()` 之后、`prof->filter` 之前调公共函数 `prof_dev_bpf_start()`：遍历 evsel，`tp_bpf_start(perf_evsel_tp(evsel))` | 源、字段、过滤器、回退 |
| 使用者 | list / help / order / 各分析器 | 列出、帮助、大小；分析器照常 evsel → tp | BPF 的一切 |

## 4 原则

1. **BPF 事件就是一种 tp。** 分析器不区分 tracepoint 与 BPF 事件；区别收在 `tep.c` 的按 id 分支里。
2. **K3 / K4 的知识归模块。** BPF_OUTPUT evsel 的属性（按 cpu、dummy 线程、`inherit=0`、必须采样）与
   "从 evsel 读出 cpu → fd 写进 map"都在模块里；调用者只给 attr 模板和 scope。模块依赖 libperf（下层），
   不依赖 tp / prof_dev（准则 2）。
3. **启动时机归框架。** evlist 打开之后由框架的公共函数 `prof_dev_bpf_start()` 统一启动，不依赖各分析器是否实现
   `.filter`（准则 4）；它经 evsel → tp 找到实例，用 tp 上的过滤器启动。
4. **回退由调用者决定，模块只报告结果。** 调用者用 `BPF_EVENT_USERSPACE_OK` 允许"不带过滤器启动"，模块用返回值
   报告是否真的装进了内核（准则 3）。
5. **scope 与输出绑定分开**（准则 6）：scope 是"跟踪什么"（cpu 集合 / 线程集合），由调用者给；输出绑定是"BPF_OUTPUT
   在哪些 cpu 上打开"，由模块按 scope 决定（cpu scope：这些 cpu；进程 scope：全部在线 cpu）。
6. **字段来自 BTF，唯一来源是 `expr_filter()` 的参数类型**；源级数据由模块持有，第一次查询时解析（只 open，
   不 load、不需要 root），进程退出时由 destructor 释放（准则 10）。
7. **模块不知道 `tp->id`**：id 的编码与 id → 源的表归 tep（准则 11）。

## 5 事件源约定（写 `.bpf.c` 的人遵守）

```c
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include "perf_output.bpf.h"     /* perf_events map 与 perf_output() */
#include "expr_filter.bpf.h"     /* DEFINE_EXPR_FILTER() */
#include "bpf_event.bpf.h"       /* 新增：BPF_EVENT_DESC() */
#include "sched_switch.h"        /* 事件结构体：skeleton 只引用 .bss 里的类型、不定义，模板要先 include 它 */

BPF_EVENT_DESC("sched_switch with prev/next pid and comm.");   /* 必须 */

struct sched_switch_event event = {};   /* 可写内存：.bss 或 map value */

DEFINE_EXPR_FILTER(struct sched_switch_event)

SEC("raw_tp/sched_switch")
int BPF_PROG(sched_switch, bool preempt, struct task_struct *prev, struct task_struct *next)
{
    ...填 event...
    if (expr_filter(&event))
        perf_output(ctx, &event, sizeof(event));
    return 0;
}
```

1. 必须有 `perf_events` map、`BPF_EVENT_DESC()` 与 `expr_filter()`。前两者由模板从 skeleton 成员取，缺了 `xxx.event.o` 编译失败；
   `expr_filter()` 缺失在第一次解析该源时报错，该源不可用。
2. **每次输出整个结构体**（`sizeof`）：表达式按 BTF 偏移读 raw、不做越界检查，截断的输出会让表达式读到 raw 之外。
   kvm_exit 线程模式现在输出 `offsetof(runq_delay)`（`kvm_exit.bpf.c:362`），改为全长输出、未用字段为 0。
3. 成员是标量或标量数组。位域、嵌套结构体、指针成员表达式访问不到（`bpf_expr_filter_fields()` 跳过它们）；
   help 照样列出并标注 `(not accessible)`，不静默丢掉。
4. `tp/` 类程序的返回值决定该 tracepoint 是否还交付给其它 perf 使用者（K11），必须返回 1；推荐用 `raw_tp/`。
5. 源名 = 文件名去掉 `.bpf.c`，必须是合法 C 标识符（不含 `-`；bpftool 会把 `-` 改成 `_`，宏拼接对不上），
   且不能是预定义宏（如 `linux`、`unix`）。`-e bpf:<源名>`。
6. BPF 程序不受 perf 事件作用域约束（K12）：`--exclude-*`、`-G` 对它不生效；进程 scope 的 pid 过滤由 `prepare`
   与程序自己实现。
7. `BPF_EVENT_DESC()` 定义 `.rodata` 变量 `bpf_event_desc`（程序不读它），于是它是 skeleton rodata 的成员，模板从 open 后的
   对象取值。不放进单独的 ELF 段：skeleton 里没有它，而且 libbpf 对每个不认识的数据段打 INFO 日志（K13）。
8. 事件结构体等 `.bss` / `.rodata` 用到的类型放在 `src/bpf-skel/xxx.h`，BPF 程序与模板都 include（旧 bpftool 生成的
   skeleton 只引用这些类型）。源的用户态 ops 放在 `src/` 下的 `.c`，include skeleton 只为结构体定义（见 6.5 演示源）。

**Build**（`src/bpf-skel/Build`）：

```make
perf-prof-$(CONFIG_LIBBPF) += kvm_exit.event.o        # 取代原来的 kvm_exit.skel.h 一行
perf-prof-$(CONFIG_LIBBPF) += sched_switch.event.o
```

`build/Makefile.build`：放在 `obj-y` 加前缀（162–167 行）之后、`bpf-target` 计算（175 行）之前：

```make
event-target := $(filter %.event.o,$(obj-y))
skel-target  += $(patsubst %.event.o,%.skel.h,$(event-target))

event-name = $(basename $(basetarget))
quiet_cmd_cc_o_event = CC      $@
      cmd_cc_o_event = $(CC) $(c_flags) -DBPF_EVENT_NAME=$(event-name) -DBPF_EVENT_SKEL='"$<"' \
                       $(if $(wildcard $(dir)/$(event-name).h),-DBPF_EVENT_TYPES='"$(event-name).h"') \
                       -c -o $@ $(dir)/bpf_event.tmpl.c

$(event-target): $(objprefix)%.event.o: $(objprefix)%.skel.h $(dir)/bpf_event.tmpl.c FORCE
	$(call rule_mkdir)
	$(call if_changed_dep,cc_o_event)
```

`skel-target` 不再来自 `obj-y`，必须同时加进 `targets`（241 行，否则 `.cmd` 读不进来，`if_changed` 每次都重新生成
skeleton、连带重编 `*.event.o`）与 `clean` 列表（233 行）。`%.event.o` 同时匹配通用的 `$(OUTPUT)%.o: %.c` 规则（没有 `kvm_exit.event.c`，不会命中），实现时确认静态规则优先。

**模板** `src/bpf-skel/bpf_event.tmpl.c`（函数名加前缀，避免与 libc 的 `open` 等冲突）：

```c
#include <stdint.h>
#include <bpf_event.h>
#include <bpf-skel/bpf_event.bpf.h>
#ifdef BPF_EVENT_TYPES
#include BPF_EVENT_TYPES                /* 源的类型头 xxx.h */
#endif
#include BPF_EVENT_SKEL                 /* xxx.skel.h */

#define SKEL(x)   __PASTE(__PASTE(BPF_EVENT_NAME, _bpf), x)   /* kvm_exit_bpf__open ... */
#define SKEL_T    struct __PASTE(BPF_EVENT_NAME, _bpf)

static void *skel_open(void)                 { return SKEL(__open)(); }
static struct bpf_object *skel_obj(void *s)  { return ((SKEL_T *)s)->obj; }
static int skel_load(void *s)                { return SKEL(__load)(s); }
static int skel_attach(void *s)              { return SKEL(__attach)(s); }
static void skel_destroy(void *s)            { SKEL(__destroy)(s); }
/* BPF 事件源特有：缺 BPF_EVENT_DESC() 或 perf_events map 时这里编译失败 */
static const char *skel_desc(void *s)        { return (const char *)((SKEL_T *)s)->rodata->bpf_event_desc; }
static struct bpf_map *skel_perf_events(void *s) { return ((SKEL_T *)s)->maps.perf_events; }

static const struct bpf_event_skel skel = {
    skel_open, skel_obj, skel_load, skel_attach, skel_destroy,
    skel_desc, skel_perf_events,
};
BPF_EVENT_REGISTER(BPF_EVENT_NAME, &skel);
```

## 6 接口稿

### 6.1 头文件 `src/bpf_event.h`

```c
#ifndef __BPF_EVENT_H
#define __BPF_EVENT_H

#include <stdbool.h>
#include <errno.h>
#include <linux/compiler.h>     /* __PASTE */
#include <linux/stringify.h>    /* __stringify */

#ifndef __ctor
#define __ctor __attribute__((constructor))
#endif

struct bpf_object;
struct perf_evsel;
struct perf_event_attr;
struct perf_cpu_map;
struct perf_thread_map;
struct global_var_declare;
struct bpf_event_source;    /* 一个注册的源：进程内常驻 */
struct bpf_event;           /* 一个实例：一个 BPF_OUTPUT evsel 与（启动后）一个 bpf_object */

/* ---- 注册：模板与源的配置代码使用 ---- */

/* skeleton 的类型无关包装。@s 是 open() 的返回值（struct xxx_bpf *）。 */
struct bpf_event_skel {
    /* 每个 skeleton 都有的函数 */
    void *(*open)(void);                  /* NULL：失败 */
    struct bpf_object *(*obj)(void *s);
    int (*load)(void *s);                 /* 0 / 负 errno */
    int (*attach)(void *s);
    void (*destroy)(void *s);
    /* BPF 事件源特有，取自 struct xxx_bpf 的成员；源缺了它们，模板编译失败 */
    const char *(*desc)(void *s);              /* BPF_EVENT_DESC()，在 .rodata */
    struct bpf_map *(*perf_events)(void *s);   /* 事件输出用的 map */
};

/* 跟踪什么。由调用者给出，模块据此决定输出绑定（原则 5）并交给 prepare。 */
enum bpf_event_scope_type {
    BPF_EVENT_SCOPE_CPU,        /* 跟踪 @cpus 上的一切 */
    BPF_EVENT_SCOPE_PROCESS,    /* 跟踪 @tgids 这些进程的全部线程，不论在哪个 cpu */
};
struct bpf_event_scope {
    enum bpf_event_scope_type type;
    struct perf_cpu_map *cpus;          /* SCOPE_CPU */
    const int *tgids;                   /* SCOPE_PROCESS：去重后的 tgid，nr_tgids >= 1 */
    int nr_tgids;
};

/* 源的用户态部分：由源自己的 .c 用 BPF_EVENT_OPS() 挂上，每个成员都可选。 */
struct bpf_event_ops {
    /*
     * 每个源单独的配置。每次 open 之后、load 之前调用一次（bpf_event_start() 重开对象时再调一次）。
     * @s：open() 的返回值，可强转为 struct xxx_bpf * 设置 rodata / bss / autoload。
     * 返回 0 继续；-EOPNOTSUPP：该源不能服务这种 scope（如源只支持单进程而 nr_tgids > 1）；其它负值：失败。
     * prepare 为 NULL：只支持 SCOPE_CPU，什么也不设。
     */
    int (*prepare)(void *s, const struct bpf_event_scope *scope);
    /*
     * 每个源单独的输出：打印一条事件的字段部分（头部 comm / pid / cpu / 时间由 tep 打印），以换行结束。
     * 用于结构体里有通用打印表达不了的内容，如源自己采样的栈（打印成符号）、枚举值的名字。
     * @size 是 raw 的大小（可能带内核补齐的尾部，K7）。NULL：按 BTF 通用打印。
     */
    void (*print)(const void *data, int size);
};

void bpf_event_register(const char *name, const struct bpf_event_skel *skel);
/*
 * 源的 ops 由另一个 .c 提供，constructor 先后不定：先记下，第一次查询注册表时按名字绑定；
 * 找不到对应源的 ops 在那时报错（不静默丢失）。
 */
void bpf_event_register_ops(const char *name, const struct bpf_event_ops *ops);

#define BPF_EVENT_REGISTER(name, skel) \
    __ctor static void __PASTE(bpf_event_register_, name)(void) \
    { bpf_event_register(__stringify(name), skel); }
#define BPF_EVENT_OPS(name, ops) \
    __ctor static void __PASTE(bpf_event_ops_, name)(void) \
    { bpf_event_register_ops(__stringify(name), ops); }

#ifdef CONFIG_LIBBPF

/* ---- 源 ---- */
struct bpf_event_source *bpf_event_source_find(const char *name);        /* NULL：没有 */
/* 按名字排序遍历：@prev 为 NULL 取第一个，返回 NULL 结束。 */
struct bpf_event_source *bpf_event_source_next(struct bpf_event_source *prev);
const char *bpf_event_source_name(struct bpf_event_source *src);

/*
 * 以下在第一次调用时 open skeleton（不 load、不需要 root）解析 BTF 与描述并缓存，
 * 进程退出时由 destructor 释放。源不符合第 5 节的约定时返回错误值，只报一次错。
 */
/* 可被表达式访问的字段：malloc 的副本，name 为 NULL 结尾，调用者 free()。NULL：失败。 */
struct global_var_declare *bpf_event_source_fields(struct bpf_event_source *src);
int bpf_event_source_size(struct bpf_event_source *src);                 /* sizeof；负值：失败 */
const char *bpf_event_source_desc(struct bpf_event_source *src);         /* BPF_EVENT_DESC()；失败时为 "" */
/* 描述 + 每个成员的类型、偏移、大小、符号（形如 tracefs 的 format），不可访问的成员加标注。 */
void bpf_event_source_help(struct bpf_event_source *src);
/* 打印字段部分：有 ops->print 用它，否则按 BTF 单行打印 " a=1 b=2 comm=\"x\"\n"（@size 小于结构体时只打印能容纳的字段）。 */
void bpf_event_source_print(struct bpf_event_source *src, const void *data, int size);

/* ---- 实例：new → start → free ---- */

/*
 * 建实例与它的 BPF_OUTPUT evsel（不 open skeleton）。evsel 按 @tmpl 建：type/config 改为
 * SOFTWARE/BPF_OUTPUT，inherit=0（K4）；自带 cpus 与 dummy 线程（K3）：SCOPE_CPU 用 @scope->cpus，
 * SCOPE_PROCESS 用全部在线 cpu。scope（含 tgid 列表）复制进实例。失败返回 NULL，原因已打印：
 *   SCOPE_PROCESS 而源没有 prepare（-EOPNOTSUPP）；@tmpl->sample_period == 0（计数模式，K10）；内存不足。
 * evsel 归调用者加入 evlist、随 evlist 删除；实例只记住它，不持有引用。调用者不得再改它的 cpus / threads。
 */
struct bpf_event *bpf_event_new(struct bpf_event_source *src, const struct bpf_event_scope *scope,
                                const struct perf_event_attr *tmpl);
struct perf_evsel *bpf_event_evsel(struct bpf_event *ev);

/* #define BPF_EVENT_USERSPACE_OK 0x1 放在 #ifdef CONFIG_LIBBPF 之外（tep 两种配置下都用） */
/*
 * 在 evsel 打开之后调用：open → 注入 @filter → prepare → load → 从 evsel 读每个 cpu 的 fd 写进
 * perf_events（key 是 cpu 号，不是 cpu map 的下标，K17）→ attach。@filter 装不进内核（BPF 后端不支持的
 * 构造、内核缺指令，或 verifier 拒绝了注入后的程序）时，带 BPF_EVENT_USERSPACE_OK 则销毁对象、
 * 不带过滤器重新 open → prepare → load。
 *   返回 0：已启动，过滤器（若有）在内核生效。
 *   返回 1：已启动，过滤器不在内核；只在 USERSPACE_OK 时出现，调用者须自己过滤。
 *   负 errno：失败，原因已打印（load 失败时附 "may need a newer kernel or kernel BTF" 提示，K9）；
 *           实例回到未启动状态。evsel 还没打开（fd 为 -1）返回 -EINVAL；已启动时再调用返回 -EBUSY。
 */
int bpf_event_start(struct bpf_event *ev, const char *filter, int flags, int verbose);
/* 只给源自己的代码用（F3）：已启动时返回 skeleton（struct xxx_bpf *），否则 NULL。 */
void *bpf_event_skel(struct bpf_event *ev);
/* detach、destroy。与 evsel fd 的关闭先后无关（K5）。NULL 无操作。 */
void bpf_event_free(struct bpf_event *ev);

#else /* !CONFIG_LIBBPF：没有源，其余接口不可达 */

static inline struct bpf_event_source *bpf_event_source_find(const char *name) { return NULL; }
static inline struct bpf_event_source *bpf_event_source_next(struct bpf_event_source *prev) { return NULL; }
static inline const char *bpf_event_source_name(struct bpf_event_source *src) { return NULL; }
static inline struct global_var_declare *bpf_event_source_fields(struct bpf_event_source *src) { return NULL; }
static inline int bpf_event_source_size(struct bpf_event_source *src) { return -1; }
static inline void bpf_event_source_help(struct bpf_event_source *src) { }
static inline void bpf_event_source_print(struct bpf_event_source *src, const void *data, int size) { }
static inline const char *bpf_event_source_desc(struct bpf_event_source *src) { return ""; }
static inline struct bpf_event *bpf_event_new(struct bpf_event_source *src, const struct bpf_event_scope *scope,
                                              const struct perf_event_attr *tmpl) { return NULL; }
static inline struct perf_evsel *bpf_event_evsel(struct bpf_event *ev) { return NULL; }
static inline int bpf_event_start(struct bpf_event *ev, const char *filter, int flags, int verbose) { return -EOPNOTSUPP; }
static inline void *bpf_event_skel(struct bpf_event *ev) { return NULL; }
static inline void bpf_event_free(struct bpf_event *ev) { }

#endif
#endif
```

说明：`bpf_event_skel` / `bpf_event_ops` / `bpf_event_scope` 是回调表与入参，可见；`bpf_event_source` /
`bpf_event` 不透明。实例的 scope 与 evsel 在 new 时一起确定，start 不再重复传。

`BPF_EVENT_OPS` 找不到源的报错发生在第一次查询注册表时；只运行 `perf-prof bpf:kvm_exit` 这类不查注册表的命令时不报，
可以接受。

### 6.2 tep / monitor 适配

```c
/* tep.h */
enum {
    KPROBE = TRACE_EVENT_TYPE_MAX + 1,
    KRETPROBE,
    UPROBE,
    URETPROBE,
    BPF_EVENT_BASE,     /* BPF_EVENT_BASE + tep 内 id → 源表的下标 */
};
static inline bool tp_is_bpf(struct tp *tp) { return tp->id >= BPF_EVENT_BASE; }

struct tp {
    ...
    struct bpf_event_source *bpf_src;   /* bpf:xxx */
    struct bpf_event *bpf;              /* tp_evsel_new() 建立，tp_list_free() 释放 */
};

/* 启动 @tp 的 BPF 事件：用 tp->filter，回退时开用户态过滤。@tp 为 NULL 或非 BPF tp 返回 0。 */
int tp_bpf_start(struct tp *tp);

/* 按 dev 的绑定得出 scope；cgroup 返回 -EOPNOTSUPP。tp_evsel_new() 与自建实例的分析器共用（准则 8）。 */
int tp_bpf_scope(struct prof_dev *dev, struct perf_cpu_map *cpus, struct bpf_event_scope *scope, int *tgids);
```

| 位置 | BPF 分支 |
|---|---|
| `parse_event_head()` | `bpf:` 前缀且 `bpf_event_source_find(name)` 命中 → `EVT_HEAD_BPF`，`head_end` 是第一个 `/`；在 `monitor_find()` 之前判断（已确认 3） |
| `tp_list_new()` | `sys="bpf"`、`name`、`tp->bpf_src`；id 由 tep 的 id → 源表分配（同一源复用一个 id）；`push=` / `pull=` 报错 |
| `tep__event_fields(id)` / `tep__event_size(id)` | 查 tep 的表 → `bpf_event_source_fields/size()`；放在 `id > TRACE_EVENT_TYPE_MAX` 的 kprobe 分支之前 |
| `tp_evsel_new()` | 见 6.5：type/config 的 switch 对 BPF 跳过（否则落进 `default: return NULL`，tep.c:1831）；stack / dwarf / batch 照旧改 attr；`perf_evsel__new(attr)`（tep.c:1855）换成 `tp_bpf_scope()` + `bpf_event_new()`；收尾共用，但跳过 `tp->cpus` 的 `perf_evsel__set_own_cpus()`（tep.c:1875，`cpus=` 已并进 scope）。要求 `PERF_SAMPLE_ID/IDENTIFIER`（同 kprobe） |
| `prof_dev_open`（monitor.c 2815 与 2835 之间） | 调公共函数 `prof_dev_bpf_start(dev)`，失败走 `out_close`；已启动的实例由 `prof->deinit → tp_list_free → bpf_event_free` 释放，不另加 stop。reinit 只发生在 open 失败时，那时尚未启动。子设备各自在自己的 `prof_dev_open` 里启动 |
| `tp_bpf_start()` | 有 `prof->ftrace_filter` 时先在用户态编译 `tp->filter`（错误的表达式只在这里报一次）；`bpf_event_start(tp->bpf, tp->filter, prog ? USERSPACE_OK : 0)`；返回 1 → 用已编译的 prog 作 `tp->ftrace_filter`、`dev->ftrace_filter = 1`、打印 "filters in userspace"（与 ftrace 回退共用 `tp_filter_userspace()`） |
| `tp_list_apply_filter()` | 跳过 BPF tp |
| `tp_list_ftrace_filter()` | 表里有 BPF tp（`tp_list->nr_bpf`）时整个跳过 `common_type` 匹配，一律按 evsel 找 tp（拿到 tp 之前无法知道这条记录是不是 BPF 的） |
| `tp_list_free()` | `bpf_event_free(tp->bpf)`；BPF tp 跳过 `tp->cpus` 时清 own_cpus 的那段（tep.c:1535） |
| `prof_dev_replace_filter()` | 经 `perf_evsel_tp()` + `tp_is_bpf()` 判断，BPF evsel 返回 `-EINVAL` |
| `tp_evsel_new()` 的提示 | `--exclude-*`（含 `-G` 即 `--exclude-host`）对 BPF 事件不生效（K12），打印一次提示 |
| `get_tgid()` | `tp_bpf_scope()` 用；从 bpf_kvm_exit.c 挪到 monitor.c 作公共函数 |
| task-state / rundelay | 不需要拒绝：task-state 不接受 `-e`；rundelay 只跟随 sched 的 tracepoint（按 id 比较），不会跟随 BPF tp，重算 `dev->ftrace_filter` 时也计入 `tp->ftrace_filter`，不会覆盖 BPF 回退置上的 1 |
| python / sql | 第 1 期 `tp_list->nr_bpf` 非 0 时报 "BPF events are not supported yet" |
| `tp_print_event()` | BPF 分支：pid 用 sample 的 tid（Q4）；不调 `trace_print_lat_fmt()`（raw 里没有 flags / preempt_count），以 `....` 表示为空并保持列对齐；名字后不换行；字段用 `bpf_event_source_print()` |
| `tp_matcher_find()` 默认 matcher | 改为用 `tep__event_fields(tp->id)` 判断有没有 `cpu` / `pid` 字段，tracepoint 与 BPF 事件一致 |
| `order.c` | `type == SOFTWARE && config == BPF_OUTPUT` 时取 `perf_evsel_tp(evsel)`，判空（`bpf:kvm_exit` 分析器自建的 evsel 没有 tp），有 tp 才用 `tep__event_size(tp->id)` |
| `help.c` | `tp_is_bpf(tp)` → `bpf_event_source_help(tp->bpf_src)` |
| `list.c` | `print_tracepoint_events()` 改名 `list_events()`（typedef `list_events_cb`），在函数内部以 `bpf:name` 并入 BPF 源，三个调用者（list、bash 补全、通配展开）都得到 BPF 事件；tracefs 打不开时也列出 BPF 事件（v4.11–v4.17 的 tracefs 里有真实的 `bpf` 子系统，K14：同名时以 BPF 源为准，列表去重） |
| `-e bpf:kvm_exit` | 语义改变：原来 `/opts/` 是分析器选项串、现在是 C 过滤表达式；原用法不再可达。`tests/test_probe_format.py:176` 改为测事件源 |

### 6.3 生命周期

```
构建期    xxx.bpf.c → xxx.bpf.o → xxx.skel.h → xxx.event.o（模板）
进程启动  constructor：bpf_event_register(name, skel)；BPF_EVENT_OPS：bpf_event_register_ops(name, ops)
首次查询  绑定 ops 到源；没有源的 ops 报错
-e 解析   tp_list_new：bpf_event_source_find() → tp->bpf_src、tp->id
          属性表达式：tep__event_fields(id) → bpf_event_source_fields()   [首次：open skeleton 读 BTF，缓存]
建 evsel  tp_evsel_new：tp_bpf_scope()；tp->bpf = bpf_event_new(src, scope, tmpl)
open      perf_evlist__open()：BPF_OUTPUT 每 cpu 一个 fd
启动      prof_dev_open → prof_dev_bpf_start(dev) → 每个 evsel：tp_bpf_start(perf_evsel_tp(evsel))
            → bpf_event_start(tp->bpf, tp->filter, flags)
            open → 注入 → prepare → load ─┬─ 成功 → 写 perf_events → attach → 0
                                          └─ 过滤器装不进 / verifier 拒绝，且 USERSPACE_OK
                                               → 销毁、不带过滤器 open → prepare → load → … → 1
            返回 1 → tp->ftrace_filter、dev->ftrace_filter = 1
prof->filter  （分析器的 .filter，照旧；tp_list_apply_filter 跳过 BPF tp）
运行      BPF 程序 → perf_output → 本 cpu 的 BPF_OUTPUT → ring → order → sample() → evsel → tp
释放      tp_list_free：bpf_event_free()
进程退出  destructor 释放源级缓存
help 模式 只到 "-e 解析"：不建实例、不 load、不需要 root
```

**初始值与第一次结果**：源的字段 / 大小在第一次查询时得到，不依赖实例；实例的 scope 在 new 时给定，过滤器在 start
时给定，start 的返回值就是第一次结果（在内核 / 在用户态 / 失败）。实例在运行期没有可变输入（F2 需要，见 6.6）。

### 6.4 状态表

| 状态 | 内部表示 | 含义 | 何时变化 | 谁查询 |
|---|---|---|---|---|
| 注册表 | 源链表（按名字排序） | 链接进来的全部源 | constructor 加入，永不删除 | find / next |
| 待绑定的 ops | 链表 | constructor 先于源注册时暂存 | 首次查询时绑定并清空 | 模块 |
| 源的解析结果 | 源内：fields、size、desc、btf、类型 id、错误标记 | 事件结构体的 BTF 描述 | 首次查询时解析；失败也缓存；destructor 释放 | fields / size / desc / help / print |
| 实例的 scope | 实例内副本（cpu map 取引用，tgid 列表复制） | 跟踪什么 | new 时 | evsel / prepare |
| 实例的 evsel | 实例内指针，不持有 | 输出绑定 | new 时 | start（读 fd） |
| 实例的 skeleton | 实例内 `void *` | 已 load、attach 的对象 | start 建立；free 销毁 | 模块；`bpf_event_skel()` |
| 过滤器是否在内核 | 不保存 | — | start 的返回值 | tep |
| id → 源 | tep 内表 | `tp->id` 的编码 | tp_list_new 首次遇到某源时追加 | tep__event_fields/size |

### 6.5 调用方伪代码

**tep：`tp_evsel_new()` 的 BPF 分支（只列改动）**

```c
    if (tp->id <= TRACE_EVENT_TYPE_MAX) {
        ...
    } else if (!tp_is_bpf(tp)) {            /* kprobe / uprobe 的 switch，BPF 跳过 */
        ...
    }
    ...stack / dwarf / batch 改 attr，照旧...

    if (tp_is_bpf(tp)) {
        struct bpf_event_scope scope;

        int tgids[perf_thread_map__nr(tp->dev->threads)];

        if (tp_bpf_scope(tp->dev, tp->cpus, &scope, tgids) < 0)
            return NULL;
        tp->bpf = bpf_event_new(tp->bpf_src, &scope, attr);
        evsel = tp->bpf ? bpf_event_evsel(tp->bpf) : NULL;
    } else
        evsel = perf_evsel__new(attr);
    if (!evsel)
        return NULL;
    ...收尾照旧；tp->cpus 的 set_own_cpus 对 BPF 跳过...
```

**tep：`tp_bpf_scope()`**

```c
/* @tgids 由调用者提供存储（dev->threads 的个数即上限），内容在 bpf_event_new() 里被复制。 */
int tp_bpf_scope(struct prof_dev *dev, struct perf_cpu_map *cpus, struct bpf_event_scope *scope, int *tgids)
{
    if (dev->env->cgroups) {
        fprintf(stderr, "bpf events: --cgroups is not supported\n");
        return -EOPNOTSUPP;
    }
    if (prof_dev_oncpu(dev)) {
        *scope = (struct bpf_event_scope){ BPF_EVENT_SCOPE_CPU, .cpus = cpus ?: dev->cpus };
        return 0;
    }
    /* -p 给的是进程，-t 的线程换算成所属进程；去重 */
    n = 0;
    perf_thread_map__for_each_thread(tid, idx, dev->threads)
        add_unique(tgids, &n, get_tgid(tid));
    *scope = (struct bpf_event_scope){ BPF_EVENT_SCOPE_PROCESS, .tgids = tgids, .nr_tgids = n };
    return 0;
}
```

**tep：`tp_bpf_start()`**

```c
int tp_bpf_start(struct tp *tp)
{
    struct prof_dev *dev;
    int err;

    if (!tp || !tp_is_bpf(tp))
        return 0;
    dev = tp->dev;
    err = bpf_event_start(tp->bpf, tp->filter,
                          dev->prof->ftrace_filter ? BPF_EVENT_USERSPACE_OK : 0, dev->env->verbose);
    if (err == 1)
        err = tp_filter_userspace(dev, tp);     /* 与 tp_list_apply_filter() 的回退共用 */
    return err;
}
```

**monitor.c：公共函数 `prof_dev_bpf_start()`，evlist 打开之后调用**

```c
static int prof_dev_bpf_start(struct prof_dev *dev)
{
    struct perf_evsel *evsel;

    perf_evlist__for_each_evsel(dev->evlist, evsel) {
        /* 分析器自建、没有 tp 的 evsel：perf_evsel_tp() 为 NULL，tp_bpf_start() 返回 0 */
        if (tp_bpf_start(perf_evsel_tp(evsel)) < 0)
            return -1;
    }
    return 0;
}

    ...
    err = perf_evlist__open(evlist);
    ...
    if (prof_dev_bpf_start(dev) < 0)
        goto out_close;
    if (prof->filter && prof->filter(dev) < 0)
    ...
```

**kvm_exit 源的 ops（`src/bpf_kvm_exit.c`）**：分析器 init 也调用同一个 `kvm_exit_prepare()`，cpu / 线程模式的设置
只有一份（准则 8）：

```c
int kvm_exit_prepare(void *s, const struct bpf_event_scope *scope)
{
    struct kvm_exit_bpf *obj = s;

    if (scope->type == BPF_EVENT_SCOPE_CPU) {
        perf_cpu_map__for_each_cpu(cpu, i, scope->cpus)
            obj->bss->work_cpus[cpu] = 1;
        ...percpu_event 初始化，关掉 *_pid 程序...
    } else {
        if (scope->nr_tgids > 1)
            return -EOPNOTSUPP;                    /* 多进程：等 tgid map 约定（7 节） */
        obj->rodata->filter_pid = scope->tgids[0];
        ...关掉 cpu 模式的程序...
    }
    return kvm_exit_select_prog(obj);
}
static const struct bpf_event_ops kvm_exit_ops = { .prepare = kvm_exit_prepare };
BPF_EVENT_OPS(kvm_exit, &kvm_exit_ops);

/* 分析器 init：
 *   tp_bpf_scope(dev, NULL, &scope, tgids);
 *   kvm_exit_prepare(ctx->obj, &scope);
 */
```

`work_cpus` 不下沉为模块约定：未绑定 cpu 上的输出已被内核丢弃（K3），`work_cpus` 只是 kvm_exit 省掉其它 cpu
上配对开销的手段，属于源自己的策略（准则 9）。

**list.c**

```c
for (src = bpf_event_source_next(NULL); src; src = bpf_event_source_next(src))
    add_event(list, "bpf", bpf_event_source_name(src));
```

**分析器：不变。** `tp = perf_evsel_tp(evsel)`；字段由 `tp_prog_run()` 按 BTF 偏移读 raw。

### 6.6 未来使用者

**F1 python / sql（第 2 期）**：字段来源从 `tep_find_event(tp->id)` 换成 `tep__event_fields(tp->id)`。
`bpf_event` 接口不变，但 **`struct global_var_declare` 要扩展**：python / sql 需要区分 char 数组按字符串还是
按 bytes（tracepoint 靠 `tep__string_field_as_binary()`），BTF 里有 char 与 u8 的区别，可加一位 `is_string`。
BPF 事件不提供 `common_*`。

**F2 跟踪多个进程（`-p 1,2,3`，已确认 11）**：scope 已经给出 tgid 列表，接口不变。预留的约定：

```c
/* bpf_event.bpf.h，源可选地使用 */
struct { __uint(type, BPF_MAP_TYPE_HASH); __type(key, u32); __type(value, u8);
         __uint(max_entries, 1); } bpf_event_tgids SEC(".maps");      /* 模块在 load 前按 nr_tgids 设 max_entries */
static __always_inline bool bpf_event_tgid_match(void);              /* 查 bpf_get_current_pid_tgid() >> 32 */
```

模块在 start 时发现源有 `bpf_event_tgids` map，就在 load 之后、attach 之前写入 scope 的 tgid；没有这个 map 的源仍由
prepare 自己处理（单进程用 rodata，多进程返回 `-EOPNOTSUPP`）。不使用 `pid_filter`：集合在启动时确定，不跟随新进程。
pid_filter 不会作用到 BPF tp：task-state 不接受 `-e`，rundelay 只跟随 sched 的 tracepoint（6.2）。

**F3 `perf-prof bpf:kvm_exit` 基于事件源（第 3 期）**：届时分析器不再自己 open skeleton，`kvm_exit.skel.h`
只被 `kvm_exit.event.o` 包含（在此之前嵌入的 ELF 在二进制里有两份）。init 里 `tp_list_new(dev, "bpf:kvm_exit/<filter>/")` +
`tp_evsel_new()`，open / load / 填 map / attach 由框架完成；`print_dev` 遍历 `kvm_vcpu` map 用
`bpf_event_skel(tp->bpf)`。线程模式的进程退出检测（`sched:sched_process_exit` 子设备）留在分析器（见风险 2）。
源的更多配置参数都在源自己的 prepare 里设置（已确认 10）；参数来自分析器选项时，由分析器在调用 prepare 前存进源的 .c 里。
进程 scope 按 tgid 跟踪，`-t tid` 实际跟踪 tid 所属的整个进程，文档写明。

**F4 BPF 源带 tp_matcher**：`TP_MATCHER_REGISTER("bpf", "xxx", refer_cpu, refer_tid)`；没有注册的，
带 `cpu` / `pid` 字段的 BPF 事件得到默认 matcher（6.2 改了 `tp_matcher_find()`），与 tracepoint 一致。

## 7 扩展点与非目标

**扩展点**

| 扩展 | 从哪层接入 |
|---|---|
| 多进程 scope（F2） | `bpf_event.bpf.h` 的 `bpf_event_tgids` map 约定，模块 start 时填写；接口不变 |
| 运行期增减进程 | 若以后需要，增加 `bpf_event_set_tgids()` 更新同一个 map |
| cpu 动态增减（跟随集合） | 模块已持有 evsel：增加 `bpf_event_update_outputs(ev)`，tep 在补开 fd 后调用 |
| python / sql | F1 |
| cgroup scope | `BPF_EVENT_SCOPE_CGROUP`，由 prepare 用 `bpf_current_task_under_cgroup` 之类实现 |

**非目标**

- `push=` / `pull=`：接收端按 tep id 匹配，BPF 源的 id 不跨进程稳定。解析时报错。
- `prof_dev_replace_filter()` 作用于 BPF evsel：返回 `-EINVAL`。
- 同一源写两次合并成一个对象：两个实例，功能正确，开销翻倍。
- 变长事件；一个 `.bpf.c` 多种事件结构体。
- `mem_ptr` / `mem_size` 按字段名（`ptr`、`bytes_alloc`）自动识别：只对 tracepoint；BPF 事件显式写 `ptr=` / `size=`。
- 第 1 期 python / sql：初始化遇到 BPF tp 报 "not supported yet"。
- 一个打开的 BPF 对象 load 多次 / 多个实例共用：libbpf 的 `bpf_object_load()` 在 `obj->loaded` 时拒绝（"load can't be
  attempted twice"），过滤器在 load 前改写对象里 `expr_filter` 的指令，prepare 改对象的 rodata，都是每个实例各不相同。
  源里缓存的对象只 open 不 load，只读 BTF 与描述；每个实例在 start 时自己 open。
- 第 1 期 `--cgroups`：报 `-EOPNOTSUPP`。
- cpu 热插拔：BPF evsel 自带 cpus，libperf 的跟随集合不碰它（`perf_evlist__cpu_evsel()`，evlist.c:1957）；进程 scope
  下新上线的 cpu 没有输出，事件被内核丢弃（K3）。`--exclude-*` 对 BPF 事件不生效，打印一次提示。

## 8 风险

1. **进程 scope 与按线程打开的 evsel 混在一个 evlist**（`trace -p 1 -e sched:x,bpf:y`）：BPF evsel 自带全部 cpu、
   dummy 线程，依赖 libperf 的 cpu domain 与 task domain 共存。用 pytest 覆盖。
2. **进程 scope 下只有 BPF 事件时，目标进程退出检测不到**：BPF evsel 在 cpu domain，不报 EPOLLHUP，prof_dev 不关闭
   （现有分析器靠 `sched:sched_process_exit` 子设备）。第 1 期不处理，文档写明（已确认 9）。
3. **multi-trace 默认 `key=common_pid`**：BPF 事件没有它，报 "undefined"。文档写明 BPF 事件须显式 `key=`。
4. **`/filter/` 语法**：BPF 事件是 C 表达式（`docs/expr.md`），tracepoint 是 ftrace 语法；`/` 是属性分隔符，
   除法要写在括号或引号里。文档写明。
5. **raw_tp 的调用栈**：`/stack/` 的栈从 BPF helper 起（K6），顶部有几帧内部函数。
6. **BPF 嵌套超过 3 层丢事件**（K15）：在 nmi 里触发的 BPF 程序可能碰到，不处理，文档写明。
7. **attach 早于 enable**：`prof_dev_bpf_start()` 在 open 之后就 attach，evsel 要到 `prof_dev_enable` 才使能；其间（含父设备
   INACTIVE 的子设备）BPF 程序照常运行、输出全部被丢弃（K3），白耗 CPU；有状态的源（kvm_exit 的配对）在这段时间积累状态。
   以后可把 attach 挪到框架 enable 时（扩展点，只改框架的公共函数）。
8. **进程 scope 的内存**：BPF_OUTPUT 按 cpu 打开，不能把输出重定向到按线程打开的 ring（K16），每个 cpu 自建 ring，
   开销 nr_cpus × pages。

## 9 依赖的内核约束

均已在最新内核（v7.3-rc1，`/root/linux`）源码上核实，旧版本行为在对应 tag 上核实（第 1 轮审查复查了 K1–K7）。
K16 在 v3.10 与 v7.3-rc1 上核实。实现时写 `Kernel constraint:` 注释，并在 `docs/kernel_dependencies.md` 第 4 节新增 "BPF 事件源" 小节。

| # | 约束 | 内核出处 | 引入 | 不成立时 |
|---|---|---|---|---|
| K1 | `PERF_COUNT_SW_BPF_OUTPUT` 与 `bpf_perf_event_output()` | `kernel/trace/bpf_trace.c` `__bpf_perf_event_output()` | a43eec304259 ("bpf: introduce bpf_perf_event_output() helper"), in v4.4 | 打开 evsel 失败 |
| K2 | `BPF_F_CURRENT_CPU` 取当前 cpu 的 map 项 | 同上 | 1e33759c788c ("bpf, trace: add BPF_F_CURRENT_CPU flag for bpf_perf_event_output"), in v4.7 | v4.4–v4.6 下标越界 `-E2BIG`，静默丢失；被 K9 覆盖 |
| K3 | 当前 cpu 的项为空 `-ENOENT`；`event->oncpu != cpu`（含 disabled）`-EOPNOTSUPP`。BPF_OUTPUT 必须按 cpu 打开，未绑定 cpu 上的事件被丢弃 | 同上 | 已在 a43eec304259, v4.4 | 依赖它实现 cpu scope 与 `cpus=`；enable 之前的输出被丢弃 |
| K4 | 写进 `PERF_EVENT_ARRAY` 的 perf event 不能 `inherit` | `kernel/bpf/arraymap.c` `perf_event_fd_array_get_ptr()` | 62544ce8e01c ("bpf: fix bpf_perf_event_read() helper"), in v4.4；f91840a32dee ("perf, bpf: Add BPF support to all perf_event types"), in v4.13 起由 `perf_event_read_local()` 返回 `-EOPNOTSUPP` | `bpf_event_evsel()` 强制 `inherit=0` |
| K5 | map 项持有 perf event 文件的引用；map 文件 release 时删除它写入的项 | `perf_event_fd_array_release()` | 3b1efb196eee ("bpf, maps: flush own entries on perf map release"), in v4.8；之前在 map 释放时删除 | free 与关闭 evsel fd 的先后无关 |
| K6 | sample 头按 BPF_OUTPUT event 的 sample_type 从运行 BPF 程序的上下文取，时间用 `perf_event_clock()`，与 tracepoint 同一时钟；raw_tp 的 regs 由 `perf_fetch_caller_regs()` 取 | `kernel/events/core.c` `perf_prepare_sample()`；`bpf_perf_event_output_raw_tp()` | 已在 a43eec304259, v4.4；raw_tp：c4f6699dfcb8 ("bpf: introduce BPF_RAW_TRACEPOINT"), in v4.17 | 依赖它不要求 `trace_entry` 头（已确认 1），并让 multi-trace 混配 |
| K7 | raw 由内核补齐到 `round_up(size + 4, 8) - 4` | `perf_prepare_sample()` / `perf_sample_save_raw_data()` | fa128e6a148a ("perf: pad raw data samples automatically"), in v4.4；7e3f977edd0b ("perf, events: add non-linear data support for raw records"), in v4.8 | order 按结构体大小估算记录长度 |
| K9 | 第 5 节的约定要求：bpf-to-bpf 调用（`expr_filter` 是 subprog）cc8b0b92a169 ("bpf: introduce function calls (function boundaries)"), v4.16；helper 参数指向 map value 5722569bb9c3 ("bpf: allow helpers access to map element values"), v4.11；全局数据 `.bss` / `.rodata` d8eca5bbb2be ("bpf: implement lookup-free direct value access for maps"), v5.2；CO-RE 需要内核 BTF，`/sys/kernel/btf/vmlinux` 341dfcf8d78e ("btf: expose BTF info through sysfs"), v5.4 | verifier、libbpf | 见左 | 模块本身的门槛是 v5.2（全局数据 + subprog）；用了 CO-RE 的源（如 kvm_exit 的 `BPF_CORE_READ`）还要内核 BTF，libbpf 也会在 `/boot/vmlinux-*` 等处找，所以不按 `/sys/kernel/btf/vmlinux` 判断。load 失败时提示 "may need a newer kernel or kernel BTF"，不只转述 verifier 日志 |
| K10 | BPF 输出的样本 period 恒为 0，event 的计数不增加：`bpf_perf_event_output()` 用 `perf_sample_data_init(sd, 0, 0)`，`perf_event_output()` 不累加 count | `kernel/trace/bpf_trace.c` | 已在 a43eec304259, v4.4 | `bpf_event_evsel()` 拒绝 `sample_period == 0`（hrcount / hrtimer 的计数模式），否则读数恒为 0；`PERF_SAMPLE_PERIOD` 为 0 |
| K11 | `tp/` 程序（`BPF_PROG_TYPE_TRACEPOINT`）返回 0 时该 tracepoint 不再交付给 perf 的其它使用者 | `perf_trace_run_bpf_submit()` in `kernel/events/core.c`：`!trace_call_bpf() → 不提交` | 98b5c2c65c29 ("perf, bpf: allow bpf programs attach to tracepoints"), in v4.7 | 约定第 4 条：必须返回 1 |
| K12 | BPF 程序在 hook 上对所有任务、所有 cpu 运行，不受 perf event 的 pid / cgroup / exclude_* 约束 | BPF 挂接机制 | 已在 v4.4 | 进程 scope 的 pid 过滤由 prepare 实现；`--cgroups` 拒绝；`--exclude-*`（含 `-G`）提示不生效 |
| K13 | libbpf 跳过不认识的 ELF 数据段，并以 INFO 级别打日志（默认打印到 stderr；用户态，libbpf 1.3） | `lib/bpf/libbpf.c` `bpf_object__elf_collect()` | — | 描述不放单独的段，改放 `.rodata` 变量（第 5 节第 7 条） |
| K14 | tracefs 在 v4.11–v4.17 有真实的 `bpf` 子系统 | `include/trace/events/bpf.h` | a67edbf4fb6d ("bpf: add initial bpf tracepoints"), v4.11；4d220ed0f814 ("bpf: remove tracepoints from bpf core"), v4.18 | 同名以 BPF 源为准；`bpf:*` 通配只展开 tracefs，文档写明 |
| K16 | `PERF_EVENT_IOC_SET_OUTPUT` 不能跨 cpu：`output_event->cpu != event->cpu` 时拒绝 | `kernel/events/core.c` `perf_event_set_output()` | 已在 v3.10 | 进程 scope 下 BPF_OUTPUT 只能每 cpu 自建 ring（风险 8），依赖 libperf 让 cpu domain 与 task domain 共存（风险 1） |
| K17 | `BPF_F_CURRENT_CPU` 用 `smp_processor_id()` 作 key；`perf_events` 没设 `max_entries` 时 libbpf 按 possible cpu 数补上 | `__bpf_perf_event_output()`；libbpf `bpf_object__create_map()` | K2 | 模块按 cpu 号写 map；cpu 不连续时用下标会写错位 |
| K15 | `bpf_perf_event_output()` 每 cpu 嵌套最多 3 层，超出 `-EBUSY` 丢事件 | `bpf_trace_sds` | 9594dc3c7e71 ("bpf: fix nested bpf tracepoints with per-cpu data"), v5.2 | 风险 6 |


## 10 分期

1. **框架**（本稿）：`bpf_event` 模块、模板与构建规则、tep / monitor 适配、list / help / order / print / 默认 matcher；
   kvm_exit 源（cpu 与单进程 scope，经共用的 prepare；线程模式改为全长输出）；演示源 sched_switch。测试 `tests/test_bpf_event.py`，
   `tests/test_probe_format.py` 的 `bpf:kvm_exit` 用例改为测事件源。
2. **补齐**：python / sql（F1，扩展 `global_var_declare`）；多进程 tgid map（F2）。
3. **收敛**：`perf-prof bpf:kvm_exit` 基于事件源（F3）；cpu 动态跟随。

## 11 提交拆分（第 1 期）

按 CLAUDE.md「提交拆分」，源码拆成可各自编译、行为正确的小提交：

1. `tp_print_event()` 加 `tid` 参数（7 个调用点），行为不变。
2. 构建：`%.event.o` 规则、`bpf_event.tmpl.c`、skel-target 进 `targets` / clean；`bpf_event` 模块的注册表与源级接口
   （fields / size / desc / help / print），kvm_exit 改由 `kvm_exit.event.o` 引入 skeleton。
3. `bpf_event` 实例接口（new / start / skel / free）。
4. tep / monitor（`prof_dev_bpf_start()`）/ order / help / list（`list_events()`）接入 `bpf:xxx`；python、sql、`prof_dev_replace_filter` 拒绝 BPF。
5. kvm_exit 源：共用的 `kvm_exit_prepare()`，`ops->print` 按 `bpf_kvm_exit_print_event()` 的格式打印，线程模式全长输出。
6. 演示源 sched_rundelay（`sched_rundelay.bpf.c` + `sched_rundelay.h` + 用户态 ops `src/bpf_sched_rundelay.c`：单进程 scope）。
7. 测试：`tests/test_bpf_event.py`，`test_probe_format.py` 的 `bpf:kvm_exit` 用例。
8. 文档：main_options / tool_usage / README / skills / modules.md / kernel_dependencies.md / bpf_event_filter.md。

## 12 审查记录

**第 1 轮（独立审查员，2026-11-24）** 违反项与处理：

| 准则 | 问题 | 处理 |
|---|---|---|
| 4 | 启动挂在 `tp_list_apply_filter()`，没有 `.filter` 的分析器（如内部 `tracepoint`，trace.c:448）收不到事件 | 改由框架在 evlist 打开后调用 `tp_bpf_start()` |
| 6 | `cpus` 同时表示输出绑定和跟踪范围；`tids==NULL` 兼表模式 | 拆出 `bpf_event_scope`（显式 type）；输出绑定由模块决定 |
| 8 | F2 / tep 两份 conf 拼装；kvm_exit 的模式设置在 prepare 与分析器各一份；`evsel_cpu_fds` 会重复 | 采纳边界 B：模块建 evsel、自己读 fd；分析器调用同一个 prepare；`work_cpus` 不下沉（理由见 6.5） |
| 11 | `bpf_event_source_index/at` 只为 tep 的 id 编码存在 | 删除；id → 源的表归 tep |
| 2（附注） | 弱符号名拼错静默变成 NULL | 改为 `BPF_EVENT_OPS()` 宏，首次查询时绑定，找不到源报错 |
| F2 | "接口不变"不成立 | 如实标为需扩展接口（6.6） |
| F3 | 拿不到 skeleton | 增加 `bpf_event_skel()` |
| 内核 | 最低版本写错；漏 period=0、嵌套上限、作用域不生效、tp/ 返回值、tracefs bpf 子系统 | K9–K15 |
| 集成 | 截断输出、`tp_print_event` 三处、cgroup、桩、verifier 拒绝后回退、ftrace_filter 整体跳过、order 判空、语义变化与测试、构建规则位置 / 命名 / 宏、位域静默跳过、默认 matcher | 分别写进第 5、6.1、6.2、7、10 节 |

审查员推荐的方案 C（源 / 实例拆成两个模块，并预留运行期 key 集合）：本稿未拆。源级部分本来就不 load、只读 BTF，
与实例的依赖是单向的，放在一个文件里不影响边界；F2 的运行期集合作为接口扩展列出。若审查员仍认为应拆，列为分歧交给人。

**第 2 轮（另一个独立审查员，2026-11-24）**：第 1 轮的处理属实。剩余：

| 准则 / 类别 | 问题 | 处理 |
|---|---|---|
| 5 | new → evsel 是多步协议（evsel 只能调一次、必须先于 start） | 合并进 `bpf_event_new(src, scope, tmpl)`，`bpf_event_evsel()` 只取值；evsel 未打开时 start 返回 `-EINVAL` |
| 8 | 从 dev 推导 scope 有两份（tep、kvm_exit 分析器） | 新增 `tp_bpf_scope()`，两处共用 |
| 集成 | `tp_bpf_start(NULL)` 解引用；`tp_evsel_new` 分支会落进 switch 的 default、`tp->cpus` 覆盖 own_cpus；`tp_list_free` 清 own_cpus | 6.2 / 6.5 写明落点 |
| 集成 | pid_filter 跟随 BPF tp 时 `prof_dev_replace_filter()` 持续失败；task-state 覆盖 `dev->ftrace_filter` | 第 1 期 task-state / rundelay 拒绝 BPF tp；`-EINVAL` 判断放在 `prof_dev_replace_filter()` |
| 内核 | 漏 set_output 不能跨 cpu、map key 是 cpu 号；K9 门槛与提示自相矛盾；attach 早于 enable 的后果 | K16、K17；改写 K9；风险 7、8 |
| 集成 | 桩缺 `desc`，缺 `errno.h` / `stringify.h` | 已补 |
| 构建 | skel-target 不在 `obj-y`：不进 `targets` 与 clean，skeleton 每次重新生成 | 写进第 5 节 |
| 其它 | cpu 热插拔不跟随；ops 延迟绑定只在查询注册表时报错；ELF 嵌两份到第 3 期 | 非目标 / 6.1 说明 / F3 |

方案 C（拆成两个模块）：第 2 轮审查员认为是偏好而非违反，同意不拆，不列为分歧。

**用户确认后的修改**：第 5 版曾改为框架经 evsel 私有数据直接驱动 `bpf_event`（不经 tp），被用户撤回——`tp->filter`
就是 BPF 事件的 `/filter/`，实例应挂在 tp 上。第 6 版回到第 4 版的 evsel → tp → bpf，只保留"框架用公共函数
`prof_dev_bpf_start()` 启动"（已确认 12）。

## 13 实现与本稿的差异

| 位置 | 本稿原写法 | 实现 | 原因 |
|---|---|---|---|
| 描述 `BPF_EVENT_DESC()` | 可选；放 ELF 段 `perf_prof_desc`，libelf 读 | 必须；`.rodata` 变量，模板经 skeleton 成员取（`bpf_event_skel.desc`） | libbpf 对不认识的段打 INFO 日志；放进 skeleton 后缺失即编译失败（用户 review） |
| `perf_events` map | 第一次解析时按名字查，缺失报错 | 模板经 skeleton 成员取（`bpf_event_skel.perf_events`），缺失编译失败 | 编译期判断合规，不必运行期查（用户 review） |
| 模板 | `-include xxx.skel.h` | 模板 `#include` 源的类型头 `xxx.h`（若有）再 include skeleton | 旧 bpftool 的 skeleton 只引用 `.bss` 里的类型，不定义 |
| list.c | `list.c` 自己遍历源并入列表 | 并进 `list_events()`（原 `print_tracepoint_events()`），补全与通配也得到 BPF 事件；`bpf:*` 展开包括 BPF 源 | 该函数另有 2 个调用者（用户 review） |
| task-state / rundelay | 第 1 期拒绝 BPF tp | 不拒绝 | task-state 不接受 `-e`；rundelay 不跟随 BPF tp，且计入 `tp->ftrace_filter` |
| `tp_bpf_start()` | 回退时才编译用户态过滤器 | 先在用户态编译，BPF 后端失败时复用 | 错误表达式否则报两次 |
| `-G` | 当作 cgroup 拒绝 | `--cgroups` 拒绝；`-G` 是 `--exclude-host`，与 `--exclude-*` 一起提示不生效 | 选项名核对 |
| 计数模式 | — | `tp_evsel_new()` 的 PERF_SAMPLE_ID 检查只在 `sample_period != 0` 时做，让 `bpf_event_new()` 报 "can only be sampled" | 报错信息准确 |
| `get_tgid()` | `tp_bpf_scope()` 伪代码直接用 | 从 bpf_kvm_exit.c 挪到 monitor.c 作公共函数 | 避免第三份副本（准则 8） |
| 演示源 | `sched_switch`，事件即一次切换 | `sched_rundelay`：一次调度延迟一条事件，内核态配对 | 用户要求做成真实可用的事件 |
| kvm_exit 源 | 只有 prepare | 加 `ops->print`，格式同 `bpf_kvm_exit_print_event()`（退出原因按名字） | 用户要求 |
| `tp_print_event()` 占位 | 4 个空格 | `....`（用户 review） | 表示该列为空 |
| 多进程 | kvm_exit 分析器只取第一个线程的 tgid | 分析器与事件源共用 prepare，多个进程一并拒绝 | 准则 8，一份配置 |

