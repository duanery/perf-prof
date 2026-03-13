#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <monitor.h>
#include <tep.h>
#include <linux/thread_map.h>
#include <tp_struct.h>
#include <linux/math64.h>
#include <api/fs/fs.h>
#include <linux/bitops.h>
#include <internal/evlist.h>

bool current_clocksource_is_tsc = false;

/*
 *  { u64           id;   } && PERF_SAMPLE_IDENTIFIER
 *  { u64           ip;   } && PERF_SAMPLE_IP
 *  { u32           pid, tid; } && PERF_SAMPLE_TID
 *  { u64           time;     } && PERF_SAMPLE_TIME
 */
#define SAMPLE_TYPE_MASK (PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME)

u64 rdtsc(void)
{
#if defined(__i386__) || defined(__x86_64__)
	unsigned int low, high;

	asm volatile("rdtsc" : "=a" (low), "=d" (high));

	return low | ((u64)high) << 32;
#else
    return 0;
#endif
}

static inline tsc_t perfclock_to_tsc(struct prof_dev *dev, perfclock_t ns)
{
    struct perf_tsc_conversion *tc = &dev->convert.tsc_conv;
    u64 t, quot, rem;

    // ((ns - time_zero) << time_shift) / time_mult
    t = ns - tc->time_zero;
    quot = t / tc->time_mult;
    rem  = t % tc->time_mult;
    return (quot << tc->time_shift) +
           (rem << tc->time_shift) / tc->time_mult;
}

static inline perfclock_t tsc_to_perfclock(struct prof_dev *dev, tsc_t tsc)
{
    struct perf_tsc_conversion *tc = &dev->convert.tsc_conv;
    u64 ns;

    // (tsc * time_mult) >> time_mult + time_zaro
    ns = mul_u64_u32_shr(tsc, tc->time_mult, tc->time_shift);
    return ns + tc->time_zero;
}

static inline perfclock_t tsc_to_fixed_perfclock(struct prof_dev *dev, tsc_t tsc)
{
    struct perf_tsc_conversion *tc = &dev->convert.tsc_conv_fixed;
    u64 ns;

    // (tsc * time_mult) >> time_mult + time_zaro
    ns = mul_u64_u32_shr(tsc, tc->time_mult, tc->time_shift);
    return ns + tc->time_zero;
}

#if defined(__i386__) || defined(__x86_64__)

#define __USE_GNU
#include <sched.h>
#include <cpuid.h>

u8  __read_mostly kvm_tsc_scaling_ratio_frac_bits = 0;
u64 __read_mostly kvm_default_tsc_scaling_ratio = 0;
unsigned short kvm_pvclock_update_id;
unsigned short kvm_write_tsc_offset_id;

#define MSR_IA32_VMX_PROCBASED_CTLS     0x00000482
#define CPU_BASED_ACTIVATE_SECONDARY_CONTROLS   0x80000000

#define MSR_IA32_VMX_PROCBASED_CTLS2    0x0000048b
#define SECONDARY_EXEC_TSC_SCALING              0x02000000

static int adjust_vmx_controls(uint64_t msr_value)
{
    u32 vmx_msr_low = (u32)msr_value;
    u32 vmx_msr_high = msr_value >> 32;
    u32 ctl = -1;

    ctl &= vmx_msr_high; /* bit == 0 in high word ==> must be zero */
    ctl |= vmx_msr_low;  /* bit == 1 in low word  ==> must be one  */

    return ctl;
}

static int tsc_scaling_setup(void)
{
    static int once = 0;
    int vendor;

    if (once != 0) return once;

    once = -1;
    vendor = get_cpu_vendor();

    if (vendor == X86_VENDOR_INTEL) {
        char path[64];
        int fd, cpu = sched_getcpu();
        uint64_t msr_value;

        snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu < 0 ? 0 : cpu);
        fd = open(path, O_RDONLY);
        if (fd < 0) return -1;

        if (pread(fd, &msr_value, sizeof(msr_value), MSR_IA32_VMX_PROCBASED_CTLS) != sizeof(msr_value))
            goto ret;
        if (!(adjust_vmx_controls(msr_value) & CPU_BASED_ACTIVATE_SECONDARY_CONTROLS))
            goto ret;
        if (pread(fd, &msr_value, sizeof(msr_value), MSR_IA32_VMX_PROCBASED_CTLS2) != sizeof(msr_value))
            goto ret;
        if (!(adjust_vmx_controls(msr_value) & SECONDARY_EXEC_TSC_SCALING))
            goto ret;

        kvm_tsc_scaling_ratio_frac_bits = 48;
        kvm_default_tsc_scaling_ratio = 1ULL << kvm_tsc_scaling_ratio_frac_bits;
        once = 1;
    ret:
        close(fd);
    } else if (vendor == X86_VENDOR_AMD) {
        __u32 eax, ebx, ecx, edx;

        eax = ebx = ecx = edx = 0;
        __get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
        if (eax >= 0x8000000a) {
            __get_cpuid(0x8000000a, &eax, &ebx, &ecx, &edx);
            /*
             * CPUID Fn8000_000A_EDX SVM Feature Identification
             * bit4 TscRateMsr MSR based TSC rate control. Indicates support for MSR TSC ratio
             * MSRC000_0104. See "TSC Ratio MSR (C000_0104h)."
             */
            if (edx & 0x8) {
                kvm_tsc_scaling_ratio_frac_bits = 32;
                kvm_default_tsc_scaling_ratio = 1ULL << kvm_tsc_scaling_ratio_frac_bits;
                once = 1;
            }
        }
    }

    return once;
}

static int vcpu_info_update_vcpu0_tsc(struct vcpu_info *vcpu)
{
    const char *debugfs;
    char path[512];
    unsigned long long tsc_offset;
    unsigned long long tsc_scaling_ratio;
    unsigned long long tsc_scaling_ratio_frac_bits;

    /*
     * Read /sys/kernel/debug/kvm/$pid-$kvm_vm_fd/vcpu/tsc-offset
     * Only read vcpu0, use the master clock to ensure that the values of all vcpu are the same.
     */
    debugfs = debugfs__mountpoint();

    snprintf(path, sizeof(path), "%s/kvm/%d-%d/vcpu0/tsc-offset", debugfs, vcpu->tgid, vcpu->kvm_vm_fd);
    if (filename__read_ull(path, &tsc_offset) < 0)
        return -1;

    snprintf(path, sizeof(path), "%s/kvm/%d-%d/vcpu0/tsc-scaling-ratio", debugfs, vcpu->tgid, vcpu->kvm_vm_fd);
    if (filename__read_ull(path, &tsc_scaling_ratio) < 0)
        tsc_scaling_ratio = 0;

    snprintf(path, sizeof(path), "%s/kvm/%d-%d/vcpu0/tsc-scaling-ratio-frac-bits", debugfs, vcpu->tgid, vcpu->kvm_vm_fd);
    if (filename__read_ull(path, &tsc_scaling_ratio_frac_bits) < 0)
        tsc_scaling_ratio_frac_bits = 0;

    vcpu->vcpu[0].tsc_offset = tsc_offset;
    vcpu->vcpu[0].tsc_scaling_ratio = tsc_scaling_ratio;
    vcpu->vcpu[0].tsc_scaling_ratio_frac_bits = tsc_scaling_ratio_frac_bits;

    return 0;
}


