/*
 * Cisco ConfList softkey machinery: build + send the participant menu
 * back to the phone in response to ConfList, plus the Mute / Remove /
 * Update action softkey two-step state machine. Compiled into
 * res_pjsip_cisco_conference.so.
 *
 * The action softkeys follow chan_sip's sip_conference_participants
 * shape:
 *   1. Phone presses ConfList                → we emit menu.
 *   2. Phone presses Remove/Mute softkey     → we STORE the action in
 *      conflist_pending_actions keyed by endpoint and re-emit the menu.
 *   3. Phone clicks a participant            → we CONSUME the pending
 *      action (default Mute if none) and apply it to the matching
 *      bridge peer, then re-emit the menu.
 *
 * "Update" is just a menu refresh.
 */

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip/sip_multipart.h>
#include <pjlib.h>

#include "asterisk/astobj2.h"
#include "asterisk/bridge.h"
#include "asterisk/bridge_channel.h"
#include "asterisk/bridge_features.h"
#include "asterisk/channel.h"
#include "asterisk/callerid.h"
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

/* Header of the multipart REFER body we send back: a datapassthroughreq
 * echo so the phone correlates the response with its ConfList request,
 * followed by a CiscoIPPhoneMenu listing participants. The conference
 * id is synthesised from the bridge's pointer-derived counter. */
#define MENU_DATAPASSTHROUGH_FMT                                          \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                  \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <datapassthroughreq>\n"                                      \
	"    <applicationid>%d</applicationid>\n"                       \
	"    <transactionid>0</transactionid>\n"                        \
	"    <stationsequence>StationSequenceLast</stationsequence>\n"  \
	"    <displaypriority>2</displaypriority>\n"                    \
	"    <appinstance>0</appinstance>\n"                            \
	"    <routingid>0</routingid>\n"                                \
	"    <confid>%u</confid>\n"                                     \
	"  </datapassthroughreq>\n"                                     \
	"</x-cisco-remotecc-request>\n"

#define MENU_HEADER_FMT                                                   \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                  \
	"<CiscoIPPhoneMenu>\n"                                          \
	"  <Title>Conference</Title>\n"

#define MENU_ITEM_FMT                                                     \
	"  <MenuItem>\n"                                                \
	"    <Name>%s</Name>\n"                                         \
	"    <URL>UserCallData:%d:0:%u:0:%d</URL>\n"                    \
	"  </MenuItem>\n"

/* Chan_sip patch uses UserCallDataSoftKey:Select:... for the sticky
 * action softkeys (the phone sends an "action-set" REFER, then on the
 * next participant click an "apply" REFER) and UserCallDataSoftKey:
 * Update:... for the one-shot menu refresh. confid is the bridge-derived
 * synthetic id we put in <confid> in the part-1 echo above; the phone
 * sends it back so callers correlate stateful traffic to a conference. */
#define MENU_FOOTER_FMT                                                   \
	"  <Prompt>Please select</Prompt>\n"                            \
	"  <SoftKeyItem>\n"                                             \
	"    <Name>Exit</Name>\n"                                       \
	"    <Position>1</Position>\n"                                  \
	"    <URL>SoftKey:Exit</URL>\n"                                 \
	"  </SoftKeyItem>\n"                                            \
	"  <SoftKeyItem>\n"                                             \
	"    <Name>Remove</Name>\n"                                     \
	"    <Position>2</Position>\n"                                  \
	"    <URL>UserCallDataSoftKey:Select:%d:0:%u:0:Remove</URL>\n"  \
	"  </SoftKeyItem>\n"                                            \
	"  <SoftKeyItem>\n"                                             \
	"    <Name>Mute</Name>\n"                                       \
	"    <Position>3</Position>\n"                                  \
	"    <URL>UserCallDataSoftKey:Select:%d:0:%u:0:Mute</URL>\n"    \
	"  </SoftKeyItem>\n"                                            \
	"  <SoftKeyItem>\n"                                             \
	"    <Name>Update</Name>\n"                                     \
	"    <Position>4</Position>\n"                                  \
	"    <URL>UserCallDataSoftKey:Update:%d:0:%u:0:Update</URL>\n"  \
	"  </SoftKeyItem>\n"                                            \
	"</CiscoIPPhoneMenu>\n"

struct conflist_task_data {
	struct ast_sip_endpoint *endpoint;
	/* The specific contact that pressed ConfList. Captured at rx time
	 * (rdata is gone by the time the task runs) and used to send the
	 * menu REFER back to only that phone — not all contacts of a
	 * shared-line AOR. */
	struct ast_sip_contact *contact;
	struct conference_dialog_id dialog_id;
};

