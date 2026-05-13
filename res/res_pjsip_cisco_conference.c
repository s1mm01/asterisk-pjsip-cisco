/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_conference
 *
 * Cisco Enterprise SIP conference control. Phase 1: read-only ConfList
 * softkey — when the user presses ConfList during an active call, the
 * phone sends an x-cisco-remotecc-request+xml REFER with
 * <softkeyeventmsg><softkeyevent>ConfList</softkeyevent>. We resolve
 * the supplied <dialogid> to the matching ast_sip_session, walk the
 * channel's bridge, and reply with a multipart REFER back to the phone
 * containing a Cisco proprietary <CiscoIPPhoneMenu> listing each
 * bridge participant.
 *
 * Wire shapes mirror the chan_sip cisco-usecallmanager patch's
 * channels/sip/conference.c:751 sip_conference_participants.
 *
 * Phase 1 is deliberately read-only:
 *   - Mute / Remove / Update softkeys on the menu are NOT yet wired up.
 *     Those arrive as datapassthroughreq REFERs with applicationid=1
 *     (SIP_REMOTECC_CONF_LIST) and a <usercalldata> field; this module
 *     does not yet handle them and they fall through to the regular
 *     RemoteCC handler (which 603 Declines).
 *   - We do not yet build conferences via Confrn — that's Phase 2.
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
#include "asterisk/bridge.h"
#include "asterisk/channel.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/taskprocessor.h"
#include "asterisk/xml.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/sorcery.h"

#include "cisco_endpoint.h"
#include "cisco_session.h"

/* application_id values mirror the chan_sip patch's
 * enum sip_remotecc_application in channels/sip/include/remotecc.h.
 * SIP_REMOTECC_NONE = 0, SIP_REMOTECC_CONF_LIST = 1. */
#define CONFERENCE_APP_ID_CONF_LIST 1

#define CONFERENCE_MAX_BODY 8192
#define CONFERENCE_MENU_PARTICIPANT_NAME 64

/* Header of the multipart REFER body we send back: a datapassthroughreq
 * echo so the phone correlates the response with its ConfList request,
 * followed by a CiscoIPPhoneMenu listing participants. The conference
 * id is synthesised from the bridge's pointer-derived counter — we
 * don't track Cisco-specific conference state in Phase 1. */
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

/* Phase 1: prompt only. Mute/Remove/Update softkey items omitted —
 * adding them would be misleading because we don't yet handle the
 * usercalldata REFERs they would generate. */
#define MENU_FOOTER                                                       \
	"  <Prompt>Phase 1 (read-only)</Prompt>\n"                      \
	"  <SoftKeyItem>\n"                                             \
	"    <Name>Exit</Name>\n"                                       \
	"    <Position>1</Position>\n"                                  \
	"    <URL>SoftKey:Exit</URL>\n"                                 \
	"  </SoftKeyItem>\n"                                            \
	"</CiscoIPPhoneMenu>\n"

static struct ast_taskprocessor *conference_serializer;

struct conference_dialog_id {
	char call_id[256];
	char local_tag[128];
	char remote_tag[128];
};

struct conflist_task_data {
	struct ast_sip_endpoint *endpoint;
	struct conference_dialog_id dialog_id;
};

static void conflist_task_data_destroy(void *obj)
{
	struct conflist_task_data *data = obj;
	ao2_cleanup(data->endpoint);
}

/*!
 * \brief Determine whether this REFER carries a ConfList request.
 *
 * Returns 1 and populates \a dialog_id from the XML body's <dialogid>
 * if so; returns 0 otherwise. ConfList currently arrives only as a
 * softkeyeventmsg with softkeyevent=ConfList — Phase 1 does not yet
 * handle the datapassthroughreq applicationid=1 path used by the
 * Mute / Remove / Update softkeys on the menu we send back.
 */
static int detect_conf_list(pjsip_msg_body *body,
	struct conference_dialog_id *dialog_id)
{
	struct ast_xml_doc *doc;
	struct ast_xml_node *root;
	struct ast_xml_node *softkey_msg;
	struct ast_xml_node *dialog_node;
	char softkey[64];
	int found = 0;

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
			sizeof(softkey)) || strcmp(softkey, "ConfList")) {
		goto done;
	}

	dialog_node = ast_xml_find_element(ast_xml_node_get_children(softkey_msg),
		"dialogid", NULL, NULL);
	if (!dialog_node) {
		goto done;
	}

	memset(dialog_id, 0, sizeof(*dialog_id));
	cisco_xml_copy_child_text(dialog_node, "callid", dialog_id->call_id,
		sizeof(dialog_id->call_id));
	cisco_xml_copy_child_text(dialog_node, "localtag", dialog_id->local_tag,
		sizeof(dialog_id->local_tag));
	cisco_xml_copy_child_text(dialog_node, "remotetag", dialog_id->remote_tag,
		sizeof(dialog_id->remote_tag));

	found = !ast_strlen_zero(dialog_id->call_id)
		&& !ast_strlen_zero(dialog_id->local_tag)
		&& !ast_strlen_zero(dialog_id->remote_tag);

