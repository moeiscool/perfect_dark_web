#include <stdlib.h>
#include <string.h>
#include "json.h"

static const char *ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
		p++;
	}
	return p;
}

static const char *skipString(const char *p)
{
	p++;
	while (*p && *p != '"') {
		if (*p == '\\' && p[1]) {
			p++;
		}
		p++;
	}
	return *p ? p + 1 : p;
}

static const char *skipValue(const char *p)
{
	p = ws(p);
	if (*p == '"') {
		return skipString(p);
	}
	if (*p == '{' || *p == '[') {
		int depth = 0;
		while (*p) {
			if (*p == '"') {
				p = skipString(p);
				continue;
			}
			if (*p == '{' || *p == '[') {
				depth++;
			} else if ((*p == '}' || *p == ']') && --depth == 0) {
				return p + 1;
			}
			p++;
		}
		return p;
	}
	while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') {
		p++;
	}
	return p;
}

int jsonString(const char *p, char *out, int size)
{
	int len = 0;
	if (!p || *(p = ws(p)) != '"') {
		if (size > 0) {
			out[0] = '\0';
		}
		return 0;
	}
	p++;
	while (*p && *p != '"') {
		char c = *p++;
		if (c == '\\' && *p) {
			c = *p++;
			if (c == 'n' || c == 'r' || c == 't') {
				c = ' ';
			} else if (c == 'u') {
				for (int i = 0; i < 4 && *p; i++) {
					p++;
				}
				c = '?';
			}
		}
		if (len < size - 1) {
			out[len++] = c;
		}
	}
	if (size > 0) {
		out[len] = '\0';
	}
	return 1;
}

int64_t jsonInt(const char *p, int64_t def)
{
	if (!p) {
		return def;
	}
	p = ws(p);
	if (*p == '-' || (*p >= '0' && *p <= '9')) {
		return strtoll(p, NULL, 10);
	}
	if (!strncmp(p, "true", 4)) {
		return 1;
	}
	if (!strncmp(p, "false", 5)) {
		return 0;
	}
	if (*p == '"') {
		char tmp[32];
		jsonString(p, tmp, sizeof(tmp));
		return tmp[0] ? strtoll(tmp, NULL, 10) : def;
	}
	return def;
}

int jsonBool(const char *p)
{
	return p && jsonInt(p, 0) != 0;
}

int jsonIsNull(const char *p)
{
	return !p || !strncmp(ws(p), "null", 4);
}

const char *jsonGet(const char *p, const char *key)
{
	char name[64];
	if (!p || *(p = ws(p)) != '{') {
		return NULL;
	}
	p = ws(p + 1);
	while (*p == '"') {
		jsonString(p, name, sizeof(name));
		p = ws(skipString(p));
		if (*p != ':') {
			return NULL;
		}
		p = ws(p + 1);
		if (!strcmp(name, key)) {
			return p;
		}
		p = ws(skipValue(p));
		if (*p == ',') {
			p = ws(p + 1);
		}
	}
	return NULL;
}

const char *jsonFirst(const char *p)
{
	if (!p || *(p = ws(p)) != '[') {
		return NULL;
	}
	p = ws(p + 1);
	return (*p == ']' || !*p) ? NULL : p;
}

const char *jsonNext(const char *p)
{
	p = ws(skipValue(p));
	if (*p != ',') {
		return NULL;
	}
	return ws(p + 1);
}

int jsonQuote(char *out, int size, const char *s)
{
	int len = 0;
	if (size < 3) {
		return 0;
	}
	out[len++] = '"';
	while (*s && len < size - 3) {
		const unsigned char c = (unsigned char)*s++;
		if (c == '"' || c == '\\') {
			out[len++] = '\\';
			out[len++] = (char)c;
		} else if (c >= 0x20 && c < 0x7f) {
			out[len++] = (char)c;
		}
	}
	out[len++] = '"';
	out[len] = '\0';
	return len;
}
