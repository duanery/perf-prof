/*
 * Expression compiler and simulator.
 * https://github.com/rswier/c4.git
 *
 * Instruction Set
 *   pc: instruction register
 *   sp: stack pointer
 *   bp: frame pointer
 *   a : general purpose register
 *
 * Opcode      Instruction        #Description
 * LEA imm64   LEA a, imm64[bp]   #case LEA: a = (long)(bp + *pc++); break;                              // load local address
 * IMM imm64   IMM a, imm64       #case IMM: a = *pc++; break;                                           // load global address or immediate
 * JMP imm64   JMP imm64          #case JMP: pc = (long *)*pc; break;                                    // jump
 * JSR imm64   JSR imm64          #case JSR: { *--sp = (long)(pc + 1); pc = (long *)*pc; } break;        // jump to subroutine
 * BZ  imm64   BZ  imm64          #case BZ:  pc = a ? pc + 1 : (long *)*pc; break;                       // branch if zero
 * BNZ imm64   BNZ imm64          #case BNZ: pc = a ? (long *)*pc : pc + 1; break;                       // branch if not zero
 * ENT imm64   ENT imm64          #case ENT: { *--sp = (long)bp; bp = sp; sp = sp - *pc++; } break;      // enter subroutine
 * ADJ imm64   ADJ imm64          #case ADJ: sp = sp + *pc++; break;                                     // stack adjust
 * LI  imm64   LI  imm64          #case LI:  switch(*pc++) { case CHAR: a = *(char *)a; ...}     // load int
 * SI  imm64   SI  imm64          #case SI:  switch(*pc++) { case CHAR: *(char *)*sp++ = a; ...} // store int
 * LEV         LEV                #case LEV: { sp = bp; bp = (long *)*sp++; pc = (long *)*sp++; } break; // leave subroutine
 * PSH         PSH a              #case PSH: *--sp = a; break;                                           // push
 *                                #
 * OR          OR  a, [sp]        #case OR:  a = *sp++ |  a; break;
 * XOR         XOR a, [sp]        #case XOR: a = *sp++ ^  a; break;
 * AND         AND a, [sp]        #case AND: a = *sp++ &  a; break;
 * EQ          EQ  a, [sp]        #case EQ:  a = *sp++ == a; break;
 * NE          NE  a, [sp]        #case NE:  a = *sp++ != a; break;
 * LT          LT  a, [sp]        #case LT:  a = *sp++ <  a; break;
 * GT          GT  a, [sp]        #case GT:  a = *sp++ >  a; break;
 * LE          LE  a, [sp]        #case LE:  a = *sp++ <= a; break;
 * GE          GE  a, [sp]        #case GE:  a = *sp++ >= a; break;
 * SHL         SHL a, [sp]        #case SHL: a = *sp++ << a; break;
 * SHR         SHR a, [sp]        #case SHR: a = *sp++ >> a; break;
 * ADD         ADD a, [sp]        #case ADD: a = *sp++ +  a; break;
 * SUB         SUB a, [sp]        #case SUB: a = *sp++ -  a; break;
 * MUL         MUL a, [sp]        #case MUL: a = *sp++ *  a; break;
 * DIV         DIV a, [sp]        #case DIV: a = *sp++ /  a; break;
 * MOD         MOD a, [sp]        #case MOD: a = *sp++ %  a; break;
 *                                #
 * PRTF        PRTF a, [sp]       #case PRTF: t = sp + pc[1]; a = printf((char *)t[-1], t[-2], ...); break;
 * KSYM        KSYM a, [sp]       #case KSYM: a = ksymbol(*sp); break;
 *                                #case EXIT: return a;
**/

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>
#include <stdarg.h>
#include <arpa/inet.h>

#include <monitor.h>
#ifdef CONFIG_LIBBPF
#include <linux/filter.h>
#include <linux/bpf_disasm.h>
#endif
#include <tep.h>
#include <expr.h>
#include <stack_helpers.h>

extern const char *syscalls_table[];
extern const int syscalls_table_size;

char *p, *lp, // current position in source code
     *data, *str;   // data/string pointer

long *e, *le,  // current position in emitted code
    tk,       // current token
    ival,     // current token value
    ty,       // current expression type
    loc;      // local variable offset

struct symbol_table *id;       // currently parsed identifier
struct symbol_table *symtab;     // symbol table (simple list of identifiers)
int n_syms, nr_syms;

jmp_buf synerr_jmp;


// tokens and classes (operators last and in precedence order)
enum {
    Num = 128, Fun, Sys, Glo, Loc, Id,
    Int, Sizeof,
    Assign, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Match, Shl, Shr, Add, Sub, Mul, Div, Mod, Inc, Dec, Brak
};

// opcodes
enum { LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LI  ,SI  ,LEV ,PSH ,LTu ,GTu ,LEu, GEu ,
       OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,SAR, ADD ,SUB ,MUL ,DIV ,MOD ,DIVu,MODu,
       PRTF, KSYM, NTHL, NTHS, STRNCMP, COMM, MATCH, STREQ, STRNE, SYSCALL, KVMEXIT, SYSTEM, EXIT };

// types
enum { CHAR, SHORT, INT, LONG, ARRAY = 0x4, UNSIGNED = 0x8, PTR = 0x10 };

#define INSN "LEA ,IMM ,JMP ,JSR ,BZ  ,BNZ ,ENT ,ADJ ,LI  ,SI  ,LEV ,PSH ,LTu ,GTu ,LEu, GEu ," \
             "OR  ,XOR ,AND ,EQ  ,NE  ,LT  ,GT  ,LE  ,GE  ,SHL ,SHR ,SAR ,ADD ,SUB ,MUL ,DIV ,MOD ,DIVu,MODu," \
             "PRTF,KSYM,NTHL,NTHS,SCMP,COMM,MACH,SSEQ,SSNE,SCAL,VMXT,SYST,EXIT,"

#define ADD_KEY(name, _token, _type) \
    { p = (char *)name; { next(); id->token = _token; id->class = 0; id->type = _type; id->value = 0; } }
#define ADD_LIB(name, _type, insn) \
    { p = (char *)name; { next(); id->class = Sys; id->type = _type; id->value = insn; } }
#define ADD_GLO(name, _type, _value) \
    { p = (char *)name; { next(); id->class = Glo; id->type = _type; id->nr_elm = 1; id->value = (long)_value; } }


#define TK_SHIFT 6
#define HASH(tk, len) (((tk) << TK_SHIFT) + (len))
#define LEN(hash) ((hash) & ((1 << TK_SHIFT) - 1))

static void synerr(const char *s)
{
    printf("%s\n", lp);
    printf("%*s%s\n", (int)(p-lp+1), "^ ", s);
    longjmp(synerr_jmp, -1);
}

/*
 * Whether `t' is a `char *' for the string operators (~, ==, !=), ignoring
 * signedness: since 3bc753c06dd0 ("kbuild: treat char as always unsigned"),
 * in v6.2, the kernel is built with -funsigned-char, so a `char comm[16]' is
 * described as signed:0 and arrives here as unsigned. No string operator
 * looks at the sign of a character, so accept either spelling.
 */
static bool is_char_ptr(long t)
{
    return (t & ~UNSIGNED) == (CHAR|PTR);
}

static int typeop(int op)
{
    switch (op) {
        case Sizeof: return (ty >= PTR) ? sizeof(void *) : (1 << (ty & ~UNSIGNED));
        case Assign: return (ty >= PTR) ? PTR : ty;
        case Inc:
        case Dec:
            return (ty>=PTR+PTR) ? sizeof(void *) : (ty>=PTR ? (1<<((ty-PTR) & ~UNSIGNED)) : 1);
        case Add:
        case Sub:
        case Brak:
            return (ty>=PTR+PTR) ? sizeof(void *) : (1<<((ty-PTR) & ~UNSIGNED));
        default: synerr("bad typeop");
    }
    return -1;
}

static void next(void)
{
    char *pp;

    while ((tk = *p)) {
        ++p;
        if (tk == '\n') ;
        else if (tk == '#') {
            while (*p != 0 && *p != '\n') ++p;
        }
        else if ((tk >= 'a' && tk <= 'z') || (tk >= 'A' && tk <= 'Z') || tk == '_') {
            pp = p - 1;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')
                tk = tk * 147 + *p++;
            tk = HASH(tk, p - pp);
            id = symtab;
            while (id < symtab+nr_syms && id->token) {
                if (tk == id->hash && !memcmp(id->name, pp, p - pp)) { tk = id->token; return; }
                id ++;
            }
            if (id == symtab+nr_syms) {
                nr_syms += 16;
                symtab = realloc(symtab, nr_syms*sizeof(*symtab));
                memset(&symtab[nr_syms-16], 0, 16*sizeof(*symtab));
                id = &symtab[n_syms];
            }
            n_syms ++;
            id->name = pp;
            id->hash = tk;
            tk = id->token = Id;
            return;
        }
        else if (tk >= '0' && tk <= '9') {
            if ((ival = tk - '0')) {
                while (*p >= '0' && *p <= '9') ival = ival * 10 + *p++ - '0';
            }
            else if (*p == 'x' || *p == 'X') {
                while ((tk = *++p) && ((tk >= '0' && tk <= '9') || (tk >= 'a' && tk <= 'f') || (tk >= 'A' && tk <= 'F')))
                    ival = ival * 16 + (tk & 15) + (tk >= 'A' ? 9 : 0);
            } else {
                while (*p >= '0' && *p <= '7')
                    ival = ival * 8 + *p++ - '0';
            }
            tk = Num;
            return;
        }
        else if (tk == '/') {
            if (*p == '/') {
                ++p;
                while (*p != 0 && *p != '\n') ++p;
            } else {
                tk = Div;
                return;
            }
        }
        else if (tk == '\'' || tk == '"') {
            pp = str;
            while (*p != 0 && *p != tk) {
                if ((ival = *p++) == '\\') {
                    if ((ival = *p++) == 'n') ival = '\n';
                }
                if (tk == '"') *str++ = ival;
            }
            ++p; *str++ = '\0';
            if (tk == '"') ival = (long)pp;
            else tk = Num;
            return;
        }
        else if (tk == '=') { if (*p == '=') { ++p; tk = Eq; } else tk = Assign; return; }
        else if (tk == '+') { if (*p == '+') { ++p; tk = Inc; } else tk = Add; return; }
        else if (tk == '-') { if (*p == '-') { ++p; tk = Dec; } else tk = Sub; return; }
        else if (tk == '!') { if (*p == '=') { ++p; tk = Ne; } return; }
        else if (tk == '<') { if (*p == '=') { ++p; tk = Le; } else if (*p == '<') { ++p; tk = Shl; } else tk = Lt; return; }
        else if (tk == '>') { if (*p == '=') { ++p; tk = Ge; } else if (*p == '>') { ++p; tk = Shr; } else tk = Gt; return; }
        else if (tk == '|') { if (*p == '|') { ++p; tk = Lor; } else tk = Or; return; }
        else if (tk == '&') { if (*p == '&') { ++p; tk = Lan; } else tk = And; return; }
        else if (tk == '^') { tk = Xor; return; }
        else if (tk == '%') { tk = Mod; return; }
        else if (tk == '*') { tk = Mul; return; }
        else if (tk == '[') { tk = Brak; return; }
        else if (tk == '?') { tk = Cond; return; }
        else if (tk == '~') { tk = Match; return; }
        else if (tk == ';' || tk == '{' || tk == '}' || tk == '(' || tk == ')' || tk == ']' || tk == ',' || tk == ':') return;
    }
}

