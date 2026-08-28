/*
 * Example of using ntype's exported JSON (ntype.json) in another program.
 *
 *   cc -Wall -Wextra -O2 -o ntype-demo src/demo.c $(pkg-config --cflags --libs
 * x11 xft)
 *   ./ntype-demo ntype.json
 *   ./ntype-demo --dump ntype.json
 *
 * Keys listed in encoding.keys are the buttons.
 * Tap = click (lowercase), hold = hold (uppercase).
 * Esc cancels the current sequence; Backspace deletes the last symbol.
 */

#define _POSIX_C_SOURCE 200809L

#include <X11/XKBlib.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

#define PRESS_MS 280
#define MAX_EMITTED 512

enum { JV_NULL, JV_NUM, JV_STR, JV_ARR, JV_OBJ };

typedef struct JVal JVal;
struct JVal {
        int kind;
        char *s;
        long num;
        int n;
        char **keys;
        JVal **vals;
};

typedef struct {
        const char *p;
        const char *err;
} Parser;

typedef struct DemoNode DemoNode;
struct DemoNode {
        char *symbol;
        DemoNode *child[256];
};

typedef struct {
        DemoNode *root;
        DemoNode *cur;
        char *keys;
        int n_buttons;
        char *emitted[MAX_EMITTED];
        int n_emitted;
        char path[64];
        int path_len;
} Demo;

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *d = xmalloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}

static void skip_ws(Parser *ps) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' ||
           *ps->p == '\r') {
        ps->p++;
    }
}

static JVal *jnew(int kind) {
    JVal *v = xmalloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->kind = kind;
    return v;
}

static void jfree(JVal *v) {
    if (!v) {
        return;
    }
    free(v->s);
    for (int i = 0; i < v->n; i++) {
        if (v->keys) {
            free(v->keys[i]);
        }
        jfree(v->vals[i]);
    }
    free(v->keys);
    free(v->vals);
    free(v);
}

static JVal *jobj_get(JVal *o, const char *k) {
    if (!o || o->kind != JV_OBJ) {
        return NULL;
    }
    for (int i = 0; i < o->n; i++) {
        if (strcmp(o->keys[i], k) == 0) {
            return o->vals[i];
        }
    }
    return NULL;
}

