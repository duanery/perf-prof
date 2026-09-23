#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <linux/zalloc.h>
#include <linux/rblist.h>
#include <linux/time64.h>
#include <monitor.h>
#include <stack_helpers.h>
#include <latency_helpers.h>

struct ldlat_ctx {
    struct latency_dist *lat_dist;
    struct callchain_ctx *ccx;
};

static int monitor_ctx_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct ldlat_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    if (get_cpu_vendor() != X86_VENDOR_INTEL) {
        fprintf(stderr, "Only supports Intel platforms\n");
        free(ctx);
        return -1;
    }

    ctx->lat_dist = latency_dist_new(env->perins, true, 0);
    ctx->ccx = callchain_ctx_new(CALLCHAIN_KERNEL | CALLCHAIN_USER, stdout);
    callchain_ctx_config(ctx->ccx, 1, 1, 1, 0, 0, '\n', '\n');

    return 0;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct ldlat_ctx *ctx = dev->private;
    latency_dist_free(ctx->lat_dist);
    callchain_ctx_free(ctx->ccx);
    free(ctx);
}

static int ldlat_loads_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_RAW,
        .config        = 0x1cd,  //MEM_TRANS_RETIRED.* /sys/bus/event_source/devices/cpu/events/mem-loads
        .size          = sizeof(struct perf_event_attr),
        //Every trigger_freq memory load, the PEBS hardware triggers an assist and causes a PEBS record to be written
        .sample_period = env->trigger_freq ?: 1000,
        .sample_type   = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR | PERF_SAMPLE_CPU |
                         PERF_SAMPLE_WEIGHT | PERF_SAMPLE_DATA_SRC | PERF_SAMPLE_PHYS_ADDR,
        .read_format   = 0,
        .pinned        = 0,
        .disabled      = 1,
        .exclude_guest = env->exclude_guest,
        .exclude_host = env->exclude_host,
        .precise_ip    = 3, // enable PEBS
        .config1       = env->ldlat <= 0 ? 3 : env->ldlat, // MSR_PEBS_LD_LAT_THRESHOLD MSR
        .watermark     = 1,
        .wakeup_watermark = (dev->pages << 12) / 2, // enable large PEBS, PERF_X86_EVENT_LARGE_PEBS
    };
    struct perf_evsel *evsel;

    if (monitor_ctx_init(dev) < 0)
        return -1;

    prof_dev_env2attr(dev, &attr);

    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        goto failed;
    }
    perf_evlist__add(evlist, evsel);
    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void ldlat_loads_interval(struct prof_dev *dev);
static void ldlat_loads_exit(struct prof_dev *dev)
{
    ldlat_loads_interval(dev);
    monitor_ctx_exit(dev);
}

// in linux/perf_event.h
// PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR | PERF_SAMPLE_CPU |
// PERF_SAMPLE_WEIGHT | PERF_SAMPLE_DATA_SRC | PERF_SAMPLE_PHYS_ADDR
struct sample_type_header {
    __u64         ip;
    struct {
        __u32    pid;
        __u32    tid;
    }    tid_entry;
    __u64     time;
    __u64     addr;
    struct {
        __u32    cpu;
        __u32    reserved;
    }    cpu_entry;
    union perf_sample_weight weight;
    __u64			data_src;
    __u64			phys_addr;
};

struct mem_info {
    union perf_mem_data_src data_src;
};

static const char * const tlb_access[] = {
	"N/A",
	"HIT",
	"MISS",
	"L1",
	"L2",
	"Walker",
	"Fault",
};

static int perf_mem__tlb_scnprintf(char *out, size_t sz, struct mem_info *mem_info)
{
	size_t l = 0, i;
	u64 m = PERF_MEM_TLB_NA;
	u64 hit, miss;

	sz -= 1; /* -1 for null termination */
	out[0] = '\0';

	if (mem_info)
		m = mem_info->data_src.mem_dtlb;

	hit = m & PERF_MEM_TLB_HIT;
	miss = m & PERF_MEM_TLB_MISS;

	/* already taken care of */
	m &= ~(PERF_MEM_TLB_HIT|PERF_MEM_TLB_MISS);

	for (i = 0; m && i < ARRAY_SIZE(tlb_access); i++, m >>= 1) {
		if (!(m & 0x1))
			continue;
		if (l) {
			strcat(out, " or ");
			l += 4;
		}
		l += scnprintf(out + l, sz - l, tlb_access[i]);
	}
	if (*out == '\0')
		l += scnprintf(out, sz - l, "N/A");
	if (hit)
		l += scnprintf(out + l, sz - l, " hit");
	if (miss)
		l += scnprintf(out + l, sz - l, " miss");

	return l;
}