static void expr(int lev)
{
    long t, *d;

    if (!tk) { synerr("unexpected eof in expression"); }
    else if (tk == Num) { *++e = IMM; *++e = ival; next(); ty = INT; }
    else if (tk == '"') {
        *++e = IMM; *++e = ival; next();
        while (tk == '"') next();
        ty = (CHAR|PTR);
    }
    else if (tk == Sizeof) {
        next(); if (tk == '(') next(); else { synerr("open paren expected in sizeof"); }
        if (tk == Int) { ty = id->type; if (ty == UNSIGNED) ty |= INT; next(); } else { synerr("wrong type"); }
        if (tk == Int) { ty = (ty&UNSIGNED) | id->type; next(); }
        while (tk == Mul) { next(); ty = ty + PTR; }
        if (tk == ')') next(); else { synerr("close paren expected in sizeof"); }
        *++e = IMM; *++e = typeop(Sizeof);
        ty = INT;
    }
    else if (tk == Id) {
        struct symbol_table *s = id;
        next();
        if (tk == '(') {
            next();
            t = 0;
            while (tk != ')') { expr(Assign); *++e = PSH; ++t; if (tk == ',') next(); }
            next();
            if (s->class == Sys) { *++e = s->value; s->ref++; }
            else if (s->class == Fun) { *++e = JSR; *++e = s->value; s->ref++; }
            else { synerr("bad function call"); }
            if (t) { *++e = ADJ; *++e = t; }
            ty = s->type;
        }
        else if (s->class == Num) { *++e = IMM; *++e = s->value; ty = INT; }
        else {
            if (s->class == Loc) { *++e = LEA; *++e = loc - s->value; s->ref++; }
            else if (s->class == Glo) { *++e = IMM; *++e = s->value; s->ref++; }
            else { synerr("undefined variable"); }
            ty = s->type;
            if (ty & ARRAY) { ty &= ~ARRAY; ty |= PTR; }
            else { *++e = LI; *++e = typeop(Assign); }
        }
    }
    else if (tk == '(') {
        next();
        if (tk == Int) {
            t = id->type; if (t == UNSIGNED) t |= INT; next();
            if (tk == Int) { t = (t&UNSIGNED) | id->type; next(); }
            while (tk == Mul) { next(); t = t + PTR; }
            if (tk == ')') next(); else { synerr("bad cast"); }
            expr(Inc);
            ty = t;
            if (*(e-1) == LI) *e = typeop(Assign);
        }
        else {
            expr(Assign);
            if (tk == ')') next(); else { synerr("close paren expected"); }
        }
    }
    else if (tk == Mul) {
        next(); expr(Inc);
        if (ty >= PTR) ty = ty - PTR; else { synerr("bad dereference"); }
        *++e = LI; *++e = typeop(Assign);
    }
    else if (tk == And) {
        next(); expr(Inc);
        if (*(e-1) == LI) e -= 2; else { synerr("bad address-of"); }
        ty = ty + PTR;
    }
    else if (tk == '!') { next(); expr(Inc); *++e = PSH; *++e = IMM; *++e = 0; *++e = EQ; ty = INT; }
    else if (tk == Match) { next(); expr(Inc); *++e = PSH; *++e = IMM; *++e = -1; *++e = XOR; ty = INT; }
    else if (tk == Add) { next(); expr(Inc); ty = INT; }
    else if (tk == Sub) {
        next(); *++e = IMM;
        if (tk == Num) { *++e = -ival; next(); } else { *++e = -1; *++e = PSH; expr(Inc); *++e = MUL; }
        ty = INT;
    }
    else if (tk == Inc || tk == Dec) {
        t = tk; next(); expr(Inc);
        if (*(e-1) == LI) { *(e-1) = PSH; ++e; *e = *(e-1); *(e-1) = LI; }
        else { synerr("bad lvalue in pre-increment"); }
        *++e = PSH;
        *++e = IMM; *++e = typeop(t);
        *++e = (t == Inc) ? ADD : SUB;
        *++e = SI; *++e = typeop(Assign);
    }
    else { synerr("bad expression"); }

    while (tk >= lev) { // "precedence climbing" or "Top Down Operator Precedence" method
        t = ty;
        if (tk == Assign) {
            next();
            if (*(e-1) == LI) *--e = PSH; else { synerr("bad lvalue in assignment"); }
            expr(Assign); ty = t; *++e = SI; *++e = typeop(Assign);
        }
        else if (tk == Cond) {
            next();
            *++e = BZ; d = ++e;
            expr(Assign);
            if (tk == ':') next(); else { synerr("conditional missing colon"); }
            *d = (long)(e + 3); *++e = JMP; d = ++e;
            expr(Cond);
            *d = (long)(e + 1);
        }
        else if (tk == Lor) { next(); *++e = BNZ; d = ++e; expr(Lan); *d = (long)(e + 1); ty = INT; }
        else if (tk == Lan) { next(); *++e = BZ;  d = ++e; expr(Or);  *d = (long)(e + 1); ty = INT; }
        else if (tk == Or)  { next(); *++e = PSH; expr(Xor); *++e = OR;  ty = INT; }
        else if (tk == Xor) { next(); *++e = PSH; expr(And); *++e = XOR; ty = INT; }
        else if (tk == And) { next(); *++e = PSH; expr(Eq);  *++e = AND; ty = INT; }
        else if (tk == Eq)  {
            next(); *++e = PSH; expr(Lt);
            if (is_char_ptr(t) && is_char_ptr(ty)) *++e = STREQ;
            else *++e = EQ;
            ty = INT;
        }
        else if (tk == Ne)  {
            next(); *++e = PSH; expr(Lt);
            if (is_char_ptr(t) && is_char_ptr(ty)) *++e = STRNE;
            else *++e = NE;
            ty = INT;
        }
        else if (tk == Lt)  { next(); *++e = PSH; expr(Shl); *++e = (t>=PTR || ty>=PTR || ((t|ty)&UNSIGNED))?LTu:LT;  ty = INT; }
        else if (tk == Gt)  { next(); *++e = PSH; expr(Shl); *++e = (t>=PTR || ty>=PTR || ((t|ty)&UNSIGNED))?GTu:GT;  ty = INT; }
        else if (tk == Le)  { next(); *++e = PSH; expr(Shl); *++e = (t>=PTR || ty>=PTR || ((t|ty)&UNSIGNED))?LEu:LE;  ty = INT; }
        else if (tk == Ge)  { next(); *++e = PSH; expr(Shl); *++e = (t>=PTR || ty>=PTR || ((t|ty)&UNSIGNED))?GEu:GE;  ty = INT; }
        else if (tk == Match) {
            if (!is_char_ptr(t)) synerr("~ operator requires char * operand");
            next(); *++e = PSH; expr(Shl);
            if (!is_char_ptr(ty)) synerr("~ operator requires char * operand");
            *++e = MATCH; ty = INT;
        }
        else if (tk == Shl) { next(); *++e = PSH; expr(Add); *++e = SHL; ty = INT; }
        else if (tk == Shr) { next(); *++e = PSH; expr(Add); *++e = (t>=PTR || ty>=PTR || ((t|ty)&UNSIGNED))?SHR:SAR; ty = INT; }
        else if (tk == Add) {
            next(); *++e = PSH; expr(Mul);
            if ((ty = t) > PTR) { *++e = PSH; *++e = IMM; *++e = typeop(Add); *++e = MUL;  }
            *++e = ADD;
        }
        else if (tk == Sub) {
            next(); *++e = PSH; expr(Mul);
            if (t > PTR && t == ty) { *++e = SUB; *++e = PSH; *++e = IMM; *++e = typeop(Sub); *++e = DIV; ty = INT; }
            else if ((ty = t) > PTR) { *++e = PSH; *++e = IMM; *++e = typeop(Sub); *++e = MUL; *++e = SUB; }
            else *++e = SUB;
        }
        else if (tk == Mul) { next(); *++e = PSH; expr(Inc); *++e = MUL; ty = INT; }
        else if (tk == Div) { next(); *++e = PSH; expr(Inc); *++e = (t>=PTR || ty>=PTR || ((t|ty)&UNSIGNED))?DIVu:DIV; ty = INT; }
        else if (tk == Mod) { next(); *++e = PSH; expr(Inc); *++e = (t>=PTR || ty>=PTR || ((t|ty)&UNSIGNED))?MODu:MOD; ty = INT; }
        else if (tk == Inc || tk == Dec) {
            if (*(e-1) == LI) { *(e-1) = PSH; ++e; *e = *(e-1); *(e-1) = LI; }
            else { synerr("bad lvalue in post-increment"); }
            *++e = PSH;
            *++e = IMM; *++e = typeop(tk); //*++e = (ty > PTR) ? sizeof(long) : sizeof(char);
            *++e = (tk == Inc) ? ADD : SUB;
            *++e = SI; *++e = typeop(Assign);
            *++e = PSH;
            *++e = IMM; *++e = typeop(tk); //(ty > PTR) ? sizeof(long) : sizeof(char);
            *++e = (tk == Inc) ? SUB : ADD;
            next();
        }
        else if (tk == Brak) {
            next(); *++e = PSH; expr(Assign);
            if (tk == ']') next(); else { synerr("close bracket expected"); }
            if (t > PTR) { *++e = PSH; *++e = IMM; ty = t; *++e = typeop(Brak); *++e = MUL;  }
            else if (t < PTR) { synerr("pointer type expected"); }
            *++e = ADD;
            ty = t - PTR;
            *++e = LI; *++e = typeop(Assign);
        }
        else { synerr("compiler error"); }
    }
}

