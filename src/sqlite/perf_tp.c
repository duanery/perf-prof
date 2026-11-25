/*
 * perf_tp.c - SQLite interface for perf tracepoint events
 *
 * This module provides two storage backends for tracepoint events:
 *
 * 1. File mode (sql_tp_file):
 *    - Events are inserted into SQLite tables using sqlite3_bind_*() + INSERT
 *    - Suitable for persistent storage to database files
 *    - Higher overhead due to SQLite's bind operations for each column.
 *
 * 2. Memory mode (sql_tp_mem):
 *    - Events are stored in a linked list (mem-copy from perf ring buffer)
 *    - Accessed via SQLite Virtual Table interface
 *    - Fields are read on-demand during query execution
 *    - Lower overhead: avoids sqlite3_bind_*() for unused columns
 *
 * Architecture:
 *
 *   File mode:                      Memory mode:
 *   ──────────                      ───────────
 *   perf_event                      perf_event
 *       │                               │
 *       ▼                               ▼
 *   sqlite3_bind_*()               memcpy to list
 *       │                               │
 *       ▼                               ▼
 *   SQLite B-tree                  Virtual Table
 *       │                               │
 *       ▼                               ▼
 *   SQL Query                      SQL Query
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <monitor.h>
#include <tep.h>
#include <linux/kernel.h>
#include <linux/rblist.h>
#include <linux/bitmap.h>
#include <event-parse-local.h>
#include <stack_helpers.h>
#include <arpa/inet.h>

#include <sqlite/ext.h>

extern const char *syscalls_table[];
extern const int syscalls_table_size;

/*
 * SQL scalar functions for kernel pointer format specifiers (%p extensions).
 * These functions convert raw binary data to human-readable strings.
 *
 * Format    Function      Description
 * ──────    ────────      ───────────
 * %pS,%ps   ksymbol()     Kernel symbol name from address
 * %pI4      ipv4_str()    IPv4 address (network byte order)
 * %pi4h     ipv4_hstr()   IPv4 address (host byte order)
 * %pI6      ipv6_str()    IPv6 address
 * %pIS      ipsa_str()    Sockaddr (network byte order)
 * %pISh     ipsa_hstr()   Sockaddr (host byte order)
 * %pUb      uuid_str()    UUID (big endian)
 * %pUl      guid_str()    GUID (little endian)
 * %pM       mac_str()     MAC address
 * (special) syscall()     Syscall number to name
 */
#define ARG_POINTER_FUNC \
    FUNC(KSYMBOL, "ksymbol"), \
    FUNC(IPV4_STR, "ipv4_str"), \
    FUNC(IPV4_HSTR, "ipv4_hstr"), \
    FUNC(IPV6_STR, "ipv6_str"), \
    FUNC(IPSA_STR, "ipsa_str"), \
    FUNC(IPSA_HSTR, "ipsa_hstr"), \
    FUNC(UUID_STR, "uuid_str"), \
    FUNC(GUID_STR, "guid_str"), \
    FUNC(MAC_STR, "mac_str"), \
    FUNC(SYSCALL, "syscall")

#define FUNC(enum_name, func_name) enum_name
enum {
    ARG_POINTER_FUNC,
    ARG_POINTER_MAX,
};
#undef FUNC

#define FUNC(enum_name, func_name) func_name
const char *arg_pointer_func[] = {
    ARG_POINTER_FUNC,
};
#undef FUNC

struct symbolic_node {
    struct rb_node rbnode;
    union {
        uint64_t event_field;
        struct {
            int event_id;
            int field_offset;
        };
    };
    uint64_t value;
    const char *str;
};

static int symbolic_node_cmp(struct rb_node *rbn, const void *entry)
{
    struct symbolic_node *node = container_of(rbn, struct symbolic_node, rbnode);
    const struct symbolic_node *e = entry;

    if (node->event_field > e->event_field)
        return 1;
    else if (node->event_field < e->event_field)
        return -1;
    else {
        if (node->value > e->value)
            return 1;
        else if (node->value < e->value)
            return -1;
        else
            return 0;
    }
}

static struct rb_node *symbolic_node_new(struct rblist *rlist, const void *new_entry)
{
    struct symbolic_node *node = malloc(sizeof(*node));
    if (node) {
        const struct symbolic_node *e = new_entry;
        node->event_field = e->event_field;
        node->value = e->value;
        /* Note: str points to TEP internal data structure.
         * It's safe as long as TEP handle lifetime covers the entire program execution.
         * No string duplication for performance reasons. */
        node->str = e->str;
        RB_CLEAR_NODE(&node->rbnode);
    }
    return node ? &node->rbnode : NULL;
}

static void symbolic_node_delete(struct rblist *rblist, struct rb_node *rb_node)
{
    struct symbolic_node *node = container_of(rb_node, struct symbolic_node, rbnode);
    free(node);
}

static void symbolic_update(struct sql_tp_ctx *ctx, int event_id, int field_offset,
                            const char *value_str, const char *str)
{
    char *endptr = NULL;
    uint64_t value = strtoull(value_str, &endptr, 0);
    struct symbolic_node key;

    if (endptr == value_str || *endptr != '\0') {
        // Not a valid number
        return;
    }

    key.event_id = event_id;
    key.field_offset = field_offset;
    key.value = value;
    key.str = str;
    rblist__findnew(&ctx->symbolic_table, &key);
}

static const char *symbolic_lookup(struct sql_tp_ctx *ctx, int event_id, int field_offset,
                                   uint64_t value)
{
    struct symbolic_node key, *node;
    struct rb_node *rbn;

    key.event_id = event_id;
    key.field_offset = field_offset;
    key.value = value;
    rbn = rblist__find(&ctx->symbolic_table, &key);
    if (rbn) {
        node = container_of(rbn, struct symbolic_node, rbnode);
        return node->str;
    } else
        return NULL;
}

struct symbolic_ctx {
    struct sql_tp_ctx *ctx;
    int event_id;
    int do_update;
    int nr_print_symbolic;
    const char *field_name;

    // Only for kvm:* events, to evaluate the isa expression.
    // (REC->isa == 1) ? __print_symbolic() : __print_symbolic()
    int isa;
    int has_isa;
    int has_unknown;
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"

static unsigned long
tep_try_eval_num_arg(struct tep_print_arg *arg, struct symbolic_ctx *sym_ctx)
{
    unsigned long val = -1;
    unsigned long left, right;

    switch (arg->type) {
    case TEP_PRINT_NULL: // ~ ! + -
        return 0;
    case TEP_PRINT_ATOM:
        return strtoul(arg->atom.atom, NULL, 0);
    case TEP_PRINT_FIELD:
        if (strcmp(arg->field.name, "isa") == 0) {
            sym_ctx->has_isa = 1;
            return sym_ctx->isa;
        }
        sym_ctx->has_unknown++;
        return val;
    case TEP_PRINT_OP:
        if (arg->op.op[0] == '[' || arg->op.op[0] == '?')
            return val;

        left = tep_try_eval_num_arg(arg->op.left, sym_ctx);
        right = tep_try_eval_num_arg(arg->op.right, sym_ctx);
        switch (arg->op.op[0]) {
        case '!':
            switch (arg->op.op[1]) {
            case 0: val = !right; break;
            case '=': val = left != right; break;
            default: goto out;
            } break;
        case '~': val = ~right; break;
        case '|': if (arg->op.op[1]) val = left || right;
                  else val = left | right;
            break;
        case '&': if (arg->op.op[1]) val = left && right;
                  else val = left & right;
            break;
        case '<':
            switch (arg->op.op[1]) {
            case 0: val = left < right; break;
            case '<': val = left << right; break;
            case '=': val = left <= right; break;
            default: goto out;
            } break;
        case '>':
            switch (arg->op.op[1]) {
            case 0: val = left > right; break;
            case '>': val = left >> right; break;
            case '=': val = left >= right; break;
            default: goto out;
            } break;
        case '=': if (arg->op.op[1] != '=') goto out;
                  val = left == right; break;
        case '-': val = left - right; break;
        case '+': val = left + right; break;
        case '/': val = left / right; break;
        case '%': val = left % right; break;
        case '*': val = left * right; break;
        default: goto out;
        }
        break;
    default:
        break;
    }
out:
    return val;
}

static void tep_symbolic(struct tep_print_arg *arg, struct symbolic_ctx *sym_ctx)
{
    switch (arg->type) {
    case TEP_PRINT_SYMBOL: {
        struct tep_print_arg *field = arg->symbol.field;
        struct tep_print_flag_sym *symbols = arg->symbol.symbols;

        /* Only handle simple case: __print_symbolic(REC->field, ...)
         * Where the first argument is a direct field reference (TEP_PRINT_FIELD).
         *
         * Supported: __print_symbolic(REC->vec, {0, "str0"}, {1, "str1"}, ...)
         * Not supported: __print_symbolic((REC->dm >> 8 & 0x7), ...)
         *                (complex expressions with bit operations)
         *
         * This ensures we can unambiguously map the field offset to symbolic values. */
        if (field && field->type == TEP_PRINT_FIELD) {
            sym_ctx->nr_print_symbolic++;
            if (sym_ctx->do_update) {
                int field_offset = field->field.field->offset;
                sym_ctx->field_name = field->field.field->name;
                /* Register all value->string mappings for this field */
                while (symbols) {
                    symbolic_update(sym_ctx->ctx, sym_ctx->event_id, field_offset,
                                    symbols->value, symbols->str);
                    symbols = symbols->next;
                }
                sym_ctx->ctx->nr_symbolic++;
                if (sym_ctx->ctx->nr_symbolic == 1) {
                    sym_ctx->ctx->event_id = sym_ctx->event_id;
                    sym_ctx->ctx->field_offset = field_offset;
                    sym_ctx->ctx->field_name = sym_ctx->field_name;
                }
            }
        }
        break;
    }
    case TEP_PRINT_OP: {
        struct tep_print_arg *left = arg->op.left;
        unsigned long val;

        /* The only op for __print_symbolic string should be ? :
         * Example: (REC->isa == 1) ? __print_symbolic(...) : __print_symbolic(...) */
        if (arg->op.op[0] != '?')
            break;

        arg = arg->op.right;
        sym_ctx->has_isa = sym_ctx->has_unknown = 0;
        val = tep_try_eval_num_arg(left, sym_ctx);

        /* Special handling for kvm events with isa-based conditional symbolic.
         * If the condition can be evaluated at registration time (has_isa && !has_unknown),
         * only process the matching branch. Otherwise, process both branches.
         *
         * Example: kvm:kvm_exit has (REC->isa == 1) ? VMX_reasons : SVM_reasons */
        if (sym_ctx->has_isa && !sym_ctx->has_unknown) {
            /* Condition is evaluable: process only the matching branch */
            if (val) tep_symbolic(arg->op.left, sym_ctx);
            else tep_symbolic(arg->op.right, sym_ctx);
        } else {
            /* Condition has unknown fields: process both branches */
            tep_symbolic(arg->op.left, sym_ctx);
            tep_symbolic(arg->op.right, sym_ctx);
        }
        break;
    }
    default:
        break;
    }
}
#pragma GCC diagnostic pop

static void symbolic_register(struct sql_tp_ctx *ctx)
{
    struct tp *tp;
    int i;
    int vendor = get_cpu_vendor();
    int isa = 0;

    if (vendor == X86_VENDOR_INTEL) isa = 1;
    else if (vendor == X86_VENDOR_AMD || vendor == X86_VENDOR_HYGON) isa = 2;

    for_each_real_tp(ctx->tp_list, tp, i) {
        struct tp_private *priv = tp->private;
        struct tep_event *event = tep_find_event(ctx->tep, tp->id);
        if (event && event->print_fmt.args) {
            struct tep_print_arg *arg = event->print_fmt.args;
            while (arg) {
                struct symbolic_ctx sym_ctx = {ctx, tp->id, 0, 0, NULL, isa, 0, 0};
                int nr_entries, ret;

                tep_symbolic(arg, &sym_ctx);
                /* Only register symbolic() function when there's exactly one __print_symbolic.
                 * Multiple __print_symbolic in one format would require more complex parsing
                 * to determine which field maps to which symbolic table - not supported yet.
                 * This check ensures we only handle the simple, unambiguous case. */
                if (sym_ctx.nr_print_symbolic == 1) {
                    nr_entries = rblist__nr_entries(&ctx->symbolic_table);
                    sym_ctx.do_update = 1;
                    tep_symbolic(arg, &sym_ctx);

                    if (nr_entries != rblist__nr_entries(&ctx->symbolic_table)) {
                        char *function_list = NULL;
                        const char *prefix = priv->function_list ? : "";
                        const char *separator = priv->function_list ? ", " : "";
                        ret = asprintf(&function_list, "%s%ssymbolic('%s.%s', %s)", prefix, separator,
                                        tp->name, sym_ctx.field_name, sym_ctx.field_name);
                        if (ret > 0) {
                            if (priv->function_list)
                                free(priv->function_list);
                            priv->function_list = function_list;
                        }
                    }
                }
                arg = arg->next;
            }
        }
    }
}

static void arg_pointer_register(struct sql_tp_ctx *ctx)
{
    struct tp *tp;
    int i, ret;
    char *function_list;

    for_each_real_tp(ctx->tp_list, tp, i) {
        struct tp_private *priv = tp->private;
        struct tep_event *event = tep_find_event(ctx->tep, tp->id);
        struct tep_print_parse *parse = event ? event->print_fmt.print_cache : NULL;
        while (parse) {
            if (parse->type == PRINT_FMT_ARG_POINTER &&
                parse->arg && parse->arg->type == TEP_PRINT_FIELD) {
                const char *format = parse->format;
                const char *field_name = parse->arg->field.name;
                int func = -1, data_type = 0;

                while (*format) if (*format++ == 'p') break;
                switch (*format) {
                    case 'F': // %pS %ps %pF %pf
                    case 'f':
                    case 'S':
                    case 's': func = KSYMBOL; data_type = SQLITE_INTEGER; ctx->ksymbol = 1; break;
                    case 'I':
                    case 'i': {
                        switch (format[1]) {
                            case '4': // %pI4, %pi4, %p[Ii]4[hnbl]
                                func = (format[2] == 'h' || format[2] == 'l') ? IPV4_HSTR : IPV4_STR;
                                data_type = SQLITE_BLOB;
                                break;
                            case '6': // %pI6, %pi6, %pI6c
                                func = IPV6_STR; data_type = SQLITE_BLOB;
                                break;
                            case 'S': { // %pIS, %piS, %pISc, %pISpc, %p[Ii]S[pfschnbl]
                                char *fmt = (char *)&format[1];
                                func = IPSA_STR; data_type = SQLITE_BLOB;
                                while (*++fmt)
                                    if (*fmt == 'h' || *fmt == 'l') { // host endian
                                        func = IPSA_HSTR;
                                        break;
                                    }
                                break;
                            }
                            default: break;
                        }
                        break;
                    }
                    case 'U': // %pUb %pUB %pUl %pUL little endian(L,l) big endian(B,b)
                        func = (format[1] == 'L' || format[1] == 'l') ? GUID_STR : UUID_STR;
                        data_type = SQLITE_BLOB;
                        break;
                    case 'M': // %pM %pMR %pMF %pm %pmR
                    case 'm': func = MAC_STR; data_type = SQLITE_BLOB; break;
                    default: break;
                }

                if (func >= 0) {
                    const char *prefix = priv->function_list ? : "";
                    const char *separator = priv->function_list ? ", " : "";
                    ret = asprintf(&function_list, "%s%s%s(%s)", prefix, separator, arg_pointer_func[func], field_name);
                    if (ret > 0) {
                        if (priv->function_list)
                            free(priv->function_list);
                        priv->function_list = function_list;
                        ctx->sqlite_funcs[func].data_type = data_type;
                        ctx->sqlite_funcs[func].func_name = arg_pointer_func[func];
                    }
                }
            }
            parse = parse->next;
        }
    }

    if (ctx->ksymbol)
        function_resolver_ref();
}

static void sqlite_symbolic(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    struct sql_tp_ctx *ctx = (struct sql_tp_ctx *)sqlite3_user_data(context);
    const char *table_field_name;
    long long value;
    int table_name_len = 0;
    const char *field_name;
    const char *symbol = NULL;
    struct tp *tp;
    int type0, type1;
    int i, j;

    /* Validate argument count */
    if (argc > 2) {
        sqlite3_result_error(context, "symbolic() requires exactly 1 or 2 arguments", -1);
        return;
    }

    type0 = sqlite3_value_type(argv[0]);

    if (argc == 1) {
        /* Handle NULL input - SQL null propagation */
        if (type0 == SQLITE_NULL) {
            sqlite3_result_null(context);
            return;
        }
        /* Validate first argument type (value) */
        if (type0 != SQLITE_INTEGER) {
            sqlite3_result_error(context, "symbolic() argument must be INTEGER", -1);
            return;
        }
        value = sqlite3_value_int64(argv[0]);
        symbol = symbolic_lookup(ctx, ctx->event_id, ctx->field_offset, value);
        goto found;
    }

    type1 = sqlite3_value_type(argv[1]);

    /* Handle NULL input - SQL null propagation */
    if (type0 == SQLITE_NULL || type1 == SQLITE_NULL) {
        sqlite3_result_null(context);
        return;
    }

    /* Validate first argument type (table_field_name) */
    if (type0 != SQLITE_TEXT) {
        sqlite3_result_error(context, "symbolic() first argument must be TEXT", -1);
        return;
    }

    /* Validate second argument type (value) */
    if (type1 != SQLITE_INTEGER) {
        sqlite3_result_error(context, "symbolic() second argument must be INTEGER", -1);
        return;
    }

    table_field_name = (const char *)sqlite3_value_text(argv[0]);
    value = sqlite3_value_int64(argv[1]);

    /* Check for NULL table_field_name */
    if (!table_field_name) {
        sqlite3_result_error(context, "symbolic() table_field_name is NULL", -1);
        return;
    }

    field_name = strchr(table_field_name, '.'); // "table_name.field_name"
    if (field_name) {
        field_name++; // skip '.'
        table_name_len = field_name - table_field_name - 1;
    } else
        field_name = table_field_name;

    for_each_real_tp(ctx->tp_list, tp, i) {
        struct tp_private *tp_priv = tp->private;
        int match = 0;

        if (!table_name_len) {
            /* No table name specified, match all tables */
            match = 1;
        } else {
            /* Exact match: ensure the table name is followed by '\0' or '.' */
            if ((strncmp(tp->name, table_field_name, table_name_len) == 0 &&
                 tp->name[table_name_len] == '\0') ||
                (strncmp(tp_priv->table_name, table_field_name, table_name_len) == 0 &&
                 tp_priv->table_name[table_name_len] == '\0')) {
                match = 1;
            }
        }

        if (match) {
            /* tep fields only: a BPF event has no print_fmt to give symbols */
            for (j = 0; tp_priv->fields && tp_priv->fields[j]; j++) {
                struct tep_format_field *field = tp_priv->fields[j];
                if (strcmp(field->name, field_name) == 0) {
                    symbol = symbolic_lookup(ctx, tp->id, field->offset, value);
                    goto found;
                }
            }
        }
    }

found:
    if (symbol)
        sqlite3_result_text(context, symbol, -1, SQLITE_STATIC);
    else
        sqlite3_result_text(context, "UNKNOWN", -1, SQLITE_STATIC);
}

static void sqlite_ksymbol(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    long func = (long)(long *)sqlite3_user_data(context);
    long long value;
    const char *symbol = NULL;
    int type0;

    /* Validate argument count */
    if (argc != 1) {
        sqlite3_result_error(context, "ksymbol() requires exactly 1 argument", -1);
        return;
    }

    type0 = sqlite3_value_type(argv[0]);

    /* Handle NULL input - SQL null propagation */
    if (type0 == SQLITE_NULL) {
        sqlite3_result_null(context);
        return;
    }

    /* Validate argument type (value) */
    if (type0 != SQLITE_INTEGER) {
        sqlite3_result_error(context, "ksymbol() argument must be INTEGER", -1);
        return;
    }

    value = sqlite3_value_int64(argv[0]);
    if (func == KSYMBOL)
        symbol = function_resolver(NULL, (unsigned long long *)&value, NULL);
    else if (func == SYSCALL) {
        if (value >= 0 && value < syscalls_table_size && syscalls_table[value])
            symbol = syscalls_table[value];
    }

    if (symbol)
        sqlite3_result_text(context, symbol, -1, SQLITE_STATIC);
    else
        sqlite3_result_text(context, "??", -1, SQLITE_STATIC);
}

static void sqlite_blob(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    long func = (long)(long *)sqlite3_user_data(context);
    const void *value;
    int bytes;
    char buf[256];
    int len = -1;
    uint32_t addr;
    const unsigned char *u;
    char *symbol = NULL;
    int type0;

    /* Validate argument count */
    if (argc != 1) {
        snprintf(buf, sizeof(buf), "%s() requires exactly 1 argument", arg_pointer_func[func]);
        sqlite3_result_error(context, buf, -1);
        return;
    }

    type0 = sqlite3_value_type(argv[0]);

    /* Handle NULL input - SQL null propagation */
    if (type0 == SQLITE_NULL) {
        sqlite3_result_null(context);
        return;
    }

    /* Validate argument type */
    if (type0 != SQLITE_BLOB) {
        snprintf(buf, sizeof(buf), "%s() argument must be BLOB", arg_pointer_func[func]);
        sqlite3_result_error(context, buf, -1);
        return;
    }

    value = sqlite3_value_blob(argv[0]);
    bytes = sqlite3_value_bytes(argv[0]);

    switch (func) {
        case IPV4_STR: // network endian
            if (bytes != 4) break;
            symbol = (char *)inet_ntop(AF_INET, value, buf, sizeof(buf));
            break;
        case IPV4_HSTR: // host endian
            if (bytes != 4) break;
            addr = htonl(*(uint32_t *)value);
            symbol = (char *)inet_ntop(AF_INET, &addr, buf, sizeof(buf));
            break;
        case IPV6_STR:
            if (bytes != 16) break;
            symbol = (char *)inet_ntop(AF_INET6, value, buf, sizeof(buf));
            break;
        case IPSA_STR: // network endian
        case IPSA_HSTR: { // host endian
            struct sockaddr *sa = (struct sockaddr *)value;

            if (sa->sa_family == AF_INET) {
                struct sockaddr_in *sin = (struct sockaddr_in *)sa;

                if (bytes < sizeof(struct sockaddr_in)) break;
                addr = func == IPSA_STR ?
                       sin->sin_addr.s_addr : htonl(sin->sin_addr.s_addr);
                if (inet_ntop(AF_INET, &addr, buf, sizeof(buf))) {
                    len = asprintf(&symbol, "%s:%d", buf, ntohs(sin->sin_port));
                    if (len < 0) symbol = NULL;
                }
            } else if (sa->sa_family == AF_INET6) {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sa;

                if (bytes < sizeof(struct sockaddr_in6)) break;
                if (inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf))) {
                    len = asprintf(&symbol, "[%s]:%d", buf, ntohs(sin6->sin6_port));
                    if (len < 0) symbol = NULL;
                }
            }
            break;
        }
        case UUID_STR:
            if (bytes != 16) break;
            u = value;
            len = snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            if (len > 0) symbol = buf;
            break;
        case GUID_STR:
            if (bytes != 16) break;
            u = value;
            len = snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    u[3], u[2], u[1], u[0], u[5], u[4], u[7], u[6], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            if (len > 0) symbol = buf;
            break;
        case MAC_STR:
            if (bytes != 6) break;
            u = value;
            len = snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", u[0], u[1], u[2], u[3], u[4], u[5]);
            if (len > 0) symbol = buf;
            break;
        default: break;
    }

    if (symbol)
        sqlite3_result_text(context, symbol, len, symbol == buf ? SQLITE_TRANSIENT : free);
    else
        sqlite3_result_text(context, "??", -1, SQLITE_STATIC);
}

