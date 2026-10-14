#include "browser.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <math.h>
#define J_AST 4096
#define J_VARS 256
#define J_LIMIT 20000
#define J_STRING 512
enum {
    T_NAME = 256,
    T_NUMBER,
    T_STRING,
    T_EQ,
    T_NE,
    T_LE,
    T_GE,
    T_AND,
    T_OR,
    T_STRICT_EQ,
    T_STRICT_NE,
    T_INC,
    T_DEC,
    T_ADDSET,
    T_SUBSET
};
enum {
    A_NUMBER = 1,
    A_STRING,
    A_NAME,
    A_MEMBER,
    A_CALL,
    A_BINARY,
    A_UNARY,
    A_ASSIGN,
    A_CONDITIONAL,
    A_UPDATE,
    S_EXPR,
    S_VAR,
    S_BLOCK,
    S_IF,
    S_WHILE,
    S_FOR,
    S_FUNCTION,
    S_RETURN
};
enum {
    V_UNDEFINED,
    V_NUMBER,
    V_STRING,
    V_BOOL,
    V_NULL,
    V_NODE,
    V_DOCUMENT,
    V_LOCATION,
    V_CONSOLE,
    V_STYLE,
    V_FUNCTION,
    V_NATIVE
};
struct value {
    int type, node;
    double number;
    char string[J_STRING];
};
struct ast {
    int kind, op, a, b, c, d, next;
    double number;
    char text[J_STRING];
};
struct binding {
    char name[64];
    struct value value;
    int constant;
};
struct js {
    struct ast ast[J_AST];
    int count, token;
    double number;
    char text[J_STRING];
    const char *source;
    struct binding vars[J_VARS];
    int variables, scope_start, global_count, steps, depth, parse_depth, execution_depth, error,
        returned;
    struct value return_value;
    struct {
        char source[J_STRING];
        int root;
    } cache[32];
    int caches;
    struct b_document *document;
};
static void fail(struct js *j, const char *message) {
    if (!j->error) {
        snprintf(j->document->message, sizeof(j->document->message), "JavaScript: %s", message);
    }
    j->error = 1;
}
static void token(struct js *j) {
    const char *p = j->source;
    for (;;) {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') {
                p++;
            }
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            const char *end = strstr(p + 2, "*/");
            if (!end) {
                fail(j, "unterminated comment");
                j->token = 0;
                return;
            }
            p = end + 2;
            continue;
        }
        break;
    }
    j->text[0] = 0;
    if (isalpha((unsigned char)*p) || *p == '_' || *p == '$') {
        size_t n = 0;
        while (isalnum((unsigned char)*p) || *p == '_' || *p == '$') {
            if (n + 1 < J_STRING) {
                j->text[n++] = *p;
            } else {
                fail(j, "identifier too long");
            }
            p++;
        }
        j->text[n] = 0;
        j->token = T_NAME;
    } else if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
        char *end;
        j->number = strtod(p, &end);
        p = end;
        j->token = T_NUMBER;
    } else if (*p == '\'' || *p == '"') {
        int quote = *p++;
        size_t n = 0;
        while (*p && *p != quote) {
            unsigned c = (unsigned char)*p++;
            if (c == '\\') {
                if (!*p) {
                    break;
                }
                c = (unsigned char)*p++;
                if (c == 'n') {
                    c = '\n';
                } else if (c == 'r') {
                    c = '\r';
                } else if (c == 't') {
                    c = '\t';
                } else if (c != '\\' && c != '\'' && c != '"') {
                    fail(j, "unsupported string escape");
                }
            }
            if (n + 1 < J_STRING) {
                j->text[n++] = c;
            } else {
                fail(j, "string exceeds 511 bytes");
            }
        }
        if (*p == quote) {
            p++;
        } else {
            fail(j, "unterminated string");
        }
        j->text[n] = 0;
        j->token = T_STRING;
    } else {
        struct {
            const char *text;
            int kind;
        } operators[] = {{"===", T_STRICT_EQ}, {"!==", T_STRICT_NE}, {"==", T_EQ},
                         {"!=", T_NE},         {"<=", T_LE},         {">=", T_GE},
                         {"&&", T_AND},        {"||", T_OR},         {"++", T_INC},
                         {"--", T_DEC},        {"+=", T_ADDSET},     {"-=", T_SUBSET}};
        int matched = 0;
        for (unsigned i = 0; i < sizeof(operators) / sizeof(operators[0]); i++) {
            size_t n = strlen(operators[i].text);
            if (!strncmp(p, operators[i].text, n)) {
                j->token = operators[i].kind;
                p += n;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            j->token = (unsigned char)*p;
            if (*p) {
                p++;
            }
        }
    }
    j->source = p;
}
static int keyword(struct js *j, const char *s) {
    return j->token == T_NAME && !strcmp(j->text, s);
}
static int accept(struct js *j, int kind) {
    if (j->token != kind) {
        return 0;
    }
    token(j);
    return 1;
}
static void expect(struct js *j, int kind) {
    if (!accept(j, kind)) {
        fail(j, "unexpected token");
    }
}
static int make(struct js *j, int kind) {
    if (j->count + 1 >= J_AST) {
        fail(j, "AST limit exceeded");
        return 0;
    }
    int n = ++j->count;
    j->ast[n] = (struct ast){.kind = kind};
    return n;
}
static int expression(struct js *, int);
static int statement(struct js *);
static int precedence(int op) {
    if (op == '=' || op == T_ADDSET || op == T_SUBSET) {
        return 1;
    }
    if (op == '?') {
        return 2;
    }
    if (op == T_OR) {
        return 3;
    }
    if (op == T_AND) {
        return 4;
    }
    if (op == T_EQ || op == T_NE || op == T_STRICT_EQ || op == T_STRICT_NE) {
        return 5;
    }
    if (op == '<' || op == '>' || op == T_LE || op == T_GE) {
        return 6;
    }
    if (op == '+' || op == '-') {
        return 7;
    }
    if (op == '*' || op == '/' || op == '%') {
        return 8;
    }
    return 0;
}
static int primary(struct js *j) {
    int n = 0;
    if (++j->parse_depth > 128) {
        fail(j, "expression nesting limit");
        j->parse_depth--;
        return 0;
    }
    if (j->token == T_NUMBER) {
        n = make(j, A_NUMBER);
        j->ast[n].number = j->number;
        token(j);
    } else if (j->token == T_STRING) {
        n = make(j, A_STRING);
        b_copy(j->ast[n].text, J_STRING, j->text);
        token(j);
    } else if (j->token == '!' || j->token == '-' || j->token == '+' || j->token == T_INC ||
               j->token == T_DEC) {
        int op = j->token;
        token(j);
        n = make(j, op == T_INC || op == T_DEC ? A_UPDATE : A_UNARY);
        j->ast[n].op = op;
        j->ast[n].a = primary(j);
    } else if (j->token == T_NAME) {
        n = make(j, A_NAME);
        b_copy(j->ast[n].text, J_STRING, j->text);
        token(j);
    } else if (accept(j, '(')) {
        n = expression(j, 1);
        expect(j, ')');
    } else {
        fail(j, "expected expression");
        if (j->token) {
            token(j);
        }
    }
    while (!j->error) {
        if (accept(j, '.')) {
            int m = make(j, A_MEMBER);
            j->ast[m].a = n;
            if (j->token != T_NAME) {
                fail(j, "expected property");
            }
            b_copy(j->ast[m].text, J_STRING, j->text);
            token(j);
            n = m;
        } else if (accept(j, '(')) {
            int m = make(j, A_CALL);
            j->ast[m].a = n;
            int *next = &j->ast[m].b;
            if (j->token != ')') {
                for (unsigned arguments = 0; !j->error; arguments++) {
                    if (arguments >= 8) {
                        fail(j, "argument limit exceeded");
                        break;
                    }
                    int arg = expression(j, 1);
                    *next = arg;
                    next = &j->ast[arg].next;
                    if (!accept(j, ',')) {
                        break;
                    }
                }
            }
            expect(j, ')');
            n = m;
        } else if (j->token == T_INC || j->token == T_DEC) {
            int m = make(j, A_UPDATE);
            j->ast[m].a = n;
            j->ast[m].op = j->token;
            j->ast[m].b = 1;
            token(j);
            n = m;
        } else {
            break;
        }
    }
    j->parse_depth--;
    return n;
}
static int expression(struct js *j, int minimum) {
    if (++j->parse_depth > 128) {
        fail(j, "expression nesting limit");
        j->parse_depth--;
        return 0;
    }
    int left = primary(j);
    while (!j->error && precedence(j->token) >= minimum) {
        int op = j->token, level = precedence(op);
        token(j);
        if (op == '?') {
            int n = make(j, A_CONDITIONAL);
            j->ast[n].a = left;
            j->ast[n].b = expression(j, 1);
            expect(j, ':');
            j->ast[n].c = expression(j, 2);
            left = n;
            continue;
        }
        int n = make(j, level == 1 ? A_ASSIGN : A_BINARY);
        j->ast[n].op = op;
        j->ast[n].a = left;
        j->ast[n].b = expression(j, level == 1 ? level : level + 1);
        left = n;
    }
    j->parse_depth--;
    return left;
}
static int variable(struct js *j, int semicolon) {
    int constant = keyword(j, "const");
    token(j);
    int block = make(j, S_BLOCK), *next = &j->ast[block].a;
    while (!j->error) {
        int n = make(j, S_VAR);
        *next = n;
        next = &j->ast[n].next;
        j->ast[n].op = constant;
        if (j->token != T_NAME) {
            fail(j, "expected variable name");
            break;
        }
        b_copy(j->ast[n].text, J_STRING, j->text);
        token(j);
        if (accept(j, '=')) {
            j->ast[n].a = expression(j, 1);
        } else if (constant) {
            fail(j, "const requires an initializer");
        }
        if (!accept(j, ',')) {
            break;
        }
    }
    if (semicolon) {
        accept(j, ';');
    }
    return block;
}
static int statement(struct js *j) {
    if (++j->parse_depth > 128) {
        fail(j, "statement nesting limit");
        j->parse_depth--;
        return 0;
    }
    int n;
    if (accept(j, '{')) {
        n = make(j, S_BLOCK);
        int *next = &j->ast[n].a;
        while (j->token && j->token != '}' && !j->error) {
            int s = statement(j);
            *next = s;
            next = &j->ast[s].next;
        }
        expect(j, '}');
    } else if (keyword(j, "var") || keyword(j, "let") || keyword(j, "const")) {
        n = variable(j, 1);
    } else if (keyword(j, "if")) {
        token(j);
        n = make(j, S_IF);
        expect(j, '(');
        j->ast[n].a = expression(j, 1);
        expect(j, ')');
        j->ast[n].b = statement(j);
        if (keyword(j, "else")) {
            token(j);
            j->ast[n].c = statement(j);
        }
    } else if (keyword(j, "while")) {
        token(j);
        n = make(j, S_WHILE);
        expect(j, '(');
        j->ast[n].a = expression(j, 1);
        expect(j, ')');
        j->ast[n].b = statement(j);
    } else if (keyword(j, "for")) {
        token(j);
        n = make(j, S_FOR);
        expect(j, '(');
        if (keyword(j, "var") || keyword(j, "let") || keyword(j, "const")) {
            j->ast[n].a = variable(j, 0);
        } else if (j->token != ';') {
            int s = make(j, S_EXPR);
            j->ast[s].a = expression(j, 1);
            j->ast[n].a = s;
        }
        expect(j, ';');
        if (j->token != ';') {
            j->ast[n].b = expression(j, 1);
        }
        expect(j, ';');
        if (j->token != ')') {
            j->ast[n].c = expression(j, 1);
        }
        expect(j, ')');
        j->ast[n].d = statement(j);
    } else if (keyword(j, "function")) {
        token(j);
        n = make(j, S_FUNCTION);
        if (j->token != T_NAME) {
            fail(j, "expected function name");
        }
        b_copy(j->ast[n].text, J_STRING, j->text);
        token(j);
        expect(j, '(');
        int *next = &j->ast[n].a;
        if (j->token != ')') {
            for (unsigned count = 0; !j->error; count++) {
                if (count >= 8) {
                    fail(j, "parameter limit exceeded");
                    break;
                }
                if (j->token != T_NAME) {
                    fail(j, "expected parameter name");
                    break;
                }
                int p = make(j, A_NAME);
                b_copy(j->ast[p].text, J_STRING, j->text);
                *next = p;
                next = &j->ast[p].next;
                token(j);
                if (!accept(j, ',')) {
                    break;
                }
            }
        }
        expect(j, ')');
        if (j->token != '{') {
            fail(j, "expected function body");
        }
        j->ast[n].b = statement(j);
    } else if (keyword(j, "return")) {
        token(j);
        n = make(j, S_RETURN);
        if (j->token != ';' && j->token != '}' && j->token) {
            j->ast[n].a = expression(j, 1);
        }
        accept(j, ';');
    } else if (accept(j, ';')) {
        n = make(j, S_BLOCK);
    } else {
        n = make(j, S_EXPR);
        j->ast[n].a = expression(j, 1);
        accept(j, ';');
    }
    j->parse_depth--;
    return n;
}
static struct value undefined(void) {
    return (struct value){0};
}
static struct value number(double n) {
    return (struct value){.type = V_NUMBER, .number = n};
}
static struct value string(const char *s) {
    struct value v = {.type = V_STRING};
    b_copy(v.string, sizeof(v.string), s);
    return v;
}
static int truth(struct value v) {
    return v.type == V_STRING                       ? *v.string != 0
           : v.type == V_NUMBER || v.type == V_BOOL ? v.number != 0 && !isnan(v.number)
                                                    : v.type != V_UNDEFINED && v.type != V_NULL;
}
static double numeric(struct value v) {
    if (v.type == V_NUMBER || v.type == V_BOOL) {
        return v.number;
    }
    if (v.type == V_NULL) {
        return 0;
    }
    if (v.type == V_STRING) {
        char *end;
        double n = strtod(v.string, &end);
        while (isspace((unsigned char)*end)) {
            end++;
        }
        return *end ? NAN : n;
    }
    return NAN;
}
static void stringify(struct value v, char *out, size_t size) {
    if (v.type == V_STRING) {
        b_copy(out, size, v.string);
    } else if (v.type == V_NUMBER) {
        snprintf(out, size, "%.10g", v.number);
    } else {
        b_copy(out, size,
               v.type == V_BOOL        ? (v.number ? "true" : "false")
               : v.type == V_NULL      ? "null"
               : v.type == V_UNDEFINED ? "undefined"
                                       : "[object]");
    }
}
static int binding(struct js *j, const char *name) {
    for (int i = j->variables - 1; i >= j->scope_start; i--) {
        if (!strcmp(j->vars[i].name, name)) {
            return i;
        }
    }
    if (j->depth) {
        for (int i = j->global_count - 1; i >= 0; i--) {
            if (!strcmp(j->vars[i].name, name)) {
                return i;
            }
        }
    }
    return -1;
}
static void declare(struct js *j, const char *name, struct value v, int constant) {
    if (strlen(name) >= sizeof(j->vars[0].name)) {
        fail(j, "variable name too long");
        return;
    }
    for (int i = j->variables - 1; i >= j->scope_start; i--) {
        if (!strcmp(j->vars[i].name, name)) {
            if (j->vars[i].constant || constant) {
                fail(j, "duplicate constant");
            } else {
                j->vars[i].value = v;
            }
            return;
        }
    }
    if (j->variables >= J_VARS) {
        fail(j, "variable limit exceeded");
        return;
    }
    struct binding *b = &j->vars[j->variables++];
    b_copy(b->name, sizeof(b->name), name);
    b->value = v;
    b->constant = constant;
}
static struct value evaluate(struct js *, int);
static void execute(struct js *, int);
static struct value property(struct js *j, struct value object, const char *name) {
    struct b_document *d = j->document;
    if (object.type == V_NODE && object.node >= 0 && object.node < d->count) {
        if (!strcmp(name, "textContent") || !strcmp(name, "innerText")) {
            char text[J_STRING] = "";
            b_collect_text(d, object.node, text, sizeof(text));
            return string(text);
        }
        if (!strcmp(name, "value") || !strcmp(name, "id") || !strcmp(name, "href") ||
            !strcmp(name, "className")) {
            return string(b_attr(d, object.node, !strcmp(name, "className") ? "class" : name));
        }
        if (!strcmp(name, "style")) {
            return (struct value){.type = V_STYLE, .node = object.node};
        }
    }
    if (object.type == V_DOCUMENT) {
        if (!strcmp(name, "title")) {
            return string(d->title);
        }
        if (!strcmp(name, "getElementById")) {
            return (struct value){.type = V_NATIVE, .node = 1};
        }
    }
    if (object.type == V_LOCATION && !strcmp(name, "href")) {
        return string(d->location);
    }
    if (object.type == V_CONSOLE && !strcmp(name, "log")) {
        return (struct value){.type = V_NATIVE, .node = 2};
    }
    if (object.type == V_STRING && !strcmp(name, "length")) {
        return number(strlen(object.string));
    }
    if (object.type == V_STYLE) {
        const char *css = !strcmp(name, "color")             ? "color"
                          : !strcmp(name, "backgroundColor") ? "background-color"
                          : !strcmp(name, "display")         ? "display"
                                                             : NULL;
        if (css) {
            char copy[512], result[512] = "", *save;
            b_copy(copy, sizeof(copy), b_attr(d, object.node, "style"));
            for (char *part = strtok_r(copy, ";", &save); part; part = strtok_r(NULL, ";", &save)) {
                char *colon = strchr(part, ':');
                if (!colon) {
                    continue;
                }
                *colon++ = 0;
                while (isspace((unsigned char)*part)) {
                    part++;
                }
                char *end = part + strlen(part);
                while (end > part && isspace((unsigned char)end[-1])) {
                    *--end = 0;
                }
                if (!strcmp(part, css)) {
                    while (isspace((unsigned char)*colon)) {
                        colon++;
                    }
                    b_copy(result, sizeof(result), colon);
                }
            }
            return string(result);
        }
    }
    if (object.type == V_NULL || object.type == V_UNDEFINED) {
        fail(j, "property access on null/undefined");
    } else {
        fail(j, "unsupported property");
    }
    return undefined();
}
static void store(struct js *j, int node, struct value value) {
    struct ast *a = &j->ast[node];
    char text[J_STRING];
    stringify(value, text, sizeof(text));
    struct b_document *d = j->document;
    if (a->kind == A_NAME) {
        int index = binding(j, a->text);
        if (index < 0) {
            fail(j, "assignment to undeclared variable");
            return;
        }
        if (j->vars[index].constant) {
            fail(j, "assignment to const");
            return;
        }
        j->vars[index].value = value;
        return;
    }
    if (a->kind == A_MEMBER) {
        struct value object = evaluate(j, a->a);
        if (object.type == V_NODE) {
            if (!strcmp(a->text, "textContent") || !strcmp(a->text, "innerText")) {
                b_text(d, object.node, text);
                return;
            }
            if (!strcmp(a->text, "value") || !strcmp(a->text, "id") || !strcmp(a->text, "href") ||
                !strcmp(a->text, "className")) {
                if (b_set_attr(d, object.node, !strcmp(a->text, "className") ? "class" : a->text,
                               text)) {
                    fail(j, "DOM attribute limit");
                }
                return;
            }
        }
        if (object.type == V_DOCUMENT && !strcmp(a->text, "title")) {
            b_copy(d->title, sizeof(d->title), text);
            return;
        }
        if (object.type == V_LOCATION && !strcmp(a->text, "href")) {
            b_copy(d->navigate, sizeof(d->navigate), text);
            return;
        }
        if (object.type == V_STYLE) {
            const char *css = !strcmp(a->text, "color")             ? "color"
                              : !strcmp(a->text, "backgroundColor") ? "background-color"
                              : !strcmp(a->text, "display")         ? "display"
                                                                    : NULL;
            if (css) {
                if (strchr(text, ';') || strchr(text, '{') || strchr(text, '}')) {
                    fail(j, "invalid style property value");
                    return;
                }
                char style[512] = "", copy[512], *save;
                b_copy(copy, sizeof(copy), b_attr(d, object.node, "style"));
                for (char *part = strtok_r(copy, ";", &save); part;
                     part = strtok_r(NULL, ";", &save)) {
                    char *colon = strchr(part, ':');
                    if (!colon) {
                        continue;
                    }
                    *colon = 0;
                    while (isspace((unsigned char)*part)) {
                        part++;
                    }
                    char *end = part + strlen(part);
                    while (end > part && isspace((unsigned char)end[-1])) {
                        *--end = 0;
                    }
                    if (strcmp(part, css)) {
                        size_t used = strlen(style);
                        int n =
                            snprintf(style + used, sizeof(style) - used, "%s:%s;", part, colon + 1);
                        if (n < 0 || (size_t)n >= sizeof(style) - used) {
                            fail(j, "style limit");
                            return;
                        }
                    }
                }
                size_t used = strlen(style);
                int n = snprintf(style + used, sizeof(style) - used, "%s:%s", css, text);
                if (n < 0 || (size_t)n >= sizeof(style) - used ||
                    b_set_attr(d, object.node, "style", style)) {
                    fail(j, "style limit");
                }
                return;
            }
        }
    }
    fail(j, "unsupported assignment target");
}
static struct value binary(struct js *j, int op, struct value a, struct value b) {
    if (op == '+' && (a.type == V_STRING || b.type == V_STRING)) {
        char x[J_STRING], y[J_STRING];
        stringify(a, x, sizeof(x));
        stringify(b, y, sizeof(y));
        if (strlen(x) + strlen(y) >= sizeof(x)) {
            fail(j, "concatenated string too long");
            return undefined();
        }
        strcat(x, y);
        return string(x);
    }
    double x = numeric(a), y = numeric(b), result = 0;
    if (op == '+' || op == T_ADDSET) {
        return number(x + y);
    }
    if (op == '-' || op == T_SUBSET) {
        return number(x - y);
    }
    if (op == '*') {
        return number(x * y);
    }
    if (op == '/') {
        return number(x / y);
    }
    if (op == '%') {
        return number(fmod(x, y));
    }
    if (op == T_EQ || op == T_NE || op == T_STRICT_EQ || op == T_STRICT_NE) {
        int same = a.type == b.type
                       ? (a.type == V_STRING                       ? !strcmp(a.string, b.string)
                          : a.type == V_NUMBER || a.type == V_BOOL ? a.number == b.number
                          : a.type == V_NODE || a.type == V_STYLE || a.type == V_FUNCTION ||
                                  a.type == V_NATIVE
                              ? a.node == b.node
                              : 1)
                   : op == T_STRICT_EQ || op == T_STRICT_NE ? 0
                   : (a.type == V_NULL && b.type == V_UNDEFINED) ||
                           (a.type == V_UNDEFINED && b.type == V_NULL)
                       ? 1
                       : x == y;
        result = op == T_NE || op == T_STRICT_NE ? !same : same;
    } else if (op == '<') {
        result = a.type == V_STRING && b.type == V_STRING ? strcmp(a.string, b.string) < 0 : x < y;
    } else if (op == '>') {
        result = a.type == V_STRING && b.type == V_STRING ? strcmp(a.string, b.string) > 0 : x > y;
    } else if (op == T_LE) {
        result = x <= y;
    } else if (op == T_GE) {
        result = x >= y;
    }
    return (struct value){.type = V_BOOL, .number = result};
}
static struct value call(struct js *j, struct value function, int args) {
    struct value values[8];
    int count = 0;
    for (int a = args; a && count < 8; a = j->ast[a].next) {
        values[count++] = evaluate(j, a);
    }
    if (j->error) {
        return undefined();
    }
    if (function.type == V_NATIVE) {
        if (function.node == 1) {
            if (!count) {
                fail(j, "getElementById requires an id");
                return undefined();
            }
            char id[J_STRING];
            stringify(values[0], id, sizeof(id));
            int node = b_element(j->document, id);
            return (struct value){.type = node < 0 ? V_NULL : V_NODE, .node = node};
        }
        if (function.node == 2) {
            char message[J_STRING] = "";
            for (int i = 0; i < count; i++) {
                char s[J_STRING];
                stringify(values[i], s, sizeof(s));
                if (strlen(message) + strlen(s) + 2 >= sizeof(message)) {
                    break;
                }
                if (i) {
                    strcat(message, " ");
                }
                strcat(message, s);
            }
            printf("KalyJS: %s\n", message);
            return undefined();
        }
        if (function.node == 3) {
            char text[J_STRING] = "";
            if (count) {
                stringify(values[0], text, sizeof(text));
            }
            b_copy(j->document->message, sizeof(j->document->message), text);
            return undefined();
        }
        if (function.node == 4) {
            char text[J_STRING] = "";
            if (count) {
                stringify(values[0], text, sizeof(text));
            }
            return string(text);
        }
        if (function.node == 5) {
            return number(count ? numeric(values[0]) : 0);
        }
        if (function.node == 6) {
            char text[J_STRING] = "";
            if (count) {
                stringify(values[0], text, sizeof(text));
            }
            char *end;
            double requested = count > 1 ? numeric(values[1]) : 0;
            if (!isfinite(requested) || requested < 0 || requested > 36) {
                return number(NAN);
            }
            int radix = (int)requested;
            const char *digits = text;
            while (isspace((unsigned char)*digits)) {
                digits++;
            }
            if (*digits == '+' || *digits == '-') {
                digits++;
            }
            if (!radix) {
                radix = digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X') ? 16 : 10;
            }
            if (radix < 2) {
                return number(NAN);
            }
            double v = strtol(text, &end, radix);
            return number(end == text ? NAN : v);
        }
    }
    if (function.type != V_FUNCTION) {
        fail(j, "value is not callable");
        return undefined();
    }
    if (j->depth >= 16) {
        fail(j, "call depth exceeded");
        return undefined();
    }
    int saved = j->variables, start = j->scope_start, returned = j->returned;
    struct value previous = j->return_value;
    if (!j->depth) {
        j->global_count = saved;
    }
    j->scope_start = saved;
    j->depth++;
    j->returned = 0;
    int parameter = j->ast[function.node].a, index = 0;
    while (parameter) {
        declare(j, j->ast[parameter].text, index < count ? values[index] : undefined(), 0);
        index++;
        parameter = j->ast[parameter].next;
    }
    execute(j, j->ast[function.node].b);
    struct value result = j->returned ? j->return_value : undefined();
    j->variables = saved;
    j->scope_start = start;
    j->depth--;
    j->returned = returned;
    j->return_value = previous;
    return result;
}
static struct value evaluate_impl(struct js *, int);
static struct value evaluate(struct js *j, int node) {
    if (++j->execution_depth > 24) {
        fail(j, "runtime nesting limit exceeded");
        j->execution_depth--;
        return undefined();
    }
    struct value result = evaluate_impl(j, node);
    j->execution_depth--;
    return result;
}
static struct value evaluate_impl(struct js *j, int node) {
    if (!node || j->error) {
        return undefined();
    }
    if (++j->steps > J_LIMIT) {
        fail(j, "instruction budget exceeded");
        return undefined();
    }
    struct ast *a = &j->ast[node];
    switch (a->kind) {
    case A_NUMBER:
        return number(a->number);
    case A_STRING:
        return string(a->text);
    case A_NAME: {
        int i = binding(j, a->text);
        if (i >= 0) {
            return j->vars[i].value;
        }
        if (!strcmp(a->text, "true") || !strcmp(a->text, "false")) {
            return (struct value){.type = V_BOOL, .number = !strcmp(a->text, "true")};
        }
        if (!strcmp(a->text, "null")) {
            return (struct value){.type = V_NULL};
        }
        if (!strcmp(a->text, "undefined")) {
            return undefined();
        }
        if (!strcmp(a->text, "document")) {
            return (struct value){.type = V_DOCUMENT};
        }
        if (!strcmp(a->text, "location")) {
            return (struct value){.type = V_LOCATION};
        }
        if (!strcmp(a->text, "console")) {
            return (struct value){.type = V_CONSOLE};
        }
        const char *names[] = {"alert", "String", "Number", "parseInt"};
        for (int n = 0; n < 4; n++) {
            if (!strcmp(a->text, names[n])) {
                return (struct value){.type = V_NATIVE, .node = n + 3};
            }
        }
        fail(j, "unknown identifier");
        return undefined();
    }
    case A_MEMBER:
        return property(j, evaluate(j, a->a), a->text);
    case A_CALL: {
        struct value f = evaluate(j, a->a);
        return call(j, f, a->b);
    }
    case A_BINARY: {
        struct value left = evaluate(j, a->a);
        if (a->op == T_AND) {
            return truth(left) ? evaluate(j, a->b) : left;
        }
        if (a->op == T_OR) {
            return truth(left) ? left : evaluate(j, a->b);
        }
        struct value right = evaluate(j, a->b);
        return binary(j, a->op, left, right);
    }
    case A_UNARY: {
        struct value v = evaluate(j, a->a);
        return a->op == '!' ? (struct value){.type = V_BOOL, .number = !truth(v)}
                            : number(a->op == '-' ? -numeric(v) : numeric(v));
    }
    case A_CONDITIONAL:
        return evaluate(j, truth(evaluate(j, a->a)) ? a->b : a->c);
    case A_ASSIGN: {
        struct value value = evaluate(j, a->b);
        if (a->op != '=') {
            value = binary(j, a->op == T_ADDSET ? '+' : '-', evaluate(j, a->a), value);
        }
        store(j, a->a, value);
        return value;
    }
    case A_UPDATE: {
        struct value old = evaluate(j, a->a),
                     next = number(numeric(old) + (a->op == T_INC ? 1 : -1));
        store(j, a->a, next);
        return a->b ? old : next;
    }
    default:
        fail(j, "invalid expression");
        return undefined();
    }
}
static void execute_impl(struct js *, int);
static void execute(struct js *j, int node) {
    if (++j->execution_depth > 24) {
        fail(j, "runtime nesting limit exceeded");
        j->execution_depth--;
        return;
    }
    execute_impl(j, node);
    j->execution_depth--;
}
static void execute_impl(struct js *j, int node) {
    if (!node || j->error || j->returned) {
        return;
    }
    if (++j->steps > J_LIMIT) {
        fail(j, "instruction budget exceeded");
        return;
    }
    struct ast *a = &j->ast[node];
    switch (a->kind) {
    case S_EXPR:
        evaluate(j, a->a);
        break;
    case S_VAR:
        declare(j, a->text, evaluate(j, a->a), a->op);
        break;
    case S_BLOCK:
        for (int s = a->a; s && !j->error && !j->returned; s = j->ast[s].next) {
            execute(j, s);
        }
        break;
    case S_IF:
        execute(j, truth(evaluate(j, a->a)) ? a->b : a->c);
        break;
    case S_WHILE:
        while (!j->error && !j->returned && truth(evaluate(j, a->a))) {
            execute(j, a->b);
        }
        break;
    case S_FOR:
        execute(j, a->a);
        while (!j->error && !j->returned && (!a->b || truth(evaluate(j, a->b)))) {
            execute(j, a->d);
            if (!j->returned) {
                evaluate(j, a->c);
            }
        }
        break;
    case S_FUNCTION:
        declare(j, a->text, (struct value){.type = V_FUNCTION, .node = node}, 0);
        break;
    case S_RETURN:
        if (!j->depth) {
            fail(j, "return outside function");
        } else {
            j->return_value = evaluate(j, a->a);
            j->returned = 1;
        }
        break;
    default:
        fail(j, "invalid statement");
        break;
    }
}
int b_js_init(struct b_document *d) {
    d->js = calloc(1, sizeof(struct js));
    if (!d->js) {
        b_copy(d->message, sizeof(d->message), "JavaScript: out of memory");
        return -1;
    }
    ((struct js *)d->js)->document = d;
    return b_js_run(d, d->scripts);
}
int b_js_run(struct b_document *d, const char *source) {
    struct js *j = d->js;
    if (!j || !*source) {
        return 0;
    }
    j->error = j->returned = j->steps = j->parse_depth = j->execution_depth = 0;
    d->message[0] = 0;
    int root = 0;
    for (int i = 0; i < j->caches; i++) {
        if (!strcmp(j->cache[i].source, source)) {
            root = j->cache[i].root;
            break;
        }
    }
    if (!root) {
        int before = j->count;
        j->source = source;
        token(j);
        root = make(j, S_BLOCK);
        int *next = &j->ast[root].a;
        while (j->token && !j->error) {
            int s = statement(j);
            *next = s;
            next = &j->ast[s].next;
        }
        if (j->error) {
            j->count = before;
            return -1;
        }
        if (strlen(source) < J_STRING && j->caches < 32) {
            int cache = j->caches++;
            b_copy(j->cache[cache].source, J_STRING, source);
            j->cache[cache].root = root;
        }
    }
    execute(j, root);
    return j->error ? -1 : 0;
}
void b_js_free(struct b_document *d) {
    free(d->js);
    d->js = NULL;
}
