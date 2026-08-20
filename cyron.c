/* ============================================================================
   Cyron  —  a tiny interpreted language: Python's heart, C's curly braces.

     - Curly braces for blocks, NO semicolons (newline ends a statement)
     - Indentation never matters
     - No manual memory management, no leaks: every allocation the
       interpreter makes is tracked and released before exit
     - Variables (let), functions (fn), classes with inheritance (class A : B),
       lists, strings, if/elif/else, while, for-in, closures, and ~24 builtins

   Build:   gcc cyron.c -o cyron.exe -O2 -lm
   Run:     cyron program.cy        (or run with no args for a REPL)

   File extension: .cy
   ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <setjmp.h>
#include <time.h>

#define CYRON_VERSION "1.0"
#define MAX_CALL_DEPTH  1800

/* ============================ tracked memory ================================
   Every heap block goes through xmalloc and is chained into a global list.
   free_all_memory() walks the list at exit, so nothing is ever leaked.       */

typedef struct AllocHdr { struct AllocHdr *next; } AllocHdr;
static AllocHdr *g_allocs = NULL;

static void *xmalloc(size_t n) {
    AllocHdr *h = (AllocHdr *)malloc(sizeof(AllocHdr) + n);
    if (!h) { fprintf(stderr, "cyron: out of memory\n"); exit(1); }
    h->next = g_allocs;
    g_allocs = h;
    return (void *)(h + 1);
}
static void *xgrow(void *p, size_t oldsz, size_t newsz) {
    void *q = xmalloc(newsz);
    if (p && oldsz) memcpy(q, p, oldsz);
    return q;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)xmalloc(n);
    memcpy(d, s, n);
    return d;
}
static void free_all_memory(void) {
    AllocHdr *h = g_allocs;
    while (h) { AllocHdr *n = h->next; free(h); h = n; }
    g_allocs = NULL;
}

/* ============================ error handling ================================ */

static jmp_buf g_recover;
static int     g_can_recover = 0;   /* 1 while the REPL is driving */

static void verror(const char *kind, int line, const char *fmt, va_list ap) {
    fprintf(stderr, "%s [line %d]: ", kind, line);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}
static void syntax_error(int line, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    verror("Syntax error", line, fmt, ap);
    va_end(ap);
    if (g_can_recover) longjmp(g_recover, 1);
    exit(65);
}
static void runtime_error(int line, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    verror("Runtime error", line, fmt, ap);
    va_end(ap);
    if (g_can_recover) longjmp(g_recover, 1);
    exit(70);
}

/* ================================ lexer ===================================== */

typedef enum {
    T_EOF, T_NL, T_ID, T_NUM, T_STR,
    T_LET, T_FN, T_CLASS, T_IF, T_ELIF, T_ELSE, T_WHILE, T_FOR, T_IN,
    T_RETURN, T_BREAK, T_CONTINUE, T_TRUE, T_FALSE, T_NIL,
    T_AND, T_OR, T_NOT, T_THIS, T_SUPER,
    T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK,
    T_COMMA, T_DOT, T_COLON,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PCT,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ,
    T_EQ, T_EQEQ, T_NEQ, T_LT, T_LE, T_GT, T_GE
} TokType;

typedef struct { TokType type; char *text; double num; int line; } Tok;
typedef struct { Tok *toks; int n, cap; } TokList;

static struct { const char *kw; TokType t; } g_keywords[] = {
    {"let", T_LET}, {"fn", T_FN}, {"class", T_CLASS}, {"if", T_IF},
    {"elif", T_ELIF}, {"else", T_ELSE}, {"while", T_WHILE}, {"for", T_FOR},
    {"in", T_IN}, {"return", T_RETURN}, {"break", T_BREAK},
    {"continue", T_CONTINUE}, {"true", T_TRUE}, {"false", T_FALSE},
    {"nil", T_NIL}, {"and", T_AND}, {"or", T_OR}, {"not", T_NOT},
    {"this", T_THIS}, {"super", T_SUPER}, {NULL, T_EOF}
};

static void addtok(TokList *tl, TokType t, char *text, double num, int line) {
    if (tl->n >= tl->cap) {
        int nc = tl->cap ? tl->cap * 2 : 64;
        tl->toks = (Tok *)xgrow(tl->toks, sizeof(Tok) * tl->cap, sizeof(Tok) * nc);
        tl->cap = nc;
    }
    tl->toks[tl->n].type = t;
    tl->toks[tl->n].text = text;
    tl->toks[tl->n].num  = num;
    tl->toks[tl->n].line = line;
    tl->n++;
}

static void lex(const char *src, TokList *tl) {
    int line = 1;
    int depth = 0;                     /* inside ( ) or [ ] newlines are ignored */
    const char *p = src;

    /* skip UTF-8 BOM if present */
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) p += 3;

    while (*p) {
        char c = *p;

        if (c == ' ' || c == '\t' || c == '\r') { p++; continue; }

        if (c == '\\' && p[1] == '\n') { line++; p += 2; continue; } /* line continuation */

        if (c == '\n') {
            line++; p++;
            if (depth == 0 && tl->n > 0 && tl->toks[tl->n - 1].type != T_NL)
                addtok(tl, T_NL, NULL, 0, line - 1);
            continue;
        }
        if (c == '#') { while (*p && *p != '\n') p++; continue; }
        if (c == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }

        if (c == ';')
            syntax_error(line, "Cyron has no semicolons -- just end the line");

        /* numbers */
        if (isdigit((unsigned char)c) ||
            (c == '.' && isdigit((unsigned char)p[1]))) {
            char *end;
            double d = strtod(p, &end);
            addtok(tl, T_NUM, NULL, d, line);
            p = end;
            continue;
        }

        /* identifiers / keywords */
        if (isalpha((unsigned char)c) || c == '_') {
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            int len = (int)(p - start);
            char *name = (char *)xmalloc(len + 1);
            memcpy(name, start, len); name[len] = 0;
            TokType t = T_ID;
            for (int i = 0; g_keywords[i].kw; i++)
                if (strcmp(name, g_keywords[i].kw) == 0) { t = g_keywords[i].t; break; }
            addtok(tl, t, name, 0, line);
            continue;
        }

        /* strings, "double" or 'single' quoted */
        if (c == '"' || c == '\'') {
            char quote = c;
            p++;
            char *buf = (char *)xmalloc(strlen(p) + 1);
            int bi = 0;
            int startline = line;
            while (*p && *p != quote) {
                if (*p == '\n') syntax_error(startline, "unterminated string");
                if (*p == '\\') {
                    p++;
                    switch (*p) {
                        case 'n':  buf[bi++] = '\n'; break;
                        case 't':  buf[bi++] = '\t'; break;
                        case 'r':  buf[bi++] = '\r'; break;
                        case '\\': buf[bi++] = '\\'; break;
                        case '"':  buf[bi++] = '"';  break;
                        case '\'': buf[bi++] = '\''; break;
                        default:
                            syntax_error(line, "unknown escape sequence '\\%c'", *p);
                    }
                    p++;
                } else {
                    buf[bi++] = *p++;
                }
            }
            if (!*p) syntax_error(startline, "unterminated string");
            p++; /* closing quote */
            buf[bi] = 0;
            addtok(tl, T_STR, buf, 0, line);
            continue;
        }

        /* two-character operators */
        if (c == '=' && p[1] == '=') { addtok(tl, T_EQEQ,   NULL, 0, line); p += 2; continue; }
        if (c == '!' && p[1] == '=') { addtok(tl, T_NEQ,    NULL, 0, line); p += 2; continue; }
        if (c == '<' && p[1] == '=') { addtok(tl, T_LE,     NULL, 0, line); p += 2; continue; }
        if (c == '>' && p[1] == '=') { addtok(tl, T_GE,     NULL, 0, line); p += 2; continue; }
        if (c == '&' && p[1] == '&') { addtok(tl, T_AND,    NULL, 0, line); p += 2; continue; }
        if (c == '|' && p[1] == '|') { addtok(tl, T_OR,     NULL, 0, line); p += 2; continue; }
        if (c == '+' && p[1] == '=') { addtok(tl, T_PLUSEQ, NULL, 0, line); p += 2; continue; }
        if (c == '-' && p[1] == '=') { addtok(tl, T_MINUSEQ,NULL, 0, line); p += 2; continue; }
        if (c == '*' && p[1] == '=') { addtok(tl, T_STAREQ, NULL, 0, line); p += 2; continue; }
        if (c == '/' && p[1] == '=') { addtok(tl, T_SLASHEQ,NULL, 0, line); p += 2; continue; }

        switch (c) {
            case '{': addtok(tl, T_LBRACE, NULL, 0, line); break;
            case '}': addtok(tl, T_RBRACE, NULL, 0, line); break;
            case '(': addtok(tl, T_LPAREN, NULL, 0, line); depth++; break;
            case ')': addtok(tl, T_RPAREN, NULL, 0, line); if (depth) depth--; break;
            case '[': addtok(tl, T_LBRACK, NULL, 0, line); depth++; break;
            case ']': addtok(tl, T_RBRACK, NULL, 0, line); if (depth) depth--; break;
            case ',': addtok(tl, T_COMMA,  NULL, 0, line); break;
            case '.': addtok(tl, T_DOT,    NULL, 0, line); break;
            case ':': addtok(tl, T_COLON,  NULL, 0, line); break;
            case '+': addtok(tl, T_PLUS,   NULL, 0, line); break;
            case '-': addtok(tl, T_MINUS,  NULL, 0, line); break;
            case '*': addtok(tl, T_STAR,   NULL, 0, line); break;
            case '/': addtok(tl, T_SLASH,  NULL, 0, line); break;
            case '%': addtok(tl, T_PCT,    NULL, 0, line); break;
            case '=': addtok(tl, T_EQ,     NULL, 0, line); break;
            case '<': addtok(tl, T_LT,     NULL, 0, line); break;
            case '>': addtok(tl, T_GT,     NULL, 0, line); break;
            case '!': addtok(tl, T_NOT,    NULL, 0, line); break;
            default:
                syntax_error(line, "unexpected character '%c'", c);
        }
        p++;
    }
    if (tl->n > 0 && tl->toks[tl->n - 1].type != T_NL)
        addtok(tl, T_NL, NULL, 0, line);
    addtok(tl, T_EOF, NULL, 0, line);
}

