#include <stdlib.h>
#include <pthread.h>
#include <linux/bitops.h>
#include <asm/perf_regs.h>
#include <linux/hw_breakpoint.h>
#include <linux/hashtable.h>
#include <monitor.h>
#include <stack_helpers.h>
#include <trace_helpers.h>

#if defined(__i386__) || defined(__x86_64__)
#define REG_NOSUPPORT_N 4
#define REG_NOSUPPORT ((1ULL << PERF_REG_X86_DS) | \
		       (1ULL << PERF_REG_X86_ES) | \
		       (1ULL << PERF_REG_X86_FS) | \
		       (1ULL << PERF_REG_X86_GS))
#if defined(__i386__)
#define PERF_REGS_MASK (((1ULL << PERF_REG_X86_32_MAX) - 1) & ~REG_NOSUPPORT)
#else
#define PERF_REGS_MASK (((1ULL << PERF_REG_X86_64_MAX) - 1) & ~REG_NOSUPPORT)
#endif

#include <asm/insn.h>

#elif defined(__aarch64__)
#define PERF_REGS_MASK ((1ULL << PERF_REG_ARM64_MAX) - 1)
#endif

#define FILTER_VAR_NAME "data"

#define HBP_NUM 4

static profiler breakpoint;

struct hw_breakpoint {
    unsigned long address;
    u8 len;
    u8 type;
    char typestr[4];
};

struct insn_decode_ctxt {
    struct hw_breakpoint *bp;
    u64 addr;
    union {
        struct {
            u64 pad1;
            u64 data;
            u64 pad2;
        };
        u8  bytes[24];
    };
    /*
     * Data is safe: all write instructions are decoded.
     * MOV: Safe.
     * ADD: Unsafe, unless a safe instruction occurs before ADD.
     */
    bool safety;
};

struct insn_decode_node {
    struct hlist_node node;
    u64 insn_ip;
    u8  insn_buff[15];
    int insn_pos;
    char *insn_str;
};
#define INSN_HASHTABLE_BITS 6

static struct hw_breakpoint hwbp[HBP_NUM];
struct breakpoint_ctx {
    struct hw_breakpoint hwbp[HBP_NUM];
    struct insn_decode_ctxt ctxt[HBP_NUM];
    DECLARE_HASHTABLE(insn_hashmap, INSN_HASHTABLE_BITS);
    struct expr_prog *data_filter;
    struct callchain_ctx *print_ip;
    struct callchain_ctx *cc;
    struct flame_graph *flame;
    bool timely_sym;
    bool print_event;
    bool sample;
    bool kcore;
};

#define insn_node_add(hashtable, obj, key) \
        obj->insn_ip = (key); \
        hlist_add_head(&obj->node, &hashtable[hash_min((key), INSN_HASHTABLE_BITS)])

#define insn_node_find(hashtable, obj, key) \
        hlist_for_each_entry(obj, &hashtable[hash_min((key), INSN_HASHTABLE_BITS)], node) \
            if (obj->insn_ip == (key))


static int monitor_ctx_init(struct prof_dev *dev)
{
    int i;
    struct env *env = dev->env;
    struct breakpoint_ctx *ctx = zalloc(sizeof(*ctx));
    if (!ctx)
        return -1;
    dev->private = ctx;

    for (i = 0; i < HBP_NUM; i++) {
        ctx->hwbp[i] = hwbp[i];
        if (ctx->hwbp[i].type == HW_BREAKPOINT_W) {
            ctx->kcore = 1;
            ctx->ctxt[i].bp = &ctx->hwbp[i];
            /*
             * We need to ensure the order of write instructions
             * so that we can safely obtain all written values.
             */
            env->order = 1;
        }
    }

    if (ctx->kcore) {
        if (env->filter) {
            struct global_var_declare data_var[2] = {{FILTER_VAR_NAME, 0, sizeof(u64), sizeof(u64), 1}, {NULL}};
            ctx->data_filter = expr_compile(env->filter, data_var);
            if (!ctx->data_filter) {
                fprintf(stderr, "Please use the 'data' variable. E.g. \"data > 0\"\n");
                goto failed;
            }
        }
        kcore_ref();
        hash_init(ctx->insn_hashmap);
    }

    if (env->callchain) {
        ctx->cc = callchain_ctx_new(callchain_flags(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER), stdout);
        if (env->flame_graph)
            ctx->flame = flame_graph_open(callchain_flags(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER), env->flame_graph);
        dev->pages *= 2;
    }

    ctx->print_ip = callchain_ctx_new(CALLCHAIN_KERNEL | CALLCHAIN_USER, stdout);
    callchain_ctx_config(ctx->print_ip, 0, 1, 1, 0, 0, '\n', '\n');

    tep__ref();
    return 0;

failed:
    free(ctx);
    return -1;
}

static void monitor_ctx_exit(struct prof_dev *dev)
{
    struct breakpoint_ctx *ctx = dev->private;
    tep__unref();
    if (ctx->kcore) {
        struct insn_decode_node *obj;
        struct hlist_node *tmp;
        int bkt;
        hash_for_each_safe(ctx->insn_hashmap, bkt, tmp, obj, node) {
            if (obj->insn_str)
                free(obj->insn_str);
            free(obj);
        }

        kcore_unref();
        expr_destroy(ctx->data_filter);
    }
    callchain_ctx_free(ctx->print_ip);
    callchain_ctx_free(ctx->cc);
    flame_graph_output(ctx->flame);
    flame_graph_close(ctx->flame);
    free(ctx);
}

static int breakpoint_argc_init(int argc, char *argv[])
{
    int i, j;

    if (argc < 1) {
        fprintf(stderr, " <addr> needs to be specified.\n");
        help();
    }
    if (argc > HBP_NUM) {
        fprintf(stderr, " Up to %d breakpoints are supported.\n", HBP_NUM);
        help();
    }

    for (i = 0; i < argc; i++) {
        char *p, *s = strdup(argv[i]);
        int tk = *s;
        unsigned long value = tk - '0';
        u8 len = 1;
        u8 type = HW_BREAKPOINT_W;

        p = s + 1;
        if (value) {
            while (*p >= '0' && *p <= '9') value = value * 10 + *p++ - '0';
        } else if (*p == 'x' || *p == 'X') {
            while ((tk = *++p) && ((tk >= '0' && tk <= '9') || (tk >= 'a' && tk <= 'f') || (tk >= 'A' && tk <= 'F')))
                value = value * 16 + (tk & 15) + (tk >= 'A' ? 9 : 0);
        } else {
            fprintf(stderr, " <addr> is not decimal or hexadecimal.");
            help();
        }

        if (*p == '/') {
            ++p;
            if (*p >= '0' && *p <= '9')
                len = *p++ - '0';
        }
        if (*p == ':') {
            ++p;
            if (*p) type = 0;
            while (*p) {
                if (*p == 'r') type |=  HW_BREAKPOINT_R;
                else if (*p == 'w') type |= HW_BREAKPOINT_W;
                else if (*p == 'x') type |= HW_BREAKPOINT_X;
                else break;
                p++;
            }
        }
        if (*p) {
            fprintf(stderr, " <addr> parsing error.");
            help();
        }

        if (type & HW_BREAKPOINT_R)
            type |= HW_BREAKPOINT_W;

        if (type & HW_BREAKPOINT_X) {
            len = sizeof(long);
            type = HW_BREAKPOINT_X;
        }

        hwbp[i].address = value;
        hwbp[i].len = len;
        hwbp[i].type = type;
        j = 0;
        if (type & HW_BREAKPOINT_R) hwbp[i].typestr[j++] = 'R';
        if (type & HW_BREAKPOINT_W) hwbp[i].typestr[j++] = 'W';
        if (type & HW_BREAKPOINT_X) hwbp[i].typestr[j++] = 'X';
        hwbp[i].typestr[j] = '\0';

        free(s);
    }

    return 0;
}

