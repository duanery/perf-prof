# 1 Guest和Host基于virtio-ports通信

## 1.1 Guest xml添加controller

```
<controller type="virtio-serial" index="0" ports="32"/>
```



## 1.2 Guest xml添加virtio-serial通道

通道可以热插拔，按需添加。

```
  <channel type='tcp'>
    <source mode='bind' host='127.0.0.1' service='9900'/>
    <target type='virtio' name='org.qemu.perf0' state='connected'/>
  </channel>
```



# 2 原理

1. qemu建立socket chardev，并绑定到127.0.0.1:9900端口。
2. socket chardev前端连接virtserialport，再连接到virtio-serial总线上。
3. 最终在Guest内加载virtio-console驱动，Guest用户态看到 /dev/virtio-ports/org.qemu.perf0 字符设备。



- Guest内 perf-prof 采样事件，并传播一份写到 /dev/virtio-ports/org.qemu.perf0 字符设备。
- Host上 perf-prof 连接 127.0.0.1:9900 端口，接收Guest的事件，并对事件做一定的转换并处理。同时还可以采样Host内核事件，并跟Guest事件一起分析。



# 3 Guest和Host通信特殊用途

## 3.1 QEMU Guest Agent

```
virsh qemu-agent-command # 跟Guest交互一些简单命令。
```

https://github.com/qemu/qemu/blob/master/qga/qapi-schema.json  包含所有同Guest交互的命令。



## 3.2 跨主机事件分析

基于perf-prof采样Guest的事件，并通过virtio-serial通道发送到Host，就可以在Host上获取事件并分析。

可以用于分析：

- 中断延迟。从dpdk注入网卡中断到Guest内开始处理中断，这段的延迟。
- 调度分析。Host vcpu调度延迟，Guest内哪些进程受到影响。
- IO分析。Guest发起IO到Host接收到IO，这段的延迟。




# 4 Demo1 中断延迟统计

dpdk注入中断 => Guest内开始处理中断，统计这中间的延迟，包含Guest内关中断的时间。



## 4.1 Guest采样事件

```
./perf-prof trace -e 'irq:irq_handler_entry/name~"virtio0-input*"/push="/dev/virtio-ports/org.qemu.perf0"/' --tsc
```

Guest 采样`irq:irq_handler_entry`事件，只过滤virtio0-input*中断，并把事件转换成tsc时间，最后写到`/dev/virtio-ports/org.qemu.perf0`字符设备。

- `--tsc`参数把事件时间戳调整为Guest tsc时间戳。
- `push`属性把事件推送出去。目前仅支持 tcp 端口、字符设备、文件。推送到tcp端口，就会广播到所有连接的tcp客户端。推送到字符设备，就是写入字符设备。推送到文件，就是写入文件。

- `/dev/virtio-ports/org.qemu.perf0`字符设备，需要等待Host连接。只有在Host连接之后，才能写入。Host连接断开，字符设备会等待，直到Host再连接上。



## 4.2 Host接收事件

```
./perf-prof trace -e irq:irq_handler_entry//pull=9900/ -N 2
Connected to 127.0.0.1:9900
2023-04-12 13:47:26.370747 G           <idle>      0 d.h. [077] 186430.038050: irq:irq_handler_entry: irq=38 name=virtio0-input.3
2023-04-12 13:47:26.407948 G           <idle>      0 d.h. [077] 186430.133708: irq:irq_handler_entry: irq=38 name=virtio0-input.3
Disconnect from 127.0.0.1:9900
```

Host也采样`irq:irq_handler_entry`事件，只不过事件来自 127.0.0.1:9900 端口，也就是来自Guest。

- `pull`属性从指定位置拉起事件。目前仅支持tcp端口，文件。从tcp端口拉取事件，就是连接到tcp服务端，并接收服务端广播的事件。从文件拉取，就是读文件。
- 时间戳后面的`G`标识事件是从外部pull到的。也表示来自Guest。



## 4.3 Host处理事件

```
./perf-prof multi-trace -e 'kvm:kvm_msi_set_irq/common_pid==26027/key="(((address>>18)&1)?42:0) + ((address>>12)&0x3f)"/' -e 'irq:irq_handler_entry//pull=9900/key="83-irq+32"/' --tsc --clock-offset 0xffdafd48df582d22 --order -i 1000 --than 1ms
```

Host采样`kvm:kvm_msi_set_irq`事件，这个来自Host内核；采样`irq:irq_handler_entry`事件，来自Guest。`multi-trace`分析`kvm:kvm_msi_set_irq =>  irq:irq_handler_entry`的延迟，就是网卡中断响应的延迟，包含了Guest内关中断导致的延迟。

- `--tsc --clock-offset 0xffdafd48df582d22` 把`kvm:kvm_msi_set_irq`事件转换为Guest tsc时间戳。来自Guest的`irq:irq_handler_entry`事件不会转换，其已经在Guest内转换过了。
- `key="(((address>>18)&1)?42:0) + ((address>>12)&0x3f)"` 把`kvm:kvm_msi_set_irq`事件address参数转换成对应的Guest CPU编号。
- `key="83-irq+32"` 把`irq:irq_handler_entry`事件的irq参数转换成对应的Guest CPU编号。virio-net中断已经在Guest内绑定CPU了。



### 4.3.1 验证

Guest 利用 irq-off 功能检测中断关闭。并通过一个内核模块，关中断50ms左右。

```
# ./perf-prof irq-off --period 5ms --than 10ms --tsc
2023-04-12 15:38:44.645139  350094/350094 [079]  203124.380711: cpu-clock: 54312782 ns
```

Host 利用 multi-trace 功能检测中断响应延迟大于1ms的事件。

```
2023-04-12 15:38:45.754019       worker3_1.95  26027 d... [095] 203124.373091: kvm:kvm_msi_set_irq: dst 65 vec 35 (Fixed|physical|edge)
2023-04-12 15:38:45.754102 G            <...> 350094 dNh. [079] 203124.380722: irq:irq_handler_entry: irq=36 name=virtio0-input.2
2023-04-12 15:38:45.755977 
          start => end                  calls      total(kcyc)    min(kcyc)    p50(kcyc)    p95(kcyc)    p99(kcyc)    max(kcyc)
---------------    ----------------- -------- ---------------- ------------ ------------ ------------ ------------ ------------
kvm_msi_set_irq => irq_handler_entry     5641        40951.486        4.971        5.736        7.039        7.938     7631.115
```

Guest在203124.380711时刻在[079]CPU上检测到一个54ms的中断关闭。Host在203124.380722时刻在[079]上检测到一个7.6ktsc的中断关闭。

时间戳基本匹配，所以利用 multi-trace 可以进行Guest和Host事件的延迟分析。



## 4.4 结论

- Guest和Host事件能够混合起来分析。
- 延迟分析需要把Guest和Host事件都转换为Guest时间戳。
- 基于virtio-ports通道通信是可以正常工作的。
