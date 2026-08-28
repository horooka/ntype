#include "utils.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
        NTypeNode *node;
        int weight;
        int priority;
} HeapItem;

typedef struct {
        const char *symbol;
        char *seq;
        int length;
} Code;

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

static char *xstrndup(const char *s, size_t n) {
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

int separate_symbols(const char *input, char ***symbols) {
    *symbols = NULL;
    if (!input) {
        return 0;
    }

    int count = 0;
    int cap = 0;
    char **items = NULL;
    const char *p = input;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *start = p;
        while (*p && *p != ',') {
            p++;
        }
        const char *end = p;
        while (end > start &&
               (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n')) {
            end--;
        }
        if (end > start) {
            if (count == cap) {
                cap = cap ? cap * 2 : 8;
                items = realloc(items, sizeof(*items) * (size_t)cap);
                if (!items) {
                    fprintf(stderr, "out of memory\n");
                    exit(1);
                }
            }
            items[count++] = xstrndup(start, (size_t)(end - start));
        }
        if (*p == ',') {
            p++;
        }
    }

    *symbols = items;
    return count;
}

void free_symbols(char **symbols, int count) {
    if (!symbols) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(symbols[i]);
    }
    free(symbols);
}

static int arity_of(const NTypeNode *n) { return 2 * (int)n->n_buttons; }

static NTypeNode *child_at(const NTypeNode *n, int slot) {
    int nb = n->n_buttons;
    if (slot < nb) {
        return n->click[slot];
    }
    return n->hold[slot - nb];
}

static void set_child(NTypeNode *n, int slot, NTypeNode *c) {
    int nb = n->n_buttons;
    if (slot < nb) {
        n->click[slot] = c;
    } else {
        n->hold[slot - nb] = c;
    }
}

static char slot_char(const NTypeNode *n, int slot) {
    int nb = n->n_buttons;
    if (slot < nb) {
        return (char)('a' + slot);
    }
    return (char)('A' + (slot - nb));
}

static NTypeNode *new_node(const char *symbol, int n_buttons) {
    NTypeNode *n = xmalloc(sizeof(*n));
    n->symbol = symbol ? xstrdup(symbol) : NULL;
    n->n_buttons = (unsigned char)n_buttons;
    for (int i = 0; i < NTYPE_MAX_BUTTONS; i++) {
        n->click[i] = NULL;
        n->hold[i] = NULL;
    }
    return n;
}

static int lighter(const HeapItem *a, const HeapItem *b) {
    if (a->weight != b->weight) {
        return a->weight < b->weight;
    }
    return a->priority > b->priority;
}

static int heavier_first(const void *a, const void *b) {
    const HeapItem *x = a;
    const HeapItem *y = b;
    if (x->weight != y->weight) {
        return (x->weight < y->weight) ? 1 : -1;
    }
    return x->priority - y->priority;
}

static int find_min(HeapItem *items, int n) {
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!items[i].node) {
            continue;
        }
        if (best < 0 || lighter(&items[i], &items[best])) {
            best = i;
        }
    }
    return best;
}

static int best_rank(const NTypeNode *n, const char **symbols, int nsym) {
    if (!n) {
        return nsym;
    }
    if (n->symbol) {
        for (int i = 0; i < nsym; i++) {
            if (strcmp(n->symbol, symbols[i]) == 0) {
                return i;
            }
        }
        return nsym;
    }
    int best = nsym;
    int arity = arity_of(n);
    for (int s = 0; s < arity; s++) {
        int r = best_rank(child_at(n, s), symbols, nsym);
        if (r < best) {
            best = r;
        }
    }
    return best;
}

static int is_dummy(const NTypeNode *n) {
    if (!n) {
        return 1;
    }
    if (n->symbol) {
        return 0;
    }
    int arity = arity_of(n);
    for (int s = 0; s < arity; s++) {
        if (child_at(n, s)) {
            return 0;
        }
    }
    return 1;
}