/*
 * Common initialization for both file and memory modes.
 * Sets up tp_private for each tracepoint and registers SQL functions.
 */
static struct sql_tp_ctx *sql_tp_common_init(sqlite3 *sql, struct tp_list *tp_list)
{
    struct sql_tp_ctx *ctx;
    struct tp *tp;
    int i, j;
    /* Stack-allocated array, only used during initialization */
    struct sqlite_func sqlite_funcs[ARG_POINTER_MAX];

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->sql = sql;
    ctx->tp_list = tp_list;
    ctx->tep = tep__ref_light();
    if (!ctx->tep)
        goto failed;

    rblist__init(&ctx->symbolic_table);
    ctx->symbolic_table.node_cmp = symbolic_node_cmp;
    ctx->symbolic_table.node_new = symbolic_node_new;
    ctx->symbolic_table.node_delete = symbolic_node_delete;

    memset(sqlite_funcs, 0, sizeof(sqlite_funcs));
    ctx->sqlite_funcs = sqlite_funcs;

    for_each_real_tp(ctx->tp_list, tp, i) {
        struct tep_event *event = tep_find_event(ctx->tep, tp->id);
        struct tep_format_field **fields;
        struct tp_private *priv = tp->private;

        if (!priv) {
            priv = calloc(1, sizeof(struct tp_private));
            if (!priv)
                goto failed;
            INIT_LIST_HEAD(&priv->event_list);
            priv->index_tree = RB_ROOT;
            tp->private = priv;

            if (tp_is_bpf(tp)) {
                /* bpf:xxx: the event struct from BTF, no trace_entry */
                priv->bpf_fields = tep__event_fields(tp->id);
                if (!priv->bpf_fields)
                    goto failed;
                for (j = 0; priv->bpf_fields[j].name; j++)
                    ;
                priv->nr_sys = 5;
            } else {
                fields = event ? tep_event_fields(event) : NULL;
                priv->fields = fields;

                for (j = 0; fields && fields[j]; j++) {
                    if ((fields[j]->flags & TEP_FIELD_IS_STRING) &&
                        tep__string_field_as_binary(event, fields[j])) {
                        fields[j]->flags &= ~TEP_FIELD_IS_STRING;
                    }
                }
                priv->nr_sys = 8;
            }
            /*
            * nr_fields = nr_sys system columns + j event-specific columns
            * System columns: _pid, _tid, _time, _cpu, _period,
            *                 common_flags, common_preempt_count, common_pid (tracepoints only)
            */
            priv->nr_fields = priv->nr_sys + j;
            priv->table_name = tp->alias ? tp->alias : tp->name;
            priv->mode = FILE_MODE;
        } else {
            if (priv->function_list) {
                free(priv->function_list);
                priv->function_list = NULL;
            }
        }

        ctx->verbose = tp->dev->env->verbose;
        if (strcmp(tp->sys, "raw_syscalls") == 0 || strcmp(tp->sys, "syscalls") == 0) {
            ctx->sqlite_funcs[SYSCALL].data_type = SQLITE_INTEGER;
            ctx->sqlite_funcs[SYSCALL].func_name = arg_pointer_func[SYSCALL];
            priv->function_list = strdup(strcmp(tp->sys, "raw_syscalls") == 0 ?
                                         "syscall(id)" : "syscall(__syscall_nr)");
        }
    }

    /* Register symbolic() function */
    symbolic_register(ctx);

    /* Register arg pointer functions */
    arg_pointer_register(ctx);

    if (ctx->nr_symbolic > 0 &&
        sqlite3_create_function(ctx->sql, "symbolic", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_DIRECTONLY,
                                ctx, sqlite_symbolic, NULL, NULL) != SQLITE_OK)
        goto failed;

    if (ctx->nr_symbolic == 1) {
        if (sqlite3_create_function(ctx->sql, "symbolic", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_DIRECTONLY,
                                    ctx, sqlite_symbolic, NULL, NULL) != SQLITE_OK)
            goto failed;

        for_each_real_tp(ctx->tp_list, tp, i) {
            struct tp_private *priv = tp->private;
            if (tp->id == ctx->event_id) {
                char *function_list = NULL;
                const char *prefix = priv->function_list ? : "";
                const char *separator = priv->function_list ? ", " : "";
                if (asprintf(&function_list, "%s%ssymbolic(%s)", prefix, separator,
                            ctx->field_name) > 0) {
                    if (priv->function_list)
                        free(priv->function_list);
                    priv->function_list = function_list;
                }
                break;
            }
        }
    }

    for (i = 0; i < ARG_POINTER_MAX; i++) {
        if (ctx->sqlite_funcs[i].func_name) {
            void *func = ctx->sqlite_funcs[i].data_type == SQLITE_BLOB ? sqlite_blob : sqlite_ksymbol;
            if (sqlite3_create_function(ctx->sql, ctx->sqlite_funcs[i].func_name, 1,
                                        SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_DIRECTONLY,
                                       (void *)(unsigned long)i, func, NULL, NULL) != SQLITE_OK)
                goto failed;
        }
    }

    ctx->sqlite_funcs = NULL;
    return ctx;

failed:
    sql_tp_free(ctx);
    return NULL;
}

static const char *ColumnName(struct tp_private *priv, int i)
{
    static const char *sys_columns[] = {
        // common fields
        "_pid", "_tid", "_time", "_cpu", "_period",
        // common_* (common_type removed - use event_id from event_metadata table)
        "common_flags", "common_preempt_count", "common_pid",
    };

    if (i >= 0 && i < priv->nr_sys)
        return sys_columns[i];
    if (i >= priv->nr_sys && i < priv->nr_fields)
        return priv->bpf_fields ? priv->bpf_fields[i - priv->nr_sys].name :
                                  priv->fields[i - priv->nr_sys]->name;
    return "unknown";
}

/*
 * SQLite type of column i, the same in all storage modes. SQLITE_NULL: no
 * such column.
 *
 * 1. TEXT: String fields
 *    - char comm[16];            offset:8;  size:16; -> TEXT
 *    - __data_loc char[] cmd;    offset:40; size:4;  -> TEXT
 *    Tracepoints: IS_STRING flag. Fields requiring special format (e.g., %pI4, %pM)
 *    have IS_STRING flag cleared in sql_tp_common_init() to be treated as BLOB.
 *    Note: IS_SIGNED flag is no longer reliable for char arrays because:
 *    - arm64 kernel: char is unsigned by default
 *    - kernel commit 3bc753c06dd0 ("kbuild: treat char as always unsigned")
 *    BPF events: char arrays (is_string from BTF), not u8 arrays.
 *
 * 2. BLOB: Array fields without string semantics
 *    - __u8 saddr[4];            offset:28; size:4;  -> BLOB
 *    - long sysctl_mem[3];       offset:40; size:24; -> BLOB
 *    Arrays without string semantics are stored as binary data.
 *
 * 3. INTEGER: All other fields (numeric types), and the system columns
 *    - int pid;                  offset:24; size:4;  -> INTEGER
 */
static int ColumnType(struct tp_private *priv, int i)
{
    if (i < priv->nr_sys)
        return SQLITE_INTEGER;
    if (i >= priv->nr_fields)
        return SQLITE_NULL;
    if (priv->bpf_fields) {
        event_fields *field = &priv->bpf_fields[i - priv->nr_sys];
        if (field->is_string)
            return SQLITE_TEXT;
        if (field->size != field->elementsize)
            return SQLITE_BLOB;
    } else {
        struct tep_format_field *field = priv->fields[i - priv->nr_sys];
        if (field->flags & TEP_FIELD_IS_STRING)
            return SQLITE_TEXT;
        if (field->flags & TEP_FIELD_IS_ARRAY)
            return SQLITE_BLOB;
    }
    return SQLITE_INTEGER;
}

static const char *ColumnTypeName(struct tp_private *priv, int i)
{
    switch (ColumnType(priv, i)) {
        case SQLITE_TEXT: return "TEXT";
        case SQLITE_BLOB: return "BLOB";
        default: return "INTEGER";
    }
}

/*
 * Bind or return a field of a BPF event, typed as ColumnType() says. A string
 * fills its array without a terminator at most.
 */
static void bpf_field_bind(sqlite3_stmt *stmt, int idx, event_fields *field, void *base)
{
    void *ptr = base + field->offset;

    if (field->is_string)
        sqlite3_bind_text(stmt, idx, ptr, strnlen(ptr, field->size), SQLITE_STATIC);
    else if (field->size != field->elementsize)
        sqlite3_bind_blob(stmt, idx, ptr, field->size, SQLITE_STATIC);
    else
        sqlite3_bind_int64(stmt, idx, event_field_scalar(field, base));
}

static void bpf_field_result(sqlite3_context *ctx, event_fields *field, void *base)
{
    void *ptr = base + field->offset;

    if (field->is_string)
        sqlite3_result_text(ctx, ptr, strnlen(ptr, field->size), SQLITE_STATIC);
    else if (field->size != field->elementsize)
        sqlite3_result_blob(ctx, ptr, field->size, SQLITE_STATIC);
    else
        sqlite3_result_int64(ctx, event_field_scalar(field, base));
}

