#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <linux/hashtable.h>
#include <linux/thread_map.h>
#include <monitor.h>

/*
 * Performance evaluation
 *
 * Sampling events can have a performance impact. Events are attached to the CPU, which
 * will cause the CPU utilization to increase. Events attached to thread context will
 * cause thread utilization to increase, and calls to __perf_event_task_sched_in()/_out()
 * will also cause more increases.
 *
 * In addition, event filtering also causes more increases. The more filter logic operations
 * there are, the more increases.
 *
 * On the trend, the more events are sampled, the more the utilization increases. Therefore,
 * the performance evaluation is to count the number of events sampled in the cpu/thread
 * context. Above a certain limit, sampling is disabled.
 */

#define PERFEVAL_HASHBITS (6)

struct perfeval_node {
    struct hlist_node node;
    u32 cpu_tid; // cpu or tid;
    u64 samples;
};
#define perfeval_node_add(hashtable, obj, key) \
        obj->cpu_tid = (key); \
        hlist_add_head(&obj->node, &hashtable[hash_min((key), PERFEVAL_HASHBITS)])

#define perfeval_node_find(hashtable, obj, key) \
        hlist_for_each_entry(obj, &hashtable[hash_min((key), PERFEVAL_HASHBITS)], node) \
            if (obj->cpu_tid == (key))

#define perfeval_node_for_each(hashtable, obj, bkt) \
        for ((bkt) = 0, obj = NULL; obj == NULL && (bkt) < (1 << PERFEVAL_HASHBITS); (bkt)++) \
            hlist_for_each_entry(obj, &hashtable[bkt], node)


void perfeval_sample(struct prof_dev *dev, union perf_event *event, int cpu, int tid)
{
    struct perfeval_node *node;
    u32 cpu_tid[2] = {0};
    int i;

    if (likely(!(dev->perfeval[0].hashmap || dev->perfeval[1].hashmap)))
        return;

    if (event->header.type == PERF_RECORD_DEV) {
        cpu_tid[0] = ((struct perf_record_dev *)event)->cpu;
        cpu_tid[1] = ((struct perf_record_dev *)event)->tid;
    } else {
        if (dev->perfeval[0].hashmap)
            cpu_tid[0] = *(u32 *)((void *)event->sample.array + dev->perfeval[0].mem_pos);
        if (dev->perfeval[1].hashmap)
            cpu_tid[1] = *(u32 *)((void *)event->sample.array + dev->perfeval[1].mem_pos);
    }

    // Count events that occurred in the specified cpu/tid context.
    for (i = 0; i < 2; i ++) {
        struct performance_evaluation *perfeval = &dev->perfeval[i];
        if (perfeval->hashmap) {
            perfeval->sampled_events ++;
            perfeval_node_find(perfeval->hashmap, node, cpu_tid[i]) {
                if (node->samples++ == 0)
                    perfeval->nr_ins ++;
                perfeval->matched_events ++;
                break;
            }
        }
    }
}

void perfeval_evaluate(struct prof_dev *dev)
{
    const char *str[2] = {"cpu", "tid"};
    struct perfeval_node *node;
    int i, j;

    if (likely(!(dev->perfeval[0].hashmap || dev->perfeval[1].hashmap)))
        return;

    for (i = 0; i < 2; i ++) {
        struct performance_evaluation *perfeval = &dev->perfeval[i];
        if (perfeval->hashmap && perfeval->sampled_events) {
            long sampled_interval = perfeval->sampled_events * 1000 / dev->env->interval;
            long matched_interval = perfeval->matched_events * 1000 / dev->env->interval;
            bool disable = false;

            // Exceeding the limit, exit perf-prof.
            if (dev->env->sampling_limit && matched_interval / perfeval->nr_ins > dev->env->sampling_limit) {
                disable = true;
                prof_dev_close(dev);
            }

            print_time(stdout);
            printf("%s: perfeval %s(%s): sampled %lu events matched %lu on %d instances%s\n", dev->prof->name,
                        i == 0 ? "cpus" : "pids", i == 0 ? dev->env->perfeval_cpus : dev->env->perfeval_pids,
                        sampled_interval, matched_interval, perfeval->nr_ins,
                        disable ? ", exceeds the limit, exit" : "");
            perfeval_node_for_each(perfeval->hashmap, node, j) {
                if (node->samples) {
                    if (disable) {
                        print_time(stdout);
                        printf("%s: perfeval: %s %u sampled %lu events\n", dev->prof->name, str[i],
                                    node->cpu_tid, node->samples);
                    }
                    node->samples = 0;
                }
            }
            perfeval->nr_ins = 0;
            perfeval->matched_events = 0;
            perfeval->sampled_events = 0;
        }
    }
}