static void kvm_pvclock_update(void *parent, void *raw)
{
    struct prof_dev *dev = parent, *tmp;
    struct kvm_pvclock_update *pvclock = raw;
    struct kvm_write_tsc_offset *tsc = raw;
    struct vcpu_info *vcpu = dev->convert.vcpu;

    if (likely(pvclock->common_type == kvm_pvclock_update_id)) {
        struct pvclock_vcpu_time_info *pvti = &vcpu->vcpu[0].pvti;
        bool update = !pvti->version;

        pvti->version = pvclock->version;
        pvti->tsc_timestamp = pvclock->tsc_timestamp;
        pvti->system_time = pvclock->system_time;
        pvti->tsc_to_system_mul = pvclock->tsc_to_system_mul;
        pvti->tsc_shift = pvclock->tsc_shift;
        pvti->flags = pvclock->flags;

        // The same --kvmclock option points to the same vcpu. So, enable the same for all devices.
        for_each_dev_get(dev, tmp, &prof_dev_list, dev_link) {
            if (dev->convert.vcpu != vcpu)
                continue;

            if (update) {
                print_time(stdout);
                printf("%s: pvclock updated.\n", dev->prof->name);
            }
            prof_dev_enable(dev);
        }
    } else if (tsc->common_type == kvm_write_tsc_offset_id) {
        u64 *tsc_offset = &vcpu->vcpu[0].tsc_offset;

        if (tsc->vcpu_id == 0 &&
            tsc->previous_tsc_offset == *tsc_offset) {
            *tsc_offset = tsc->next_tsc_offset;
        } else {
            fprintf(stderr, "%s: tsc_offset update failed\n", dev->prof->name);
        }
    }
}

static void kvm_pvclock_hangup(void *parent)
{
    prof_dev_close(parent);
}

static inline u64 __scale_tsc(u64 ratio, u64 tsc)
{
	return mul_u64_u64_shr(tsc, ratio, kvm_tsc_scaling_ratio_frac_bits);
}

static inline u64 kvm_scale_tsc(struct prof_dev *dev, u64 tsc)
{
	u64 _tsc = tsc;
	u64 ratio = dev->convert.vcpu->vcpu[0].tsc_scaling_ratio;

	if (ratio != kvm_default_tsc_scaling_ratio)
		_tsc = __scale_tsc(ratio, tsc);

	return _tsc;
}

static inline u64 kvm_read_l1_tsc(struct prof_dev *dev, u64 host_tsc)
{
	return dev->convert.vcpu->vcpu[0].tsc_offset + kvm_scale_tsc(dev, host_tsc);
}

static inline kvmclock_t host_tsc_to_kvmclock(struct prof_dev *dev, tsc_t host_tsc)
{
    // host_tsc => guest_tsc
    // guest_tsc = tsc_offset + (host_tsc * tsc_scaling_ratio) >> kvm_tsc_scaling_ratio_frac_bits
    tsc_t guest_tsc = kvm_read_l1_tsc(dev, host_tsc);

    // guest_tsc => kvmclock
    // nsec = (guest_tsc - tsc_timestamp) * tsc_to_system_mul * 2^(tsc_shift-32)
    //          + system_time
    return __pvclock_read_cycles(&dev->convert.vcpu->vcpu[0].pvti, guest_tsc);
}

static inline kvmclock_t perfclock_to_kvmclock(struct prof_dev *dev, perfclock_t time)
{
    tsc_t host_tsc = perfclock_to_tsc(dev, time);
    return host_tsc_to_kvmclock(dev, host_tsc);
}

static inline perfclock_t kvmclock_to_perfclock(struct prof_dev *dev, kvmclock_t time)
{
    struct vcpu_data *v0 = &dev->convert.vcpu->vcpu[0];
    int tsc_shift = v0->pvti.tsc_shift - 32;
    u64 offset;
    u64 delta;
    u64 guest_tsc;
    u64 host_tsc;

    // kvmclock => guest_tsc
    // guest_tsc = ((time - system_time) << -(tsc_shift-32)) / tsc_to_system_mul + tsc_timestamp
    if (tsc_shift < 0) tsc_shift = -tsc_shift;
    offset = time - v0->pvti.system_time;
    delta = mul_u64_u64_div64(offset, 1UL << tsc_shift, v0->pvti.tsc_to_system_mul);
    guest_tsc = delta + v0->pvti.tsc_timestamp;

    // guest_tsc => host_tsc
    // host_tsc = ((guest_tsc - tsc_offset) << kvm_tsc_scaling_ratio_frac_bits) / tsc_scaling_ratio
    host_tsc = guest_tsc - v0->tsc_offset;
    if (v0->tsc_scaling_ratio != kvm_default_tsc_scaling_ratio)
        host_tsc = mul_u64_u64_div64(guest_tsc, kvm_default_tsc_scaling_ratio, v0->tsc_scaling_ratio);

    // host_tsc => perfclock
    return tsc_to_perfclock(dev, host_tsc);
}

