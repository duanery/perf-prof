/*
 * Clone the new prof_dev under the control of ptrace.
 *
 * For prof_dev, after it is attached to a process (-p <pid>), there is no way to
 * track the newly forked child process. However, we can use ptrace to control the
 * forked child process(tracee), which is initially in a STOP state, and can notify
 * it to CONTINUE at the appropriate time.
 *
 * The main idea is:
 *    When in the STOP state, clone a new prof_dev for the tracee.
 *    Then, notify it to CONTINUE.
 *
 *    When tracee exits, close its prof_dev.
 *
 * Initially, ptrace is used to trace the fork, vfork, and clone syscalls.
 * And re-inject all delivered signals.
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <linux/rblist.h>
#include <linux/thread_map.h>
#include <sys/ptrace.h>
#include <linux/ptrace.h>
#include <monitor.h>

struct pid_node {
    struct rb_node rbnode;
    int pid, ppid;
    int initial;
    bool detach, workload;

    struct list_head dev_list; // link &struct pid_link_dev

    int refcount;
    struct pid_node *parent;
    struct list_head initial_child_list;
    struct list_head initial_link;
};

struct pid_link_dev {
    struct pid_node *pid;
    struct list_head link_to_pid;
    struct prof_dev *dev;
    struct list_head link_to_dev;
};

static void put_pid(struct pid_node *p, int pid);
static void del_thread_tree(struct prof_dev *dev, pid_t pid);

static int __ptrace_link(struct pid_node *node, struct prof_dev *dev)
{
    struct pid_link_dev *link;

    list_for_each_entry(link, &dev->ptrace_list, link_to_dev) {
        if (link->pid == node)
            return 1;
    }

    link = malloc(sizeof(*link));
    if (link && dev && prof_dev_use(dev)) {
        link->pid = node;
        list_add(&link->link_to_pid, &node->dev_list);
        link->dev = dev;
        list_add(&link->link_to_dev, &dev->ptrace_list);
        return 1;
    }
    if (link) free(link);
    return 0;
}

static void __ptrace_unlink_free(struct pid_link_dev *link)
{
    struct prof_dev *dev;
    pid_t pid;

    if (!link)
        return;
    dev = link->dev;
    pid = link->pid ? link->pid->pid : -1;

    list_del(&link->link_to_pid);
    list_del(&link->link_to_dev);
    free(link);

    /*
     * Drain this thread before dropping the ptrace use. unuse() of the
     * last link closes the device; del_thread must run while the evlist
     * is still alive. Skip when already closing: close -> detach walks
     * this list and must not re-enter del_thread.
     */
    if (dev && dev->evlist && pid >= 0 && !dev->inclose)
        del_thread_tree(dev, pid);
    prof_dev_unuse(dev);
}

static void __ptrace_unlink(struct pid_node *node)
{
    struct pid_link_dev *link;

    while (!list_empty(&node->dev_list)) {
        link = list_first_entry(&node->dev_list, struct pid_link_dev, link_to_pid);
        __ptrace_unlink_free(link);
    }
}

static int __node_cmp(struct rb_node *rbn, const void *entry)
{
    struct pid_node *b = container_of(rbn, struct pid_node, rbnode);
    const struct pid_node *e = entry;

    return b->pid - e->pid;
}
static struct rb_node *__node_new(struct rblist *rlist, const void *new_entry)
{
    struct pid_node *b = malloc(sizeof(*b));
    const struct pid_node *e = new_entry;
    if (b) {
        memset(b, 0, sizeof(*b));
        RB_CLEAR_NODE(&b->rbnode);
        b->pid = e->pid;
        b->initial = e->initial;
        b->workload = e->workload;
        b->refcount = 1;
        INIT_LIST_HEAD(&b->dev_list);
        INIT_LIST_HEAD(&b->initial_child_list);
        INIT_LIST_HEAD(&b->initial_link);
        return &b->rbnode;
    }
    return NULL;
}
static void __node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct pid_node *b = container_of(rb_node, struct pid_node, rbnode);
    __ptrace_unlink(b);
    list_del_init(&b->initial_link);
    if (b->parent)
        put_pid(b->parent, 0);
    if (b->refcount != 0 || !list_empty(&b->initial_child_list))
        fprintf(stderr, "BUG: initial_child_list not empty\n");
    free(b);
}