void free_ntype_tree(NTypeNode *root) {
    if (!root) {
        return;
    }
    int arity = arity_of(root);
    for (int s = 0; s < arity; s++) {
        free_ntype_tree(child_at(root, s));
    }
    free(root->symbol);
    free(root);
}

static void prefer_cheap(NTypeNode *n, const char **symbols, int nsym) {
    if (!n) {
        return;
    }
    int arity = arity_of(n);
    for (int s = 0; s < arity; s++) {
        prefer_cheap(child_at(n, s), symbols, nsym);
    }

    NTypeNode *kids[NTYPE_MAX_BUTTONS * 2];
    int nk = 0;
    for (int s = 0; s < arity; s++) {
        NTypeNode *c = child_at(n, s);
        if (c) {
            kids[nk++] = c;
        }
        set_child(n, s, NULL);
    }
    for (int i = 1; i < nk; i++) {
        NTypeNode *x = kids[i];
        int rx = best_rank(x, symbols, nsym);
        int j = i;
        while (j > 0 && best_rank(kids[j - 1], symbols, nsym) > rx) {
            kids[j] = kids[j - 1];
            j--;
        }
        kids[j] = x;
    }
    for (int i = 0; i < nk; i++) {
        set_child(n, i, kids[i]);
    }
}

static void prune_dummies(NTypeNode *n) {
    if (!n) {
        return;
    }
    int arity = arity_of(n);
    for (int s = 0; s < arity; s++) {
        NTypeNode *c = child_at(n, s);
        prune_dummies(c);
        if (is_dummy(c)) {
            free_ntype_tree(c);
            set_child(n, s, NULL);
        }
    }
}

int build_ntype_tree(NTypeNode **root, const char **symbols, int count,
                     int n_buttons) {
    *root = NULL;
    if (count <= 0 || n_buttons < 1 || n_buttons > NTYPE_MAX_BUTTONS) {
        return -1;
    }

    int arity = 2 * n_buttons;
    int dummies = 0;
    if (count > 1 && arity > 1) {
        while ((count + dummies - 1) % (arity - 1) != 0) {
            dummies++;
        }
    }

    int heap_n = count + dummies;
    HeapItem *items = xmalloc(sizeof(*items) * (size_t)heap_n);
    int live = heap_n;
    for (int i = 0; i < count; i++) {
        items[i].node = new_node(symbols[i], n_buttons);
        items[i].weight = (count * count) / (i + 1);
        if (items[i].weight < 1) {
            items[i].weight = 1;
        }
        items[i].priority = i;
    }
    for (int i = 0; i < dummies; i++) {
        items[count + i].node = new_node(NULL, n_buttons);
        items[count + i].weight = 0;
        items[count + i].priority = count + i;
    }

    HeapItem taken[NTYPE_MAX_BUTTONS * 2];
    while (live > 1) {
        int take = arity;
        if (take > live) {
            take = live;
        }
        for (int t = 0; t < take; t++) {
            int i = find_min(items, heap_n);
            taken[t] = items[i];
            items[i].node = NULL;
        }
        live -= take;
        qsort(taken, (size_t)take, sizeof(*taken), heavier_first);

        NTypeNode *parent = new_node(NULL, n_buttons);
        int wsum = 0;
        int pmin = taken[0].priority;
        for (int t = 0; t < take; t++) {
            set_child(parent, t, taken[t].node);
            wsum += taken[t].weight;
            if (taken[t].priority < pmin) {
                pmin = taken[t].priority;
            }
        }

        int hole = -1;
        for (int i = 0; i < heap_n; i++) {
            if (!items[i].node) {
                hole = i;
                break;
            }
        }
        items[hole].node = parent;
        items[hole].weight = wsum;
        items[hole].priority = pmin;
        live++;
    }

    for (int i = 0; i < heap_n; i++) {
        if (items[i].node) {
            *root = items[i].node;
            break;
        }
    }
    free(items);
    prefer_cheap(*root, symbols, count);
    prune_dummies(*root);
    return 0;
}