static int sql_create_table(struct sql_tp_ctx *ctx)
{
    struct tp *tp;
    int i;
    const char *table_fmt = "DROP TABLE IF EXISTS %s; CREATE TABLE %s (%s);";
    const char *insert_fmt = "INSERT INTO %s VALUES(%s);";
    char buf[1024 + 256 + strlen(table_fmt)];
    char *errmsg = NULL;

    for_each_real_tp(ctx->tp_list, tp, i) {
        struct tp_private *priv = (struct tp_private *)tp->private;
        char col_buf[1024 + 256];
        char ins_buf[512];
        int j, col_len = 0, ins_len = 0;

        /* System columns and raw data columns, typed by ColumnType() */
        for (j = 0; j < priv->nr_fields; j++) {
            col_len += scnprintf(col_buf + col_len, sizeof(col_buf) - col_len,
                            "%s%s %s", j ? ", " : "", ColumnName(priv, j), ColumnTypeName(priv, j));
            if (!priv->insert_stmt)
                ins_len += scnprintf(ins_buf + ins_len, sizeof(ins_buf) - ins_len,
                                "%s?", j ? ", " : "");
        }
        col_buf[col_len] = '\0';
        ins_buf[ins_len] = '\0';

        snprintf(buf, sizeof(buf), table_fmt, priv->table_name, priv->table_name, col_buf);
        if (ctx->verbose)
            printf("CREATE SQL: %s\n", buf);
        if (sqlite3_exec(ctx->sql, buf, NULL, NULL, &errmsg) != SQLITE_OK) {
            fprintf(stderr, "Failed to create table %s: %s\n", priv->table_name, errmsg);
            sqlite3_free(errmsg);
            return -1;
        }
        priv->created_time = time(NULL);

        if (priv->insert_stmt)
            continue;

        snprintf(buf, sizeof(buf), insert_fmt, priv->table_name, ins_buf);
        if (ctx->verbose)
            printf("INSERT SQL: %s\n", buf);

        if (sqlite3_prepare_v3(ctx->sql, buf, -1, SQLITE_PREPARE_PERSISTENT, &priv->insert_stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "Failed to prepare insert statement for %s: %s\n", priv->table_name, sqlite3_errmsg(ctx->sql));
            return -1;
        }
    }
    return 0;
}

static int sql_tp_file_sample(struct sql_tp_ctx *ctx, struct tp *tp, union perf_event *event)
{
    struct tp_private *priv = tp->private;
    struct sql_sample_type *data = (void *)event->sample.array;
    int idx = 1;
    int i, ret = -1;

    // Reset the statement for new use
    sqlite3_reset(priv->insert_stmt);

    // Bind common fields
    sqlite3_bind_int(priv->insert_stmt, idx++, data->tid_entry.pid);
    sqlite3_bind_int(priv->insert_stmt, idx++, data->tid_entry.tid);
    sqlite3_bind_int64(priv->insert_stmt, idx++, data->time);
    sqlite3_bind_int(priv->insert_stmt, idx++, data->cpu_entry.cpu);
    sqlite3_bind_int64(priv->insert_stmt, idx++, data->period);

    // common_* (common_type removed - use event_id from event_metadata table)
    // A BPF event has no trace_entry.
    if (!priv->bpf_fields) {
        sqlite3_bind_int(priv->insert_stmt, idx++, data->raw.common.common_flags);
        sqlite3_bind_int(priv->insert_stmt, idx++, data->raw.common.common_preempt_count);
        sqlite3_bind_int(priv->insert_stmt, idx++, data->raw.common.common_pid);
    }

    /* Parse and bind event-specific fields
        *
        * Field type handling must match the SQLite type mapping in ColumnType():
        *
        * 1. TEXT binding: String fields (IS_STRING flag)
        *    - char comm[16];         -> sqlite3_bind_text()
        *    - __data_loc char[] cmd; -> sqlite3_bind_text() with dynamic offset
        *
        * 2. BLOB binding: Array fields (IS_ARRAY flag, without IS_STRING)
        *    - __u8 saddr[4];         -> sqlite3_bind_blob()
        *    - __data_loc __u8[] buf  -> sqlite3_bind_blob() with dynamic offset
        *
        * 3. INTEGER binding: Numeric fields (default)
        *    - int pid;               -> sqlite3_bind_int64()
        *
        * BPF events: bpf_field_bind().
        */
    for (i = 0; priv->bpf_fields && priv->bpf_fields[i].name; i++)
        bpf_field_bind(priv->insert_stmt, idx++, &priv->bpf_fields[i], data->raw.data);
    for (i = 0; priv->fields && priv->fields[i]; i++) {
        struct tep_format_field *field = priv->fields[i];
        void *base = data->raw.data;
        long long val = 0;
        void *ptr;
        int len;

        if ((field->flags & TEP_FIELD_IS_STRING)) {
            // TEXT: String fields (IS_STRING flag)
            if (field->flags & TEP_FIELD_IS_DYNAMIC) {
                // Dynamic string: __data_loc char[] field
                ptr = base + *(unsigned short *)(base + field->offset);
                len = *(unsigned short *)(base + field->offset + sizeof(unsigned short));
            } else {
                // Fixed string: char field[N]
                ptr = base + field->offset;
                len = -1;
            }
            // Use SQLITE_STATIC for zero-copy: data valid during sqlite3_step()
            sqlite3_bind_text(priv->insert_stmt, idx++, ptr, len, SQLITE_STATIC);
        } else if (field->flags & TEP_FIELD_IS_ARRAY) {
            // BLOB: Array fields without string semantics
            if (field->flags & TEP_FIELD_IS_DYNAMIC) {
                // Dynamic array: __data_loc __u8[] buf
                ptr = base + *(unsigned short *)(base + field->offset);
                len = *(unsigned short *)(base + field->offset + sizeof(unsigned short));
            } else {
                // Fixed array: __u8 saddr[4], long sysctl_mem[3]
                ptr = base + field->offset;
                len = field->size;
            }
            // Use SQLITE_STATIC for zero-copy: data valid during sqlite3_step()
            sqlite3_bind_blob(priv->insert_stmt, idx++, ptr, len, SQLITE_STATIC);
        } else {
            // INTEGER: Numeric fields
            // Must respect signedness to correctly bind values to SQLite:
            //   - unsigned char 255 should bind as 255, not -1
            //   - signed char -1 should bind as -1, not 255
            // Using proper type casts ensures correct sign/zero extension to int64.
            //
            // Don't collapse the two branches with `is_signed ? *(int *) : *(unsigned int *)` —
            // C's usual arithmetic conversions promote the whole `?:` expression to the
            // wider unsigned type, so a signed `-4` becomes `0xFFFFFFFC` once assigned to
            // `long long`. Branch on is_signed instead.
            bool is_signed = field->flags & TEP_FIELD_IS_SIGNED;
            if (is_signed) {
                if (field->size == 1)
                    val = *(signed char *)(base + field->offset);
                else if (field->size == 2)
                    val = *(short *)(base + field->offset);
                else if (field->size == 4)
                    val = *(int *)(base + field->offset);
                else if (field->size == 8)
                    val = *(long long *)(base + field->offset);
            } else {
                if (field->size == 1)
                    val = *(unsigned char *)(base + field->offset);
                else if (field->size == 2)
                    val = *(unsigned short *)(base + field->offset);
                else if (field->size == 4)
                    val = *(unsigned int *)(base + field->offset);
                else if (field->size == 8)
                    val = *(unsigned long long *)(base + field->offset);
            }

            if (field->size <= 8)
                sqlite3_bind_int64(priv->insert_stmt, idx++, val);
            else
                sqlite3_bind_null(priv->insert_stmt, idx++);
        }
    }

    // Execute the insert statement
    if (sqlite3_step(priv->insert_stmt) != SQLITE_DONE) {
        if (ctx->verbose) {
            fprintf(stderr, "Failed to insert record into %s: %s\n",
                    priv->table_name, sqlite3_errmsg(ctx->sql));
        }
    } else {
        if (priv->mode == FILE_MODE) {
            priv->sample_count++;
            if (priv->first_sample_time == 0 || data->time < priv->first_sample_time)
                priv->first_sample_time = data->time;
            if (data->time > priv->last_sample_time)
                priv->last_sample_time = data->time;
        }
        ret = 0;
    }

    return ret;
}

static void sql_tp_file_reset(struct sql_tp_ctx *ctx)
{
    struct tp *tp;
    int i;

    // Finalize all prepared statements before dropping tables
    for_each_real_tp(ctx->tp_list, tp, i) {
        struct tp_private *priv = tp->private;
        if (priv->insert_stmt) {
            sqlite3_finalize(priv->insert_stmt);
            priv->insert_stmt = NULL;
        }
        priv->sample_count = 0;
        priv->first_sample_time = 0;
        priv->last_sample_time = 0;
    }

    // Recreate tables
    if (sql_create_table(ctx) < 0)
        fprintf(stderr, "Failed to recreate tables\n");
}

/*
 * Initialize file mode: events stored in SQLite tables via INSERT.
 * Used when --output2 specifies a database file.
 */
struct sql_tp_ctx *sql_tp_file(sqlite3 *sql, struct tp_list *tp_list)
{
    struct sql_tp_ctx *ctx = sql_tp_common_init(sql, tp_list);
    if (ctx) {
        if (sql_create_table(ctx) < 0)
            goto failed;
        ctx->sample = sql_tp_file_sample;
        ctx->reset = sql_tp_file_reset;
        return ctx;
    }

failed:
    if (ctx)
        sql_tp_free(ctx);
    return NULL;
}

/*
 * SQLite Virtual Table for memory mode.
 *
 * perf_tp module allows SQL queries to directly access events stored in memory
 * without sqlite3_bind_*() overhead. Fields are extracted on-demand when
 * xColumn() is called, so unused columns have zero cost.
 */
struct perf_tp_table {
    sqlite3_vtab base;              /* Base class. Must be first */
    struct sql_tp_ctx *ctx;
    struct tp *tp;                  /* Associated tracepoint */
    struct tp_private *priv;
    int verbose;
};

/*
 * Structured data passed from xBestIndex to xFilter.
 * IMPORTANT: This structure is read-only and shared across multiple xFilter calls
 * for the same query plan. xFilter must copy any data it needs to modify.
 */
struct index_info {
    char str[32];                   /* Header identifier for debugging */
    int nr_ops;                     /* Number of active constraints */
    int order_by;                   /* True if ORDER BY was consumed by index */
    int desc;                       /* True if ORDER BY DESC (descending order) */
    int distinct;                   /* The value from sqlite3_vtab_distinct() */
    int col_used;                   /* Mask of columns used by statement */
    /*
     * Constraint filter table built from xBestIndex decisions.
     * Each entry represents one WHERE clause condition on an INTEGER column.
     * NOTE: This table contains planning-time values and should be copied
     * in xFilter to bind actual runtime values from argv[].
     */
    struct one_op {
        int field;                  /* Column index (nr_sys system cols, then event fields) */
        int op;                     /* Comparison operator (EQ, GT, LE, LT, GE, NE) */
        int64_t value;              /* RHS value from sqlite3_vtab_rhs_value() */
    } op_table[0];
};

struct IndexNode {
    struct rb_node node;
    int64_t value;
    struct list_head event_list; // struct tp_event::link_index
};

/* Cursor for iterating over events in the linked list */
struct perf_tp_cursor {
    sqlite3_vtab_cursor base;       /* Base class. Must be first */
    struct rb_root *index_tree;     /* Index tree for indexed access */
    struct tp_event *start;         /* List head (sentinel), its next is a real tp_event */
    struct tp_event *curr;          /* Current event in iteration */
    /*
     * Index segment boundaries for range queries.
     * leftmost/rightmost define the current segment of IndexNodes being iterated.
     * left_op_value/right_op_value track the constraint range for segment iteration.
     */
    struct IndexNode *leftmost;     /* First IndexNode in current segment (>= left_op_value) */
    struct IndexNode *rightmost;    /* Last IndexNode in current segment (<= right_op_value) */
    struct boundary {
        int64_t value;
        int op;
        int valid;
    } left, right;                  /* Left and Right bound for current segment */
    int index_done;
    int scan_list;                  /* 1: full list scan, 0: index-based iteration */

    const struct index_info *ii;
    struct one_op *op_table;        /* All constraints copied from index_info with runtime values bound */
    int nr_ops;                     /* Number of active constraints */
    struct one_op *op_index;        /* Index field constraints only, sorted by value for query_op_boundary() */
    int nr_idx;                     /* Number of index field constraints */
    int nr_filter_ops;              /* Number of filter operations */
};

/* Internal operator codes for constraint filtering */
enum {
    EQ, GT, LE, LT, GE, NE, GLOB,          /* Maps to SQLITE_INDEX_CONSTRAINT_* */
    TEXT = 1<<8
};
#define VOID_PTR(v) ((void *)v)
#define CHAR_PTR(v) ((char *)(uint64_t)v)

/*
 * Comparators for qsort: sort op_index by value.
 *
 * NE (not-equal) constraints split the index range into segments. The op_index
 * must be sorted by value so that query_op_boundary() can process segments in the
 * correct order:
 *   - Ascending:  process segments left-to-right (smallest values first)
 *   - Descending: process segments right-to-left (largest values first)
 */
static int one_op_cmp(const void *aa, const void *bb)
{
    const struct one_op *a = aa, *b = bb;
    if (a->value < b->value) return -1;
    else if (a->value > b->value) return 1;
    else return 0;
}

static int one_op_strcmp(const void *aa, const void *bb)
{
    const struct one_op *a = aa, *b = bb;
    return strcmp(CHAR_PTR(a->value), CHAR_PTR(b->value));
}

/*
 * Find or create an index node in the red-black tree.
 *
 * Each IndexNode maintains a list of events with the same indexed field value,
 * enabling O(log n) lookup instead of O(n) full list scan.
 */
static inline struct IndexNode *get_IndexNode(struct rb_root *root, int64_t value, bool text)
{
    struct IndexNode *new;
    struct rb_node **p = &root->rb_node;
    struct rb_node *parent = NULL;
    int ret = 0;

    while (*p != NULL) {
        parent = *p;
        new = rb_entry(parent, struct IndexNode, node);
        if (text)
            ret = strcmp(CHAR_PTR(value), CHAR_PTR(new->value));
        if (text ? ret < 0 : value < new->value)
            p = &parent->rb_left;
        else if (text ? ret > 0 : value > new->value)
            p = &parent->rb_right;
        else
            return new;
    }

    new = malloc(sizeof(*new));
    if (!new)
        return NULL;

    new->value = value;
    INIT_LIST_HEAD(&new->event_list);

    rb_link_node(&new->node, parent, p);
    rb_insert_color(&new->node, root);
    return new;
}

/*
 * Find an IndexNode in the red-black tree based on comparison operator.
 *
 * During tree traversal, we track three potential results:
 *   - match: node with exact value match (node->value == value)
 *   - left:  rightmost node with value < search value (predecessor candidate)
 *   - right: leftmost node with value > search value (successor candidate)
 *
 * Return value based on operator:
 *   - EQ: exact match only
 *   - GT: first node > value (match's successor if exact match, else right)
 *   - LT: last node < value (match's predecessor if exact match, else left)
 *   - GE: first node >= value (match if exists, else right)
 *   - LE: last node <= value (match if exists, else left)
 *   - NE: exact match (for exclusion checks)
 */
static inline struct IndexNode *find_IndexNode(struct rb_root *root, int op, int64_t value)
{
    struct rb_node *node = root->rb_node;
    struct IndexNode *cmp;
    struct IndexNode *match = NULL;  /* Exact match: node->value == value */
    struct IndexNode *left = NULL;   /* Rightmost node with value < search value */
    struct IndexNode *right = NULL;  /* Leftmost node with value > search value */
    int ret;
    int text = op & TEXT;

    op &= ~TEXT;
    while (node) {
        cmp = rb_entry(node, struct IndexNode, node);
        if (text)
            ret = strcmp(CHAR_PTR(value), CHAR_PTR(cmp->value));
        if (text ? ret < 0 : value < cmp->value) {
            right = cmp;  /* cmp is a candidate for "first node > value" */
            node = node->rb_left;
        } else if (text ? ret > 0 : value > cmp->value) {
            left = cmp;   /* cmp is a candidate for "last node < value" */
            node = node->rb_right;
        } else {
            match = cmp;
            break;
        }
    }
    switch (op) {
        case EQ: return match;
        case GT: return match ? rb_entry_safe(rb_next(&match->node), struct IndexNode, node) : right;
        case LE: return match ? match : left;
        case LT: return match ? rb_entry_safe(rb_prev(&match->node), struct IndexNode, node) : left;
        case GE: return match ? match : right;
        case NE: return match;
        default: return NULL;
    }
}