static const char *ksymbol(unsigned long func)
{
    char *name = function_resolver(NULL, (unsigned long long *)&func, NULL);
    return name ? : "Unknown";
}

/* Wildcard matching function that implements trace event filter's ~ operator
 * Supports:
 *   * - matches any number of characters
 *   ? - matches exactly one character
 *   [abc] - matches any character in the set
 *   [a-z] - matches any character in the range
 *   [^abc]|[!abc] - matches any character NOT in the set
 * Returns: 1 if string matches pattern, 0 otherwise
 */
static int wildcard_match(const char *str, const char *pattern)
{
    const char *s, *p, *star = NULL, *str_backup = NULL;
    int invert, match;

    if (!str || !pattern)
        return 0;

    s = str;
    p = pattern;

    while (*s) {
        if (*p == '*') {
            /* Skip consecutive stars */
            while (*p == '*')
                p++;
            /* If star is at the end, it matches everything */
            if (!*p)
                return 1;
            /* Save positions for backtracking */
            star = p;
            str_backup = s;
        } else if (*p == '?') {
            /* ? matches exactly one character */
            s++;
            p++;
        } else if (*p == '[') {
            /* Character class matching */
            p++;
            invert = 0;
            if (*p == '^' || *p == '!') {
                invert = 1;
                p++;
            }
            match = 0;
            while (*p && *p != ']') {
                if (*(p+1) == '-' && *(p+2) != ']' && *(p+2)) {
                    /* Range: [a-z] */
                    if (*s >= *p && *s <= *(p+2))
                        match = 1;
                    p += 3;
                } else {
                    /* Single character */
                    if (*s == *p)
                        match = 1;
                    p++;
                }
            }
            if (*p == ']')
                p++;
            if (invert)
                match = !match;
            if (!match) {
                /* Backtrack if we have a star */
                if (star) {
                    p = star;
                    s = ++str_backup;
                    continue;
                }
                return 0;
            }
            s++;
        } else if (*p == *s) {
            /* Exact character match */
            s++;
            p++;
        } else {
            /* No match, try to backtrack */
            if (star) {
                p = star;
                s = ++str_backup;
            } else {
                return 0;
            }
        }
    }

    /* Skip any trailing stars */
    while (*p == '*')
        p++;

    /* Match if we've consumed both strings */
    return !*p;
}
static void dump_symtab(struct symbol_table *symtab, int nr, bool print_value);
struct expr_prog *expr_compile_flags(char *expr_str, struct global_var_declare *declare,
                                     unsigned int flags)
{
    int i, err;
    int nr_insn = 1024;
    int datasize = 256;
    /* Upper bound on the literals: each one loses its two delimiters and gains
     * a terminator, so it never outgrows its source text. The +1 guarantees a
     * zero byte past the last literal, where expr_dump() stops walking. */
    int strsize = strlen(expr_str) + 1;
    char *d, *s;
    char *names = NULL, *nm;
    struct expr_prog *prog = NULL;

    // reset
    loc = 0;
    n_syms = 0;
    // Default keywords (6) + library symbols (9) + default global (2) + tracepoint headers (4) = 21 symbols
    // Original nr_syms = 16 is too small, memory reallocations needed
    nr_syms = 32;
    symtab = calloc(nr_syms, sizeof(struct symbol_table));
    data = d = malloc(datasize);
    str = s = malloc(strsize);
    le = e = calloc(nr_insn, sizeof(long));

    if (!symtab || !data || !str || !le)
        goto err_return;

    prog = calloc(1, sizeof(*prog));
    if (!prog)
        goto err_return;


    // add keywords to symbol table
    ADD_KEY("char", Int, CHAR);
    ADD_KEY("short", Int, SHORT);
    ADD_KEY("int", Int, INT);
    ADD_KEY("long", Int, LONG);
    ADD_KEY("unsigned", Int, UNSIGNED);
    ADD_KEY("sizeof", Sizeof, INT);

    // add library to symbol table
    // int printf(const char *format, ...);
    ADD_LIB("printf", INT, PRTF);
    // char *ksymbol(unsigned long func);
    ADD_LIB("ksymbol", CHAR | PTR, KSYM);
    // uint32_t ntohl(uint32_t netlong);
    ADD_LIB("ntohl", INT, NTHL);
    // uint16_t ntohs(uint16_t netshort);
    ADD_LIB("ntohs", SHORT, NTHS);
    // int strncmp(const char *s1, const char *s2, long n);
    ADD_LIB("strncmp", INT, STRNCMP);
    // char *comm_get(int pid);
    ADD_LIB("comm_get", CHAR | PTR, COMM);
    // char *syscall_name(int id);
    ADD_LIB("syscall_name", CHAR | PTR, SYSCALL);
    // char *exit_reason_str(int isa, int val);
    ADD_LIB("exit_reason_str", CHAR | PTR, KVMEXIT);
    // int system(const char *format, ...);
    ADD_LIB("system", INT, SYSTEM);

    // add default global var to symbol table
    if (!(flags & EXPR_F_NO_SAMPLE_GLO)) {
        ADD_GLO("_cpu", INT, &prog->glo._cpu);
        ADD_GLO("_pid", INT, &prog->glo._pid);
    }


    data = d; // reset data
    memset(d, 0, datasize);
    str = s; // reset str
    memset(s, 0, strsize);

    // global variable declaration
    if (declare) {
        struct global_var_declare *save = declare;
        int max_offset, name_size = 0;

        while (declare->name) {
            name_size += strlen(declare->name) + 1;
            declare ++;
        }
        /* Copy the names: declare[] and its name fields (e.g. the synthetic
         * "_offset", "_len") are temporary and the caller frees them once this
         * returns, yet symtab[].name keeps pointing at them for expr_dump().
         * They get a buffer of their own rather than sharing `str', which then
         * holds nothing but the expression's own string literals. */
        names = malloc(name_size);
        if (!names) goto err_return;

    restart:
        max_offset = 0;
        declare = save;
        nm = names;
        while (declare->name) {
            p = strcpy(nm, declare->name);
            nm += strlen(declare->name) + 1;
            next();
            id->class = Glo;
            switch (declare->elementsize) {
                case sizeof(char) : id->type = CHAR; break;
                case sizeof(short) : id->type = SHORT; break;
                case sizeof(int) : id->type = INT; break;
                case sizeof(long) : id->type = LONG; break;
                default: goto err_return;
            }
            if (declare->size != declare->elementsize)
                id->type |= ARRAY;
            if (declare->is_unsigned)
                id->type |= UNSIGNED;
            id->nr_elm = declare->size / declare->elementsize;
            id->value = (long)data + declare->offset;
            if (declare->offset + declare->size > max_offset)
                max_offset = declare->offset + declare->size;
            declare ++;
        }
        if (max_offset > datasize) {
            datasize += 256;
            data = d = realloc(d, datasize);
            memset(d, 0, datasize);
            goto restart;
        }
        data += max_offset;
    }

    lp = p = expr_str;

    err = setjmp(synerr_jmp);
    if (err == 0) {
        do {
            next();
            expr(Assign);
        } while (tk == ',');
        *++e = EXIT;
    } else
        goto err_return;

    nr_insn = e - le + 1;

    prog->symtab = realloc(symtab, n_syms*sizeof(*symtab));
    prog->nr_syms = n_syms;
    if (data - d) {
        prog->data = d; prog->datasize = datasize; prog->datalen = data - d;
    } else {
        free(d); prog->data = NULL; prog->datasize = 0; prog->datalen = 0;
    }
    if (str - s) { prog->str = s; prog->strsize = str - s; }
    else { free(s); prog->str = NULL; prog->strsize = 0; }
    prog->names = names;
    names = NULL;
    prog->insn = realloc(le, nr_insn*sizeof(long));
    prog->nr_insn = nr_insn;
    // FIX: Instruction relocation mechanism for jump targets after realloc
    // For JMP/JSR/BZ/BNZ instructions, adjust target address by offset
    if (prog->insn != le) {
        for (i=1; i<nr_insn; i++) {
            switch (prog->insn[i]) {
                case JMP: case JSR: case BZ: case BNZ:
                    prog->insn[i+1] -= (void *)le - (void *)prog->insn;
                default: break;
            }
            if (prog->insn[i] <= SI) i++;
        }
    }

    for (i=0; i<prog->nr_syms; i++) {
        struct symbol_table *sym = &prog->symtab[i];
        if (sym->class == Sys && sym->value == KSYM && sym->ref)
            function_resolver_ref();
        if (sym->class == Sys && sym->value == COMM && sym->ref)
            global_comm_ref();
    }

    return prog;

err_return:
    printf("Available variables:\n");
    dump_symtab(symtab, nr_syms, 0);
    if (symtab) free(symtab);
    if (d) free(d);
    if (s) free(s);
    if (names) free(names);
    if (le) free(le);
    if (prog) free(prog);
    return NULL;
}

