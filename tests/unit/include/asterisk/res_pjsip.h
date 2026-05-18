/*
 * Test-only stub of asterisk/res_pjsip.h. The body-builder helper
 * under test (cisco_blf_build_pidf) only needs res_pjsip.h as a
 * transitive include path — the symbols it actually references
 * (ast_sip_sanitize_xml) live in res_pjsip_presence_xml.h. We keep
 * this as an empty stub so the source's #include line resolves;
 * picking up nothing else from the real header is intentional.
 */

#ifndef TEST_SHIM_ASTERISK_RES_PJSIP_H
#define TEST_SHIM_ASTERISK_RES_PJSIP_H
#endif