static void utf8_from_cp(unsigned cp, char *out, size_t *n) {
    if (cp < 0x80) {
        out[(*n)++] = (char)cp;
    } else if (cp < 0x800) {
        out[(*n)++] = (char)(0xC0 | (cp >> 6));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[(*n)++] = (char)(0xE0 | (cp >> 12));
        out[(*n)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*n)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static JVal *parse_value(Parser *ps);

static char *parse_string_raw(Parser *ps) {
    if (*ps->p != '"') {
        ps->err = "expected string";
        return NULL;
    }
    ps->p++;
    size_t cap = 32;
    size_t n = 0;
    char *buf = xmalloc(cap);
    while (*ps->p && *ps->p != '"') {
        unsigned char c = (unsigned char)*ps->p++;
        if (n + 4 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) {
                fprintf(stderr, "out of memory\n");
                exit(1);
            }
        }
        if (c != '\\') {
            buf[n++] = (char)c;
            continue;
        }
        char e = *ps->p++;
        if (e == 'u') {
            unsigned cp = 0;
            for (int i = 0; i < 4; i++) {
                char h = *ps->p++;
                cp <<= 4;
                if (h >= '0' && h <= '9') {
                    cp |= (unsigned)(h - '0');
                } else if (h >= 'a' && h <= 'f') {
                    cp |= (unsigned)(h - 'a' + 10);
                } else if (h >= 'A' && h <= 'F') {
                    cp |= (unsigned)(h - 'A' + 10);
                } else {
                    ps->err = "bad \\u escape";
                    free(buf);
                    return NULL;
                }
            }
            utf8_from_cp(cp, buf, &n);
        } else if (e == 'n') {
            buf[n++] = '\n';
        } else if (e == 't') {
            buf[n++] = '\t';
        } else if (e == '"' || e == '\\' || e == '/') {
            buf[n++] = e;
        } else {
            buf[n++] = e;
        }
    }
    if (*ps->p != '"') {
        ps->err = "unterminated string";
        free(buf);
        return NULL;
    }
    ps->p++;
    buf[n] = '\0';
    return buf;
}

static JVal *parse_string(Parser *ps) {
    char *s = parse_string_raw(ps);
    if (!s) {
        return NULL;
    }
    JVal *v = jnew(JV_STR);
    v->s = s;
    return v;
}

static JVal *parse_number(Parser *ps) {
    char *end = NULL;
    long n = strtol(ps->p, &end, 10);
    if (end == ps->p) {
        ps->err = "bad number";
        return NULL;
    }
    ps->p = end;
    JVal *v = jnew(JV_NUM);
    v->num = n;
    return v;
}

static int parse_comma_list(Parser *ps, int obj, JVal *v) {
    skip_ws(ps);
    if ((obj && *ps->p == '}') || (!obj && *ps->p == ']')) {
        return 0;
    }
    for (;;) {
        skip_ws(ps);
        if (obj) {
            char *k = parse_string_raw(ps);
            if (!k) {
                return -1;
            }
            skip_ws(ps);
            if (*ps->p != ':') {
                ps->err = "expected ':'";
                free(k);
                return -1;
            }
            ps->p++;
            JVal *item = parse_value(ps);
            if (!item) {
                free(k);
                return -1;
            }
            v->keys = realloc(v->keys, sizeof(*v->keys) * (size_t)(v->n + 1));
            v->vals = realloc(v->vals, sizeof(*v->vals) * (size_t)(v->n + 1));
            v->keys[v->n] = k;
            v->vals[v->n] = item;
            v->n++;
        } else {
            JVal *item = parse_value(ps);
            if (!item) {
                return -1;
            }
            v->vals = realloc(v->vals, sizeof(*v->vals) * (size_t)(v->n + 1));
            v->vals[v->n] = item;
            v->n++;
        }
        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        break;
    }
    return 0;
}

static JVal *parse_value(Parser *ps) {
    skip_ws(ps);
    if (*ps->p == '"') {
        return parse_string(ps);
    }
    if (*ps->p == '{') {
        ps->p++;
        JVal *v = jnew(JV_OBJ);
        if (parse_comma_list(ps, 1, v) != 0) {
            jfree(v);
            return NULL;
        }
        skip_ws(ps);
        if (*ps->p != '}') {
            ps->err = "expected '}'";
            jfree(v);
            return NULL;
        }
        ps->p++;
        return v;
    }
    if (*ps->p == '[') {
        ps->p++;
        JVal *v = jnew(JV_ARR);
        if (parse_comma_list(ps, 0, v) != 0) {
            jfree(v);
            return NULL;
        }
        skip_ws(ps);
        if (*ps->p != ']') {
            ps->err = "expected ']'";
            jfree(v);
            return NULL;
        }
        ps->p++;
        return v;
    }
    if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9')) {
        return parse_number(ps);
    }
    if (!strncmp(ps->p, "null", 4)) {
        ps->p += 4;
        return jnew(JV_NULL);
    }
    if (!strncmp(ps->p, "true", 4)) {
        ps->p += 4;
        JVal *v = jnew(JV_NUM);
        v->num = 1;
        return v;
    }
    if (!strncmp(ps->p, "false", 5)) {
        ps->p += 5;
        return jnew(JV_NUM);
    }
    ps->err = "unexpected token";
    return NULL;
}

static JVal *parse_json(const char *text) {
    Parser ps = {text, NULL};
    JVal *v = parse_value(&ps);
    if (!v) {
        fprintf(stderr, "JSON: %s\n", ps.err ? ps.err : "parse error");
        return NULL;
    }
    return v;
}

static char *read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        perror(path);
        fclose(fp);
        return NULL;
    }
    long n = ftell(fp);
    if (n < 0) {
        perror(path);
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    char *buf = xmalloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        fprintf(stderr, "short read: %s\n", path);
        free(buf);
        fclose(fp);
        return NULL;
    }
    buf[n] = '\0';
    fclose(fp);
    return buf;
}

static int node_has_child(const DemoNode *n) {
    for (int i = 0; i < 256; i++) {
        if (n->child[i]) {
            return 1;
        }
    }
    return 0;
}

