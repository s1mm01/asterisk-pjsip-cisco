/*
 * Cisco Confrn + Join + RmLastConf softkey machinery: bridge-modifying
 * conference operations. Compiled into res_pjsip_cisco_conference.so.
 *
 * Confrn (3-way build):
 *   The phone has two SIP dialogs at the moment Confrn is pressed —
 *   the active call (held during consult) and the consult call. We
 *   merge their remote-side legs plus the original phone-side leg
 *   into a fresh multimix bridge, then drive Cisco's UI transition
 *   via holdretrievereq + Cisco-flavoured in-dialog NOTIFY +
 *   connected-line update carrying the \2004 display token.
 *
 * Join (multi-call merge):
 *   For each previously-Selected dialog, UNHOLD its remote peer, move
 *   it into a new multimix conference, softhangup its phone-side
 *   anchor. The active call's phone-side becomes the conference
 *   anchor; cleanup is a single OOB Join notifyreq REFER.
 *
 * RmLastConf:
 *   Walks the per-channel join-time datastore (set by
 *   cisco_conf_mark_joined in res/cisco_endpoint/session.c at every remote-leg
 *   ast_bridge_move) to find the participant with the latest
 *   timestamp, then ast_bridge_remove()s it.
 */

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_ua.h>
#include <pjlib.h>

#include "asterisk/astobj2.h"
#include "asterisk/bridge.h"
#include "asterisk/channel.h"
#include "asterisk/callerid.h"
#include "asterisk/format_cap.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/taskprocessor.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/sorcery.h"

#include "cisco/endpoint.h"
#include "cisco/rdata.h"
#include "cisco/refer.h"
#include "cisco/session.h"
#include "conference_private.h"

/* ----------------------------------------------------------------------
 * Shared: pick the bridge's video routing mode.
 *
 * Asterisk's softmix bridge defaults to AST_BRIDGE_VIDEO_MODE_NONE,
 * which drops every video RTP frame between participants. With the
 * SMART flag on a multimix bridge that auto-promotes 2-party-native
 * -> 3-party-softmix during a Confrn or Join, that default kicks in
 * the moment the third channel joins — and the only-video-capable
 * participant's stream goes black on every other participant's
 * screen. Symptom observed live on 2026-05-15: door-camera video
 * disappeared from the 8865 the instant a mobile leg was Confrn'd
 * into the call.
 *
 * Pick the mode per actual participant capability:
 *
 *   exactly one video-capable channel
 *       -> SINGLE_SRC pinned to it. Right for the door-camera-into-
 *          audio-conference case: always forward the camera feed
 *          regardless of who's talking. Other participants without
 *          a screen (a SIP trunk to a mobile) get the frames and
 *          drop them harmlessly.
 *
 *   two or more video-capable channels
 *       -> TALKER_SRC. Bridge forwards whichever video participant
 *          is currently the active audio talker — the standard
 *          multi-party video-conference behaviour Cisco SX10 /
 *          Polycom RealPresence / etc use.
 *
 *   zero video-capable channels
 *       -> TALKER_SRC. Harmless (nothing to route either way) and
 *          keeps the bridge in a defined state if a video peer
 *          arrives later via a transfer/join.
 *
 * "Video-capable" is read off the channel's negotiated native
 * formats — set during the call's SDP exchange, stable by the time
 * we're forming the conference. ast_format_cap_has_type returns 1
 * if ANY video format is present, which correctly handles
 * sendonly-camera peers (door has video in its native formats even
 * though it won't accept video back) and rejects audio-only peers
 * (mobile/trunk with no h264 in its codec list).
 */
/*!
 * \brief If \a chan advertises any video format in its native
 *        capability set, bump the running tally and remember the
 *        channel as a potential SINGLE_SRC anchor.
 *
 * Used as a per-participant probe by both conference_send_task
 * (called three times across known channel pointers) and
 * join_send_task (called incrementally inside the selected-leg
 * loop, where chan_remote_sel pointers don't survive past their
 * iteration). The tally is later consumed by apply_bridge_video_mode.
 */
static void tally_video_participant(struct ast_channel *chan,
	struct ast_channel **video_src, int *video_count)
{
	struct ast_format_cap *caps;

	if (!chan) {
		return;
	}
	caps = ast_channel_nativeformats(chan);
	if (caps && ast_format_cap_has_type(caps, AST_MEDIA_TYPE_VIDEO)) {
		*video_src = chan;
		++(*video_count);
	}
}

/*!
 * \brief Set the bridge's video routing mode based on the participant
 *        tally produced by tally_video_participant.
 *
 * exactly 1 video-capable -> SINGLE_SRC pinned to that channel.
 * 0 or 2+                  -> TALKER_SRC (follows the active audio
 *                             talker). 0 is harmless (nothing to
 *                             route) and keeps the bridge in a
 *                             defined state in case a video peer
 *                             arrives later via transfer/join.
 *
 * \a video_src must still be in the bridge at this call site —
 * the function reads its name for the log line; it doesn't take
 * an additional ref.
 */
static void apply_bridge_video_mode(struct ast_bridge *bridge,
	struct ast_channel *video_src, int video_count,
	const char *endpoint_id)
{
	if (video_count == 1) {
		ast_bridge_set_single_src_video_mode(bridge, video_src);
		ast_log(LOG_NOTICE,
			"cisco-conference: %s — bridge video mode SINGLE_SRC "
			"pinned to %s (only video-capable participant)\n",
			endpoint_id, ast_channel_name(video_src));
	} else {
		ast_bridge_set_talker_src_video_mode(bridge);
		ast_log(LOG_NOTICE,
			"cisco-conference: %s — bridge video mode TALKER_SRC "
			"(%d video-capable participants)\n",
			endpoint_id, video_count);
	}
}