static const char * const mem_lvl[] = {
	"N/A",
	"HIT",
	"MISS",
	"L1",
	"LFB",
	"L2",
	"L3",
	"Local RAM",
	"Remote RAM (1 hop)",
	"Remote RAM (2 hops)",
	"Remote Cache (1 hop)",
	"Remote Cache (2 hops)",
	"I/O",
	"Uncached",
};

static const char * const mem_lvlnum[] = {
	[PERF_MEM_LVLNUM_ANY_CACHE] = "Any cache",
	[PERF_MEM_LVLNUM_LFB] = "LFB",
	[PERF_MEM_LVLNUM_RAM] = "RAM",
	[PERF_MEM_LVLNUM_PMEM] = "PMEM",
	[PERF_MEM_LVLNUM_NA] = "N/A",
};

static int perf_mem__lvl_scnprintf(char *out, size_t sz, struct mem_info *mem_info)
{
	size_t i, l = 0;
	u64 m =  PERF_MEM_LVL_NA;
	u64 hit, miss;
	int printed;

	if (mem_info)
		m  = mem_info->data_src.mem_lvl;

	sz -= 1; /* -1 for null termination */
	out[0] = '\0';

	hit = m & PERF_MEM_LVL_HIT;
	miss = m & PERF_MEM_LVL_MISS;

	/* already taken care of */
	m &= ~(PERF_MEM_LVL_HIT|PERF_MEM_LVL_MISS);


	if (mem_info && mem_info->data_src.mem_remote) {
		strcat(out, "Remote ");
		l += 7;
	}

	printed = 0;
	for (i = 0; m && i < ARRAY_SIZE(mem_lvl); i++, m >>= 1) {
		if (!(m & 0x1))
			continue;
		if (printed++) {
			strcat(out, " or ");
			l += 4;
		}
		l += scnprintf(out + l, sz - l, mem_lvl[i]);
	}

	if (mem_info && mem_info->data_src.mem_lvl_num) {
		int lvl = mem_info->data_src.mem_lvl_num;
		if (printed++) {
			strcat(out, " or ");
			l += 4;
		}
		if (mem_lvlnum[lvl])
			l += scnprintf(out + l, sz - l, mem_lvlnum[lvl]);
		else
			l += scnprintf(out + l, sz - l, "L%d", lvl);
	}

	if (l == 0)
		l += scnprintf(out + l, sz - l, "N/A");
	if (hit)
		l += scnprintf(out + l, sz - l, " hit");
	if (miss)
		l += scnprintf(out + l, sz - l, " miss");

	return l;
}

static const char * const snoop_access[] = {
	"N/A",
	"None",
	"Hit",
	"Miss",
	"HitM",
};

static int perf_mem__snp_scnprintf(char *out, size_t sz, struct mem_info *mem_info)
{
	size_t i, l = 0;
	u64 m = PERF_MEM_SNOOP_NA;

	sz -= 1; /* -1 for null termination */
	out[0] = '\0';

	if (mem_info)
		m = mem_info->data_src.mem_snoop;

	for (i = 0; m && i < ARRAY_SIZE(snoop_access); i++, m >>= 1) {
		if (!(m & 0x1))
			continue;
		if (l) {
			strcat(out, " or ");
			l += 4;
		}
		l += scnprintf(out + l, sz - l, snoop_access[i]);
	}
	if (mem_info &&
	     (mem_info->data_src.mem_snoopx & PERF_MEM_SNOOPX_FWD)) {
		if (l) {
			strcat(out, " or ");
			l += 4;
		}
		l += scnprintf(out + l, sz - l, "Fwd");
	}

	if (*out == '\0')
		l += scnprintf(out, sz - l, "N/A");

	return l;
}

static int perf_mem__lck_scnprintf(char *out, size_t sz, struct mem_info *mem_info)
{
	u64 mask = PERF_MEM_LOCK_NA;
	int l;

	if (mem_info)
		mask = mem_info->data_src.mem_lock;

	if (mask & PERF_MEM_LOCK_NA)
		l = scnprintf(out, sz, "N/A");
	else if (mask & PERF_MEM_LOCK_LOCKED)
		l = scnprintf(out, sz, "Yes");
	else
		l = scnprintf(out, sz, "No");

	return l;
}

