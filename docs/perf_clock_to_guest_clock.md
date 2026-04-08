# perf clock convert to guest clock

在虚拟化场景，经常需要同时跟踪，host发生的事件和guest发生的事件。但由于kvm模块存在tsc-offset、tsc-scaling，host事件和guest事件的时间戳是不对应的。

时间戳对齐，就是把host事件的时间戳转换为guest时间戳。时间戳在同一尺度上，就可以跟guest事件的时间戳做比较。

时间戳对齐后，能够带来更多可能性：

- 可以观察，host注入中断到guest处理中断的延迟。
- 可以观察，vcpu发生一次调度，guest内在执行哪个进程。
- 可以观察，guest发送IO到host处理完IO的延迟。



时间戳对齐，分为多步转换工作。又细分为2种情况：

- Guest使用tsc时钟源。
  1. Host事件时间戳转换为tsc。
     1. tsc 再转换为 guest tsc。
  2. Guest事件时间戳转换为tsc。

- Guest使用kvmclock时钟源。
  1. Host事件时间戳转换为tsc。
     1. tsc 再转换为 guest tsc。
     2. guest tsc 转换为 kvmclock。
  2. Guest事件时间转换为 kvmclock。



# 1 Guest使用tsc时钟源

```
$ cat /sys/devices/system/clocksource/clocksource0/current_clocksource 
tsc
```

`tsc`指示使用tsc时钟源。

## 1.1 Host事件时间戳转换为tsc

利用`--tsc`参数，可以把host事件的时间戳直接转换为tsc。

```
$ ./perf-prof trace -e sched:sched_wakeup --tsc -N 1
TSC conversion is not supported.
2023-03-28 18:45:07.098609           <idle>      0 dNh. [002] 18085684.369061: sched:sched_wakeup: sap1014:11492 [120] success=1 CPU:002
```

先使用`perf-prof trace`做个测试，如果不支持tsc转换，会提示`TSC conversion is not supported.`



## 1.2 tsc转换为guest tsc

利用`--clock-offset`参数，可以把tsc时间戳转换为guest tsc。

先要利用kvm模块的信息，找到`vcpu->arch.tsc_offset`的值。不同的guest值不一样，需要找到指定guest的。

### 1.2.1 得到tsc_offset

```
$ cat /sys/kernel/debug/kvm/11524-15/vcpu0/tsc-offset 
-4949202418480468
# 11524-15, 11524是qemu进程的pid，15是kvm-vm的文件描述符。
$ printf '0x%x\n' $(cat /sys/kernel/debug/kvm/11524-15/vcpu0/tsc-offset)
0xffee6aba03eccaac
```

转换成16进制值。

### 1.2.2 转换为guest tsc

```
$ ./perf-prof trace -e sched:sched_wakeup --tsc --clock-offset 0xffee6aba03eccaac -N 1
2023-03-28 18:53:53.757492           <idle>      0 dNh. [024] 5696917.795461: sched:sched_wakeup: pal_session:57886 [100] success=1 CPU:024
```

可以正常转换。

此时，host事件的时间戳已经转换为guest tsc时间。



## 1.3 guest配置

### 1.3.1 guest关闭kvmclock

加上内核启动参数 "no-kvmclock tsc=nowatchdog"。需要配置grub，并重启guest生效。

### 1.3.2 stable tsc(可选项)

host 需要配置xml文件：

```
<feature policy='require' name="invtsc"/>
```



## 1.4 Guest事件时间戳转换为tsc

利用`--tsc`参数做个测试，判断是否支持转换为tsc。如果不支持，配置#3.2 stable tsc。



host事件和guest事件，时间戳全部都转换为tsc，**时间戳对齐**。



## 1.5 验证正确性

Guest内通过`rdmsr 0x11`命令读取msr的值。

1. Guest跟踪`msr:read_msr`事件。
2. Host跟踪`kvm:kvm_msr`事件。

Guest

```
# ./perf-prof trace -e msr:read_msr/msr==0x11/ --tsc
2024-04-24 17:01:11.009305              rdmsr   3862 d... [000] 681.185714: msr:read_msr: 11, value 0
```

Host

```
# printf '0x%x\n' $(cat /sys/kernel/debug/kvm/149237-15/vcpu0/tsc-offset)
0xff13c6854e697358
# ./perf-prof trace -e kvm:kvm_msr/ecx==0x11/ --tsc --clock-offset 0xff13c6854e697358
2024-04-24 17:01:10.992690          CPU 0/KVM 149383 .... [019] 681.185687: kvm:kvm_msr: msr_read 11 = 0x0
```

Host在`681.185687`读取msr 0x11的值，Guest在`681.185714`取到msr的值。时间偏差27khz。

时间戳都是Guest tsc时钟。



# 2 Guest使用kvmclock时钟源

```
$ cat /sys/devices/system/clocksource/clocksource0/current_clocksource 
kvm-clock
```

`kvm-clock`指示使用的是kvmclock时钟源。



## 2.1 Host事件时间戳转换为kvmclock

利用`--kvmclock`参数，可以把Host事件时间戳转换为kvmclock。