static void format_seq(const NTypeNode *n, const unsigned char *turns,
                       int depth, char *out) {
    for (int i = 0; i < depth; i++) {
        out[i] = slot_char(n, turns[i]);
    }
    out[depth] = '\0';
}

static int tree_height(const NTypeNode *n) {
    if (!n) {
        return 0;
    }
    int h = 0;
    int arity = arity_of(n);
    for (int s = 0; s < arity; s++) {
        int ch = tree_height(child_at(n, s));
        if (ch > h) {
            h = ch;
        }
    }
    return 1 + h;
}

static int seq_char_rank(char c) {
    if (c >= 'a' && c <= 'z') {
        return c - 'a';
    }
    if (c >= 'A' && c <= 'Z') {
        return 26 + (c - 'A');
    }
    return 64;
}

static int cmp_code(const void *a, const void *b) {
    const Code *x = a;
    const Code *y = b;
    if (x->length != y->length) {
        return x->length - y->length;
    }
    for (int i = 0;; i++) {
        unsigned char cx = (unsigned char)x->seq[i];
        unsigned char cy = (unsigned char)y->seq[i];
        if (!cx && !cy) {
            return 0;
        }
        int rx = seq_char_rank((char)cx);
        int ry = seq_char_rank((char)cy);
        if (rx != ry) {
            return rx - ry;
        }
    }
}

static void collect_codes(const NTypeNode *n, unsigned char *turns, int depth,
                          Code **codes, int *count, int *cap) {
    if (!n) {
        return;
    }
    if (n->symbol) {
        if (*count == *cap) {
            *cap = *cap ? *cap * 2 : 8;
            *codes = realloc(*codes, sizeof(**codes) * (size_t)*cap);
            if (!*codes) {
                fprintf(stderr, "out of memory\n");
                exit(1);
            }
        }
        char *seq = xmalloc((size_t)depth + 1);
        format_seq(n, turns, depth, seq);
        (*codes)[*count].symbol = n->symbol;
        (*codes)[*count].seq = seq;
        (*codes)[*count].length = depth;
        (*count)++;
    }
    int arity = arity_of(n);
    for (int s = 0; s < arity; s++) {
        turns[depth] = (unsigned char)s;
        collect_codes(child_at(n, s), turns, depth + 1, codes, count, cap);
    }
}

static void gather(const NTypeNode *root, Code **codes, int *count) {
    *codes = NULL;
    *count = 0;
    if (!root) {
        return;
    }
    int cap = 0;
    int h = tree_height(root);
    unsigned char *turns = xmalloc((size_t)h + 1);
    collect_codes(root, turns, 0, codes, count, &cap);
    free(turns);
    if (*count > 1) {
        qsort(*codes, (size_t)*count, sizeof(**codes), cmp_code);
    }
}

static void free_codes(Code *codes, int count) {
    for (int i = 0; i < count; i++) {
        free(codes[i].seq);
    }
    free(codes);
}

static void print_buttons_legend(const NTypeNode *root) {
    int nb = root->n_buttons;
    printf("%d button%s (", nb, nb == 1 ? "" : "s");
    if (nb == 1) {
        fputs("a", stdout);
    } else {
        printf("a-%c", 'a' + nb - 1);
    }
    fputs("): lowercase = click, uppercase = hold\n\n", stdout);
}