static int perf_event_convert_kvmclock_init(struct prof_dev *dev)
{
    struct vcpu_data *vcpu0;

    tsc_scaling_setup();

    // The same --kvmclock option points to the same vcpu.
    dev->convert.vcpu = vcpu_info_get(dev->env->kvmclock);
    if (!dev->convert.vcpu)
        goto failed;

    vcpu0 = &dev->convert.vcpu->vcpu[0];
    if (!vcpu0->pvclock_update) {
        struct perf_thread_map *vcpumap;
        struct prof_dev *pvclock;

        vcpumap = thread_map__new_by_tid(vcpu0->thread_id);
        if (!vcpumap)
            goto failed;

        pvclock = trace_dev_open("kvm:kvm_pvclock_update,kvm:kvm_write_tsc_offset", NULL, vcpumap,
                                  dev, kvm_pvclock_update, kvm_pvclock_hangup);
        perf_thread_map__put(vcpumap);
        if (!pvclock)
            goto failed;

        if (prof_dev_enable(pvclock) < 0)
            goto failed;

        if (vcpu_info_update_vcpu0_tsc(dev->convert.vcpu) < 0)
            goto failed;

        kvm_pvclock_update_id = tep__event_id("kvm", "kvm_pvclock_update");
        kvm_write_tsc_offset_id = tep__event_id("kvm", "kvm_write_tsc_offset");

        vcpu0->pvclock_update = true;
    }

    // version == 0, means pvclock has not been updated.
    if (!vcpu0->pvti.version) {
        print_time(stdout);
        printf("%s: wait pvclock update\n", dev->prof->name);
        dev->state = PROF_DEV_STATE_OFF;
    }
    dev->convert.need_conv = CONVERT_TO_KVMCLOCK;
    return 0;

failed:
    fprintf(stderr, "Could not convert to kvmclock.\n");
    return -1;
}

static void perf_event_convert_kvmclock_deinit(struct prof_dev *dev)
{
    if (dev->convert.vcpu)
        vcpu_info_put(dev->convert.vcpu);
}

#else


static inline kvmclock_t host_tsc_to_kvmclock(struct prof_dev *dev, tsc_t host_tsc)
{
    return (kvmclock_t)host_tsc;
}

static inline kvmclock_t perfclock_to_kvmclock(struct prof_dev *dev, perfclock_t time)
{
    return (kvmclock_t)time;
}

static inline perfclock_t kvmclock_to_perfclock(struct prof_dev *dev, kvmclock_t time)
{
    return (perfclock_t)time;
}

static int perf_event_convert_kvmclock_init(struct prof_dev *dev)
{
    fprintf(stderr, "Non-x86 architecture cannot be converted to kvmclock.\n");
    return -1;
}

static void perf_event_convert_kvmclock_deinit(struct prof_dev *dev) {}

#endif

static inline evclock_t __perfclock_to_evclock(struct prof_dev *dev, perfclock_t time)
{
    evclock_t evclock;

    if (dev->convert.need_conv == CONVERT_TO_TSC) {
        evclock.tsc = perfclock_to_tsc(dev, time);
    } else if (dev->convert.need_conv == CONVERT_TO_KVMCLOCK) {
        evclock.kvmclock = perfclock_to_kvmclock(dev, time);
    } else
        evclock.perfclock = time;

    evclock.clock += dev->env->clock_offset;
    return evclock;
}

evclock_t perfclock_to_evclock(struct prof_dev *dev, perfclock_t time)
{
    if (likely(!dev->convert.need_conv))
        return (evclock_t)time;
    else
        return __perfclock_to_evclock(dev, time);
}

perfclock_t evclock_to_perfclock(struct prof_dev *dev, evclock_t time)
{
    if (likely(!dev->convert.need_conv)) {
        return time.perfclock;
    }

    time.clock -= dev->env->clock_offset;

    if (dev->convert.need_conv == CONVERT_TO_TSC) {
        return tsc_to_perfclock(dev, time.tsc);
    } else if (dev->convert.need_conv == CONVERT_TO_KVMCLOCK) {
        return kvmclock_to_perfclock(dev, time.kvmclock);
    } else
        return time.perfclock;
}

/*
 * evclock converts to real ns units.
 *
 * CONVERT_NONE
 * perfclock is originally in ns unit.
 *   kernel <  4.12.0: perfclock is inaccurate and needs to be fixed.
 *   kernel >= 4.12.0: is accurate, no fix needed.
 * See the comments for tsc_conv_fixed().
 *
 * CONVERT_TO_TSC
 * Needs to be converted to perfclock, which is in ns units.
 *   kernel <  4.12.0: Convert to fixed perfclock.
 *   kernel >= 4.12.0: Convert to perfclock.
 *
 * CONVERT_TO_KVMCLOCK
 * It is originally in ns units and does not need to be converted.
 *
 * CONVERT_ADD_OFFSET
 * Same as CONVERT_NONE.
 */
real_ns_t evclock_to_real_ns(struct prof_dev *dev, evclock_t time)
{
    if (likely(!dev->convert.need_conv)) {
convert_none:
        if (dev->convert.need_fixed) {
            tsc_t tsc = perfclock_to_tsc(dev, time.perfclock);
            return tsc_to_fixed_perfclock(dev, tsc);
        } else
            return time.perfclock;
    }

    time.clock -= dev->env->clock_offset;

    if (dev->convert.need_conv == CONVERT_TO_TSC) {
        return dev->convert.need_fixed ?
               tsc_to_fixed_perfclock(dev, time.tsc) :
               tsc_to_perfclock(dev, time.tsc);
    } else if (dev->convert.need_conv == CONVERT_TO_KVMCLOCK) {
        return time.kvmclock;
    } else
        goto convert_none;
}

u64 evclock_to_realtime_ns(struct prof_dev *dev, evclock_t evtime)
{
    if (likely(dev->time_ctx.base_evtime > 0 && evtime.clock > 0)) {
        s64 off_ns = evclock_to_real_ns(dev, evtime) - dev->time_ctx.base_evtime;
        off_ns += dev->time_ctx.base_timespec.tv_nsec;

        return dev->time_ctx.base_timespec.tv_sec * NSEC_PER_SEC + off_ns;
    } else
        return 0UL;
}

static inline bool is_sampling_event(struct perf_event_attr *attr)
{
	return attr->sample_period != 0;
}

