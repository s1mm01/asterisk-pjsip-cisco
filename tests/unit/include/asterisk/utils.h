/*
 * Test-only stub of asterisk/utils.h — the malloc/strdup/free wrappers
 * Asterisk modules use. Real Asterisk routes these through
 * MALLOC_DEBUG-aware wrappers (__ast_repl_malloc and friends); the
 * stub uses plain malloc/strdup/free.
 */

#ifndef TEST_SHIM_ASTERISK_UTILS_H
#define TEST_SHIM_ASTERISK_UTILS_H

#include <stddef.h>

char *ast_strdup(const char *s);
void ast_free(void *p);

#endif
