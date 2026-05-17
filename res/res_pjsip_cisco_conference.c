/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_conference
 *
 * Cisco Enterprise SIP conference control.
 *
 * Wire shapes mirror the chan_sip cisco-usecallmanager patch's
 * channels/sip/conference.c:751 sip_conference_participants.
 *
 * Coverage:
 *   - ConfList softkey + Mute / Remove / Update / participant-pick
 *     action softkeys (chan_sip's two-step state machine: a sticky
 *     softkey REFER sets pending action; the next participant-pick
 *     REFER applies it. Default action when no softkey was pressed
 *     first is Mute, matching the patch).
 *   - Confrn (3-way conference creation) is implemented in full,
 *     including holdretrieve REFER + Cisco-flavoured completion NOTIFY,
 *     connected-line "Conference" display token, explicit consult-anchor
 *     softhangup, and the cisco_keep_conference initiator-hangup knob.
 *   - Select / Unselect / Join: the multi-call merge path. Select adds a
 *     dialog to the cisco_selected_calls list; Unselect removes it; Join
 *     (pressed on the active call) merges the active call's phone-side
 *     plus each selected call's remote-side into a single multimix
 *     conference, softhanging the selected calls' phone-side anchors.
 *     Cleanup is a single OOB REFER with a <notifyreq><feature>Join
 *     </feature><status>Complete</status> body targeting the active
 *     dialog.
 *   - RmLastConf: remove the most-recently-joined participant from
 *     the conference. Tracks join order via per-channel datastores
 *     attached by cisco_conf_mark_joined() (res/cisco_endpoint/session.c) at every
 *     remote-leg ast_bridge_move; the handler finds the channel with
 *     the latest timestamp and ast_bridge_remove()s it. Phone-side
 *     anchors aren't marked, so RmLastConf can't remove the user who
 *     pressed it.
 *
 * Architecture:
 *   - Registers a pjsip_module at PJSIP_MOD_PRIORITY_APPLICATION - 2,
 *     one slot before res_pjsip_cisco_remotecc, so we see incoming
 *     REFERs with x-cisco bodies first. ConfList REFERs we claim and
 *     return PJ_TRUE; everything else returns PJ_FALSE so the
 *     remotecc handler still gets its turn.
 *   - Resolves the XML <dialogid> to an ast_sip_session via
 *     pjsip_ua_find_dialog + ast_sip_dialog_get_session — independent
 *     of the remotecc dialog registry, so conference.so does not need
 *     a cross-module symbol export.
 *
 * This .c is the module entry point: REFER body classification +
 * dispatch + module load/unload. Implementations live in sibling
 * files (res/cisco_conference/state.c, res/cisco_conference/list.c, res/cisco_conference/confrn.c)
 * sharing declarations via conference_private.h, all compiled into the
 * same .so.
 */

/*** MODULEINFO
	<depend>res_pjsip</depend>
	<depend>res_pjsip_session</depend>
	<depend>libxml2</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_ua.h>
#include <pjsip/sip_multipart.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/astobj2.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/taskprocessor.h"
#include "asterisk/xml.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"

#include "cisco/endpoint.h"
#include "cisco/rdata.h"
#include "cisco/refer.h"
#include "cisco/session.h"
#include "conference_private.h"

/* Globals declared in conference_private.h; owned by load/unload below. */
struct ast_taskprocessor *conference_serializer;
struct ao2_container *conflist_pending_actions;
struct ao2_container *cisco_selected_calls;

/*!
 * \brief Copy the three child fields of <dialogid> / <consultdialogid> /
 *        <joindialogid> into \a out.
 *
 * \retval 1 on a fully-populated dialog ref (callid + both tags non-empty),
 * \retval 0 otherwise.
 */
static int copy_dialog_id(struct ast_xml_node *dialog_node,
	struct conference_dialog_id *out)
{
	if (!dialog_node) {
		return 0;
	}
	memset(out, 0, sizeof(*out));
	cisco_xml_copy_child_text(dialog_node, "callid", out->call_id,
		sizeof(out->call_id));
	cisco_xml_copy_child_text(dialog_node, "localtag", out->local_tag,
		sizeof(out->local_tag));
	cisco_xml_copy_child_text(dialog_node, "remotetag", out->remote_tag,
		sizeof(out->remote_tag));
	return !ast_strlen_zero(out->call_id)
		&& !ast_strlen_zero(out->local_tag)
		&& !ast_strlen_zero(out->remote_tag);
}