static int perf_sample_pos_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct perf_evsel *evsel, *onlyone = NULL;
    u64 mask = PERF_SAMPLE_IDENTIFIER | PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME |
               PERF_SAMPLE_ADDR | PERF_SAMPLE_ID | PERF_SAMPLE_STREAM_ID | PERF_SAMPLE_CPU |
               PERF_SAMPLE_PERIOD | PERF_SAMPLE_READ;
    u64 sample_type = 0;
    int pos = 0;

    perf_evlist__for_each_evsel(evlist, evsel) {
        struct perf_event_attr *attr = perf_evsel__attr(evsel);
        if (is_sampling_event(attr)) {
            if (sample_type == 0) {
                sample_type = attr->sample_type & mask;
                onlyone = evsel;
            } else if (sample_type != (attr->sample_type & mask)) {
                fprintf(stderr, "Could not init pos: sample_type mismatch.\n");
                return -1;
            } else
                onlyone = NULL;
        }
    }

    dev->pos.sample_type = sample_type;
    dev->pos.tid_pos = -1;
    dev->pos.time_pos = -1;
    dev->pos.id_pos = -1;
    dev->pos.cpu_pos = -1;
    dev->pos.callchain_pos = -1;

    if (sample_type & PERF_SAMPLE_IDENTIFIER)
        pos += sizeof(u64);
    if (sample_type & PERF_SAMPLE_IP)
        pos += sizeof(u64);
    if (sample_type & PERF_SAMPLE_TID) {
        dev->pos.tid_pos = pos;
        pos += sizeof(u32) + sizeof(u32);
    }
    if (sample_type & PERF_SAMPLE_TIME) {
        dev->pos.time_pos = pos;
        pos += sizeof(u64);
    }
    if (sample_type & PERF_SAMPLE_ADDR)
        pos += sizeof(u64);
    if (sample_type & PERF_SAMPLE_ID) {
        dev->pos.id_pos = pos;
        pos += sizeof(u64);
    }
    if (sample_type & PERF_SAMPLE_STREAM_ID)
        pos += sizeof(u64);
    if (sample_type & PERF_SAMPLE_CPU) {
        dev->pos.cpu_pos = pos;
        pos += sizeof(u32) + sizeof(u32);
    }
    if (sample_type & PERF_SAMPLE_PERIOD)
        pos += sizeof(u64);
    if (sample_type & PERF_SAMPLE_READ) {
        if (onlyone) {
            pos += perf_evsel__read_size(onlyone);
            dev->pos.callchain_pos = pos;
        }
    }
    else
        dev->pos.callchain_pos = pos;

    if (dev->prof->fix_sample_pos)
        dev->prof->fix_sample_pos(dev);

    return 0;
}

int perf_sample_forward_init(struct prof_dev *dev)
{
    u64 sample_type_mask = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU;
    u64 sample_type;

    if (perf_sample_pos_init(dev) < 0)
        return -1;

    sample_type = dev->pos.sample_type;
    if (sample_type && (sample_type & sample_type_mask) != sample_type_mask) {
        fprintf(stderr, "Could not init forward: sample_type mismatch.\n");
        return -1;
    }

    return 0;
}

int perf_sample_time_init(struct prof_dev *dev)
{
    u64 sample_type;

    if (perf_sample_pos_init(dev) < 0)
        return -1;

    sample_type = dev->pos.sample_type;
    if (sample_type && !(sample_type & PERF_SAMPLE_TIME)) {
        fprintf(stderr, "Could not init time_ctx: sample_type mismatch.\n");
        return -1;
    }

    dev->time_ctx.last_evtime.clock = ULLONG_MAX;
    return 0;
}

int perf_event_convert_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    u64 sample_type = 0;
    int err;

    err = perf_sample_time_init(dev);

    if (!env->tsc && !env->kvmclock && !env->clock_offset) {
        dev->convert.need_conv = CONVERT_NONE;
        return 0;
    }

    if (err < 0)
        return -1;

    sample_type = dev->pos.sample_type;
    if (sample_type & PERF_SAMPLE_TIME) {
        if (env->tsc) {
            env->tsc = true;
            dev->convert.need_conv = CONVERT_TO_TSC;
        } else if (env->kvmclock) {
            if (perf_event_convert_kvmclock_init(dev) < 0)
                return -1;
        } else
            dev->convert.need_conv = CONVERT_ADD_OFFSET;

        dev->convert.event_copy = malloc(PERF_SAMPLE_MAX_SIZE);
        if (!dev->convert.event_copy) {
            fprintf(stderr, "Could not alloc event_copy.\n");
            return -1;
        }
    } else {
        env->tsc = false;
        env->clock_offset = 0;
        dev->convert.need_conv = CONVERT_NONE;
    }

    return 0;
}

void perf_event_convert_deinit(struct prof_dev *dev)
{
    perf_event_convert_kvmclock_deinit(dev);
    if (dev->convert.event_copy)
        free(dev->convert.event_copy);
    dev->convert.need_conv = CONVERT_NONE;
}

void perf_event_convert_read_tsc_conversion(struct prof_dev *dev, struct perf_mmap *map)
{
    if (unlikely(dev->convert.need_conv == CONVERT_TO_TSC ||
                 dev->convert.need_conv == CONVERT_TO_KVMCLOCK)) {
        if (perf_mmap__read_tsc_conversion(map, &dev->convert.tsc_conv) == -EOPNOTSUPP ||
            !current_clocksource_is_tsc) {
            fprintf(stderr, (!dev->env->monotonic) ? "TSC conversion is not supported.\n" :
                              "TSC conversion does not support the --monotonic option.\n");
            dev->env->tsc = false;
            dev->env->clock_offset = 0;
            dev->convert.need_conv = CONVERT_NONE;
        }
    }
}

union perf_event *perf_event_convert(struct prof_dev *dev, union perf_event *event, bool writable)
{
    void *data;
    evclock_t *time;

    if (likely(!dev->convert.need_conv))
        return event;

    if (likely(!writable)) {
        memcpy(dev->convert.event_copy, event, event->header.size);
        event = (union perf_event *)dev->convert.event_copy;
    }

    data = (void *)event->sample.array;

    time = (evclock_t *)(data + dev->pos.time_pos);
    *time = __perfclock_to_evclock(dev, time->perfclock);

    return event;
}

#include <asm/div64.h>

/**
 * clocks_calc_mult_shift - calculate mult/shift factors for scaled math of clocks
 * @mult:	pointer to mult variable
 * @shift:	pointer to shift variable
 * @from:	frequency to convert from
 * @to:		frequency to convert to
 * @maxsec:	guaranteed runtime conversion range in seconds
 *
 * The function evaluates the shift/mult pair for the scaled math
 * operations of clocksources and clockevents.
 *
 * @to and @from are frequency values in HZ. For clock sources @to is
 * NSEC_PER_SEC == 1GHz and @from is the counter frequency. For clock
 * event @to is the counter frequency and @from is NSEC_PER_SEC.
 *
 * The @maxsec conversion range argument controls the time frame in
 * seconds which must be covered by the runtime conversion with the
 * calculated mult and shift factors. This guarantees that no 64bit
 * overflow happens when the input value of the conversion is
 * multiplied with the calculated mult factor. Larger ranges may
 * reduce the conversion accuracy by chosing smaller mult and shift
 * factors.
 */