/* ================================= AST ====================================== */

enum {
    N_NUM, N_STR, N_BOOL, N_NIL, N_VAR, N_LISTLIT,
    N_ASSIGN, N_BIN, N_LOGIC, N_UNARY, N_CALL, N_GET, N_SET,
    N_INDEX, N_INDEXSET, N_THIS, N_SUPER,
    N_EXPRSTMT, N_LET, N_BLOCK, N_IF, N_WHILE, N_FOR,
    N_FN, N_RETURN, N_BREAK, N_CONTINUE, N_CLASS
};

typedef struct Node Node;
struct Node {
    int    kind;
    int    line;
    int    op;         /* operator token for N_BIN / N_LOGIC / N_UNARY   */
    double num;        /* N_NUM, N_BOOL                                  */
    char  *str;        /* names, string literals                         */
    Node  *a, *b, *c;  /* children (meaning depends on kind)             */
    Node **kids;       /* stmt lists, args, params, elements, methods    */
    int    nkids;
};

static Node *new_node(int kind, int line) {
    Node *n = (Node *)xmalloc(sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->kind = kind;
    n->line = line;
    return n;
}
static void node_add_kid(Node *n, Node *kid) {
    n->kids = (Node **)xgrow(n->kids, sizeof(Node *) * n->nkids,
                             sizeof(Node *) * (n->nkids + 1));
    n->kids[n->nkids++] = kid;
}

/* ================================ parser ==================================== */

static Tok *g_toks;
static int  g_pos;

static Tok    *peek(void)         { return &g_toks[g_pos]; }
static TokType peekt(void)        { return g_toks[g_pos].type; }
static Tok    *advance(void)      { return &g_toks[g_pos++]; }
static int     check(TokType t)   { return peekt() == t; }
static int     match(TokType t)   { if (check(t)) { g_pos++; return 1; } return 0; }

static Tok *expect(TokType t, const char *what) {
    if (!check(t)) syntax_error(peek()->line, "expected %s", what);
    return advance();
}
static void skip_newlines(void) { while (check(T_NL)) g_pos++; }

static Node *parse_expr(void);
static Node *parse_stmt(void);
static Node *parse_block(void);

static void end_of_stmt(void) {
    if (match(T_NL)) return;
    if (check(T_EOF) || check(T_RBRACE)) return;
    syntax_error(peek()->line, "expected end of line (one statement per line)");
}

/* ---- expressions ---- */

static Node *parse_primary(void) {
    Tok *t = peek();
    if (match(T_NUM))  { Node *n = new_node(N_NUM, t->line); n->num = t->num; return n; }
    if (match(T_STR))  { Node *n = new_node(N_STR, t->line); n->str = t->text; return n; }
    if (match(T_TRUE)) { Node *n = new_node(N_BOOL, t->line); n->num = 1; return n; }
    if (match(T_FALSE)){ Node *n = new_node(N_BOOL, t->line); n->num = 0; return n; }
    if (match(T_NIL))  { return new_node(N_NIL, t->line); }
    if (match(T_THIS)) { return new_node(N_THIS, t->line); }
    if (match(T_SUPER)) {
        expect(T_DOT, "'.' after 'super'");
        Tok *m = expect(T_ID, "method name after 'super.'");
        Node *n = new_node(N_SUPER, t->line);
        n->str = m->text;
        return n;
    }
    if (match(T_ID)) {
        Node *n = new_node(N_VAR, t->line);
        n->str = t->text;
        return n;
    }
    if (match(T_LPAREN)) {
        Node *e = parse_expr();
        expect(T_RPAREN, "')'");
        return e;
    }
    if (match(T_LBRACK)) {
        Node *n = new_node(N_LISTLIT, t->line);
        if (!check(T_RBRACK)) {
            do {
                node_add_kid(n, parse_expr());
            } while (match(T_COMMA) && !check(T_RBRACK));
        }
        expect(T_RBRACK, "']'");
        return n;
    }
    syntax_error(t->line, "unexpected token here");
    return NULL; /* unreachable */
}

static Node *parse_postfix(void) {
    Node *e = parse_primary();
    for (;;) {
        if (match(T_LPAREN)) {
            Node *call = new_node(N_CALL, e->line);
            call->a = e;
            if (!check(T_RPAREN)) {
                do {
                    node_add_kid(call, parse_expr());
                } while (match(T_COMMA) && !check(T_RPAREN));
            }
            expect(T_RPAREN, "')' after arguments");
            e = call;
        } else if (match(T_LBRACK)) {
            Node *ix = new_node(N_INDEX, e->line);
            ix->a = e;
            ix->b = parse_expr();
            expect(T_RBRACK, "']'");
            e = ix;
        } else if (match(T_DOT)) {
            Tok *name = expect(T_ID, "property name after '.'");
            Node *g = new_node(N_GET, name->line);
            g->a = e;
            g->str = name->text;
            e = g;
        } else break;
    }
    return e;
}

static Node *parse_unary(void) {
    Tok *t = peek();
    if (match(T_NOT) || match(T_MINUS)) {
        Node *n = new_node(N_UNARY, t->line);
        n->op = t->type;
        n->a = parse_unary();
        return n;
    }
    return parse_postfix();
}

static Node *parse_factor(void) {
    Node *e = parse_unary();
    while (check(T_STAR) || check(T_SLASH) || check(T_PCT)) {
        Tok *op = advance();
        Node *n = new_node(N_BIN, op->line);
        n->op = op->type; n->a = e; n->b = parse_unary();
        e = n;
    }
    return e;
}
static Node *parse_term(void) {
    Node *e = parse_factor();
    while (check(T_PLUS) || check(T_MINUS)) {
        Tok *op = advance();
        Node *n = new_node(N_BIN, op->line);
        n->op = op->type; n->a = e; n->b = parse_factor();
        e = n;
    }
    return e;
}
static Node *parse_comparison(void) {
    Node *e = parse_term();
    while (check(T_LT) || check(T_LE) || check(T_GT) || check(T_GE)) {
        Tok *op = advance();
        Node *n = new_node(N_BIN, op->line);
        n->op = op->type; n->a = e; n->b = parse_term();
        e = n;
    }
    return e;
}
static Node *parse_equality(void) {
    Node *e = parse_comparison();
    while (check(T_EQEQ) || check(T_NEQ)) {
        Tok *op = advance();
        Node *n = new_node(N_BIN, op->line);
        n->op = op->type; n->a = e; n->b = parse_comparison();
        e = n;
    }
    return e;
}
static Node *parse_and(void) {
    Node *e = parse_equality();
    while (check(T_AND)) {
        Tok *op = advance();
        Node *n = new_node(N_LOGIC, op->line);
        n->op = T_AND; n->a = e; n->b = parse_equality();
        e = n;
    }
    return e;
}
static Node *parse_or(void) {
    Node *e = parse_and();
    while (check(T_OR)) {
        Tok *op = advance();
        Node *n = new_node(N_LOGIC, op->line);
        n->op = T_OR; n->a = e; n->b = parse_and();
        e = n;
    }
    return e;
}

static Node *parse_assignment(void) {
    Node *lhs = parse_or();
    TokType t = peekt();
    if (t == T_EQ || t == T_PLUSEQ || t == T_MINUSEQ ||
        t == T_STAREQ || t == T_SLASHEQ) {
        Tok *op = advance();
        Node *rhs = parse_assignment();

        if (t != T_EQ) {  /* desugar  x += e  into  x = x + e  */
            Node *bin = new_node(N_BIN, op->line);
            bin->op = (t == T_PLUSEQ)  ? T_PLUS  :
                      (t == T_MINUSEQ) ? T_MINUS :
                      (t == T_STAREQ)  ? T_STAR  : T_SLASH;
            bin->a = lhs;
            bin->b = rhs;
            rhs = bin;
        }

        if (lhs->kind == N_VAR) {
            Node *n = new_node(N_ASSIGN, op->line);
            n->str = lhs->str;
            n->a = rhs;
            return n;
        }
        if (lhs->kind == N_GET) {
            Node *n = new_node(N_SET, op->line);
            n->a = lhs->a;      /* object   */
            n->str = lhs->str;  /* property */
            n->b = rhs;
            return n;
        }
        if (lhs->kind == N_INDEX) {
            Node *n = new_node(N_INDEXSET, op->line);
            n->a = lhs->a;      /* container */
            n->b = lhs->b;      /* index     */
            n->c = rhs;
            return n;
        }
        syntax_error(op->line, "invalid assignment target");
    }
    return lhs;
}

static Node *parse_expr(void) { return parse_assignment(); }

/* ---- statements ---- */

static Node *parse_block(void) {
    Tok *lb = expect(T_LBRACE, "'{'");
    Node *blk = new_node(N_BLOCK, lb->line);
    for (;;) {
        skip_newlines();
        if (check(T_RBRACE) || check(T_EOF)) break;
        node_add_kid(blk, parse_stmt());
    }
    expect(T_RBRACE, "'}'");
    return blk;
}

static Node *parse_if(int line) {
    Node *n = new_node(N_IF, line);
    n->a = parse_expr();
    n->b = parse_block();
    int save = g_pos;
    skip_newlines();
    if (check(T_ELIF)) {
        Tok *t = advance();
        n->c = parse_if(t->line);
    } else if (check(T_ELSE)) {
        Tok *t = advance();
        if (check(T_IF)) { advance(); n->c = parse_if(t->line); }  /* else if */
        else n->c = parse_block();
    } else {
        g_pos = save;
    }
    return n;
}

static Node *parse_fn_rest(int line) {  /* after the 'fn' keyword */
    Tok *name = expect(T_ID, "function name after 'fn'");
    Node *n = new_node(N_FN, line);
    n->str = name->text;
    expect(T_LPAREN, "'(' after function name");
    if (!check(T_RPAREN)) {
        do {
            Tok *p = expect(T_ID, "parameter name");
            Node *param = new_node(N_VAR, p->line);
            param->str = p->text;
            node_add_kid(n, param);
        } while (match(T_COMMA));
    }
    expect(T_RPAREN, "')' after parameters");
    n->b = parse_block();
    return n;
}

static Node *parse_stmt(void) {
    Tok *t = peek();

    if (match(T_LET)) {
        Tok *name = expect(T_ID, "variable name after 'let'");
        Node *n = new_node(N_LET, t->line);
        n->str = name->text;
        if (match(T_EQ)) n->a = parse_expr();
        end_of_stmt();
        return n;
    }
    if (match(T_FN)) {
        return parse_fn_rest(t->line);
    }
    if (match(T_CLASS)) {
        Tok *name = expect(T_ID, "class name after 'class'");
        Node *n = new_node(N_CLASS, t->line);
        n->str = name->text;
        if (match(T_COLON)) {
            Tok *sup = expect(T_ID, "superclass name after ':'");
            Node *sv = new_node(N_VAR, sup->line);
            sv->str = sup->text;
            n->a = sv;
        }
        expect(T_LBRACE, "'{' to open class body");
        for (;;) {
            skip_newlines();
            if (check(T_RBRACE) || check(T_EOF)) break;
            Tok *m = expect(T_FN, "'fn' (classes contain only methods)");
            node_add_kid(n, parse_fn_rest(m->line));
        }
        expect(T_RBRACE, "'}' to close class body");
        return n;
    }
    if (match(T_IF))    return parse_if(t->line);
    if (match(T_WHILE)) {
        Node *n = new_node(N_WHILE, t->line);
        n->a = parse_expr();
        n->b = parse_block();
        return n;
    }
    if (match(T_FOR)) {
        Tok *var = expect(T_ID, "loop variable after 'for'");
        expect(T_IN, "'in' after loop variable");
        Node *n = new_node(N_FOR, t->line);
        n->str = var->text;
        n->a = parse_expr();
        n->b = parse_block();
        return n;
    }
    if (match(T_RETURN)) {
        Node *n = new_node(N_RETURN, t->line);
        if (!check(T_NL) && !check(T_RBRACE) && !check(T_EOF))
            n->a = parse_expr();
        end_of_stmt();
        return n;
    }
    if (match(T_BREAK))    { Node *n = new_node(N_BREAK, t->line);    end_of_stmt(); return n; }
    if (match(T_CONTINUE)) { Node *n = new_node(N_CONTINUE, t->line); end_of_stmt(); return n; }
    if (check(T_LBRACE))   return parse_block();

    Node *n = new_node(N_EXPRSTMT, t->line);
    n->a = parse_expr();
    end_of_stmt();
    return n;
}

static Node *parse_program(TokList *tl) {
    g_toks = tl->toks;
    g_pos = 0;
    Node *prog = new_node(N_BLOCK, 1);
    for (;;) {
        skip_newlines();
        if (check(T_EOF)) break;
        node_add_kid(prog, parse_stmt());
    }
    return prog;
}

/* ============================== runtime values ============================== */

typedef struct Env    Env;
typedef struct VList  VList;
typedef struct Fn     Fn;
typedef struct Class  Class;
typedef struct Inst   Inst;
typedef struct Bound  Bound;
typedef struct Native Native;

typedef enum {
    V_NIL, V_BOOL, V_NUM, V_STR, V_LIST,
    V_FN, V_NATIVE, V_CLASS, V_INST, V_BOUND
} VType;

typedef struct Value {
    VType t;
    union {
        int     b;
        double  n;
        char   *s;
        VList  *list;
        Fn     *fn;
        Native *nat;
        Class  *cls;
        Inst   *inst;
        Bound  *bound;
    } v;
} Value;

struct VList { Value *items; int n, cap; };
struct Env   { Env *parent; char **names; Value *vals; int n, cap; };

struct Fn {
    char  *name;
    char **params;
    int    nparams;
    Node  *body;
    Env   *closure;
    int    is_init;
};

typedef Value (*NativeC)(int argc, Value *args, int line);
struct Native { const char *name; NativeC fn; int min_args, max_args; };

typedef struct { char **names; Fn  **fns;  int n, cap; } MethodTable;
typedef struct { char **names; Value *vals; int n, cap; } FieldTable;

struct Class { char *name; Class *super; MethodTable methods; };
struct Inst  { Class *cls; FieldTable fields; };
struct Bound { Value recv; Fn *fn; };

static Value NILV(void)      { Value v; v.t = V_NIL;  v.v.n = 0; return v; }
static Value BOOLV(int b)    { Value v; v.t = V_BOOL; v.v.b = b != 0; return v; }
static Value NUMV(double n)  { Value v; v.t = V_NUM;  v.v.n = n; return v; }
static Value STRV(char *s)   { Value v; v.t = V_STR;  v.v.s = s; return v; }
static Value LISTV(VList *l) { Value v; v.t = V_LIST; v.v.list = l; return v; }

static VList *new_vlist(void) {
    VList *l = (VList *)xmalloc(sizeof(VList));
    l->items = NULL; l->n = 0; l->cap = 0;
    return l;
}
static void vlist_push(VList *l, Value v) {
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        l->items = (Value *)xgrow(l->items, sizeof(Value) * l->cap, sizeof(Value) * nc);
        l->cap = nc;
    }
    l->items[l->n++] = v;
}