/*!
 * \brief Parse a Cisco RemoteCC REFER body and classify the softkey.
 *
 * Returns 1 + populates \a out for ConfList and Conference (with both
 * <dialogid> AND, for Conference, <consultdialogid>). Returns 0 for any
 * other softkey (which leaves the REFER to res_pjsip_cisco_remotecc).
 *
 * For Conference: both dialog blocks must be present and complete;
 * otherwise we 0 it and let the caller pass through.
 */
static int detect_remotecc_softkey(pjsip_msg_body *body,
	struct remotecc_softkey_msg *out)
{
	struct ast_xml_doc *doc;
	struct ast_xml_node *root;
	struct ast_xml_node *softkey_msg;
	char softkey[64];
	int handled = 0;

	memset(out, 0, sizeof(*out));

	if (!body || body->len > CONFERENCE_MAX_BODY) {
		return 0;
	}

	doc = cisco_xml_read_body(body);
	if (!doc) {
		return 0;
	}

	root = ast_xml_get_root(doc);
	if (!root || strcasecmp(ast_xml_node_get_name(root),
			"x-cisco-remotecc-request")) {
		goto done;
	}

	softkey_msg = ast_xml_find_element(ast_xml_node_get_children(root),
		"softkeyeventmsg", NULL, NULL);
	if (!softkey_msg) {
		goto done;
	}

	if (!cisco_xml_copy_child_text(softkey_msg, "softkeyevent", softkey,
			sizeof(softkey))) {
		goto done;
	}

	if (!strcmp(softkey, "ConfList")) {
		out->kind = REMOTECC_SOFTKEY_CONFLIST;
	} else if (!strcmp(softkey, "Conference")) {
		out->kind = REMOTECC_SOFTKEY_CONFERENCE;
	} else if (!strcmp(softkey, "Select")) {
		out->kind = REMOTECC_SOFTKEY_SELECT;
	} else if (!strcmp(softkey, "Unselect")) {
		out->kind = REMOTECC_SOFTKEY_UNSELECT;
	} else if (!strcmp(softkey, "Join")) {
		out->kind = REMOTECC_SOFTKEY_JOIN;
	} else if (!strcmp(softkey, "RmLastConf")) {
		out->kind = REMOTECC_SOFTKEY_RMLASTCONF;
	} else {
		/* Not ours — let remotecc.c handle (HLog/MCID/Park/…). */
		goto done;
	}

	if (!copy_dialog_id(ast_xml_find_element(
			ast_xml_node_get_children(softkey_msg),
			"dialogid", NULL, NULL),
			&out->dialog_id)) {
		out->kind = REMOTECC_SOFTKEY_NONE;
		goto done;
	}

	if (out->kind == REMOTECC_SOFTKEY_CONFERENCE) {
		if (!copy_dialog_id(ast_xml_find_element(
				ast_xml_node_get_children(softkey_msg),
				"consultdialogid", NULL, NULL),
				&out->consult_dialog_id)) {
			ast_log(LOG_WARNING,
				"cisco-conference: Conference REFER missing or "
				"incomplete <consultdialogid> — ignoring\n");
			out->kind = REMOTECC_SOFTKEY_NONE;
			goto done;
		}
	}

	handled = 1;

done:
	ast_xml_close(doc);
	return handled;
}

/*!
 * \brief Parse a datapassthroughreq REFER and classify it as a ConfList
 *        action (Mute / Remove / Update / select-participant).
 *
 * The REFER body shape is multipart/mixed:
 *   part 1: application/x-cisco-remotecc-request+xml containing
 *           <x-cisco-remotecc-request>
 *             <datapassthroughreq>
 *               <applicationid>1</applicationid>     (SIP_REMOTECC_CONF_LIST)
 *               <confid>NNN</confid>
 *             </datapassthroughreq>
 *           </x-cisco-remotecc-request>
 *   part 2: application/x-cisco-remotecc-cm+xml containing the raw
 *           user_call_data string ("Mute", "Remove", "Update", or a
 *           participant ordinal like "2").
 *
 * The first part we already have (the existing
 * cisco_find_remotecc_request_body call surfaces it). The second part we
 * walk the rdata's full multipart body to find. If applicationid != 1
 * we let res_pjsip_cisco_remotecc handle it (it does callback, etc.).
 *
 * \retval 1 if this is a CONFLIST_ACTION REFER and \a out is populated.
 * \retval 0 otherwise.
 */