static int breakpoint_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct breakpoint_ctx *ctx;
    struct perf_event_attr attr = {
        .type        = PERF_TYPE_BREAKPOINT,
        .config      = 0,
        .size        = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR | PERF_SAMPLE_CPU |
                       (env->callchain ? PERF_SAMPLE_CALLCHAIN : 0) | PERF_SAMPLE_REGS_INTR,
        .read_format = 0,
        .sample_regs_intr = PERF_REGS_MASK,
        .pinned        = 1,
        .disabled      = 1,
        .exclude_user  = env->exclude_user,
        .exclude_kernel = env->exclude_kernel,
        .exclude_callchain_user = exclude_callchain_user(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER),
        .exclude_callchain_kernel = exclude_callchain_kernel(dev, CALLCHAIN_KERNEL | CALLCHAIN_USER),
        .wakeup_events = 1,
    };
    struct perf_evsel *evsel;
    int i;

    if (monitor_ctx_init(dev) < 0)
        return -1;
    ctx = dev->private;

    prof_dev_env2attr(dev, &attr);

    if (!attr.watermark)
        ctx->timely_sym = 1;

    for (i = 0; i < HBP_NUM; i++) {
        if (ctx->hwbp[i].address) {
            if (env->verbose)
                printf("%p len %d type %d\n", (void *)ctx->hwbp[i].address, ctx->hwbp[i].len, ctx->hwbp[i].type);

            attr.bp_addr = ctx->hwbp[i].address;
            attr.bp_type = ctx->hwbp[i].type;
            attr.bp_len = ctx->hwbp[i].len;

            evsel = perf_evsel__new(&attr);
            if (!evsel)
                goto failed;

            perf_evlist__add(evlist, evsel);
        } else
            break;
    }
    return 0;

failed:
    monitor_ctx_exit(dev);
    return -1;
}

static void breakpoint_deinit(struct prof_dev *dev)
{
    monitor_ctx_exit(dev);
}

struct sample_regs_intr {
    u64     abi;
    u64     regs[hweight64(PERF_REGS_MASK)];
};

static void print_regs_intr(struct sample_regs_intr *regs_intr, u64 unused)
{
#if defined(__i386__) || defined(__x86_64__)
#define REG(r) regs_intr->regs[PERF_REG_X86_##r - (PERF_REG_X86_##r > PERF_REG_X86_DS ? REG_NOSUPPORT_N : 0)]
    printf("      RIP: %016lx RSP: %016lx RFLAGS:%08lx\n", REG(IP), REG(SP), REG(FLAGS));
    printf("      RAX: %016lx RBX: %016lx RCX: %016lx\n", REG(AX), REG(BX), REG(CX));
    printf("      RDX: %016lx RSI: %016lx RDI: %016lx\n", REG(DX), REG(SI), REG(DI));

#if defined(__i386__)
    printf("      RBP: %016lx CS: %04lx SS: %04lx\n", REG(BP), REG(CS), REG(SS));
#else
    printf("      RBP: %016lx R08: %016lx R09: %016lx\n", REG(BP), REG(R8), REG(R9));
    printf("      R10: %016lx R11: %016lx R12: %016lx\n", REG(R10), REG(R11), REG(R12));
    printf("      R13: %016lx R14: %016lx R15: %016lx\n", REG(R13), REG(R14), REG(R15));
    printf("      CS: %04lx SS: %04lx\n", REG(CS), REG(SS));
#endif

#elif defined(__aarch64__)
#define REG(r) regs_intr->regs[PERF_REG_ARM64_##r]
    printf("      X00: %016lx X01: %016lx X02: %016lx X03: %016lx\n", REG(X0), REG(X1), REG(X2), REG(X3));
    printf("      X04: %016lx X05: %016lx X06: %016lx X07: %016lx\n", REG(X4), REG(X5), REG(X6), REG(X7));
    printf("      X08: %016lx X09: %016lx X10: %016lx X11: %016lx\n", REG(X8), REG(X9), REG(X10), REG(X11));
    printf("      X12: %016lx X13: %016lx X14: %016lx X15: %016lx\n", REG(X12), REG(X13), REG(X14), REG(X15));
    printf("      X16: %016lx X17: %016lx X18: %016lx X19: %016lx\n", REG(X16), REG(X17), REG(X18), REG(X19));
    printf("      X20: %016lx X21: %016lx X22: %016lx X23: %016lx\n", REG(X20), REG(X21), REG(X22), REG(X23));
    printf("      X24: %016lx X25: %016lx X26: %016lx X27: %016lx\n", REG(X24), REG(X25), REG(X26), REG(X27));
    printf("      X28: %016lx X29: %016lx LR: %016lx\n", REG(X28), REG(X29), REG(LR));
    printf("      SP: %016lx PC: %016lx\n", REG(SP), REG(PC));
#endif
}

#if defined(__i386__) || defined(__x86_64__)

#define byte_mask(n) ((1UL << 8*(n))-1)
#define RIP    16
#define RFLAGS 17

#define OpNone             0
#define OpReg              1  /* Register */
#define OpMem              2  /* Memory */
#define OpMemAbs           3  /* Memory Offset */
#define OpAcc              4  /* Accumulator: AL/AX/EAX/RAX */
#define OpCL               5  /* CL register (for shifts) */
#define OpImm              6  /* Sign extended up to 32-bit immediate */
#define OpImmByte          7  /* 8-bit sign extended immediate */
#define OpOne              8  /* Implied 1 */

static char *str_add(char *a, const char *fmt, ...)
{
    va_list ap;
    char *ptr;

    va_start(ap, fmt);
    ptr = straddv(a, free, fmt, ap);
    va_end(ap);
    return ptr;
}

static u64 reg_read(struct sample_regs_intr *regs_intr, int reg)
{
    switch (reg)
    {
        case 0: return regs_intr->regs[PERF_REG_X86_AX];
        case 1: return regs_intr->regs[PERF_REG_X86_CX];
        case 2: return regs_intr->regs[PERF_REG_X86_DX];
        case 3: return regs_intr->regs[PERF_REG_X86_BX];
        case 4: return regs_intr->regs[PERF_REG_X86_SP];
        case 5: return regs_intr->regs[PERF_REG_X86_BP];
        case 6: return regs_intr->regs[PERF_REG_X86_SI];
        case 7: return regs_intr->regs[PERF_REG_X86_DI];
        case 8: return regs_intr->regs[PERF_REG_X86_R8-REG_NOSUPPORT_N];
        case 9: return regs_intr->regs[PERF_REG_X86_R9-REG_NOSUPPORT_N];
        case 10: return regs_intr->regs[PERF_REG_X86_R10-REG_NOSUPPORT_N];
        case 11: return regs_intr->regs[PERF_REG_X86_R11-REG_NOSUPPORT_N];
        case 12: return regs_intr->regs[PERF_REG_X86_R12-REG_NOSUPPORT_N];
        case 13: return regs_intr->regs[PERF_REG_X86_R13-REG_NOSUPPORT_N];
        case 14: return regs_intr->regs[PERF_REG_X86_R14-REG_NOSUPPORT_N];
        case 15: return regs_intr->regs[PERF_REG_X86_R15-REG_NOSUPPORT_N];
        case RIP: return regs_intr->regs[PERF_REG_X86_IP];
        case RFLAGS: return regs_intr->regs[PERF_REG_X86_FLAGS];
        default: return 0UL;
    }
}