/* ---- environments ---- */

static Env *env_new(Env *parent) {
    Env *e = (Env *)xmalloc(sizeof(Env));
    e->parent = parent; e->names = NULL; e->vals = NULL; e->n = 0; e->cap = 0;
    return e;
}
static void env_define(Env *e, const char *name, Value v) {
    for (int i = 0; i < e->n; i++)
        if (strcmp(e->names[i], name) == 0) { e->vals[i] = v; return; }
    if (e->n >= e->cap) {
        int nc = e->cap ? e->cap * 2 : 8;
        e->names = (char **)xgrow(e->names, sizeof(char *) * e->cap, sizeof(char *) * nc);
        e->vals  = (Value *)xgrow(e->vals,  sizeof(Value)  * e->cap, sizeof(Value)  * nc);
        e->cap = nc;
    }
    e->names[e->n] = xstrdup(name);
    e->vals[e->n]  = v;
    e->n++;
}
static Value *env_find(Env *e, const char *name) {
    for (Env *s = e; s; s = s->parent)
        for (int i = 0; i < s->n; i++)
            if (strcmp(s->names[i], name) == 0) return &s->vals[i];
    return NULL;
}
static Value env_get(Env *e, const char *name, int line) {
    Value *slot = env_find(e, name);
    if (!slot) runtime_error(line, "undefined variable '%s'", name);
    return *slot;
}
static void env_assign(Env *e, const char *name, Value v, int line) {
    Value *slot = env_find(e, name);
    if (!slot)
        runtime_error(line, "undefined variable '%s' (declare it first with 'let %s = ...')",
                      name, name);
    *slot = v;
}