static void node_free(DemoNode *n) {
    if (!n) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        node_free(n->child[i]);
    }
    free(n->symbol);
    free(n);
}

static DemoNode *node_from_json(JVal *obj) {
    if (!obj || obj->kind != JV_OBJ) {
        return NULL;
    }
    DemoNode *n = xmalloc(sizeof(*n));
    memset(n, 0, sizeof(*n));
    JVal *sym = jobj_get(obj, "symbol");
    if (sym && sym->kind == JV_STR && sym->s[0]) {
        n->symbol = xstrdup(sym->s);
    }
    for (int i = 0; i < obj->n; i++) {
        const char *k = obj->keys[i];
        if (!k[0] || k[1] || strcmp(k, "symbol") == 0) {
            continue;
        }
        unsigned char ch = (unsigned char)k[0];
        n->child[ch] = node_from_json(obj->vals[i]);
    }
    return n;
}

static void demo_reset_path(Demo *d) {
    d->cur = d->root;
    d->path_len = 0;
    d->path[0] = '\0';
}

static void demo_clear_text(Demo *d) {
    for (int i = 0; i < d->n_emitted; i++) {
        free(d->emitted[i]);
    }
    d->n_emitted = 0;
}

static void demo_free(Demo *d) {
    node_free(d->root);
    free(d->keys);
    demo_clear_text(d);
}

static int demo_load(Demo *d, const char *path) {
    memset(d, 0, sizeof(*d));
    char *text = read_file(path);
    if (!text) {
        return -1;
    }
    JVal *root = parse_json(text);
    free(text);
    if (!root) {
        return -1;
    }
    JVal *enc = jobj_get(root, "encoding");
    JVal *keys = jobj_get(enc, "keys");
    JVal *nb = jobj_get(enc, "buttons");
    JVal *tree = jobj_get(root, "tree");
    if (!keys || keys->kind != JV_STR || !keys->s[0] || !tree) {
        fprintf(stderr, "JSON is missing encoding.keys / tree\n");
        jfree(root);
        return -1;
    }
    d->keys = xstrdup(keys->s);
    d->n_buttons = (int)strlen(d->keys);
    if (nb && nb->kind == JV_NUM && nb->num > 0) {
        d->n_buttons = (int)nb->num;
    }
    d->root = node_from_json(tree);
    jfree(root);
    if (!d->root) {
        fprintf(stderr, "failed to load tree\n");
        free(d->keys);
        return -1;
    }
    demo_reset_path(d);
    return 0;
}

static const char *hint_for(const DemoNode *cur, char ch) {
    const DemoNode *n = cur->child[(unsigned char)ch];
    if (!n) {
        return "—";
    }
    if (n->symbol && !node_has_child(n)) {
        return n->symbol;
    }
    return "…";
}

static void emit_symbol(Demo *d, const char *sym) {
    if (d->n_emitted >= MAX_EMITTED) {
        return;
    }
    d->emitted[d->n_emitted++] = xstrdup(sym);
}

static void apply_turn(Demo *d, char ch) {
    DemoNode *next = d->cur->child[(unsigned char)ch];
    if (!next) {
        demo_reset_path(d);
        return;
    }
    if (d->path_len + 1 < (int)sizeof(d->path)) {
        d->path[d->path_len++] = ch;
        d->path[d->path_len] = '\0';
    }
    d->cur = next;
    if (d->cur->symbol && !node_has_child(d->cur)) {
        emit_symbol(d, d->cur->symbol);
        demo_reset_path(d);
    }
}

static void dump_walk(const DemoNode *n, char *path, int depth) {
    if (!n) {
        return;
    }
    if (n->symbol) {
        printf("  %s  %s\n", path[0] ? path : "(root)", n->symbol);
    }
    for (int i = 0; i < 256; i++) {
        if (!n->child[i]) {
            continue;
        }
        path[depth] = (char)i;
        path[depth + 1] = '\0';
        dump_walk(n->child[i], path, depth + 1);
        path[depth] = '\0';
    }
}

