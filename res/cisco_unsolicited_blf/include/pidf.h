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
 * Activity bitmask: the set of <e:activities> elements (plus the basic
 * open/closed status) that cisco_blf_build_pidf would emit for the
 * given (exten_state, presence_state) pair. Used by the state-change
 * dedup gate in res_pjsip_cisco_unsolicited_blf.c to skip a NOTIFY
 * whose wire body would be identical to the last one sent for the
 * same (endpoint, exten, contact) triple — Asterisk fires the
 * extension_state callback for transitions that don't change what
 * BLF watchers see (e.g. INUSE → INUSE|RINGING when alerting is
 * suppressed by the engaged-line rule).
 *
 * Single source of truth: cisco_blf_build_pidf() consumes the same
 * helper so the dedup gate and the body it gates can never disagree.
 */
enum cisco_blf_activity_bit {
	CISCO_BLF_BIT_ALERTING     = 1u << 0, /* <ce:alerting/> */
	CISCO_BLF_BIT_ON_THE_PHONE = 1u << 1, /* <e:on-the-phone/> */
	CISCO_BLF_BIT_BUSY         = 1u << 2, /* <e:busy/> */
	CISCO_BLF_BIT_DND          = 1u << 3, /* <ce:dnd/> */
	CISCO_BLF_BIT_BASIC_OPEN   = 1u << 4, /* <basic>open</basic> */
};

unsigned int cisco_blf_activity_bits(int exten_state, int presence_state);

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