/* ----------------------------------------------------------------------
 * Confrn — 3-way conference build.
 * ---------------------------------------------------------------------- */

struct conference_task_data {
	struct ast_sip_endpoint *endpoint;
	struct ast_sip_contact *contact;            /* who pressed Confrn */
	struct conference_dialog_id active_dialog;  /* <dialogid>: the live call */
	struct conference_dialog_id consult_dialog; /* <consultdialogid>: the held call */
	/* UAS dialog created from the inbound REFER, so we can send the
	 * follow-up Event:refer NOTIFY in-dialog. Cisco firmware requires
	 * the NOTIFY to be in-dialog (same Call-ID + From/To tags as the
	 * REFER) for the phone's UI to transition from "active + held" to
	 * "Conference"; an OOB NOTIFY is silently ignored. We hold a +1
	 * session count on the dialog from REFER receipt; the destructor
	 * below drops it. NULL if pjsip_dlg_create_uas_and_inc_lock failed
	 * at receipt time — in that case the merge still happens but the
	 * phone UI won't transition (better than no merge at all). */
	pjsip_dialog *dlg;
	/* Snapshot of the cisco sorcery flag at REFER-receipt time. We sample
	 * here rather than retain the cisco_endpoint ref across the task so
	 * the task body has one fewer object to manage. */
	int keep_conference;
};

/* Forward decls — definitions further down. */
static void send_holdretrieve_refer(struct conference_task_data *data);
static void send_conference_completion_notify(struct conference_task_data *data);

static void conference_task_data_destroy(void *obj)
{
	struct conference_task_data *data = obj;

	if (data->dlg) {
		pjsip_dlg_dec_session(data->dlg, &conference_module);
	}
	ao2_cleanup(data->contact);
	ao2_cleanup(data->endpoint);
}

static void send_conference_active_notify_locked(pjsip_dialog *dlg,
	const char *endpoint_id)
{
	pjsip_tx_data *notify_tdata = NULL;
	pj_str_t method_name = pj_str("NOTIFY");
	pjsip_method method;
	pj_str_t hdr_event_name    = pj_str("Event");
	pj_str_t hdr_event_val     = pj_str("refer");
	pj_str_t hdr_substate_name = pj_str("Subscription-State");
	pj_str_t hdr_substate_val  = pj_str("active;expires=60");

	pjsip_method_init_np(&method, &method_name);
	if (pjsip_dlg_create_request(dlg, &method, -1, &notify_tdata)
			!= PJ_SUCCESS) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — pjsip_dlg_create_request failed "
			"for active refer NOTIFY\n", endpoint_id);
		return;
	}

	pjsip_msg_add_hdr(notify_tdata->msg,
		(pjsip_hdr *) pjsip_generic_string_hdr_create(
			notify_tdata->pool, &hdr_event_name, &hdr_event_val));
	pjsip_msg_add_hdr(notify_tdata->msg,
		(pjsip_hdr *) pjsip_generic_string_hdr_create(
			notify_tdata->pool, &hdr_substate_name,
			&hdr_substate_val));

	if (pjsip_dlg_send_request(dlg, notify_tdata, -1, NULL)
			!= PJ_SUCCESS) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — pjsip_dlg_send_request failed "
			"for active refer NOTIFY\n", endpoint_id);
		return;
	}

	ast_log(LOG_NOTICE,
		"cisco-conference: %s — active refer NOTIFY sent\n",
		endpoint_id);
}

/*!
 * \brief Create a UAS dialog from the REFER's rdata, send the 202 Accepted
 *        in that dialog, and return the dialog with a held session count.
 *
 * Cisco firmware requires the follow-up Event:refer NOTIFY (the one that
 * transitions the phone's UI to "Conference" after Confrn succeeds) to be
 * in-dialog with the original REFER — i.e. share Call-ID + From/To tags.
 * An OOB NOTIFY against the contact URI is silently ignored. So instead
 * of going through cisco_send_refer_response (which is OOB-friendly and
 * adds Refer-Sub: false), we build the dialog ourselves and send the 202
 * inside it, leaving the implicit subscription open for the NOTIFY.
 *
 * Lifecycle bookkeeping is subtle — pjsip's inc_lock and inc_session both
 * touch sess_count, which makes mismatches silently disastrous:
 *
 *   pjsip_dlg_create_uas_and_inc_lock  — creates dlg, takes mutex, AND
 *     does an inc_lock side effect: sess_count goes 0 → 1.
 *   pjsip_dlg_dec_lock                  — releases mutex AND drops
 *     sess_count by 1. If sess_count hits 0 with no pending tsx, the
 *     dialog is destroyed inside dec_lock.
 *   pjsip_dlg_inc_session / dec_session — independent of the mutex;
 *     this is what we want for keeping a dialog alive across function
 *     returns.
 *
 * So on the success path we must add a real session ref before dec_lock,
 * otherwise sess_count goes 1 → 0 inside dec_lock and the dialog only
 * survives as long as the inbound REFER's UAS tsx keeps tsx_count > 0
 * — a race against the conference_send_task serializer.
 *
 * On the failure path we don't take that session ref, so dec_lock just
 * unwinds the inc_lock side effect and the dialog destroys naturally.
 *
 * \retval dlg with +1 session count held — caller must dec_session via
 *         conference_task_data_destroy.
 * \retval NULL on failure (already logged; nothing to release).
 */