static int detect_conflist_action(pjsip_rx_data *rdata, pjsip_msg_body *body,
	struct remotecc_softkey_msg *out)
{
	struct ast_xml_doc *doc = NULL;
	struct ast_xml_node *root;
	struct ast_xml_node *dpt;
	char appid[32];
	char confid[32];
	pjsip_msg_body *full_body;
	pjsip_media_type cm_type;
	pjsip_multipart_part *cm_part;
	int handled = 0;

	if (!body || body->len > CONFERENCE_MAX_BODY) {
		return 0;
	}

	doc = cisco_xml_read_body(body);
	if (!doc) {
		return 0;
	}

	root = ast_xml_get_root(doc);
	if (!root || strcasecmp(ast_xml_node_get_name(root),
			"x-cisco-remotecc-request")) {
		goto done;
	}

	dpt = ast_xml_find_element(ast_xml_node_get_children(root),
		"datapassthroughreq", NULL, NULL);
	if (!dpt) {
		goto done;
	}

	if (!cisco_xml_copy_child_text(dpt, "applicationid", appid,
			sizeof(appid))) {
		goto done;
	}
	if (atoi(appid) != CONFERENCE_APP_ID_CONF_LIST) {
		/* Not ours — remotecc.c handles other application_ids. */
		goto done;
	}
	memset(out, 0, sizeof(*out));
	out->kind = REMOTECC_SOFTKEY_CONFLIST_ACTION;

	if (cisco_xml_copy_child_text(dpt, "confid", confid, sizeof(confid))) {
		out->conf_id = (unsigned int) strtoul(confid, NULL, 10);
	}

	/* Now extract user_call_data from the second multipart part. The
	 * phone always wraps datapassthroughreq in multipart/mixed; the
	 * x-cisco-remotecc-cm+xml part's body is a plain ASCII string. */
	full_body = rdata->msg_info.msg->body;
	if (!full_body
		|| !cisco_media_type_is(&full_body->content_type, "multipart",
			"mixed")) {
		/* No multipart wrapper — no user_call_data. Treat as a "menu
		 * refresh" (empty user_call_data; queue_conflist_action_task
		 * just re-emits the menu). */
		handled = 1;
		goto done;
	}
	pjsip_media_type_init2(&cm_type, "application",
		"x-cisco-remotecc-cm+xml");
	cm_part = pjsip_multipart_find_part(full_body, &cm_type, NULL);
	if (cm_part && cm_part->body && cm_part->body->data
		&& cm_part->body->len > 0
		&& cm_part->body->len < (int) sizeof(out->user_call_data)) {
		memcpy(out->user_call_data, cm_part->body->data,
			cm_part->body->len);
		out->user_call_data[cm_part->body->len] = '\0';
		ast_trim_blanks(out->user_call_data);
		/* ast_trim_blanks only NUL-terminates after trailing space; for
		 * leading whitespace use ast_skip_blanks-equivalent pattern. */
		{
			char *p = out->user_call_data;
			while (*p == ' ' || *p == '\t' || *p == '\r'
				|| *p == '\n') {
				p++;
			}
			if (p != out->user_call_data) {
				memmove(out->user_call_data, p,
					strlen(p) + 1);
			}
		}
	}

	handled = 1;

done:
	if (doc) {
		ast_xml_close(doc);
	}
	return handled;
}

