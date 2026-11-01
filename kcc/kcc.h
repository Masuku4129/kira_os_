/* kcc: компилятор C для Киры. Общие объявления (этап M1: препроцессор, лексер, разбор, типы, IR).
 * Чистый C99, без внешних библиотек; из libc нужны только string.h, stdarg.h, setjmp.h, snprintf/vsnprintf.
 * Память: одна арена (bump) с лимитом, выделяется вызывающим; malloc не используется. */
#ifndef KCC_H
#define KCC_H
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>

typedef struct Type Type;
typedef struct Token Token;
typedef struct Node Node;
typedef struct Obj Obj;
typedef struct Member Member;
typedef struct Param Param;
typedef struct Reloc Reloc;
typedef struct HS HS;

/* ---------- util.c ---------- */
typedef struct {
    const char *(*read)(void *ud, const char *path, size_t *len); /* NULL, если файла нет */
    void *ud;
    const char **incdirs; int nincdirs;
    const char **defs; int ndefs;           /* "N" или "N=V" */
    const char *target;                      /* "x86_64" (по умолчанию) или "aarch64" */
    /* Необязательно: рост арены по требованию (на Кире - SYS_HEAP). Вызывается, когда в арене не хватает места:
       cap - текущая ёмкость, need - сколько байт нужно всего. Возвращает новую ёмкость (память продолжает арену
       без разрыва) или 0, если расти нельзя (лимит ядра). */
    size_t (*grow)(void *ud, size_t cap, size_t need);
} KCfg;

typedef struct { char *p; size_t len, cap; } SB;

typedef struct {
    char *abase; size_t acap, aused;
    jmp_buf jb;
    char err[600];
    const KCfg *cfg;
    SB warn;
    int counter;
    long tokbudget;                          /* защита от взрыва макрорасширения */
    int depth;                               /* вложенность разбора */
    const char *srcpath;                     /* главный исходный файл (для сообщений бэкенда) */
} KCtx;
extern KCtx K;