/*
 * Delete all nodes in the index tree using post-order traversal.
 *
 * Post-order traversal (left -> right -> root) ensures children are freed
 * before their parent, allowing safe deletion without rb_erase() calls.
 * This is more efficient than iterative deletion when clearing the entire tree.
 *
 * Stack depth concern: With 32 levels of recursion, this can handle 2^32 nodes.
 * In practice, a single interval period cannot generate that many events.
 */
static inline void del_IndexTree(struct rb_root *root, struct rb_node *p)
{
    if (p->rb_left) del_IndexTree(root, p->rb_left);
    if (p->rb_right) del_IndexTree(root, p->rb_right);
    free(rb_entry(p, struct IndexNode, node));
}

static int perf_tp_xConnect(sqlite3 *db, void *pAux, int argc, const char *const*argv,
                            sqlite3_vtab **ppVtab, char **pzErr)
{
    struct perf_tp_table *table;
    struct sql_tp_ctx *ctx = pAux;
    struct tp *tp;
    int i, rc;
    struct tp_private *priv = NULL;

    table = sqlite3_malloc(sizeof(*table));
    if (!table)
        return SQLITE_NOMEM;

    memset(table, 0, sizeof(*table));
    table->ctx = ctx;

    for_each_real_tp(ctx->tp_list, tp, i) {
        priv = tp->private;
        if (strcmp(priv->table_name, argv[2]) == 0) {
            table->tp = tp;
            table->priv = priv;
            table->verbose = ctx->verbose;
            break;
        }
    }

    if (table->tp) {
        const char *table_fmt = "CREATE TABLE x (%s);";
        char buf[1024 + 256 + strlen(table_fmt)];
        char col_buf[1024 + 256];
        int j, col_len = 0;

        /* System columns and raw data columns, typed by ColumnType() */
        for (j = 0; j < priv->nr_fields; j++)
            col_len += scnprintf(col_buf + col_len, sizeof(col_buf) - col_len,
                            "%s%s %s", j ? ", " : "", ColumnName(priv, j), ColumnTypeName(priv, j));
        col_buf[col_len] = '\0';

        snprintf(buf, sizeof(buf), table_fmt, col_buf);
        if (table->verbose)
            printf("DECLARE SQL: %s\n", buf);

        rc = sqlite3_declare_vtab(db, buf);
        if (rc != SQLITE_OK) {
            *pzErr = sqlite3_mprintf("Failed to declare virtual table %s: %s",
                                     priv->table_name, sqlite3_errmsg(db));
            goto error;
        }

        priv->created_time = time(NULL);
    } else {
        *pzErr = sqlite3_mprintf("No such event: %s", argv[2]);
        rc = SQLITE_ERROR;
        goto error;
    }

    *ppVtab = &table->base;
    return SQLITE_OK;

error:
    if (table) sqlite3_free(table);
    return rc;
}

static int perf_tp_xCreate(sqlite3 *db, void *pAux, int argc, const char *const*argv,
                           sqlite3_vtab **ppVtab, char **pzErr)
{
    int rc = perf_tp_xConnect(db, pAux, argc, argv, ppVtab, pzErr);
    return rc;
}

static inline const char *OpStr(int op)
{
    // EQ, GT, LE, LT, GE, NE
    const char *op_str[] = {"==", ">", "<=", "<", ">=", "!=", "~"};
    op &= ~TEXT;
    if (op < sizeof(op_str) / sizeof(op_str[0]))
        return op_str[op];
    else
        return "NON";
}

static inline const char *IndexOpName(unsigned char op)
{
    switch(op) {
        case SQLITE_INDEX_CONSTRAINT_EQ:        return "EQ";
        case SQLITE_INDEX_CONSTRAINT_GT:        return "GT";
        case SQLITE_INDEX_CONSTRAINT_LE:        return "LE";
        case SQLITE_INDEX_CONSTRAINT_LT:        return "LT";
        case SQLITE_INDEX_CONSTRAINT_GE:        return "GE";
        case SQLITE_INDEX_CONSTRAINT_MATCH:     return "MATCH";
        case SQLITE_INDEX_CONSTRAINT_LIKE:      return "LIKE";
        case SQLITE_INDEX_CONSTRAINT_GLOB:      return "GLOB";
        case SQLITE_INDEX_CONSTRAINT_REGEXP:    return "REGEXP";
        case SQLITE_INDEX_CONSTRAINT_NE:        return "NE";
        case SQLITE_INDEX_CONSTRAINT_ISNOT:     return "ISNOT";
        case SQLITE_INDEX_CONSTRAINT_ISNOTNULL: return "ISNOTNULL";
        case SQLITE_INDEX_CONSTRAINT_ISNULL:    return "ISNULL";
        case SQLITE_INDEX_CONSTRAINT_IS:        return "IS";
        case SQLITE_INDEX_CONSTRAINT_LIMIT:     return "LIMIT";
        case SQLITE_INDEX_CONSTRAINT_OFFSET:    return "OFFSET";
        case SQLITE_INDEX_CONSTRAINT_FUNCTION:  return "FUNCTION";
        default: break;
    }
    return "UNKNOWN";
}

/*
 * Check if column i is an INTEGER type (suitable for constraint filtering).
 * System columns are always integers. Event fields depend on ColumnType().
 */
static inline bool Column_isInt(struct tp_private *priv, int i)
{
    return ColumnType(priv, i) == SQLITE_INTEGER;
}

static inline bool Column_isStr(struct tp_private *priv, int i)
{
    return ColumnType(priv, i) == SQLITE_TEXT;
}

/*
 * Check if a constraint can be pushed down to the kernel filter.
 *
 * Tracepoints: only _cpu (column 3), common_* and event fields can be
 * converted to kernel ftrace filter expressions.
 *
 * BPF events: only integer event fields, the filter is an expression compiled
 * to BPF (docs/bpf_event_filter.md). _cpu and strings cannot be: the BPF
 * backend refuses them, and the whole filter would run in userspace.
 */
static inline bool constraint_can_ftrace(struct tp_private *priv, int column)
{
    if (priv->bpf_fields)
        return column >= priv->nr_sys && Column_isInt(priv, column);
    return column > 4 || column == 3;
}

static void dump_pIdxInfo(sqlite3_vtab *pVtab, sqlite3_index_info *pIdxInfo)
{
    struct perf_tp_table *table = (void *)pVtab;
    struct tp_private *priv = table->priv;
    sqlite3_value *value;
    int i;

    printf("%s: %d %d\n", priv->table_name, pIdxInfo->nConstraint, pIdxInfo->nOrderBy);

    for (i = 0; i < pIdxInfo->nConstraint; i++) {
        bool col_not_used = pIdxInfo->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_LIMIT ||
                            pIdxInfo->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_OFFSET;

        printf("    Constraint[%d]: %s%s%s ", i,
                    col_not_used ? "" : ColumnName(priv, pIdxInfo->aConstraint[i].iColumn),
                    col_not_used ? "" : " ",
                    IndexOpName(pIdxInfo->aConstraint[i].op));

        if (sqlite3_vtab_rhs_value(pIdxInfo, i, &value) == SQLITE_OK) {
            switch (sqlite3_value_type(value)) {
                case SQLITE_INTEGER: printf("%lld (INTEGER)", sqlite3_value_int64(value)); break;
                case SQLITE_FLOAT: printf("%.3f (FLOAT)", sqlite3_value_double(value)); break;
                case SQLITE_TEXT: printf("'%s' (TEXT)", sqlite3_value_text(value)); break;
                case SQLITE_BLOB: printf("BLOB[%d]", sqlite3_value_bytes(value)); break;
                case SQLITE_NULL: printf("NULL"); break;
                default: printf("?"); break;
            }
        } else
            printf("?");

        printf("%s\n", pIdxInfo->aConstraint[i].usable ? "" : " (not usable)");
    }

    for (i = 0; i < pIdxInfo->nOrderBy; i++)
        printf("    OrderBy[%d]: %s %s\n", i,
                    ColumnName(priv, pIdxInfo->aOrderBy[i].iColumn),
                    pIdxInfo->aOrderBy[i].desc ? "DESC" : "ASC");
    if (pIdxInfo->nOrderBy)
        printf("    DISTINCT: %d\n", sqlite3_vtab_distinct(pIdxInfo));

    printf("    colUsed: 0x%016llx, ", pIdxInfo->colUsed);
    for_each_set_bit(i, (unsigned long *)&pIdxInfo->colUsed, 64) {
        printf("%s ", ColumnName(priv, i));
    }
    printf("\n");
}

/*
 * xBestIndex: Query planner interface for Virtual Table optimization.
 *
 * Called by SQLite during query planning to:
 *   1. Communicate filtering strategy to xFilter via idxNum/idxStr/argvIndex
 *   2. Report estimated cost to help SQLite choose optimal query plan
 *   3. Collect optimization data during init phase (priv->init == true)
 *
 * Output to SQLite:
 *   - idxNum: Number of usable INTEGER constraints
 *   - idxStr: Pointer to index_info structure containing constraint definitions
 *   - argvIndex: Maps constraint values to xFilter's argv[] array
 *   - omit=1: Tells SQLite we handle filtering (skip double-check)
 *   - estimatedCost: Average cost of all constraints (lower = better)
 *
 * During init phase (priv->init == true), also collects:
 *   - col_used: Bitmask of columns needed by query
 *   - col_refs: Reference count of supported constraint columns and order-by column
 *   - best_index[]: Array of constraint sets for index field selection
 *
 * Only INTEGER/STRING columns support constraint pushdown (EQ, GT, LE, LT, GE, NE, GLOB).
 * BLOB columns are filtered by SQLite after xColumn returns.
 */
static int perf_tp_xBestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *pIdxInfo)
{
    struct perf_tp_table *table = (void *)pVtab;
    struct tp_private *priv = table->priv;
    int i, column, op;
    struct index_info *ii;
    sqlite3_value *value;
    sqlite3_int64 rhs_value;
    bool rhs_available;
    int cost = 0;
    int nr_cost = 0;
    struct constraint *curr = NULL;
    int c = 0;
    bool isInt, isStr;

    if (unlikely(table->verbose))
        dump_pIdxInfo(pVtab, pIdxInfo);

    if (priv->init && pIdxInfo->nConstraint > 0) {
        curr = sqlite3_malloc(pIdxInfo->nConstraint * sizeof(*curr));
        if (!curr)
            return SQLITE_NOMEM;
        memset(curr, 0, pIdxInfo->nConstraint * sizeof(*curr));
    }

    ii = sqlite3_malloc(offsetof(struct index_info, op_table[pIdxInfo->nConstraint]));
    if (!ii)
        return SQLITE_NOMEM;
    memset(ii, 0, offsetof(struct index_info, op_table[pIdxInfo->nConstraint]));

    for (i = 0; i < pIdxInfo->nConstraint; i++) {
        if (!pIdxInfo->aConstraint[i].usable)
            continue;

        column = pIdxInfo->aConstraint[i].iColumn;
        isInt = Column_isInt(priv, column);
        isStr = Column_isStr(priv, column);
        switch(pIdxInfo->aConstraint[i].op) {
            case SQLITE_INDEX_CONSTRAINT_EQ: op = EQ; break;
            case SQLITE_INDEX_CONSTRAINT_GT: op = GT; break;
            case SQLITE_INDEX_CONSTRAINT_LE: op = LE; break;
            case SQLITE_INDEX_CONSTRAINT_LT: op = LT; break;
            case SQLITE_INDEX_CONSTRAINT_GE: op = GE; break;
            case SQLITE_INDEX_CONSTRAINT_NE: op = NE; break;
            case SQLITE_INDEX_CONSTRAINT_GLOB: if (isInt) goto unsupported;
                                             op = GLOB; break;
            default: goto unsupported;
        }
        if (isInt || isStr) {
            /* Try to get RHS value at planning time */
            rhs_value = 0;
            rhs_available = sqlite3_vtab_rhs_value(pIdxInfo, i, &value) == SQLITE_OK;
            if (rhs_available) {
                int rhs_type = sqlite3_value_type(value);
                if (isInt && rhs_type != SQLITE_INTEGER)
                    goto unsupported;
                if (isStr && rhs_type != SQLITE_TEXT)
                    goto unsupported;
                if (isInt)
                    rhs_value = sqlite3_value_int64(value);
            }

            ii->op_table[ii->nr_ops].field = column;
            ii->op_table[ii->nr_ops].op = op | (isStr ? TEXT : 0);
            ii->op_table[ii->nr_ops].value = rhs_value;
            ii->nr_ops++;

            pIdxInfo->aConstraintUsage[i].argvIndex = ii->nr_ops; /* argvIndex starts from 1 */
            pIdxInfo->aConstraintUsage[i].omit = 1;
            if (priv->init)
                priv->col_refs[column]++;

            /*
             * Cost model for query planning (lower is better):
             *   10:   ftrace-compatible constraint with known value (kernel filtering)
             *   50:   EQ/NE on integer/string field (good for user-space index lookup)
             *   200:  Range operators (GT/LT/GE/LE) on integer/string field
             *   1000: Non-integer/string field or unsupported operator (no optimization)
             *
             * Final cost = average of all constraint costs.
             */
            if (rhs_available && constraint_can_ftrace(priv, column)) {
                cost += 10;   /* Best: kernel ftrace filter */
            } else {
                if (op == EQ || op == NE)
                    cost += 50;   /* Good: index-friendly equality check */
                else
                    cost += 200;  /* Moderate: range scan required */
            }
            nr_cost++;

            /* Track constraint details for best index selection */
            if (curr) {
                curr[c].field = column;
                curr[c].op = op;
                curr[c].used_by = USED_BY_NONE;
                curr[c].value_set = rhs_available && isInt;
                curr[c].value = rhs_value;
                c++;
            }
            continue;
        }

    unsupported:
        cost += 1000;
        nr_cost++;
    }

    /*
     * ORDER BY optimization: count column references during init phase,
     * or consume ORDER BY if index field matches.
     */
    if (priv->init) {
        /*
         * Init phase: count ORDER BY column references.
         * Combined with WHERE constraint refs, the most referenced INTEGER
         * column will be chosen as the index field.
         */
        if (pIdxInfo->nOrderBy == 1) {
            column = pIdxInfo->aOrderBy[0].iColumn;
            if (Column_isInt(priv, column) || Column_isStr(priv, column))
                priv->col_refs[column]++;
        }
    } else
    if (pIdxInfo->nOrderBy == 1 && priv->have_index &&
        pIdxInfo->aOrderBy[0].iColumn == priv->index_field) {
        /*
         * Query phase: if ORDER BY matches our index field, we can provide
         * sorted output directly from index traversal (ascending or descending).
         * This avoids SQLite's sorting overhead.
         */
        pIdxInfo->orderByConsumed = 1;
        ii->order_by = 1;
        if (pIdxInfo->aOrderBy[0].desc)
            ii->desc = 1;
    }

    /* Set header identifier and displayed during EXPLAIN */
    snprintf(ii->str, sizeof(ii->str), "perf_tp:%p", ii);
    ii->distinct = sqlite3_vtab_distinct(pIdxInfo);
    ii->col_used = pIdxInfo->colUsed;

    pIdxInfo->idxNum = ii->nr_ops;
    pIdxInfo->idxStr = (void *)ii;
    pIdxInfo->needToFreeIdxStr = 1;

    /* Calculate estimated cost */
    cost = nr_cost ? cost / nr_cost : 1000;
    pIdxInfo->estimatedCost = cost;
    priv->have_prepare = 1; /* Mark: this table is queried by current SQL statement */

    if (unlikely(table->verbose)) {
        printf("    idxNum: 0x%x\n", pIdxInfo->idxNum);
        printf("    idxStr: \"%s\"\n", pIdxInfo->idxStr ? : "");
        printf("    orderByConsumed: %d\n", pIdxInfo->orderByConsumed);
        printf("    estimatedCost: %d\n", cost);
    }

    if (priv->init) {
        /*
         * Collect colUsed during initialization phase
         * OR accumulates columns from multi-statement queries like:
         *   --query "select pid from sched_wakeup; select comm from sched_wakeup;"
         */
        priv->col_used |= pIdxInfo->colUsed;

        if (curr && c == 0) {
            sqlite3_free(curr);
            curr = NULL;
        }

        // Check if this constraint set is already recorded
        if (curr && priv->best_index_num) {
            for (i = 0; i < priv->best_index_num; i++) {
                if (priv->best_index[i].nr_constraints == c &&
                    memcmp(priv->best_index[i].constraints, curr, c * sizeof(*curr)) == 0) {
                    /* Already recorded this constraint set */
                    sqlite3_free(curr);
                    curr = NULL;
                    break;
                }
            }
        }

        if (curr) {
            int size = (priv->best_index_num + 1) * sizeof(*priv->best_index);
            void *new_best_index = sqlite3_realloc(priv->best_index, size);
            if (!new_best_index) {
                sqlite3_free(curr);
                return SQLITE_NOMEM;
            }
            priv->best_index = new_best_index;
            priv->best_index[priv->best_index_num] = (struct BestIndex) {
                .constraints = curr, .nr_constraints = c, .cost = cost,
            };
            priv->best_index_num++;
        }
    }
    return SQLITE_OK;
}