static pj_bool_t conference_on_rx_request(pjsip_rx_data *rdata)
{
	struct ast_sip_endpoint *endpoint;
	struct cisco_endpoint *cisco;
	const char *endpoint_id;
	pjsip_msg_body *body;
	struct remotecc_softkey_msg msg;
	struct ast_sip_contact *contact;
	int keep_conference = 0;

	if (!rdata || !rdata->msg_info.msg
		|| rdata->msg_info.msg->type != PJSIP_REQUEST_MSG) {
		return PJ_FALSE;
	}

	if (pjsip_method_cmp(&rdata->msg_info.msg->line.req.method,
			pjsip_get_refer_method())) {
		return PJ_FALSE;
	}

	endpoint = cisco_rdata_get_endpoint(rdata);
	if (!endpoint) {
		return PJ_FALSE;
	}
	endpoint_id = ast_sorcery_object_get_id(endpoint);

	cisco = cisco_endpoint_get(endpoint_id);
	if (!cisco) {
		ao2_cleanup(endpoint);
		return PJ_FALSE;
	}
	keep_conference = cisco->keep_conference;
	ao2_cleanup(cisco);

	body = cisco_find_remotecc_request_body(rdata);
	if (!body) {
		ao2_cleanup(endpoint);
		return PJ_FALSE;
	}

	/* Try the softkeyeventmsg shape first (ConfList / Conference); if
	 * that doesn't classify, try the datapassthroughreq shape
	 * (ConfList action softkeys). One of the two must produce a non-NONE
	 * kind, otherwise it's a body the remotecc module handles instead. */
	if (!detect_remotecc_softkey(body, &msg) || msg.kind == REMOTECC_SOFTKEY_NONE) {
		if (!detect_conflist_action(rdata, body, &msg)) {
			ao2_cleanup(endpoint);
			return PJ_FALSE;
		}
	}

	/* Capture WHICH contact pressed the softkey. The rdata's source
	 * IP:port matches the host:port in exactly one of the endpoint's
	 * registered contacts (the phone that sent the REFER). The follow-up
	 * REFER / NOTIFY goes back to that one only — otherwise on a shared
	 * line every phone receives the response. */
	contact = cisco_endpoint_find_contact_from_rdata(endpoint, rdata);
	if (!contact) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed a softkey but source "
			"%s:%d doesn't match any registered contact — "
			"can't target the response\n", endpoint_id,
			rdata->pkt_info.src_name, rdata->pkt_info.src_port);
		cisco_send_refer_response(rdata, 202, endpoint);
		ao2_cleanup(endpoint);
		return PJ_TRUE;
	}

	switch (msg.kind) {
	case REMOTECC_SOFTKEY_CONFLIST:
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed ConfList from %s (callid=%s)\n",
			endpoint_id, contact->uri, msg.dialog_id.call_id);
		/* Stash the active-call dialog so subsequent action REFERs
		 * (which carry no <dialogid>, only <confid>) can relocate the
		 * conference bridge. */
		conflist_pending_set_dialog(endpoint_id, &msg.dialog_id);
		cisco_send_refer_response(rdata, 202, endpoint);
		queue_conflist(endpoint, contact, &msg.dialog_id);
		break;
	case REMOTECC_SOFTKEY_CONFLIST_ACTION:
		{
			struct conference_dialog_id stashed_dialog_id;

			if (!conflist_pending_lookup(endpoint_id,
					&stashed_dialog_id, NULL, 0)) {
				ast_log(LOG_NOTICE,
					"cisco-conference: %s sent a ConfList action "
					"(usercalldata='%s') but no prior ConfList press "
					"is on file — ignoring\n",
					endpoint_id, msg.user_call_data);
				cisco_send_refer_response(rdata, 202, endpoint);
				break;
			}
			ast_log(LOG_NOTICE,
				"cisco-conference: %s ConfList action from %s "
				"(usercalldata='%s', confid=%u)\n",
				endpoint_id, contact->uri,
				msg.user_call_data, msg.conf_id);
			cisco_send_refer_response(rdata, 202, endpoint);
			queue_conflist_action(endpoint, contact,
				&stashed_dialog_id, msg.user_call_data);
		}
		break;
	case REMOTECC_SOFTKEY_CONFERENCE:
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed Confrn from %s "
			"(active callid=%s, consult callid=%s)\n",
			endpoint_id, contact->uri,
			msg.dialog_id.call_id, msg.consult_dialog_id.call_id);
		{
			pjsip_dialog *dlg = conference_open_uas_dialog_and_202(
				rdata, endpoint_id);

			if (!dlg) {
				/* Fallback: bridge still gets built; phone UI just
				 * won't transition. We already logged why. */
				cisco_send_refer_response(rdata, 202, endpoint);
			}
			queue_conference(endpoint, contact, &msg.dialog_id,
				&msg.consult_dialog_id, dlg, keep_conference);
		}
		break;
	case REMOTECC_SOFTKEY_SELECT:
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed Select from %s "
			"(callid=%s)\n", endpoint_id, contact->uri,
			msg.dialog_id.call_id);
		cisco_selected_add(endpoint_id, &msg.dialog_id);
		cisco_send_refer_response(rdata, 202, endpoint);
		break;
	case REMOTECC_SOFTKEY_UNSELECT:
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed Unselect from %s "
			"(callid=%s)\n", endpoint_id, contact->uri,
			msg.dialog_id.call_id);
		cisco_selected_remove(endpoint_id, &msg.dialog_id);
		cisco_send_refer_response(rdata, 202, endpoint);
		break;
	case REMOTECC_SOFTKEY_JOIN:
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed Join from %s "
			"(active callid=%s)\n", endpoint_id, contact->uri,
			msg.dialog_id.call_id);
		cisco_send_refer_response(rdata, 202, endpoint);
		queue_join(endpoint, contact, &msg.dialog_id, keep_conference);
		break;
	case REMOTECC_SOFTKEY_RMLASTCONF:
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed RmLastConf from %s "
			"(callid=%s)\n", endpoint_id, contact->uri,
			msg.dialog_id.call_id);
		cisco_send_refer_response(rdata, 202, endpoint);
		queue_rmlastconf(endpoint, contact, &msg.dialog_id);
		break;
	default:
		/* Compiler completeness — kind == NONE already returned above. */
		break;
	}

	ao2_cleanup(contact);
	ao2_cleanup(endpoint);
	return PJ_TRUE;
}