pjsip_dialog *conference_open_uas_dialog_and_202(pjsip_rx_data *rdata,
	const char *endpoint_id)
{
	pjsip_dialog *dlg = NULL;
	pjsip_tx_data *tdata = NULL;
	int kept = 0;

	if (pjsip_dlg_create_uas_and_inc_lock(pjsip_ua_instance(),
			rdata, NULL, &dlg) != PJ_SUCCESS) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — pjsip_dlg_create_uas_and_inc_lock "
			"failed for Confrn REFER; falling back to OOB 202 (merge "
			"will run but phone UI won't transition)\n", endpoint_id);
		return NULL;
	}

	if (pjsip_dlg_create_response(dlg, rdata, 202, NULL, &tdata)
			== PJ_SUCCESS
		&& pjsip_dlg_send_response(dlg, pjsip_rdata_get_tsx(rdata),
			tdata) == PJ_SUCCESS) {
		kept = 1;
	} else {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — in-dialog 202 build/send failed; "
			"phone UI will not transition\n", endpoint_id);
	}

	if (kept) {
		/* Real session ref that survives dec_lock. Matched by
		 * dec_session in conference_task_data_destroy. */
		pjsip_dlg_inc_session(dlg, &conference_module);
		send_conference_active_notify_locked(dlg, endpoint_id);
	}
	pjsip_dlg_dec_lock(dlg);
	return kept ? dlg : NULL;
}

/*!
 * \brief Build a 3-way multimix bridge from the two existing 1:1 bridges.
 *
 * The phone has two SIP dialogs at the moment Confrn is pressed:
 *
 *   active_dialog  — the original call (now held during the consult).
 *                    chan_phone_A is the phone's leg; chan_remote_A is
 *                    the other party.
 *   consult_dialog — the consult call (currently active).
 *                    chan_phone_B is the phone's leg; chan_remote_B is
 *                    the other party.
 *
 * The merge:
 *   1. Create a fresh multimix bridge.
 *   2. Move chan_phone_A from bridge_A into the conference.
 *   3. Move chan_remote_A from bridge_A — bridge_A is now empty and
 *      dissolves (DISSOLVE_EMPTY).
 *   4. Move chan_remote_B from bridge_B into the conference.
 *      chan_phone_B is explicitly hung up after chan_remote_B moves, so
 *      bridge_B dissolves too.
 *
 * chan_phone_B is deliberately NOT imparted — the phone only has one
 * physical audio path; chan_phone_A is the anchor. Same pattern the
 * chan_sip cisco-usecallmanager patch's conference_thread uses
 * (see channels/chan_sip.c, handle_remotecc_conference flow).
 *
 * The inbound REFER gets the same implicit-subscription shape as the
 * chan_sip patch: 202 Accepted, an immediate active Event:refer NOTIFY,
 * and a terminal Cisco-flavoured completion NOTIFY after bridge cleanup.
 */
