/* The class table: reading classes.txt and finding a name's row (ow_classes.h). */
#include "ow_classes.h"

#include <stdio.h>
#include <string.h>

#include "ow_classes_data.h"
#include "ow_wire.h"

static const char *const CLASS_NAMES[] = {"map_object", "world_object", "enemy",      "boss",   "animal",
                                          "townsperson", "item",        "projectile", "puppet", "device"};
static const char *const COMP_NAMES[] = {"", "anim", "life", "gear", "mind", "ride", "glide", "aim", "held", "scale"};
static const struct {
    const char *word;
    uint32_t bit;
} RULE_NAMES[] = {{"still", OW_CR_STILL}, {"fight", OW_CR_FIGHT},     {"lock", OW_CR_LOCK},
                  {"held", OW_CR_HELD},   {"fixed", OW_CR_FIXED},     {"quest", OW_CR_QUEST},
                  {"cluster", OW_CR_CLUSTER}, {"core", OW_CR_CORE},   {"take", OW_CR_TAKE}};

#define COUNT(a) (sizeof(a) / sizeof *(a))

static int fail(char *err, size_t errn, int line, const char *what, const char *word) {
    if (err && errn) snprintf(err, errn, "classes.txt line %d: %s '%s'", line, what, word);
    return -1;
}

/* Copies the next space-separated word of *s into out (at most max bytes); 0 at the end of the line. */
static int next_word(const char **s, char *out, size_t max) {
    const char *p = *s;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t') {
        if (n + 1 >= max) return -1;
        out[n++] = *p++;
    }
    out[n] = 0;
    *s = p;
    return n > 0;
}

/* Splits list at sep and calls each(word); returns 0 or the first failure's -1. */
static int each_item(char *list, char sep, int (*each)(ow_class_row *, const char *), ow_class_row *r) {
    char *p = list;
    for (;;) {
        char *e = strchr(p, sep);
        if (e) *e = 0;
        if (!*p || each(r, p)) return -1;
        if (!e) return 0;
        p = e + 1;
    }
}

static int add_comp(ow_class_row *r, const char *w) {
    for (size_t k = 1; k < COUNT(COMP_NAMES); k++)
        if (!strcmp(w, COMP_NAMES[k])) {
            r->comps |= (uint16_t)(1u << k);
            return 0;
        }
    return -1;
}

static int add_part(ow_class_row *r, const char *w) {
    if (r->nparts >= OW_CLASS_PARTS_MAX || strlen(w) > OW_CLASS_PATTERN_MAX) return -1;
    strcpy(r->parts[r->nparts++], w);
    return 0;
}

static int add_rule(ow_class_row *r, const char *w) {
    if (!strncmp(w, "part=", 5)) {
        r->rules |= OW_CR_PART;
        char buf[OW_CLASS_PARTS_MAX * (OW_CLASS_PATTERN_MAX + 1) + 8];
        if (strlen(w + 5) >= sizeof buf) return -1;
        strcpy(buf, w + 5);
        return each_item(buf, '|', add_part, r);
    }
    for (size_t k = 0; k < COUNT(RULE_NAMES); k++)
        if (!strcmp(w, RULE_NAMES[k].word)) {
            r->rules |= RULE_NAMES[k].bit;
            return 0;
        }
    return -1;
}