pjsip_module conference_module = {
	.name             = { "cisco-conference", 16 },
	.id               = -1,
	/* One slot before res_pjsip_cisco_remotecc so we see ConfList
	 * REFERs first; non-ConfList traffic falls through to remotecc. */
	.priority         = PJSIP_MOD_PRIORITY_APPLICATION - 2,
	.on_rx_request    = conference_on_rx_request,
};

static int load_module(void)
{
	conference_serializer = ast_sip_create_serializer("pjsip/cisco-conference");
	if (!conference_serializer) {
		return AST_MODULE_LOAD_DECLINE;
	}

	conflist_pending_actions = ao2_container_alloc_hash(
		AO2_ALLOC_OPT_LOCK_MUTEX, 0, CONFLIST_PENDING_BUCKETS,
		conflist_pending_hash, NULL, conflist_pending_cmp);
	if (!conflist_pending_actions) {
		ast_taskprocessor_unreference(conference_serializer);
		conference_serializer = NULL;
		return AST_MODULE_LOAD_DECLINE;
	}

	cisco_selected_calls = ao2_container_alloc_hash(
		AO2_ALLOC_OPT_LOCK_MUTEX, 0, CISCO_SELECTED_BUCKETS,
		cisco_selected_hash, NULL, cisco_selected_cmp);
	if (!cisco_selected_calls) {
		ao2_cleanup(conflist_pending_actions);
		conflist_pending_actions = NULL;
		ast_taskprocessor_unreference(conference_serializer);
		conference_serializer = NULL;
		return AST_MODULE_LOAD_DECLINE;
	}

	if (ast_sip_register_service(&conference_module)) {
		ao2_cleanup(cisco_selected_calls);
		cisco_selected_calls = NULL;
		ao2_cleanup(conflist_pending_actions);
		conflist_pending_actions = NULL;
		ast_taskprocessor_unreference(conference_serializer);
		conference_serializer = NULL;
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	/* Refuse runtime unload — same rationale as every other cisco_*
	 * module: pjsip service unregister doesn't wait for in-flight
	 * callbacks on other servant threads, and the serializer may have
	 * pending menu sends queued. */
	if (!ast_shutdown_final()) {
		return -1;
	}

	ast_sip_unregister_service(&conference_module);
	ao2_cleanup(cisco_selected_calls);
	cisco_selected_calls = NULL;
	ao2_cleanup(conflist_pending_actions);
	conflist_pending_actions = NULL;
	ast_taskprocessor_unreference(conference_serializer);
	conference_serializer = NULL;
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco RemoteCC conference control",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_session,res_pjsip_cisco_endpoint",
);
