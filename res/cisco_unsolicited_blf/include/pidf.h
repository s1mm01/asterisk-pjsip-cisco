/*
 * Cisco-flavoured PIDF body builder for res_pjsip_cisco_unsolicited_blf.
 *
 * Separated from the module entry point so tests/unit/test_blf_pidf.c
 * can exercise it as a pure function (given inputs, the returned XML
 * is deterministic). The runtime caller in
 * res_pjsip_cisco_unsolicited_blf.c invokes it once per outbound
 * unsolicited NOTIFY.
 *
 * The body shape mirrors res_pjsip_cisco_pidf_body_generator + the
 * chan_sip patch's channels/sip/request.c:549-583 — do not edit the
 * wire format without cross-referencing those.
 */

#ifndef CISCO_UNSOLICITED_BLF_PIDF_H
#define CISCO_UNSOLICITED_BLF_PIDF_H

/*
 * Build a Cisco-flavoured PIDF body for the given extension state.
 *
 *   exten / domain   - SIP URI components for the watched extension.
 *                       Both are XML-escaped internally.
 *   exten_state      - AST_EXTENSION_* bitmask (devicestate.h).
 *   presence_state   - AST_PRESENCE_* enum value (presencestate.h);
 *                       AST_PRESENCE_DND emits the Cisco-private
 *                       <ce:dnd/> activity.
 *
 * Returns a newly-allocated string (ast_strdup'd; caller frees with
 * ast_free), or NULL on allocation failure.
 */
char *cisco_blf_build_pidf(const char *exten, const char *domain,
	int exten_state, int presence_state);

#endif /* CISCO_UNSOLICITED_BLF_PIDF_H */