/* ---- stringification ---- */

static char *num_to_str(double d) {
    char buf[64];
    if (d == floor(d) && fabs(d) < 1e15)   /* integral */
        snprintf(buf, sizeof buf, "%.0f", d);
    else
        snprintf(buf, sizeof buf, "%.10g", d);
    return xstrdup(buf);
}

static char *to_display(Value v);

static char *to_repr(Value v) {
    if (v.t == V_STR) {
        size_t n = strlen(v.v.s);
        char *out = (char *)xmalloc(n + 3);
        out[0] = '"';
        memcpy(out + 1, v.v.s, n);
        out[n + 1] = '"'; out[n + 2] = 0;
        return out;
    }
    return to_display(v);
}

static char *to_display(Value v) {
    switch (v.t) {
        case V_NIL:  return xstrdup("nil");
        case V_BOOL: return xstrdup(v.v.b ? "true" : "false");
        case V_NUM:  return num_to_str(v.v.n);
        case V_STR:  return v.v.s;
        case V_LIST: {
            size_t cap = 64, len = 0;
            char *out = (char *)xmalloc(cap);
            out[len++] = '[';
            for (int i = 0; i < v.v.list->n; i++) {
                char *item = to_repr(v.v.list->items[i]);
                size_t il = strlen(item);
                while (len + il + 4 > cap) { out = (char *)xgrow(out, cap, cap * 2); cap *= 2; }
                if (i) { out[len++] = ','; out[len++] = ' '; }
                memcpy(out + len, item, il); len += il;
            }
            out[len++] = ']'; out[len] = 0;
            return out;
        }
        case V_FN: {
            char buf[128];
            snprintf(buf, sizeof buf, "<fn %s>", v.v.fn->name);
            return xstrdup(buf);
        }
        case V_NATIVE: {
            char buf[128];
            snprintf(buf, sizeof buf, "<builtin %s>", v.v.nat->name);
            return xstrdup(buf);
        }
        case V_CLASS: {
            char buf[128];
            snprintf(buf, sizeof buf, "<class %s>", v.v.cls->name);
            return xstrdup(buf);
        }
        case V_INST: {
            char buf[128];
            snprintf(buf, sizeof buf, "<%s instance>", v.v.inst->cls->name);
            return xstrdup(buf);
        }
        case V_BOUND: {
            char buf[128];
            snprintf(buf, sizeof buf, "<method %s>", v.v.bound->fn->name);
            return xstrdup(buf);
        }
    }
    return xstrdup("?");
}

static const char *type_name(Value v) {
    switch (v.t) {
        case V_NIL: return "nil";       case V_BOOL: return "bool";
        case V_NUM: return "num";       case V_STR:  return "str";
        case V_LIST: return "list";     case V_FN:   return "fn";
        case V_NATIVE: return "fn";     case V_CLASS: return "class";
        case V_INST: return "instance"; case V_BOUND: return "fn";
    }
    return "?";
}

static int truthy(Value v) {
    switch (v.t) {
        case V_NIL:  return 0;
        case V_BOOL: return v.v.b;
        case V_NUM:  return v.v.n != 0;
        case V_STR:  return v.v.s[0] != 0;
        case V_LIST: return v.v.list->n > 0;
        default:     return 1;
    }
}

static int val_eq(Value a, Value b) {
    if (a.t != b.t) return 0;
    switch (a.t) {
        case V_NIL:  return 1;
        case V_BOOL: return a.v.b == b.v.b;
        case V_NUM:  return a.v.n == b.v.n;
        case V_STR:  return strcmp(a.v.s, b.v.s) == 0;
        case V_LIST: return a.v.list == b.v.list;
        case V_FN:   return a.v.fn == b.v.fn;
        case V_NATIVE: return a.v.nat == b.v.nat;
        case V_CLASS:  return a.v.cls == b.v.cls;
        case V_INST:   return a.v.inst == b.v.inst;
        case V_BOUND:  return a.v.bound == b.v.bound;
    }
    return 0;
}