struct expr_prog *expr_compile(char *expr_str, struct global_var_declare *declare)
{
    return expr_compile_flags(expr_str, declare, 0);
}

long expr_run(struct expr_prog *prog)
{
    long *pc, *sp, *bp, a, cycle; // vm registers
    long i, *t; // temps
    long stack[512];

    pc = prog->insn + 1;
    bp = sp = stack + 512;
    a = 0;
    cycle = 0;
    if (prog->debug) printf("Program running:\n");
    while (1) {
        i = *pc++;
        ++cycle;
        if (prog->debug) {
            if (cycle > 1) printf("; a: 0x%lx\n", a);
            printf("%ld> %.4s", cycle, &INSN[i * 5]);
            if (i <= SI) printf(" 0x%-16lx", *pc); else printf(" %-18s", "");
        }
        switch (i) {
            case LEA: a = (long)(bp + *pc++); break;                              // load local address
            case IMM: a = *pc++; break;                                           // load global address or immediate
            case JMP: pc = (long *)*pc; break;                                    // jump
            case JSR: { *--sp = (long)(pc + 1); pc = (long *)*pc; } break;        // jump to subroutine
            case BZ:  pc = a ? pc + 1 : (long *)*pc; break;                       // branch if zero
            case BNZ: pc = a ? (long *)*pc : pc + 1; break;                       // branch if not zero
            case ENT: { *--sp = (long)bp; bp = sp; sp = sp - *pc++; } break;      // enter subroutine
            case ADJ: sp = sp + *pc++; break;                                     // stack adjust
            case LI:  switch(*pc++) {                                             // load int
                          case CHAR: a = *(char *)a; break;
                          case UNSIGNED|CHAR: a = *(unsigned char *)a; break;
                          case SHORT: a = *(short *)a; break;
                          case UNSIGNED|SHORT: a = *(unsigned short *)a; break;
                          case INT: a = *(int *)a; break;
                          case UNSIGNED|INT: a = *(unsigned int *)a; break;
                          case LONG: a = *(long *)a; break;
                          case UNSIGNED|LONG: a = *(unsigned long *)a; break;
                          case PTR: a = *(unsigned long *)a; break;
                          default: printf("wrong instruction\n"); return -1;
                      } break;
            case SI:  switch(*pc++) {                                             // store int
                          case CHAR: *(char *)*sp++ = a; break;
                          case UNSIGNED|CHAR: *(unsigned char *)*sp++ = a; break;
                          case SHORT: *(short *)*sp++ = a; break;
                          case UNSIGNED|SHORT: *(unsigned short *)*sp++ = a; break;
                          case INT: *(int *)*sp++ = a; break;
                          case UNSIGNED|INT: *(unsigned int *)*sp++ = a; break;
                          case LONG: *(long *)*sp++ = a; break;
                          case UNSIGNED|LONG: *(unsigned long *)*sp++ = a; break;
                          case PTR: *(unsigned long *)*sp++ = a; break;
                          default: printf("wrong instruction\n"); return -1;
                      } break;

            case LEV: { sp = bp; bp = (long *)*sp++; pc = (long *)*sp++; } break; // leave subroutin
            case PSH: *--sp = a; break;                                           // push

            case OR:  a = *sp++ |  a; break;
            case XOR: a = *sp++ ^  a; break;
            case AND: a = *sp++ &  a; break;
            case EQ:  a = *sp++ == a; break;
            case NE:  a = *sp++ != a; break;
            case LT:  a = *sp++ <  a; break;
            case GT:  a = *sp++ >  a; break;
            case LE:  a = *sp++ <= a; break;
            case GE:  a = *sp++ >= a; break;
            case LTu: a = (unsigned long)(*sp++) <  (unsigned long)a; break;
            case GTu: a = (unsigned long)(*sp++) >  (unsigned long)a; break;
            case LEu: a = (unsigned long)(*sp++) <= (unsigned long)a; break;
            case GEu: a = (unsigned long)(*sp++) >= (unsigned long)a; break;
            case SHL: a = *sp++ << a; break;
            case SHR: a = (unsigned long)(*sp++) >> (unsigned long)a; break;
            case SAR: a = *sp++ >> a; break;
            case ADD: a = *sp++ +  a; break;
            case SUB: a = *sp++ -  a; break;
            case MUL: a = *sp++ *  a; break;
            case DIV: a = *sp++ /  a; break;
            case DIVu:a = (unsigned long)(*sp++) /  (unsigned long)a; break;
            case MOD: a = *sp++ %  a; break;
            case MODu:a = (unsigned long)(*sp++) %  (unsigned long)a; break;

            case PRTF: t = sp + pc[1]; a = printf((char *)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6], t[-7]); break;
            case KSYM: a = (long)(void *)ksymbol(*sp); break;
            case NTHL: a = (int)ntohl((int)*sp); break;
            case NTHS: a = (short)ntohs((short)*sp); break;
            case STRNCMP: t = sp + pc[1]; a = strncmp((const char *)t[-1], (const char *)t[-2], (long)t[-3]); break;
            case COMM: a = (long)(void *)(global_comm_get((int)*sp) ?: "<...>"); break;
            case MATCH: a = wildcard_match((const char *)*sp++, (const char *)a); break;
            case STREQ: a = (strcmp((const char *)*sp++, (const char *)a) == 0); break;
            case STRNE: a = (strcmp((const char *)*sp++, (const char *)a) != 0); break;
            case SYSCALL: {
                    int id = (int)*sp;
                    if (id >= 0 && id < syscalls_table_size && syscalls_table[id])
                        a = (long)(void *)syscalls_table[id];
                    else
                        a = (long)(void *)"Unknown";
                } break;
            case KVMEXIT: t = sp + pc[1];
                    a = (long)(void *)find_exit_reason((unsigned int)t[-1], (unsigned int)t[-2]); break;
            case SYSTEM: t = sp + pc[1]; {
                    char cmd[4096];
                    int ret = snprintf(cmd, sizeof(cmd), (char *)t[-1], t[-2], t[-3], t[-4], t[-5], t[-6], t[-7]);
                    a = (ret < 0 || ret >= sizeof(cmd)) ? -1 : system(cmd);
                } break;
            case EXIT: if (prog->debug) printf("exit(0x%lx) cycle = %ld\n", a, cycle); return a;
            default: printf("unknown instruction = %ld! cycle = %ld\n", i, cycle); return -1;
        }
    }
}

int expr_load_glo(struct expr_prog *prog, const char *name, long value)
{
    int i;

    if (!prog) return 0;

    for (i=prog->nr_syms-1; i>=0; i--) {
        struct symbol_table *s = &prog->symtab[i];
        if (s->token == Id && s->class == Glo && !memcmp(s->name, name, LEN(s->hash))) {
            switch (s->type) {
                case CHAR: *(char *)s->value = (char)value; break;
                case UNSIGNED|CHAR: *(unsigned char *)s->value = (unsigned char)value; break;
                case SHORT: *(short *)s->value = (short)value; break;
                case UNSIGNED|SHORT: *(unsigned short *)s->value = (unsigned short)value; break;
                case INT: *(int *)s->value = (int)value; break;
                case UNSIGNED|INT: *(unsigned int *)s->value = (unsigned int)value; break;
                case LONG: *(long *)s->value = (long)value; break;
                case UNSIGNED|LONG: *(unsigned long *)s->value = (unsigned long)value; break;
                default: return -1;
            }
            return 0;
        }
    }
    return -1;
}

int expr_load_data(struct expr_prog *prog, void *d, int size)
{
    if (!prog || size > prog->datasize) return -1;
    memcpy(prog->data, d, size);
    prog->data[size] = 0;
    return 0;
}

int expr_load_global(struct expr_prog *prog, struct expr_global *global)
{
    if (!prog || !global) return -1;

    // Load tracepoint raw data
    if (global->data && global->size > 0) {
        if (expr_load_data(prog, global->data, global->size) != 0)
            return -1;
    }

    // Set default global variables
    prog->glo._cpu = global->_cpu;
    prog->glo._pid = global->_pid;

    return 0;
}

void expr_destroy(struct expr_prog *prog)
{
    int i;

    if (!prog) return;

    for (i=0; i<prog->nr_syms; i++) {
        struct symbol_table *s = &prog->symtab[i];
        if (s->class == Sys && s->value == KSYM && s->ref)
            function_resolver_unref();
        if (s->class == Sys && s->value == COMM && s->ref)
            global_comm_unref();
    }
    if (prog->symtab) free(prog->symtab);
    if (prog->data) free(prog->data);
    if (prog->names) free(prog->names);
    if (prog->str) free(prog->str);
    if (prog->insn) free(prog->insn);
    free(prog);
}

