/*
 * Test-only stub of asterisk/strings.h — declares the ast_str API
 * surface needed by body-building helpers we exercise from
 * tests/unit/. The matching implementations live in
 * tests/unit/asterisk_stubs.c.
 *
 * Real Asterisk's ast_str_set / ast_str_append are inline-helper
 * macros that route through __ast_str_helper. We expose them as
 * plain variadic functions here — semantically equivalent at every
 * call site that uses them as `ast_str_set(&buf, 0, "fmt", ...)`.
 */

#ifndef TEST_SHIM_ASTERISK_STRINGS_H
#define TEST_SHIM_ASTERISK_STRINGS_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

struct ast_str;

struct ast_str *ast_str_create(size_t init_len);
int ast_str_set(struct ast_str **buf, ssize_t max_len, const char *fmt, ...);
int ast_str_append(struct ast_str **buf, ssize_t max_len, const char *fmt, ...);
char *ast_str_buffer(const struct ast_str *buf);

#endif