/* ---- classes / instances ---- */

static Fn *class_find_method(Class *c, const char *name) {
    for (Class *s = c; s; s = s->super)
        for (int i = 0; i < s->methods.n; i++)
            if (strcmp(s->methods.names[i], name) == 0) return s->methods.fns[i];
    return NULL;
}
static void class_add_method(Class *c, const char *name, Fn *f) {
    MethodTable *m = &c->methods;
    if (m->n >= m->cap) {
        int nc = m->cap ? m->cap * 2 : 8;
        m->names = (char **)xgrow(m->names, sizeof(char *) * m->cap, sizeof(char *) * nc);
        m->fns   = (Fn **)  xgrow(m->fns,   sizeof(Fn *)   * m->cap, sizeof(Fn *)   * nc);
        m->cap = nc;
    }
    m->names[m->n] = xstrdup(name);
    m->fns[m->n]   = f;
    m->n++;
}
static Value *inst_find_field(Inst *in, const char *name) {
    for (int i = 0; i < in->fields.n; i++)
        if (strcmp(in->fields.names[i], name) == 0) return &in->fields.vals[i];
    return NULL;
}
static void inst_set_field(Inst *in, const char *name, Value v) {
    Value *slot = inst_find_field(in, name);
    if (slot) { *slot = v; return; }
    FieldTable *f = &in->fields;
    if (f->n >= f->cap) {
        int nc = f->cap ? f->cap * 2 : 8;
        f->names = (char **)xgrow(f->names, sizeof(char *) * f->cap, sizeof(char *) * nc);
        f->vals  = (Value *)xgrow(f->vals,  sizeof(Value)  * f->cap, sizeof(Value)  * nc);
        f->cap = nc;
    }
    f->names[f->n] = xstrdup(name);
    f->vals[f->n]  = v;
    f->n++;
}

/* ============================== interpreter ================================= */

typedef enum { X_NORMAL, X_RETURN, X_BREAK, X_CONTINUE } Flow;

static Flow  exec(Node *n, Env *env, Value *ret);
static Value eval(Node *n, Env *env);

static int   g_depth = 0;
static Value g_last;          /* value of the most recent expression statement */
static int   g_last_set = 0;

static long need_index(Value v, int line, const char *what) {
    if (v.t != V_NUM || v.v.n != floor(v.v.n))
        runtime_error(line, "%s must be a whole number, got %s", what, type_name(v));
    return (long)v.v.n;
}

static Value call_value(Value callee, int argc, Value *args, int line);

static Value call_fn(Fn *f, Value *recv, int argc, Value *args, int line) {
    if (argc != f->nparams)
        runtime_error(line, "%s() expects %d argument(s), got %d",
                      f->name, f->nparams, argc);
    if (++g_depth > MAX_CALL_DEPTH) {
        g_depth = 0;
        runtime_error(line, "maximum recursion depth exceeded");
    }
    Env *env = env_new(f->closure);
    if (recv) env_define(env, "this", *recv);
    for (int i = 0; i < argc; i++) env_define(env, f->params[i], args[i]);

    Value ret = NILV();
    Flow fl = exec(f->body, env, &ret);
    g_depth--;

    if (fl == X_BREAK)    runtime_error(line, "'break' outside of a loop");
    if (fl == X_CONTINUE) runtime_error(line, "'continue' outside of a loop");
    if (f->is_init && recv) return *recv;   /* init() always yields the instance */
    return (fl == X_RETURN) ? ret : NILV();
}

static Value call_value(Value callee, int argc, Value *args, int line) {
    switch (callee.t) {
        case V_NATIVE: {
            Native *nat = callee.v.nat;
            if (argc < nat->min_args || (nat->max_args >= 0 && argc > nat->max_args)) {
                if (nat->min_args == nat->max_args)
                    runtime_error(line, "%s() expects %d argument(s), got %d",
                                  nat->name, nat->min_args, argc);
                runtime_error(line, "%s() got %d argument(s) -- wrong arity",
                              nat->name, argc);
            }
            return nat->fn(argc, args, line);
        }
        case V_FN:
            return call_fn(callee.v.fn, NULL, argc, args, line);
        case V_BOUND:
            return call_fn(callee.v.bound->fn, &callee.v.bound->recv, argc, args, line);
        case V_CLASS: {
            Class *cls = callee.v.cls;
            Inst *in = (Inst *)xmalloc(sizeof(Inst));
            in->cls = cls;
            in->fields.names = NULL; in->fields.vals = NULL;
            in->fields.n = 0; in->fields.cap = 0;
            Value self; self.t = V_INST; self.v.inst = in;
            Fn *init = class_find_method(cls, "init");
            if (init) {
                call_fn(init, &self, argc, args, line);
            } else if (argc != 0) {
                runtime_error(line, "class %s has no init() and takes no arguments",
                              cls->name);
            }
            return self;
        }
        default:
            runtime_error(line, "cannot call a value of type %s", type_name(callee));
    }
    return NILV();
}

static Value make_bound(Value recv, Fn *f) {
    Bound *b = (Bound *)xmalloc(sizeof(Bound));
    b->recv = recv; b->fn = f;
    Value v; v.t = V_BOUND; v.v.bound = b;
    return v;
}

static char *str_concat(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *s = (char *)xmalloc(la + lb + 1);
    memcpy(s, a, la);
    memcpy(s + la, b, lb + 1);
    return s;
}

static Value eval_binary(Node *n, Env *env) {
    Value a = eval(n->a, env);
    Value b = eval(n->b, env);
    int line = n->line;
    switch (n->op) {
        case T_PLUS:
            if (a.t == V_NUM && b.t == V_NUM) return NUMV(a.v.n + b.v.n);
            if (a.t == V_STR || b.t == V_STR)
                return STRV(str_concat(to_display(a), to_display(b)));
            if (a.t == V_LIST && b.t == V_LIST) {
                VList *l = new_vlist();
                for (int i = 0; i < a.v.list->n; i++) vlist_push(l, a.v.list->items[i]);
                for (int i = 0; i < b.v.list->n; i++) vlist_push(l, b.v.list->items[i]);
                return LISTV(l);
            }
            runtime_error(line, "cannot add %s and %s", type_name(a), type_name(b));
            break;
        case T_MINUS:
            if (a.t == V_NUM && b.t == V_NUM) return NUMV(a.v.n - b.v.n);
            runtime_error(line, "cannot subtract %s from %s", type_name(b), type_name(a));
            break;
        case T_STAR:
            if (a.t == V_NUM && b.t == V_NUM) return NUMV(a.v.n * b.v.n);
            if ((a.t == V_STR && b.t == V_NUM) || (a.t == V_NUM && b.t == V_STR)) {
                const char *s = (a.t == V_STR) ? a.v.s : b.v.s;
                Value cnt = (a.t == V_STR) ? b : a;
                long k = need_index(cnt, line, "string repeat count");
                if (k < 0) k = 0;
                size_t sl = strlen(s);
                char *out = (char *)xmalloc(sl * (size_t)k + 1);
                for (long i = 0; i < k; i++) memcpy(out + (size_t)i * sl, s, sl);
                out[sl * (size_t)k] = 0;
                return STRV(out);
            }
            runtime_error(line, "cannot multiply %s and %s", type_name(a), type_name(b));
            break;
        case T_SLASH:
            if (a.t == V_NUM && b.t == V_NUM) {
                if (b.v.n == 0) runtime_error(line, "division by zero");
                return NUMV(a.v.n / b.v.n);
            }
            runtime_error(line, "cannot divide %s by %s", type_name(a), type_name(b));
            break;
        case T_PCT:
            if (a.t == V_NUM && b.t == V_NUM) {
                if (b.v.n == 0) runtime_error(line, "modulo by zero");
                return NUMV(fmod(a.v.n, b.v.n));
            }
            runtime_error(line, "cannot take %s %% %s", type_name(a), type_name(b));
            break;
        case T_EQEQ: return BOOLV(val_eq(a, b));
        case T_NEQ:  return BOOLV(!val_eq(a, b));
        case T_LT: case T_LE: case T_GT: case T_GE: {
            double cmp;
            if (a.t == V_NUM && b.t == V_NUM) cmp = a.v.n - b.v.n;
            else if (a.t == V_STR && b.t == V_STR) cmp = (double)strcmp(a.v.s, b.v.s);
            else {
                runtime_error(line, "cannot compare %s with %s",
                              type_name(a), type_name(b));
                return NILV();
            }
            switch (n->op) {
                case T_LT: return BOOLV(cmp < 0);
                case T_LE: return BOOLV(cmp <= 0);
                case T_GT: return BOOLV(cmp > 0);
                default:   return BOOLV(cmp >= 0);
            }
        }
    }
    runtime_error(line, "bad binary operator");
    return NILV();
}