static int perf_tp_xDisconnect(sqlite3_vtab *pVtab)
{
    sqlite3_free(pVtab);
    return SQLITE_OK;
}

static int perf_tp_xOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor)
{
    struct perf_tp_cursor *cursor;

    cursor = sqlite3_malloc(sizeof(*cursor));
    if (!cursor)
        return SQLITE_NOMEM;

    memset (cursor, 0, sizeof(*cursor));
    *ppCursor = &cursor->base;
    return SQLITE_OK;
}

static int perf_tp_xClose(sqlite3_vtab_cursor *pCursor)
{
    struct perf_tp_cursor *cursor = (void *)pCursor;
    int i;
    if (cursor->op_table) {
        for (i = 0; i < cursor->nr_ops; i++)
            if ((cursor->op_table[i].op & TEXT) && cursor->op_table[i].value)
                sqlite3_free(VOID_PTR(cursor->op_table[i].value));
        sqlite3_free(cursor->op_table);
    }
    sqlite3_free(pCursor);
    return SQLITE_OK;
}

/*
 * Extract INTEGER/STRING field value from event for constraint comparison.
 * Used by xNext to filter events against op_table conditions.
 */
static inline sqlite3_int64 perf_tp_field(struct tp_private *priv, struct tp_event *e, int i)
{
    struct sql_sample_type *data = (void *)e->event.sample.array;

    // event-specific fields of BPF events: no common_* before them
    if (priv->bpf_fields && i >= priv->nr_sys) {
        event_fields *field = &priv->bpf_fields[i - priv->nr_sys];
        void *base = data->raw.data;

        if (field->is_string)
            return (sqlite3_int64)base + field->offset;
        return event_field_scalar(field, base);
    }

    switch (i) {
        // common fields
        case 0: return data->tid_entry.pid;
        case 1: return data->tid_entry.tid;
        case 2: return data->time;
        case 3: return data->cpu_entry.cpu;
        case 4: return data->period;
        // common_* (common_type removed - use event_id from event_metadata table)
        case 5: return data->raw.common.common_flags;
        case 6: return data->raw.common.common_preempt_count;
        case 7: return data->raw.common.common_pid;
        // event-specific fields
        default: {
            struct tep_format_field *field = priv->fields[i - priv->nr_sys];
            void *base = data->raw.data;
            sqlite3_int64 val = 0;
            bool is_signed;

            if (field->flags & TEP_FIELD_IS_STRING) {
                if (field->flags & TEP_FIELD_IS_DYNAMIC) {
                    // Dynamic string: __data_loc char[] field
                    return (sqlite3_int64)base + *(unsigned short *)(base + field->offset);
                } else {
                    // Fixed string: char field[N]
                    return (sqlite3_int64)base + field->offset;
                }
            }
            // INTEGER: Numeric fields.
            // See the comment in sql_tp_file_sample() for why is_signed is on
            // an if/else instead of a `?:` expression.
            is_signed = field->flags & TEP_FIELD_IS_SIGNED;
            if (is_signed) {
                if (field->size == 1)
                    val = *(signed char *)(base + field->offset);
                else if (field->size == 2)
                    val = *(short *)(base + field->offset);
                else if (field->size == 4)
                    val = *(int *)(base + field->offset);
                else if (field->size == 8)
                    val = *(long long *)(base + field->offset);
            } else {
                if (field->size == 1)
                    val = *(unsigned char *)(base + field->offset);
                else if (field->size == 2)
                    val = *(unsigned short *)(base + field->offset);
                else if (field->size == 4)
                    val = *(unsigned int *)(base + field->offset);
                else if (field->size == 8)
                    val = *(unsigned long long *)(base + field->offset);
            }

            return val;
        }
    }
    return -1;
}

static void print_boundary(struct boundary *left, struct boundary *right)
{
    if (left->valid) {
        if (left->op & TEXT)
            printf("(%-2s '%s', ", OpStr(left->op), CHAR_PTR(left->value));
        else
            printf("(%-2s %ld, ", OpStr(left->op), left->value);
    } else
        printf("(-infinity, ");

    if (right->valid) {
        if (right->op & TEXT)
            printf("%-2s '%s')\n", OpStr(right->op), CHAR_PTR(right->value));
        else
            printf("%-2s %ld)\n", OpStr(right->op), right->value);
    } else
        printf("+infinity)\n");
}

static bool _op_(int64_t left_value, int op, int64_t right_value)
{
    switch (op) {
        case EQ: return left_value == right_value;
        case GT: return left_value >  right_value;
        case LE: return left_value <= right_value;
        case LT: return left_value <  right_value;
        case GE: return left_value >= right_value;
        case NE: return left_value != right_value;
        case TEXT|EQ: return strcmp(CHAR_PTR(left_value), CHAR_PTR(right_value)) == 0;
        case TEXT|GT: return strcmp(CHAR_PTR(left_value), CHAR_PTR(right_value)) >  0;
        case TEXT|LE: return strcmp(CHAR_PTR(left_value), CHAR_PTR(right_value)) <= 0;
        case TEXT|LT: return strcmp(CHAR_PTR(left_value), CHAR_PTR(right_value)) <  0;
        case TEXT|GE: return strcmp(CHAR_PTR(left_value), CHAR_PTR(right_value)) >= 0;
        case TEXT|NE: return strcmp(CHAR_PTR(left_value), CHAR_PTR(right_value)) != 0;
        default: return 0;
    }
}

/**
 * query_op_boundary - Compute the next valid segment boundaries from WHERE constraints.
 *
 * @table:   Array of index field constraints, MUST be sorted by value (ascending).
 * @nr_ops:  Number of constraints in the table.
 * @desc:    Iteration direction: 0 = ascending, 1 = descending.
 * @left:    IN/OUT: Left boundary of current segment.
 * @right:   IN/OUT: Right boundary of current segment.
 *
 * Return: 0 if valid segment found, -1 if no valid segment (empty set or conflict).
 *
 * OVERVIEW
 * --------
 * This function processes SQL WHERE constraints on an indexed field and computes the
 * next valid segment boundaries. NE (!=) constraints may split the range into multiple
 * disjoint segments, which are returned one at a time across successive calls.
 *
 * BOUNDARY REPRESENTATION
 * -----------------------
 * Unlike integer-based algorithms that use value+1/value-1 for open intervals,
 * this algorithm stores (value, operator) pairs to represent boundaries:
 *   - (10, GE) means >= 10, i.e., [10, ...)
 *   - (10, GT) means > 10,  i.e., (10, ...)
 *   - (20, LE) means <= 20, i.e., (..., 20]
 *   - (20, LT) means < 20,  i.e., (..., 20)
 *
 * This approach naturally supports strings (where +1/-1 is undefined) and floats.
 *
 * TWO-PASS ALGORITHM
 * ------------------
 * Pass 1: Establish base range from EQ/GE/LE/GT/LT constraints
 *   - EQ: Sets both left (GE) and right (LE) to the same value
 *   - GE/GT: Updates left boundary if tighter (value larger or same value but stricter)
 *   - LE/LT: Updates right boundary if tighter (value smaller or same value but stricter)
 *   - "Stricter" means GT overrides GE, LT overrides LE when values are equal
 *
 * Pass 2: Apply NE constraints to split the range
 *   - Table is sorted, so we scan in iteration direction to find nearest cut
 *   - NE outside range: ignored
 *   - NE at boundary: converts closed boundary to open (GE->GT or LE->LT)
 *   - NE inside range: becomes new boundary, splitting the segment
 *
 * EXAMPLE: "pid != 5 AND pid > 10 AND pid != 20 AND pid < 100 AND pid != 200"
 *   Pass 1: GT(10), LT(100) -> (10, GT) to (100, LT) i.e., (10, 100)
 *   Pass 2 (ascending): NE(5) ignored, NE(20) splits -> (10, GT) to (20, LT)
 *   Result: First segment is (10, 20), next call returns (20, GT) to (100, LT)
 *
 * EXAMPLE: WHERE comm glob 'perf*'
 * SQLite creates 3 constraints for this pattern:
 *   1. comm GE 'perf'    - lower boundary (inclusive)
 *   2. comm LT 'perg'    - upper boundary (exclusive, 'perf' + 1)
 *   3. comm GLOB 'perf*' - pattern matching (handled by xNext)
 *
 *   This function processes the range constraints (GE/LT) to establish the
 *   search boundaries [GE 'perf', LT 'perg'). The GLOB constraint is stored
 *   in op_table and evaluated later by xNext for each row within the range.
 */
static int query_op_boundary(struct one_op *table, int nr_ops, int desc,
                             struct boundary *left, struct boundary *right)
{
    int i, op, text, op_ne = 0, ret = 0;
    int64_t op_value;

    /*
     * Pass 1: Establish the base range [L, R].
     *
     * For each constraint, update the boundary if the new constraint is "tighter":
     *   - For left boundary: new value > current value, OR same value but stricter (GT > GE)
     *   - For right boundary: new value < current value, OR same value but stricter (LT > LE)
     */
    for (i = 0; i < nr_ops; i++) {
        op = table[i].op & ~TEXT;
        text = table[i].op & TEXT;
        op_value = table[i].value;
        switch (op) {
            case EQ:
                /* EQ implies both >= value AND <= value, fall through to set both bounds */
            case GE:
                /* Update left if: no current bound OR new value > current value */
                if (!left->valid || _op_(op_value, text|GT, left->value)) {
                    left->value = op_value;
                    left->op = text | GE;
                    left->valid = 1;
                }
                if (op == GE) break;
                /* fall through for EQ to also set right bound */
            case LE:
                /* Update right if: no current bound OR new value < current value */
                if (!right->valid || _op_(op_value, text|LT, right->value)) {
                    right->value = op_value;
                    right->op = text | LE;
                    right->valid = 1;
                }
                break;
            case GT:
                /* Strict lower bound: use >= comparison so GT(10) overrides GE(10) */
                if (!left->valid || _op_(op_value, text|GE, left->value)) {
                    left->value = op_value;
                    left->op = text | GT;
                    left->valid = 1;
                }
                break;
            case LT:
                /* Strict upper bound: use <= comparison so LT(10) overrides LE(10) */
                if (!right->valid || _op_(op_value, text|LE, right->value)) {
                    right->value = op_value;
                    right->op = text | LT;
                    right->valid = 1;
                }
                break;
            case NE: op_ne++; break;
            default: break;
        }
    }

conflict_detect:
    /*
     * Conflict detection: check if the range is empty.
     *
     * Empty range conditions:
     *   1. left.value > right.value (e.g., > 100 AND < 50)
     *   2. left.value == right.value but not both closed
     *      (e.g., > 10 AND < 10, or > 10 AND <= 10, or >= 10 AND < 10)
     *      Only >= 10 AND <= 10 is valid (single point)
     */
    if (left->valid && right->valid) {
        text = left->op & TEXT;
        if (text != (right->op & TEXT))
            return -1; /* Type mismatch (should not happen) */

        if (text)
            ret = strcmp(CHAR_PTR(left->value), CHAR_PTR(right->value));

        /* left > right: empty range */
        if (!text ? left->value > right->value : ret > 0)
            return -1;

        /* left == right but not [value, value]: empty range */
        if ((!text ? left->value == right->value : ret == 0) &&
            ((left->op & ~TEXT) != GE || (right->op & ~TEXT) != LE))
            return -1;
    }

    /* No NE constraints: base range is final result */
    if (op_ne == 0)
        return 0;

    /*
     * Pass 2: Apply NE constraints to split the range.
     *
     * Since table is sorted by value, we scan in iteration direction:
     *   - Ascending: left-to-right, find first NE that cuts or is inside range
     *   - Descending: right-to-left, find first NE that cuts or is inside range
     *
     * For each NE value:
     *   Case 1: NE outside range -> ignore
     *   Case 2: NE at boundary -> convert to open interval (GE->GT or LE->LT)
     *   Case 3: NE inside range -> becomes new boundary, stop
     */
    if (desc) {
        /* Descending: scan from largest to smallest NE value */
        for (i = nr_ops - 1; i >= 0; i--) {
            if ((table[i].op & ~TEXT) == NE) {
                text = table[i].op & TEXT;
                op_value = table[i].value;

                if (right->valid) {
                    if (text)
                        ret = strcmp(CHAR_PTR(op_value), CHAR_PTR(right->value));

                    /* Case 1: NE > right boundary, outside range, skip */
                    if (!text ? op_value > right->value : ret > 0)
                        continue;

                    /* Case 2: NE == right boundary, make it exclusive */
                    if (!text ? op_value == right->value : ret == 0) {
                        right->op = text | LT;
                        continue;
                    }
                }

                /* Case 3: NE inside range, becomes new left boundary (exclusive) */
                if (!left->valid || _op_(op_value, text|GE, left->value)) {
                    left->value = op_value;
                    left->op = text | GT;
                    left->valid = 1;
                }
                break;
            }
        }
    } else {
        /* Ascending: scan from smallest to largest NE value */
        for (i = 0; i < nr_ops; i++) {
            if ((table[i].op & ~TEXT) == NE) {
                text = table[i].op & TEXT;
                op_value = table[i].value;

                if (left->valid) {
                    if (text)
                        ret = strcmp(CHAR_PTR(op_value), CHAR_PTR(left->value));

                    /* Case 1: NE < left boundary, outside range, skip */
                    if (!text ? op_value < left->value : ret < 0)
                        continue;

                    /* Case 2: NE == left boundary, make it exclusive */
                    if (!text ? op_value == left->value : ret == 0) {
                        left->op = text | GT;
                        continue;
                    }
                }

                /* Case 3: NE inside range, becomes new right boundary (exclusive) */
                if (!right->valid || _op_(op_value, text|LE, right->value)) {
                    right->value = op_value;
                    right->op = text | LT;
                    right->valid = 1;
                }
                break;
            }
        }
    }

    /*
     * After Pass 2, re-check for conflicts. The NE processing may have
     * created an empty range (e.g., >= 10 AND != 10 -> > 10, but if
     * original right was <= 10, now we have > 10 AND <= 10 which is empty).
     *
     * Set op_ne = 0 to skip Pass 2 on the second iteration.
     */
    op_ne = 0;
    goto conflict_detect;
}

/* Return values for perf_tp_do_index() */
enum {
    INDEX_DONE,     /* No more segments, iteration complete */
    INDEX_CONT,     /* Valid segment found in [leftmost, rightmost] */
    INDEX_ALL_NODE  /* No index constraints, use full table scan */
};

/**
 * perf_tp_do_index - Find the next valid index segment for cursor iteration.
 *
 * @cursor: The cursor containing index state and constraints.
 *
 * Return: INDEX_CONT (segment found), INDEX_DONE (exhausted), INDEX_ALL_NODE (no constraints)
 *
 * OVERVIEW
 * --------
 * This function locates IndexNodes in the red-black tree that satisfy WHERE
 * constraints on the indexed field. It handles NE (!=) constraints that split
 * the value space into disjoint segments by returning one segment at a time.
 *
 * INDEX TREE STRUCTURE
 * --------------------
 * The index tree is a red-black tree where:
 *   - Each node (IndexNode) represents a unique field value
 *   - Each node contains a list of events with that value
 *   - Nodes are ordered by value (integers or strings via strcmp)
 *
 * SEGMENT ITERATION
 * -----------------
 * NE constraints can split the search range into multiple segments:
 *
 *   Example: WHERE pid > 10 AND pid < 100 AND pid != 50
 *   Segments: (10, 50) and (50, 100)
 *
 * This function is called repeatedly, returning one segment per call:
 *   1st call: finds segment (10, 50), sets cursor to iterate nodes in [11, 49]
 *   2nd call: finds segment (50, 100), sets cursor to iterate nodes in [51, 99]
 *   3rd call: returns INDEX_DONE
 *
 * ITERATION DIRECTION
 * -------------------
 * - Ascending (ORDER BY field ASC):
 *     Process segments left-to-right, within each segment iterate left-to-right
 *     After segment [L, R], next search starts from (R, +inf)
 *
 * - Descending (ORDER BY field DESC):
 *     Process segments right-to-left, within each segment iterate right-to-left
 *     After segment [L, R], next search starts from (-inf, L)
 *
 * ALGORITHM STEPS
 * ---------------
 * 1. Call query_op_boundary() to compute segment boundaries from constraints
 * 2. Use find_IndexNode() to locate actual tree nodes within the segment:
 *    - leftmost: first node satisfying left boundary constraint
 *    - rightmost: last node satisfying right boundary constraint
 * 3. Validate: ensure leftmost <= rightmost (segment contains nodes)
 * 4. Advance: move search window to next segment for subsequent call
 * 5. Termination: when both boundaries become invalid (infinity)
 */