struct rblist pid_list = {
    .entries = RB_ROOT_CACHED,
    .node_cmp = __node_cmp,
    .node_new = __node_new,
    .node_delete = __node_delete,
};

static struct pid_node *new_find_pid(int pid)
{
    struct pid_node node = {.initial = 0, .workload = 0};
    struct pid_node *p;

    node.pid = pid;
    p = rb_entry_safe(rblist__findnew(&pid_list, &node), struct pid_node, rbnode);
    return p;
}

static struct pid_node *get_pid(struct pid_node *p, int pid)
{
    if (!p) {
        struct pid_node node = {.pid = pid};
        struct rb_node *rbn = rblist__find(&pid_list, &node);
        p = rb_entry_safe(rbn, struct pid_node, rbnode);
    }
    if (p) {
        p->refcount++;
    }
    return p;
}

static void put_pid(struct pid_node *p, int pid)
{
    if (!p) {
        struct pid_node node = {.pid = pid};
        struct rb_node *rbn = rblist__find(&pid_list, &node);
        p = rb_entry_safe(rbn, struct pid_node, rbnode);
    }
    if (p && --p->refcount == 0) {
        rblist__remove_node(&pid_list, &p->rbnode);
    }
}

int ptrace_attach(struct perf_thread_map *thread_map, struct prof_dev *dev)
{
    struct pid_node node;
    int pid;
    int idx;
    bool workload;
    int succ = 0;
    int ret;

    if (prof_dev_is_cloned(dev))
        return 0;
    if (!prof_dev_use(dev))
        return 0;

    workload = dev->env->workload.pid > 0;
    node.initial = 2;
    node.workload = workload;
    perf_thread_map__for_each_thread(pid, idx, thread_map) {
        struct rb_node *rbn;
        struct pid_node *p;

        node.pid = pid;
        rbn = rblist__find(&pid_list, &node);
        if (rbn)
            goto skip_ptrace;

        /*
         * The pid can only be attached to one tracer. If this pid is attached
         * to gdb, or strace, or other perf-prof, etc. PTRACE_SEIZE returns -1.
         */
        ret = ptrace(PTRACE_SEIZE, pid, 0,
                         (workload ? PTRACE_O_EXITKILL : 0) | // tracer exits, SIGKILL workload(every tracee).
                         PTRACE_O_TRACEFORK |  // trace fork
                         PTRACE_O_TRACEVFORK | // trace vfork
                         PTRACE_O_TRACECLONE | // trace clone
                         //PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT |
                         PTRACE_O_TRACESYSGOOD);

        if (ret < 0) {
            // On failure, skip it and continue with other pids.
            // Don't return -1.
        } else
            rbn = rblist__findnew(&pid_list, &node);

    skip_ptrace:
        p = rb_entry_safe(rbn, struct pid_node, rbnode);
        if (p && __ptrace_link(p, dev)) {
            succ ++;
            // p->detach == 1: attach immediately after detach.
            // See prof_dev_open_internal() reinit.
            p->detach = 0;

            // p->initial == 1: the initial ptrace-stop new child.
            // p->workload != workload: BUG
            if (!p->workload && workload)
                fprintf(stderr, "BUG: PTRACE_O_EXITKILL flag is not "
                                "set for workload(%d).\n", p->pid);
        }
    }
    if (!succ)
        fprintf(stderr, "ptrace fails for all pids.\n");

    // If all ptrace returns fail, prof_dev will be closed.
    prof_dev_unuse(dev);
    return 0;
}

