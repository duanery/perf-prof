#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <gelf.h>
#include <linux/string.h>
#include <linux/sdt_arg.h>
#include <api/fs/fs.h>
#include <monitor.h>
#include <uprobe_helpers.h>

extern struct env env;

#define SDT_PROBES_SCN ".probes"
#define SDT_BASE_SCN ".stapsdt.base"
#define SDT_NOTE_SCN  ".note.stapsdt"
#define SDT_NOTE_TYPE 3
#define SDT_NOTE_NAME "stapsdt"

struct sdt_probe {
    const char *binpath;
    const char *provider;
    const char *name;
    uint64_t addr;
    uint64_t addr_offset; // Offset from start of file
    uint64_t base_addr;
    uint64_t semaphore;
    uint64_t semaphore_offset; // Offset from start of file
    const char *arg_fmt;
};
typedef void (*sdt_probecb)(struct sdt_probe *probe, void *payload);

static const char *parse_stapsdt_note(struct sdt_probe *probe,
                          GElf_Shdr *probes_shdr, GElf_Off stapsdt_base_offset,
                          const char *desc, int elf_class)
{
    if (elf_class == ELFCLASS32) {
        probe->addr = *((uint32_t *)(desc));
        probe->base_addr = *((uint32_t *)(desc + 4));
        probe->semaphore = *((uint32_t *)(desc + 8));
        desc = desc + 12;
    } else {
        probe->addr = *((uint64_t *)(desc));
        probe->base_addr = *((uint64_t *)(desc + 8));
        probe->semaphore = *((uint64_t *)(desc + 16));
        desc = desc + 24;
    }

    // Offset from start of file
    probe->addr_offset = probe->addr - probe->base_addr + stapsdt_base_offset;

    // Offset from start of file
    if (probe->semaphore && probes_shdr)
        probe->semaphore_offset =
                probe->semaphore - probes_shdr->sh_addr + probes_shdr->sh_offset;
    else
        probe->semaphore_offset = 0;

    probe->provider = desc;
    desc += strlen(desc) + 1;

    probe->name = desc;
    desc += strlen(desc) + 1;

    probe->arg_fmt = desc;
    desc += strlen(desc) + 1;

    return desc;
}

static int do_note_segment(Elf_Scn *section, GElf_Shdr *probes_shdr, int elf_class,
                   sdt_probecb callback, const char *binpath, GElf_Off stapsdt_base_offset,
                   uint64_t first_inst_offset, void *payload) {
    Elf_Data *data = NULL;

    while ((data = elf_getdata(section, data)) != 0) {
        size_t offset = 0;
        GElf_Nhdr hdr;
        size_t name_off, desc_off;

        while ((offset = gelf_getnote(data, offset, &hdr, &name_off, &desc_off)) != 0) {
            const char *desc, *desc_end;
            struct sdt_probe probe = {
                .binpath = binpath,
            };

            if (hdr.n_type != SDT_NOTE_TYPE)
                continue;

            if (hdr.n_namesz != 8)
                continue;

            if (memcmp((const char *)data->d_buf + name_off, SDT_NOTE_NAME, 8) != 0)
                continue;

            desc = (const char *)data->d_buf + desc_off;
            desc_end = desc + hdr.n_descsz;

            if (parse_stapsdt_note(&probe, probes_shdr, stapsdt_base_offset, desc, elf_class) == desc_end) {
                if (probe.addr < first_inst_offset)
                    fprintf(stderr,
                        "WARNING: invalid address 0x%lx for probe (%s,%s) in binary %s\n",
                        probe.addr, probe.provider, probe.name, binpath);
                else
                    callback(&probe, payload);
            }
        }
    }
    return 0;
}