static void
clocks_calc_mult_shift(u32 *mult, u16 *shift, u32 from, u32 to, u32 maxsec)
{
	u64 tmp;
	u32 sft, sftacc= 32;

	/*
	 * Calculate the shift factor which is limiting the conversion
	 * range:
	 */
	tmp = ((u64)maxsec * from) >> 32;
	while (tmp) {
		tmp >>=1;
		sftacc--;
	}

	/*
	 * Find the conversion shift/mult pair which has the best
	 * accuracy and fits the maxsec conversion range:
	 */
	for (sft = 32; sft > 0; sft--) {
		tmp = (u64) to << sft;
		tmp += from / 2;
		do_div(tmp, from);
		if ((tmp >> sftacc) == 0)
			break;
	}
	*mult = tmp;
	*shift = sft;
}

static void tsc_conv_fixed(struct prof_dev *dev)
{
    static int once = 0;
    static int tsc_khz = 0;

    /*
     * For kernels before 4.12
     *
     * LINUX aa7b630 x86/tsc: Feed refined TSC calibration into sched_clock()
     *
     * In the Linux kernel, the initial tsc_khz=2500000, is refined in
     * tsc_refine_calibration_work(), and then tsc_khz=2494140. cyc2ns_mul, cyc2ns_shift
     * are used to convert tsc to ns, but they are not re-modified after tsc_khz changes.
     * It's fixed in aa7b630.
     *
     * Therefore, within sched_clock(), tsc to ns are not accurate, and so is perfclock.
     * Get the correct tsc_khz, and calculate cyc2ns_mul and cyc2ns_shift, here.
     */
    if (kernel_release() < KERNEL_VERSION(4,12,0)) {
        if (once == 0) {
            once = 1;
            tsc_khz = get_tsc_khz();
        }

        dev->convert.need_fixed = tsc_khz > 0;
        if (dev->convert.need_fixed) {
            dev->convert.tsc_conv_fixed = dev->convert.tsc_conv;
            /*
             * Compute a new multiplier as per the above comment and ensure our
             * time function is continuous; see the comment near struct
             * cyc2ns_data.
            */
            clocks_calc_mult_shift(&dev->convert.tsc_conv_fixed.time_mult,
                                   &dev->convert.tsc_conv_fixed.time_shift,
                                   tsc_khz, NSEC_PER_MSEC, 0);

            /*
             * cyc2ns_shift is exported via arch_perf_update_userpage() where it is
             * not expected to be greater than 31 due to the original published
             * conversion algorithm shifting a 32-bit value (now specifies a 64-bit
             * value) - refer perf_event_mmap_page documentation in perf_event.h.
            */
            if (dev->convert.tsc_conv_fixed.time_shift == 32) {
                dev->convert.tsc_conv_fixed.time_shift = 31;
                dev->convert.tsc_conv_fixed.time_mult >>= 1;
            }
        }
    }
}

static int evtime_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TIME,
        .pinned        = 1,
        .disabled      = 0,
        .watermark     = 0,
        .wakeup_events = 1,
    };
    struct perf_evsel *evsel;
    int id = tep__event_id("syscalls", "sys_enter_getpid");

    if (id < 0) goto failed;

    dev->private = NULL;
    dev->type = PROF_DEV_TYPE_SERVICE;

    attr.config = id;
    evsel = perf_evsel__new(&attr);
    if (!evsel)  goto failed;
    perf_evlist__add(evlist, evsel);

    return 0;
failed:
    return -1;
}

static void evtime_deinit(struct prof_dev *dev)
{
}

static void evtime_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct prof_dev *pdev = dev->private;
    // PERF_SAMPLE_TIME
    struct sample_type_header {
        __u64   time;
    } *data = (void *)event->sample.array;

    pdev->time_ctx.base_evtime = data->time;
}

static profiler evtime = {
    .name = "event-basetime",
    .pages = 1,
    .init = evtime_init,
    .deinit = evtime_deinit,
    .sample = evtime_sample,
};

static void perf_timespec_sync(struct timer *timer)
{
    struct prof_dev *dev = container_of(timer, struct prof_dev, time_ctx.base_timer);
    perf_timespec_init(dev);
}

__ctor static void current_clocksource(void)
{
    char *current_clocksource = NULL;
    size_t size;
    /*
     * LINUX 698eff6355f (sched/clock, x86/perf: Fix "perf test tsc")
     * Only for tsc clocksource. Determine whether the current clocksource is tsc.
     */
    current_clocksource_is_tsc =
        (sysfs__read_str("devices/system/clocksource/clocksource0/current_clocksource",
         &current_clocksource, &size) == 0 && strncmp(current_clocksource, "tsc", 3) == 0);

    if (current_clocksource)
        free(current_clocksource);
}