static Value eval(Node *n, Env *env) {
    switch (n->kind) {
        case N_NUM:  return NUMV(n->num);
        case N_STR:  return STRV(n->str);
        case N_BOOL: return BOOLV((int)n->num);
        case N_NIL:  return NILV();
        case N_VAR:  return env_get(env, n->str, n->line);

        case N_LISTLIT: {
            VList *l = new_vlist();
            for (int i = 0; i < n->nkids; i++) vlist_push(l, eval(n->kids[i], env));
            return LISTV(l);
        }
        case N_ASSIGN: {
            Value v = eval(n->a, env);
            env_assign(env, n->str, v, n->line);
            return v;
        }
        case N_BIN:   return eval_binary(n, env);
        case N_LOGIC: {
            Value a = eval(n->a, env);
            if (n->op == T_OR)  return truthy(a) ? a : eval(n->b, env);
            /* T_AND */         return truthy(a) ? eval(n->b, env) : a;
        }
        case N_UNARY: {
            Value a = eval(n->a, env);
            if (n->op == T_MINUS) {
                if (a.t != V_NUM)
                    runtime_error(n->line, "cannot negate a %s", type_name(a));
                return NUMV(-a.v.n);
            }
            return BOOLV(!truthy(a));   /* not / ! */
        }
        case N_CALL: {
            Value callee = eval(n->a, env);
            int argc = n->nkids;
            Value *args = (Value *)xmalloc(sizeof(Value) * (argc ? argc : 1));
            for (int i = 0; i < argc; i++) args[i] = eval(n->kids[i], env);
            return call_value(callee, argc, args, n->line);
        }
        case N_GET: {
            Value obj = eval(n->a, env);
            if (obj.t == V_INST) {
                Value *field = inst_find_field(obj.v.inst, n->str);
                if (field) return *field;
                Fn *m = class_find_method(obj.v.inst->cls, n->str);
                if (m) return make_bound(obj, m);
                runtime_error(n->line, "'%s' object has no property '%s'",
                              obj.v.inst->cls->name, n->str);
            }
            runtime_error(n->line, "%s has no properties (did you mean len(x)?)",
                          type_name(obj));
            break;
        }
        case N_SET: {
            Value obj = eval(n->a, env);
            if (obj.t != V_INST)
                runtime_error(n->line, "only object instances have settable properties");
            Value v = eval(n->b, env);
            inst_set_field(obj.v.inst, n->str, v);
            return v;
        }
        case N_INDEX: {
            Value obj = eval(n->a, env);
            Value idx = eval(n->b, env);
            if (obj.t == V_LIST) {
                long i = need_index(idx, n->line, "list index");
                long len = obj.v.list->n;
                if (i < 0) i += len;
                if (i < 0 || i >= len)
                    runtime_error(n->line, "list index out of range (len %ld)", len);
                return obj.v.list->items[i];
            }
            if (obj.t == V_STR) {
                long i = need_index(idx, n->line, "string index");
                long len = (long)strlen(obj.v.s);
                if (i < 0) i += len;
                if (i < 0 || i >= len)
                    runtime_error(n->line, "string index out of range (len %ld)", len);
                char *ch = (char *)xmalloc(2);
                ch[0] = obj.v.s[i]; ch[1] = 0;
                return STRV(ch);
            }
            runtime_error(n->line, "%s is not indexable", type_name(obj));
            break;
        }
        case N_INDEXSET: {
            Value obj = eval(n->a, env);
            Value idx = eval(n->b, env);
            if (obj.t != V_LIST)
                runtime_error(n->line, "%s does not support item assignment",
                              type_name(obj));
            long i = need_index(idx, n->line, "list index");
            long len = obj.v.list->n;
            if (i < 0) i += len;
            if (i < 0 || i >= len)
                runtime_error(n->line, "list index out of range (len %ld)", len);
            Value v = eval(n->c, env);
            obj.v.list->items[i] = v;
            return v;
        }
        case N_THIS: {
            Value *slot = env_find(env, "this");
            if (!slot) runtime_error(n->line, "'this' can only be used inside a method");
            return *slot;
        }
        case N_SUPER: {
            Value *sup = env_find(env, "super");
            if (!sup || sup->t != V_CLASS)
                runtime_error(n->line, "'super' can only be used in a class that inherits");
            Value *self = env_find(env, "this");
            if (!self)
                runtime_error(n->line, "'super' can only be used inside a method");
            Fn *m = class_find_method(sup->v.cls, n->str);
            if (!m)
                runtime_error(n->line, "superclass has no method '%s'", n->str);
            return make_bound(*self, m);
        }
    }
    runtime_error(n->line, "internal: bad expression node");
    return NILV();
}

static Fn *make_fn(Node *n, Env *env, int is_init) {
    Fn *f = (Fn *)xmalloc(sizeof(Fn));
    f->name = n->str;
    f->nparams = n->nkids;
    f->params = (char **)xmalloc(sizeof(char *) * (n->nkids ? n->nkids : 1));
    for (int i = 0; i < n->nkids; i++) f->params[i] = n->kids[i]->str;
    f->body = n->b;
    f->closure = env;
    f->is_init = is_init;
    return f;
}