void ptrace_detach(struct prof_dev *dev)
{
    struct pid_link_dev *link;
    struct pid_node *node, *initial;
    struct prof_dev *child, *tmp;

    /*
     * When prof_dev is closed in advance(i.e. -N 10), ptrace_list is not
     * empty and detach is performed.
     * But for daemonize, its parent process will exit normally and
     * ptrace_list is empty.
     */
    if (list_empty(&dev->ptrace_list))
        return;

    prof_dev_get(dev);

    while (!list_empty(&dev->ptrace_list)) {
        link = list_first_entry(&dev->ptrace_list, struct pid_link_dev, link_to_dev);
        node = link->pid;
        __ptrace_unlink_free(link);

        if (!node || !list_empty(&node->dev_list))
            continue;

        if (!node->detach) {
            node->detach = 1;
            if (node->workload) {
                // node is free within ptrace_exited().
                kill(node->pid, SIGKILL); // PTRACE_KILL is deprecated; do not use it!
            } else {
                // node is free on this path:
                //    __new_or_interrupt() ->
                //        __detach()
                if (ptrace(PTRACE_INTERRUPT, node->pid, 0, 0) < 0) {
                    /*
                     * PTRACE 32261 FORK 32307
                     *         CONT 32261 signo 0
                     * CHILD 32261 EXITED return 0  # put_pid(NULL, 32261) has been called,
                     *                              # but it is not free and is referenced
                     *                              # by 32307.
                     * PTRACE_INTERRUPT 32261       # errno = ESRCH
                     * PTRACE 32307 PTRACE_INTERRUPT | SIGCONT | initial new
                     *         DETACH 32307 signo 0
                     */
                    // ESRCH is the only errno.
                    if (errno != ESRCH)
                        fprintf(stderr, "BUG: PTRACE_INTERRUPT return %d !ESRCH\n", errno);
                } else
                    d_printf("PTRACE_INTERRUPT %d\n", node->pid);
            }

            list_for_each_entry(initial, &node->initial_child_list, initial_link) {
                initial->detach = 1;
                if (initial->workload)
                    kill(initial->pid, SIGKILL);
                else {
                    // Dont need PTRACE_INTERRUPT
                    // IN initial STOP
                    d_printf("MAYBE-STOP %d\n", initial->pid);
                }
            }
        }
    }

    // Also close all prof_dev cloned from 'dev'.
    for_each_child_dev_get(child, tmp, dev) {
        if (child->clone && !list_empty(&child->ptrace_list))
            ptrace_detach(child);
    }

    prof_dev_put(dev);
}

void ptrace_print(struct prof_dev *dev, int indent)
{
    struct pid_link_dev *link;
    struct pid_node *node, *initial;

    if (list_empty(&dev->ptrace_list))
        return;

    dev_printf("ptrace:");
    list_for_each_entry(link, &dev->ptrace_list, link_to_dev) {
        node = link->pid;
        printf(" %d", node->pid);
        list_for_each_entry(initial, &node->initial_child_list, initial_link) {
            printf("/%d", initial->pid);
        }
    }
    printf("\n");
}

bool ptrace_detach_done(void)
{
    return rblist__empty(&pid_list);
}

enum {
    CONT = 0,
    SKIP_CONT = 1,
    KEEP_STOP = 1,
};

static inline void __cont(int pid)
{
    ptrace(PTRACE_CONT, pid, 0, 0);
    d_printf("        CONT %d signo 0\n", pid);
}

static int __detach(struct pid_node *node)
{
    if (unlikely(node->workload))
        fprintf(stderr, "BUG: The workload cannot be detached.\n");
    else {
        ptrace(PTRACE_DETACH, node->pid, 0, 0);
        d_printf("        DETACH %d signo 0\n", node->pid);
        put_pid(node, 0);
    }

    return SKIP_CONT;
}

static int add_thread_tree(struct prof_dev *dev, pid_t pid)
{
    struct prof_dev *child, *tmp;
    int err = 0;

    if (!prof_dev_ins_oncpu(dev)) {
        err = prof_dev_add_thread(dev, pid);
        if (err)
            return err;
    }
    if (dev->prof->add_thread)
        dev->prof->add_thread(dev, pid);
    for_each_child_dev_get(child, tmp, dev)
        add_thread_tree(child, pid);
    return err;
}

static void del_thread_tree(struct prof_dev *dev, pid_t pid)
{
    struct prof_dev *child, *tmp;

    for_each_child_dev_get(child, tmp, dev)
        del_thread_tree(child, pid);
    if (dev->prof->del_thread)
        dev->prof->del_thread(dev, pid);
    if (!prof_dev_ins_oncpu(dev))
        prof_dev_del_thread(dev, pid);
}

static int __fork__new(struct pid_node *child)
{
    struct pid_node *parent = child->parent;

    list_del_init(&child->initial_link);

    if (child->detach)
        return __detach(child);

    if (parent) {
        struct pid_link_dev *link;

        list_for_each_entry(link, &parent->dev_list, link_to_pid) {
            if (add_thread_tree(link->dev, child->pid) < 0)
                continue;
            if (!__ptrace_link(child, link->dev))
                del_thread_tree(link->dev, child->pid);
        }
        put_pid(parent, 0);
        child->parent = NULL;
    }
    if (unlikely(list_empty(&child->dev_list)))
        return __detach(child);

    __cont(child->pid);
    return SKIP_CONT;
}