static s32 disp(struct insn *insn)
{
    return insn->displacement.got ? insn->displacement.value : 0;
}

static bool is_byteop(struct insn *insn)
{
    /*
     * Intel SDM Volume 2, B.1.4.3
     * All opcodes selected by support() satisfy B.1.4.3, except 0xBA0F.
     */
    insn_byte_t last_byte = insn->opcode.bytes[insn->opcode.nbytes-1];
    switch (insn->opcode.value) {
        case 0xBA0F: return 0; // Grp8 Ev,Ib (1A)
        default: return !(last_byte & 1);
    }
}

static const char *reg_name(struct insn *insn, int reg, bool mem)
{
    static const char *gp64[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                   "r8","r9","r10","r11","r12","r13","r14","r15"};
    static const char *gp32[16] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi",
                                   "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
    static const char *gp16[16] = {"ax","cx","dx","bx","sp","bp","si","di",
                                   "r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w"};
    static const char *gp8[16] = {"al","cl","dl","bl","ah","ch","dh","bh",
                                  "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"};
    static const char **gp[4] = {gp8, gp16, gp32, gp64};
    if (mem)
        return gp64[reg];
    else {
        int bytes = is_byteop(insn) ? 1 : insn->opnd_bytes;
        int o = fls(bytes) - 1;
        return gp[o][reg];
    }
}

static char *decode_modrm_str(struct insn *insn, char *opstr)
{
    u8 rex_prefix = insn->rex_prefix.got ? insn->rex_prefix.bytes[0] : 0;
    int index_reg, base_reg, scale;
    int modrm_mod, modrm_rm;
    char *str = NULL;

    index_reg = (rex_prefix << 2) & 8; /* REX.X */
    base_reg = (rex_prefix << 3) & 8; /* REX.B */

    modrm_mod = X86_MODRM_MOD(insn->modrm.bytes[0]);
    modrm_rm = base_reg | X86_MODRM_RM(insn->modrm.bytes[0]);
    if (modrm_mod == 3)
        return str_add(opstr, "%%%s", reg_name(insn, modrm_rm, 0));

    if (insn->displacement.nbytes) {
        if (insn->displacement.value < 0)
            str = str_add(str, "-0x%x", -insn->displacement.value);
        else
            str = str_add(str, "0x%x", insn->displacement.value);
    }

    /* 32/64-bit ModR/M decode. */
    if ((modrm_rm & 7) == 4) {
        u8 sib = insn->sib.bytes[0];
        index_reg |= (sib >> 3) & 7;
        base_reg |= sib & 7;
        scale = sib >> 6;

        if ((base_reg & 7) == 5 && modrm_mod == 0) {
            if (index_reg == 4) goto addr32;
            str = str_add(str, "(");
        } else
            str = str_add(str, "(%%%s", reg_name(insn, base_reg, 1));

        if (index_reg != 4)
            str = str_add(str, ",%%%s,%d)", reg_name(insn, index_reg, 1), 1<<scale);
        else
            str = str_add(str, ")");
    } else if ((modrm_rm & 7) == 5 && modrm_mod == 0) {
        if (insn->x86_64) // RIP-Relative Addressing
            str = str_add(str, "(%%rip)");
        else
            goto addr32;
    } else
        str = str_add(str, "(%%%s)", reg_name(insn, modrm_rm, 1));

    opstr = str_add(opstr, str);
    if (str) free(str);
    return opstr;

addr32:
    if (str) free(str);
    return str_add(opstr, "0x%x(none)", insn->displacement.value);
}

static u64 decode_modrm(struct insn *insn, struct sample_regs_intr *regs_intr)
{
    u8 rex_prefix = insn->rex_prefix.got ? insn->rex_prefix.bytes[0] : 0;
    int modrm_reg, index_reg, base_reg, scale;
    int modrm_mod, modrm_rm;
    u64 addr = 0UL;

    modrm_reg = ((rex_prefix << 1) & 8); /* REX.R */
    index_reg = (rex_prefix << 2) & 8; /* REX.X */
    base_reg = (rex_prefix << 3) & 8; /* REX.B */

    modrm_mod = X86_MODRM_MOD(insn->modrm.bytes[0]);
    modrm_reg = modrm_reg | X86_MODRM_REG(insn->modrm.bytes[0]);
    modrm_rm = base_reg | X86_MODRM_RM(insn->modrm.bytes[0]);
    if (modrm_mod == 3)
        return 0UL;

    /* 32/64-bit ModR/M decode. */
    if ((modrm_rm & 7) == 4) {
        u8 sib = insn->sib.bytes[0];
        index_reg |= (sib >> 3) & 7;
        base_reg |= sib & 7;
        scale = sib >> 6;

        if ((base_reg & 7) == 5 && modrm_mod == 0)
            addr += disp(insn);
        else
            addr += reg_read(regs_intr, base_reg);

        if (index_reg != 4)
            addr += reg_read(regs_intr, index_reg) << scale;
    } else if ((modrm_rm & 7) == 5 && modrm_mod == 0) {
        addr += disp(insn);
        if (insn->x86_64) // RIP-Relative Addressing
            addr += reg_read(regs_intr, RIP /* rIP */);
    } else {
        base_reg = modrm_rm;
        addr += reg_read(regs_intr, base_reg);
    }

    if (modrm_mod == 1 || modrm_mod == 2)
        addr += disp(insn);

    return addr;
}

static u64 decode_mem_abs(struct insn *insn)
{
    u64 addr;

    addr = insn->moffset2.got ? insn->moffset2.value : 0UL;
    addr = (addr << 32) | (insn->moffset1.got ? insn->moffset1.value : 0UL);

    return addr;
}

static u64 decode_opsrc_reg(struct insn *insn, struct sample_regs_intr *regs_intr, bool byteop)
{
    int bytes = byteop ? 1 : insn->opnd_bytes;
    u8 rex_prefix = insn->rex_prefix.got ? insn->rex_prefix.bytes[0] : 0;
    int highbyte_regs = (rex_prefix == 0) && byteop;
    int modrm_reg;
    u64 data;

    modrm_reg = ((rex_prefix << 1) & 8); /* REX.R */
    modrm_reg = modrm_reg | X86_MODRM_REG(insn->modrm.bytes[0]);

    if (highbyte_regs && modrm_reg >= 4 && modrm_reg < 8)
        data = reg_read(regs_intr, modrm_reg & 3) >> 8;
    else
        data = reg_read(regs_intr, modrm_reg);

    return bytes == 8 ? data : (data & byte_mask(bytes));
}

static u64 decode_opsrc_Acc(struct insn *insn, struct sample_regs_intr *regs_intr, bool byteop)
{
    int bytes = byteop ? 1 : insn->opnd_bytes;
    u64 data = reg_read(regs_intr, 0); // AL/rAX
    return bytes == 8 ? data : (data & byte_mask(bytes));
}

static u64 decode_opsrc_Imm(struct insn *insn, bool byteop, bool sign_extension)
{
    int bytes = byteop ? 1 : insn->opnd_bytes;
    int size = bytes == 8 ? 4 : bytes;
    u64 data;

    data = insn->immediate2.got ? (s64)insn->immediate2.value : 0UL;
    data = (data << 32) | (insn->immediate1.got ? (s64)insn->immediate1.value : 0UL);

    if (!sign_extension)
        data = data & byte_mask(size);

    return bytes == 8 ? data : (data & byte_mask(bytes));
}

