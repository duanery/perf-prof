#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pager.h>
#include <linux/zalloc.h>
#include <api/fs/tracing_path.h>
#include <monitor.h>
#include <bpf_event.h>

#define for_each_subsystem(sys_dir, sys_dirent)         \
    while ((sys_dirent = readdir(sys_dir)) != NULL)     \
        if (sys_dirent->d_type == DT_DIR &&     \
            (strcmp(sys_dirent->d_name, ".")) &&    \
            (strcmp(sys_dirent->d_name, "..")))

static int tp_event_has_id(const char *dir_path, struct dirent *evt_dir)
{
    char evt_path[MAXPATHLEN];
    int fd;

    snprintf(evt_path, MAXPATHLEN, "%s/%s/id", dir_path, evt_dir->d_name);
    fd = open(evt_path, O_RDONLY);
    if (fd < 0)
        return -EINVAL;
    close(fd);

    return 0;
}

#define for_each_event(dir_path, evt_dir, evt_dirent)       \
    while ((evt_dirent = readdir(evt_dir)) != NULL)     \
        if (evt_dirent->d_type == DT_DIR &&     \
            (strcmp(evt_dirent->d_name, ".")) &&    \
            (strcmp(evt_dirent->d_name, "..")) &&   \
            (!tp_event_has_id(dir_path, evt_dirent)))


static int cmp_string(const void *a, const void *b)
{
    const char * const *as = a;
    const char * const *bs = b;

    return strcmp(*as, *bs);
}

/*
 * The BPF event sources, `bpf:name', join the tracepoints of @evt_list, which
 * has room for them. A source hides a tracepoint of the same name, as in
 * parse_event_head(). Returns the new number of events.
 */
static int add_bpf_events(char **evt_list, int evt_num)
{
    struct bpf_event_source *src;
    int i, n = 0;

    for (i = 0; i < evt_num; i++) {
        if (strncmp(evt_list[i], "bpf:", 4) == 0 && bpf_event_source_find(evt_list[i] + 4))
            zfree(&evt_list[i]);
        else
            evt_list[n++] = evt_list[i];
    }
    for (src = bpf_event_source_next(NULL); src; src = bpf_event_source_next(src)) {
        const char *name = bpf_event_source_name(src);

        evt_list[n] = malloc(strlen("bpf:") + strlen(name) + 1);
        if (!evt_list[n])
            break;
        sprintf(evt_list[n++], "bpf:%s", name);
    }
    return n;
}

/*
 * List the events, sorted: those from <debugfs_mount_point>/tracing/events,
 * and the BPF events. Hand them to @cb, or print them if NULL.
 */
void list_events(list_events_cb cb, void *opaque)
{
    DIR *sys_dir, *evt_dir;
    struct dirent *sys_dirent, *evt_dirent;
    char evt_path[MAXPATHLEN];
    char *dir_path;
    char **evt_list = NULL;
    unsigned int evt_i = 0, evt_num = 0, nr_bpf = 0;
    bool evt_num_known = false;
    struct bpf_event_source *src;

    for (src = bpf_event_source_next(NULL); src; src = bpf_event_source_next(src))
        nr_bpf++;

restart:
    sys_dir = tracing_events__opendir();
    if (!sys_dir) {
        // No tracefs: the BPF events still are.
        evt_num = 0;
        evt_list = zalloc(sizeof(char *) * (nr_bpf + 1));
        if (!evt_list)
            return;
        goto out_list;
    }

    if (evt_num_known) {
        evt_list = zalloc(sizeof(char *) * (evt_num + nr_bpf));
        if (!evt_list)
            goto out_close_sys_dir;
    }

    for_each_subsystem(sys_dir, sys_dirent) {
        dir_path = get_events_file(sys_dirent->d_name);
        if (!dir_path)
            continue;
        evt_dir = opendir(dir_path);
        if (!evt_dir)
            goto next;

        for_each_event(dir_path, evt_dir, evt_dirent) {
            if (!evt_num_known) {
                evt_num++;
                continue;
            }

            snprintf(evt_path, MAXPATHLEN, "%s:%s",
                    sys_dirent->d_name, evt_dirent->d_name);

            evt_list[evt_i] = strdup(evt_path);
            if (evt_list[evt_i] == NULL) {
                put_events_file(dir_path);
                goto out_close_evt_dir;
            }
            evt_i++;
        }
        closedir(evt_dir);
next:
        put_events_file(dir_path);
    }
    closedir(sys_dir);

    if (!evt_num_known) {
        evt_num_known = true;
        goto restart;
    }

out_list:
    evt_num = add_bpf_events(evt_list, evt_num);
    qsort(evt_list, evt_num, sizeof(char *), cmp_string);

    if (cb)
        cb(evt_list, evt_num, opaque);
    else {
        evt_i = 0;
        while (evt_i < evt_num) {
            printf("%s\n", evt_list[evt_i]);
            evt_i++;
        }
    }

out_free:
    for (evt_i = 0; evt_i < evt_num; evt_i++)
        zfree(&evt_list[evt_i]);
    zfree(&evt_list);
    return;

out_close_evt_dir:
    closedir(evt_dir);
out_close_sys_dir:
    closedir(sys_dir);

    evt_num = evt_i;
    if (evt_list)
        goto out_free;
}

static void event_match(char **evt_list, int evt_num, void *opaque)
{
    int i;
    char *e = opaque;

    for (i = 0; i < evt_num; i++)
        if (strstr(evt_list[i], e))
            printf("%s\n", evt_list[i]);
}

static int list_argc_init(int argc, char *argv[])
{
    setup_pager();
    if (argc == 0)
        list_events(NULL, NULL);
    else
        list_events(event_match, argv[0]);
    exit(0);
}

static const char *list_desc[] = PROFILER_DESC("list",
    "[OPTION...] event",
    "List all tracepoint events and BPF events.",
    "",
    "SYNOPSIS",
    "    Print the events from <debugfs_mount_point>/tracing/events, and the BPF",
    "    events built into perf-prof, bpf:name.",
    "",
    "EXAMPLES",
    "    "PROGRAME" list");
static const char *list_argv[] = PROFILER_ARGV("list",
    "OPTION:",
    "version", "verbose", "quiet", "help"
);
static profiler list = {
    .name = "list",
    .desc = list_desc,
    .argv = list_argv,
    .pages = 0,
    .argc_init = list_argc_init,
};
PROFILER_REGISTER(list);

