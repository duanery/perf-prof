# kmemleak

分析内存泄露，一般内存分配至少需要alloc和free两个接口。内存泄露，alloc之后永远不释放。

工具原理：

- 在alloc点抓到对应的内存分配信息，进程id、comm、内核栈、分配时间。并存到alloc链表里。
- 在free点，从alloc链表查找alloc信息。能找到，说明正确的分配和释放，删除alloc链表的记录。找不到说明在工具启动前分配的，直接丢弃。
- alloc和free之间通过`ptr`指针关联起来。动态增加的alloc/free tracepoint点需要ptr指针。
- 在工具结束时，打印所有alloc链表的信息。即为，*最可能的内存泄露点*。工具执行时间越久，越能得到最准确的信息。

可以解决内核[kmemleak](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/dev-tools/kmemleak.rst)工具不支持percpu内存泄露问题。

可以分析**内核态内存泄露**、**用户态内存泄露**。

共监控2个事件：

- **alloc**，需要自己指定，可以是`kmem:kmalloc、pcpu_alloc`等分配内存点。alloc点需要获取内核栈。可以指定多个alloc点。
  - alloc点可以指定一些属性信息：`ptr=filed`内存分配指针字段，`size=field`内存分配大小字段。通过指定的这些字段可以读取指针和大小。
- **free**，需要自己指定，与alloc相对应。可以是`kmem:kfree、free_percpu`等释放内存点。free点不需要栈信息。可以指定多个free点。

```
用法:
perf-prof kmemleak --alloc EVENT[...] --free EVENT[...] [-g [--flame-graph file]] [-v]

Event selector. use 'perf list tracepoint' to list available tp events.
  EVENT,EVENT,...
  EVENT: sys:name[/filter/ATTR/ATTR/.../]
  filter: ftrace filter
  ATTR:
      stack: sample_type PERF_SAMPLE_CALLCHAIN
      max-stack=int : sample_max_stack
      ptr=field: kmemleak, ptr field, Dflt: ptr=ptr
      size=field: kmemleak, size field, Dflt: size=bytes_alloc

 OPTION:
  -C, --cpu=CPU[-CPU],...    Monitor the specified CPU, Dflt: all cpu
  -i, --interval=ms          Interval, Unit: ms
  -m, --mmap-pages=pages     Number of mmap data pages and AUX area tracing mmap pages
      --order                Order events by timestamp.
  -p, --pids=PID,...         Attach to processes
  -t, --tids=TID,...         Attach to thread
  -v, --verbose              Verbose debug output

      --alloc=EVENT,...      Memory alloc tracepoint/kprobe
      --free=EVENT,...       memory free tracepoint/kprobe
      --flame-graph=file     Specify the folded stack file.
  -g, --call-graph           Enable call-graph recording
  -v, --verbose              Verbose debug output
```

## 1 内核态内存泄露

## 1.1 kmalloc

```
perf-prof kmemleak --alloc "kmem:kmalloc,kmem:kmalloc_node" --free kmem:kfree -m 64 -g --order
```

--alloc，监控`kmem:kmalloc, kmem:kmalloc_node`2个分配入口。

--free，监控`kmem:kfree`1个释放点。

启用order，按时间排序所有cpu上的采样事件，可以减少内核唤醒perf-prof的次数，减少cpu消耗。并启用4M的排序内存。

## 1.2 kmem_cache_alloc

```
perf-prof kmemleak --alloc "kmem:kmem_cache_alloc/bytes_alloc>256/,kmem:kmem_cache_alloc_node/bytes_alloc>256/" --free "kmem:kmem_cache_free" -m 64 -g --order
```

--alloc，监控`kmem:kmem_cache_alloc, kmem:kmem_cache_alloc_node`2个分配入口。并过滤`bytes_alloc>256`的内存分配请求。

--free，监控`kmem:kmem_cache_free`1个释放点。

## 1.3  percpu