static int is_button_key(const Demo *d, char k) {
    for (int i = 0; i < d->n_buttons; i++) {
        if (d->keys[i] == k) {
            return 1;
        }
    }
    return 0;
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

typedef struct {
        Display *dpy;
        Window win;
        Pixmap buf;
        GC gc;
        Visual *vis;
        XftFont *font;
        XftDraw *xd;
        XftColor xft_fg, xft_dim;
        Colormap cmap;
        unsigned long bg, fg, dim, click, hold, hold, textbg;
        int w, h;
        Atom wm_delete;
} Ui;

static unsigned long color(Ui *ui, const char *spec) {
    XColor c;
    if (!XParseColor(ui->dpy, ui->cmap, spec, &c) ||
        !XAllocColor(ui->dpy, ui->cmap, &c)) {
        return BlackPixel(ui->dpy, DefaultScreen(ui->dpy));
    }
    return c.pixel;
}

static void utf8_draw(Ui *ui, int x, int y, int dim, const char *s) {
    if (!s || !ui->xd) {
        return;
    }
    XftDrawStringUtf8(ui->xd, dim ? &ui->xft_dim : &ui->xft_fg, ui->font, x, y,
                      (const FcChar8 *)s, (int)strlen(s));
}

static int utf8_width(Ui *ui, const char *s) {
    if (!s || !s[0] || !ui->font) {
        return 0;
    }
    XGlyphInfo ext;
    XftTextExtentsUtf8(ui->dpy, ui->font, (const FcChar8 *)s, (int)strlen(s),
                       &ext);
    return ext.xOff;
}

static void fill_rect(Ui *ui, int x, int y, int w, int h, unsigned long c) {
    XSetForeground(ui->dpy, ui->gc, c);
    XFillRectangle(ui->dpy, ui->buf, ui->gc, x, y, (unsigned)w, (unsigned)h);
}

static void draw_rect(Ui *ui, int x, int y, int w, int h, unsigned long c) {
    XSetForeground(ui->dpy, ui->gc, c);
    XDrawRectangle(ui->dpy, ui->buf, ui->gc, x, y, (unsigned)w, (unsigned)h);
}

static void recreate_buf(Ui *ui) {
    if (ui->xd) {
        XftDrawDestroy(ui->xd);
        ui->xd = NULL;
    }
    if (ui->buf) {
        XFreePixmap(ui->dpy, ui->buf);
    }
    ui->buf =
        XCreatePixmap(ui->dpy, ui->win, (unsigned)ui->w, (unsigned)ui->h,
                      (unsigned)DefaultDepth(ui->dpy, DefaultScreen(ui->dpy)));
    ui->xd = XftDrawCreate(ui->dpy, ui->buf, ui->vis, ui->cmap);
}

static void typed_text(const Demo *d, char *out, size_t out_sz) {
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < d->n_emitted; i++) {
        size_t n = strlen(d->emitted[i]);
        if (used + n + 1 >= out_sz) {
            break;
        }
        memcpy(out + used, d->emitted[i], n);
        used += n;
        out[used] = '\0';
    }
}

