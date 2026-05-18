/*
 * Test-only stub of asterisk/res_pjsip_presence_xml.h. Declares
 * ast_sip_sanitize_xml — the only symbol the body-builder helpers
 * we test actually consume from this header. Real Asterisk's
 * implementation escapes &, <, >, ", '; the stub in
 * tests/unit/asterisk_stubs.c does the same.
 */

#ifndef TEST_SHIM_ASTERISK_RES_PJSIP_PRESENCE_XML_H
#define TEST_SHIM_ASTERISK_RES_PJSIP_PRESENCE_XML_H

#include <stddef.h>

void ast_sip_sanitize_xml(const char *input, char *output, size_t len);

#endif
