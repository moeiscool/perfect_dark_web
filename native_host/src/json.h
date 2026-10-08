#ifndef PDHOST_JSON_H
#define PDHOST_JSON_H

// Minimal JSON reading for the lobby protocol (web/net/lobby.js). Values are read in place from a
// NUL-terminated message: functions take a pointer to a value and return pointers into the text.

#include <stdint.h>

const char *jsonGet(const char *obj, const char *key);           // value of key, or NULL
int jsonString(const char *val, char *out, int size);            // 1 if it was a string
int64_t jsonInt(const char *val, int64_t def);
int jsonBool(const char *val);
int jsonIsNull(const char *val);
// iterate an array: for (p = jsonFirst(arr); p; p = jsonNext(p)) ...
const char *jsonFirst(const char *arr);
const char *jsonNext(const char *val);

// writes s as a JSON string literal into out (with quotes)
int jsonQuote(char *out, int size, const char *s);

#endif