static void conflist_task_data_destroy(void *obj)
{
	struct conflist_task_data *data = obj;
	ao2_cleanup(data->contact);
	ao2_cleanup(data->endpoint);
}

static void append_menu_item(struct ast_str **out, struct ast_channel *peer,
	unsigned int conference_id, int index)
{
	char raw_name[CONFERENCE_MENU_PARTICIPANT_NAME];
	char escaped_name[CONFERENCE_MENU_PARTICIPANT_NAME * 5 + 1];
	struct ast_party_caller *caller;

	raw_name[0] = '\0';

	ast_channel_lock(peer);
	caller = ast_channel_caller(peer);
	if (caller->id.name.valid && !ast_strlen_zero(caller->id.name.str)) {
		ast_copy_string(raw_name, caller->id.name.str, sizeof(raw_name));
	} else if (caller->id.number.valid && !ast_strlen_zero(caller->id.number.str)) {
		ast_copy_string(raw_name, caller->id.number.str, sizeof(raw_name));
	} else {
		snprintf(raw_name, sizeof(raw_name), "Anonymous %d", index);
	}
	ast_channel_unlock(peer);

	if (ast_xml_escape(raw_name, escaped_name, sizeof(escaped_name))) {
		/* Source somehow grew past 5x — fall back to a placeholder so
		 * the menu doesn't get truncated mid-element. */
		snprintf(escaped_name, sizeof(escaped_name), "Participant %d", index);
	}

	ast_str_append(out, 0, MENU_ITEM_FMT, escaped_name,
		CONFERENCE_APP_ID_CONF_LIST, conference_id, index);
}

/*!
 * \brief Build the multipart REFER body listing bridge participants.
 *
 * \param pool          tdata pool the body is allocated from
 * \param self          the channel that sent ConfList (excluded from the menu)
 * \param bridge        the bridge enumerated for participants
 * \param conference_id synthetic id used in the URL action of each MenuItem
 * \retval NULL on alloc / format failure
 */
static pjsip_msg_body *make_menu_body(pj_pool_t *pool, struct ast_channel *self,
	struct ast_bridge *bridge, unsigned int conference_id)
{
	pj_str_t boundary = pj_str("uniqueBoundary");
	pjsip_msg_body *multipart;
	struct ast_str *menu;
	struct ast_str *header;
	int index;
	struct ast_channel *peer;

	multipart = pjsip_multipart_create(pool, NULL, &boundary);
	if (!multipart) {
		return NULL;
	}

	header = ast_str_create(512);
	if (!header) {
		return NULL;
	}
	ast_str_set(&header, 0, MENU_DATAPASSTHROUGH_FMT,
		CONFERENCE_APP_ID_CONF_LIST, conference_id);
	cisco_remotecc_multipart_add_part(pool, multipart, ast_str_buffer(header));
	ast_free(header);

	menu = ast_str_create(2048);
	if (!menu) {
		return NULL;
	}
	ast_str_set(&menu, 0, MENU_HEADER_FMT);

	for (index = 0; (peer = bridge_peer_channel_ref(self, bridge, index)); ++index) {
		append_menu_item(&menu, peer, conference_id, index);
		ast_channel_unref(peer);
	}

	if (!index) {
		ast_str_append(&menu, 0,
			"  <MenuItem>\n"
			"    <Name>(no other participants)</Name>\n"
			"    <URL>SoftKey:Exit</URL>\n"
			"  </MenuItem>\n");
	}
	ast_str_append(&menu, 0, MENU_FOOTER_FMT,
		CONFERENCE_APP_ID_CONF_LIST, conference_id,
		CONFERENCE_APP_ID_CONF_LIST, conference_id,
		CONFERENCE_APP_ID_CONF_LIST, conference_id);

	/* The menu part uses application/x-cisco-remotecc-cm+xml, NOT the
	 * shared helper's default application/x-cisco-remotecc-request+xml.
	 * Cisco firmware looks at the second part's content-type to decide
	 * whether to render the CiscoIPPhoneMenu; mislabelling it as
	 * x-cisco-remotecc-request+xml silently drops the menu (which is
	 * how this looked: REFER 202'd by firmware, no menu rendered). The
	 * chan_sip patch (handle_remotecc_conflist) gets this right. So
	 * inline pjsip_multipart_create_part here rather than using the
	 * cisco_remotecc_multipart_add_part shared helper. */
	{
		pj_str_t menu_type    = pj_str("application");
		pj_str_t menu_subtype = pj_str("x-cisco-remotecc-cm+xml");
		pj_str_t menu_text;
		pjsip_multipart_part *menu_part;

		pj_strdup2(pool, &menu_text, ast_str_buffer(menu));
		menu_part = pjsip_multipart_create_part(pool);
		if (menu_part) {
			menu_part->body = pjsip_msg_body_create(pool,
				&menu_type, &menu_subtype, &menu_text);
			pjsip_multipart_add_part(pool, multipart, menu_part);
		}
	}
	ast_free(menu);