static u64 decode_opsrc_CL(struct insn *insn, struct sample_regs_intr *regs_intr)
{
    return reg_read(regs_intr, 1) /* CL/rCX */  & 0xff;
}

static u64 decode_addr(struct insn *insn, struct sample_regs_intr *regs_intr)
{
    switch (insn->opcode.value)
    {
        // MOV
        case 0x88: // MOV Eb,Gb
        case 0x89: // MOV Ev,Gv
        case 0xC6: // Grp11 Eb,Ib (1A)
        case 0xC7: // Grp11 Ev,Iz (1A)
            return decode_modrm(insn, regs_intr);
        case 0xA2: // MOV Ob,AL
        case 0xA3: // MOV Ov,rAX
            return decode_mem_abs(insn);

        // CMPXCHG
        case 0xB00F: // CMPXCHG Eb,Gb
        case 0xB10F: // CMPXCHG Ev,Gv

        // ADD
        case 0x00: // ADD Eb,Gb         Add r8 to r/m8
        case 0x01: // ADD Ev,Gv         Add r64 to r/m64
        case 0x80: // Grp1 Eb,Ib (1A)   Add/Sub/.. imm8 to r/m8.
        case 0x81: // Grp1 Ev,Iz (1A)   Add/Sub/.. imm32 to r/m32.
        case 0x83: // Grp1 Ev,Ib (1A)   Add/Sub/.. sign-extended imm8 to r/m64

        // SUB
        case 0x28: // SUB Eb,Gb
        case 0x29: // SUB Ev,Gv

        // OR, AND, XOR
        case 0x08: // OR Eb,Gb
        case 0x09: // OR Ev,Gv
        case 0x20: // AND Eb,Gb
        case 0x21: // AND Ev,Gv
        case 0x30: // XOR Eb,Gb
        case 0x31: // XOR Ev,Gv

        // INC, DEC
        case 0xFE: // INC Eb; DEC Eb;
        case 0xFF: // INC Ev; DEC Ev;
        // NOT, NEG
        case 0xF6: // NOT Eb; NEG Eb;
        case 0xF7: // NOT Ev; NEG Ev;

        // SAL/SAR/SHL/SHR, ROL/ROR
        case 0xC0: // Grp2 Eb,Ib (1A)
        case 0xC1: // Grp2 Ev,Ib (1A)
        case 0xD0: // Grp2 Eb,1 (1A)
        case 0xD1: // Grp2 Ev,1 (1A)
        case 0xD2: // Grp2 Eb,CL (1A)
        case 0xD3: // Grp2 Ev,CL (1A)
            return decode_modrm(insn, regs_intr);

        // Bit Test and Set/Reset/Complement
        case 0xAB0F: // BTS Ev,Gv
        case 0xB30F: // BTR Ev,Gv
        case 0xBB0F: // BTC Ev,Gv
        case 0xBA0F: { // Grp8 Ev,Ib (1A)
            u64 addr = decode_modrm(insn, regs_intr);
            s64 bitoffset, mask = ~((s64)insn->opnd_bytes * 8 - 1);

            if (insn->opcode.value == 0xBA0F)
                bitoffset = decode_opsrc_Imm(insn, 1, 1);
            else
                bitoffset = decode_opsrc_reg(insn, regs_intr, 0);

            /*
             * Align to opnd_bytes boundary, ensuring the decoded address
             * overlaps breakpoint range, in x86_decode_insn(). Otherwise,
             * the breakpoint is triggered but the data cannot be decoded.
             *
             *    |  LENn  |            # LENn=2, 2-byte breakpoint.
             *    |             +   |   # opnd_bytes=4
             *                  ` Returns this address. Check failed.
             *    ` Return this address. Check passed.
             */
            if (insn->opnd_bytes == 2)
                bitoffset = (s16)bitoffset & (s16)mask;
            else if (insn->opnd_bytes == 4)
                bitoffset = (s32)bitoffset & (s32)mask;
            else
                bitoffset = (s64)bitoffset & (s64)mask;

            return addr + (bitoffset >> 3);
        }

        default:
            return 0UL;
    }
}

