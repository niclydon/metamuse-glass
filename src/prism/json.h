/* prismd: a small JSON tree parser and a compact writer. The writer emits exactly
 * what Python's json.dumps(separators=(",", ":"), ensure_ascii=False) emits for the
 * same values, so a frame prismd signs is the frame nexusd would have built; the
 * parser keeps object keys in their original order so a parsed object re-encodes
 * byte-for-byte. Numbers are kept as int64 when integral, else as double. */
#ifndef PRISM_JSON_H
#define PRISM_JSON_H
#include <stddef.h>
#include <stdint.h>

typedef enum { J_NULL, J_BOOL, J_INT, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jnode {
    jtype type;
    char *key;            /* set when this node is a member of an object */
    union {
        int b;
        int64_t i;
        double d;
        char *s;          /* NUL-terminated UTF-8 (embedded NULs are refused) */
    } v;
    struct jnode *first;  /* arrays/objects: first child */
    struct jnode *next;   /* next sibling */
    size_t count;         /* arrays/objects: number of children */
} jnode;

/* Parse len bytes; the whole input must be one value (surrounding whitespace ok).
 * Returns NULL on any syntax error, depth > 32, or a string with an embedded NUL. */
jnode *json_parse(const char *text, size_t len);
void json_free(jnode *n);
/* Object member lookup (NULL when absent or n is not an object). */
const jnode *json_get(const jnode *n, const char *key);
/* Array/object index (NULL when out of range). */
const jnode *json_at(const jnode *n, size_t i);
/* Convenience accessors: return the fallback when missing or of another type. */
const char *json_str(const jnode *n, const char *key, const char *fallback);
int64_t json_int(const jnode *n, const char *key, int64_t fallback);
int json_bool(const jnode *n, const char *key, int fallback);

/* Writer: appends to a fixed buffer, tracks commas, and sets overflow instead of
 * writing past the end. Check jw_ok() when done. */
typedef struct {
    char *buf;
    size_t cap, len;
    int overflow;
    int need_comma[33];
    int depth;
} jwriter;

void jw_init(jwriter *w, char *buf, size_t cap);
int jw_ok(const jwriter *w);                 /* 1 when nothing overflowed */
void jw_obj_begin(jwriter *w);
void jw_obj_end(jwriter *w);
void jw_arr_begin(jwriter *w);
void jw_arr_end(jwriter *w);
void jw_key(jwriter *w, const char *key);    /* inside an object, before a value */
void jw_str(jwriter *w, const char *s);
void jw_int(jwriter *w, int64_t i);
void jw_bool(jwriter *w, int b);
void jw_null(jwriter *w);
void jw_raw(jwriter *w, const char *json);   /* pre-encoded JSON, written verbatim */
/* Encode a parsed tree back to compact JSON (same conventions as the writer). */
void jw_node(jwriter *w, const jnode *n);
#endif
