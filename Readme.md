# 基于perf的监控框架

基于`libperf`和`libtraceevent`库实现简单的监控框架，提供比perf更灵活的特性。

- 数据不落盘。
- 数据过滤，基于tracepoint的过滤机制，减少数据量。
- 数据实时处理并输出。不需要存盘后再处理。
- 基于perf_event_open系统调用。

虽然比perf更灵活，但不能替代perf。perf灵活的符号处理，支持大量的event，支持很多硬件PMU特性。

## 1 框架介绍

```
# ./perf-monitor  --help
Usage: perf-monitor [OPTION...]
Monitor based on perf_event

USAGE:
    perf-monitor split-lock [-T trigger] [-C cpu] [-G] [-i INT] [--test]
    perf-monitor irq-off [-L lat] [-C cpu] [-g] [--precise]
    perf-monitor profile [-F freq] [-C cpu] [-g]
    perf-monitor trace -e event [-C cpu]
    perf-monitor signal [--filter comm] [-C cpu] [-g]
    perf-monitor task-state [-S] [-D] [--than ms] [--filter comm] [-C cpu] [-g]
    perf-monitor watchdog [-F freq] [-g] [-C cpu] [-v]

EXAMPLES:
    perf-monitor split-lock -T 1000 -C 1-21,25-46 -g  # Monitor split-lock
    perf-monitor irq-off -L 10000 -C 1-21,25-46  # Monitor irq-off

  -C, --cpu=CPU              Monitor the specified CPU, Dflt: all cpu
  -D, --uninterruptible      TASK_UNINTERRUPTIBLE
  -e, --event=event          event selector. use 'perf list tracepoint' to list
                             available tp events
      --filter=filter        event filter/comm filter
  -F, --freq=n               profile at this frequency, Dflt: 10
  -g, --call-graph           Enable call-graph recording
  -G, --guest                Monitor GUEST, Dflt: false
  -i, --interval=INT         Interval, ms
  -L, --latency=LAT          Interrupt off latency, unit: us, Dflt: 20ms
      --precise              Generate precise interrupt
  -S, --interruptible        TASK_INTERRUPTIBLE
      --test                 Test verification
      --than=ms              Greater than specified time, ms
  -T, --trigger=T            Trigger Threshold, Dflt: 1000
  -v, --verbose              Verbose debug output
  -?, --help                 Give this help list
      --usage                Give a short usage message

Mandatory or optional arguments to long options are also mandatory or optional
for any corresponding short options.
```

监控框架采样模块化设计，目前支持一些基础的监控模块：

- split-lock，监控硬件pmu，发生split-lock的次数，以及触发情况。
- irq-off，监控中断关闭的情况。
- trace，读取某个tracepoint事件。
- signal，监控给特定进程发送的信号。
- task-state，监控进程处于D、S状态的时间，超过指定时间可以打印栈。
- watchdog，监控hard、soft lockup的情况，在将要发生时，预先打印出内核栈。
- 可行的扩展：监控kvm-exit，采样内存分配释放来判定内存泄露等。

每个监控模块都需要定义一个`struct monitor `结构，来指定如何初始化、过滤、释放监控事件，以及如何处理采样到的监控事件。

## 2 Example: signal

一个最简单demo例子。

```
struct monitor monitor_signal = {
    .name = "signal",
    .pages = 2,
    .init = signal_init,
    .filter = signal_filter,
    .deinit = signal_exit,
    .sample = signal_sample,
};
MONITOR_REGISTER(monitor_signal)
```

定义模块初始化、过滤、销毁、处理采样接口。

## 3 monitor.init

```
static int signal_init(struct perf_evlist *evlist, struct env *env)
{
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW |
                         (env->callchain ? PERF_SAMPLE_CALLCHAIN : 0),
        .read_format   = 0,
        .pinned        = 1,
        .disabled      = 1,
        .exclude_callchain_user = 1,
        .wakeup_events = 1, //1个事件
    };
    struct perf_evsel *evsel;
    int id;

    if (monitor_ctx_init(env) < 0)
        return -1;

    id = tep__event_id("signal", "signal_generate");
    if (id < 0)
        return -1;

    attr.config = id;
    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        return -1;
    }
    perf_evlist__add(evlist, evsel);
    return 0;
}
```