static int parse_line(const char *line, int lineno, ow_class_row *r, char *err, size_t errn) {
    char word[5][256], extra[8];
    const char *s = line;
    for (int i = 0; i < 5; i++) {
        int k = next_word(&s, word[i], sizeof word[i]);
        if (k <= 0) return fail(err, errn, lineno, k < 0 ? "word too long" : "needs five columns, ends at", line);
    }
    if (next_word(&s, extra, sizeof extra)) return fail(err, errn, lineno, "more than five columns", line);
    memset(r, 0, sizeof *r);
    r->line = lineno;
    const char *pat = word[0];
    if (pat[0] == '*') r->contains = 1, pat++;
    if (!*pat || strlen(pat) > OW_CLASS_PATTERN_MAX) return fail(err, errn, lineno, "bad pattern", word[0]);
    strcpy(r->pattern, pat);

    if (!strcmp(word[1], "local")) {
        r->class_ = OW_CLASS_LOCAL;
    } else {
        size_t k = 0;
        while (k < COUNT(CLASS_NAMES) && strcmp(word[1], CLASS_NAMES[k])) k++;
        if (k == COUNT(CLASS_NAMES)) return fail(err, errn, lineno, "unknown class", word[1]);
        r->class_ = (uint8_t)k;
    }

    if (strcmp(word[2], "-")) {
        char *p = word[2];
        for (;;) {
            char *e = strchr(p, '|');
            if (e) *e = 0;
            if (!strcmp(p, "map")) r->ids |= OW_IDS_MAP;
            else if (!strcmp(p, "creator")) r->ids |= OW_IDS_CREATOR;
            else if (!strcmp(p, "leader")) r->ids |= OW_IDS_LEADER;
            else return fail(err, errn, lineno, "unknown id", p);
            if (!e) break;
            p = e + 1;
        }
    }
    if ((r->class_ == OW_CLASS_LOCAL) != (r->ids == 0))
        return fail(err, errn, lineno, "a local row has id -, every other row an id", word[2]);
    if (strcmp(word[3], "-") && each_item(word[3], ',', add_comp, r))
        return fail(err, errn, lineno, "unknown component in", word[3]);
    if (strcmp(word[4], "-") && each_item(word[4], ',', add_rule, r))
        return fail(err, errn, lineno, "unknown rule in", word[4]);
    return 0;
}

int ow_classes_parse(const char *text, ow_class_row *rows, int max, char *err, size_t errn) {
    int n = 0, lineno = 0;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[512];
        lineno++;
        if (len >= sizeof line) return fail(err, errn, lineno, "line too long", "");
        memcpy(line, p, len);
        line[len] = 0;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        for (char *c = line; *c; c++)
            if (*c == '\r') *c = ' ';
        const char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        if (*q) {
            if (n >= max) return fail(err, errn, lineno, "more rows than", "OW_CLASS_ROWS_MAX");
            if (parse_line(q, lineno, &rows[n], err, errn)) return -1;
            n++;
        }
        p = e ? e + 1 : p + len;
    }
    return n;
}

static int matches(const ow_class_row *r, const char *name) {
    if (r->contains) return strstr(name, r->pattern) != NULL;
    return !strncmp(name, r->pattern, strlen(r->pattern));
}

const ow_class_row *ow_class_find_in(const ow_class_row *rows, int n, const char *name) {
    /* A *TEXT row outranks every prefix row; within a kind the longest pattern wins, the first of equals. */
    const ow_class_row *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < n; i++)
        if (matches(&rows[i], name)) {
            size_t len = strlen(rows[i].pattern);
            int better = !best || rows[i].contains > best->contains ||
                         (rows[i].contains == best->contains && len > best_len);
            if (better) best = &rows[i], best_len = len;
        }
    return best;
}

static ow_class_row g_rows[OW_CLASS_ROWS_MAX];
static int g_nrows = -1;

const ow_class_row *ow_classes_builtin(int *n) {
    if (g_nrows < 0) {
        char err[160];
        int k = ow_classes_parse(OW_CLASSES_TEXT, g_rows, OW_CLASS_ROWS_MAX, err, sizeof err);
        /* The built-in text is checked by tests/net_proto_check.py, so a failure here is a build that skipped it:
         * share nothing rather than guess. */
        if (k < 0) fprintf(stderr, "ow_classes: %s; nothing is shared\n", err);
        g_nrows = k < 0 ? 0 : k;
    }
    if (n) *n = g_nrows;
    return g_rows;
}

const ow_class_row *ow_class_find(const char *name) {
    int n;
    const ow_class_row *rows = ow_classes_builtin(&n);
    return ow_class_find_in(rows, n, name);
}

uint32_t ow_name_hash(const char *name) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) h = (h ^ *p) * 16777619u;
    return h;
}

static uint16_t cell_axis(float v) {
    /* floor without libm, so no build needs -lm; NaN and the far outside clamp to the grid's ends */
    float q = v / OW_HERD_CELL_M;
    if (!(q >= -32768.0f)) return (uint16_t)(int16_t)-32768;
    if (q >= 32767.0f) return 32767;
    int32_t f = (int32_t)q;
    if ((float)f > q) f--;
    return (uint16_t)(int16_t)f;
}

uint32_t ow_herd_cell(float x, float z) { return (uint32_t)cell_axis(x) | (uint32_t)cell_axis(z) << 16; }

int ow_class_shared(const char *name) {
    const ow_class_row *r = ow_class_find(name);
    return r && r->class_ != OW_CLASS_LOCAL;
}