int perf_timespec_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct perf_mmap *map;
    struct perf_thread_map *tidmap;
    struct env *e = NULL;
    struct prof_dev *evt;

    if (!dev->pages || dev->prof == &evtime)
        return 0;

    if (!(dev->pos.sample_type & PERF_SAMPLE_TIME))
        return 0;

    if (dev->silent)
        return 0;

    perf_evlist__for_each_mmap(evlist, map, dev->env->overwrite) {
        int err = 0;
        perf_event_convert_read_tsc_conversion(dev, map);
        if (dev->convert.need_conv == CONVERT_TO_TSC ||
            dev->convert.need_conv == CONVERT_TO_KVMCLOCK ||
            /*
             * Guest uses kvm-clock source, perf_mmap__read_tsc_conversion() can also return successfully
             * on old kernels, but tsc_conv_fixed() cannot fix the conversion. Therefore, the tsc conversion
             * can only be done when the current clocksource is tsc.
             */
            ((err = perf_mmap__read_tsc_conversion(map, &dev->convert.tsc_conv)) == 0 &&
            current_clocksource_is_tsc)) {
            evclock_t base_evtime;

            base_evtime.tsc = rdtsc();
            clock_gettime(CLOCK_REALTIME, &dev->time_ctx.base_timespec);

            if (base_evtime.tsc > 0) {
                /*
                 * First, tsc -> perfclock; secondly, perfclock -> evclock.
                 *
                 * Simplified:
                 * CONVERT_NONE,        tsc => perfclock + 0.
                 * CONVERT_TO_TSC,      tsc => tsc + clock_offset
                 * CONVERT_TO_KVMCLOCK, tsc => kvmclock + clock_offset.
                 * CONVERT_ADD_OFFSET,  tsc => perfclock + clock_offset.
                 */
                if (!dev->convert.need_conv || dev->convert.need_conv == CONVERT_ADD_OFFSET)
                    base_evtime.perfclock = tsc_to_perfclock(dev, base_evtime.tsc);
                else if (dev->convert.need_conv == CONVERT_TO_KVMCLOCK)
                    base_evtime.kvmclock = host_tsc_to_kvmclock(dev, base_evtime.tsc);

                base_evtime.clock += dev->env->clock_offset;

                if (!timer_started(&dev->time_ctx.base_timer)) {
                    tsc_conv_fixed(dev);
                    /*
                     * There will be a slight difference between tsc_khz and the real frequency.
                     * After a long time, the converted nanoseconds will accumulate a large error.
                     * Therefore, Synchronize base_evtime and base_timespec every 30 seconds.
                     */
                    if (timer_init(&dev->time_ctx.base_timer, 1, perf_timespec_sync) == 0)
                        timer_start(&dev->time_ctx.base_timer, 30 * NSEC_PER_SEC, false);
                }

                dev->time_ctx.base_evtime = evclock_to_real_ns(dev, base_evtime);
                return 0;
            }
        }
        if (err == -EOPNOTSUPP)
            break;
    }

    tidmap = thread_map__new_by_tid(getpid());
    if (!tidmap) goto NULL_tidmap;

    e = zalloc(sizeof(*e)); // free in prof_dev_close()
    if (!e) goto NULL_e;

    evt = prof_dev_open_cpu_thread_map(&evtime, e, NULL, tidmap, NULL);
    e = NULL;
    if (!evt) goto NULL_e;

    evt->private = dev;

    // trigger getpid syscall
    clock_gettime(CLOCK_REALTIME, &dev->time_ctx.base_timespec);
    syscall(SYS_getpid); // syscall does not necessarily occur with getpid().

    prof_dev_flush(evt, PROF_DEV_FLUSH_NORMAL);
    prof_dev_close(evt);

    if (dev->time_ctx.base_evtime == 0) {
        dev->time_ctx.base_timespec.tv_sec = 0;
        dev->time_ctx.base_timespec.tv_nsec = 0;
    } else {
        evclock_t base_evtime = perfclock_to_evclock(dev, dev->time_ctx.base_evtime);
        dev->time_ctx.base_evtime = evclock_to_real_ns(dev, base_evtime);

        if (!timer_started(&dev->time_ctx.base_timer)) {
            // Synchronize base_evtime and base_timespec every 60 seconds.
            // evtime is very slow from open to close, so choose 60s synchronization interval.
            if (timer_init(&dev->time_ctx.base_timer, 1, perf_timespec_sync) == 0)
                timer_start(&dev->time_ctx.base_timer, 60 * NSEC_PER_SEC, false);
        }
    }

NULL_e:
    perf_thread_map__put(tidmap);
NULL_tidmap:
    return dev->time_ctx.base_evtime > 0 ? 0 : -1;
}

/*
 * evsel->external (struct perf_evsel_external)
 *
 * Extends perf_evsel with perf-prof specific data, providing fast reverse
 * lookups from evsel to its owning context:
 *
 *   - tp:           The tp (tracepoint descriptor) associated with this evsel.
 *                   Set by perf_evsel_link_tp() during tp_evsel_new().
 *   - dev:          The prof_dev instance that owns this evsel.
 *                   Set by prof_dev_evsel_external_init() after prof->init().
 *   - member_cache: Describes the sample record layout based on sample_type,
 *                   used by profiler event sources (dev_tp) to access fields
 *                   in forwarded events. Built by perf_event_members().
 *
 * Access helpers: perf_evsel_tp(), perf_evsel_dev(), perf_evsel_member_cache().
 */

int perf_evsel_link_tp(struct perf_evsel *evsel, struct tp *tp)
{
    evsel->external = calloc(1, sizeof(struct perf_evsel_external));
    if (!evsel->external)
        return -1;
    ((struct perf_evsel_external *)evsel->external)->tp = tp;
    return 0;
}

int prof_dev_evsel_external_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct perf_evsel *evsel;
    struct perf_evsel_external *ext;

    if (dev->silent || dev->type == PROF_DEV_TYPE_SERVICE)
        return 0;

    perf_evlist__for_each_evsel(evlist, evsel) {
        /* evsel->external may already be allocated by perf_evsel_link_tp() */
        if (!evsel->external) {
            evsel->external = calloc(1, sizeof(struct perf_evsel_external));
            if (!evsel->external) {
                prof_dev_evsel_external_deinit(dev);
                return -1;
            }
        }
        ext = evsel->external;
        ext->dev = dev;

        /* Initialize member_cache for events with sample_type */
        if (!ext->member_cache) {
            ext->member_cache = perf_event_members(evsel);
        }
    }
    return 0;
}

void prof_dev_evsel_external_deinit(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct perf_evsel *evsel;

    perf_evlist__for_each_evsel(evlist, evsel) {
        if (evsel->external) {
            struct perf_evsel_external *ext = evsel->external;
            if (ext->member_cache)
                free(ext->member_cache);
            free(evsel->external);
            evsel->external = NULL;
        }
    }
}

struct perf_evsel *perf_event_evsel(struct prof_dev *dev, union perf_event *event)
{
    struct perf_evlist *evlist = dev->evlist;
    u64 id;

    if (evlist->nr_entries == 1)
        return perf_evlist__first(evlist);

    if (dev->pos.id_pos < 0) {
        struct perf_evsel *evsel, *first = NULL;

        perf_evlist__for_each_evsel(evlist, evsel) {
            struct perf_event_attr *attr = perf_evsel__attr(evsel);
            if (is_sampling_event(attr)) {
                if (!first)
                    first = evsel;
                else
                    return NULL;
            }
        }
        return first;
    }

    id = *(u64 *)((void *)event->sample.array + dev->pos.id_pos);
    return perf_evlist__id_to_evsel(dev->evlist, id, NULL);
}

/*
 * perf_event_members - Parse sample_type and build member_cache for a perf_evsel.
 *
 * Build an array of perf_event_member describing each field in the sample record,
 * based on attr->sample_type. Fields are listed in kernel-defined order. Fixed-size
 * fields have a known offset; variable-size fields (callchain, raw, branch_stack,
 * stack_user) have size=0 and record their dependencies (deps) on preceding
 * variable-size fields. Use perf_event_member_offset() to resolve the actual offset
 * at runtime.
 *
 * The cache also stores quick-access pointers (id, pid, tid, time, cpu, callchain,
 * raw, branch_stack, stack_user) for commonly used fields.
 *
 * Returns NULL if sample_type is empty or on allocation failure.
 */
struct perf_event_member_cache *perf_event_members(struct perf_evsel *evsel)
{
    struct perf_event_attr *attr = perf_evsel__attr(evsel);
    u64 sample_type = attr->sample_type & (PERF_SAMPLE_MAX - 1);
    struct perf_event_member_cache *cache;
    struct perf_event_member *members;
    int nr_members = 0;
    int i = 0;
    int size;
    int offset = 0;
    unsigned long deps = 0;