定义perf_event_attr表示监控的事件。

tep__event_id("signal", "signal_generate")，获取signal:signal_generate tracepoint点的id。

perf_evsel__new(&attr)，根据perf事件，创建evsel。1个evsel表示一个特点的事件，拿着这个事件可以到对应的cpu、线程上创建出perf_event。

perf_evlist__add(evlist, evsel)，加到evlist。一个evlist表示一组evsel事件。

### 3.1 perf_event_attr

定义event的属性。可以指定perf命令定义的所有事件。

- 硬件pmu事件
  - breakpoint事件
  - cpu事件
  - uncore事件
- tracepoint点事件
- kprobe事件
- uprobe事件

可以通过`ls /sys/bus/event_source/devices`命令看到所有的事件类型。

- **perf_event_attr.type** 事件类型

  ```
  PERF_TYPE_*
  	通过`cat /sys/bus/event_source/devices/*/type`获取类型。
  ```

- **perf_event_attr.config** 事件配置

  ```
  根据不同的type, config值不一样。
  	PERF_TYPE_TRACEPOINT: config 指定tracepoint点的id.
  	PERF_TYPE_HARDWARE: config 指定特定的参数PERF_COUNT_HW_*
  ```

- **perf_event_attr.sample_period** 采样周期

  ```
  定义采样周期, 发生多少次事件之后, 发起1个event到ring buffer
  ```

- **perf_event_attr.sample_type** 采样类型

  ```
  PERF_SAMPLE_*
  	定义放到ring buffer的事件, 需要哪些字段
  ```

- **perf_event_attr.comm**

  ```
  PERF_RECORD_COMM
  	记录进程comm和pid/tid的对应关系, 可以用于libtraceevent模块中tep_register_comm,
  	之后tep_print_event(ctx.tep, &s, &record, "%s", TEP_PRINT_COMM)就能打印出进程名
  	这样只能收集新创建进程的名字, 已启动进程的pid使用/proc/pid/comm来获取.
  ```

- **perf_event_attr.task**

  ```
  PERF_RECORD_FORK/PERF_RECORD_EXIT
  	记录进程创建和退出事件
  ```

- **perf_event_attr.context_switch**

  ```
  PERF_RECORD_SWITCH/PERF_RECORD_SWITCH_CPU_WIDE
  	记录进程切换信息
  ```

## 4 monitor.sample

```
static void signal_sample(union perf_event *event)
{
    // in linux/perf_event.h
    // PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_RAW
    struct sample_type_data {
        struct {
            __u32    pid;
            __u32    tid;
        }    tid_entry;
        __u64   time;
        struct {
            __u32    cpu;
            __u32    reserved;
        }    cpu_entry;
        struct {
            __u32   size;
	        __u8    data[0];
        } raw;
    } *data = (void *)event->sample.array;

    tep__update_comm(NULL, data->tid_entry.tid);
    print_time(stdout);
    tep__print_event(data->time/1000, data->cpu_entry.cpu, data->raw.data, data->raw.size);
}
```

根据`perf_event_attr.sample_type`来定义采样的事件的字段，可以还原出一个结构体。

tep__print_event，打印tracepoint事件。

## 5 插件

```
TRACEEVENT_PLUGIN_DIR
	export TRACEEVENT_PLUGIN_DIR=$(pwd)/lib/traceevent/plugins
	可以加载libtraceevent的插件
```



## 6 已支持的模块

### 6.1 watchdog

总共监控5个事件。

- **timer:hrtimer_expire_entry**，加上过滤器，用于跟踪watchdog_timer_fn的执行。
- **timer:hrtimer_start**，加上过滤器，监控watchdog_timer的启动。
- **timer:hrtimer_cancel**，加上过滤器，监控watchdog_timer的取消。
- **sched:sched_switch**，加上过滤器，监控watchdog线程的运行。
- **pmu:bus-cycles**，用于定时发起NMI中断，采样内核栈。

在pmu:bus-cycles事件发生时，判断hard lockup，如果预测将要发生硬死锁，就输出抓取的内核栈。

在timer:hrtimer_expire_entry事件发生时，判断soft lockup，如果预测将要发生软死锁，就输出线程调度信息和pmu:bus-cycles事件采样的内核栈。