static int conference_send_task(void *obj)
{
	struct conference_task_data *data = obj;
	const char *endpoint_id = ast_sorcery_object_get_id(data->endpoint);
	struct ast_sip_session *session_a = NULL;
	struct ast_sip_session *session_b = NULL;
	struct ast_channel *chan_phone_a = NULL;
	struct ast_channel *chan_phone_b = NULL;
	struct ast_channel *chan_remote_a = NULL;
	struct ast_channel *chan_remote_b = NULL;
	struct ast_bridge *bridge_a = NULL;
	struct ast_bridge *bridge_b = NULL;
	struct ast_bridge *conf = NULL;

	session_a = cisco_dialog_session_lookup(data->active_dialog.call_id,
		data->active_dialog.local_tag, data->active_dialog.remote_tag);
	session_b = cisco_dialog_session_lookup(data->consult_dialog.call_id,
		data->consult_dialog.local_tag, data->consult_dialog.remote_tag);
	if (!session_a || !session_b) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed Confrn but %s dialog "
			"doesn't match any live session (active callid=%s, "
			"consult callid=%s) — aborting\n",
			endpoint_id,
			!session_a && !session_b ? "neither"
				: !session_a ? "active" : "consult",
			data->active_dialog.call_id, data->consult_dialog.call_id);
		goto cleanup;
	}

	chan_phone_a = cisco_session_channel_ref(session_a);
	chan_phone_b = cisco_session_channel_ref(session_b);
	if (!chan_phone_a || !chan_phone_b) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s — one or both Confrn legs has no "
			"live channel (active=%p, consult=%p) — aborting\n",
			endpoint_id, chan_phone_a, chan_phone_b);
		goto cleanup;
	}

	chan_remote_a = ast_channel_bridge_peer(chan_phone_a);
	chan_remote_b = ast_channel_bridge_peer(chan_phone_b);
	if (!chan_remote_a || !chan_remote_b) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s — can't conference (%s has no "
			"bridge peer; happens when a leg is talking to a "
			"single-channel app like Playback/MoH rather than a "
			"real call)\n", endpoint_id,
			!chan_remote_a && !chan_remote_b ? "both legs"
				: !chan_remote_a ? "active leg" : "consult leg");
		goto cleanup;
	}

	ast_channel_lock(chan_phone_a);
	bridge_a = ast_channel_get_bridge(chan_phone_a);
	ast_channel_unlock(chan_phone_a);
	ast_channel_lock(chan_phone_b);
	bridge_b = ast_channel_get_bridge(chan_phone_b);
	ast_channel_unlock(chan_phone_b);
	if (!bridge_a || !bridge_b) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s — one or both legs has no bridge "
			"object (active=%p, consult=%p) — aborting\n",
			endpoint_id, bridge_a, bridge_b);
		goto cleanup;
	}

	/* MULTIMIX | NATIVE: softmix mixer with native pass-through where
	 * possible. SMART: let the bridging framework auto-promote tech as
	 * channel count changes. DISSOLVE_EMPTY: tear down when the last
	 * channel leaves. No DISSOLVE_HANGUP at the bridge level — the
	 * initiator-hangup behaviour is decided per call by the endpoint's
	 * keep_conference sorcery field: keep_conference=no (default) calls
	 * set_dissolve_on_initiator_hangup to flip DISSOLVE_HANGUP on
	 * chan_phone_a's bridge_channel; keep_conference=yes leaves it
	 * unflagged so the mix survives the initiator dropping.
	 * TRANSFER_BRIDGE_ONLY: don't let an attended-transfer-into-
	 * conference race the merge. */
	conf = ast_bridge_base_new(
		AST_BRIDGE_CAPABILITY_MULTIMIX | AST_BRIDGE_CAPABILITY_NATIVE,
		AST_BRIDGE_FLAG_DISSOLVE_EMPTY | AST_BRIDGE_FLAG_SMART
			| AST_BRIDGE_FLAG_TRANSFER_BRIDGE_ONLY,
		"cisco_conference", NULL, NULL);
	if (!conf) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — ast_bridge_base_new failed; "
			"aborting Confrn\n", endpoint_id);
		goto cleanup;
	}

	/* Move chan_phone_A first so the user's audio path follows it into
	 * the conference, then mirror chan_sip's channel nudges: UNHOLD the
	 * two remote bridge peers, move them in, and explicitly hang up the
	 * consult-side phone anchor after its remote party has joined the
	 * mix. attempt_recovery=1: if a move fails, put chan back into its
	 * source bridge so it isn't orphaned.
	 *
	 * mark_channel_as_conference and the post-merge REFER/NOTIFY pair
	 * intentionally run AT THE END (after softhangup), because all three
	 * trigger outbound SIP transactions and chan_pjsip session refreshes
	 * that take channel/session locks via bridge-frame relay. With
	 * chan_phone_b still alive earlier in the sequence, the connected-
	 * line re-INVITE (carrying Conference RPID = \2004 after the call_
	 * extras hook unconditionally emits it for Conference-marked legs)
	 * propagates as a frame through the conf bridge and the residual
	 * bridge_b, locking peer channel/session state. ast_softhangup on
	 * chan_phone_b then blocks on that lock and the cisco-conference
	 * serializer wedges — observed live on 2026-05-14 as four channels
	 * stuck Up with the taskprocessor at 0 processed / 4 queued. */
	if (ast_bridge_move(conf, bridge_a, chan_phone_a, NULL, 1)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — partial bridge move during Confrn; "
			"some channels may have been left in their original bridges. "
			"Phone audio path may be in an odd state — hang up to recover.\n",
			endpoint_id);
		/* Leave the (possibly-partial) conf bridge in place; dissolving
		 * it now could yank channels that did succeed into limbo. The
		 * DISSOLVE_EMPTY flag will collect it once everyone leaves. */
		goto cleanup;
	}

	if (!data->keep_conference) {
		set_dissolve_on_initiator_hangup(chan_phone_a, endpoint_id);
	}

	indicate_remote_unhold(chan_remote_a, endpoint_id, "original remote leg");
	if (ast_bridge_move(conf, bridge_a, chan_remote_a, NULL, 1)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — failed to move original remote leg "
			"during Confrn; phone audio path may be in an odd state — "
			"hang up to recover.\n", endpoint_id);
		goto cleanup;
	}
	cisco_conf_mark_joined(chan_remote_a);

	indicate_remote_unhold(chan_remote_b, endpoint_id, "consult remote leg");
	if (ast_bridge_move(conf, bridge_b, chan_remote_b, NULL, 1)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — failed to move consult remote leg "
			"during Confrn; phone audio path may be in an odd state — "
			"hang up to recover.\n", endpoint_id);
		goto cleanup;
	}
	cisco_conf_mark_joined(chan_remote_b);

	{
		struct ast_channel *video_src = NULL;
		int video_count = 0;

		tally_video_participant(chan_phone_a, &video_src, &video_count);
		tally_video_participant(chan_remote_a, &video_src, &video_count);
		tally_video_participant(chan_remote_b, &video_src, &video_count);
		apply_bridge_video_mode(conf, video_src, video_count,
			endpoint_id);
	}

	if (ast_softhangup(chan_phone_b, AST_SOFTHANGUP_EXPLICIT)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — failed to hang up consult-side "
			"anchor %s after Confrn merge\n",
			endpoint_id, ast_channel_name(chan_phone_b));
	} else {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s — hung up consult-side anchor %s\n",
			endpoint_id, ast_channel_name(chan_phone_b));
	}

	ast_log(LOG_NOTICE,
		"cisco-conference: %s — built 3-way conference: %s + %s + %s "
		"(consult-side anchor %s hung up)\n",
		endpoint_id,
		ast_channel_name(chan_phone_a),
		ast_channel_name(chan_remote_a),
		ast_channel_name(chan_remote_b),
		ast_channel_name(chan_phone_b));

	/* Post-merge wire dance, mirroring the chan_sip patch's
	 * conference_thread cleanup paths (NON-join case). Without BOTH of
	 * these the phone's UI stays "active + held" — empirically verified
	 * across three attempts with progressively closer SIP shapes.
	 *
	 * The smoking gun is the FIRST item: until the phone receives a
	 * "<holdretrievereq>" Cisco-private signal targeting the ORIGINAL
	 * call, its UI keeps showing that call as held — even though the
	 * far end is now mixed into the conference. The chan_sip patch's
	 * inline comment says it best: "We need to signal to the phone to
	 * take the first call leg off hold, even though the generator on
	 * that channel has gone due to the masquerade as the phone still
	 * thinks that it is on hold". The SECOND item (Cisco-flavoured
	 * NOTIFY) closes the implicit REFER subscription with a Cisco
	 * status payload rather than RFC 3515 sipfrag — same Subscription-
	 * State header, different Content-Type and body. */
	send_holdretrieve_refer(data);
	send_conference_completion_notify(data);

	/* Mark the phone leg's connected-line as Conference LAST. chan_pjsip
	 * turns this into an outbound re-INVITE on the original call's dialog
	 * carrying the Cisco \2004 display token (added by call_extras' hook),
	 * which is what flips the phone UI from a regular active call to the
	 * Conference glyph + localised label. Done last so any locks the
	 * outbound re-INVITE acquires don't collide with the bridge moves or
	 * the consult-side softhangup above. */
	mark_channel_as_conference(chan_phone_a, endpoint_id);