done:
	ast_xml_close(doc);
	return found;
}

static struct ast_channel *bridge_peer_channel_ref(struct ast_channel *self,
	struct ast_bridge *bridge, int index)
{
	struct ao2_container *peers;
	struct ao2_iterator iter;
	struct ast_channel *peer;
	struct ast_channel *match = NULL;
	int i = 0;

	peers = ast_bridge_peers(bridge);
	if (!peers) {
		return NULL;
	}

	iter = ao2_iterator_init(peers, 0);
	while ((peer = ao2_iterator_next(&iter))) {
		if (peer == self) {
			ao2_cleanup(peer);
			continue;
		}
		if (i == index) {
			match = peer;  /* keep ref */
			break;
		}
		++i;
		ao2_cleanup(peer);
	}
	ao2_iterator_destroy(&iter);
	ao2_cleanup(peers);
	return match;
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
	ast_str_append(&menu, 0, MENU_FOOTER);

	cisco_remotecc_multipart_add_part(pool, multipart, ast_str_buffer(menu));
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

static void send_menu_to_contacts(struct ast_sip_endpoint *endpoint,
	struct ast_channel *self, struct ast_bridge *bridge,
	unsigned int conference_id)
{
	struct conflist_build_ctx ctx = {
		.self          = self,
		.bridge        = bridge,
		.conference_id = conference_id,
	};

	ast_log(LOG_NOTICE,
		"cisco-conference: pushing ConfList menu to %s\n",
		ast_sorcery_object_get_id(endpoint));

	cisco_endpoint_send_refer_to_all_contacts(endpoint,
		"cisco-conference", "cisco-conflist", "ConfList menu",
		conflist_build_adapter, &ctx, NULL, NULL);
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
	 * datapassthroughreq REFERs come in for Mute/Remove/Update — Phase 1
	 * doesn't yet handle those, but giving each bridge a deterministic
	 * id keeps Phase 2 wire-compatible. Truncating the bridge pointer
	 * isn't ideal but matches the chan_sip patch's per-conference
	 * counter shape. */
	conference_id = (unsigned int)(uintptr_t) bridge;

	send_menu_to_contacts(data->endpoint, channel, bridge, conference_id);

cleanup:
	ao2_cleanup(bridge);
	ao2_cleanup(channel);
	ao2_cleanup(session);
	ao2_cleanup(data);
	return 0;
}

static void queue_conflist(struct ast_sip_endpoint *endpoint,
	const struct conference_dialog_id *dialog_id)
{
	struct conflist_task_data *data;

	data = ao2_alloc(sizeof(*data), conflist_task_data_destroy);
	if (!data) {
		return;
	}

	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	data->dialog_id = *dialog_id;

	if (ast_sip_push_task(conference_serializer, conflist_send_task, data)) {
		ast_log(LOG_WARNING,
			"cisco-conference: failed to queue ConfList task\n");
		ao2_cleanup(data);
	}
}

static pj_bool_t conference_on_rx_request(pjsip_rx_data *rdata)
{
	struct ast_sip_endpoint *endpoint;
	struct cisco_endpoint *cisco;
	const char *endpoint_id;
	pjsip_msg_body *body;
	struct conference_dialog_id dialog_id;

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
	ao2_cleanup(cisco);

	body = cisco_find_remotecc_request_body(rdata);
	if (!body || !detect_conf_list(body, &dialog_id)) {
		ao2_cleanup(endpoint);
		return PJ_FALSE;
	}

	ast_log(LOG_NOTICE,
		"cisco-conference: %s pressed ConfList (callid=%s)\n",
		endpoint_id, dialog_id.call_id);

	cisco_send_refer_response(rdata, 202, endpoint);
	queue_conflist(endpoint, &dialog_id);

	ao2_cleanup(endpoint);
	return PJ_TRUE;
}

static pjsip_module conference_module = {
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

	if (ast_sip_register_service(&conference_module)) {
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
	ast_taskprocessor_unreference(conference_serializer);
	conference_serializer = NULL;
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco RemoteCC conference control (Phase 1: ConfList)",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_session,res_pjsip_cisco_endpoint",
);