```
echo 'r:alloc_percpu pcpu_alloc ptr=$retval' >> /sys/kernel/debug/tracing/kprobe_events #ptr指向分配的内存地址
echo 'p:free_percpu free_percpu ptr=%di' >> /sys/kernel/debug/tracing/kprobe_events
perf-prof kmemleak --alloc kprobes:alloc_percpu --free kprobes:free_percpu -m 8 -g --order
```

alloc_percpu，增加pcpu_alloc return kprobe点，使用ptr参数接收返回的指针。

free_percpu，增加free_percpu kprobe点，使用ptr参数接收释放的指针。

--alloc，监控`kprobes:alloc_percpu`分配入口。

--free，监控`kprobes:free_percpu`释放点。



## 2 用户态内存泄露

可以借助tcmalloc的MallocHook功能，来把多个分配和释放接口，统一成2个：NewHook，DeleteHook。然后增加对应的uprobe点，来跟踪用户态内存泄露。

1. 编译libtcmalloc

   ```
   git clone https://github.com/gperftools/gperftools.git
   cd gperftools
   ./autogen.sh
   CPPFLAGS=-fno-omit-frame-pointer ./configure
   CPPFLAGS=-fno-omit-frame-pointer make
   make install
   把 libtcmalloc.so.4.5.9 移动到 /tmp/trace 目录
   tcmalloc库需要启用frame pointer, 才可以穿透tcmalloc库来获取应用程序的栈
   ```

2. 编译[malloc_hook](malloc_hook.c)

   ```
   gcc -shared -fPIC malloc_hook.c -o malloc_hook.so
   把 malloc_hook.so 移动到 /tmp/trace 目录
   ```

3. 添加uprobe点

   ```
   # 0x7d5为NewHook函数的偏移量，0x7e3为DeleteHook函数的偏移量。
   echo "p:NewHook /tmp/trace/malloc_hook.so:0x7d5 ptr=%di len=%si" >> /sys/kernel/debug/tracing/uprobe_events
   echo "p:DeleteHook /tmp/trace/malloc_hook.so:0x7e3 ptr=%di" >> /sys/kernel/debug/tracing/uprobe_events
   ```

4. 复现内存泄露

   ```
   LD_PRELOAD="/tmp/trace/libtcmalloc.so.4.5.9 /tmp/trace/malloc_hook.so" ./leak
   ```

5. 启动perf-prof

   ```
   perf-prof kmemleak --alloc "uprobes:NewHook//size=len/" --free uprobes:DeleteHook -p 213597 -g # 213597为leak进程PID。
   ```



**内部原理**

```
leak       libtcmalloc    malloc_hook                        perf-prof
malloc ->  tc_malloc   -> NewHook(触发int3)                读取ring buffer中的栈，获取分配的内存指针ptr
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
                            |                                 |
kernel                  命中uprobe:NewHook tracepoint点        |
                            |                                 |
                        采样用户态栈传到到perf ringbuffer        |
                            `----------------------------------


leak       libtcmalloc    malloc_hook                     perf-prof
free   ->  tc_free     -> DeleteHook(触发int3)         读取内存释放ptr指针(DeleteHook)
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
                            |                                 |
kernel                  命中uprobe:DeleteHook tracepoint点     |
                            |                                 |
                        内存释放信息传到到perf ringbuffer        |
                            `----------------------------------
```



# 泄露栈报告