cleanup:
	ao2_cleanup(conf);
	ao2_cleanup(bridge_a);
	ao2_cleanup(bridge_b);
	ast_channel_cleanup(chan_remote_a);
	ast_channel_cleanup(chan_remote_b);
	ast_channel_cleanup(chan_phone_a);
	ast_channel_cleanup(chan_phone_b);
	ao2_cleanup(session_a);
	ao2_cleanup(session_b);
	ao2_cleanup(data);
	return 0;
}

/*!
 * \brief OOB REFER to the phone carrying a <holdretrievereq> body for the
 *        original call's dialog. Cisco-private signal that tells the
 *        firmware "the leg you have on hold is now in the conference,
 *        please un-hold it visually".
 *
 * Identical wire shape to the chan_sip patch's conference_thread inline
 * holdretrievereq emission. Uses the same OOB-REFER-with-Content-ID
 * machinery our ConfList path uses; <dialogid> is filled from the active
 * (originally held) call. localtag is the phone's side per Cisco
 * convention.
 */
static pjsip_msg_body *holdretrieve_build(pj_pool_t *pool, void *vctx)
{
	struct conference_task_data *data = vctx;
	char xml[1024];
	pj_str_t type    = pj_str("application");
	pj_str_t subtype = pj_str("x-cisco-remotecc-request+xml");
	pj_str_t text;

	snprintf(xml, sizeof(xml),
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<x-cisco-remotecc-request>\n"
		" <holdretrievereq>\n"
		"  <dialogid>\n"
		"   <callid>%s</callid>\n"
		"   <localtag>%s</localtag>\n"
		"   <remotetag>%s</remotetag>\n"
		"  </dialogid>\n"
		" </holdretrievereq>\n"
		"</x-cisco-remotecc-request>\n",
		data->active_dialog.call_id,
		data->active_dialog.local_tag,
		data->active_dialog.remote_tag);

	pj_strdup2(pool, &text, xml);
	return pjsip_msg_body_create(pool, &type, &subtype, &text);
}

static void send_holdretrieve_refer(struct conference_task_data *data)
{
	cisco_endpoint_send_refer_to_contact(data->endpoint, data->contact,
		"cisco-conference", "cisco-holdretrieve",
		"holdretrieve REFER",
		holdretrieve_build, data);
}

/*!
 * \brief In-dialog NOTIFY terminating the REFER's implicit subscription,
 *        carrying the Cisco-flavoured x-cisco-remotecc-response body
 *        (chan_sip patch's conference_thread cleanup shape).
 *
 * Body is a tiny <response><code>200</code></response> wrapped in
 * <x-cisco-remotecc-response>. This is the Cisco signal that the
 * conference operation completed (the RFC 3515 message/sipfrag body
 * we tried first was 200-OK'd by the phone but didn't drive UI
 * transition — Cisco firmware reads the Content-Type to decide
 * "this is the conference completion ack").
 */
static void send_conference_completion_notify(struct conference_task_data *data)
{
	const char *endpoint_id = ast_sorcery_object_get_id(data->endpoint);
	pjsip_tx_data *notify_tdata = NULL;
	pj_str_t method_name = pj_str("NOTIFY");
	pjsip_method method;
	pj_str_t hdr_event_name    = pj_str("Event");
	pj_str_t hdr_event_val     = pj_str("refer");
	pj_str_t hdr_substate_name = pj_str("Subscription-State");
	pj_str_t hdr_substate_val  = pj_str("terminated;reason=noresource");
	pj_str_t body_type    = pj_str("application");
	pj_str_t body_subtype = pj_str("x-cisco-remotecc-response+xml");
	pj_str_t body_text    = pj_str(
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<x-cisco-remotecc-response>\n"
		" <response>\n"
		"  <code>200</code>\n"
		" </response>\n"
		"</x-cisco-remotecc-response>\n");
	pjsip_dialog *dlg = data->dlg;

	if (!dlg) {
		return;
	}

	/* inc_lock takes the mutex AND increments sess_count by 1; dec_lock
	 * undoes both. Our caller already added a real session ref via
	 * pjsip_dlg_inc_session in conference_open_uas_dialog_and_202, so the
	 * net sess_count delta of inc_lock/dec_lock here is zero — we never
	 * hit "Assert failed: sess_count > 0" and we never deadlock by
	 * forgetting to dec_lock. */
	pjsip_method_init_np(&method, &method_name);
	pjsip_dlg_inc_lock(dlg);
	if (pjsip_dlg_create_request(dlg, &method, -1, &notify_tdata)
			== PJ_SUCCESS) {
		pjsip_msg_add_hdr(notify_tdata->msg,
			(pjsip_hdr *) pjsip_generic_string_hdr_create(
				notify_tdata->pool, &hdr_event_name,
				&hdr_event_val));
		pjsip_msg_add_hdr(notify_tdata->msg,
			(pjsip_hdr *) pjsip_generic_string_hdr_create(
				notify_tdata->pool, &hdr_substate_name,
				&hdr_substate_val));
		notify_tdata->msg->body = pjsip_msg_body_create(
			notify_tdata->pool, &body_type, &body_subtype,
			&body_text);

		if (pjsip_dlg_send_request(dlg, notify_tdata, -1, NULL)
				== PJ_SUCCESS) {
			ast_log(LOG_NOTICE,
				"cisco-conference: %s — in-dialog completion NOTIFY "
				"(x-cisco-remotecc-response code=200) sent to %s\n",
				endpoint_id, data->contact->uri);
		} else {
			ast_log(LOG_WARNING,
				"cisco-conference: %s — pjsip_dlg_send_request failed "
				"for completion NOTIFY; phone UI may not transition\n",
				endpoint_id);
		}
	} else {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — pjsip_dlg_create_request failed "
			"for completion NOTIFY; phone UI may not transition\n",
			endpoint_id);
	}
	pjsip_dlg_dec_lock(dlg);
}