static Flow exec(Node *n, Env *env, Value *ret) {
    switch (n->kind) {
        case N_EXPRSTMT: {
            g_last = eval(n->a, env);
            g_last_set = 1;
            return X_NORMAL;
        }
        case N_LET: {
            Value v = n->a ? eval(n->a, env) : NILV();
            env_define(env, n->str, v);
            return X_NORMAL;
        }
        case N_BLOCK: {
            Env *inner = env_new(env);
            for (int i = 0; i < n->nkids; i++) {
                Flow fl = exec(n->kids[i], inner, ret);
                if (fl != X_NORMAL) return fl;
            }
            return X_NORMAL;
        }
        case N_IF: {
            if (truthy(eval(n->a, env))) return exec(n->b, env, ret);
            if (n->c) return exec(n->c, env, ret);
            return X_NORMAL;
        }
        case N_WHILE: {
            while (truthy(eval(n->a, env))) {
                Flow fl = exec(n->b, env, ret);
                if (fl == X_RETURN) return X_RETURN;
                if (fl == X_BREAK) break;
            }
            return X_NORMAL;
        }
        case N_FOR: {
            Value it = eval(n->a, env);
            Env *loop = env_new(env);
            if (it.t == V_LIST) {
                for (int i = 0; i < it.v.list->n; i++) {
                    env_define(loop, n->str, it.v.list->items[i]);
                    Flow fl = exec(n->b, loop, ret);
                    if (fl == X_RETURN) return X_RETURN;
                    if (fl == X_BREAK) break;
                }
            } else if (it.t == V_STR) {
                for (size_t i = 0; it.v.s[i]; i++) {
                    char *ch = (char *)xmalloc(2);
                    ch[0] = it.v.s[i]; ch[1] = 0;
                    env_define(loop, n->str, STRV(ch));
                    Flow fl = exec(n->b, loop, ret);
                    if (fl == X_RETURN) return X_RETURN;
                    if (fl == X_BREAK) break;
                }
            } else {
                runtime_error(n->line,
                    "for-in needs a list or a string, got %s (tip: range(n))",
                    type_name(it));
            }
            return X_NORMAL;
        }
        case N_FN: {
            Fn *f = make_fn(n, env, 0);
            Value v; v.t = V_FN; v.v.fn = f;
            env_define(env, n->str, v);
            return X_NORMAL;
        }
        case N_RETURN: {
            *ret = n->a ? eval(n->a, env) : NILV();
            return X_RETURN;
        }
        case N_BREAK:    return X_BREAK;
        case N_CONTINUE: return X_CONTINUE;
        case N_CLASS: {
            Class *super = NULL;
            if (n->a) {
                Value sv = eval(n->a, env);
                if (sv.t != V_CLASS)
                    runtime_error(n->a->line, "superclass '%s' is not a class",
                                  n->a->str);
                super = sv.v.cls;
            }
            Class *cls = (Class *)xmalloc(sizeof(Class));
            cls->name = n->str;
            cls->super = super;
            cls->methods.names = NULL; cls->methods.fns = NULL;
            cls->methods.n = 0; cls->methods.cap = 0;

            Env *menv = env;
            if (super) {
                menv = env_new(env);
                Value sv; sv.t = V_CLASS; sv.v.cls = super;
                env_define(menv, "super", sv);
            }
            for (int i = 0; i < n->nkids; i++) {
                Node *m = n->kids[i];
                int is_init = strcmp(m->str, "init") == 0;
                class_add_method(cls, m->str, make_fn(m, menv, is_init));
            }
            Value cv; cv.t = V_CLASS; cv.v.cls = cls;
            env_define(env, n->str, cv);
            return X_NORMAL;
        }
    }
    runtime_error(n->line, "internal: bad statement node");
    return X_NORMAL;
}

/* =============================== builtins =================================== */

static Value nat_print(int argc, Value *args, int line) {
    (void)line;
    for (int i = 0; i < argc; i++) {
        if (i) fputc(' ', stdout);
        fputs(to_display(args[i]), stdout);
    }
    fputc('\n', stdout);
    fflush(stdout);
    return NILV();
}
static Value nat_input(int argc, Value *args, int line) {
    (void)line;
    if (argc == 1) { fputs(to_display(args[0]), stdout); fflush(stdout); }
    char buf[4096];
    if (!fgets(buf, sizeof buf, stdin)) return STRV(xstrdup(""));
    char *s = buf;
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) s += 3;   /* skip a UTF-8 BOM */
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
    return STRV(xstrdup(s));
}
static Value nat_len(int argc, Value *args, int line) {
    (void)argc;
    if (args[0].t == V_STR)  return NUMV((double)strlen(args[0].v.s));
    if (args[0].t == V_LIST) return NUMV((double)args[0].v.list->n);
    runtime_error(line, "len() needs a string or a list, got %s", type_name(args[0]));
    return NILV();
}
static Value nat_str(int argc, Value *args, int line) {
    (void)argc; (void)line;
    return STRV(to_display(args[0]));
}
static Value nat_num(int argc, Value *args, int line) {
    (void)argc;
    Value v = args[0];
    if (v.t == V_NUM)  return v;
    if (v.t == V_BOOL) return NUMV(v.v.b ? 1 : 0);
    if (v.t == V_STR) {
        char *end;
        double d = strtod(v.v.s, &end);
        while (*end == ' ' || *end == '\t') end++;
        if (end == v.v.s || *end != 0)
            runtime_error(line, "num() cannot parse \"%s\"", v.v.s);
        return NUMV(d);
    }
    runtime_error(line, "num() cannot convert %s", type_name(v));
    return NILV();
}
static Value nat_range(int argc, Value *args, int line) {
    for (int i = 0; i < argc; i++)
        if (args[i].t != V_NUM)
            runtime_error(line, "range() needs numbers");
    double start = 0, stop, step = 1;
    if (argc == 1) stop = args[0].v.n;
    else { start = args[0].v.n; stop = args[1].v.n; }
    if (argc == 3) step = args[2].v.n;
    if (step == 0) runtime_error(line, "range() step cannot be 0");
    VList *l = new_vlist();
    if (step > 0) for (double x = start; x < stop; x += step) vlist_push(l, NUMV(x));
    else          for (double x = start; x > stop; x += step) vlist_push(l, NUMV(x));
    return LISTV(l);
}
static Value nat_push(int argc, Value *args, int line) {
    if (args[0].t != V_LIST)
        runtime_error(line, "push() needs a list as its first argument");
    for (int i = 1; i < argc; i++) vlist_push(args[0].v.list, args[i]);
    return args[0];
}
static Value nat_pop(int argc, Value *args, int line) {
    (void)argc;
    if (args[0].t != V_LIST) runtime_error(line, "pop() needs a list");
    VList *l = args[0].v.list;
    if (l->n == 0) runtime_error(line, "pop() from an empty list");
    return l->items[--l->n];
}
static Value nat_type(int argc, Value *args, int line) {
    (void)argc; (void)line;
    if (args[0].t == V_INST) return STRV(xstrdup(args[0].v.inst->cls->name));
    return STRV(xstrdup(type_name(args[0])));
}
static Value nat_clock(int argc, Value *args, int line) {
    (void)argc; (void)args; (void)line;
    return NUMV((double)clock() / CLOCKS_PER_SEC);
}
static double need_num_arg(Value v, int line, const char *fname) {
    if (v.t != V_NUM) runtime_error(line, "%s() needs a number, got %s",
                                    fname, type_name(v));
    return v.v.n;
}
static Value nat_abs(int argc, Value *args, int line)   { (void)argc; return NUMV(fabs(need_num_arg(args[0], line, "abs"))); }
static Value nat_floor(int argc, Value *args, int line) { (void)argc; return NUMV(floor(need_num_arg(args[0], line, "floor"))); }
static Value nat_ceil(int argc, Value *args, int line)  { (void)argc; return NUMV(ceil(need_num_arg(args[0], line, "ceil"))); }
static Value nat_sqrt(int argc, Value *args, int line) {
    (void)argc;
    double d = need_num_arg(args[0], line, "sqrt");
    if (d < 0) runtime_error(line, "sqrt() of a negative number");
    return NUMV(sqrt(d));
}
static Value nat_pow(int argc, Value *args, int line) {
    (void)argc;
    return NUMV(pow(need_num_arg(args[0], line, "pow"),
                    need_num_arg(args[1], line, "pow")));
}
static Value nat_min(int argc, Value *args, int line) {
    double best = need_num_arg(args[0], line, "min");
    for (int i = 1; i < argc; i++) {
        double d = need_num_arg(args[i], line, "min");
        if (d < best) best = d;
    }
    return NUMV(best);
}
static Value nat_max(int argc, Value *args, int line) {
    double best = need_num_arg(args[0], line, "max");
    for (int i = 1; i < argc; i++) {
        double d = need_num_arg(args[i], line, "max");
        if (d > best) best = d;
    }
    return NUMV(best);
}
static Value nat_random(int argc, Value *args, int line) {
    (void)argc; (void)args; (void)line;
    return NUMV((double)rand() / ((double)RAND_MAX + 1.0));
}
static Value nat_split(int argc, Value *args, int line) {
    (void)argc;
    if (args[0].t != V_STR || args[1].t != V_STR)
        runtime_error(line, "split() needs (string, separator)");
    const char *s = args[0].v.s, *sep = args[1].v.s;
    size_t seplen = strlen(sep);
    if (seplen == 0) runtime_error(line, "split() separator cannot be empty");
    VList *l = new_vlist();
    const char *p = s;
    for (;;) {
        const char *hit = strstr(p, sep);
        if (!hit) { vlist_push(l, STRV(xstrdup(p))); break; }
        size_t n = (size_t)(hit - p);
        char *piece = (char *)xmalloc(n + 1);
        memcpy(piece, p, n); piece[n] = 0;
        vlist_push(l, STRV(piece));
        p = hit + seplen;
    }
    return LISTV(l);
}
static Value nat_join(int argc, Value *args, int line) {
    (void)argc;
    if (args[0].t != V_LIST || args[1].t != V_STR)
        runtime_error(line, "join() needs (list, separator)");
    VList *l = args[0].v.list;
    const char *sep = args[1].v.s;
    size_t cap = 64, len = 0;
    char *out = (char *)xmalloc(cap);
    out[0] = 0;
    for (int i = 0; i < l->n; i++) {
        char *item = to_display(l->items[i]);
        size_t need = len + strlen(item) + strlen(sep) + 1;
        while (need > cap) { out = (char *)xgrow(out, cap, cap * 2); cap *= 2; }
        if (i) { strcpy(out + len, sep); len += strlen(sep); }
        strcpy(out + len, item); len += strlen(item);
    }
    return STRV(out);
}
static Value nat_upper(int argc, Value *args, int line) {
    (void)argc;
    if (args[0].t != V_STR) runtime_error(line, "upper() needs a string");
    char *s = xstrdup(args[0].v.s);
    for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p);
    return STRV(s);
}
static Value nat_lower(int argc, Value *args, int line) {
    (void)argc;
    if (args[0].t != V_STR) runtime_error(line, "lower() needs a string");
    char *s = xstrdup(args[0].v.s);
    for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
    return STRV(s);
}
static Value nat_ord(int argc, Value *args, int line) {
    (void)argc;
    if (args[0].t != V_STR || !args[0].v.s[0])
        runtime_error(line, "ord() needs a non-empty string");
    return NUMV((double)(unsigned char)args[0].v.s[0]);
}
static Value nat_chr(int argc, Value *args, int line) {
    (void)argc;
    long c = need_index(args[0], line, "chr() argument");
    if (c < 0 || c > 255) runtime_error(line, "chr() argument out of range 0..255");
    char *s = (char *)xmalloc(2);
    s[0] = (char)c; s[1] = 0;
    return STRV(s);
}

