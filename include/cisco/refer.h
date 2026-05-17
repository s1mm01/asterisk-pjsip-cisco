/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Out-of-band Cisco RemoteCC REFER + multipart-body sending helpers
 * for the res_pjsip_cisco_* family.
 *
 * Cisco unsolicited REFERs (bulkupdate, HLog state push, MCID feedback,
 * ConfList menu) all share the same fan-out: walk every AOR on the
 * endpoint, fetch each AOR's registered contacts, build one REFER per
 * contact with a Content-ID-keyed body. The conference / park /
 * phone-initiated paths instead target a single contact (or the contact
 * resolved from the inbound rdata's source IP:port). Both shapes live
 * here.
 *
 * Lives separately from cisco/endpoint.h so modules that only deal with
 * sorcery / REGISTER-time supplements don't compile in the multipart
 * machinery — and so the multipart-add helper sits next to the REFER
 * sender that drives it.
 *
 * Bodies live in res/res/cisco_endpoint/refer.c, compiled into
 * res_pjsip_cisco_endpoint.so; other cisco_* modules resolve the
 * symbols at load time.
 *
 * Depends on cisco/endpoint.h transitively (the helpers take
 * struct ast_sip_endpoint *), but does not require cisco/rdata.h.
 */

#ifndef _RES_PJSIP_CISCO_REFER_H
#define _RES_PJSIP_CISCO_REFER_H

#include "asterisk.h"

#include <pjlib.h>
#include <pjsip.h>

#include "asterisk/res_pjsip.h"

/*!
 * \name REFER fan-out to all registered contacts
 *
 * Cisco unsolicited REFERs (bulkupdate, HLog state push, MCID feedback,
 * ConfList menu) all share the same loop: walk every AOR on the
 * endpoint, fetch each AOR's registered contacts, build one REFER per
 * contact with a Content-ID-keyed body. Lift the scaffolding here so
 * the call sites collapse to: (1) define a ctx struct, (2) write a
 * one-line body-builder adapter, (3) call this helper.
 *
 * The per-iter log is uniform: "<prefix>: <subject> sent to <uri>".
 * If a caller wants additional context (endpoint_id, state flags) on
 * the success line, log it once before the helper call at the
 * task-start boundary.
 */
/* @{ */

typedef pjsip_msg_body *(*cisco_refer_body_builder)(pj_pool_t *pool, void *ctx);

/*!
 * \brief Send one Cisco RemoteCC REFER to a single registered contact.
 *
 * Same body-builder convention as cisco_endpoint_send_refer_to_all_contacts
 * (which uses this helper). Use this when the REFER is the response to
 * a phone-initiated request (e.g. ConfList) where only the originating
 * contact should be answered — sending to all contacts of a shared-line
 * AOR would render menus on every phone that shares the line, which
 * Cisco firmware presents as "did someone else press this?" confusion.
 *
 * \retval 0  ast_sip_send_request returned success.
 * \retval -1 create/build/send failure (already logged).
 */
int cisco_endpoint_send_refer_to_contact(
	struct ast_sip_endpoint *endpoint, struct ast_sip_contact *contact,
	const char *log_prefix, const char *cid_suffix, const char *subject,
	cisco_refer_body_builder build, void *ctx);

/*!
 * \brief Send one REFER per registered contact across every AOR on
 *        \a endpoint, with the body produced by \a build(pool, ctx).
 *
 * \param endpoint       endpoint to fan out across
 * \param log_prefix     module log tag, e.g. "cisco-bulkupdate"
 * \param cid_suffix     trailing @suffix on the Content-ID, e.g.
 *                       "cisco-bulkupdate" — phones don't care about
 *                       the value, only that it matches Refer-To
 * \param subject        short label for log lines, e.g. "REFER" or
 *                       "HLog update"
 * \param build          body-builder callback (called per contact;
 *                       returning NULL aborts that contact, increments
 *                       attempted but not succeeded)
 * \param ctx            opaque builder ctx (passed through unchanged)
 * \param attempted_out  optional; total contacts the loop attempted
 * \param succeeded_out  optional; subset where ast_sip_send_request
 *                       returned success
 *
 * Both counter pointers may be NULL — callers that don't gate a cache
 * commit on all-or-nothing don't need them.
 */
void cisco_endpoint_send_refer_to_all_contacts(
	struct ast_sip_endpoint *endpoint,
	const char *log_prefix, const char *cid_suffix, const char *subject,
	cisco_refer_body_builder build, void *ctx,
	int *attempted_out, int *succeeded_out);

/*!
 * \brief Find the registered contact whose URI host:port matches the
 *        inbound rdata's source IP:port.
 *
 * For ConfList / Park / any phone-initiated RemoteCC REFER we want to
 * answer only the contact that asked, not fan-out across every
 * registered binding on a shared-line AOR. The match is a substring
 * check: Cisco's REGISTER Contact URI carries the phone's host:port
 * verbatim (e.g. "sip:6003@192.168.18.119:49927;user=phone;…") and the
 * rdata exposes the live source IP:port via pkt_info, so the two
 * line up exactly for v4/v6 alike. Bracket form ([::1]:5060) survives
 * substring match cleanly too.
 *
 * \retval contact with +1 ref (caller ao2_cleanups), or NULL on no match.
 */
struct ast_sip_contact *cisco_endpoint_find_contact_from_rdata(
	struct ast_sip_endpoint *endpoint, pjsip_rx_data *rdata);
/* @} */

/*!
 * \brief Add one application/x-cisco-remotecc-request+xml part to an
 *        existing multipart/mixed body.
 *
 * The XML payload is duplicated into \a pool so the caller can free
 * its source buffer immediately. Used by bulkupdate and remotecc; if
 * a third multipart consumer shows up, this is the shared call site.
 *
 * pjsip's multipart API requires using its own multipart_print_body
 * callback for any multipart subtype — hand-rolling boundaries trips
 * an internal assert in pjproject's transport layer. That's why we
 * go through pjsip_multipart_create_part / add_part rather than
 * just appending to a string.
 */
void cisco_remotecc_multipart_add_part(pj_pool_t *pool,
	pjsip_msg_body *multipart, const char *xml);

#endif /* _RES_PJSIP_CISCO_REFER_H */