static void draw(Ui *ui, const Demo *d, int held, int became_hold) {
    fill_rect(ui, 0, 0, ui->w, ui->h, ui->bg);
    utf8_draw(ui, 16, 28, 0, "NType demo  ·  tap = click  ·  hold = hold");
    utf8_draw(
        ui, 16, 48, 1,
        "Esc cancel sequence   Backspace delete   extra hold uses uppercase");

    int pad = 16;
    int text_y = 64;
    int text_h = 72;
    fill_rect(ui, pad, text_y, ui->w - 2 * pad, text_h, ui->textbg);
    char text[2048];
    typed_text(d, text, sizeof(text));
    if (!text[0]) {
        utf8_draw(ui, pad + 12, text_y + 44, 1, "(typed symbols appear here)");
    } else {
        utf8_draw(ui, pad + 12, text_y + 44, 0, text);
    }

    int btn_y = text_y + text_h + 20;
    int btn_h = ui->h - btn_y - 48;
    if (btn_h < 80) {
        btn_h = 80;
    }
    int n = d->n_buttons > 0 ? d->n_buttons : 1;
    int gap = 10;
    int bw = (ui->w - 2 * pad - (n - 1) * gap) / n;
    if (bw < 48) {
        bw = 48;
    }
    int half = (btn_h - 8) / 2;

    for (int i = 0; i < d->n_buttons; i++) {
        char click_ch = d->keys[i];
        char hold_ch = (char)toupper((unsigned char)click_ch);
        int x = pad + i * (bw + gap);
        int this_held = (held == click_ch);
        unsigned long c_click =
            (this_held && !became_hold) ? ui->hold : ui->click;
        unsigned long c_hold =
            (this_held && became_hold) ? ui->hold : ui->hold;

        fill_rect(ui, x, btn_y, bw, half, c_click);
        fill_rect(ui, x, btn_y + half + 8, bw, half, c_hold);
        draw_rect(ui, x, btn_y, bw, half, ui->dim);
        draw_rect(ui, x, btn_y + half + 8, bw, half, ui->dim);

        char lab[4] = {click_ch, 0};
        utf8_draw(ui, x + 10, btn_y + 22, 1, lab);
        const char *hclick = hint_for(d->cur, click_ch);
        utf8_draw(ui, x + (bw - utf8_width(ui, hclick)) / 2, btn_y + half - 12,
                  0, hclick);

        lab[0] = hold_ch;
        utf8_draw(ui, x + 10, btn_y + half + 30, 1, lab);
        const char *hhold = hint_for(d->cur, hold_ch);
        utf8_draw(ui, x + (bw - utf8_width(ui, hhold)) / 2,
                  btn_y + half + 8 + half - 12, 0, hhold);
    }

    char status[128];
    if (d->path[0]) {
        snprintf(status, sizeof(status), "sequence: %s", d->path);
    } else {
        snprintf(status, sizeof(status), "sequence: (root)");
    }
    utf8_draw(ui, pad, ui->h - 18, 1, status);

    XCopyArea(ui->dpy, ui->buf, ui->win, ui->gc, 0, 0, (unsigned)ui->w,
              (unsigned)ui->h, 0, 0);
}

static int lookup_button(XKeyEvent *ev) {
    KeySym ks = XLookupKeysym(ev, 0);
    if (ks >= XK_a && ks <= XK_z) {
        return (int)('a' + (ks - XK_a));
    }
    if (ks >= XK_A && ks <= XK_Z) {
        return (int)('a' + (ks - XK_A));
    }
    return 0;
}