void perfeval_free(struct prof_dev *dev)
{
    struct env *env = dev->env;

    if (!env->interval ||
        !(env->perfeval_cpus || env->perfeval_pids))
        return ;

    if (dev->perfeval[0].hashmap)
        free(dev->perfeval[0].hashmap);
    if (dev->perfeval[1].hashmap)
        free(dev->perfeval[1].hashmap);
}

int perfeval_init(struct prof_dev *dev)
{
    struct env *env = dev->env;
    struct prof_dev *source, *tmp;
    struct perf_cpu_map *cpus = NULL;
    struct perf_thread_map *threads = NULL;
    struct performance_evaluation *perfeval;
    struct hlist_head *hash;
    struct perfeval_node *node;
    int idx, cpu, thread;
    int tid_pos = dev->pos.tid_pos;
    int cpu_pos = dev->pos.cpu_pos;

    if (!env->interval ||
        !(env->perfeval_cpus || env->perfeval_pids))
        return 0;

    if (env->perfeval_cpus) {
        if (cpu_pos < 0)
            goto out_free;
        // Check the cpu_pos of the forwarding source device.
        for_each_source_dev_get(source, tmp, dev)
            if (source->pos.cpu_pos < 0 ||
                cpu_pos != sizeof(u32)+sizeof(u32)+sizeof(u64)+sizeof(u64) /* struct perf_record_dev cpu pos */) {
                prof_dev_put(source);
                goto out_free;
            }

        perfeval = &dev->perfeval[0];
        perfeval->mem_pos = cpu_pos;
        cpus = perf_cpu_map__new(env->perfeval_cpus);
        perfeval->hashmap = zalloc((sizeof(struct hlist_head) << PERFEVAL_HASHBITS) +
                                    sizeof(*node) * perf_cpu_map__nr(cpus));
        if (!cpus || !perfeval->hashmap)
            goto out_free;

        hash = perfeval->hashmap;
        node = (void *)(hash + (1 << PERFEVAL_HASHBITS));
        __hash_init(hash, 1 << PERFEVAL_HASHBITS);
        perf_cpu_map__for_each_cpu(cpu, idx, cpus) {
            perfeval_node_add(hash, node, cpu);
            node ++;
        }
    }

    if (env->perfeval_pids) {
        if (tid_pos < 0)
            goto out_free;
        // Check the tid_pos of the forwarding source device.
        for_each_source_dev_get(source, tmp, dev)
            if (source->pos.tid_pos < 0 ||
                tid_pos != sizeof(u32) /* struct perf_record_dev tid pos */) {
                prof_dev_put(source);
                goto out_free;
            }

        perfeval = &dev->perfeval[1];
        perfeval->mem_pos = tid_pos + sizeof(u32); // tid
        threads = thread_map__new_str(env->perfeval_pids, NULL, 0, 0);
        perfeval->hashmap = zalloc((sizeof(struct hlist_head) << PERFEVAL_HASHBITS) +
                                    sizeof(*node) * perf_thread_map__nr(threads));
        if (!threads || !perfeval->hashmap)
            goto out_free;

        hash = perfeval->hashmap;
        node = (void *)(hash + (1 << PERFEVAL_HASHBITS));
        __hash_init(hash, 1 << PERFEVAL_HASHBITS);
        perf_thread_map__for_each_thread(thread, idx, threads) {
            perfeval_node_add(hash, node, thread);
            node ++;
        }
    }

    perf_cpu_map__put(cpus);
    perf_thread_map__put(threads);
    return 0;

out_free:
    fprintf(stderr, "perfeval init failed\n");
    perf_cpu_map__put(cpus);
    perf_thread_map__put(threads);
    perfeval_free(dev);
    return -1;
}

