/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Foundation header for the res_pjsip_cisco_* family of modules.
 *
 * Declares the 'cisco' sorcery struct, the cisco_endpoint_get accessor,
 * and the astdb-backed feature-state accessors (DND, HuntGroup,
 * call-forward-all) that the dialplan, feature-event SUBSCRIBE handler,
 * bulkupdate, and PIDF body builders all share as their single source
 * of truth. Bodies live in res/res/cisco_endpoint/endpoint.c, compiled into
 * res_pjsip_cisco_endpoint.so; other cisco_* modules resolve the
 * symbols at load time via the dynamic symbol table (the endpoint
 * module is loaded with AST_MODFLAG_GLOBAL_SYMBOLS — same pattern stock
 * res_pjsip uses to export ast_sip_*).
 *
 * URI / XML / rdata utilities live in cisco/rdata.h; REGISTER 200-OK
 * address-change tracking in cisco/register.h; OOB REFER + multipart
 * sending in cisco/refer.h; session / dialog lookup in cisco/session.h.
 * Each of those includes this one (or transitively does so) for the
 * cisco_endpoint_get gating check.
 *
 * pjsip.conf usage:
 *
 *     [1010]
 *     type           = cisco
 *     line_index     = 1
 *     subscribe      = 1001,1002,1003,1004,1005,1006,1007
 *
 * The section name MUST match the endpoint section name. The presence
 * of a [name] type=cisco section is the gating signal — endpoints
 * without one are treated as non-Cisco and the cisco_* modules leave
 * them alone.
 */

#ifndef _RES_PJSIP_CISCO_ENDPOINT_H
#define _RES_PJSIP_CISCO_ENDPOINT_H

#include "asterisk.h"

#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"
#include "asterisk/stringfields.h"

struct cisco_endpoint {
	SORCERY_OBJECT(details);
	AST_DECLARE_STRING_FIELDS(
		/*! Comma-separated list of extensions whose state we push to
		 * the phone via unsolicited NOTIFY at REGISTER time. */
		AST_STRING_FIELD(subscribe);
		/*! Default context to look up subscribe= extensions in.
		 * Empty -> use the default "local_sip_phone" fallback. */
		AST_STRING_FIELD(subscribe_context);
		/*! Comma-separated list of OTHER cisco-endpoint IDs that
		 * share the same physical phone (multi-line phone, one
		 * endpoint per line button). The bulkupdate REFER emits a
		 * <contact line="N"> element for each, sourcing line_index
		 * from each alias's own cisco sorcery object. Empty
		 * (default) = single-line phone. */
		AST_STRING_FIELD(aliases);
		/*! Dialplan extension the RemoteCC Park softkey blind-transfers
		 * the call to — i.e. res_parking's parkext. Default "700";
		 * change to match res_parking.conf if a site uses a different
		 * parkext. The context is resolved per call (TRANSFER_CONTEXT
		 * chan var, else the endpoint's context). */
		AST_STRING_FIELD(parkext);
	);
	/*! Cisco line button index (1 for primary line on most phones). */
	int line_index;
	/*! When DND is enabled, reject calls (1) vs ring silently (0).
	 * This is a static preference; the on/off state itself is read
	 * at runtime from astdb (key DND/<endpoint-id>). */
	int dnd_busy;
	/*! Confrn outcome when the initiator hangs up while ≥2 other parties
	 * are still in the conference bridge. 0 = match chan_sip's default:
	 * dissolve the conference, BYE the remaining legs (single-button
	 * "I left, the call's over" mental model). 1 = persist: the
	 * remaining legs keep talking, useful for receptionist-pattern
	 * "drop in, connect them, drop out" flows. Mapped to per-bridge-
	 * channel DISSOLVE_HANGUP flag on chan_phone_a in conference_send_
	 * task. */
	int keep_conference;
};

/*!
 * \brief Retrieve cisco config for an endpoint by id.
 * \retval ao2 ref-bumped cisco_endpoint, NULL if endpoint isn't
 *         configured as Cisco.
 *
 * Caller must ao2_cleanup() the returned object.
 */
struct cisco_endpoint *cisco_endpoint_get(const char *id);

/*!
 * \name astdb feature-state accessors
 *
 * Cisco feature state (DND, hunt-group login, call-forward-all) is
 * stored in astdb so the dialplan, the feature-event SUBSCRIBE handler,
 * and the bulkupdate / PIDF body builders all share one source of
 * truth. The astdb key shape is part of the documented project
 * contract — see README.md.
 *
 * Setter convention: pass enabled=0 (or NULL/empty target for CF) to
 * delete the key rather than store a "no" value.
 */
/* @{ */

int cisco_dnd_is_enabled(const char *endpoint_id);
void cisco_dnd_set(const char *endpoint_id, int enabled);

int cisco_huntgroup_is_in(const char *endpoint_id);
void cisco_huntgroup_set(const char *endpoint_id, int enabled);

/*!
 * \brief Read the call-forward-all target into \a buf (empty when unset).
 * \return the (NUL-terminated) buf pointer, never NULL.
 */
const char *cisco_cfwd_get(const char *endpoint_id, char *buf, size_t buflen);
void cisco_cfwd_set(const char *endpoint_id, const char *target);
/* @} */

#endif /* _RES_PJSIP_CISCO_ENDPOINT_H */