static int perf_mem__blk_scnprintf(char *out, size_t sz, struct mem_info *mem_info)
{
	size_t l = 0;
	u64 mask = PERF_MEM_BLK_NA;

	sz -= 1; /* -1 for null termination */
	out[0] = '\0';

	if (mem_info)
		mask = mem_info->data_src.mem_blk;

	if (!mask || (mask & PERF_MEM_BLK_NA)) {
		l += scnprintf(out + l, sz - l, " N/A");
		return l;
	}
	if (mask & PERF_MEM_BLK_DATA)
		l += scnprintf(out + l, sz - l, " Data");
	if (mask & PERF_MEM_BLK_ADDR)
		l += scnprintf(out + l, sz - l, " Addr");

	return l;
}

static int perf_script__meminfo_scnprintf(char *out, size_t sz, struct mem_info *mem_info)
{
	int i = 0;

	i += perf_mem__lvl_scnprintf(out, sz, mem_info);
	if (mem_info->data_src.mem_snoop) {
		i += scnprintf(out + i, sz - i, "|SNP ");
		i += perf_mem__snp_scnprintf(out + i, sz - i, mem_info);
	}
	if (mem_info->data_src.mem_dtlb) {
		i += scnprintf(out + i, sz - i, "|TLB ");
		i += perf_mem__tlb_scnprintf(out + i, sz - i, mem_info);
	}
	if (mem_info->data_src.mem_lock) {
		i += scnprintf(out + i, sz - i, "|LCK ");
		i += perf_mem__lck_scnprintf(out + i, sz - i, mem_info);
	}
	if (mem_info->data_src.mem_blk) {
		i += scnprintf(out + i, sz - i, "|BLK ");
		i += perf_mem__blk_scnprintf(out + i, sz - i, mem_info);
	}
	return i;
}

static void ldlat_print_node(void *opaque, struct latency_node *node)
{
    struct prof_dev *dev = opaque;
    int oncpu = prof_dev_oncpu(dev);
    struct mem_info mem_info;
    char buf[128];

    mem_info.data_src.val = node->key;
    perf_script__meminfo_scnprintf(buf, sizeof(buf), &mem_info);

    if (dev->env->perins) {
        if (oncpu)
            printf("[%03d] ", prof_binding_cpu(node->instance));
        else
            printf("%-8d ", prof_binding_tid(node->instance));
    }
    printf("%-60s %8lu %16lu %12lu %12lu %12lu\n", buf,
        node->n, node->sum, node->min, node->sum/node->n, node->max);
}

static void ldlat_loads_interval(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct ldlat_ctx *ctx = dev->private;
    int i;
    int oncpu = prof_dev_oncpu(dev);

    if (latency_dist_empty(ctx->lat_dist))
        return ;

    print_time(stdout);
    printf("\n");

    if (env->perins)
        printf(oncpu ? "[CPU] " : "[THREAD] ");

    printf("%-60s %8s %16s %12s %12s %12s\n", "Mem Load Latency",
                 "samples", "total(cycles)", "min(cycles)", "avg(cycles)", "max(cycles)");

    if (env->perins)
        printf(oncpu ? "----- " : "-------- ");
    for (i=0; i<60; i++) printf("-");
    printf(" %8s %16s %12s %12s %12s\n",
                    "--------", "----------------", "------------", "------------", "------------");

    latency_dist_print(ctx->lat_dist, ldlat_print_node, dev);
    return ;
}

static void ldlat_loads_print_event(struct prof_dev *dev, union perf_event *event, int cpu, int tid, int flags)
{
    struct ldlat_ctx *ctx = dev->private;
    struct sample_type_header *data = (void *)event->sample.array;
    struct mem_info mem_info;
    char buf[64];
    struct {
        __u64 nr;
        __u64 ips[2];
    } callchain;

    callchain.nr = 2;
    callchain.ips[0] = data->ip >= START_OF_KERNEL ? PERF_CONTEXT_KERNEL : PERF_CONTEXT_USER;
    callchain.ips[1] = data->ip;

    mem_info.data_src.val = data->data_src;
    perf_mem__lvl_scnprintf(buf, sizeof(buf), &mem_info);

    if (!(flags & OMIT_TIMESTAMP))
        prof_dev_print_time(dev, data->time, stdout);

    printf("    pid %6u tid %6u [%03d] %llu.%06llu: %s: DATA ADDR %016llx PHYS %016llx latency %6llu cycles %s RIP ",
            data->tid_entry.pid, data->tid_entry.tid, data->cpu_entry.cpu,
            data->time / NSEC_PER_SEC, (data->time % NSEC_PER_SEC)/1000, dev->prof->name,
            data->addr, data->phys_addr, data->weight.full, buf);
    print_callchain(ctx->ccx, (struct callchain *)&callchain, data->tid_entry.pid);
}