void print_ntype_codes(const NTypeNode *root) {
    Code *codes;
    int count;
    gather(root, &codes, &count);

    int max_sym = 6;
    int max_seq = 4;
    for (int i = 0; i < count; i++) {
        const char *seq = codes[i].seq[0] ? codes[i].seq : "(root)";
        int sl = (int)strlen(codes[i].symbol);
        int ql = (int)strlen(seq);
        if (sl > max_sym) {
            max_sym = sl;
        }
        if (ql > max_seq) {
            max_seq = ql;
        }
    }

    print_buttons_legend(root);
    printf("%-*s  %-*s  len\n", max_sym, "symbol", max_seq, "code");
    for (int i = 0; i < max_sym; i++) {
        putchar('-');
    }
    fputs("  ", stdout);
    for (int i = 0; i < max_seq; i++) {
        putchar('-');
    }
    fputs("  ---\n", stdout);

    int total = 0;
    for (int i = 0; i < count; i++) {
        const char *seq = codes[i].seq[0] ? codes[i].seq : "(root)";
        printf("%-*s  %-*s  %d\n", max_sym, codes[i].symbol, max_seq, seq,
               codes[i].length);
        total += codes[i].length;
    }
    if (count > 0) {
        printf("\n%d symbol%s, mean length %.2f (equal frequency)\n", count,
               count == 1 ? "" : "s", (double)total / (double)count);
    }
    free_codes(codes, count);
}

static void json_escape(FILE *fp, const char *s) {
    fputc('"', fp);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            fputc('\\', fp);
            fputc(c, fp);
        } else if (c == '\n') {
            fputs("\\n", fp);
        } else if (c == '\t') {
            fputs("\\t", fp);
        } else if (c < 32) {
            fprintf(fp, "\\u%04x", c);
        } else {
            fputc(c, fp);
        }
    }
    fputc('"', fp);
}

static void indent(FILE *fp, int n) {
    for (int i = 0; i < n; i++) {
        fputc(' ', fp);
    }
}

static void emit_node(FILE *fp, const NTypeNode *n, int ind) {
    if (!n) {
        fputs("null", fp);
        return;
    }
    fputs("{\n", fp);
    indent(fp, ind + 2);
    fputs("\"symbol\": ", fp);
    if (n->symbol) {
        json_escape(fp, n->symbol);
    } else {
        fputs("null", fp);
    }

    int arity = arity_of(n);
    for (int s = 0; s < arity; s++) {
        NTypeNode *c = child_at(n, s);
        if (!c) {
            continue;
        }
        fputs(",\n", fp);
        indent(fp, ind + 2);
        fputc('"', fp);
        fputc(slot_char(n, s), fp);
        fputs("\": ", fp);
        emit_node(fp, c, ind + 2);
    }
    fputc('\n', fp);
    indent(fp, ind);
    fputc('}', fp);
}

int export_ntype_json(const NTypeNode *root, FILE *fp) {
    if (!fp || !root) {
        return -1;
    }

    Code *codes;
    int count;
    gather(root, &codes, &count);

    int nb = root->n_buttons;
    fputs("{\n", fp);
    fputs("  \"encoding\": {\n", fp);
    fprintf(fp, "    \"buttons\": %d,\n", nb);
    fputs("    \"keys\": \"", fp);
    for (int i = 0; i < nb; i++) {
        fputc('a' + i, fp);
    }
    fputs("\",\n", fp);
    fputs("    \"click\": \"lowercase\",\n", fp);
    fputs("    \"hold\": \"uppercase\"\n", fp);
    fputs("  },\n", fp);
    fputs("  \"tree\": ", fp);
    emit_node(fp, root, 2);
    fputs(",\n", fp);
    fputs("  \"codes\": [\n", fp);
    for (int i = 0; i < count; i++) {
        indent(fp, 4);
        fputs("{ \"symbol\": ", fp);
        json_escape(fp, codes[i].symbol);
        fputs(", \"seq\": ", fp);
        json_escape(fp, codes[i].seq);
        fprintf(fp, ", \"length\": %d }%s\n", codes[i].length,
                i + 1 < count ? "," : "");
    }
    fputs("  ]\n", fp);
    fputs("}\n", fp);

    free_codes(codes, count);
    return 0;
}