void queue_conference(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *active_dialog,
	const struct conference_dialog_id *consult_dialog,
	pjsip_dialog *dlg, int keep_conference)
{
	struct conference_task_data *data;

	data = ao2_alloc(sizeof(*data), conference_task_data_destroy);
	if (!data) {
		/* Couldn't queue — must release the dialog ourselves. */
		if (dlg) {
			pjsip_dlg_dec_session(dlg, &conference_module);
		}
		return;
	}
	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	ao2_ref(contact, +1);
	data->contact = contact;
	data->active_dialog   = *active_dialog;
	data->consult_dialog  = *consult_dialog;
	data->dlg             = dlg;
	data->keep_conference = keep_conference;

	if (ast_sip_push_task(conference_serializer, conference_send_task, data)) {
		ast_log(LOG_WARNING,
			"cisco-conference: failed to queue Conference task\n");
		ao2_cleanup(data);
	}
}

/* ----------------------------------------------------------------------
 * Join / Select / Unselect — multi-call merge.
 * ---------------------------------------------------------------------- */

struct join_task_data {
	struct ast_sip_endpoint *endpoint;
	struct ast_sip_contact *contact;
	struct conference_dialog_id active_dialog;
	struct conference_dialog_id selected[JOIN_MAX_SELECTED];
	int selected_count;
	int keep_conference;
};

static void join_task_data_destroy(void *obj)
{
	struct join_task_data *data = obj;
	ao2_cleanup(data->contact);
	ao2_cleanup(data->endpoint);
}

static pjsip_msg_body *join_notifyreq_build(pj_pool_t *pool, void *vctx)
{
	struct join_task_data *data = vctx;
	char xml[1024];
	pj_str_t type    = pj_str("application");
	pj_str_t subtype = pj_str("x-cisco-remotecc-request+xml");
	pj_str_t text;

	snprintf(xml, sizeof(xml),
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<x-cisco-remotecc-request>\n"
		" <notifyreq>\n"
		"  <dialogid>\n"
		"   <callid>%s</callid>\n"
		"   <localtag>%s</localtag>\n"
		"   <remotetag>%s</remotetag>\n"
		"  </dialogid>\n"
		"  <feature>Join</feature>\n"
		"  <status>Complete</status>\n"
		" </notifyreq>\n"
		"</x-cisco-remotecc-request>\n",
		data->active_dialog.call_id,
		data->active_dialog.local_tag,
		data->active_dialog.remote_tag);

	pj_strdup2(pool, &text, xml);
	return pjsip_msg_body_create(pool, &type, &subtype, &text);
}

static void send_join_notifyreq_refer(struct join_task_data *data)
{
	cisco_endpoint_send_refer_to_contact(data->endpoint, data->contact,
		"cisco-conference", "cisco-join-notifyreq",
		"Join notifyreq REFER",
		join_notifyreq_build, data);
}

