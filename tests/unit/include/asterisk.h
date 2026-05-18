/*
 * Umbrella shim for unit tests. Real asterisk.h pulls in module
 * machinery (AST_MODULE_INFO, sorcery wiring) and global typedefs
 * we don't need to test a pure body-building helper. Empty file is
 * sufficient for code that only consumes the ast_str_* / utils
 * APIs declared via the per-subsystem stubs in this directory.
 */