```bash
[root@VM ~]# perf-prof kmemleak --alloc "uprobes:NewHook" --free uprobes:DeleteHook -p 20215 -g
^C2022-04-22 19:52:32.756241 
KMEMLEAK STATS:
TOTAL alloc 6 free 9

KMEMLEAK REPORT: 3
            leak  20215 .... [005] 35171405.038116: uprobes:NewHook: (7f12bf3767d5) ptr=1cd4b80 len=80
    00007f12bf3767d5 NewHook+0x0 (/tmp/trace/malloc_hook.so)
    00007f12bf5af5d4 _ZN8tcmalloc24allocate_full_malloc_oomEm+0x154 (/tmp/trace/libtcmalloc.so.4.5.9)
    000000000040061b main+0xe (/tmp/trace/leak)
    00007f12befc9575 __libc_start_main+0xf5 (/usr/lib64/libc-2.17.so)
            leak  20215 .... [005] 35171406.038246: uprobes:NewHook: (7f12bf3767d5) ptr=1cd4c00 len=80
    00007f12bf3767d5 NewHook+0x0 (/tmp/trace/malloc_hook.so)
    00007f12bf5af5d4 _ZN8tcmalloc24allocate_full_malloc_oomEm+0x154 (/tmp/trace/libtcmalloc.so.4.5.9)
    000000000040061b main+0xe (/tmp/trace/leak)
    00007f12befc9575 __libc_start_main+0xf5 (/usr/lib64/libc-2.17.so)
            leak  20215 .... [005] 35171407.038375: uprobes:NewHook: (7f12bf3767d5) ptr=1cd4c80 len=80
    00007f12bf3767d5 NewHook+0x0 (/tmp/trace/malloc_hook.so)
    00007f12bf5af5d4 _ZN8tcmalloc24allocate_full_malloc_oomEm+0x154 (/tmp/trace/libtcmalloc.so.4.5.9)
    000000000040061b main+0xe (/tmp/trace/leak)
    00007f12befc9575 __libc_start_main+0xf5 (/usr/lib64/libc-2.17.so)
```

默认情况下，会报告泄露的栈。按照内存分配的时间先后顺序汇报。

这种场景报告的栈会比较多，可以通过`--flame-graph`启用火焰图，利用把相同的栈聚合起来。



# 泄露字节报告

```bash
[root@VM ~]# cat /sys/kernel/debug/tracing/events/uprobes/NewHook/format 
name: NewHook
ID: 1305
format:
        field:unsigned short common_type;       offset:0;       size:2; signed:0;
        field:unsigned char common_flags;       offset:2;       size:1; signed:0;
        field:unsigned char common_preempt_count;       offset:3;       size:1; signed:0;
        field:int common_pid;   offset:4;       size:4; signed:1;

        field:unsigned long __probe_ip; offset:8;       size:8; signed:0;
        field:u64 ptr;  offset:16;      size:8; signed:0;
        field:u64 len;  offset:24;      size:8; signed:0;

print fmt: "(%lx) ptr=%llx len=%llx", REC->__probe_ip, REC->ptr, REC->len
[root@VM ~]# perf-prof kmemleak --alloc "uprobes:NewHook//size=len/" --free uprobes:DeleteHook -p 20215 -g
^C2022-04-22 18:03:41.696534 
KMEMLEAK STATS:
TOTAL alloc 48 free 72

LEAKED BYTES REPORT:
Leak of 3072 bytes in 24 objects allocated from:
    00007f12bf3767d5 NewHook+0x0 (/tmp/trace/malloc_hook.so)
    00007f12bf5af5d4 _ZN8tcmalloc24allocate_full_malloc_oomEm+0x154 (/tmp/trace/libtcmalloc.so.4.5.9)
    000000000040061b main+0xe (/tmp/trace/leak)
    00007f12befc9575 __libc_start_main+0xf5 (/usr/lib64/libc-2.17.so)
```

通过`uprobes:NewHook`可以发现ptr存放内存分配的指针，len存放内存分配大小。通过指定`--alloc "uprobes:NewHook//size=len/"`size=属性可以启用泄露字节报告。

泄露字节报告，会把相同的栈聚合起来，报告泄露的总字节数、分配的对象数量，以及栈信息。

- 相同的栈只报告一次。
- 按照泄露字节数量，从大到下排序。
- 不支持火焰图。栈相对较少，不再支持火焰图。

只需要重点分析泄露字节最多的栈。