static inline int perf_tp_do_index(struct perf_tp_cursor *cursor)
{
    struct perf_tp_table *table = (void *)cursor->base.pVtab;
    struct tp_private *priv = table->priv;
    const struct index_info *ii = cursor->ii;
    const char *index_field_name = "";
    int ret = INDEX_DONE;
    int text = priv->index_is_str ? TEXT : 0;

    if (RB_EMPTY_ROOT(cursor->index_tree))
        return INDEX_DONE;

    if (unlikely(table->verbose))
        index_field_name = ColumnName(priv, priv->index_field);

    /* Loop through segments until a valid one is found or all exhausted */
    while (!cursor->index_done) {
        if (unlikely(table->verbose)) {
            printf("    Query '%s' IN ", index_field_name);
            print_boundary(&cursor->left, &cursor->right);
        }

        /*
         * Step 1: Compute segment boundaries from constraints.
         * query_op_boundary() updates left/right boundaries and handles NE splitting.
         */
        if (query_op_boundary(cursor->op_index, cursor->nr_idx, ii->desc, &cursor->left, &cursor->right) < 0)
            return INDEX_DONE;

        if (unlikely(table->verbose)) {
            printf("    Index '%s' IN ", index_field_name);
            print_boundary(&cursor->left, &cursor->right);
        }

        /*
         * Special case: no constraints on indexed field AND no ORDER BY requirement.
         * Fall back to full table scan which is more efficient than index traversal.
         */
        if (!ii->order_by && !cursor->left.valid && !cursor->right.valid)
            return INDEX_ALL_NODE;

        /*
         * Step 2: Find actual tree nodes within the segment boundaries.
         *
         * For valid boundary: use find_IndexNode() with the boundary operator
         *   - left boundary (GE/GT): find first node satisfying the constraint
         *   - right boundary (LE/LT): find last node satisfying the constraint
         *
         * For invalid boundary (infinity): use tree's first/last node
         */
        cursor->leftmost  = cursor->left.valid ?
                            find_IndexNode(cursor->index_tree, cursor->left.op, cursor->left.value) :
                            rb_entry_safe(rb_first(cursor->index_tree), struct IndexNode, node);
        cursor->rightmost = cursor->right.valid ?
                            find_IndexNode(cursor->index_tree, cursor->right.op, cursor->right.value) :
                            rb_entry_safe(rb_last(cursor->index_tree), struct IndexNode, node);

        /*
         * Step 3: Validate segment - ensure nodes exist and leftmost <= rightmost.
         * If valid, this segment will be used for iteration.
         */
        if (cursor->leftmost && cursor->rightmost &&
            _op_(cursor->leftmost->value, text|LE, cursor->rightmost->value)) {
            if (unlikely(table->verbose)) {
                if (text)
                     printf("          '%s' IN ['%s', '%s']\n", index_field_name,
                                            CHAR_PTR(cursor->leftmost->value), CHAR_PTR(cursor->rightmost->value));
                else printf("          '%s' IN [%ld, %ld]\n", index_field_name, cursor->leftmost->value, cursor->rightmost->value);
            }
            ret = INDEX_CONT;
        }

        /*
         * Step 4: Advance to next segment for subsequent call.
         *
         * The key insight: after processing segment with boundary B, the next
         * segment starts just past B. We invert the boundary operator:
         *   - If B was inclusive (GE/LE), next starts exclusive (GT/LT)
         *   - If B was exclusive (GT/LT), next starts inclusive (GE/LE)
         *
         * This ensures no values are skipped or double-counted.
         */
        if (ii->desc) {
            /* Descending: next segment is (-inf, current_left) */
            cursor->right = cursor->left;
            cursor->right.op = (cursor->left.op & ~TEXT) == GT ? text|LE : text|LT;
            cursor->left.valid = 0;
        } else {
            /* Ascending: next segment is (current_right, +inf) */
            cursor->left = cursor->right;
            cursor->left.op = (cursor->right.op & ~TEXT) == LT ? text|GE : text|GT;
            cursor->right.valid = 0;
        }

        /*
         * Step 5: Check termination condition.
         * When both boundaries are invalid (infinity), we've covered the entire range.
         */
        if (!cursor->left.valid && !cursor->right.valid) {
            cursor->index_done = 1;
            if (unlikely(table->verbose))
                printf("    -> Index DONE\n");
        }

        if (ret == INDEX_CONT) return ret;
    }
    return INDEX_DONE;
}

/*
 * xNext: Advance cursor to next matching event.
 *
 * Two iteration modes:
 *   1. scan_list=1: Full list scan via link, checking all constraints
 *   2. scan_list=0: Index-based iteration via link_index
 *
 * Index iteration structure:
 *   - Each IndexNode contains a list of events with the same indexed field value
 *   - Iterate through events in current IndexNode (via link_index)
 *   - When exhausted, move to next IndexNode (leftmost -> rightmost within segment)
 *   - When segment exhausted, call perf_tp_do_index() for next segment
 *
 * If op_table is set (constraints from xBestIndex), filter events by evaluating
 * all conditions. Event must satisfy ALL constraints (AND logic) to be returned.
 * Skips non-matching events until a match is found or list exhausted.
 */
static int perf_tp_xNext(sqlite3_vtab_cursor *pCursor)
{
    struct perf_tp_table *table = (void *)pCursor->pVtab;
    struct tp_private *priv = table->priv;
    struct perf_tp_cursor *cursor = (void *)pCursor;
    sqlite3_int64 value, op_value;
    int i;

    priv->xNext++;
    while (1) {
        if (cursor->scan_list) {
            /* Full list scan: iterate through all events via global link */
            cursor->curr = list_next_entry(cursor->curr, link);
        } else {
            /* Index-based iteration: iterate through events in IndexNode */
            cursor->curr = list_next_entry(cursor->curr, link_index);
            if (cursor->curr == cursor->start) {
                const struct index_info *ii = cursor->ii;
                /* Current IndexNode exhausted, move to next node or segment */
                if (cursor->leftmost != cursor->rightmost) {
                    /*
                     * More nodes in current segment: advance to next IndexNode.
                     * Direction depends on ORDER BY:
                     *   - Ascending:  leftmost++ (traverse left to right)
                     *   - Descending: rightmost-- (traverse right to left)
                     */
                    if (ii->desc)
                        cursor->rightmost = rb_entry_safe(rb_prev(&cursor->rightmost->node), struct IndexNode, node);
                    else
                        cursor->leftmost = rb_entry_safe(rb_next(&cursor->leftmost->node), struct IndexNode, node);

                    if (unlikely(!cursor->leftmost || !cursor->rightmost)) {
                        fprintf(stderr, "%s: Indexing BUG: got NULL before reaching %s\n", priv->table_name,
                                        ii->desc ? "leftmost" : "rightmost");
                        break;
                    }
                } else {
                    /* Current segment exhausted: try next segment (for NE splits) */
                    if (perf_tp_do_index(cursor) == INDEX_DONE)
                        break;
                }

                if (unlikely(table->verbose > VERBOSE_NOTICE)) {
                    if (priv->index_is_str)
                        printf("%s: '%s' = '%s'\n", priv->table_name, ColumnName(priv, priv->index_field),
                            ii->desc ? CHAR_PTR(cursor->rightmost->value) : CHAR_PTR(cursor->leftmost->value));
                    else
                        printf("%s: '%s' = %ld\n", priv->table_name, ColumnName(priv, priv->index_field),
                            ii->desc ? cursor->rightmost->value : cursor->leftmost->value);
                }
                /* Set up iteration for new IndexNode's event list */
                cursor->start = ii->desc ? list_entry(&cursor->rightmost->event_list, struct tp_event, link_index) :
                                           list_entry(&cursor->leftmost->event_list, struct tp_event, link_index);
                cursor->curr = list_next_entry(cursor->start, link_index);
            }
        }

        /* No constraints or reached end of list */
        if (!cursor->nr_filter_ops ||
            cursor->curr == cursor->start)
            break;

        priv->do_filter++;
        /* Check all constraints (AND logic) */
        for (i = 0; i < cursor->nr_filter_ops; i++) {
            value = perf_tp_field(priv, cursor->curr, cursor->op_table[i].field);
            op_value = cursor->op_table[i].value;
            switch (cursor->op_table[i].op) {
                case EQ: if (value == op_value) break; else goto next;
                case GT: if (value >  op_value) break; else goto next;
                case LE: if (value <= op_value) break; else goto next;
                case LT: if (value <  op_value) break; else goto next;
                case GE: if (value >= op_value) break; else goto next;
                case NE: if (value != op_value) break; else goto next;
                case TEXT|EQ: if (strcmp(CHAR_PTR(value), CHAR_PTR(op_value)) == 0) break; else goto next;
                case TEXT|GT: if (strcmp(CHAR_PTR(value), CHAR_PTR(op_value)) >  0) break; else goto next;
                case TEXT|LE: if (strcmp(CHAR_PTR(value), CHAR_PTR(op_value)) <= 0) break; else goto next;
                case TEXT|LT: if (strcmp(CHAR_PTR(value), CHAR_PTR(op_value)) <  0) break; else goto next;
                case TEXT|GE: if (strcmp(CHAR_PTR(value), CHAR_PTR(op_value)) >= 0) break; else goto next;
                case TEXT|NE: if (strcmp(CHAR_PTR(value), CHAR_PTR(op_value)) != 0) break; else goto next;
                case TEXT|GLOB: if (sqlite3_strglob(CHAR_PTR(op_value), CHAR_PTR(value)) == 0) break; else goto next;
                default: goto next;
            }
        }
        break; /* all conditions met */
    next:
        continue;
    }
    return SQLITE_OK;
}

/*
 * Convert SQL query planner constraints to kernel trace event filter.
 *
 * During initialization (priv->init), generate ftrace_filter from op_table.
 * Only fields supported by kernel ftrace filter are included.
 *
 * Filter combination logic:
 *   - Within single xFilter call: constraints are combined with && (AND)
 *     Example: WHERE pid > 1000 AND prio < 10 -> "pid>1000&&prio<10"
 *   - Across multiple xFilter calls: filters are combined with || (OR)
 *     Example: First call "pid>1000", second call "prio==120"
 *              -> "(pid>1000)||(prio==120)"
 *
 * String handling:
 *   - String values are automatically quoted (e.g., comm == "bash")
 *   - Supported operators: EQ (==), NE (!=), GLOB (~)
 *
 * The filter is stored in priv->ftrace_filter and applied later by tp_list_apply_filter().
 */
static inline void perf_tp_ftrace_filter(struct perf_tp_cursor *cursor)
{
    struct perf_tp_table *table = (void *)cursor->base.pVtab;
    struct tp_private *priv = table->priv;
    struct constraint *c;
    int i, j, k;

    /* Generate kernel filter only during init and if not already set */
    if (priv->init && !table->tp->filter) {
        char *filter = NULL;
        for (i = 0; i < cursor->nr_ops; i++) {
            /* Only include fields supported by kernel ftrace filter */
            if (constraint_can_ftrace(priv, cursor->op_table[i].field)) {
                char *tmp = NULL;
                const char *name;
                int op = cursor->op_table[i].op;

                if (cursor->op_table[i].field == 3) name = "CPU";
                else name = ColumnName(priv, cursor->op_table[i].field);

                if (op & TEXT) {
                    /*
                     * Handle string constraints:
                     * Only EQ, NE, and GLOB are supported for strings in ftrace
                     */
                    op &= ~TEXT;
                    if (op == EQ || op == NE || op == GLOB)
                        tmp = sqlite3_mprintf("%s%s%s%s\"%s\"", filter ?: "", filter ? "&&" : "",
                                name, OpStr(op), CHAR_PTR(cursor->op_table[i].value));
                } else
                    tmp = sqlite3_mprintf("%s%s%s%s%lld", filter ?: "", filter ? "&&" : "",
                            name, OpStr(op), cursor->op_table[i].value);
                if (tmp) {
                    if (filter) sqlite3_free(filter);
                    filter = tmp;
                    priv->have_filter = 1; /* Mark: this table has WHERE constraints */

                    // Mark constraint as used by ftrace filter
                    for_each_constraint(priv, c, j, k) {
                        if (c->field == cursor->op_table[i].field &&
                            c->op == cursor->op_table[i].op &&
                            c->used_by == USED_BY_NONE)
                            c->used_by = USED_BY_FTRACE;
                    }
                }
            }
        }
        if (filter) {
            if (!priv->ftrace_filter)
                priv->ftrace_filter = filter;
            else {
                const char *fmt = priv->ftrace_filter[0] == '(' ? "%s||(%s)" : "(%s)||(%s)";
                char *tmp = sqlite3_mprintf(fmt, priv->ftrace_filter, filter);
                if (tmp) {
                    sqlite3_free(priv->ftrace_filter);
                    priv->ftrace_filter = tmp;
                }
                sqlite3_free(filter);
            }
        }
    }
}

/* Decide whether to use index lookup or full list scan. */
static inline void perf_tp_do_filter(struct perf_tp_cursor *cursor)
{
    struct perf_tp_table *table = (void *)cursor->base.pVtab;
    struct tp_private *priv = table->priv;
    const struct index_info *ii = cursor->ii;
    int i;

    if (unlikely(table->verbose)) {
        if (cursor->nr_ops)
            printf("%s: %p nr_filter_ops %d nr_idx %d\n", priv->table_name, ii, cursor->nr_filter_ops, cursor->nr_idx);
        for (i = 0; i < cursor->nr_ops; i++) {
            printf("    Filter[%d]: %s %s ", i, ColumnName(priv, cursor->op_table[i].field),
                        OpStr(cursor->op_table[i].op));
            if (cursor->op_table[i].op & TEXT)
                printf("'%s'\n", CHAR_PTR(cursor->op_table[i].value));
            else
                printf("%ld\n", cursor->op_table[i].value);
        }
        printf("%s: %p%s", priv->table_name, ii, ii->order_by ? "" : "\n");
        if (ii->order_by)
            printf(" order by '%s' %s\n", ColumnName(priv, priv->index_field), ii->desc ? "DESC" : "ASC");
    }

    /*
     * Index lookup: use red-black tree index if available.
     * Supports all comparison operators (EQ, GT, GE, LT, LE, NE).
     * Falls back to full list scan if no index or no constraints on indexed field.
     */
    if (priv->have_index) {
        if (!ii->order_by && cursor->nr_idx == 0)
            goto scan_list;

        cursor->index_tree = &priv->index_tree;
        cursor->left.valid = 0;
        cursor->right.valid = 0;
        cursor->index_done = 0;

        switch (perf_tp_do_index(cursor)) {
            case INDEX_DONE:
                /* No matching nodes found in any segment */
                cursor->start = NULL;
                cursor->curr = NULL;
                if (unlikely(table->verbose))
                    printf("    NULL\n");
                break;
            case INDEX_CONT:
                if (!ii->order_by &&
                    &cursor->leftmost->node == rb_first(cursor->index_tree) &&
                    &cursor->rightmost->node == rb_last(cursor->index_tree))
                    goto scan_list;

                if (unlikely(table->verbose > VERBOSE_NOTICE)) {
                    if (priv->index_is_str)
                        printf("%s: '%s' = '%s'\n", priv->table_name, ColumnName(priv, priv->index_field),
                            ii->desc ? CHAR_PTR(cursor->rightmost->value) : CHAR_PTR(cursor->leftmost->value));
                    else
                        printf("%s: '%s' = %ld\n", priv->table_name, ColumnName(priv, priv->index_field),
                            ii->desc ? cursor->rightmost->value : cursor->leftmost->value);
                }
                /* Valid segment found, set up iteration from leftmost node */
                cursor->start = ii->desc ? list_entry(&cursor->rightmost->event_list, struct tp_event, link_index) :
                                           list_entry(&cursor->leftmost->event_list, struct tp_event, link_index);
                cursor->curr = cursor->start;
                break;
            case INDEX_ALL_NODE:
            default: goto scan_list;  /* No index constraints, fall back to scan */
        }
        cursor->scan_list = 0;  /* Use index-based iteration */
        priv->do_index++;
    } else {
    scan_list:
        /* Full list scan: no index available or not applicable */
        priv->scan_list++;
        if (unlikely(table->verbose))
            printf("    Scan list\n");
    }
}