static int join_send_task(void *obj)
{
	struct join_task_data *data = obj;
	const char *endpoint_id = ast_sorcery_object_get_id(data->endpoint);
	struct ast_sip_session *session_active = NULL;
	struct ast_channel *chan_phone_active = NULL;
	struct ast_channel *chan_remote_active = NULL;
	struct ast_bridge *bridge_active = NULL;
	struct ast_bridge *conf = NULL;
	struct ast_channel *video_src = NULL;
	int video_count = 0;
	int joined_count = 0;
	int i;

	/* Active call — its phone-side becomes the anchor. */
	session_active = cisco_dialog_session_lookup(
		data->active_dialog.call_id,
		data->active_dialog.local_tag,
		data->active_dialog.remote_tag);
	if (!session_active) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s Join — active call dialog %s not "
			"matched to any live session — aborting\n",
			endpoint_id, data->active_dialog.call_id);
		goto cleanup;
	}
	chan_phone_active = cisco_session_channel_ref(session_active);
	if (!chan_phone_active) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s Join — active call has no live "
			"channel — aborting\n", endpoint_id);
		goto cleanup;
	}
	chan_remote_active = ast_channel_bridge_peer(chan_phone_active);
	if (!chan_remote_active) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s Join — active call has no bridge "
			"peer — aborting\n", endpoint_id);
		goto cleanup;
	}
	ast_channel_lock(chan_phone_active);
	bridge_active = ast_channel_get_bridge(chan_phone_active);
	ast_channel_unlock(chan_phone_active);
	if (!bridge_active) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s Join — active call's channel "
			"isn't in a bridge — aborting\n", endpoint_id);
		goto cleanup;
	}

	conf = ast_bridge_base_new(
		AST_BRIDGE_CAPABILITY_MULTIMIX | AST_BRIDGE_CAPABILITY_NATIVE,
		AST_BRIDGE_FLAG_DISSOLVE_EMPTY | AST_BRIDGE_FLAG_SMART
			| AST_BRIDGE_FLAG_TRANSFER_BRIDGE_ONLY,
		"cisco_conference", NULL, NULL);
	if (!conf) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s Join — ast_bridge_base_new failed; "
			"aborting\n", endpoint_id);
		goto cleanup;
	}

	/* Move the active call's phone-side and remote-side into the conf
	 * first; it's the anchor. */
	if (ast_bridge_move(conf, bridge_active, chan_phone_active, NULL, 1)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s Join — failed to move active "
			"phone-side %s; aborting\n", endpoint_id,
			ast_channel_name(chan_phone_active));
		goto cleanup;
	}

	if (!data->keep_conference) {
		set_dissolve_on_initiator_hangup(chan_phone_active, endpoint_id);
	}

	if (ast_bridge_move(conf, bridge_active, chan_remote_active, NULL, 1)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s Join — failed to move active "
			"remote leg %s\n", endpoint_id,
			ast_channel_name(chan_remote_active));
		goto cleanup;
	}
	cisco_conf_mark_joined(chan_remote_active);

	tally_video_participant(chan_phone_active, &video_src, &video_count);
	tally_video_participant(chan_remote_active, &video_src, &video_count);

	/* Now walk the selected list: for each one, UNHOLD its remote bridge
	 * peer, move the remote into conf, and softhangup the phone-side
	 * anchor. Skip any selected entry that points at the active call
	 * itself (the phone may have left its active call in the selected
	 * list — chan_sip's joining loop has the same defensive skip). */
	for (i = 0; i < data->selected_count; i++) {
		const struct conference_dialog_id *sel = &data->selected[i];
		struct ast_sip_session *session_sel = NULL;
		struct ast_channel *chan_phone_sel = NULL;
		struct ast_channel *chan_remote_sel = NULL;
		struct ast_bridge *bridge_sel = NULL;

		if (!strcmp(sel->call_id, data->active_dialog.call_id)
			&& !strcmp(sel->local_tag, data->active_dialog.local_tag)
			&& !strcmp(sel->remote_tag, data->active_dialog.remote_tag)) {
			continue;
		}

		session_sel = cisco_dialog_session_lookup(sel->call_id,
			sel->local_tag, sel->remote_tag);
		if (!session_sel) {
			ast_log(LOG_NOTICE,
				"cisco-conference: %s Join — selected dialog %s "
				"no longer has a live session; skipping\n",
				endpoint_id, sel->call_id);
			continue;
		}
		chan_phone_sel = cisco_session_channel_ref(session_sel);
		if (!chan_phone_sel) {
			ast_log(LOG_NOTICE,
				"cisco-conference: %s Join — selected dialog %s "
				"has no live channel; skipping\n",
				endpoint_id, sel->call_id);
			ao2_cleanup(session_sel);
			continue;
		}
		chan_remote_sel = ast_channel_bridge_peer(chan_phone_sel);
		if (!chan_remote_sel) {
			ast_log(LOG_NOTICE,
				"cisco-conference: %s Join — selected dialog %s "
				"has no bridge peer; skipping\n",
				endpoint_id, sel->call_id);
			ast_channel_unref(chan_phone_sel);
			ao2_cleanup(session_sel);
			continue;
		}
		ast_channel_lock(chan_phone_sel);
		bridge_sel = ast_channel_get_bridge(chan_phone_sel);
		ast_channel_unlock(chan_phone_sel);
		if (!bridge_sel) {
			ast_log(LOG_NOTICE,
				"cisco-conference: %s Join — selected dialog %s "
				"has no source bridge; skipping\n",
				endpoint_id, sel->call_id);
			ast_channel_cleanup(chan_remote_sel);
			ast_channel_unref(chan_phone_sel);
			ao2_cleanup(session_sel);
			continue;
		}

		indicate_remote_unhold(chan_remote_sel, endpoint_id,
			"selected remote leg");

		if (ast_bridge_move(conf, bridge_sel, chan_remote_sel, NULL, 1)) {
			ast_log(LOG_WARNING,
				"cisco-conference: %s Join — failed to move "
				"selected remote leg %s; skipping\n",
				endpoint_id, ast_channel_name(chan_remote_sel));
		} else {
			cisco_conf_mark_joined(chan_remote_sel);
			tally_video_participant(chan_remote_sel,
				&video_src, &video_count);
			joined_count++;
		}

		if (ast_softhangup(chan_phone_sel, AST_SOFTHANGUP_EXPLICIT)) {
			ast_log(LOG_WARNING,
				"cisco-conference: %s Join — failed to hang up "
				"selected phone-side %s\n",
				endpoint_id, ast_channel_name(chan_phone_sel));
		} else {
			ast_log(LOG_NOTICE,
				"cisco-conference: %s Join — hung up selected "
				"phone-side %s\n", endpoint_id,
				ast_channel_name(chan_phone_sel));
		}

		ao2_cleanup(bridge_sel);
		ast_channel_cleanup(chan_remote_sel);
		ast_channel_unref(chan_phone_sel);
		ao2_cleanup(session_sel);
	}

	ast_log(LOG_NOTICE,
		"cisco-conference: %s Join — built %d-way conference "
		"(active=%s + %d selected leg%s merged)\n",
		endpoint_id, 2 + joined_count,
		ast_channel_name(chan_phone_active),
		joined_count, joined_count == 1 ? "" : "s");

	/* video_src may point to a chan_remote_sel whose LOCAL ref was
	 * released at end of its loop iteration — that's fine, the
	 * bridge still holds its own ref so the channel object remains
	 * alive until it leaves the bridge. */
	apply_bridge_video_mode(conf, video_src, video_count, endpoint_id);

	send_join_notifyreq_refer(data);
	mark_channel_as_conference(chan_phone_active, endpoint_id);

	cisco_selected_clear_endpoint(endpoint_id);