```
$ ./perf-prof trace -e sched:sched_wakeup -N 1 --kvmclock 8ab13543-95fc-4a78-9056-d605a03e9033
2024-04-24 16:09:26.631550 trace: wait pvclock update
2024-04-24 16:11:44.473313 trace: pvclock updated.
2024-04-24 16:11:44.474445            swapper      0 dN.. [048] 16132.145419: sched:sched_wakeup: sshd:64373 [120] success=1 CPU:048
```

首先会提示等待pvclock更新，更新完成之后，会正常采样时间。`16132.145419`就是转换后的kvmclock时钟，使用的是`8ab13543-95fc-4a78-9056-d605a03e9033`虚拟机的pvclock结构。

如果Guest使用的是tsc时钟源，则`wait pvclock update`会一直等待，不会有pvclock更新事件。

内部的转换过程，分为2个阶段：

### 2.1.1 建立阶段

1. 使用`virsh qemu-monitor-command UUID --hmp info cpus`找到Qemu进程所有vcpu的线程id，以及进程id，kvm-vm文件描述符。

2. 从`/sys/kernel/debug/kvm/11524-15/vcpu0/tsc-offset`读取tsc_offset参数。`11524-15`为qemu的进程id和kvm-vm文件描述符。

3. 跟踪`kvm:kvm_pvclock_update`事件，获取虚拟机的pvclock结构。

4. 更新pvclock结构之后，开始采样。

   

### 2.1.2 转换阶段

在Host上采样的事件，其时间戳是perf调用内核态的local_clock()函数获取的。

1. local_clock转换为Host tsc。使用`perf_event_mmap_page`保存的字段：`time_mult, time_shift, time_zero`
2. Host tsc转换为Guest tsc。使用 tsc_offset 参数。
3. Guest tsc 转换为 Guest kvmclock。使用 pvclock 结构。



这样，Host上采样的事件，其时间戳就转换为Guest的kvmclock时钟。



## 2.2 Guest事件时间戳转换为kvmclock

在Guest采样的事件，其时间戳是perf调用内核态的local_clock()函数获取的。其local_clock的读取的kvmclock时钟源。

```
local_clock() = kvm_clock_read() - kvm_sched_clock_offset + __sched_clock_offset
```

内核配置CONFIG_HAVE_UNSTABLE_SCHED_CLOCK=y，则有__sched_clock_offset。

经过调整后。

```
kvm_clock_read() = local_clock() + kvm_sched_clock_offset - __sched_clock_offset
```

### 2.2.1 获取参数

```
$ echo 'p:try_to_wake_up try_to_wake_up kvm_sched_clock_offset=@kvm_sched_clock_offset __sched_clock_offset=@__sched_clock_offset' > /sys/kernel/debug/tracing/kprobe_events
$ ./perf-prof trace -e kprobes:try_to_wake_up -N 1
2024-04-24 16:41:10.346226            swapper      0 d.s. [004] 17890.752743: kprobes:try_to_wake_up: (ffffffff810ba2a0) kvm_sched_clock_offset=0x1abf681cf __sched_clock_offset=0xfffffffffcc60c03
$ printf "0x%x\n" $((0x1abf681cf-0xfffffffffcc60c03))
0x1af3075cc
```

`kvm_sched_clock_offset`和`__sched_clock_offset`都是内核变量，可以直接获取。

计算kvm_sched_clock_offset - __sched_clock_offset的值，`0x1af3075cc`

### 2.2.2 转换为kvmclock

利用`--clock-offset`参数，可以把perf时间戳转换为kvmclock。

```
$ ./perf-prof trace -e sched:sched_wakeup --clock-offset 0x1af3075cc -N 1
2024-04-24 16:44:52.636906            swapper      0 dNh. [006] 18120.273901: sched:sched_wakeup: tuned:1228 [120] CPU:006
```

`18120.273901`就是转换后的kvmclock时钟。



host事件和guest事件，时间戳全部都转换为kvmclock，**时间戳对齐**。



## 2.3 验证正确性

Guest内通过`rdmsr 0x11`命令读取msr的值。

1. Guest跟踪`msr:read_msr`事件。
2. Host跟踪`kvm:kvm_msr`事件。

Guest

```
$ ./perf-prof trace -e msr:read_msr/msr==0x11/ --clock-offset 0x1af3075cc
2024-04-24 16:52:00.049514            swapper      0 d.h. [000] 18547.682219: msr:read_msr: 11, value 3600000
```

Host

```
$ ./perf-prof trace -e kvm:kvm_msr/ecx==0x11/ --kvmclock 8ab13543-95fc-4a78-9056-d605a03e9033
2024-04-24 16:47:55.220411 trace: wait pvclock update
2024-04-24 16:51:52.880260 trace: pvclock updated.
2024-04-24 16:52:00.051508          CPU 0/KVM  11929 .... [018] 18547.682210: kvm:kvm_msr: msr_read 11 = 0x360000
```

Host在`18547.682210`读取msr 0x11的值，Guest在`18547.682219`取到msr的值。时间偏差9us。

时间戳都是Guest kvmclock时钟。