	return multipart;
}

struct conflist_build_ctx {
	struct ast_channel *self;
	struct ast_bridge *bridge;
	unsigned int conference_id;
};

static pjsip_msg_body *conflist_build_adapter(pj_pool_t *pool, void *vctx)
{
	struct conflist_build_ctx *ctx = vctx;
	return make_menu_body(pool, ctx->self, ctx->bridge, ctx->conference_id);
}

static void send_menu_to_contact(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	struct ast_channel *self, struct ast_bridge *bridge,
	unsigned int conference_id)
{
	struct conflist_build_ctx ctx = {
		.self          = self,
		.bridge        = bridge,
		.conference_id = conference_id,
	};

	ast_log(LOG_NOTICE,
		"cisco-conference: pushing ConfList menu to %s @ %s\n",
		ast_sorcery_object_get_id(endpoint), contact->uri);

	cisco_endpoint_send_refer_to_contact(endpoint, contact,
		"cisco-conference", "cisco-conflist", "ConfList menu",
		conflist_build_adapter, &ctx);
}

static int conflist_send_task(void *obj)
{
	struct conflist_task_data *data = obj;
	struct ast_sip_session *session;
	struct ast_channel *channel = NULL;
	struct ast_bridge *bridge = NULL;
	const char *endpoint_id;
	unsigned int conference_id;

	endpoint_id = ast_sorcery_object_get_id(data->endpoint);

	session = cisco_dialog_session_lookup(data->dialog_id.call_id,
		data->dialog_id.local_tag, data->dialog_id.remote_tag);
	if (!session) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s sent ConfList for unknown dialog "
			"(callid=%s localtag=%s remotetag=%s)\n",
			endpoint_id, data->dialog_id.call_id,
			data->dialog_id.local_tag, data->dialog_id.remote_tag);
		goto cleanup;
	}

	channel = cisco_session_channel_ref(session);
	if (!channel) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s sent ConfList for dialog with no channel\n",
			endpoint_id);
		goto cleanup;
	}

	ast_channel_lock(channel);
	bridge = ast_channel_get_bridge(channel);
	ast_channel_unlock(channel);

	if (!bridge) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed ConfList outside any bridge\n",
			endpoint_id);
		goto cleanup;
	}

	/* Synthesise a stable per-bridge id for the menu URLs. The menu's
	 * <confid> is consulted by the firmware when subsequent
	 * datapassthroughreq REFERs come in for Mute/Remove/Update. Truncating
	 * the bridge pointer isn't ideal but matches the chan_sip patch's
	 * per-conference counter shape. */
	conference_id = (unsigned int)(uintptr_t) bridge;

	send_menu_to_contact(data->endpoint, data->contact,
		channel, bridge, conference_id);

cleanup:
	ao2_cleanup(bridge);
	ao2_cleanup(channel);
	ao2_cleanup(session);
	ao2_cleanup(data);
	return 0;
}

void queue_conflist(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *dialog_id)
{
	struct conflist_task_data *data;

	data = ao2_alloc(sizeof(*data), conflist_task_data_destroy);
	if (!data) {
		return;
	}

	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	ao2_ref(contact, +1);
	data->contact = contact;
	data->dialog_id = *dialog_id;

	if (ast_sip_push_task(conference_serializer, conflist_send_task, data)) {
		ast_log(LOG_WARNING,
			"cisco-conference: failed to queue ConfList task\n");
		ao2_cleanup(data);
	}
}

struct conflist_action_task_data {
	struct ast_sip_endpoint *endpoint;
	struct ast_sip_contact *contact;
	struct conference_dialog_id dialog_id;
	char user_call_data[64];
};

static void conflist_action_task_data_destroy(void *obj)
{
	struct conflist_action_task_data *data = obj;
	ao2_cleanup(data->contact);
	ao2_cleanup(data->endpoint);
}

/*!
 * \brief Toggle mute on the bridge_channel that owns \a peer.
 *
 * \retval 0 on success (mute flag toggled, log line emitted)
 * \retval -1 if peer has no bridge_channel (already left).
 */
static int apply_participant_mute(struct ast_channel *peer,
	const char *endpoint_id)
{
	struct ast_bridge_channel *bc;
	int now_muted;

	bc = ast_channel_get_bridge_channel(peer);
	if (!bc) {
		return -1;
	}
	ast_bridge_channel_lock(bc);
	bc->features->mute = !bc->features->mute;
	now_muted = bc->features->mute;
	ast_bridge_channel_unlock(bc);
	ao2_ref(bc, -1);

	ast_log(LOG_NOTICE,
		"cisco-conference: %s — ConfList %s %s\n",
		endpoint_id, now_muted ? "muted" : "unmuted",
		ast_channel_name(peer));
	return 0;
}