static u64 decode_data(struct insn *insn, struct sample_regs_intr *regs_intr, u64 old)
{
    int op = insn->opcode.value;
    int opext = 0;
    u64 data;
    int bytes = insn->opnd_bytes;
    bool byteop;
    u8 shift;
    u64 bitoffset;
    static const void * const grp1_tbl[8] = {
        [0 ... 7] = && default_label,
        [0] = &&ADD,
        [1] = &&OR,
        [4] = &&AND,
        [5] = &&SUB,
        [6] = &&XOR,
    };

    byteop = is_byteop(insn); // Intel SDM Volume 2, B.1.4.3
    if (byteop) bytes = 1;
    switch (op)
    {
        // MOV
        case 0x88: /* MOV Eb,Gb  */
        case 0x89: /* MOV Ev,Gv  */ return decode_opsrc_reg(insn, regs_intr, byteop);
        case 0xA2: /* MOV Ob,AL  */
        case 0xA3: /* MOV Ov,rAX */ return decode_opsrc_Acc(insn, regs_intr, byteop);
        case 0xC6: /* Grp11 Eb,Ib (1A) */
        case 0xC7: /* Grp11 Ev,Iz (1A) */ return decode_opsrc_Imm(insn, byteop, 1);

        // CMPXCHG
        case 0xB00F: // CMPXCHG Eb,Gb
        case 0xB10F: { // CMPXCHG Ev,Gv
            u64 rflags = reg_read(regs_intr, RFLAGS);
            // Compare RAX with r/m64.
            // If equal, ZF is set and r64 is loaded into r/m64.
            // Else, clear ZF and load r/m64 into RAX.
            if (test_bit(6 /*ZF*/, &rflags))
                return decode_opsrc_reg(insn, regs_intr, byteop);
            else
                return decode_opsrc_Acc(insn, regs_intr, byteop);
        }   break;

        // ADD
        case 0x00: /* ADD Eb,Gb */
        case 0x01: /* ADD Ev,Gv */ data = decode_opsrc_reg(insn, regs_intr, byteop);
        ADD: data = old + data;
            break;

        // Grp1
        case 0x80: /* Grp1 Eb,Ib (1A) */
        case 0x81: /* Grp1 Ev,Iz (1A) */
        case 0x83: /* Grp1 Ev,Ib (1A) */
            data = decode_opsrc_Imm(insn, byteop, 1);
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            goto *grp1_tbl[opext]; // 0:ADD, 1:OR, 2:ADC, 3:SBB, 4:AND, 5:SUB, 6:XOR, 7:CMP;

        // SUB
        case 0x28: /* SUB Eb,Gb */
        case 0x29: /* SUB Ev,Gv */ data = decode_opsrc_reg(insn, regs_intr, byteop);
        SUB: data = old - data;
            break;

        // OR, AND, XOR
        case 0x08: /* OR Eb,Gb */
        case 0x09: /* OR Ev,Gv */ data = decode_opsrc_reg(insn, regs_intr, byteop);
        OR: data = old | data;
            break;
        case 0x20: /* AND Eb,Gb */
        case 0x21: /* AND Ev,Gv */ data = decode_opsrc_reg(insn, regs_intr, byteop);
        AND: data = old & data;
            break;
        case 0x30: /* XOR Eb,Gb */
        case 0x31: /* XOR Ev,Gv */ data = decode_opsrc_reg(insn, regs_intr, byteop);
        XOR: data = old ^ data;
            break;

        // INC, DEC
        case 0xFE: // INC Eb; DEC Eb;
        case 0xFF: // INC Ev; DEC Ev;
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            if (opext == 0) data = old + 1; // INC
            else if (opext == 1) data = old - 1; // DEC
            else goto default_label;
            break;

        // NOT, NEG
        case 0xF6: // NOT Eb; NEG Eb;
        case 0xF7: // NOT Ev; NEG Ev;
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            if (opext == 2) data = ~old; // NOT
            else if (opext == 3) data = -old; // NEG
            else goto default_label;
            break;

        // SAL/SAR/SHL/SHR, ROL/ROR
        case 0xC0: // Grp2 Eb,Ib (1A)
        case 0xC1: /* Grp2 Ev,Ib (1A) */ shift = decode_opsrc_Imm(insn, 1, 1); goto Grp2;
        case 0xD0: // Grp2 Eb,1 (1A)
        case 0xD1: /* Grp2 Ev,1 (1A) */ shift = 1; goto Grp2;
        case 0xD2: // Grp2 Eb,CL (1A)
        case 0xD3: /* Grp2 Ev,CL (1A) */ shift = decode_opsrc_CL(insn, regs_intr); goto Grp2;
        Grp2:
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            // 0:ROL 1:ROR 2:RCL 3:RCR 4:SHL/SAL 5:SHR 7:SAR
            data = old;
            switch (opext) {
            case 0: switch (bytes) {
                    case 1: asm("rolb %b1, %b0": "+r" (data) : "c" (shift)); break;
                    case 2: asm("rolw %b1, %w0": "+r" (data) : "c" (shift)); break;
                    case 4: asm("roll %b1, %k0": "+r" (data) : "c" (shift)); break;
                    case 8: asm("rolq %b1, %q0": "+r" (data) : "c" (shift)); break;
                    default: goto default_label;
                    } break;
            case 1: switch (bytes) {
                    case 1: asm("rorb %b1, %b0": "+r" (data) : "c" (shift)); break;
                    case 2: asm("rorw %b1, %w0": "+r" (data) : "c" (shift)); break;
                    case 4: asm("rorl %b1, %k0": "+r" (data) : "c" (shift)); break;
                    case 8: asm("rorq %b1, %q0": "+r" (data) : "c" (shift)); break;
                    default: goto default_label;
                    } break;
            case 4: data = data << shift; break;
            case 5: switch (bytes) {
                    case 1: data = (u8)data >> shift; break;
                    case 2: data = (u16)data >> shift; break;
                    case 4: data = (u32)data >> shift; break;
                    case 8: data = (u64)data >> shift; break;
                    default: goto default_label;
                    } break;
            case 7: switch (bytes) {
                    case 1: data = (s8)data >> shift; break;
                    case 2: data = (s16)data >> shift; break;
                    case 4: data = (s32)data >> shift; break;
                    case 8: data = (s64)data >> shift; break;
                    default: goto default_label;
                    } break;
            default: goto default_label;
            }
            break;

        // Bit Test and Set/Reset/Complement
        case 0xAB0F: /* BTS Ev,Gv */ opext = 5; goto BTSRC;
        case 0xB30F: /* BTR Ev,Gv */ opext = 6; goto BTSRC;
        case 0xBB0F: /* BTC Ev,Gv */ opext = 7; goto BTSRC;
        case 0xBA0F: { // Grp8 Ev,Ib (1A)
        BTSRC:
            if (insn->opcode.value == 0xBA0F) {
                bitoffset = decode_opsrc_Imm(insn, 1, 1) & (bytes * 8 - 1);
                opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            } else
                bitoffset = decode_opsrc_reg(insn, regs_intr, 0) & (bytes * 8 - 1);

            data = old;
            switch (opext) { // 4:BT 5:BTS 6:BTR 7:BTC
                case 5: asm("bts %1, %0": "+r" (data) : "r" (bitoffset)); break;
                case 6: asm("btr %1, %0": "+r" (data) : "r" (bitoffset)); break;
                case 7: asm("btc %1, %0": "+r" (data) : "r" (bitoffset)); break;
                default: goto default_label;
            }
        } break;

        default_label:
        default: return 0UL;
    }

    return bytes == 8 ? data : (data & byte_mask(bytes));
}

static char *decode_opstr(struct insn *insn, const char *op, int opsrc, int opdst)
{
    int bytes = is_byteop(insn) ? 1 : insn->opnd_bytes;
    u8 rex_prefix = insn->rex_prefix.got ? insn->rex_prefix.bytes[0] : 0;
    int modrm_reg;
    static char opw[4] = {'b', 'w', 'l', 'q'};
    int w = fls(bytes) - 1;
    char *src = NULL;

    switch (opsrc) {
        case OpNone:
            src = str_add(src, "%s%c ", op, opw[w]);
            break;
        case OpReg:
            modrm_reg = ((rex_prefix << 1) & 8); /* REX.R */
            modrm_reg = modrm_reg | X86_MODRM_REG(insn->modrm.bytes[0]);
            src = str_add(src, "%s %%%s,", op, reg_name(insn, modrm_reg, 0));
            break;
        case OpImm:
            src = str_add(src, "%s%c $0x%lx,", op, opw[w], decode_opsrc_Imm(insn, bytes == 1, 1));
            break;
        case OpAcc:
            src = str_add(src, "%s %%%s,", op, reg_name(insn, 0, 0));
            break;
        case OpCL:
            src = str_add(src, "%s%c %%cl,", op, opw[w]);
            break;
        case OpOne:
            src = str_add(src, "%s%c $0x1,", op, opw[w]);
            break;
        default: return NULL;
    }

    switch (opdst) {
        case OpMem:
            return decode_modrm_str(insn, src);
        case OpMemAbs:
            return str_add(src, "0x%lx", decode_mem_abs(insn));
        default: break;
    }

    if (src) free(src);
    return NULL;
}

static bool safety(struct insn *insn)
{
    switch (insn->opcode.value)
    {
        case 0x88: // MOV Eb,Gb
        case 0x89: // MOV Ev,Gv
        case 0xA2: // MOV Ob,AL
        case 0xA3: // MOV Ov,rAX
        case 0xC6: // Grp11 Eb,Ib (1A)
        case 0xC7: // Grp11 Ev,Iz (1A)
        case 0xB00F: // CMPXCHG Eb,Gb
        case 0xB10F: // CMPXCHG Ev,Gv
            return true;
        default:
            return false;
    }
}