static int listprobes(Elf *e, sdt_probecb callback, const char *binpath,
                      void *payload) {
    Elf_Scn *section = NULL;
    bool found_probes_shdr;
    size_t stridx;
    int elf_class = gelf_getclass(e);
    uint64_t first_inst_offset = 0;
    GElf_Shdr probes_shdr = {};
    GElf_Off stapsdt_base_offset = 0;

    if (elf_getshdrstrndx(e, &stridx) != 0)
        return -1;

    // Get the offset to the first instruction
    while ((section = elf_nextscn(e, section)) != 0) {
        GElf_Shdr header;

        if (!gelf_getshdr(section, &header))
            continue;

        // The elf file section layout is based on increasing virtual address,
        // getting the first section with SHF_EXECINSTR is enough.
        if (header.sh_flags & SHF_EXECINSTR) {
            first_inst_offset = header.sh_addr;
            break;
        }
    }

    section = NULL;
    while ((section = elf_nextscn(e, section)) != 0) {
        GElf_Shdr header;
        char *name;

        if (!gelf_getshdr(section, &header))
            continue;

        if (header.sh_type != SHT_PROGBITS)
            continue;

        name = elf_strptr(e, stridx, header.sh_name);
        if (name && !strcmp(name, SDT_BASE_SCN)) {
            stapsdt_base_offset = header.sh_offset;
            break;
        }
    }

    section = NULL;
    found_probes_shdr = false;
    while ((section = elf_nextscn(e, section)) != 0) {
        char *name;

        if (!gelf_getshdr(section, &probes_shdr))
            continue;

        name = elf_strptr(e, stridx, probes_shdr.sh_name);
        if (name && !strcmp(name, SDT_PROBES_SCN)) {
            found_probes_shdr = true;
            break;
        }
    }

    section = NULL;
    while ((section = elf_nextscn(e, section)) != 0) {
        GElf_Shdr header;
        char *name;

        if (!gelf_getshdr(section, &header))
            continue;

        if (header.sh_type != SHT_NOTE)
            continue;

        name = elf_strptr(e, stridx, header.sh_name);
        if (name && !strcmp(name, SDT_NOTE_SCN)) {
            GElf_Shdr *shdr_ptr = found_probes_shdr ? &probes_shdr : NULL;
            if (do_note_segment(section, shdr_ptr, elf_class, callback, binpath,
                            stapsdt_base_offset, first_inst_offset, payload) < 0)
                return -1;
        }
    }

    return 0;
}

static int elf_foreach_probe(const char *binpath, sdt_probecb callback, void *payload)
{
    int res = -1;
    int fd = -1;
    Elf *e = open_elf(binpath, &fd);;

    if (e) {
        res = listprobes(e, callback, binpath, payload);
        close_elf(e, fd);
    }
    return res;
}

int __weak arch_sdt_arg_parse_op(char *old_op __maybe_unused,
                                 char **new_op __maybe_unused)
{
        return SDT_ARG_SKIP;
}

static const char * const type_to_suffix[] = {
    ":s64", "", "", "", ":s32", "", ":s16", ":s8",
    "", ":u8", ":u16", "", ":u32", "", "", "", ":u64"
};

/*
 * Isolate the string number and convert it into a decimal value;
 * this will be an index to get suffix of the uprobe name (defining
 * the type)
 */
static int sdt_arg_size(char *n_ptr, const char **suffix)
{
    long type_idx;

    type_idx = strtol(n_ptr, NULL, 10);
    if (type_idx < -8 || type_idx > 8) {
        fprintf(stderr, "Failed to get a valid sdt type\n");
        return -1;
    }

    *suffix = type_to_suffix[type_idx + 8];
    return 0;
}

static int sdt_probe_arg(char **pcmd, int i, const char *arg, const char *fmt)
{
    char *op, *desc = strdup(arg), *new_op = NULL;
    const char *suffix = "";
    int ret = -1;

    if (desc == NULL)
        return ret;

    /*
     * Argument is in N@OP format. N is size of the argument and OP is
     * the actual assembly operand. N can be omitted; in that case
     * argument is just OP(without @).
     */
    op = strchr(desc, '@');
    if (op) {
        op[0] = '\0';
        op++;

        if (sdt_arg_size(desc, &suffix))
            goto error;
    } else {
        op = desc;
    }

    ret = arch_sdt_arg_parse_op(op, &new_op);

    if (ret < 0)
        goto error;

    if (ret == SDT_ARG_VALID) {
        if (fmt && strstr(fmt, "%s")) {
            *pcmd = straddf(*pcmd, free, " ");
            if (!*pcmd)
                goto error;
            *pcmd = straddf(*pcmd, free, fmt, new_op);
        } else
            *pcmd = straddf(*pcmd, free, " arg%d=%s%s", i + 1, new_op, suffix);
        if (!*pcmd)
            goto error;
    }

    ret = 0;
error:
    free(desc);
    free(new_op);
    return ret;
}

