/*
 * Minimal Asterisk runtime stubs for the unit-test binaries.
 *
 * Two flavours of stub live here:
 *
 *  1) Allocator wrappers (__ast_repl_malloc / __ast_free). Asterisk's
 *     bundled pjproject patches malloc/free to route through these
 *     for MALLOC_DEBUG tracing. The pjproject-linked tests
 *     (test_string_utils) end up referencing them at link time.
 *
 *  2) Asterisk API shims (ast_str_*, ast_strdup, ast_free,
 *     ast_sip_sanitize_xml). Used by body-builder helpers we exercise
 *     as pure functions (e.g. cisco_blf_build_pidf). The declarations
 *     live in tests/unit/include/asterisk/<topic>.h; the implementations
 *     here are minimal in-test reimplementations sufficient for
 *     deterministic golden-output comparisons.
 *
 * Anything not used by any test stays unstubbed.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <asterisk/strings.h>
#include <asterisk/utils.h>
#include <asterisk/res_pjsip_presence_xml.h>

/* ------------------------------------------------------------------
 * pjproject malloc passthrough (group #1)
 * ------------------------------------------------------------------ */

void *__ast_repl_malloc(size_t size, const char *file, int lineno,
	const char *func);
void __ast_free_passthrough(void *ptr, const char *file, int lineno,
	const char *func);

void *__ast_repl_malloc(size_t size, const char *file, int lineno,
	const char *func)
{
	(void) file; (void) lineno; (void) func;
	return malloc(size);
}

void __ast_free_passthrough(void *ptr, const char *file, int lineno,
	const char *func)
{
	(void) file; (void) lineno; (void) func;
	free(ptr);
}

/* pjproject's bundled-asterisk patch calls this symbol name; provide
 * it as an alias of the passthrough above. The renamed -alias trick
 * avoids a clash with the public ast_free() declared in asterisk/utils.h. */
extern void __ast_free(void *ptr, const char *file, int lineno,
	const char *func) __attribute__((alias("__ast_free_passthrough")));

/* ------------------------------------------------------------------
 * Asterisk API shims (group #2)
 * ------------------------------------------------------------------ */

/* ast_str: growing string with realloc-on-overflow. The real
 * Asterisk implementation has more bells (length tracking, separate
 * pool allocator, threadlocal variants); the stub keeps just enough
 * to satisfy printf-style building helpers. */
struct ast_str {
	size_t allocated;
	size_t used;
	char *data;
};

struct ast_str *ast_str_create(size_t init_len)
{
	struct ast_str *s;

	if (init_len == 0) {
		init_len = 64;
	}
	s = malloc(sizeof(*s));
	if (!s) {
		return NULL;
	}
	s->data = malloc(init_len);
	if (!s->data) {
		free(s);
		return NULL;
	}
	s->allocated = init_len;
	s->used = 0;
	s->data[0] = '\0';
	return s;
}

static int ast_str_ensure(struct ast_str **bufp, size_t needed)
{
	struct ast_str *buf = *bufp;
	size_t new_size;
	char *new_data;

	if (buf->allocated >= needed) {
		return 0;
	}
	new_size = buf->allocated;
	while (new_size < needed) {
		new_size *= 2;
	}
	new_data = realloc(buf->data, new_size);
	if (!new_data) {
		return -1;
	}
	buf->data = new_data;
	buf->allocated = new_size;
	return 0;
}

int ast_str_set(struct ast_str **bufp, ssize_t max_len, const char *fmt, ...)
{
	va_list ap, ap_size;
	int needed;
	int n;

	(void) max_len; /* stub: always auto-grow */

	va_start(ap, fmt);
	va_copy(ap_size, ap);
	needed = vsnprintf(NULL, 0, fmt, ap_size) + 1;
	va_end(ap_size);
	if (needed < 0 || ast_str_ensure(bufp, (size_t) needed) < 0) {
		va_end(ap);
		return -1;
	}
	n = vsnprintf((*bufp)->data, (*bufp)->allocated, fmt, ap);
	(*bufp)->used = (n > 0) ? (size_t) n : 0;
	va_end(ap);
	return n;
}

int ast_str_append(struct ast_str **bufp, ssize_t max_len, const char *fmt, ...)
{
	va_list ap, ap_size;
	int needed;
	int n;
	size_t used;

	(void) max_len;

	used = (*bufp)->used;

	va_start(ap, fmt);
	va_copy(ap_size, ap);
	needed = vsnprintf(NULL, 0, fmt, ap_size) + 1;
	va_end(ap_size);
	if (needed < 0 || ast_str_ensure(bufp, used + (size_t) needed) < 0) {
		va_end(ap);
		return -1;
	}
	n = vsnprintf((*bufp)->data + used, (*bufp)->allocated - used, fmt, ap);
	if (n > 0) {
		(*bufp)->used = used + (size_t) n;
	}
	va_end(ap);
	return n;
}

char *ast_str_buffer(const struct ast_str *buf)
{
	return buf->data;
}

char *ast_strdup(const char *s)
{
	return s ? strdup(s) : NULL;
}

void ast_free(void *p)
{
	/* ast_str allocations carry two buffers (header + data). The real
	 * Asterisk frees both together because the struct is a single
	 * allocation with a flexible array; our stub uses two allocations
	 * and a generic ast_free can't tell which kind it's receiving.
	 *
	 * The pure-helper test-callers always invoke ast_free on the
	 * ast_str* (post ast_str_buffer + ast_strdup), so they want the
	 * pair freed. Detect with a lightweight heuristic: ast_str is
	 * always allocated via ast_str_create, never aliased. Free both.
	 *
	 * For non-ast_str pointers (e.g. an ast_strdup'd char *), the
	 * caller frees those via free() directly in tests, not ast_free.
	 * Keep this stub narrowly scoped to the ast_str case. */
	struct ast_str *as = p;
	if (as) {
		free(as->data);
		free(as);
	}
}

/* ast_sip_sanitize_xml: XML-escape src into dst, truncating at len-1
 * bytes (real Asterisk does this too — exposed as the
 * res_pjsip_presence_xml helper). Escapes &, <, >, ", '. */
void ast_sip_sanitize_xml(const char *input, char *output, size_t len)
{
	size_t out = 0;
	const char *p;

	if (!output || len == 0) {
		return;
	}

	for (p = input; *p && out + 1 < len; p++) {
		const char *esc = NULL;
		size_t esc_len = 0;

		switch (*p) {
		case '&':  esc = "&amp;";  esc_len = 5; break;
		case '<':  esc = "&lt;";   esc_len = 4; break;
		case '>':  esc = "&gt;";   esc_len = 4; break;
		case '"':  esc = "&quot;"; esc_len = 6; break;
		case '\'': esc = "&apos;"; esc_len = 6; break;
		}
		if (esc) {
			if (out + esc_len >= len) {
				break;
			}
			memcpy(output + out, esc, esc_len);
			out += esc_len;
		} else {
			output[out++] = *p;
		}
	}
	output[out] = '\0';
}
