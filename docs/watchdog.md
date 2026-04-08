# watchdog

监控hard、soft lockup的情况，在将要发生时，预先打印出内核栈。

总共监控5个事件。

- **timer:hrtimer_expire_entry**，加上过滤器，用于跟踪watchdog_timer_fn的执行。
- **timer:hrtimer_start**，加上过滤器，监控watchdog_timer的启动。
- **timer:hrtimer_cancel**，加上过滤器，监控watchdog_timer的取消。
- **sched:sched_switch**，加上过滤器，监控watchdog线程的运行。
- **pmu:bus-cycles**，用于定时发起NMI中断，采样内核栈。

在pmu:bus-cycles事件发生时，判断hard lockup，如果预测将要发生硬死锁，就输出抓取的内核栈。

在timer:hrtimer_expire_entry事件发生时，判断soft lockup，如果预测将要发生软死锁，就输出线程调度信息和pmu:bus-cycles事件采样的内核栈。

在发生hardlockup时，一般伴随着长时间关闭中断，可能会导致其他cpu执行也卡住，导致perf-prof工具也无法执行。这样的场景，可以借助crash来分析内核perf_event的ring buffer来获取到一定的栈。虽然工具无法执行，但采样还是会持续采的。

```
用法:
	perf-prof watchdog -F 1 -g

  -F, --freq=n               指定采样的频率，采样是使用内核的pmu事件，会发起nmi中断，-F指定发起nmi中断的频率。
  -g, --call-graph           抓取内核栈，发起pmu事件时，把内核态栈采样到。
  -C, --cpu=CPU              指定在哪些cpu上启用watchdog监控，不指定默认会读取/proc/sys/kernel/watchdog_cpumask来确定所有启用的CPU，默认开启nohzfull的cpu不启用watchdog。
```