static bool supported(struct insn_decode_ctxt *ctxt, struct insn *insn)
{
    struct hw_breakpoint *bp = ctxt->bp;
    int op, opext;

    if (bp->type != HW_BREAKPOINT_W)
        return false;
    if (!insn->opcode.got)
        return false;
    if (insn->opcode.nbytes > 2) // only one-byte/two-byte opcode
        return false;
    if (!insn->modrm.got || !insn->modrm.nbytes)
        return false;

    op = insn->opcode.value;
    switch (op)
    {
        // MOV
        case 0x88: // MOV Eb,Gb.        Move r8 to r/m8.
        case 0x89: // MOV Ev,Gv.        Move r64 to r/m64.
        case 0xA2: // MOV Ob,AL.        Move AL to (seg:offset)
        case 0xA3: // MOV Ov,rAX.       Move RAX to (offset)
            break;
        case 0xC6: // Grp11 Eb,Ib (1A). Move imm8 to r/m8
        case 0xC7: // Grp11 Ev,Iz (1A). Move imm32 to r/m32
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            if (opext != 0) // ! MOV
                return false;
            break;

        // CMPXCHG
        case 0xB00F: // CMPXCHG Eb,Gb
        case 0xB10F: // CMPXCHG Ev,Gv
            break;

        // ADD
        case 0x00: // ADD Eb,Gb         Add r8 to r/m8
        case 0x01: // ADD Ev,Gv         Add r64 to r/m64
            break;

        // Grp1
        case 0x80: // Grp1 Eb,Ib (1A)   Add/Sub/.. imm8 to r/m8.
        case 0x81: // Grp1 Ev,Iz (1A)   Add/Sub/.. imm32 to r/m32.
        case 0x83: // Grp1 Ev,Ib (1A)   Add/Sub/.. sign-extended imm8 to r/m64
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            // 0:ADD, 1:OR, 2:ADC, 3:SBB, 4:AND, 5:SUB, 6:XOR, 7:CMP;
            if (opext == 2 || opext == 3 || opext == 7) // ! ADC SBB CMP
                return false;
            break;

        // SUB
        case 0x28: // SUB Eb,Gb
        case 0x29: // SUB Ev,Gv
            break;

        // OR, AND, XOR
        case 0x08: // OR Eb,Gb
        case 0x09: // OR Ev,Gv
        case 0x20: // AND Eb,Gb
        case 0x21: // AND Ev,Gv
        case 0x30: // XOR Eb,Gb
        case 0x31: // XOR Ev,Gv
            break;

        // INC, DEC
        case 0xFE: // Grp4 (1A) Eb
        case 0xFF: // Grp5 (1A) Ev
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            // 0:INC, 1:DEC;
            if (opext != 0 && opext != 1)
                return false;
            break;

        // NOT, NEG
        case 0xF6: // Grp3 Eb (1A)
        case 0xF7: // Grp3 Ev (1A)
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            // 2:NOT, 3:NEG;
            if (opext != 2 && opext != 3)
                return false;
            break;

        // SAL/SAR/SHL/SHR, ROL/ROR
        case 0xC0: // Grp2 Eb,Ib (1A)
        case 0xC1: // Grp2 Ev,Ib (1A)
        case 0xD0: // Grp2 Eb,1 (1A)
        case 0xD1: // Grp2 Ev,1 (1A)
        case 0xD2: // Grp2 Eb,CL (1A)
        case 0xD3: // Grp2 Ev,CL (1A)
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            // 0:ROL 1:ROR 2:RCL 3:RCR 4:SHL/SAL 5:SHR 7:SAR
            if (opext == 2 || opext == 3 || opext == 6)
                return false;
            break;

        // Bit Test and Set/Reset/Complement
        case 0xAB0F: // BTS Ev,Gv
        case 0xB30F: // BTR Ev,Gv
        case 0xBB0F: // BTC Ev,Gv
            break;
        case 0xBA0F: // Grp8 Ev,Ib (1A)
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            // 4:BT 5:BTS 6:BTR 7:BTC
            if (opext <= 4)
                return false;
            break;

        default:
            return false;
    }

    if (!safety(insn) && !ctxt->safety)
        return false;

    return true;
}

static char *disassemble(struct insn *insn)
{
    int opext;
    int opsrc;
    switch (insn->opcode.value)
    {
        // MOV
        case 0x88: // MOV Eb,Gb.        Move r8 to r/m8.
        case 0x89: // MOV Ev,Gv.        Move r64 to r/m64.
            return decode_opstr(insn, "mov", OpReg, OpMem);
        case 0xA2: // MOV Ob,AL.        Move AL to (seg:offset)
        case 0xA3: // MOV Ov,rAX.       Move RAX to (offset)
            return decode_opstr(insn, "movabs", OpAcc, OpMemAbs);
        case 0xC6: // Grp11 Eb,Ib (1A). Move imm8 to r/m8
        case 0xC7: // Grp11 Ev,Iz (1A). Move imm32 to r/m32
            return decode_opstr(insn, "mov", OpImm, OpMem);

        // XCHG
        case 0x86: // XCHG Eb,Gb        Exchange r8 with byte from r/m8
        case 0x87: // XCHG Ev,Gv        Exchange r64 with quadword from r/m64
            return decode_opstr(insn, "xchg", OpReg, OpMem);
        // CMPXCHG
        case 0xB00F: // CMPXCHG Eb,Gb
        case 0xB10F: // CMPXCHG Ev,Gv
            return decode_opstr(insn, "cmpxchg", OpReg, OpMem);

        // ADD
        case 0x00: // ADD Eb,Gb         Add r8 to r/m8
        case 0x01: // ADD Ev,Gv         Add r64 to r/m64
            return decode_opstr(insn, "add", OpReg, OpMem);

        // Grp1
        case 0x80: // Grp1 Eb,Ib (1A)   Add/Sub/.. imm8 to r/m8.
        case 0x81: // Grp1 Ev,Iz (1A)   Add/Sub/.. imm32 to r/m32.
        case 0x83: // Grp1 Ev,Ib (1A)   Add/Sub/.. sign-extended imm8 to r/m64
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            // 0:ADD, 1:OR, 2:ADC, 3:SBB, 4:AND, 5:SUB, 6:XOR, 7:CMP;
            switch (opext) {
                case 0: return decode_opstr(insn, "add", OpImm, OpMem);
                case 1: return decode_opstr(insn, "or", OpImm, OpMem);
                case 4: return decode_opstr(insn, "and", OpImm, OpMem);
                case 5: return decode_opstr(insn, "sub", OpImm, OpMem);
                case 6: return decode_opstr(insn, "xor", OpImm, OpMem);
                default: break;
            } break;

        // SUB
        case 0x28: // SUB Eb,Gb
        case 0x29: // SUB Ev,Gv
            return decode_opstr(insn, "sub", OpReg, OpMem);

        // OR, AND, XOR
        case 0x08: // OR Eb,Gb
        case 0x09: // OR Ev,Gv
            return decode_opstr(insn, "or", OpReg, OpMem);
        case 0x20: // AND Eb,Gb
        case 0x21: // AND Ev,Gv
            return decode_opstr(insn, "and", OpReg, OpMem);
        case 0x30: // XOR Eb,Gb
        case 0x31: // XOR Ev,Gv
            return decode_opstr(insn, "xor", OpReg, OpMem);

        // INC, DEC
        case 0xFE: // Grp4 (1A) Eb
        case 0xFF: // Grp5 (1A) Ev
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            // 0:INC, 1:DEC;
            return decode_opstr(insn, opext == 0 ? "inc" : "dec", OpNone, OpMem);

        // NOT, NEG
        case 0xF6: // Grp3 Eb (1A)
        case 0xF7: // Grp3 Ev (1A)
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            // 2:NOT, 3:NEG;
            return decode_opstr(insn, opext == 2 ? "not" : "neg", OpNone, OpMem);

        // SAL/SAR/SHL/SHR, ROL/ROR
        case 0xC0: // Grp2 Eb,Ib (1A)
        case 0xC1: /* Grp2 Ev,Ib (1A) */ opsrc = OpImm; goto SSSRR;
        case 0xD0: // Grp2 Eb,1 (1A)
        case 0xD1: /* Grp2 Ev,1 (1A) */ opsrc = OpOne; goto SSSRR;
        case 0xD2: // Grp2 Eb,CL (1A)
        case 0xD3: /* Grp2 Ev,CL (1A) */ opsrc = OpCL;
        SSSRR:
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            // 0:ROL 1:ROR 2:RCL 3:RCR 4:SHL/SAL 5:SHR 7:SAR
            switch (opext) {
                case 0: return decode_opstr(insn, "rol", opsrc, OpMem);
                case 1: return decode_opstr(insn, "ror", opsrc, OpMem);
                case 4: return decode_opstr(insn, "shl", opsrc, OpMem);
                case 5: return decode_opstr(insn, "shr", opsrc, OpMem);
                case 7: return decode_opstr(insn, "sar", opsrc, OpMem);
                default: break;
            } break;

        // Bit Test and Set/Reset/Complement
        case 0xAB0F: opsrc = OpReg; goto BTS; // BTS Ev,Gv
        case 0xB30F: opsrc = OpReg; goto BTR; // BTR Ev,Gv
        case 0xBB0F: opsrc = OpReg; goto BTC; // BTC Ev,Gv
        case 0xBA0F: // Grp8 Ev,Ib (1A)
            opext = X86_MODRM_REG(insn->modrm.bytes[0]);
            // 4:BT 5:BTS 6:BTR 7:BTC
            opsrc = OpImm;
            switch (opext) {
                case 5: BTS: return decode_opstr(insn, "bts", opsrc, OpMem);
                case 6: BTR: return decode_opstr(insn, "btr", opsrc, OpMem);
                case 7: BTC: return decode_opstr(insn, "btc", opsrc, OpMem);
                default: break;
            } break;

        default: break;
    }
    return NULL;
}