    if (!sample_type)
        return NULL;

    /* Count number of members */
    if (sample_type & PERF_SAMPLE_IDENTIFIER) nr_members++;
    if (sample_type & PERF_SAMPLE_IP) nr_members++;
    if (sample_type & PERF_SAMPLE_TID) nr_members += 2;  /* pid and tid are exposed as two members */
    if (sample_type & PERF_SAMPLE_TIME) nr_members++;
    if (sample_type & PERF_SAMPLE_ADDR) nr_members++;
    if (sample_type & PERF_SAMPLE_ID) nr_members++;
    if (sample_type & PERF_SAMPLE_STREAM_ID) nr_members++;
    if (sample_type & PERF_SAMPLE_CPU) nr_members++;
    if (sample_type & PERF_SAMPLE_PERIOD) nr_members++;
    if (sample_type & PERF_SAMPLE_READ) nr_members++;
    if (sample_type & PERF_SAMPLE_CALLCHAIN) nr_members++;
    if (sample_type & PERF_SAMPLE_RAW) nr_members++;
    if (sample_type & PERF_SAMPLE_BRANCH_STACK) nr_members++;
    if (sample_type & PERF_SAMPLE_REGS_USER) nr_members++;
    if (sample_type & PERF_SAMPLE_STACK_USER) nr_members++;
    if (sample_type & PERF_SAMPLE_WEIGHT) nr_members++;
    else if (sample_type & PERF_SAMPLE_WEIGHT_STRUCT) nr_members++;
    if (sample_type & PERF_SAMPLE_DATA_SRC) nr_members++;
    if (sample_type & PERF_SAMPLE_TRANSACTION) nr_members++;
    if (sample_type & PERF_SAMPLE_REGS_INTR) nr_members++;
    if (sample_type & PERF_SAMPLE_PHYS_ADDR) nr_members++;
    if (sample_type & PERF_SAMPLE_AUX) nr_members++;
    if (sample_type & PERF_SAMPLE_CGROUP) nr_members++;
    if (sample_type & PERF_SAMPLE_DATA_PAGE_SIZE) nr_members++;
    if (sample_type & PERF_SAMPLE_CODE_PAGE_SIZE) nr_members++;

    cache = malloc(sizeof(*cache) + nr_members * sizeof(struct perf_event_member));
    if (!cache)
        return NULL;

    memset(cache, 0, sizeof(*cache));
    cache->sample_type = sample_type;
    cache->nr_members = nr_members;
    cache->members = (struct perf_event_member *)(cache + 1);
    members = cache->members;

    /*
     * Sample record layout (after perf_event_header):
     *  { u64           id;       } && PERF_SAMPLE_IDENTIFIER
     *  { u64           ip;       } && PERF_SAMPLE_IP
     *  { u32           pid, tid; } && PERF_SAMPLE_TID
     *  { u64           time;     } && PERF_SAMPLE_TIME
     *  { u64           addr;     } && PERF_SAMPLE_ADDR
     *  { u64           id;       } && PERF_SAMPLE_ID
     *  { u64           stream_id;} && PERF_SAMPLE_STREAM_ID
     *  { u32           cpu, res; } && PERF_SAMPLE_CPU
     *  { u64           period;   } && PERF_SAMPLE_PERIOD
     *  { struct read_format values; } && PERF_SAMPLE_READ
     *  { u64 nr, u64 ips[nr];    } && PERF_SAMPLE_CALLCHAIN
     *  { u32 size; char data[size]; } && PERF_SAMPLE_RAW
     *  { u64 nr; { u64 hw_idx; } && PERF_SAMPLE_BRANCH_HW_INDEX
     *            { u64 from, to, flags } lbr[nr];
     *            { u64 counters; } cntr[nr] && PERF_SAMPLE_BRANCH_COUNTERS
     *  } && PERF_SAMPLE_BRANCH_STACK
     *  { u64 abi; u64 regs[weight(mask)]; } && PERF_SAMPLE_REGS_USER
     *  { u64 size; char data[size]; u64 dyn_size; } && PERF_SAMPLE_STACK_USER
     *  { u64 weight; } && PERF_SAMPLE_WEIGHT / PERF_SAMPLE_WEIGHT_STRUCT
     *  { u64 data_src; } && PERF_SAMPLE_DATA_SRC
     *  { u64 transaction; } && PERF_SAMPLE_TRANSACTION
     *  { u64 abi; u64 regs[weight(mask)]; } && PERF_SAMPLE_REGS_INTR
     *  { u64 phys_addr; } && PERF_SAMPLE_PHYS_ADDR
     *  { u64 size; char data[size]; } && PERF_SAMPLE_AUX
     *  { u64 cgroup; } && PERF_SAMPLE_CGROUP
     *  { u64 data_page_size; } && PERF_SAMPLE_DATA_PAGE_SIZE
     *  { u64 code_page_size; } && PERF_SAMPLE_CODE_PAGE_SIZE
     */

#define ADD_MEMBER(name_str, sz, fmt, doc_str, ...) \
do { \
    if (sample_type & (fmt)) { \
        members[i].name = name_str; \
        members[i].offset = offset; \
        members[i].size = sz; \
        members[i].deps = deps; \
        members[i].doc = doc_str; \
        members[i].format = fmt; \
        members[i].private = NULL; \
        __VA_ARGS__; \
        i++; \
        deps |= (sz == 0 ? (fmt) : 0); \
        offset += sz; \
    } \
} while (0)

    /* Fixed size members in order */
    ADD_MEMBER("identifier", sizeof(u64), PERF_SAMPLE_IDENTIFIER, "Sample identifier", cache->id = &members[i]);
    ADD_MEMBER("ip", sizeof(u64), PERF_SAMPLE_IP, "Instruction pointer");
    ADD_MEMBER("pid", sizeof(u32), PERF_SAMPLE_TID, "Process ID", cache->pid = &members[i]);
    ADD_MEMBER("tid", sizeof(u32), PERF_SAMPLE_TID, "Thread ID", cache->tid = &members[i]);
    ADD_MEMBER("time", sizeof(u64), PERF_SAMPLE_TIME, "Event timestamp", cache->time = &members[i]);
    ADD_MEMBER("addr", sizeof(u64), PERF_SAMPLE_ADDR, "Memory address");
    ADD_MEMBER("id", sizeof(u64), PERF_SAMPLE_ID, "Sample ID", cache->id = &members[i]);
    ADD_MEMBER("stream_id", sizeof(u64), PERF_SAMPLE_STREAM_ID, "Stream ID", cache->id = &members[i]);
    ADD_MEMBER("cpu", sizeof(u32), PERF_SAMPLE_CPU, "CPU number", cache->cpu = &members[i]; offset += 4; /* skip reserved */);
    ADD_MEMBER("period", sizeof(u64), PERF_SAMPLE_PERIOD, "Sample period");