static void ldlat_loads_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    u64 binding = prof_binding_key(cpu, tid);

    struct env *env = dev->env;
    struct ldlat_ctx *ctx = dev->private;
    struct sample_type_header *data = (void *)event->sample.array;

    if (env->verbose || (env->greater_than &&
        data->weight.full > env->greater_than))
        ldlat_loads_print_event(dev, event, cpu, tid, 0);

    latency_dist_input(ctx->lat_dist, binding, data->data_src, data->weight.full, env->greater_than);
}

static const char *ldlat_loads_desc[] = PROFILER_DESC("ldlat-loads",
    "[OPTION...] [--ldlat cycles] [-T trigger] [--perins] [--than cycles]",
    "Count the retired load latency on Intel Platform.", "",
    "SYNOPSIS",
    "    PMU: MEM_TRANS_RETIRED.*", "",
    "EXAMPLES",
    "    "PROGRAME" ldlat-loads -C 0 -i 1000",
    "    "PROGRAME" ldlat-loads -p 2347 --ldlat 10 --than 100 -i 1000");
static const char *ldlat_loads_argv[] = PROFILER_ARGV("ldlat-loads",
    PROFILER_ARGV_OPTION,
    "FILTER OPTION:",
    "exclude-host", "exclude-guest",
    PROFILER_ARGV_PROFILER, "ldlat", "trigger", "perins", "than\nGreater than specified cycles");

//PEBS
//18.3.4.4.2 Load Latency Performance Monitoring Facility
static profiler ldlat_loads = {
    .name = "ldlat-loads",
    .desc = ldlat_loads_desc,
    .argv = ldlat_loads_argv,
    .pages = 32, // 2x PEBS_BUFFER_SIZE, in kernel PEBS_BUFFER_SIZE=(PAGE_SIZE << 4)
    .init = ldlat_loads_init,
    .deinit = ldlat_loads_exit,
    .interval = ldlat_loads_interval,
    .print_event = ldlat_loads_print_event,
    .sample = ldlat_loads_sample,
};
PROFILER_REGISTER(ldlat_loads);


static int ldlat_stores_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_RAW,
        .config        = 0x82d0, //MEM_INST_RETIRED.ALL_STORES /sys/bus/event_source/devices/cpu/events/mem-stores
        .size          = sizeof(struct perf_event_attr),
        //Every trigger_freq memory load, the PEBS hardware triggers an assist and causes a PEBS record to be written
        .sample_period = env->trigger_freq ?: 1000,
        .sample_type   = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR | PERF_SAMPLE_CPU |
                         PERF_SAMPLE_WEIGHT | PERF_SAMPLE_DATA_SRC | PERF_SAMPLE_PHYS_ADDR,
        .read_format   = 0,
        .pinned        = 0,
        .disabled      = 1,
        .exclude_guest = env->exclude_guest,
        .exclude_host = env->exclude_host,
        .precise_ip    = 3, // enable PEBS
        .watermark     = 1,
        .wakeup_watermark = (dev->pages << 12) / 2, // enable large PEBS, PERF_X86_EVENT_LARGE_PEBS
    };
    struct perf_evsel *evsel;

    if (monitor_ctx_init(dev) < 0)
        return -1;

    prof_dev_env2attr(dev, &attr);

    evsel = perf_evsel__new(&attr);
    if (!evsel) {
        goto failed;
    }
    perf_evlist__add(evlist, evsel);
    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}


static const char *ldlat_stores_desc[] = PROFILER_DESC("ldlat-stores",
    "[OPTION...] [-T trigger] [--perins]",
    "Count the retired store instructions on Intel Platform.", "",
    "SYNOPSIS",
    "    PMU: MEM_INST_RETIRED.ALL_STORES", "",
    "EXAMPLES",
    "    "PROGRAME" ldlat-stores -C 0 -i 1000",
    "    "PROGRAME" ldlat-stores -p 2347 -v -i 1000");
static const char *ldlat_stores_argv[] = PROFILER_ARGV("ldlat-stores",
    PROFILER_ARGV_OPTION,
    "FILTER OPTION:",
    "exclude-host", "exclude-guest",
    PROFILER_ARGV_PROFILER, "trigger", "perins", "than\nGreater than specified cycles");
static profiler ldlat_stores = {
    .name = "ldlat-stores",
    .desc = ldlat_stores_desc,
    .argv = ldlat_stores_argv,
    .pages = 32, // 2x PEBS_BUFFER_SIZE, in kernel PEBS_BUFFER_SIZE=(PAGE_SIZE << 4)
    .init = ldlat_stores_init,
    .deinit = ldlat_loads_exit,
    .interval = ldlat_loads_interval,
    .print_event = ldlat_loads_print_event,
    .sample = ldlat_loads_sample,
};
PROFILER_REGISTER(ldlat_stores);