static Native g_natives[] = {
    {"print",  nat_print,  0, -1}, {"input",  nat_input,  0,  1},
    {"len",    nat_len,    1,  1}, {"str",    nat_str,    1,  1},
    {"num",    nat_num,    1,  1}, {"range",  nat_range,  1,  3},
    {"push",   nat_push,   2, -1}, {"pop",    nat_pop,    1,  1},
    {"type",   nat_type,   1,  1}, {"clock",  nat_clock,  0,  0},
    {"abs",    nat_abs,    1,  1}, {"floor",  nat_floor,  1,  1},
    {"ceil",   nat_ceil,   1,  1}, {"sqrt",   nat_sqrt,   1,  1},
    {"pow",    nat_pow,    2,  2}, {"min",    nat_min,    1, -1},
    {"max",    nat_max,    1, -1}, {"random", nat_random, 0,  0},
    {"split",  nat_split,  2,  2}, {"join",   nat_join,   2,  2},
    {"upper",  nat_upper,  1,  1}, {"lower",  nat_lower,  1,  1},
    {"ord",    nat_ord,    1,  1}, {"chr",    nat_chr,    1,  1},
    {NULL, NULL, 0, 0}
};

static Env *make_globals(void) {
    Env *g = env_new(NULL);
    for (int i = 0; g_natives[i].name; i++) {
        Value v; v.t = V_NATIVE; v.v.nat = &g_natives[i];
        env_define(g, g_natives[i].name, v);
    }
    return g;
}

/* ================================ drivers =================================== */

static void run_source(const char *src, Env *globals, int echo_last) {
    TokList tl = {NULL, 0, 0};
    lex(src, &tl);
    Node *prog = parse_program(&tl);

    g_last_set = 0;
    Value ret = NILV();
    for (int i = 0; i < prog->nkids; i++) {
        Flow fl = exec(prog->kids[i], globals, &ret);
        if (fl == X_RETURN)   runtime_error(prog->kids[i]->line, "'return' outside of a function");
        if (fl == X_BREAK)    runtime_error(prog->kids[i]->line, "'break' outside of a loop");
        if (fl == X_CONTINUE) runtime_error(prog->kids[i]->line, "'continue' outside of a loop");
    }

    if (echo_last && prog->nkids > 0 &&
        prog->kids[prog->nkids - 1]->kind == N_EXPRSTMT &&
        g_last_set && g_last.t != V_NIL) {
        printf("%s\n", to_repr(g_last));
    }
}

static void run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cyron: cannot open '%s'\n", path); exit(66); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = (char *)xmalloc((size_t)size + 1);
    size_t got = fread(src, 1, (size_t)size, f);
    src[got] = 0;
    fclose(f);

    Env *globals = make_globals();
    run_source(src, globals, 0);
}

/* count net open brackets, honouring strings and comments (for the REPL) */
static int open_brackets(const char *s) {
    int depth = 0;
    while (*s) {
        char c = *s;
        if (c == '"' || c == '\'') {
            char q = c; s++;
            while (*s && *s != q) { if (*s == '\\' && s[1]) s++; s++; }
            if (*s) s++;
            continue;
        }
        if (c == '#') { while (*s && *s != '\n') s++; continue; }
        if (c == '/' && s[1] == '/') { while (*s && *s != '\n') s++; continue; }
        if (c == '{' || c == '(' || c == '[') depth++;
        if (c == '}' || c == ')' || c == ']') depth--;
        s++;
    }
    return depth;
}

static void repl(void) {
    printf("Cyron %s -- curly braces, no semicolons, no leaks.\n", CYRON_VERSION);
    printf("Type a statement and press Enter. Ctrl+Z then Enter (or Ctrl+C) to quit.\n");
    Env *globals = make_globals();

    char line[4096];
    size_t bufcap = 8192;
    char *buf = (char *)xmalloc(bufcap);

    for (;;) {
        buf[0] = 0;
        size_t blen = 0;
        printf(">>> ");
        fflush(stdout);
        for (;;) {
            if (!fgets(line, sizeof line, stdin)) { printf("\n"); return; }
            size_t ll = strlen(line);
            while (blen + ll + 1 > bufcap) {
                buf = (char *)xgrow(buf, bufcap, bufcap * 2);
                bufcap *= 2;
            }
            memcpy(buf + blen, line, ll + 1);
            blen += ll;
            if (open_brackets(buf) <= 0) break;
            printf("... ");
            fflush(stdout);
        }
        int only_ws = 1;
        for (char *p = buf; *p; p++)
            if (!isspace((unsigned char)*p)) { only_ws = 0; break; }
        if (only_ws) continue;

        g_can_recover = 1;
        if (setjmp(g_recover) == 0)
            run_source(buf, globals, 1);
        g_depth = 0;   /* reset after a possible mid-call error */
    }
}

int main(int argc, char **argv) {
    atexit(free_all_memory);
    srand((unsigned)time(NULL));
    if (argc >= 2) run_file(argv[1]);
    else repl();
    return 0;
}