/*
 * xFilter: Initialize cursor and set up constraint filtering.
 *
 * This function is called by SQLite to initialize a cursor for a query.
 * It receives the index_info structure (serialized in idxStr) that was
 * constructed in xBestIndex. It then:
 * 1. Resets any previous cursor state and frees old constraint tables.
 * 2. Binds the actual runtime values from argv[] to the operator table
 *    (op_table) used for filtering in xNext.
 * 3. Separates constraints into index field constraints (op_index) and
 *    non-index field constraints (filter operations).
 * 4. Sorts index constraints for efficient boundary queries.
 * 5. Advances the cursor to the first matching record by calling xNext.
 */
static int perf_tp_xFilter(sqlite3_vtab_cursor *pCursor, int idxNum, const char *idxStr,
                           int argc, sqlite3_value **argv)
{
    struct perf_tp_table *table = (void *)pCursor->pVtab;
    struct tp_private *priv = table->priv;
    struct perf_tp_cursor *cursor = (void *)pCursor;
    const struct index_info *ii;
    int i, text, idx, value_type;

    /* Reset previous filter state */
    cursor->scan_list = 1;
    cursor->start = list_entry(&priv->event_list, struct tp_event, link);
    cursor->curr = cursor->start;
    cursor->ii = NULL;

    if (cursor->op_table) {
        for (i = 0; i < cursor->nr_ops; i++)
            if ((cursor->op_table[i].op & TEXT) && cursor->op_table[i].value)
                sqlite3_free(VOID_PTR(cursor->op_table[i].value));
        sqlite3_free(cursor->op_table);
        cursor->op_table = NULL;
        cursor->nr_ops = 0;
        cursor->op_index = NULL;
        cursor->nr_idx = 0;
        cursor->nr_filter_ops = 0;
    }

    if (idxStr && argc == idxNum) {
        ii = (const struct index_info *)idxStr;
        cursor->ii = ii;

        if (argc) {
            /*
             * Allocate and copy operator table with runtime values.
             * IMPORTANT: index_info object is read-only and shared across multiple
             * xFilter calls for the same query plan. We must copy the operator
             * table and bind actual runtime values from argv[].
             */
            cursor->op_table = sqlite3_malloc(argc * sizeof(*cursor->op_table));
            if (!cursor->op_table)
                return SQLITE_NOMEM;

            cursor->nr_ops = argc;
            cursor->nr_idx = 0;
            cursor->nr_filter_ops = 0;
            for (i = 0; i < argc; i++) {
                text = ii->op_table[i].op & TEXT;
                value_type = sqlite3_value_type(argv[i]);

                if (unlikely(value_type != (text ? SQLITE_TEXT : SQLITE_INTEGER))) {
                    fprintf(stderr, "ERROR: Filter[%d]: %s %s", i, ColumnName(priv, ii->op_table[i].field),
                            OpStr(ii->op_table[i].op));
                    switch (value_type) {
                        case SQLITE_INTEGER: fprintf(stderr, "%lld (INTEGER)", sqlite3_value_int64(argv[i])); break;
                        case SQLITE_FLOAT: fprintf(stderr, "%.3f (FLOAT)", sqlite3_value_double(argv[i])); break;
                        case SQLITE_TEXT: fprintf(stderr, "'%s' (TEXT)", sqlite3_value_text(argv[i])); break;
                        case SQLITE_BLOB: fprintf(stderr, "BLOB[%d]", sqlite3_value_bytes(argv[i])); break;
                        case SQLITE_NULL: fprintf(stderr, "NULL"); break;
                        default: fprintf(stderr, "?"); break;
                    }
                    fprintf(stderr, ", Not %s value type\n", text ? "TEXT" : "INTEGER");
                }

                if (priv->have_index &&
                    ii->op_table[i].field == priv->index_field &&
                    (ii->op_table[i].op & ~TEXT) < GLOB) {
                    cursor->nr_idx++;
                    idx = cursor->nr_ops - cursor->nr_idx;
                } else
                    idx = cursor->nr_filter_ops++;

                cursor->op_table[idx].field = ii->op_table[i].field;
                cursor->op_table[idx].op = ii->op_table[i].op;
                cursor->op_table[idx].value = !text ? sqlite3_value_int64(argv[i]) :
                    (int64_t)(void *)sqlite3_mprintf("%s",  sqlite3_value_text(argv[i]));
            }
            if (cursor->nr_idx)
                cursor->op_index = cursor->op_table + cursor->nr_filter_ops;
            /*
             * Index field constraints are stored in the second half of op_table
             * (starting at nr_filter_ops offset). Sort them by value for efficient
             * boundary queries and NE constraint handling.
             */
            if (!priv->init && cursor->nr_idx) {
                /* Sort op_index by value (ascending) for query_op_boundary() */
                qsort(cursor->op_index, cursor->nr_idx, sizeof(*cursor->op_index),
                        priv->index_is_str ? one_op_strcmp : one_op_cmp);
            }
            perf_tp_ftrace_filter(cursor);
        }
        perf_tp_do_filter(cursor);
    }
    priv->xFilter++;

    /* Position cursor at first matching event */
    if (cursor->start)
        perf_tp_xNext(pCursor);

    return SQLITE_OK;
}

static int perf_tp_xEof(sqlite3_vtab_cursor *pCursor)
{
    struct tp_private *priv = ((struct perf_tp_table *)pCursor->pVtab)->priv;
    struct perf_tp_cursor *cursor = (void *)pCursor;
    priv->xEof++;
    return cursor->curr == cursor->start;
}

static int perf_tp_xColumn(sqlite3_vtab_cursor *pCursor, sqlite3_context *ctx, int i)
{
    struct tp_private *priv = ((struct perf_tp_table *)pCursor->pVtab)->priv;
    struct perf_tp_cursor *cursor = (void *)pCursor;
    struct tp_event *e = cursor->curr;
    struct sql_sample_type *data = (void *)e->event.sample.array;

    priv->xColumn++;

    // event-specific fields of BPF events: no common_* before them
    if (priv->bpf_fields && i >= priv->nr_sys) {
        if (i >= priv->nr_fields)
            return SQLITE_ERROR;
        bpf_field_result(ctx, &priv->bpf_fields[i - priv->nr_sys], data->raw.data);
        return SQLITE_OK;
    }

    switch (i) {
        // common fields
        case 0: sqlite3_result_int(ctx, data->tid_entry.pid); break;
        case 1: sqlite3_result_int(ctx, data->tid_entry.tid); break;
        case 2: sqlite3_result_int64(ctx, data->time); break;
        case 3: sqlite3_result_int(ctx, data->cpu_entry.cpu); break;
        case 4: sqlite3_result_int64(ctx, data->period); break;

        // common_* (common_type removed - use event_id from event_metadata table)
        case 5: sqlite3_result_int(ctx, data->raw.common.common_flags); break;
        case 6: sqlite3_result_int(ctx, data->raw.common.common_preempt_count); break;
        case 7: sqlite3_result_int(ctx, data->raw.common.common_pid); break;

        // event-specific fields
        default: {
            struct tep_format_field *field = i < priv->nr_fields ? priv->fields[i - priv->nr_sys] : NULL;
            void *base = data->raw.data;
            long long val = 0;
            void *ptr;
            int len;

            if (!field)
                return SQLITE_ERROR;

            if ((field->flags & TEP_FIELD_IS_STRING)) {
                // TEXT: String fields (IS_STRING flag)
                if (field->flags & TEP_FIELD_IS_DYNAMIC) {
                    // Dynamic string: __data_loc char[] field
                    ptr = base + *(unsigned short *)(base + field->offset);
                    len = *(unsigned short *)(base + field->offset + sizeof(unsigned short));
                } else {
                    // Fixed string: char field[N]
                    ptr = base + field->offset;
                    len = -1;
                }
                sqlite3_result_text(ctx, ptr, len, SQLITE_STATIC);
            } else if (field->flags & TEP_FIELD_IS_ARRAY) {
                // BLOB: Array fields without string semantics
                if (field->flags & TEP_FIELD_IS_DYNAMIC) {
                    // Dynamic array: __data_loc __u8[] buf
                    ptr = base + *(unsigned short *)(base + field->offset);
                    len = *(unsigned short *)(base + field->offset + sizeof(unsigned short));
                } else {
                    // Fixed array: __u8 saddr[4], long sysctl_mem[3]
                    ptr = base + field->offset;
                    len = field->size;
                }
                sqlite3_result_blob(ctx, ptr, len, SQLITE_STATIC);
            } else {
                // INTEGER: Numeric fields.
                // See the comment in sql_tp_file_sample() for why is_signed is on
                // an if/else instead of a `?:` expression.
                bool is_signed = field->flags & TEP_FIELD_IS_SIGNED;
                if (is_signed) {
                    if (field->size == 1)
                        val = *(signed char *)(base + field->offset);
                    else if (field->size == 2)
                        val = *(short *)(base + field->offset);
                    else if (field->size == 4)
                        val = *(int *)(base + field->offset);
                    else if (field->size == 8)
                        val = *(long long *)(base + field->offset);
                } else {
                    if (field->size == 1)
                        val = *(unsigned char *)(base + field->offset);
                    else if (field->size == 2)
                        val = *(unsigned short *)(base + field->offset);
                    else if (field->size == 4)
                        val = *(unsigned int *)(base + field->offset);
                    else if (field->size == 8)
                        val = *(unsigned long long *)(base + field->offset);
                }

                if (field->size <= 8)
                    sqlite3_result_int64(ctx, val);
                else
                    sqlite3_result_null(ctx);
            }
        }
        break;
    }
    return SQLITE_OK;
}

static int perf_tp_xRowid(sqlite3_vtab_cursor *pCursor, sqlite_int64 *pRowid)
{
    struct tp_private *priv = ((struct perf_tp_table *)pCursor->pVtab)->priv;
    struct perf_tp_cursor *cursor = (void *)pCursor;
    *pRowid = cursor->curr->rowid;
    priv->xRowid++;
    return SQLITE_OK;
}

static sqlite3_module perf_tp_module = {
    0,                       /* iVersion */
    perf_tp_xCreate,         /* xCreate */
    perf_tp_xConnect,        /* xConnect */
    perf_tp_xBestIndex,      /* xBestIndex */
    perf_tp_xDisconnect,     /* xDisconnect */
    perf_tp_xDisconnect,     /* xDestroy */
    perf_tp_xOpen,           /* xOpen - open a cursor */
    perf_tp_xClose,          /* xClose - close a cursor */
    perf_tp_xFilter,         /* xFilter - configure scan constraints */
    perf_tp_xNext,           /* xNext - advance a cursor */
    perf_tp_xEof,            /* xEof - check for end of scan */
    perf_tp_xColumn,         /* xColumn - read data */
    perf_tp_xRowid,          /* xRowid - read data */
    0,                       /* xUpdate */
    0,                       /* xBegin */
    0,                       /* xSync */
    0,                       /* xCommit */
    0,                       /* xRollback */
    0,                       /* xFindMethod */
    0,                       /* xRename */
    0,                       /* xSavepoint */
    0,                       /* xRelease */
    0                        /* xRollbackTo */
};

/*
 * Create regular tables for events using MEM_REGULAR_TABLE_MODE.
 *
 * Called after mode selection to replace Virtual Tables with regular tables
 * for events where INSERT is more efficient than Virtual Table callbacks.
 *
 * For MEM_REGULAR_TABLE_MODE events:
 *   - DROP the Virtual Table (same name)
 *   - CREATE regular table with only columns specified in col_used bitmask
 *   - Prepare INSERT statement for sql_tp_mem_sample()
 *
 * For MEM_VIRTUAL_TABLE_MODE events:
 *   - Skip (continue using Virtual Table for on-demand field extraction)
 */
static int sql_tp_mem_create_table(struct sql_tp_ctx *ctx)
{
    struct tp *tp;
    int i;
    const char *table_fmt = "DROP TABLE IF EXISTS %s; CREATE TABLE %s (%s);";
    const char *insert_fmt = "INSERT INTO %s VALUES(%s);";
    char buf[1024 + 256 + strlen(table_fmt)];
    char *errmsg = NULL;

    for_each_real_tp(ctx->tp_list, tp, i) {
        struct tp_private *priv = (struct tp_private *)tp->private;
        char col_buf[1024 + 256];
        char ins_buf[512];
        int j, col_len = 0, ins_len = 0;

        if (priv->mode == MEM_VIRTUAL_TABLE_MODE)
            continue;

        /* System columns and raw data columns used by the query, typed by ColumnType() */
        for (j = 0; j < priv->nr_fields; j++) {
            if (j < BITS_PER_LONG && !test_bit(j, &priv->col_used))
                continue;

            col_len += scnprintf(col_buf + col_len, sizeof(col_buf) - col_len,
                            "%s %s, ", ColumnName(priv, j), ColumnTypeName(priv, j));
            if (!priv->mem_insert_stmt)
                ins_len += scnprintf(ins_buf + ins_len, sizeof(ins_buf) - ins_len,
                                "?, ");
        }
        if (col_len) col_len -= 2; // remove ", "
        if (ins_len) ins_len -= 2; // remove ", "
        col_buf[col_len] = '\0';
        ins_buf[ins_len] = '\0';

        snprintf(buf, sizeof(buf), table_fmt, priv->table_name, priv->table_name, col_buf);
        if (ctx->verbose)
            printf("CREATE SQL: %s\n", buf);
        if (sqlite3_exec(ctx->sql, buf, NULL, NULL, &errmsg) != SQLITE_OK) {
            fprintf(stderr, "Failed to create table %s: %s\n", priv->table_name, errmsg);
            sqlite3_free(errmsg);
            return -1;
        }
        priv->created_time = time(NULL);

        if (priv->mem_insert_stmt)
            continue;

        snprintf(buf, sizeof(buf), insert_fmt, priv->table_name, ins_buf);
        if (ctx->verbose)
            printf("INSERT SQL: %s\n", buf);

        if (sqlite3_prepare_v3(ctx->sql, buf, -1, SQLITE_PREPARE_PERSISTENT, &priv->mem_insert_stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "Failed to prepare insert statement for %s: %s\n", priv->table_name, sqlite3_errmsg(ctx->sql));
            return -1;
        }
    }
    return 0;
}

/*
 * Memory mode sample handler.
 *
 * Two paths based on storage mode:
 *   MEM_VIRTUAL_TABLE_MODE: Store raw event in linked list for on-demand field extraction.
 *                           If index is enabled, also add to index tree.
 *   MEM_REGULAR_TABLE_MODE: Parse event and INSERT only needed columns to regular table.
 */
