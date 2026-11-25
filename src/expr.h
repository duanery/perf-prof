#ifndef __EXPR_H
#define __EXPR_H

struct symbol_table {
    int token;
    int class;
    char *name;
    long hash;
    int ref;
    int type;
    int nr_elm; //array
    long value;
};

struct expr_global {
    int _cpu;
    int _pid;
    void *data;
    int size;
};

struct expr_prog {
    struct symbol_table *symtab;
    int nr_syms;
    struct expr_global glo; // default global variables
    char *data; //global var
    /*
     * datasize is the allocated capacity, rounded up in 256-byte steps. It is
     * the bound expr_load_data() copies within, and it has to stay a capacity:
     * a tracepoint record has variable-length parts, so the data actually
     * loaded can be longer than what the fixed declarations cover.
     *
     * datalen is the extent the declared globals occupy, i.e. max(offset +
     * size). Range checks that ask "is this address a field?" want this one --
     * datasize would also accept the slack past the last field.
     */
    int datasize;
    int datalen;
    /*
     * Two separate buffers, deliberately not one.
     *
     * names holds copies of the declared globals' names; symtab entries point
     * into it, so it has to outlive the caller's declare[]. str holds only the
     * string literals the expression itself contains. Keeping them apart means
     * "is this address a string literal?" is a plain range test on str, with
     * no risk of a global's name answering yes.
     */
    char *names;
    char *str;
    int strsize;
    long *insn;
    int nr_insn;
    int debug;
};

struct global_var_declare {
    const char *name;
    int offset;
    int size;
    int elementsize;
    bool is_unsigned;
    /*
     * A char array holding text rather than bytes, for consumers that type
     * the fields (python str vs bytes, sql TEXT vs BLOB). Set by the fields of
     * BPF events, from BTF. Tracepoint fields leave it false: those consumers
     * read them through libtraceevent, see tep__string_field_as_binary().
     */
    bool is_string;
};

#define GLOBAL(a,b,c,d) &((struct expr_global){a,b,c,d})

/*
 * Omit the _cpu and _pid globals.
 *
 * Those two are not event fields: they are read from the perf sample header
 * (PERF_SAMPLE_CPU / PERF_SAMPLE_TID) into struct expr_global, so they only
 * exist for events a userspace consumer pulls off a ring buffer. A backend
 * without that header -- notably the BPF filter, which runs in the kernel and
 * sees nothing but the event struct -- passes this so that naming them is an
 * `undefined variable' error. Without it they compile to a load from the
 * userspace address of prog->glo, which is silently wrong rather than refused.
 */
#define EXPR_F_NO_SAMPLE_GLO    (1U << 0)

struct expr_prog *expr_compile(char *expr_str, struct global_var_declare *declare);
struct expr_prog *expr_compile_flags(char *expr_str, struct global_var_declare *declare,
                                     unsigned int flags);
long expr_run(struct expr_prog *prog);
int expr_load_glo(struct expr_prog *prog, const char *name, long value);
int expr_load_data(struct expr_prog *prog, void *d, int size);
int expr_load_global(struct expr_prog *prog, struct expr_global *global);
void expr_destroy(struct expr_prog *prog);
void expr_dump(struct expr_prog *prog);

#ifdef CONFIG_LIBBPF
struct bpf_insn;
/*
 * Translate a compiled expression into BPF instructions, for use as a
 * kernel-side filter on a BPF-generated event. The generated code expects the
 * event pointer in r1 and returns its verdict in r0.
 *
 * Returns a malloc'd array of *nr_insn instructions, or NULL if the
 * expression uses a construct the BPF backend cannot express.
 */
struct bpf_insn *expr_to_bpf(struct expr_prog *prog, int *nr_insn);
void expr_bpf_dump(struct bpf_insn *insn, int nr_insn);
#endif


#endif

