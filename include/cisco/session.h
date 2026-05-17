/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Shared session helpers for res_pjsip_cisco_* modules that work
 * with active SIP dialogs. Kept separate from cisco/endpoint.h so
 * modules that only deal with sorcery / REGISTER-time supplements
 * don't pull in res_pjsip_session and channel.h.
 *
 * Bodies live in res/res/cisco_endpoint/session.c, compiled into
 * res_pjsip_cisco_endpoint.so; other cisco_* modules resolve the
 * symbols at load time.
 */

#ifndef _RES_PJSIP_CISCO_SESSION_H
#define _RES_PJSIP_CISCO_SESSION_H

#include "asterisk.h"

#include <pjsip.h>

#include "asterisk/channel.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"

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
struct ast_channel *cisco_session_channel_ref(struct ast_sip_session *session);

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
struct ast_sip_session *cisco_dialog_session_lookup(
	const char *call_id, const char *phone_local_tag,
	const char *phone_remote_tag);

/*!
 * \brief Send a REFER response with Refer-Sub: false. Handles both
 *        in-dialog (REFER inside an INVITE dialog) and out-of-dialog
 *        (Cisco RemoteCC REFERs that arrive before any dialog exists)
 *        cases.
 *
 * Out-of-dialog branch goes through Asterisk's stateful wrapper so
 * retransmissions are coalesced by the UAS transaction layer.
 */
void cisco_send_refer_response(pjsip_rx_data *rdata, int code,
	struct ast_sip_endpoint *endpoint);

struct ast_bridge;

/*!
 * \brief Mark this channel as just-joined a Cisco conference.
 *
 * Attaches (or refreshes) a small datastore on the channel carrying
 * the current wall-clock timestamp. `cisco_conf_find_last_joined`
 * later uses these timestamps to identify the most-recently-joined
 * participant for the RmLastConf softkey — chan_sip mirrors the same
 * semantics via the LIFO insertion order of its
 * `sip_conference->participants` list.
 *
 * Idempotent: re-marking a channel that already carries the
 * datastore updates the timestamp in place. No-op if \a chan is
 * NULL.
 *
 * Conference modules should call this immediately after a successful
 * `ast_bridge_move(conf, src, remote_leg, …)` — only for remote
 * participants the conference creator is bringing in, NOT for the
 * conference owner's own phone-side anchor.
 */
void cisco_conf_mark_joined(struct ast_channel *chan);

/*!
 * \brief Find the most-recently-joined participant in a bridge.
 *
 * Walks the bridge's channel snapshot, picks the one with the
 * highest `cisco_conf_mark_joined` timestamp, optionally skipping
 * \a exclude (the channel that pressed the softkey — typically the
 * conference owner's phone-side leg, which should not remove itself).
 *
 * Untracked channels (no `cisco_conf_mark_joined` datastore) are
 * ignored — they presumably joined this bridge via a stock path
 * (ConfBridge, plain transfer) that the Cisco conference module
 * never saw, and treating them as candidates would surprise the
 * user.
 *
 * \retval ref-bumped channel; caller must \c ast_channel_unref()
 * \retval NULL when the bridge contains no tracked candidates
 */
struct ast_channel *cisco_conf_find_last_joined(struct ast_bridge *bridge,
	struct ast_channel *exclude);

#endif /* _RES_PJSIP_CISCO_SESSION_H */