static int __fork(int ppid, int pid)
{
    struct pid_node *parent, *child;

    parent = get_pid(NULL, ppid);
    child = new_find_pid(pid);

    // parent
    if (unlikely(!parent)) {
        ptrace(PTRACE_DETACH, ppid, 0, 0);
        d_printf("        DETACH %d signo 0\n", ppid);
        fprintf(stderr, "BUG: The parent node is freed.\n");
    } else {
        /*
         * Save one PTRACE_INTERRUPT for parent.
         *
         * PTRACE_INTERRUPT 12101
         * PTRACE 12142 PTRACE_INTERRUPT | SIGCONT | initial new
         * PTRACE 12101 CLONE 12142
         *         DETACH 12101 signo 0   # not CONT, get parent
         *         DETACH 12142 signo 0   # put parent
         *
         * PTRACE_INTERRUPT 97252
         * PTRACE 97252 CLONE 97257
         *         DETACH 97252 signo 0   # not CONT, get parent
         * PTRACE 97257 PTRACE_INTERRUPT | SIGCONT | initial new
         *         DETACH 97257 signo 0   # put parent
         */
        if (parent->detach)
            __detach(parent);
        else
            __cont(ppid);
    }

    // child
    if (unlikely(!child)) {
        if (parent) put_pid(parent, 0);
        fprintf(stderr, "BUG: Cannot alloc the child node.\n");
    } else {
        child->ppid = ppid;
        /*
         * PTRACE_INTERRUPT 23024        # 20234 detach=1, in ptrace_detach().
         * PTRACE 23028 PTRACE_INTERRUPT | SIGCONT | initial new
         * PTRACE 23024 CLONE 23028      # 23028 detach=1
         *         CONT 23024 signo 0
         *         DETACH 23028 signo 0  # DETACH, not CONT
         * PTRACE 23024 PTRACE_INTERRUPT | SIGCONT | initial new
         *         DETACH 23024 signo 0
         */
        child->detach = parent ? parent->detach : 1;
        child->workload = parent ? parent->workload : 0;
        /*
         * Reference parent->dev to prevent it from being closed.
         *
         * After fork, the parent process may exit before the child process,
         * close prof_dev and free pid_node in ptrace_exited(). Within __fork(),
         * the parent process is in a stopped state (see PTRACE_EVENT stops)
         * and it is impossible to exit. Therefore, it is safe to reference
         * parent->dev.
         *
         *   PTRACE 17645 FORK 17646
         *           CONT 17645 signo 0
         *   CHILD 17645 EXITED return 0
         *   PTRACE 17646 PTRACE_INTERRUPT | SIGCONT | initial new
         *           CONT 17646 signo 0
         *
         * For the same reason, 'workload' and detach' also need to be passed
         * to the child node in advance.
         */
        child->parent = parent;
        /*
         * PTRACE 17645 FORK 17646
         *         CONT 17645 signo 0
         * PTRACE 17645 FORK 17647
         *         CONT 17645 signo 0
         * PTRACE 17645 FORK 17648
         *         CONT 17645 signo 0
         * PTRACE_INTERRUPT 17645     # detach 17645 17646 17647 17648
         * PTRACE 17645 PTRACE_INTERRUPT | SIGCONT | initial new
         *         DETACH 17645 signo 0
         * PTRACE 17646 PTRACE_INTERRUPT | SIGCONT | initial new
         *         DETACH 17646 signo 0
         * ...
         */
        if (parent)
            list_add_tail(&child->initial_link, &parent->initial_child_list);

        child->initial ++;
        if (child->initial == 2)
            __fork__new(child);
    }

    return SKIP_CONT;
}

static int __new_or_interrupt(int pid)
{
    struct pid_node *curr = new_find_pid(pid);

    if (unlikely(!curr)) {
        fprintf(stderr, "BUG: Cannot alloc the child node.\n");
        ptrace(PTRACE_DETACH, pid, 0, 0);
        d_printf("        DETACH %d signo 0\n", pid);
        return SKIP_CONT;
    }

    if (curr->parent) {
        /*
         * PTRACE 14002 CLONE 14010      # 14010 detach=0
         *        CONT 14002 signo 0
         * PTRACE_INTERRUPT 14002        # 14002 detach=1, 14010 detach = 1
         * PTRACE 14010 PTRACE_INTERRUPT | SIGCONT | initial new
         *         DETACH 14010 signo 0
         * CHILD 14002 EXITED return 0
         */
        if (curr->detach != curr->parent->detach)
            fprintf(stderr, "BUG: detach of parent and child is not equal.\n");
    }

    // PTRACE_INTERRUPT, SIGCONT
    if (curr->initial == 2) {
        if (curr->detach)
            return __detach(curr);
        // SIGCONT
        return CONT;
    }

    // initial new
    curr->initial ++;
    if (curr->initial == 2)
        return __fork__new(curr);

    return KEEP_STOP;
}

