/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LIBPERF_MMAP_H
#define __LIBPERF_MMAP_H

#include <linux/types.h>
#include <sys/types.h>
#include <stdbool.h>
#include <perf/core.h>

struct perf_mmap;
union perf_event;

LIBPERF_API int perf_mmap__idx(struct perf_mmap *map);
/*
 * The binding of the ring buffer. A ring buffer is bound either to a cpu
 * (perf_mmap__cpu() >= 0, perf_mmap__tid() == -1) or to a thread
 * (perf_mmap__cpu() == -1, perf_mmap__tid() >= 0).
 */
LIBPERF_API int perf_mmap__cpu(struct perf_mmap *map);
LIBPERF_API pid_t perf_mmap__tid(struct perf_mmap *map);
LIBPERF_API bool perf_mmap__oncpu(struct perf_mmap *map);
LIBPERF_API void perf_mmap__consume(struct perf_mmap *map);
LIBPERF_API int perf_mmap__read_init(struct perf_mmap *map);
LIBPERF_API void perf_mmap__read_done(struct perf_mmap *map);
LIBPERF_API union perf_event *perf_mmap__read_event(struct perf_mmap *map, bool *writable);
LIBPERF_API void perf_mmap__unread_event(struct perf_mmap *map, union perf_event *event);

struct perf_tsc_conversion {
	u16 time_shift;
	u32 time_mult;
	u64 time_zero;
	bool cap_user_time_zero;
};
LIBPERF_API int perf_mmap__read_tsc_conversion(struct perf_mmap *map, struct perf_tsc_conversion *tc);


#endif /* __LIBPERF_MMAP_H */
