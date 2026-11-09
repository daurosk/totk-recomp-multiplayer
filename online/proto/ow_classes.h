/* The class table of the actors add-on: what is shared, as what, and under which owner rules.
 *
 * The rows are online/proto/classes.txt, compiled in as text (ow_classes_data.h, written by tools/gen_classes.py)
 * and read once on first use, so the game and the server decide from the same table. docs/online-wire.md, "The
 * actors add-on", says what each column means to the two ends.
 */
#ifndef OW_CLASSES_H
#define OW_CLASSES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OW_CLASS_PATTERN_MAX 47
#define OW_CLASS_PARTS_MAX 4
#define OW_CLASS_ROWS_MAX 512
/* class_ of a row whose things are never shared. */
#define OW_CLASS_LOCAL 255

/* How a row's actors are identified (bits of ow_class_row.ids). */
enum { OW_IDS_MAP = 1, OW_IDS_CREATOR = 2, OW_IDS_LEADER = 4 };

/* A row's owner rules (bits of ow_class_row.rules); docs/online-wire.md numbers the rules they change. */
enum {
    OW_CR_STILL = 1 << 0,
    OW_CR_FIGHT = 1 << 1,
    OW_CR_LOCK = 1 << 2,
    OW_CR_PART = 1 << 3,
    OW_CR_HELD = 1 << 4,
    OW_CR_FIXED = 1 << 5,
    OW_CR_QUEST = 1 << 6,
    OW_CR_CLUSTER = 1 << 7,
    OW_CR_CORE = 1 << 8,
    OW_CR_TAKE = 1 << 9,
};

typedef struct ow_class_row {
    char pattern[OW_CLASS_PATTERN_MAX + 1];
    uint8_t contains; /* 1: the pattern was *TEXT, matched anywhere in the name */
    uint8_t class_;   /* OW_CLASS_*, or OW_CLASS_LOCAL */
    uint8_t ids;      /* OW_IDS_* bits; 0 for a local row */
    uint16_t comps;   /* bit k set: component kind k */
    uint32_t rules;   /* OW_CR_* bits */
    uint8_t nparts;   /* the body prefixes of part=P|Q */
    char parts[OW_CLASS_PARTS_MAX][OW_CLASS_PATTERN_MAX + 1];
    int line;         /* line in classes.txt */
} ow_class_row;

/* Reads a table in the format of classes.txt into rows (at most max). Returns the number of rows, or -1 with a message
 * naming the line in err. */
int ow_classes_parse(const char *text, ow_class_row *rows, int max, char *err, size_t errn);

/* The row that decides for name from the built-in table: a matching *TEXT row before any prefix row, then the
 * longest pattern, the first of equals.
 * NULL when no row matches. A row whose class_ is OW_CLASS_LOCAL means: not shared. */
const ow_class_row *ow_class_find(const char *name);
/* The same over a table of the caller's (tests, the replay lab). */
const ow_class_row *ow_class_find_in(const ow_class_row *rows, int n, const char *name);
/* Whether name is shared: a row matches and it is not local. */
int ow_class_shared(const char *name);
/* The built-in table, read on first use; *n gets its row count. */
const ow_class_row *ow_classes_builtin(int *n);

/* The hash of an actor or animation name wherever the wire carries one (herd_row.name_hash, anim_slot.anim, the gear
 * component): FNV-1a 32 over the name's UTF-8 bytes, without the terminating NUL. */
uint32_t ow_name_hash(const char *name);
/* The herd cell of a position (herd.cell): the 250 m grid, column floor(x / 250) in the low 16 bits and row
 * floor(z / 250) in the high 16 bits, each as a signed 16-bit number. A label only: it is not the map section of
 * place.cell_x and cell_y. */
#define OW_HERD_CELL_M 250.0f
uint32_t ow_herd_cell(float x, float z);

#ifdef __cplusplus
}
#endif
#endif
