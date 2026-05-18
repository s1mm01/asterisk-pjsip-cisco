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

/*!
 * \brief Aggregate MWI counts across the endpoint's configured mailboxes.
 *
 * Resolution order matches stock PJSIP: endpoint->subscription.mwi.mailboxes
 * if set (overrides everything); otherwise walk every AOR in endpoint->aors
 * and union their mailboxes= settings. Counts are summed by
 * ast_app_inboxcount.
 *
 * Both *new and *old are always written (zeroed on no mailboxes / no AORs /
 * NULL endpoint), so the caller doesn't have to pre-initialise.
 *
 * Used by res_pjsip_cisco_bulkupdate.so to build the <emwi> element of the
 * REGISTER-triggered bulkupdate REFER, and by 'pjsip cisco status' for
 * read-only diagnostics. Sharing it keeps the two reports byte-identical;
 * a future change to MWI resolution lands in one place.
 */
void cisco_endpoint_mwi_count(struct ast_sip_endpoint *endpoint,
	int *mwi_new, int *mwi_old);

#define CISCO_MAC_LEN 12

/*!
 * \brief Per-device facts learned at REGISTER time.
 *
 * Populated by res_pjsip_cisco_feature_events from the inbound REGISTER's
 * Contact +sip.instance / +u.sip params (for MAC + src_host + endpoint_id)
 * and Reason header (for device_name + firmware versions, when the phone
 * is configured for cisco-usecallmanager-style Reason reporting). Stored
 * in res_pjsip_cisco_endpoint.so so multiple consumers see the same data:
 *
 *  - cisco_mac_identify (PATH C — distributor lookup by MAC URI)
 *  - 'pjsip cisco status' (diagnostic dump by endpoint id)
 *
 * Lifetime: entries auto-expire ttl-seconds after registration. A
 * re-REGISTER replaces by MAC; Contact: * forgets.
 *
 * Field semantics:
 *   mac            12 lowercase hex digits.
 *   src_host       source-IP / hostname the REGISTER arrived from.
 *                  PATH C gates MAC identification on this matching the
 *                  current request's source.
 *   endpoint_id    Cisco endpoint that REGISTERed (most-recent winner
 *                  when several lines of a multi-line phone share a MAC).
 *   device_name    "SEPxxxxxxxxxxxx", from Reason "Name=...". Empty when
 *                  the phone wasn't configured for Reason reporting or
 *                  sent no Reason header at all.
 *   active_load    Running firmware version, from Reason "ActiveLoad="
 *                  (or "Load=" on older phones). Empty when unknown.
 *   inactive_load  Alternate partition's firmware version, from Reason
 *                  "InactiveLoad=". Empty when unknown.
 *   expires        Absolute timestamp; entry treated as stale past this.
 *
 * Three-string Reason-header fields are stored as-parsed (no quoting,
 * no NULL — empty string when absent), matching what the chan_sip
 * cisco-usecallmanager patch's peer->cisco_devicename etc. look like.
 */
struct cisco_mac_info {
	char mac[CISCO_MAC_LEN + 1];
	char src_host[64];
	char endpoint_id[128];
	char device_name[32];
	char active_load[64];
	char inactive_load[64];
	struct timeval expires;
};

/*!
 * \brief Insert / replace by MAC. The caller's struct is copied; the
 *        container owns its data thereafter. Returns 0 on success, -1
 *        on allocation failure or empty MAC.
 */
int cisco_mac_register(const struct cisco_mac_info *info);

/*! \brief Forget the entry for \a mac (no-op if absent). */
void cisco_mac_forget(const char *mac);

/*!
 * \brief Lookup by MAC. On match, copies the entry into *out and
 *        returns 0. Returns -1 if no live (non-expired) entry exists.
 */
int cisco_mac_lookup_by_mac(const char *mac, struct cisco_mac_info *out);

/*!
 * \brief Lookup by endpoint id. On match, copies the live (non-
 *        expired) entry into *out and returns 0; -1 on no match.
 *
 * Walk is alias-aware in one direction: if the direct lookup
 * (entry->endpoint_id == \a endpoint_id) misses, the queried
 * endpoint's cisco object's aliases= field is consulted and each
 * alias is tried in turn. This handles the multi-line-phone case
 * where the queried endpoint is the primary but one of its sibling
 * lines won the most-recent REGISTER (the MAC-keyed slot stores
 * the alias's id, not the primary's, but the device facts apply to
 * both). The returned struct's endpoint_id field will name the
 * matched alias rather than the queried primary — compare against
 * \a endpoint_id to detect the indirection.
 *
 * The reverse direction (\a endpoint_id is itself an alias of some
 * primary Y) is NOT walked — that would need an O(N_endpoints)
 * scan of every cisco object's aliases= per status lookup. Operator
 * workaround: query the primary, which has the aliases= field set.
 */
int cisco_mac_lookup_by_endpoint(const char *endpoint_id,
	struct cisco_mac_info *out);

#endif /* _RES_PJSIP_CISCO_ENDPOINT_H */