static char *sdt_probe_command(struct sdt_probe *probe, const char *prefix, const char *arg_fmt)
{
    char *cmd = NULL;
    char **args = NULL;
    char **fmts = NULL;
    int i, args_count, fmts_count;

    cmd = straddf(cmd, free, "%s%s/%s %s:0x%0*llx", prefix,
                probe->provider, probe->name, probe->binpath,
                (int)(sizeof(void *) * 2), probe->addr_offset);
    if (!cmd)
        return NULL;

    /*
     * Kernel constraint: uprobe_events takes the semaphore of an SDT probe as
     * "PATH:OFFSET(REF_CTR_OFFSET)" and increments it while the probe is
     * enabled, __trace_uprobe_create() in kernel/trace/trace_uprobe.c. Since
     * 1cc33161a83d ("uprobes: Support SDT markers having reference count
     * (semaphore)"), in v4.20. Before that the offset fails to parse and the
     * write is rejected: the error is printed and the probe is not created.
     */
    if (probe->semaphore) {
        cmd = straddf(cmd, free, "(0x%llx)", probe->semaphore_offset);
        if (!cmd)
            return NULL;
    }

    if (probe->arg_fmt[0] == '\0')
        goto out;

    if (arg_fmt)
        fmts = argv_split(arg_fmt, &fmts_count);

    args = argv_split(probe->arg_fmt, &args_count);
    for (i = 0; i < args_count && (!fmts || i < fmts_count); ++i) {
        if (sdt_probe_arg(&cmd, i, args[i], fmts ? fmts[i] : NULL) < 0)
            goto error;
    }
out:
    if (args)
        argv_free(args);
    if (fmts)
        argv_free(fmts);
    return cmd;

error:
    if (cmd) {
        free(cmd);
        cmd = NULL;
    }
    goto out;
}

static void uprobe_events(struct sdt_probe *probe, void *payload, const char *prefix)
{
    struct sdt_probe *p = payload;
    const char *uprobe_trace = "kernel/debug/tracing/uprobe_events";
    char *uprobe = NULL;
    int ret;

    if (p->provider &&
        strcmp(probe->provider, p->provider) != 0)
        return ;

    if (p->name &&
        strcmp(probe->name, p->name) != 0)
        return ;

    if (p->addr && probe->addr != p->addr)
        return ;

    uprobe = sdt_probe_command(probe, prefix, p->arg_fmt);
    if (uprobe) {
        printf("%s:%s@%s\n", probe->provider, probe->name, probe->binpath);
        if (env.verbose)
            printf("    echo '%s' > %s/%s\n", uprobe, sysfs__mountpoint(), uprobe_trace);
        ret = sysfs__write_str(uprobe_trace, uprobe, strlen(uprobe));
        if (ret < 0)
            fprintf(stderr, "    echo '%s' > %s/%s failed, %d(%s)\n", uprobe, sysfs__mountpoint(),
                            uprobe_trace, ret, strerror(-ret));
        free(uprobe);
    }
}

static void create(struct sdt_probe *probe, void *payload)
{
    uprobe_events(probe, payload, "p:");
}

static void delete(struct sdt_probe *probe, void *payload)
{
    uprobe_events(probe, payload, "-:");
}