static int run_x11(Demo *d) {
    Ui ui;
    memset(&ui, 0, sizeof(ui));
    ui.dpy = XOpenDisplay(NULL);
    if (!ui.dpy) {
        fprintf(stderr, "cannot open X display\n");
        return 1;
    }
    XkbSetDetectableAutoRepeat(ui.dpy, True, NULL);

    int scr = DefaultScreen(ui.dpy);
    ui.cmap = DefaultColormap(ui.dpy, scr);
    ui.w = 760;
    ui.h = 420;
    ui.win = XCreateSimpleWindow(
        ui.dpy, RootWindow(ui.dpy, scr), 80, 80, (unsigned)ui.w, (unsigned)ui.h,
        0, BlackPixel(ui.dpy, scr), BlackPixel(ui.dpy, scr));
    XStoreName(ui.dpy, ui.win, "NType demo");
    XSelectInput(ui.dpy, ui.win,
                 ExposureMask | KeyPressMask | KeyReleaseMask |
                     StructureNotifyMask);
    ui.wm_delete = XInternAtom(ui.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(ui.dpy, ui.win, &ui.wm_delete, 1);

    ui.gc = XCreateGC(ui.dpy, ui.win, 0, NULL);
    ui.vis = DefaultVisual(ui.dpy, scr);
    ui.font = XftFontOpenName(ui.dpy, scr, "sans:size=13");
    if (!ui.font) {
        ui.font = XftFontOpenName(ui.dpy, scr, "DejaVu Sans:size=13");
    }
    if (!ui.font) {
        fprintf(stderr, "cannot open Xft font\n");
        return 1;
    }
    if (!XftColorAllocName(ui.dpy, ui.vis, ui.cmap, "#ececf0", &ui.xft_fg) ||
        !XftColorAllocName(ui.dpy, ui.vis, ui.cmap, "#8a8a92", &ui.xft_dim)) {
        fprintf(stderr, "cannot allocate Xft colors\n");
        return 1;
    }
    ui.bg = color(&ui, "#1a1a1c");
    ui.fg = color(&ui, "#ececf0");
    ui.dim = color(&ui, "#8a8a92");
    ui.click = color(&ui, "#2c4458");
    ui.hold = color(&ui, "#4a3050");
    ui.hold = color(&ui, "#3d6b4a");
    ui.textbg = color(&ui, "#121214");
    recreate_buf(&ui);
    XMapWindow(ui.dpy, ui.win);

    int xfd = ConnectionNumber(ui.dpy);
    int held = 0;
    int became_hold = 0;
    long hold_start = 0;
    int running = 1;

    while (running) {
        if (!XPending(ui.dpy)) {
            if (held && !became_hold) {
                long left = PRESS_MS - (now_ms() - hold_start);
                if (left <= 0) {
                    became_hold = 1;
                    draw(&ui, d, held, became_hold);
                    XFlush(ui.dpy);
                    continue;
                }
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(xfd, &rfds);
                struct timeval tv;
                tv.tv_sec = left / 1000;
                tv.tv_usec = (left % 1000) * 1000;
                select(xfd + 1, &rfds, NULL, NULL, &tv);
                continue;
            }
        }
        XEvent ev;
        XNextEvent(ui.dpy, &ev);
        if (ev.type == Expose && ev.xexpose.count == 0) {
            draw(&ui, d, held, became_hold);
        } else if (ev.type == ConfigureNotify) {
            if (ev.xconfigure.width != ui.w || ev.xconfigure.height != ui.h) {
                ui.w = ev.xconfigure.width;
                ui.h = ev.xconfigure.height;
                if (ui.w < 320) {
                    ui.w = 320;
                }
                if (ui.h < 240) {
                    ui.h = 240;
                }
                recreate_buf(&ui);
                draw(&ui, d, held, became_hold);
            }
        } else if (ev.type == ClientMessage &&
                   (Atom)ev.xclient.data.l[0] == ui.wm_delete) {
            running = 0;
        } else if (ev.type == KeyPress) {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (ks == XK_Escape) {
                demo_reset_path(d);
                held = 0;
                draw(&ui, d, 0, 0);
            } else if (ks == XK_BackSpace) {
                if (d->n_emitted > 0) {
                    free(d->emitted[--d->n_emitted]);
                }
                demo_reset_path(d);
                held = 0;
                draw(&ui, d, 0, 0);
            } else {
                int k = lookup_button(&ev.xkey);
                if (k && is_button_key(d, (char)k) && !held) {
                    held = k;
                    became_hold = 0;
                    hold_start = now_ms();
                    draw(&ui, d, held, became_hold);
                }
            }
        } else if (ev.type == KeyRelease) {
            int k = lookup_button(&ev.xkey);
            if (held && k == held) {
                char ch =
                    (char)(became_hold ? toupper((unsigned char)held) : held);
                apply_turn(d, ch);
                held = 0;
                became_hold = 0;
                draw(&ui, d, 0, 0);
            }
        }
    }

    if (ui.xd) {
        XftDrawDestroy(ui.xd);
    }
    if (ui.buf) {
        XFreePixmap(ui.dpy, ui.buf);
    }
    XFreeGC(ui.dpy, ui.gc);
    XftColorFree(ui.dpy, ui.vis, ui.cmap, &ui.xft_fg);
    XftColorFree(ui.dpy, ui.vis, ui.cmap, &ui.xft_dim);
    XftFontClose(ui.dpy, ui.font);
    XDestroyWindow(ui.dpy, ui.win);
    XCloseDisplay(ui.dpy);
    return 0;
}

int main(int argc, char **argv) {
    int dump = 0;
    const char *path = "ntype.json";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0) {
            dump = 1;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [--dump] [ntype.json]\n", argv[0]);
            return 0;
        } else {
            path = argv[i];
        }
    }

    Demo demo;
    if (demo_load(&demo, path) != 0) {
        return 1;
    }

    if (dump) {
        printf("buttons: %d  keys: %s\n", demo.n_buttons, demo.keys);
        printf("codes loaded from JSON tree:\n");
        char pathbuf[64] = {0};
        dump_walk(demo.root, pathbuf, 0);
        demo_free(&demo);
        return 0;
    }

    int rc = run_x11(&demo);
    demo_free(&demo);
    return rc;
}