cleanup:
	ao2_cleanup(conf);
	ao2_cleanup(bridge_active);
	ast_channel_cleanup(chan_remote_active);
	ast_channel_cleanup(chan_phone_active);
	ao2_cleanup(session_active);
	ao2_cleanup(data);
	return 0;
}

struct collect_selected_ctx {
	struct join_task_data *data;
};

static int collect_selected_visitor(
	const struct conference_dialog_id *dialog_id, void *arg)
{
	struct collect_selected_ctx *ctx = arg;
	if (ctx->data->selected_count >= JOIN_MAX_SELECTED) {
		ast_log(LOG_NOTICE,
			"cisco-conference: Join — more than %d selected calls; "
			"only the first %d will be merged\n",
			JOIN_MAX_SELECTED, JOIN_MAX_SELECTED);
		return 1;
	}
	ctx->data->selected[ctx->data->selected_count++] = *dialog_id;
	return 0;
}

void queue_join(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *active_dialog,
	int keep_conference)
{
	struct join_task_data *data;
	const char *endpoint_id = ast_sorcery_object_get_id(endpoint);
	struct collect_selected_ctx ctx;

	data = ao2_alloc(sizeof(*data), join_task_data_destroy);
	if (!data) {
		return;
	}
	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	ao2_ref(contact, +1);
	data->contact = contact;
	data->active_dialog   = *active_dialog;
	data->keep_conference = keep_conference;

	/* Snapshot the endpoint's current selected list into the task data
	 * so the task body can iterate without holding any container locks
	 * and without races against further Select/Unselect REFERs while
	 * the task runs on the serializer. */
	ctx.data = data;
	cisco_selected_iterate_for_endpoint(endpoint_id,
		collect_selected_visitor, &ctx);

	if (ast_sip_push_task(conference_serializer, join_send_task, data)) {
		ast_log(LOG_WARNING,
			"cisco-conference: failed to queue Join task\n");
		ao2_cleanup(data);
	}
}

/* ----------------------------------------------------------------------
 * RmLastConf — remove the most-recently-joined participant.
 * ---------------------------------------------------------------------- */

struct rmlastconf_task_data {
	struct ast_sip_endpoint *endpoint;
	struct ast_sip_contact *contact;
	struct conference_dialog_id active_dialog;
};

static void rmlastconf_task_data_destroy(void *obj)
{
	struct rmlastconf_task_data *data = obj;

	ao2_cleanup(data->endpoint);
	ao2_cleanup(data->contact);
}

static int rmlastconf_send_task(void *obj)
{
	RAII_VAR(struct rmlastconf_task_data *, data, obj, ao2_cleanup);
	struct ast_sip_session *session;
	struct ast_channel *chan_phone = NULL;
	struct ast_bridge *bridge = NULL;
	struct ast_channel *victim = NULL;
	const char *endpoint_id;

	endpoint_id = ast_sorcery_object_get_id(data->endpoint);

	session = cisco_dialog_session_lookup(data->active_dialog.call_id,
		data->active_dialog.local_tag, data->active_dialog.remote_tag);
	if (!session) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s RmLastConf — dialog %s no longer "
			"exists; nothing to remove\n",
			endpoint_id, data->active_dialog.call_id);
		return 0;
	}
	chan_phone = cisco_session_channel_ref(session);
	ao2_cleanup(session);
	if (!chan_phone) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s RmLastConf — dialog %s has no "
			"channel\n", endpoint_id, data->active_dialog.call_id);
		return 0;
	}

	ast_channel_lock(chan_phone);
	bridge = ast_channel_get_bridge(chan_phone);
	ast_channel_unlock(chan_phone);
	if (!bridge) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s RmLastConf — channel %s is not in "
			"a bridge (no active conference to operate on)\n",
			endpoint_id, ast_channel_name(chan_phone));
		ast_channel_unref(chan_phone);
		return 0;
	}

	victim = cisco_conf_find_last_joined(bridge, chan_phone);
	if (!victim) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s RmLastConf — no tracked participant "
			"in the bridge to remove (a 2-party call, a stock "
			"ConfBridge we didn't build, or every conference participant "
			"has already left)\n", endpoint_id);
		ao2_ref(bridge, -1);
		ast_channel_unref(chan_phone);
		return 0;
	}

	ast_log(LOG_NOTICE,
		"cisco-conference: %s RmLastConf — removing most-recently-"
		"joined participant %s from conference bridge\n",
		endpoint_id, ast_channel_name(victim));
	if (ast_bridge_remove(bridge, victim)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s RmLastConf — ast_bridge_remove(%s) "
			"failed; the participant may have already left\n",
			endpoint_id, ast_channel_name(victim));
	}

	ast_channel_unref(victim);
	ao2_ref(bridge, -1);
	ast_channel_unref(chan_phone);
	return 0;
}

void queue_rmlastconf(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *active_dialog)
{
	struct rmlastconf_task_data *data;

	data = ao2_alloc(sizeof(*data), rmlastconf_task_data_destroy);
	if (!data) {
		return;
	}
	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	ao2_ref(contact, +1);
	data->contact = contact;
	data->active_dialog = *active_dialog;

	if (ast_sip_push_task(conference_serializer, rmlastconf_send_task,
			data)) {
		ast_log(LOG_WARNING,
			"cisco-conference: failed to queue RmLastConf task\n");
		ao2_cleanup(data);
	}
}