/*!
 * \brief Pull \a peer out of the conference bridge. chan_pjsip's
 *        teardown then BYE's the dialog.
 */
static int apply_participant_remove(struct ast_bridge *bridge,
	struct ast_channel *peer, const char *endpoint_id)
{
	if (ast_bridge_remove(bridge, peer)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — ConfList remove failed for %s "
			"(channel may have already left)\n",
			endpoint_id, ast_channel_name(peer));
		return -1;
	}
	ast_log(LOG_NOTICE,
		"cisco-conference: %s — ConfList removed %s from bridge\n",
		endpoint_id, ast_channel_name(peer));
	return 0;
}

static int conflist_action_send_task(void *obj)
{
	struct conflist_action_task_data *data = obj;
	struct ast_sip_session *session;
	struct ast_channel *channel = NULL;
	struct ast_bridge *bridge = NULL;
	struct ast_channel *target_peer = NULL;
	const char *endpoint_id;
	const char *ucd;
	unsigned int conference_id;

	endpoint_id = ast_sorcery_object_get_id(data->endpoint);
	ucd         = data->user_call_data;

	session = cisco_dialog_session_lookup(data->dialog_id.call_id,
		data->dialog_id.local_tag, data->dialog_id.remote_tag);
	if (!session) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s ConfList action for unknown dialog "
			"(callid=%s)\n", endpoint_id, data->dialog_id.call_id);
		goto cleanup;
	}
	channel = cisco_session_channel_ref(session);
	if (!channel) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s ConfList action: dialog has no "
			"channel\n", endpoint_id);
		goto cleanup;
	}
	ast_channel_lock(channel);
	bridge = ast_channel_get_bridge(channel);
	ast_channel_unlock(channel);
	if (!bridge) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s ConfList action: channel is not "
			"in a bridge\n", endpoint_id);
		goto cleanup;
	}

	/* Dispatch on user_call_data. */
	if (!strcmp(ucd, "Mute") || !strcmp(ucd, "Remove")) {
		/* Sticky softkey: remember which action the next participant
		 * pick should trigger. */
		conflist_pending_set_action(endpoint_id, ucd);
		ast_log(LOG_NOTICE,
			"cisco-conference: %s ConfList queued %s for next "
			"participant pick\n", endpoint_id, ucd);
	} else if (ast_strlen_zero(ucd) || !strcmp(ucd, "Update")) {
		/* Just a menu refresh — nothing to do. */
	} else {
		/* Numeric participant index. chan_sip default: if no pending
		 * action was set, treat as Mute. */
		char action[16] = "";
		int idx;

		conflist_pending_lookup(endpoint_id, NULL, action,
			sizeof(action));
		if (ast_strlen_zero(action)) {
			ast_copy_string(action, "Mute", sizeof(action));
		}

		idx = atoi(ucd);
		target_peer = bridge_peer_channel_ref(channel, bridge, idx);
		if (!target_peer) {
			ast_log(LOG_NOTICE,
				"cisco-conference: %s ConfList %s requested for "
				"participant %d but that index isn't in the "
				"bridge (someone left between menu emit and "
				"click?)\n", endpoint_id, action, idx);
		} else if (!strcmp(action, "Remove")) {
			apply_participant_remove(bridge, target_peer,
				endpoint_id);
		} else {
			/* Default + explicit Mute path */
			apply_participant_mute(target_peer, endpoint_id);
		}
	}

	/* Re-emit the menu so the user sees the post-action state. */
	conference_id = (unsigned int)(uintptr_t) bridge;
	send_menu_to_contact(data->endpoint, data->contact, channel, bridge,
		conference_id);

cleanup:
	ast_channel_cleanup(target_peer);
	ao2_cleanup(bridge);
	ao2_cleanup(channel);
	ao2_cleanup(session);
	ao2_cleanup(data);
	return 0;
}

void queue_conflist_action(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *dialog_id,
	const char *user_call_data)
{
	struct conflist_action_task_data *data;

	data = ao2_alloc(sizeof(*data), conflist_action_task_data_destroy);
	if (!data) {
		return;
	}
	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	ao2_ref(contact, +1);
	data->contact = contact;
	data->dialog_id = *dialog_id;
	ast_copy_string(data->user_call_data,
		S_OR(user_call_data, ""), sizeof(data->user_call_data));

	if (ast_sip_push_task(conference_serializer, conflist_action_send_task,
			data)) {
		ast_log(LOG_WARNING,
			"cisco-conference: failed to queue ConfList action task\n");
		ao2_cleanup(data);
	}
}
