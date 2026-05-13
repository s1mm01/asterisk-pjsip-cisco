/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Shared session helpers for res_pjsip_cisco_* modules that work
 * with active SIP dialogs. Kept separate from cisco_endpoint.h so
 * modules that only deal with sorcery / REGISTER-time supplements
 * don't pull in res_pjsip_session and channel.h.
 */

#ifndef _RES_PJSIP_CISCO_SESSION_H
#define _RES_PJSIP_CISCO_SESSION_H

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_ua.h>

#include "asterisk/astobj2.h"
#include "asterisk/channel.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/strings.h"

/*!
 * \brief Ref-bumped accessor for a session's owner channel.
 *
 * Locks the session, reads session->channel under the lock, and
 * ref-bumps before unlocking — same convention stock res_pjsip_caller_id
 * (res_pjsip_caller_id.c:570) and the cisco-usecallmanager chan_sip
 * patch's MCID handler use to avoid racing with channel destruction
 * on another thread.
 *
 * \retval ref-bumped channel; caller must ast_channel_unref()
 * \retval NULL if session is NULL or has no channel (yet, or any more)
 */
static inline struct ast_channel *cisco_session_channel_ref(
	struct ast_sip_session *session)
{
	struct ast_channel *channel;

	if (!session) {
		return NULL;
	}

	ao2_lock(session);
	channel = session->channel;
	if (channel) {
		ast_channel_ref(channel);
	}
	ao2_unlock(session);

	return channel;
}

/*!
 * \brief Resolve a Cisco-XML &lt;dialogid&gt; triple to a live ast_sip_session.
 *
 * Cisco RemoteCC and Conference XML dialogids are encoded from the
 * phone's viewpoint: the XML's localtag is the phone's tag and the
 * XML's remotetag is Asterisk's tag. \c pjsip_ua_find_dialog takes the
 * tuple from PJSIP's viewpoint (local = Asterisk), so this helper does
 * the swap before calling in.
 *
 * The PJSIP dialog lookup is locked (PJ_TRUE) for the duration of the
 * session ref-bump, so the dialog can't be torn down between match and
 * the ref. The returned session survives the dialog dec_lock.
 *
 * \retval ref-bumped session; caller must \c ao2_cleanup
 * \retval NULL on missing args or no dialog match
 */
static inline struct ast_sip_session *cisco_dialog_session_lookup(
	const char *call_id, const char *phone_local_tag,
	const char *phone_remote_tag)
{
	pj_str_t call_id_pj;
	pj_str_t pj_local_tag;
	pj_str_t pj_remote_tag;
	pjsip_dialog *dlg;
	struct ast_sip_session *session;

	if (ast_strlen_zero(call_id) || ast_strlen_zero(phone_local_tag)
		|| ast_strlen_zero(phone_remote_tag)) {
		return NULL;
	}

	pj_cstr(&call_id_pj, call_id);
	pj_cstr(&pj_local_tag, phone_remote_tag);   /* swap */
	pj_cstr(&pj_remote_tag, phone_local_tag);   /* swap */

	dlg = pjsip_ua_find_dialog(&call_id_pj, &pj_local_tag, &pj_remote_tag,
		PJ_TRUE);
	if (!dlg) {
		return NULL;
	}
	session = ast_sip_dialog_get_session(dlg);
	pjsip_dlg_dec_lock(dlg);
	return session;
}

/*!
 * \brief Send a REFER response with Refer-Sub: false. Handles both
 *        in-dialog (REFER inside an INVITE dialog) and out-of-dialog
 *        (Cisco RemoteCC REFERs that arrive before any dialog exists)
 *        cases.
 *
 * Out-of-dialog branch goes through Asterisk's stateful wrapper so
 * retransmissions are coalesced by the UAS transaction layer.
 */
static inline void cisco_send_refer_response(pjsip_rx_data *rdata, int code,
	struct ast_sip_endpoint *endpoint)
{
	pjsip_dialog *dlg;
	pjsip_tx_data *tdata = NULL;

	dlg = pjsip_rdata_get_dlg(rdata);
	if (dlg) {
		if (pjsip_dlg_create_response(dlg, rdata, code, NULL, &tdata)
				!= PJ_SUCCESS) {
			return;
		}
	} else if (ast_sip_create_response(rdata, code, NULL, &tdata)) {
		return;
	}

	ast_sip_add_header(tdata, "Refer-Sub", "false");

	if (dlg) {
		pjsip_dlg_send_response(dlg, pjsip_rdata_get_tsx(rdata), tdata);
	} else {
		ast_sip_send_stateful_response(rdata, tdata, endpoint);
	}
}

#endif /* _RES_PJSIP_CISCO_SESSION_H */
