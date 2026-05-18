/*
 * Test-only stub of asterisk/presencestate.h — the AST_PRESENCE_*
 * enum used to drive presence-state-dependent body shapes. Values
 * mirror upstream's exact ordering; AST_PRESENCE_DND in particular
 * is index 6, not a bit-flag, so the order matters.
 */

#ifndef TEST_SHIM_ASTERISK_PRESENCESTATE_H
#define TEST_SHIM_ASTERISK_PRESENCESTATE_H

enum ast_presence_state {
	AST_PRESENCE_NOT_SET = 0,
	AST_PRESENCE_UNAVAILABLE,
	AST_PRESENCE_AVAILABLE,
	AST_PRESENCE_AWAY,
	AST_PRESENCE_XA,
	AST_PRESENCE_CHAT,
	AST_PRESENCE_DND,
	AST_PRESENCE_INVALID
};

#endif