void ptrace_exited(int pid)
{
    put_pid(NULL, pid);
}

int ptrace_stop(int pid, int status)
{
    unsigned long pid_forked, pid_oldns, exit_code, syscall;
    const char * __maybe_unused str = NULL;
    int signo = 0;
    int ret = CONT;
    int cont = PTRACE_CONT;

    switch (status) {
    // PTRACE_EVENT stops
    case SIGTRAP | (PTRACE_EVENT_FORK  << 8): str = "FORK";  goto FORK;
    case SIGTRAP | (PTRACE_EVENT_VFORK << 8): str = "VFORK"; goto FORK;
    case SIGTRAP | (PTRACE_EVENT_CLONE << 8): str = "CLONE"; FORK:
        ptrace(PTRACE_GETEVENTMSG, pid, 0, &pid_forked);
        d_printf("PTRACE %d %s %ld\n", pid, str, pid_forked);
        ret = __fork(pid, pid_forked);
        break;
    case SIGTRAP | (PTRACE_EVENT_VFORK_DONE << 8):
        ptrace(PTRACE_GETEVENTMSG, pid, 0, &pid_forked);
        d_printf("PTRACE %d VFORK_DONE %ld\n", pid, pid_forked);
        break;
    case SIGTRAP | (PTRACE_EVENT_EXEC << 8):
        ptrace(PTRACE_GETEVENTMSG, pid, 0, &pid_oldns);
        d_printf("PTRACE %d EXEC NSPID %ld\n", pid, pid_oldns);
        break;
    case SIGTRAP | (PTRACE_EVENT_EXIT << 8):
        ptrace(PTRACE_GETEVENTMSG, pid, 0, &exit_code);
        d_printf("PTRACE %d EXIT CODE %ld\n", pid, exit_code);
        break;
    case SIGTRAP | (PTRACE_EVENT_STOP << 8):
        d_printf("PTRACE %d PTRACE_INTERRUPT | SIGCONT | initial new\n", pid);
        ret = __new_or_interrupt(pid);
        break;

    // Syscall-stops
    case SIGTRAP | 0x80 /* PTRACE_O_TRACESYSGOOD */:
        // new kernel
        ptrace(PTRACE_GETEVENTMSG, pid, 0, &syscall);
        d_printf("PTRACE %d SYSCALL %s\n", pid, syscall == PTRACE_EVENTMSG_SYSCALL_ENTRY ? "ENTRY" : "EXIT");
        break;

    // Group-stop
    case SIGSTOP | (PTRACE_EVENT_STOP << 8): str = "SIGSTOP"; goto STOP;
    case SIGTSTP | (PTRACE_EVENT_STOP << 8): str = "SIGTSTP"; goto STOP;
    case SIGTTIN | (PTRACE_EVENT_STOP << 8): str = "SIGTTIN"; goto STOP;
    case SIGTTOU | (PTRACE_EVENT_STOP << 8): str = "SIGTTOU"; STOP:
        d_printf("PTRACE %d %s stop\n", pid, str);
        /*
         * If cont is PTRACE_CONT, pid cannot be STOP.
         * Only PTRACE_LISTEN, `kill -SIGCONT pid` can actually wake it up.
         */
        cont = PTRACE_LISTEN;
        break;

    // Signal-delivery-stop
    default:
        signo = status;
        if (unlikely(signo & (PTRACE_EVENT_STOP << 8))) {
            d_printf("PTRACE %d unknown STOP\n", pid);
            signo = 0;
            break;
        }
        d_printf("PTRACE %d == inject %d == \n", pid, signo);
        signo &= 0xff;
        break;
    }

    if (ret == CONT) {
        ptrace(cont, pid, 0, signo);
        d_printf("        %s %d signo %d\n", cont == PTRACE_CONT ? "CONT" : "LISTEN", pid, signo);
    }
    return 0;
}

