/* A small JSON reader for the updater (update.c): the GitHub release API's answer, the signed update manifest and the
   setup's progress events. It parses a whole text into a tree of nodes; strings come out decoded as UTF-8 (\u escapes
   and surrogate pairs included), numbers as their text. The input is untrusted: every limit is checked, nesting is
   capped, and any malformed input gives NULL. */
#ifndef UPDATER_JSON_H
#define UPDATER_JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_FALSE, J_TRUE, J_NUMBER, J_STRING, J_ARRAY, J_OBJECT } JsonType;

typedef struct JsonNode JsonNode;
struct JsonNode {
    JsonType type;
    char *key;          /* the member name inside an object, else NULL */
    char *text;         /* J_STRING: the decoded string; J_NUMBER: the number as written */
    JsonNode *child;    /* J_ARRAY, J_OBJECT: the first element */
    JsonNode *next;     /* the next element of the parent */
};

/* The tree of text[0..len), or NULL when it is not one complete JSON value. Free it with json_free. */
JsonNode *json_parse(const char *text, size_t len);
void json_free(JsonNode *node);

/* The member key of an object, or NULL (also when obj is NULL or not an object). */
const JsonNode *json_get(const JsonNode *obj, const char *key);
/* The string of a J_STRING member, or NULL. */
const char *json_str(const JsonNode *obj, const char *key);
/* A J_NUMBER member as a non-negative integer of at most 18 digits, or -1. */
long long json_uint(const JsonNode *obj, const char *key);
/* 1 for a J_TRUE member, 0 otherwise. */
int json_true(const JsonNode *obj, const char *key);

#endif
