/*
 * Test-only stub of asterisk/pbx.h — just the AST_EXTENSION_*
 * bit flags consumed by body builders. Real asterisk/pbx.h is huge
 * (dialplan engine APIs); we stub only what the helpers under test
 * actually reference. Values mirror upstream's exact constants.
 */

#ifndef TEST_SHIM_ASTERISK_PBX_H
#define TEST_SHIM_ASTERISK_PBX_H

enum {
	AST_EXTENSION_NOT_INUSE   = 0,
	AST_EXTENSION_INUSE       = 1 << 0,
	AST_EXTENSION_BUSY        = 1 << 1,
	AST_EXTENSION_UNAVAILABLE = 1 << 2,
	AST_EXTENSION_RINGING     = 1 << 3,
	AST_EXTENSION_ONHOLD      = 1 << 4
};

#endif