    /* PERF_SAMPLE_READ - Fixed size based on read_format */
    ADD_MEMBER("read", perf_evsel__read_size(evsel), PERF_SAMPLE_READ, "Read format values");

    /* PERF_SAMPLE_CALLCHAIN - variable size */
    ADD_MEMBER("callchain", 0, PERF_SAMPLE_CALLCHAIN, "Call chain", cache->callchain = &members[i]);

    /* PERF_SAMPLE_RAW - variable size */
    ADD_MEMBER("raw", 0, PERF_SAMPLE_RAW, "Raw data", cache->raw = &members[i]);

    /* PERF_SAMPLE_BRANCH_STACK - variable size */
    ADD_MEMBER("branch_stack", 0, PERF_SAMPLE_BRANCH_STACK, "Branch stack", cache->branch_stack = &members[i]);

    /* PERF_SAMPLE_REGS_USER - Fixed size */
    size = sizeof(u64) + hweight64(attr->sample_regs_user) * sizeof(u64);
    ADD_MEMBER("regs_user", 0, PERF_SAMPLE_REGS_USER, "User registers", members[i].size = size; cache->regs_user = &members[i]);

    /* PERF_SAMPLE_STACK_USER - variable size */
    ADD_MEMBER("stack_user", 0, PERF_SAMPLE_STACK_USER, "User stack", cache->stack_user = &members[i]);

    ADD_MEMBER("weight", sizeof(u64), PERF_SAMPLE_WEIGHT, "Sample weight");
    if (!(sample_type & PERF_SAMPLE_WEIGHT))
        ADD_MEMBER("weight", sizeof(u64), PERF_SAMPLE_WEIGHT_STRUCT, "Sample weight struct");
    ADD_MEMBER("data_src", sizeof(u64), PERF_SAMPLE_DATA_SRC, "Data source");
    ADD_MEMBER("transaction", sizeof(u64), PERF_SAMPLE_TRANSACTION, "Transaction");

    /* PERF_SAMPLE_REGS_INTR - Fixed size */
    size = sizeof(u64) + hweight64(attr->sample_regs_intr) * sizeof(u64);
    ADD_MEMBER("regs_intr", 0, PERF_SAMPLE_REGS_INTR, "Interrupt registers", members[i].size = size; cache->regs_intr = &members[i]);

    ADD_MEMBER("phys_addr", sizeof(u64), PERF_SAMPLE_PHYS_ADDR, "Physical address");

    /* PERF_SAMPLE_AUX - variable size */
    ADD_MEMBER("aux", 0, PERF_SAMPLE_AUX, "AUX data", cache->aux = &members[i]);

    ADD_MEMBER("cgroup", sizeof(u64), PERF_SAMPLE_CGROUP, "Cgroup ID");
    ADD_MEMBER("data_page_size", sizeof(u64), PERF_SAMPLE_DATA_PAGE_SIZE, "Data page size");
    ADD_MEMBER("code_page_size", sizeof(u64), PERF_SAMPLE_CODE_PAGE_SIZE, "Code page size");

#undef ADD_MEMBER

    return cache;
}

/*
 * Calculate the offset of a member in a perf_event sample.
 * For variable-size members (callchain, raw, etc.), we need to
 * compute the actual offset based on deps.
 *
 * Each variable-size member's stored offset assumes all prior variable-size
 * members occupy 0 bytes. We accumulate deps_size by reading the actual size
 * of each prior variable-size member, and add it to the stored offset.
 */
int perf_event_member_offset(struct perf_event_member_cache *cache,
                             struct perf_event_member *member,
                             union perf_event *event)
{
    void *data = event->sample.array;
    int deps_size = 0;

    if (!member->deps)
        return member->offset;

    /* PERF_SAMPLE_CALLCHAIN: { u64 nr; u64 ips[nr]; } */
    if (member->deps & PERF_SAMPLE_CALLCHAIN) {
        u64 nr = *(u64 *)(data + cache->callchain->offset + deps_size);
        deps_size += sizeof(u64) + nr * sizeof(u64);
    }

    /* PERF_SAMPLE_RAW: { u32 size; char data[size]; } */
    if (member->deps & PERF_SAMPLE_RAW) {
        u32 size = *(u32 *)(data + cache->raw->offset + deps_size);
        deps_size += sizeof(u32) + size;
    }

    /* PERF_SAMPLE_BRANCH_STACK - { u64 nr; { u64 from, to, flags } lbr[nr]; } */
    if (member->deps & PERF_SAMPLE_BRANCH_STACK) {
        u64 nr = *(u64 *)(data + cache->branch_stack->offset + deps_size);
        deps_size += sizeof(u64) + nr * sizeof(struct perf_branch_entry);
    }

    /* PERF_SAMPLE_REGS_USER: { u64 abi; u64 regs[weight(mask)]; }
     * When abi == 0 (e.g. kernel threads), kernel only writes the abi field. */
    if (member->deps & PERF_SAMPLE_REGS_USER) {
        u64 abi = *(u64 *)(data + cache->regs_user->offset + deps_size);
        deps_size += abi ? cache->regs_user->size : sizeof(u64);
    }

    /* PERF_SAMPLE_STACK_USER: { u64 size; char data[size]; u64 dyn_size; }
     * When size == 0 (e.g. kernel threads), kernel only writes the size field,
     * data[] and dyn_size are not present. */
    if (member->deps & PERF_SAMPLE_STACK_USER) {
        u64 size = *(u64 *)(data + cache->stack_user->offset + deps_size);
        deps_size += sizeof(u64) + (size ? size + sizeof(u64) : 0);
    }

    /* PERF_SAMPLE_REGS_INTR: { u64 abi; u64 regs[weight(mask)]; }
     * When abi == 0, kernel only writes the abi field. */
    if (member->deps & PERF_SAMPLE_REGS_INTR) {
        u64 abi = *(u64 *)(data + cache->regs_intr->offset + deps_size);
        deps_size += abi ? cache->regs_intr->size : sizeof(u64);
    }

    /* PERF_SAMPLE_AUX: { u64 size; char data[size]; } */
    if (member->deps & PERF_SAMPLE_AUX) {
        u64 size = *(u64 *)(data + cache->aux->offset + deps_size);
        deps_size += sizeof(u64) + size;
    }

    return member->offset + deps_size;
}