static void dump_symtab(struct symbol_table *symtab, int nr, bool print_value)
{
    int i;
    for (i = 0; i < nr; i++) {
        struct symbol_table *s = &symtab[i];
        if (s->token == Id && s->class == Glo) {
            if (print_value)
                printf("    %16p%s", (void *)s->value, s->ref ? " +" : "  ");
            else
                printf("    ");
            if (s->type & UNSIGNED) printf(" unsigned");
            switch (s->type & 0x3) {
                case CHAR: printf(" char"); break;
                case SHORT: printf(" short"); break;
                case INT: printf(" int"); break;
                case LONG: printf(" long"); break;
                default: break;
            }
            if (s->type >= PTR) {
                int t = s->type;
                printf(" "); do { printf("*"); t -= PTR; } while (t >= PTR);
            }
            if (s->type & ARRAY) {
                /* Display array dimensions - show empty brackets for
                 * flexible array members where nr_elm is 0 */
                if (s->nr_elm) printf(" [%d]", s->nr_elm);
                else printf(" []");
            }
            printf(" %.*s\n", (int)LEN(s->hash), s->name);
        }
    }
}

void expr_dump(struct expr_prog *prog)
{
    long *insn_end, *insn;

    if (!prog) return;
    insn = prog->insn;
    insn_end = insn+prog->nr_insn-1;

    printf("Instruction:\n");
    while (insn < insn_end) {
        printf("  %p:", insn+1);
        printf("%8.4s", &INSN[*++insn * 5]);
        if (*insn <= SI) printf(" 0x%lx\n", *++insn); else printf("\n");
    }

    if (prog->data) {
        printf("Global variable:\n");
        dump_symtab(prog->symtab, prog->nr_syms, 1);
    }

    if (prog->str) {
        char *c = prog->str;
        printf("Strings:\n");
        while (*c) {
            printf("    %p:", c);
            c += printf("    %s\n", c) - 4;
        }
    }
}


#ifdef CONFIG_LIBBPF

/*
 * eBPF backend: translate the compiled instruction stream into BPF
 * instructions, for use as a kernel-side filter on a BPF-generated event.
 *
 * Calling convention of the generated code, matching the expr_filter()
 * placeholder that it replaces (see src/bpf-skel/expr_filter.bpf.h):
 *   r1 - pointer to the event struct on entry
 *   r0 - result; non-zero keeps the event, zero drops it
 *
 * Global variables are bound the same way as for the userspace VM: their
 * address is `prog->data + offset`, so an `IMM addr; LI type` pair is
 * exactly `*(type *)(event + offset)` and folds into a single BPF_LDX_MEM.
 * Assignment is the mirror image, `IMM addr; PSH; <value>; SI type` becoming
 * a BPF_STX_MEM; see the SI case for why writing the event is allowed.
 *
 * The expression VM is stack based, but expr() only ever emits the pattern
 * `<sub-expression>; PSH; <sub-expression>; <binop>`, so the stack depth at
 * every point is known statically. Stack slot N is therefore mapped to a
 * register, avoiding any memory traffic. r1 holds the event pointer and
 * r0 the accumulator, leaving r2..r9 for slots.
 *
 * Using r6-r9 would need no saving on our part either, even though they are
 * callee-saved in the BPF calling convention, and this holds whether or not
 * the JIT is on:
 *
 *  - JIT: a subprogram is compiled as its own bpf_prog (jit_subprogs() splits
 *    each one out and calls bpf_int_jit_compile() on it), so it gets its own
 *    prologue and epilogue. On x86 r6-r9 map one-to-one onto the callee-saved
 *    rbx/r13/r14/r15, which the prologue pushes and every exit path pops, so a
 *    caller's r6-r9 survive the call even if the caller uses them for
 *    something else. How many get pushed is a per-kernel detail -- before 5.10
 *    the prologue pushed all four unconditionally, and since then
 *    detect_reg_usage() pushes only the ones that subprogram actually touches
 *    -- but either way the saving is the JIT's job, not the generated code's.
 *
 *  - Interpreter: fixup_call_args() rewrites the call to BPF_CALL_ARGS, which
 *    dispatches to one of interpreters_args[] (picked by stack depth). Each of
 *    those declares a fresh `u64 regs[MAX_BPF_EXT_REG]' on its own C stack
 *    frame and only copies r1-r5 in from the caller, so the callee's r6-r9 are
 *    different memory entirely and cannot alias the caller's.
 *
 * What the verifier does require is that a subprogram write r6-r9 before
 * reading them, which is not about protecting the caller: interpreters_args[]
 * leaves that array uninitialised, so a read would return stack garbage. A
 * slot is always pushed before it is read, so this is satisfied either way.
 *
 * Slots would normally start at r2 rather than r6, because r2-r5 map onto x86
 * registers that are caller-saved (rsi/rdx/rcx/r8) and so are never pushed at
 * all, on any kernel. The usual reason to prefer r6-r9 -- surviving a helper
 * call -- does not apply, because every builtin that would need one is
 * rejected below.
 *
 * That rests on clang honouring the calling convention: the call site is
 * compiled against a placeholder whose body is only `r0 = 1; exit', so nothing
 * but the ABI stops it from keeping a value in r2-r5 across the call. The build
 * checks this rather than assuming it -- src/bpf-skel/slot_check.bpf.c is
 * compiled and inspected by scripts/bpf_slot_check.py -- and defines
 * BPF_SLOT_FIRST_R6 if the check either fails or cannot be run, which costs
 * four slots and nothing else. See docs/bpf_event_filter.md 5.2.1 and 5.2.2.
 */

/* Named BPF_ACC, not BPF_A: <uapi/linux/filter.h>, pulled in by
 * <linux/filter.h>, uses BPF_A for a classic-BPF addressing mode. */
#define BPF_ACC        BPF_REG_0          /* accumulator, mirrors VM's `a' */
#define BPF_EVENT      BPF_REG_1          /* event pointer, never clobbered */
#ifdef BPF_SLOT_FIRST_R6
#define BPF_SLOT_FIRST BPF_REG_6
#else
#define BPF_SLOT_FIRST BPF_REG_2
#endif
#define BPF_SLOT_LAST  BPF_REG_9
#define BPF_NR_SLOTS   (BPF_SLOT_LAST - BPF_SLOT_FIRST + 1)

/* Upper bound on emitted instructions per VM instruction: the comparison ops
 * expand to 3, a sign-extending load to 3 on kernels without MEMSX. */
#define BPF_INSN_PER_OP 8

struct bpf_emit {
    struct bpf_insn *insn;
    int nr_insn;
    int max_insn;
    int depth;          /* current stack depth, in slots */
    int failed;
    const char *err;
    /* Whether the target kernel has the cpu v4 instructions -- resolved once
     * per translation, in expr_to_bpf(). */
    bool cpu_v4;
    /* Map from VM instruction index to emitted instruction index, so that
     * jump targets can be resolved once everything has been emitted. */
    int *pc_map;
    int nr_pc;
    /* Emitted jumps awaiting resolution, and the VM pc each targets. Only
     * these are patched; branches emitted for comparisons already hold a
     * correct relative offset. */
    int *fixup_at;
    int *fixup_to;
    int nr_fixup;
};

static void emit(struct bpf_emit *e, struct bpf_insn insn)
{
    if (e->failed)
        return;
    if (e->nr_insn >= e->max_insn) {
        e->failed = 1;
        e->err = "instruction buffer overflow";
        return;
    }
    e->insn[e->nr_insn++] = insn;
}

static void emit_fail(struct bpf_emit *e, const char *err)
{
    if (!e->failed) {
        e->failed = 1;
        e->err = err;
    }
}

/* Register holding stack slot `n' (0 is the bottom of the expression stack). */
static int slot_reg(struct bpf_emit *e, int n)
{
    if (n < 0 || n >= BPF_NR_SLOTS) {
        emit_fail(e, "expression too deeply nested for register allocation");
        return BPF_SLOT_FIRST;
    }
    return BPF_SLOT_FIRST + n;
}

/* Convert an LI/SI type operand into a BPF load/store size. */
static int bpf_size_of(struct bpf_emit *e, long type)
{
    if (type >= PTR)
        return BPF_DW;
    switch (type & ~UNSIGNED) {
        case CHAR:  return BPF_B;
        case SHORT: return BPF_H;
        case INT:   return BPF_W;
        case LONG:  return BPF_DW;
        default:
            emit_fail(e, "unsupported type in load");
            return BPF_DW;
    }
}

/*
 * True when the running kernel has the "cpu v4" instructions -- BPF
 * instruction-set version 4, what clang calls -mcpu=v4. They all landed
 * together in v6.6: sign-extension loads in 1f9a1ea821ff ("bpf: Support new
 * sign-extension load insns") and signed div/mod in ec0e2da95f72 ("bpf:
 * Support new signed div/mod instructions."), with the x86 and arm64 JITs
 * (cc88f540da52) in the same release. So one check covers both.
 *
 * This has to be a runtime test, not a build-time one: perf-prof is built once
 * and run on whatever kernel is in front of it, and the filter is generated
 * for the kernel it is about to be loaded into. It reads uname(), so the
 * result is cached in bpf_emit rather than asked for per instruction.
 */
static bool bpf_have_cpu_v4(void)
{
    return kernel_release() >= KERNEL_VERSION(6, 6, 0);
}

/*
 * Load a field into `dst'. The userspace VM's LI reads through a pointer of
 * the field's own type, so a narrow signed field arrives sign-extended; plain
 * BPF_LDX_MEM zero-extends instead, and the difference is visible to every
 * signed comparison downstream. Two ways to match the VM:
 *
 *   6.6+    one ldx in MEMSX mode. Sizes B/H/W only -- a DW load fills the
 *           register, so there is nothing left to extend.
 *   older   shift the sign bit up to bit 63 and arithmetic-shift back down.
 */