static int sql_tp_mem_sample(struct sql_tp_ctx *ctx, struct tp *tp, union perf_event *event)
{
    struct tp_private *priv = tp->private;
    struct sql_sample_type *data = (void *)event->sample.array;
    int idx = 1;
    int i, ret = -1;

    if (priv->mode == MEM_VIRTUAL_TABLE_MODE) {
        /* Virtual Table path: store raw event for on-demand field extraction */
        struct tp_event *e = malloc(offsetof(struct tp_event, event) + event->header.size);
        if (e) {
            e->rowid = priv->rowid++;
            memcpy(&e->event, event, event->header.size);
            list_add_tail(&e->link, &priv->event_list);
            /* Index maintenance: add event to index tree */
            if (priv->have_index) {
                int64_t value = perf_tp_field(priv, e, priv->index_field);
                struct IndexNode *node = get_IndexNode(&priv->index_tree, value, priv->index_is_str);
                if (node)
                    list_add_tail(&e->link_index, &node->event_list);
            }
            ret = 0;
        }
    } else {
        /* Regular table path: parse and bind only needed columns */
        sqlite3_reset(priv->mem_insert_stmt);

        // Bind common fields
        if (test_bit(0, &priv->col_used))
            sqlite3_bind_int(priv->mem_insert_stmt, idx++, data->tid_entry.pid);
        if (test_bit(1, &priv->col_used))
            sqlite3_bind_int(priv->mem_insert_stmt, idx++, data->tid_entry.tid);
        if (test_bit(2, &priv->col_used))
            sqlite3_bind_int64(priv->mem_insert_stmt, idx++, data->time);
        if (test_bit(3, &priv->col_used))
            sqlite3_bind_int(priv->mem_insert_stmt, idx++, data->cpu_entry.cpu);
        if (test_bit(4, &priv->col_used))
            sqlite3_bind_int64(priv->mem_insert_stmt, idx++, data->period);

        // common_* (common_type removed - use event_id from event_metadata table)
        // A BPF event has no trace_entry, its event fields follow _period.
        if (!priv->bpf_fields) {
            if (test_bit(5, &priv->col_used))
                sqlite3_bind_int(priv->mem_insert_stmt, idx++, data->raw.common.common_flags);
            if (test_bit(6, &priv->col_used))
                sqlite3_bind_int(priv->mem_insert_stmt, idx++, data->raw.common.common_preempt_count);
            if (test_bit(7, &priv->col_used))
                sqlite3_bind_int(priv->mem_insert_stmt, idx++, data->raw.common.common_pid);
        }

        /* Parse and bind event-specific fields */
        for (i = 0; priv->bpf_fields && priv->bpf_fields[i].name; i++) {
            if (priv->nr_sys + i < BITS_PER_LONG && !test_bit(priv->nr_sys + i, &priv->col_used))
                continue;
            bpf_field_bind(priv->mem_insert_stmt, idx++, &priv->bpf_fields[i], data->raw.data);
        }
        for (i = 0; priv->fields && priv->fields[i]; i++) {
            struct tep_format_field *field = priv->fields[i];
            void *base = data->raw.data;
            long long val = 0;
            void *ptr;
            int len;

            if (priv->nr_sys + i < BITS_PER_LONG && !test_bit(priv->nr_sys + i, &priv->col_used))
                continue;

            if ((field->flags & TEP_FIELD_IS_STRING)) {
                // TEXT: String fields (IS_STRING flag)
                if (field->flags & TEP_FIELD_IS_DYNAMIC) {
                    // Dynamic string: __data_loc char[] field
                    ptr = base + *(unsigned short *)(base + field->offset);
                    len = *(unsigned short *)(base + field->offset + sizeof(unsigned short));
                } else {
                    // Fixed string: char field[N]
                    ptr = base + field->offset;
                    len = -1;
                }
                // Use SQLITE_STATIC for zero-copy: data valid during sqlite3_step()
                sqlite3_bind_text(priv->mem_insert_stmt, idx++, ptr, len, SQLITE_STATIC);
            } else if (field->flags & TEP_FIELD_IS_ARRAY) {
                // BLOB: Array fields without string semantics
                if (field->flags & TEP_FIELD_IS_DYNAMIC) {
                    // Dynamic array: __data_loc __u8[] buf
                    ptr = base + *(unsigned short *)(base + field->offset);
                    len = *(unsigned short *)(base + field->offset + sizeof(unsigned short));
                } else {
                    // Fixed array: __u8 saddr[4], long sysctl_mem[3]
                    ptr = base + field->offset;
                    len = field->size;
                }
                // Use SQLITE_STATIC for zero-copy: data valid during sqlite3_step()
                sqlite3_bind_blob(priv->mem_insert_stmt, idx++, ptr, len, SQLITE_STATIC);
            } else {
                // INTEGER: Numeric fields.
                // See the comment in sql_tp_file_sample() for why is_signed is on
                // an if/else instead of a `?:` expression.
                bool is_signed = field->flags & TEP_FIELD_IS_SIGNED;
                if (is_signed) {
                    if (field->size == 1)
                        val = *(signed char *)(base + field->offset);
                    else if (field->size == 2)
                        val = *(short *)(base + field->offset);
                    else if (field->size == 4)
                        val = *(int *)(base + field->offset);
                    else if (field->size == 8)
                        val = *(long long *)(base + field->offset);
                } else {
                    if (field->size == 1)
                        val = *(unsigned char *)(base + field->offset);
                    else if (field->size == 2)
                        val = *(unsigned short *)(base + field->offset);
                    else if (field->size == 4)
                        val = *(unsigned int *)(base + field->offset);
                    else if (field->size == 8)
                        val = *(unsigned long long *)(base + field->offset);
                }

                if (field->size <= 8)
                    sqlite3_bind_int64(priv->mem_insert_stmt, idx++, val);
                else
                    sqlite3_bind_null(priv->mem_insert_stmt, idx++);
            }
        }

        // Execute the insert statement
        if (sqlite3_step(priv->mem_insert_stmt) != SQLITE_DONE) {
            if (ctx->verbose) {
                fprintf(stderr, "Failed to insert record into %s: %s\n",
                        priv->table_name, sqlite3_errmsg(ctx->sql));
            }
        } else
            ret = 0;
    }

    if (ret == 0) {
        priv->sample_count++;
        if (priv->first_sample_time == 0 || data->time < priv->first_sample_time)
            priv->first_sample_time = data->time;
        if (data->time > priv->last_sample_time)
            priv->last_sample_time = data->time;
    }
    return ret;
}

/* Memory mode reset: free all events from linked lists after each interval */
static void sql_tp_mem_reset(struct sql_tp_ctx *ctx)
{
    struct tp *tp;
    int i;

    for_each_real_tp(ctx->tp_list, tp, i) {
        struct tp_private *priv = tp->private;
        struct tp_event *e, *n;

        if (!RB_EMPTY_ROOT(&priv->index_tree)) {
            del_IndexTree(&priv->index_tree, priv->index_tree.rb_node);
            priv->index_tree = RB_ROOT;
        }
        list_for_each_entry_safe(e, n, &priv->event_list, link) {
            __list_del_entry(&e->link);
            free(e);
        }
        if (priv->mem_insert_stmt) {
            sqlite3_finalize(priv->mem_insert_stmt);
            priv->mem_insert_stmt = NULL;
        }
        if (ctx->verbose)
            printf("%s: xFilter %lu xEof %lu xNext %lu xColumn %lu xRowid %lu scan_list %lu do_index %lu do_filter %lu\n",
                    priv->table_name, priv->xFilter, priv->xEof, priv->xNext, priv->xColumn, priv->xRowid,
                    priv->scan_list, priv->do_index, priv->do_filter);
        priv->rowid = 0;
        priv->created_time = time(NULL);
        priv->xFilter = priv->xEof = priv->xNext = priv->xColumn = priv->xRowid = 0;
        priv->scan_list = priv->do_index = priv->do_filter = 0;
        priv->sample_count = 0;
        priv->first_sample_time = 0;
        priv->last_sample_time = 0;
    }

    // Recreate tables
    if (sql_tp_mem_create_table(ctx) < 0)
        fprintf(stderr, "Failed to recreate tables\n");
}

/*
 * Execute query on empty Virtual Tables to collect query planner info.
 *
 * Called during initialization to trigger SQLite query planning on empty tables.
 * This collects critical optimization data without processing any real events:
 *
 *   1. sqlite3_prepare_v3() triggers xBestIndex:
 *      - Collects colUsed bitmask (which columns the query needs)
 *      - Collects WHERE clause constraints for ftrace filter and index selection
 *      - Builds cost model for query optimization
 *
 *   2. sqlite3_step() triggers xFilter on empty table:
 *      - Converts constraints to ftrace filter expression
 *      - Since table is empty, xFilter iterates all filter conditions without early exit
 *      - This ensures all query patterns are captured for index field selection
 *
 * Supports multi-statement queries separated by ';'. Failures are ignored
 * because some tables (like event_metadata) don't exist yet during init,
 * but event Virtual Tables are already available for data collection.
 */
static int sql_tp_mem_try_exec(struct sql_tp_ctx *ctx, const char *query)
{
    sqlite3_stmt *stmt;
    const char *next_query;
    int ret = -1;
    struct tp *tp;
    int i;

    while (1) {
        if (sqlite3_prepare_v3(ctx->sql, query, -1, SQLITE_PREPARE_PERSISTENT, &stmt, &next_query) == SQLITE_OK) {
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            ret = 0;

            /*
             * Count queries and filters per table for this SQL statement.
             * Used to determine if ftrace filter can be safely applied:
             * only when ALL queries on a table have WHERE constraints.
             */
            for_each_real_tp(ctx->tp_list, tp, i) {
                struct tp_private *priv = tp->private;
                if (priv->have_prepare) {
                    priv->nr_query++;
                    priv->have_prepare = 0;
                }
                if (priv->have_filter) {
                    priv->nr_filter++;
                    priv->have_filter = 0;
                }
            }
        }
        /* Continue to next statement even if current one failed */
        if (*next_query) query = next_query;
        else break;
    }
    return ret;
}

/*
 * sql_tp_mem: Initialize memory mode for SQL event storage.
 * Used when no --output2 is specified (in-memory database).
 *
 * Initialization flow:
 *   1. Create Virtual Tables for all events (enables xBestIndex calls)
 *   2. Set priv->init = 1 to enable optimization data collection
 *   3. Execute --query on empty tables via sql_tp_mem_try_exec():
 *      - sqlite3_prepare_v3() triggers xBestIndex: collects colUsed, constraints, cost
 *      - sqlite3_step() triggers xFilter: generates ftrace_filter from constraints
 *   4. Set priv->init = 0 to stop optimization data collection
 *   5. Analyze collected data and select optimizations:
 *      - Choose storage mode (Virtual Table vs Regular Table)
 *      - Select index field (most referenced field in constraints)
 *      - Apply ftrace_filter to tp->filter for kernel-level filtering
 *   6. Create regular tables for events using MEM_REGULAR_TABLE_MODE
 *
 * After init, optimization settings are fixed for the session.
 */
struct sql_tp_ctx *sql_tp_mem(sqlite3 *sql, struct tp_list *tp_list, const char *query)
{
    struct sql_tp_ctx *ctx = sql_tp_common_init(sql, tp_list);
    const char *vtable_fmt = "CREATE VIRTUAL TABLE IF NOT EXISTS %s USING perf_tp";
    struct tp *tp;
    int i;
    char buf[128];
    char *errmsg;

    if (ctx) {
        /* Register perf_tp virtual table module */
        if (sqlite3_create_module(ctx->sql, "perf_tp", &perf_tp_module, ctx) != SQLITE_OK) {
            fprintf(stderr, "Failed to create perf_tp module: %s\n", sqlite3_errmsg(ctx->sql));
            goto failed;
        }
        /* Step 1-2 */
        for_each_real_tp(ctx->tp_list, tp, i) {
            struct tp_private *priv = (struct tp_private *)tp->private;

            priv->col_refs = calloc(priv->nr_fields, sizeof(*priv->col_refs));
            if (!priv->col_refs)
                goto failed;

            snprintf(buf, sizeof(buf), vtable_fmt, priv->table_name);
            if (ctx->verbose)
                printf("CREATE VTABLE SQL: %s\n", buf);
            if (sqlite3_exec(ctx->sql, buf, NULL, NULL, &errmsg) != SQLITE_OK) {
                fprintf(stderr, "Failed to create vtable %s: %s\n", priv->table_name, errmsg);
                sqlite3_free(errmsg);
                goto failed;
            }
            priv->init = 1;
        }

        /* Step 3 */
        if (query && query[0])
            sql_tp_mem_try_exec(ctx, query);

        /*
         * Step 4-5: Analyze constraints and select storage mode + index field.
         *
         * Storage mode selection:
         *   - MEM_REGULAR_TABLE_MODE: Query uses >50% of fields, INSERT is more efficient
         *   - MEM_VIRTUAL_TABLE_MODE: Query uses few fields, or has ftrace filter/index
         *
         * Index field selection: Choose the field referenced most often in constraints.
         * This maximizes index hit rate across different query patterns.
         */
        for_each_real_tp(ctx->tp_list, tp, i) {
            struct tp_private *priv = (struct tp_private *)tp->private;
            struct constraint *c;
            int j, k, max_refs = 0;

            priv->init = 0;

            /* Mode selection: based on column usage ratio */
            if (priv->nr_fields > BITS_PER_LONG ||
                hweight64(priv->col_used) > priv->nr_fields/2)
                priv->mode = MEM_REGULAR_TABLE_MODE;
            else
                priv->mode = MEM_VIRTUAL_TABLE_MODE;

            /*
             * Apply ftrace filter only when ALL queries have WHERE constraints.
             * If nr_query != nr_filter, some queries lack filters, so applying
             * ftrace filter would incorrectly drop events needed by those queries.
             */
            if (!tp->filter && priv->ftrace_filter && priv->nr_query == priv->nr_filter) {
                priv->mode = MEM_VIRTUAL_TABLE_MODE;
                tp->filter = strdup(priv->ftrace_filter);
                printf("%s:%s: SQL Query planner filter: %s\n", tp->sys, tp->name, tp->filter);
            }

            /*
             * Index field auto-selection: pick the INTEGER/STRING field with most references.
             * References are counted from both WHERE constraints and ORDER BY columns.
             * This heuristic maximizes index effectiveness for common query patterns.
             */
            for (j = 0; j < priv->nr_fields; j++) {
                if (priv->col_refs[j] > max_refs) {
                    max_refs = priv->col_refs[j];
                    priv->have_index = 1;
                    priv->index_field = j;
                }
            }

            /*
             * User-specified index field (via index=field attribute) overrides auto-selection.
             * Validates that the specified field exists and is an INTEGER/STRING type.
             */
            if (tp->index) {
                int found = 0;
                for (j = 0; j < priv->nr_fields; j++)
                    if (strcmp(tp->index, ColumnName(priv, j)) == 0) {
                        if (Column_isInt(priv, j) || Column_isStr(priv, j)) {
                            priv->have_index = 1;
                            priv->index_field = j;
                            found = 2;
                        } else
                            found = 1;
                        break;
                    }
                if (found != 2)
                    fprintf(stderr, "%s:%s: Warning: index field '%s' not %s, using '%s'\n",
                            tp->sys, tp->name, tp->index, found == 1 ? "integer or string" : "found",
                            priv->have_index ? ColumnName(priv, priv->index_field) : "");
            }

            /* Index available: force Virtual Table */
            if (priv->have_index) {
                priv->mode = MEM_VIRTUAL_TABLE_MODE;
                priv->index_is_str = Column_isStr(priv, priv->index_field);
                printf("%s:%s: Chosen '%s' field%s for indexing\n", tp->sys, tp->name,
                        ColumnName(priv, priv->index_field),
                        (priv->col_used & (1<<priv->index_field)) ? "" : " (not used)");
            }
            if (ctx->verbose) {
                for_each_constraint(priv, c, j, k) {
                    printf("%s: Constraint[%d][%d]: %s %s ", priv->table_name, j, k,
                            ColumnName(priv, c->field), OpStr(c->op));
                    if (c->value_set) printf("%lld\t", c->value);
                    else printf("?\t");
                    printf("%s\n", c->used_by == USED_BY_FTRACE ? "FTRACE" : "");
                }
            }
        }

        /* Step 6 */
        if (sql_tp_mem_create_table(ctx) < 0)
            goto failed;

        ctx->sample = sql_tp_mem_sample;
        ctx->reset = sql_tp_mem_reset;
        return ctx;
    }

failed:
    if (ctx)
        sql_tp_free(ctx);
    return NULL;
}

void sql_tp_free(struct sql_tp_ctx *ctx)
{
    struct tp *tp;
    int i, j;
    for_each_real_tp(ctx->tp_list, tp, i) {
        struct tp_private *priv = tp->private;
        struct tp_event *e, *n;
        if (priv) {
            if (priv->fields)
                free(priv->fields);
            free(priv->bpf_fields);
            if (priv->function_list)
                free(priv->function_list);
            if (priv->col_refs)
                free(priv->col_refs);
            if (priv->ftrace_filter)
                sqlite3_free(priv->ftrace_filter);
            if (priv->best_index) {
                for (j = 0; j < priv->best_index_num; j++)
                    if (priv->best_index[j].constraints)
                        sqlite3_free(priv->best_index[j].constraints);
                sqlite3_free(priv->best_index);
            }
            list_for_each_entry_safe(e, n, &priv->event_list, link) {
                __list_del_entry(&e->link);
                free(e);
            }
            if (priv->mem_insert_stmt)
                sqlite3_finalize(priv->mem_insert_stmt);
            if (priv->insert_stmt)
                sqlite3_finalize(priv->insert_stmt);
            free(priv);
            tp->private = NULL;
        }
    }
    if (ctx->ksymbol)
        function_resolver_unref();
    rblist__exit(&ctx->symbolic_table);
    if (ctx->tep)
        tep__unref();
    free(ctx);
}