static void x86_decode_insn(struct breakpoint_ctx *bpctx, struct insn_decode_ctxt *ctxt, u64 ip, struct sample_regs_intr *regs_intr)
{
    struct hw_breakpoint *bp = ctxt->bp;
    struct insn_decode_node *node;
    unsigned char insn_buff[15];
    unsigned char *in = insn_buff;
    int in_len = sizeof(insn_buff);
    struct insn insn;
    enum insn_mode mode;
    int i, insn_pos = in_len;
    int bytes;
    u64 data;
    void *mem;
    bool safety = 0;

    if (regs_intr->abi == PERF_SAMPLE_REGS_ABI_NONE)
        return;

    mode = regs_intr->abi == PERF_SAMPLE_REGS_ABI_64 ? INSN_MODE_64 : INSN_MODE_32;

    insn_node_find(bpctx->insn_hashmap, node, ip) {
        in = node->insn_buff;
        insn_pos = node->insn_pos;
        goto decode;
    }

    if (kcore_read(ip - in_len /* Trap */, in, in_len) != in_len)
        return;

    for (i = in_len - 1; i >= 0; i--) {
        if (insn_decode(&insn, in + i, in_len - i, mode) < 0)
            continue;
        if (insn.length != in_len - i)
            continue;
        insn_pos = i;
    }

    node = calloc(1, sizeof(*node));
    if (!node)
        return;
    memcpy(node->insn_buff, in, in_len);
    node->insn_pos = insn_pos;
    insn_node_add(bpctx->insn_hashmap, node, ip);

    /*
     * print_event path: set safety to 0 to break continuity.
     * Non-continuous events cannot maintain accurate ctxt->bytes[] state.
     * When sample path resumes, non-safe instructions (ADD/SUB/etc) will be
     * skipped until a safe instruction (MOV/CMPXCHG) re-establishes safety.
     */
    if (bpctx->print_event)
        ctxt->safety = 0;

decode:
    for (i = insn_pos; i < in_len; i++) {
        if (insn_decode(&insn, in + i, in_len - i, mode) < 0)
            continue;
        if (insn.length != in_len - i)
            continue;

        // Only supports destination memory operand.
        if (!supported(ctxt, &insn))
            continue;

        bytes = is_byteop(&insn) ? 1 : insn.opnd_bytes;

        // Decode the memory address of the destination operand.
        ctxt->addr = decode_addr(&insn, regs_intr);
        if (ctxt->addr + bytes < bp->address ||
            ctxt->addr >= bp->address + bp->len)
            continue;

        /*
         * Intel SDM Volume3 19.2.5 Breakpoint Field Recognition
         * A data breakpoint for reading or writing data is triggered if any of
         * the bytes participating in an access is within the range defined by
         * a breakpoint address register and its LENn field.
         *
         *           bp_addr
         *    |        |  LENn  |        |
         *           ++++      ++++
         *              ++++   +
         *              ++++++++
         *   + Write range and bytes.
         *
         * Therefore, it is necessary to track the writing of data before and
         * after LENn and read the data at bp_addr.
         */
        mem = &ctxt->bytes[sizeof(u64) + ctxt->addr - bp->address];
        switch (bytes) {
            case 1: data = *(u8 *)mem; break;
            case 2: data = *(u16 *)mem; break;
            case 4: data = *(u32 *)mem; break;
            default: data = *(u64 *)mem; break;
        }

        // Decode the data of the source operand.
        data = decode_data(&insn, regs_intr, data);
        // writeback
        switch (bytes) {
            case 1: *(u8 *)mem = (u8)data; break;
            case 2: *(u16 *)mem = (u16)data; break;
            case 4: *(u32 *)mem = (u32)data; break;
            default: *(u64 *)mem = (u64)data; break;
        }

        node->insn_pos = i;
        if (!node->insn_str)
            node->insn_str = disassemble(&insn);
        safety = 1;

        switch(bp->len) {
            case 1: data = (u8)ctxt->data; break;
            case 2: data = (u16)ctxt->data; break;
            case 4: data = (u32)ctxt->data; break;
            default: data = (u64)ctxt->data; break;
        }
        printf("      INSN: ");
        for (; i < in_len; i++)
            printf("%02x ", in[i]);
        printf(" %s ADDR: %lx  DATA: %lx\n", node->insn_str, ctxt->addr, data);
        break;
    }

    // Only sample path updates safety for continuous event tracking.
    if (bpctx->sample)
        ctxt->safety = safety;
}

#endif