static void emit_load(struct bpf_emit *e, long type, int dst, int src, int off)
{
    int sz = bpf_size_of(e, type);
    int bits;

    if ((type & UNSIGNED) || type >= PTR || sz == BPF_DW) {
        emit(e, BPF_LDX_MEM(sz, dst, src, off));
        return;
    }

    if (e->cpu_v4) {
        /* BPF_LDX_MEMSX(sz, dst, src, off), written out: the macro lives in
         * the kernel's own filter.h, not the tools copy this tree carries. */
        emit(e, BPF_RAW_INSN(BPF_LDX | BPF_SIZE(sz) | BPF_MEMSX, dst, src, off, 0));
        return;
    }

    bits = sz == BPF_B ? 56 : (sz == BPF_H ? 48 : 32);
    emit(e, BPF_LDX_MEM(sz, dst, src, off));
    emit(e, BPF_ALU64_IMM(BPF_LSH, dst, bits));
    emit(e, BPF_ALU64_IMM(BPF_ARSH, dst, bits));
}

/*
 * Load a 64-bit constant. BPF_MOV64_IMM only carries 32 bits, sign-extended,
 * so wider values need ld_imm64, which occupies two instruction slots: the low
 * half in the first slot's imm, the high half in the second's. The second slot
 * is not an instruction of its own -- its opcode must be zero -- so jump
 * offsets, which count slots, stay correct without any special casing.
 */
static void emit_imm64(struct bpf_emit *e, int reg, long v)
{
    if (v == (int)v) {
        emit(e, BPF_MOV64_IMM(reg, (int)v));
        return;
    }
    /* BPF_LD_IMM64(reg, v), expanded: the macro is two comma-separated
     * initialisers, so it cannot be handed to emit() as one argument. */
    emit(e, BPF_RAW_INSN(BPF_LD | BPF_DW | BPF_IMM, reg, 0, 0, (__u32)v));
    emit(e, BPF_RAW_INSN(0, 0, 0, 0, (__u32)((__u64)v >> 32)));
}

/*
 * Emit `a = (slot <cond> a)`. eBPF has no set-on-condition, so materialise
 * the boolean with a branch. The comparison must happen before the result
 * register is written, because the right operand lives in the accumulator:
 *      if (slot <cond> a) goto +2; a = 0; goto +1; a = 1
 */
static void emit_cmp(struct bpf_emit *e, int op, int lhs)
{
    emit(e, BPF_JMP_REG(op, lhs, BPF_ACC, 2));
    emit(e, BPF_MOV64_IMM(BPF_ACC, 0));
    emit(e, BPF_JMP_A(1));
    emit(e, BPF_MOV64_IMM(BPF_ACC, 1));
}

/*
 * Translate the program. Returns a malloc'd instruction array, or NULL on
 * failure (with a diagnostic printed).
 */
struct bpf_insn *expr_to_bpf(struct expr_prog *prog, int *nr_insn)
{
    struct bpf_emit e = { .cpu_v4 = bpf_have_cpu_v4() };
    long *pc, *insn_end;
    long *base;
    int i, lhs;

    if (!prog || !prog->insn)
        return NULL;

    base = prog->insn;
    insn_end = base + prog->nr_insn;

    e.max_insn = prog->nr_insn * BPF_INSN_PER_OP + 8;
    e.insn = calloc(e.max_insn, sizeof(*e.insn));
    e.pc_map = calloc(prog->nr_insn + 1, sizeof(*e.pc_map));
    e.fixup_at = calloc(prog->nr_insn + 1, sizeof(*e.fixup_at));
    e.fixup_to = calloc(prog->nr_insn + 1, sizeof(*e.fixup_to));
    if (!e.insn || !e.pc_map || !e.fixup_at || !e.fixup_to)
        goto out;

    for (i = 0; i <= prog->nr_insn; i++)
        e.pc_map[i] = -1;

    /*
     * Every global the expression touches must live in the event, because the
     * only base register this backend has is the event pointer. A global
     * outside that range is backed by userspace memory the kernel cannot
     * reach; its address would otherwise fall through to the plain-immediate
     * case below and be emitted as a scalar, giving a load from a truncated
     * host address with no diagnostic. Catch it here instead of trusting the
     * frontend to have withheld the symbol (see EXPR_F_NO_SAMPLE_GLO).
     */
    for (i = 0; i < prog->nr_syms; i++) {
        struct symbol_table *sym = &prog->symtab[i];

        if (sym->token != Id || sym->class != Glo || !sym->ref)
            continue;
        if (prog->data && sym->value >= (long)prog->data &&
            sym->value < (long)prog->data + prog->datalen)
            continue;
        emit_fail(&e, "variable is not a field of the event");
        goto out;
    }