void *ka_new(size_t n);                      /* обнулённая память арены, выравнивание 8 */
char *ka_strdup(const char *s, size_t n);
void kerr(const char *file, int line, const char *fmt, ...) __attribute__((noreturn, format(printf, 3, 4)));
void kerr_tok(const Token *t, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void kwarn(const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void sb_add(SB *b, const char *s, size_t n);
void sb_printf(SB *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *kfmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---------- lex.c ---------- */
typedef enum { TK_EOF, TK_ID, TK_KW, TK_NUM, TK_STR, TK_CHR, TK_PUNCT } TKind;
struct Token {
    TKind k; Token *next;
    const char *s; int len;                  /* исходное написание */
    int line; const char *file;
    unsigned bol : 1, sp : 1, hdr : 1, mark : 1;
    HS *hs;
    int64_t ival; Type *ty;                  /* TK_NUM, TK_CHR после convert_tokens */
    const char *str; int slen;               /* TK_STR после convert_tokens: байты без завершающего 0 */
};
Token *lex_buf(const char *file, const char *src, size_t len);
void convert_tokens(Token *t);
void parse_intlit(const Token *t, int64_t *v, Type **ty);
int tok_is(const Token *t, const char *s);
void conv_char(Token *t);
int decode_escape(const Token *t, const char **pp);   /* для строк/символов; *pp за '\\' */

/* ---------- pp.c ---------- */
Token *preprocess(const char *path);
Token *pp_lex_one(Token *tmpl, const char *text);

/* ---------- type.c ---------- */
typedef enum { TY_VOID, TY_BOOL, TY_CHAR, TY_SHORT, TY_INT, TY_LONG, TY_LLONG, TY_ENUM,
               TY_PTR, TY_ARRAY, TY_FUNC, TY_STRUCT, TY_UNION } TyKind;
struct Param { Param *next; Type *ty; const char *name; };
struct Member { Member *next; const char *name; Type *ty; int off, bw, bo, idx; const Token *tok; };
struct Type {
    TyKind k; int size, align;
    unsigned is_unsigned : 1, is_const : 1, is_volatile : 1, complete : 1, variadic : 1, noproto : 1, packed : 1, noret : 1;
    Type *base; int64_t alen;                /* PTR/ARRAY/FUNC(возврат); alen<0 - неполный массив */
    Member *mem; int nmem;                   /* STRUCT/UNION */
    Param *params;                           /* FUNC */
    const char *tag; int id;
    Type *orig;                              /* исходный тип без квалификаторов */
};
extern Type ty_void, ty_bool, ty_char, ty_uchar, ty_short, ty_ushort, ty_int, ty_uint, ty_long, ty_ulong, ty_llong, ty_ullong;
Type *ty_new(TyKind k, int size, int align);
Type *ty_ptr(Type *base);
Type *ty_array(Type *base, int64_t len);
Type *ty_func(Type *ret, Param *params, int variadic, int noproto);
Type *ty_qual(Type *t, int is_const, int is_volatile);
Type *ty_unqual(Type *t);
int ty_is_int(const Type *t);                /* целые, bool, enum */
int ty_is_scalar(const Type *t);
int ty_is_agg(const Type *t);
int ty_compat(const Type *a, const Type *b);
Type *ty_promote(Type *t);
Type *ty_common(Type *a, Type *b);
const char *ty_name(const Type *t);
int ty_bits(const Type *t);

/* ---------- parse.c ---------- */
typedef enum {
    ND_NUM, ND_STR, ND_VAR, ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD, ND_AND, ND_OR, ND_XOR, ND_SHL, ND_SHR,
    ND_EQ, ND_NE, ND_LT, ND_LE, ND_LOGAND, ND_LOGOR, ND_NOT, ND_BITNOT, ND_NEG,
    ND_ASSIGN, ND_OPASSIGN, ND_COMMA, ND_COND, ND_ADDR, ND_DEREF, ND_MEMBER, ND_CAST, ND_CALL, ND_INCDEC,
    ND_STMTEXPR, ND_ZERO, ND_ASM, ND_UNREACH, ND_VASTART, ND_VAARG, ND_ATOMIC,
    ND_BLOCK, ND_IF, ND_FOR, ND_DO, ND_SWITCH, ND_CASE, ND_BREAK, ND_CONTINUE, ND_GOTO, ND_LABEL, ND_RETURN, ND_EXPR
} NKind;
struct Node {
    NKind k; Type *ty; const Token *tok;
    Node *lhs, *rhs, *c, *t, *e, *ini, *inc, *body, *next, *args, *cases, *dflt;
    Obj *var; Member *mem; Type *oty; Node *gnext;
    int64_t val, val2;                       /* NUM; CASE (val2 - конец диапазона при op==2); INCDEC: +1/-1; ATOMIC: форма результата */
    int op;                                  /* OPASSIGN: вид операции; INCDEC: 1=постфиксная */
    int lbl, lbl2;                           /* метки: BREAK/CONTINUE/CASE/LABEL/FOR/SWITCH */
    const char *name;                        /* GOTO/LABEL/ASM */
    Node *ao, *ai;                           /* ASM: выходы, входы (списки по next) */
    int nao, nai;
};
struct Reloc { Reloc *next; int off; const char *sym; int64_t addend; };
struct Obj {
    Obj *next; const char *name; Type *ty;
    unsigned is_local : 1, is_func : 1, is_static : 1, is_extern : 1, is_def : 1, is_inline : 1,
             is_str : 1, is_param : 1, is_ro : 1, used : 1, tentative : 1, noret : 1, emit : 1;
    void *ir;                                /* бэкенд: IrFn функции */
    uint64_t addr;                           /* бэкенд: функция - смещение в .text, данные - адрес в образе */
    int slot, align, line, nparams;
    Obj *params, *locals;                    /* функция: параметры (по next), все локальные */
    Node *body; int nlabels;
    unsigned char *init; Reloc *relocs;      /* данные глобала (size байт) или NULL = нули */
};
typedef struct { Obj *globals; Obj *funcs_tail; } Prog;
Obj *parse_program(Token *tok);
int64_t eval_const(Node *n);

/* ---------- ir.c ---------- */
typedef enum { IR_LABEL, IR_CONST, IR_GADDR, IR_LADDR, IR_PARAM, IR_LOAD, IR_STORE, IR_MOV, IR_BIN, IR_NEG, IR_NOT,
               IR_CMP, IR_EXT, IR_JMP, IR_BRZ, IR_BRNZ, IR_CALL, IR_RET, IR_COPY, IR_ZERO, IR_ASM, IR_UNREACH,
               IR_ATOMIC, IR_VAINFO } IrOp;
/* IR_ATOMIC.sub: операции над целым/указателем шириной w (1,2,4,8) с порядком seq_cst; a - адрес, b - значение (для CAS - ожидаемое),
   c - новое значение CAS; dst - прежнее значение в каноническом виде (LOAD - загруженное; STORE, FENCE - без dst) */
enum { AT_LOAD = 1, AT_STORE, AT_XCHG, AT_CAS, AT_ADD, AT_SUB, AT_AND, AT_OR, AT_XOR, AT_NAND, AT_FENCE };
/* IR_VAINFO.sub: 0 - dst = адрес области сохранения регистровых аргументов (целочисленные подряд), 1 - адрес первого анонимного
   аргумента в стеке, 2 - число целочисленных регистров, занятых именованными параметрами */
typedef struct Ins Ins;
struct Ins {
    IrOp op; int sub, dst, a, b, c, w, sgn, vol, nargs, nout;
    int rsz, rt;                             /* CALL: размер возвращаемого агрегата (0 - нет) и vreg с адресом буфера результата; RET: sub=1 - агрегат размера w */
    int *asz;                                /* CALL: по аргументу размер агрегата (0 - скаляр; тогда args[k] - адрес агрегата) */
    int64_t imm; const char *sym; int *args; Ins *next;
    int line; const char *file;              /* только IR_ASM: место в исходнике */
};
typedef struct { Obj *f; Ins *head; int nreg, nlab; } IrFn;
IrFn *ir_lower(Obj *f);                      /* линейный IR одной функции с телом */
void ir_dump(Obj *globals, SB *out);

/* ---------- gen_x86.c / asm_x86.c / elf.c (этап M2) ---------- */
/* Компиляция программы в исполняемый ELF Киры (x86-64). Результат - в арене. Ошибки - через kerr. */
void x86_build_elf(Obj *globals, unsigned char **out, size_t *outlen);

/* ---------- API ---------- */
typedef struct { const char *ir; const char *err; const char *warn; const unsigned char *elf; size_t elflen; } KResult;
/* Компиляция одного файла. mem/memsz - арена. emit_ir=0: только проверка, 1: текстовый IR, 2: ELF x86-64. Возврат 0 - успех. */
int kcc_compile(const KCfg *cfg, void *mem, size_t memsz, const char *path, int emit_ir, KResult *res);

#endif