static void print_event(struct prof_dev *dev, union perf_event *event, int instance, int flags)
{
    struct breakpoint_ctx *ctx = dev->private;
    struct env *env = dev->env;
    // in linux/perf_event.h
    // PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR | PERF_SAMPLE_CPU | PERF_SAMPLE_CALLCHAIN |
    // PERF_SAMPLE_REGS_INTR
    struct sample_type_data {
        __u64   ip;
        struct {
            __u32    pid;
            __u32    tid;
        }    tid_entry;
        __u64   time;
        __u64   addr;
        struct {
            __u32    cpu;
            __u32    reserved;
        }    cpu_entry;
        struct callchain callchain;
    } *data = (void *)event->sample.array;
    struct {
        __u64 nr;
        __u64 ips[2];
    } callchain;
    struct sample_regs_intr *regs_intr;
    u64 rip;
    int i;

    for (i = 0; i < HBP_NUM; i++) {
        if (ctx->hwbp[i].address == data->addr)
            break;
    }

    if (env->callchain)
        regs_intr = (struct sample_regs_intr *)&data->callchain.ips[data->callchain.nr];
    else
        regs_intr = (struct sample_regs_intr *)&data->callchain;

#if defined(__i386__) || defined(__x86_64__)
    /*
     * In the kernel, the PERF_SAMPLE_IP function perf_instruction_pointer() may
     * return the guest ip. In fact, in kvm virtualization, EXTERNAL_INTERRUPT vmexit,
     * and breakpoints triggered inside the external-interrupt, the guest ip will be
     * obtained strangely.
     * The correct host rip is within the sampled regs.
     */
    rip = reg_read(regs_intr, RIP);
#elif defined(__aarch64__)
    rip = data->ip;
#endif

    if (!(flags & OMIT_TIMESTAMP))
        prof_dev_print_time(dev, data->time, stdout);

    tep__update_comm(NULL, data->tid_entry.tid);
    printf("%16s %6u [%03d] %llu.%06llu: breakpoint: 0x%llx/%d:%s ip ", tep__pid_to_comm(data->tid_entry.tid), data->tid_entry.tid,
            data->cpu_entry.cpu, data->time/NSEC_PER_SEC, (data->time%NSEC_PER_SEC)/1000,
            data->addr, ctx->hwbp[i].len, ctx->hwbp[i].typestr);

    if (rip >= START_OF_KERNEL || (ctx->sample && ctx->timely_sym)) {
        callchain.nr = 2;
        callchain.ips[0] = rip >= START_OF_KERNEL ? PERF_CONTEXT_KERNEL : PERF_CONTEXT_USER;
        callchain.ips[1] = rip;
        print_callchain(ctx->print_ip, (struct callchain *)&callchain, data->tid_entry.pid);
    } else
        printf("%016lx\n", rip);

    if (ctx->kcore && ctx->hwbp[i].type == HW_BREAKPOINT_W) {
        #if defined(__i386__) || defined(__x86_64__)
        /*
         * Decode the instruction and get the value written to data->addr.
         *
         * You cannot directly use kcore_read to read the value of data->addr
         * because it may change. Currently, kcore_read() reads instructions,
         * which generally do not change, and decode the source operands.
         */
        // Instruction breakpoint, Exception Class: Fault.
        // Data write breakpoint,  Exception Class: Trap.
        x86_decode_insn(ctx, &ctx->ctxt[i], rip, regs_intr);

        if (ctx->data_filter && ctx->sample) {
            struct insn_decode_ctxt *ctxt = &ctx->ctxt[i];
            u64 mem_data;

            if (!ctxt->safety)
                return;

            switch(ctx->hwbp[i].len) {
                case 1: mem_data = (u8)ctxt->data; break;
                case 2: mem_data = (u16)ctxt->data; break;
                case 4: mem_data = (u32)ctxt->data; break;
                default: mem_data = (u64)ctxt->data; break;
            }
            if (expr_load_glo(ctx->data_filter, FILTER_VAR_NAME, mem_data) < 0)
                return;
            // Filter conditions not met
            if (expr_run(ctx->data_filter) == 0)
                return;
        }
        #endif
    }

    if ((flags & OMIT_CALLCHAIN) && ctx->print_event)
        return;

    if (env->callchain) {
        struct callchain_data cd;
        perf_event_build_callchain_data(perf_event_evsel(dev, event), event, &cd);
        if (!env->flame_graph || ctx->print_event) {
            print_callchain_data_cbs(ctx->cc, &cd,
                env->verbose >= 0 ? (callchain_cbs)print_regs_intr : NULL, NULL, regs_intr);
        } else
            flame_graph_add_callchain(ctx->flame, &cd, data->tid_entry.pid, NULL);
    } else if (env->verbose >= 0)
        print_regs_intr(regs_intr, 0);
}

/*
 * breakpoint_print_event - Print forwarded breakpoint event.
 *
 * For non-continuous forwarded events, set print_event=1 to:
 * - Break instruction decode safety (ctxt->safety = 0)
 * - Skip data filtering (requires continuous events)
 * - Print callchain instead of writing to flame graph
 */
static void breakpoint_print_event(struct prof_dev *dev, union perf_event *event, int instance, int flags)
{
    struct breakpoint_ctx *ctx = dev->private;
    ctx->print_event = 1;
    ctx->sample = 0;
    print_event(dev, event, instance, flags);
}

/*
 * breakpoint_sample - Process continuous breakpoint sample.
 *
 * For continuous local samples, set sample=1 to:
 * - Maintain instruction decode safety state
 * - Execute data filtering with --filter option
 * - Write callchain to flame graph if configured
 */
static void breakpoint_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct breakpoint_ctx *ctx = dev->private;
    ctx->print_event = 0;
    ctx->sample = 1;
    print_event(dev, event, instance, 0);
}

static const char *breakpoint_desc[] = PROFILER_DESC("breakpoint",
    "[OPTION...] [-g [--flame-graph file]] [--filter EXPR] <addr>[/1/2/4/8][:rwx] ...",
    "Kernel/user-space hardware breakpoint facility.",
    "",
    "SYNOPSIS",
    "    Use PERF_TYPE_BREAKPOINT PMU to track read, write, or execute accesses to",
    "    specified addresses. Configure PERF_SAMPLE_REGS_INTR to capture general-purpose",
    "    registers at breakpoint trigger time for instruction analysis.",
    "",
    "    Each process has a maximum of 4 hardware breakpoints.",
    "",
    "ADDRESS FORMAT",
    "    <addr>[/size][:type]",
    "      addr  - Memory address in hex (0x...). Kernel addresses start with 0xffff.",
    "      size  - Access size: 1, 2, 4, or 8 bytes (default: 1)",
    "      type  - Access type: r (read), w (write), x (execute), rw (read/write)",
    "              Default is 'w' for data addresses.",
    "",
    "CALL STACK ANALYSIS",
    "    The -g option enables call stack sampling to identify the code path triggering",
    "    the breakpoint. Use --flame-graph to generate flame graphs for visualization.",
    "",
    "WRITE VALUE TRACKING (x86 kernel addresses only)",
    "    For write breakpoints on x86 kernel addresses, the tool decodes write instructions",
    "    to extract the written value from the source operand, combined with sampled",
    "    registers. This enables tracking value changes over time without reading memory",
    "    directly (which would race with writers).",
    "",
    "    Supported instructions: MOV, CMPXCHG, ADD, SUB, OR, AND, XOR, INC, DEC, NOT, NEG,",
    "    shift/rotate (SHL/SHR/SAL/SAR/ROL/ROR), and bit operations (BTS/BTR/BTC).",
    "    Note: XCHG, XADD are not yet supported.",
    "",
    "    Useful for analyzing spinlock acquire/release paths, reference counting changes,",
    "    and other atomic operations in the kernel.",
    "",
    "FILTER OPTION",
    "    The --filter option filters samples based on the written value, using 'data' as",
    "    the variable referencing the value. E.g., --filter 'data > 0'.",
    "",
    "EXAMPLES",
    "    "PROGRAME" breakpoint 0x7ffd8c7dae28 -g",
    "        Track read/write access to user-space address with call stack.",
    "",
    "    "PROGRAME" breakpoint 0x7ffd8c7dae28/8:w -g --flame-graph bp",
    "        Track 8-byte write access to user-space address, generate flame graph.",
    "",
    "    "PROGRAME" breakpoint 0xffffffff82345678/4:w -g --filter 'data==1'",
    "        Track 4-byte kernel write, filter samples where written value equals 1.");
static const char *breakpoint_argv[] = PROFILER_ARGV("breakpoint",
    PROFILER_ARGV_OPTION,
    PROFILER_ARGV_CALLCHAIN_FILTER, "exclude-user", "exclude-kernel",
    PROFILER_ARGV_PROFILER, "call-graph", "flame-graph", "filter\nEXPR, Filter 'data' for write breakpoints");
static profiler breakpoint = {
    .name = "breakpoint",
    .desc = breakpoint_desc,
    .argv = breakpoint_argv,
    .pages = 1,
    .argc_init = breakpoint_argc_init,
    .init = breakpoint_init,
    .deinit = breakpoint_deinit,
    .print_event = breakpoint_print_event,
    .sample = breakpoint_sample,
};
PROFILER_REGISTER(breakpoint)