    /* prog->insn[0] is unused: expr_compile() pre-increments before storing. */
    pc = base + 1;
    while (pc < insn_end && !e.failed) {
        long op = *pc;

        e.pc_map[pc - base] = e.nr_insn;
        pc++;

        switch (op) {
            case IMM: {
                long v = *pc++;
                /* An IMM naming a global, immediately dereferenced by LI, is
                 * a field access: fold the pair into one load off the event
                 * pointer. */
                if (prog->data && pc < insn_end && *pc == LI &&
                    v >= (long)prog->data && v < (long)prog->data + prog->datalen) {
                    long type = pc[1];
                    int off = (int)(v - (long)prog->data);

                    e.pc_map[pc - base] = e.nr_insn;
                    pc += 2;
                    emit_load(&e, type, BPF_ACC, BPF_EVENT, off);
                } else if (prog->data &&
                           v >= (long)prog->data &&
                           v < (long)prog->data + prog->datalen) {
                    /* Address of a field, e.g. `&pid'. */
                    emit(&e, BPF_MOV64_REG(BPF_ACC, BPF_EVENT));
                    emit(&e, BPF_ALU64_IMM(BPF_ADD, BPF_ACC, (int)(v - (long)prog->data)));
                } else if (prog->str && v >= (long)prog->str &&
                           v < (long)prog->str + prog->strsize) {
                    emit_fail(&e, "string literals are not supported by the BPF backend");
                } else {
                    emit_imm64(&e, BPF_ACC, v);
                }
                break;
            }
            case LI: {
                /* A dereference that was not folded above: the address is in
                 * the accumulator. Sign-extends just like the folded case,
                 * which matters for e.g. `*(char *)&field'. */
                emit_load(&e, *pc++, BPF_ACC, BPF_ACC, 0);
                break;
            }
            case PSH:
                emit(&e, BPF_MOV64_REG(slot_reg(&e, e.depth), BPF_ACC));
                e.depth++;
                if (e.depth > BPF_NR_SLOTS)
                    emit_fail(&e, "expression too deeply nested for register allocation");
                break;

            case BZ:
            case BNZ: {
                long target = *pc++;
                int at = e.nr_insn;

                emit(&e, BPF_JMP_IMM(op == BZ ? BPF_JEQ : BPF_JNE, BPF_ACC, 0, 0));
                if (!e.failed) {
                    e.fixup_at[e.nr_fixup] = at;
                    e.fixup_to[e.nr_fixup] = (int)(((long *)target) - base);
                    e.nr_fixup++;
                }
                break;
            }
            case JMP: {
                long target = *pc++;
                int at = e.nr_insn;

                emit(&e, BPF_JMP_A(0));
                if (!e.failed) {
                    e.fixup_at[e.nr_fixup] = at;
                    e.fixup_to[e.nr_fixup] = (int)(((long *)target) - base);
                    e.nr_fixup++;
                }
                break;
            }

            case EXIT:
                emit(&e, BPF_EXIT_INSN());
                break;

            /* Binary operators: the left operand was pushed, the right is in
             * the accumulator. */
            case OR: case XOR: case AND: case SHL: case SHR: case SAR:
            case ADD: case SUB: case MUL: case DIVu: case MODu:
            case DIV: case MOD:
            case EQ: case NE:
            case LT: case GT: case LE: case GE:
            case LTu: case GTu: case LEu: case GEu: {
                if (e.depth <= 0) {
                    emit_fail(&e, "stack underflow");
                    break;
                }
                e.depth--;
                lhs = slot_reg(&e, e.depth);

                switch (op) {
                    /* Arithmetic and bitwise: a = lhs <op> a. Operands are
                     * reversed relative to BPF's dst <op>= src, so compute
                     * into the slot register and move back. */
                    case OR:   emit(&e, BPF_ALU64_REG(BPF_OR,  lhs, BPF_ACC)); goto commit;
                    case XOR:  emit(&e, BPF_ALU64_REG(BPF_XOR, lhs, BPF_ACC)); goto commit;
                    case AND:  emit(&e, BPF_ALU64_REG(BPF_AND, lhs, BPF_ACC)); goto commit;
                    case SHL:  emit(&e, BPF_ALU64_REG(BPF_LSH, lhs, BPF_ACC)); goto commit;
                    case SHR:  emit(&e, BPF_ALU64_REG(BPF_RSH, lhs, BPF_ACC)); goto commit;
                    case SAR:  emit(&e, BPF_ALU64_REG(BPF_ARSH, lhs, BPF_ACC)); goto commit;
                    case ADD:  emit(&e, BPF_ALU64_REG(BPF_ADD, lhs, BPF_ACC)); goto commit;
                    case SUB:  emit(&e, BPF_ALU64_REG(BPF_SUB, lhs, BPF_ACC)); goto commit;
                    case MUL:  emit(&e, BPF_ALU64_REG(BPF_MUL, lhs, BPF_ACC)); goto commit;
                    case DIVu: emit(&e, BPF_ALU64_REG(BPF_DIV, lhs, BPF_ACC)); goto commit;
                    case MODu: emit(&e, BPF_ALU64_REG(BPF_MOD, lhs, BPF_ACC)); goto commit;
                    /*
                     * Signed division and modulo. This batch of instructions
                     * did not spend two new opcodes on them: BPF_DIV and
                     * BPF_MOD are reused, and the off field -- which every
                     * other ALU instruction requires to be 0 -- selects signed
                     * when set to 1. Hence BPF_RAW_INSN rather than
                     * BPF_ALU64_REG(), whose off is hardcoded to 0.
                     *
                     * The verifier reads it exactly that way, in
                     * adjust_scalar_min_max_vals():
                     *      if (off == 1) scalar_min_max_sdiv(dst_reg, &src_reg);
                     *      else          scalar_min_max_udiv(dst_reg, &src_reg);
                     * and off==1 is rejected for any other opcode. A pre-6.6
                     * kernel has no such exemption -- it requires off==0 for
                     * every ALU op and answers "BPF_ALU uses reserved fields"
                     * -- so the version check has to come first.
                     */
                    case DIV:
                    case MOD:
                        if (!e.cpu_v4) {
                            emit_fail(&e, "signed division requires a 6.6+ kernel");
                            break;
                        }
                        emit(&e, BPF_RAW_INSN(BPF_ALU64 | BPF_OP(op == DIV ? BPF_DIV : BPF_MOD) |
                                              BPF_X, lhs, BPF_ACC, 1, 0));
                        goto commit;
                    commit:
                        emit(&e, BPF_MOV64_REG(BPF_ACC, lhs));
                        break;

                    case EQ:  emit_cmp(&e, BPF_JEQ, lhs); break;
                    case NE:  emit_cmp(&e, BPF_JNE, lhs); break;
                    case LT:  emit_cmp(&e, BPF_JSLT, lhs); break;
                    case GT:  emit_cmp(&e, BPF_JSGT, lhs); break;
                    case LE:  emit_cmp(&e, BPF_JSLE, lhs); break;
                    case GE:  emit_cmp(&e, BPF_JSGE, lhs); break;
                    case LTu: emit_cmp(&e, BPF_JLT, lhs); break;
                    case GTu: emit_cmp(&e, BPF_JGT, lhs); break;
                    case LEu: emit_cmp(&e, BPF_JLE, lhs); break;
                    case GEu: emit_cmp(&e, BPF_JGE, lhs); break;
                    default:  emit_fail(&e, "unsupported binary operator"); break;
                }
                break;
            }

            /*
             * Store to an event field: `*sp++ = a', so the slot holds the
             * destination address (an `IMM addr' that was not folded into a
             * load) and the accumulator holds the value.
             *
             * Writing to the event is deliberately allowed, for two reasons.
             * First, a filter sometimes needs to correct or annotate what it
             * is about to emit -- masking a field, rescaling a latency -- and
             * doing that here is cheaper than shipping the event to userspace
             * only to rewrite it. Second, the expression language has no
             * variables of its own, so an event field is the only place to
             * keep a temporary: `offcpu_wait = exit_latency /
             * switches, offcpu_wait > 100' has to put the quotient
             * somewhere -- at the cost of whatever that field would have
             * reported, since an event source need not have a spare one.
             *
             * This requires the event to live in writable memory, which holds
             * for the .bss and map-value storage event sources use, and the
             * verifier enforces the bounds either way.
             *
             * Note the event may be storage the BPF program keeps across
             * events rather than a scratch copy, so writing a field the
             * program still needs can perturb later events too.
             */
            case SI: {
                int sz = bpf_size_of(&e, *pc++);

                if (e.depth <= 0) {
                    emit_fail(&e, "stack underflow");
                    break;
                }
                e.depth--;
                /* The slot holds a complete address, hence offset 0. The
                 * accumulator keeps the stored value, so an assignment still
                 * evaluates to it. */
                emit(&e, BPF_STX_MEM(sz, slot_reg(&e, e.depth), BPF_ACC, 0));
                break;
            }

            case NTHL:
                emit(&e, BPF_ENDIAN(BPF_TO_BE, BPF_ACC, 32));
                break;
            case NTHS:
                emit(&e, BPF_ENDIAN(BPF_TO_BE, BPF_ACC, 16));
                break;

            case PRTF:      emit_fail(&e, "printf() is not supported by the BPF backend"); break;
            case KSYM:      emit_fail(&e, "ksymbol() is not supported by the BPF backend"); break;
            case COMM:      emit_fail(&e, "comm_get() is not supported by the BPF backend"); break;
            case STRNCMP:   emit_fail(&e, "strncmp() is not supported by the BPF backend"); break;
            case MATCH:     emit_fail(&e, "~ is not supported by the BPF backend"); break;
            case STREQ:
            case STRNE:     emit_fail(&e, "string comparison is not supported by the BPF backend"); break;
            case SYSCALL:   emit_fail(&e, "syscall_name() is not supported by the BPF backend"); break;
            case KVMEXIT:   emit_fail(&e, "exit_reason_str() is not supported by the BPF backend"); break;
            case SYSTEM:    emit_fail(&e, "system() is not supported by the BPF backend"); break;
            case ADJ:       emit_fail(&e, "function calls are not supported by the BPF backend"); break;

            /*
             * Everything the VM can emit is handled above, so reaching here
             * means expr_compile() grew an opcode this backend has not been
             * taught. Not implemented, in the order they appear in the opcode
             * enum:
             *
             *   LEA, JSR, ENT, LEV  frame and call setup. expr_compile() never
             *                       emits these: it only ever assigns symbol
             *                       classes Glo and Sys, so there are no
             *                       locals to address and no user-defined
             *                       functions to call.
             *   JMP, BZ, BNZ        handled; listed only because they are the
             *                       ones whose targets need fixing up.
             *
             * and, handled above by failing with a specific message rather
             * than silently: ADJ (function calls), and every builtin that
             * returns or inspects a string -- PRTF, KSYM, COMM, STRNCMP,
             * MATCH, STREQ, STRNE, SYSCALL, KVMEXIT, SYSTEM. Those need
             * either userspace state (a symbol table, a pid->comm cache) or
             * unbounded loops, neither of which exists here. DIV and MOD are
             * handled, but fail on kernels below 6.6.
             */
            default:
                emit_fail(&e, "unsupported instruction");
                break;
        }
    }

    if (e.failed)
        goto out;

    e.pc_map[prog->nr_insn] = e.nr_insn;

    /* Resolve the recorded jumps. */
    for (i = 0; i < e.nr_fixup; i++) {
        struct bpf_insn *ins = &e.insn[e.fixup_at[i]];
        int target_pc = e.fixup_to[i];
        int target;

        if (target_pc < 0 || target_pc > prog->nr_insn ||
            e.pc_map[target_pc] < 0) {
            emit_fail(&e, "unresolved jump target");
            goto out;
        }
        target = e.pc_map[target_pc];
        /* BPF jump offsets are relative to the next instruction. */
        ins->off = target - e.fixup_at[i] - 1;
    }

    /* The placeholder is 2 instructions long, and libbpf relocates BTF
     * line_info using the original count while validating against the new
     * one, so never emit fewer than that. */
    while (e.nr_insn < 2)
        emit(&e, BPF_EXIT_INSN());

    free(e.pc_map);
    free(e.fixup_at);
    free(e.fixup_to);
    *nr_insn = e.nr_insn;
    return e.insn;

out:
    if (e.err)
        fprintf(stderr, "expr: cannot compile to BPF: %s\n", e.err);
    free(e.pc_map);
    free(e.fixup_at);
    free(e.fixup_to);
    free(e.insn);
    return NULL;
}