static void list(struct sdt_probe *probe, void *payload)
{
    struct sdt_probe *p = payload;
    char *uprobe = NULL;

    if (p->provider &&
        strcmp(probe->provider, p->provider) != 0)
        return ;

    if (p->name &&
        strcmp(probe->name, p->name) != 0)
        return ;

    if (p->addr && probe->addr != p->addr)
        return ;

    printf("%s:%s@%s\n", probe->provider, probe->name, probe->binpath);

    if (env.verbose) {
        printf("    Addr: 0x%016lx, Base: 0x%016lx, Semaphore: 0x%016lx\n", probe->addr,
                    probe->base_addr, probe->semaphore);
        printf("    Arguments: %s\n", probe->arg_fmt);

        uprobe = sdt_probe_command(probe, "p:", p->arg_fmt);
        printf("    Uprobe: %s\n", uprobe ? : "");
        free(uprobe);
    }
}

static int usdt_argc_init(int argc, char *argv[])
{
    struct sdt_probe usdt = {};
    char *cmd = NULL;
    sdt_probecb cb = NULL;
    int i;

    if (argc < 1) {
        fprintf(stderr, " One of {add|del|list} needs to be specified.\n");
        help();
    } else {
        cmd = argv[0];
        if (strcmp(cmd, "add") == 0)
            cb = create;
        else if (strcmp(cmd, "del") == 0)
            cb = delete;
        else if (strcmp(cmd, "list") == 0)
            cb = list;
        else
            help();
    }

    if (argc < 2) {
        fprintf(stderr, " 'binpath' needs to be specified.\n");
        help();
    }

    for (i = 1; i < argc; i++) {
        char *s = strdup(argv[i]);
        char *f = s;
        char *sep = s;

        memset(&usdt, 0, sizeof(usdt));
        if ((sep = strchr(s, ':'))) {
            usdt.provider = s;
            *sep = '\0';
            s = sep + 1;
        }
        if ((sep = strchr(s, '@'))) {
            usdt.name = s;
            *sep = '\0';
            s = sep + 1;
        }
        if (*s)
            usdt.binpath = s;
        else {
            fprintf(stderr, " binpath is empty.\n");
            help();
        }
        if ((sep = strchr(s, '@'))) {
            *sep = '\0';
            s = sep + 1;
            if (*s)
                usdt.addr = strtoll(s, NULL, 0);
        }
        if ((sep = strchr(s, ' '))) {
            *sep = '\0';
            s = sep + 1;
            usdt.arg_fmt = s;
        }

        elf_foreach_probe(usdt.binpath, cb, &usdt);

        free(f);
    }

    exit(0);
}

static const char *usdt_desc[] = PROFILER_DESC("usdt",
    "[OPTION...] {add|del|list} [[profider:]name@]binpath[@addr][ args] ...",
    "User Statically-Defined Tracing.", "",
    "SYNOPSIS",
    "    Find the location of the static trace point from the .note.stapsdt section",
    "    of the elf file, and add the kernel uprobe event.", "",
    "FORMAT",
    "      [[profider:]name@]binpath[@addr][ args]",
    "    profider:name is optional. binpath is required. @addr is optional, used to",
    "    select a specific one among multiple profider:name. args is optional, use",
    "    '%s' to format each arg, see EXAMPLES.", "",
    "EXAMPLES",
    "    "PROGRAME" usdt list /usr/lib64/libc.so.6",
    "    "PROGRAME" usdt add /usr/lib64/libc.so.6",
    "    "PROGRAME" usdt del /usr/lib64/libc.so.6",
    "    "PROGRAME" usdt list /lib64/libpython3.6m.so.1.0 -v",
    "    "PROGRAME" usdt add 'python:function__entry@/lib64/libpython3.6m.so.1.0 filename=+0(%s):string funcname=+0(%s):string'");
static const char *usdt_argv[] = PROFILER_ARGV("usdt",
    "OPTION:",
    "version", "verbose", "quiet", "help"
);
static profiler usdt = {
    .name = "usdt",
    .desc = usdt_desc,
    .argv = usdt_argv,
    .pages = 2,
    .argc_init = usdt_argc_init,
};
PROFILER_REGISTER(usdt);