static void print_insn(void *private_data, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

/*
 * Disassemble the generated program in the syntax `bpftool prog dump xlated'
 * prints, e.g.
 *
 *      0: (79) r0 = *(u64 *)(r1 +8)
 *      1: (bf) r2 = r0
 *      2: (25) if r2 > 0x64 goto pc+2
 *
 * The formatting is the kernel's own print_bpf_insn() (lib/bpf_disasm.c, a
 * verbatim copy of kernel/bpf/disasm.c), which bpftool links against rather
 * than reimplements -- so this output and bpftool's cannot drift apart. This
 * function is bpftool's dump_xlated_plain() minus everything that only applies
 * to a loaded program: there is no BTF, no line info, and no kallsyms, since
 * the program has not been through the verifier yet and calls nothing.
 */
void expr_bpf_dump(struct bpf_insn *insn, int nr_insn)
{
    const struct bpf_insn_cbs cbs = {
        .cb_print = print_insn,
    };
    bool double_insn = false;
    int i;

    printf("BPF instruction:\n");
    for (i = 0; i < nr_insn; i++) {
        /* The second half of an ld_imm64 is not an instruction of its own; it
         * was consumed by the one before it. It still occupies an index, so
         * that the pc%+d in the jumps above stays meaningful. */
        if (double_insn) {
            double_insn = false;
            continue;
        }
        double_insn = insn[i].code == (BPF_LD | BPF_IMM | BPF_DW);

        printf("  %3d: ", i);
        /* print_bpf_insn() terminates the line itself. */
        print_bpf_insn(&cbs, insn + i, true);
    }
}

#endif /* CONFIG_LIBBPF */


/*
 * expr profiler
 *
**/
char *expression = NULL;
struct expression_info {
    char *expression;
    struct expr_prog *prog;
    struct tp_list *tp_list;
};

static int expr_argc_init(int argc, char *argv[])
{
    if (argc < 1) {
        fprintf(stderr, " {expression} needs to be specified.\n");
        help();
    } else {
        expression = strdup(argv[0]);
    }
    return 0;
}

static void expr_deinit(struct prof_dev *dev);
static int expr_init(struct prof_dev *dev)
{
    struct perf_evlist *evlist = dev->evlist;
    struct env *env = dev->env;
    struct expression_info *info;
    struct global_var_declare *declare;
    struct perf_event_attr attr = {
        .type          = PERF_TYPE_TRACEPOINT,
        .config        = 0,
        .size          = sizeof(struct perf_event_attr),
        .sample_period = 1,
        .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD |
                         PERF_SAMPLE_RAW,
        .read_format   = 0,
        .pinned        = 1,
        .disabled      = 1,
        .wakeup_events = 1,
    };
    struct perf_evsel *evsel;
    struct tp *tp;
    int i;

    if (!env->event)
        return -1;

    info = zalloc(sizeof(*info));
    if (!info)
        return -1;
    dev->private = info;
    info->expression = expression;
    expression = NULL;

    tep__ref();

    info->tp_list = tp_list_new(dev, env->event);
    if (!info->tp_list)
        goto failed;
    if (info->tp_list->nr_tp != 1) {
        fprintf(stderr, "Only a single event is allowed to be specified.\n");
        goto failed;
    }

    declare = tep__event_fields(info->tp_list->tp[0].id);
    if (!declare)
        goto failed;

    printf("expression: %s\n", info->expression);
    info->prog = expr_compile(info->expression, declare);
    free(declare);
    if (!info->prog)
        goto failed;
    info->prog->debug = env->verbose;
    expr_dump(info->prog);

    for_each_real_tp(info->tp_list, tp, i) {
        evsel = tp_evsel_new(tp, &attr);
        if (!evsel) {
            goto failed;
        }
        perf_evlist__add(evlist, evsel);
    }

    // Only test a small number of events.
    if (env->exit_n == 0) env->exit_n = 5;
    return 0;

failed:
    expr_deinit(dev);
    return -1;
}

static int expr_filter(struct prof_dev *dev)
{
    struct expression_info *info = dev->private;
    return tp_list_apply_filter(dev, info->tp_list);
}

static void expr_deinit(struct prof_dev *dev)
{
    struct expression_info *info = dev->private;
    if (info->expression)
        free(info->expression);
    expr_destroy(info->prog);
    tp_list_free(info->tp_list);
    tep__unref();
    free(info);
}

// in linux/perf_event.h
// PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD | PERF_SAMPLE_RAW
struct sample_type_header {
    struct {
        __u32    pid;
        __u32    tid;
    }    tid_entry;
    __u64   time;
    __u64   id;
    struct {
        __u32    cpu;
        __u32    reserved;
    }    cpu_entry;
    __u64       period;
    struct {
        __u32   size;
        __u8    data[0];
    } raw;
};

static long expr_ftrace_filter(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct expression_info *info = dev->private;
    struct sample_type_header *raw = (void *)event->sample.array;
    struct expr_global *glo = GLOBAL(raw->cpu_entry.cpu, raw->tid_entry.pid, raw->raw.data, raw->raw.size);
    return tp_list_ftrace_filter(dev, info->tp_list, glo);
}

static void expr_sample(struct prof_dev *dev, union perf_event *event, int instance)
{
    struct expression_info *info = dev->private;
    struct sample_type_header *raw = (void *)event->sample.array;
    long result;

    prof_dev_print_time(dev, raw->time, stdout);
    tp_print_marker(&info->tp_list->tp[0]);
    tp_print_event(&info->tp_list->tp[0], raw->time, raw->cpu_entry.cpu, raw->raw.data, raw->raw.size);

    info->prog->glo._cpu = raw->cpu_entry.cpu;
    info->prog->glo._pid = raw->tid_entry.pid;
    expr_load_data(info->prog, raw->raw.data, raw->raw.size);
    result = expr_run(info->prog);
    printf("result: 0x%lx\n", result);
}

static void expr_help(struct help_ctx *hctx)
{
    struct tp *tp = &hctx->tp_list[0]->tp[0];

    printf(PROGRAME " expr ");
    printf("-e \"");
    printf("%s:%s/%s/", tp->sys, tp->name, tp->filter&&tp->filter[0]?tp->filter:".");
    printf("\" {expression} ");

    common_help(hctx, true, true, true, false, false, true, true);
    common_help(hctx, false, true, true, false, false, true, true);
    printf("\n");
}

static const char *expr_desc[] = PROFILER_DESC("expr",
    "[OPTION...] {expression}",
    "Expression compiler and simulator.", "",
    "SYNOPSIS",
    "    Expressions are first compiled into assembly instructions, which can then be",
    "    simulated and executed multiple times. Expressions can use global variables,",
    "    which come from tracepoint fields.",
    "",
    "SYNTAX",
    "    Supported types: char, short, int, long, unsigned, and pointer types.",
    "    Most operators are supported. See Operators.",
    "    Supports multiple built-in functions. See Built-in Functions.",
    "    Global variables _cpu and _pid are available. See Global Variables.",
    "",
    "  Operators",
    "    Precedence  Operator    Description",
    "    1           ++ --       Suffix/postfix increment and decrement",
    "                ()          Function call",
    "                []          Array subscripting",
    "    2           ++ --       Prefix increment and decrement",
    "                + -         Unary plus and minus",
    "                ! ~         Logical NOT and bitwise NOT",
    "                (type)      Cast",
    "                *           Indirection (dereference)",
    "                &           Address-of",
    "                sizeof      Size-of",
    "    3           * / %       Multiplication, division, and remainder",
    "    4           + -         Addition and subtraction",
    "    5           << >>       Bitwise left shift and right shift",
    "    6           < <=        For relational operators < and <= respectively",
    "                > >=        For relational operators > and >= respectively",
    "                ~           Extended Operator: Wildcard pattern matching",
    "    7           == !=       For relational = and != respectively",
    "                            Supports both numeric and string comparison",
    "    8           &           Bitwise AND",
    "    9           ^           Bitwise XOR (exclusive or)",
    "    10          |           Bitwise OR (inclusive or)",
    "    11          &&          Logical AND",
    "    12          ||          Logical OR",
    "    13          ?:          Ternary conditional",
    "    14          =           Simple assignment",
    "    15          ,           Comma",
    "",
    "  Built-in Functions",
    "    int printf(char *fmt, args...)",
    "        Prints args according to fmt, and return the number of characters",
    "        printed, args can take up to 6 variable parameters.",
    "",
    "    char *ksymbol(long addr)",
    "        Get the kernel symbol name according to addr, and return a string.",
    "",
    "    int ntohl(int netlong)",
    "    short ntohs(short netshort)",
    "        These functions convert network byte order to host byte order.",
    "",
    "    int strncmp(const char *s1, const char *s2, long n)",
    "        Compare two strings.",
    "",
    "    char *comm_get(int pid)",
    "        Get the comm string of pid",
    "",
    "    char *syscall_name(int id)",
    "        Get the syscall name by id, and return a string.",
    "",
    "    char *exit_reason_str(int isa, int val)",
    "        Get the KVM exit reason string according to ISA and exit reason value.",
    "        ISA: 1=VMX, 2=SVM, 3=ARM. Returns a string describing the exit reason.",
    "",
    "    int system(const char *format, ...)",
    "        Format a command string using printf-style formatting and execute it via system().",
    "        Returns the command's exit status. Supports up to 6 additional parameters.",
    "        WARNING: Be cautious with untrusted input to avoid command injection.",
    "",
    "  Global Variables",
    "    int _cpu",
    "        The CPU number where the event occurred.",
    "",
    "    int _pid",
    "        The process ID (not thread ID) of the event.",
    "",
    "    Other variables come from the tracepoint event fields. Use 'help' to see",
    "    available fields for a specific event, e.g.:",
    "        "PROGRAME" trace -e sched:sched_wakeup help",
    "",
    "  Extended Operator (C++-style operator overloading)",
    "    ~ (const char *str, const char *pattern)",
    "        This operator overloads the bitwise NOT operator to provide trace event filter's ~ functionality.",
    "        Supports *, ?, and character classes [abc], [a-z], [^abc], [!abc].",
    "        Returns 1 if str matches pattern, 0 otherwise.",
    "",
    "    ==(const char *s1, const char *s2)",
    "    !=(const char *s1, const char *s2)",
    "        When both operands are string pointers (char *), == and != operators",
    "        perform string comparison instead of pointer comparison.",
    "        Returns 1 for true, 0 for false.",
    "        Examples:",
    "            comm == \"systemd\"          - Check if process name equals systemd",
    "            prev_comm == next_comm     - Compare two string fields",
    "",
    "EXAMPLES",
    "    "PROGRAME" expr -e sched:sched_wakeup help",
    "    "PROGRAME" expr -e sched:sched_wakeup '&pid'",
    "    "PROGRAME" expr -e 'kmem:mm_page_alloc/order>0/' '1<<order' -v",
    "    "PROGRAME" expr -e workqueue:workqueue_execute_start 'printf(\"%s \", ksymbol(function))' -v",
    "    "PROGRAME" expr -e sched:sched_process_exec 'printf(\"%s \", filename)'",
    "    "PROGRAME" expr -e sched:sched_wakeup 'comm ~ \"*sh\"' -v",
    "    "PROGRAME" expr -e sched:sched_switch 'prev_comm ~ \"systemd*\" || next_comm ~ \"systemd*\"'",
    "    "PROGRAME" expr -e sched:sched_process_exec 'filename != \"/bin/sh\"'"
);
static const char *expr_argv[] = PROFILER_ARGV("expr",
    "OPTION:",
    "cpus", "pids", "tids", "output", "mmap-pages", "exit-N",
    "version", "verbose", "quiet", "help",
    PROFILER_ARGV_PROFILER, "event"
);
static profiler _expr = {
    .name = "expr",
    .desc = expr_desc,
    .argv = expr_argv,
    .pages = 2,
    .help = expr_help,
    .argc_init = expr_argc_init,
    .init = expr_init,
    .filter = expr_filter,
    .deinit = expr_deinit,
    .ftrace_filter = expr_ftrace_filter,
    .sample = expr_sample,
};
PROFILER_REGISTER(_expr);
