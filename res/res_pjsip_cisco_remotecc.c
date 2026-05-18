/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_remotecc
 *
 * Handles Cisco RemoteCC phone -> server REFER traffic. Cisco
 * Enterprise SIP firmware sends several proprietary REFER bodies with
 * Content-Type application/x-cisco-remotecc-*. This module claims
 * those REFERs for endpoints with a matching [name] type=cisco
 * section so the stock PJSIP REFER transfer handler does not try to
 * treat them as normal blind/attended transfers.
 *
 * Module entry + dispatch live in this file; per-feature handlers are
 * split out into sibling .c files (all compiled into one .so):
 *
 *   res/cisco_remotecc/mcid.c    — MCID softkey
 *   res/cisco_remotecc/park.c    — Park / ParkMonitor softkey
 *   res/cisco_remotecc/record.c  — StartRecording / StopRecording softkeys
 *
 * Implemented here (entry-side, small enough not to need its own file):
 *   - token-registration REFERs
 *   - alarm and remotecc-response REFERs
 *   - x-cisco-location notifications
 *   - softkeyeventmsg HLog, stored in astdb HuntGroup/<endpoint>
 *
 * Ack-only (202, no server-side action — matches chan_sip):
 *   - softkeyeventmsg Cancel. The chan_sip patch's dispatch-level
 *     Cancel handler is a bare 202 with no logic; the phone uses it
 *     as a UI-state signal when backing out of a softkey-driven flow,
 *     and the server just needs to acknowledge the REFER. (Feature-
 *     specific cancel semantics, like dismissing a queued CallBack,
 *     live inside the relevant feature handler — usercalldata="Cancel"
 *     in chan_sip's handle_remotecc_callback — not at the dispatch
 *     level. We don't implement CallBack, so no feature-level Cancel
 *     branch exists in this module.)
 *
 * Other RemoteCC softkeys are parsed and declined with 603 until their
 * underlying Asterisk integrations are ported.
 *
 * res_parking is an optional dep (.optional_modules, not .requires).
 * The parking symbols we touch (ast_parking_topic /
 * ast_parked_call_type / ast_parking_is_exten_park) live in the
 * asterisk binary, not res_parking.so, so the module loads cleanly
 * without it; handle_park gates on ast_module_check("res_parking.so")
 * and returns 501 if the actual park-bridge-feature isn't registered.
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
#include "asterisk/channel.h"
#include "asterisk/cli.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/taskprocessor.h"
#include "asterisk/astdb.h"
#include "asterisk/xml.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/sorcery.h"

#include "cisco/endpoint.h"
#include "cisco/rdata.h"
#include "cisco/refer.h"
#include "cisco/session.h"
#include "remotecc_private.h"

#define REMOTECC_MAX_BODY 8192

/* HLOG_UPDATE_FMT and the rest of the body templates live in
 * remotecc_private.h so the tests/unit/test_xml_bodies.c regression
 * pulls in the same strings and validates well-formedness. */

/*
 * Serializers shared with the per-feature handler files. Non-static
 * globals so remotecc_private.h can extern them; both are still hidden
 * outside the .so by the local: *; export script.
 *
 *   remotecc_serializer carries server->phone REFER/NOTIFY sends and
 *   the MixMonitor exec. The Park blind-transfer runs on
 *   park_serializer, not remotecc_serializer, so a stuck bridge op
 *   can't stall queued HLog/MCID/park-toast/orbit sends.
 */
struct ast_taskprocessor *remotecc_serializer;
struct ast_taskprocessor *park_serializer;

struct hlog_task_data {
	struct ast_sip_endpoint *endpoint;
	int huntgroup_in;
};

static void hlog_task_data_destroy(void *obj)
{
	struct hlog_task_data *data = obj;
	ao2_cleanup(data->endpoint);
}

/*
 * Local belt-and-suspenders Content-Type check used by the alarm
 * dispatch path (which feeds in non-remotecc types). The
 * x-cisco-remotecc-request+xml lookup is in cisco/endpoint.h's
 * cisco_find_remotecc_request_body() — this helper survives here only
 * because the alarm path also checks application/x-cisco-alarm+xml.
 */
static int request_content_type_is(pjsip_rx_data *rdata, const char *type,
	const char *subtype)
{
	if (!rdata || !rdata->msg_info.msg) {
		return 0;
	}

	if (rdata->msg_info.ctype
		&& cisco_media_type_is(&rdata->msg_info.ctype->media, type, subtype)) {
		return 1;
	}

	if (rdata->msg_info.msg->body
		&& cisco_media_type_is(&rdata->msg_info.msg->body->content_type,
			type, subtype)) {
		return 1;
	}

	return 0;
}

static pjsip_msg_body *make_hlog_update_body(pj_pool_t *pool, int huntgroup_in)
{
	pj_str_t type = pj_str("application");
	pj_str_t subtype = pj_str("x-cisco-remotecc-request+xml");
	pj_str_t text;
	char xml[512];

	snprintf(xml, sizeof(xml), HLOG_UPDATE_FMT, huntgroup_in ? "on" : "off");
	pj_strdup2(pool, &text, xml);

	return pjsip_msg_body_create(pool, &type, &subtype, &text);
}

static pjsip_msg_body *hlog_build_adapter(pj_pool_t *pool, void *vctx)
{
	struct hlog_task_data *data = vctx;
	return make_hlog_update_body(pool, data->huntgroup_in);
}

static int hlog_send_task(void *obj)
{
	struct hlog_task_data *data = obj;
	const char *endpoint_id;

	endpoint_id = ast_sorcery_object_get_id(data->endpoint);

	ast_log(LOG_NOTICE, "cisco-remotecc: pushing HLog %s for %s\n",
		data->huntgroup_in ? "on" : "off", endpoint_id);

	cisco_endpoint_send_refer_to_all_contacts(data->endpoint,
		"cisco-remotecc", "cisco-hlog", "HLog update",
		hlog_build_adapter, data, NULL, NULL);

	ao2_cleanup(data);
	return 0;
}

static void queue_hlog_update(struct ast_sip_endpoint *endpoint, int huntgroup_in)
{
	struct hlog_task_data *data;

	data = ao2_alloc(sizeof(*data), hlog_task_data_destroy);
	if (!data) {
		return;
	}

	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	data->huntgroup_in = huntgroup_in;

	if (ast_sip_push_task(remotecc_serializer, hlog_send_task, data)) {
		ast_log(LOG_WARNING,
			"cisco-remotecc: failed to queue HLog update task\n");
		ao2_cleanup(data);
	}
}

static int header_value_matches(pjsip_rx_data *rdata, const char *name,
	const char *value)
{
	pj_str_t hdr_name;
	pjsip_generic_string_hdr *hdr;
	char buf[256];
	char *trimmed;

	pj_cstr(&hdr_name, name);
	hdr = (pjsip_generic_string_hdr *) pjsip_msg_find_hdr_by_name(
		rdata->msg_info.msg, &hdr_name, NULL);
	if (!hdr) {
		return 0;
	}

	ast_copy_pj_str(buf, &hdr->hvalue, sizeof(buf));
	trimmed = ast_strip(buf);

	return !strcasecmp(trimmed, value);
}

static int copy_dialog_id(struct ast_xml_node *parent,
	struct remotecc_dialog_id *dialog_id)
{
	struct ast_xml_node *dialog_node;

	if (!parent || !dialog_id) {
		return 0;
	}

	memset(dialog_id, 0, sizeof(*dialog_id));
	dialog_node = ast_xml_find_element(ast_xml_node_get_children(parent),
		"dialogid", NULL, NULL);
	if (!dialog_node) {
		return 0;
	}

	cisco_xml_copy_child_text(dialog_node, "callid", dialog_id->call_id,
		sizeof(dialog_id->call_id));
	cisco_xml_copy_child_text(dialog_node, "localtag", dialog_id->local_tag,
		sizeof(dialog_id->local_tag));
	cisco_xml_copy_child_text(dialog_node, "remotetag", dialog_id->remote_tag,
		sizeof(dialog_id->remote_tag));

	return !ast_strlen_zero(dialog_id->call_id)
		&& !ast_strlen_zero(dialog_id->local_tag)
		&& !ast_strlen_zero(dialog_id->remote_tag);
}

static int handle_softkey_event(struct ast_sip_endpoint *endpoint,
	const char *endpoint_id, const char *softkey,
	const struct remotecc_dialog_id *dialog_id)
{
	int hlog_in;

	if (ast_strlen_zero(softkey)) {
		return 400;
	}

	if (!strcmp(softkey, "Cancel")) {
		/* chan_sip parity (patch line 5767): dispatch-level Cancel is
		 * a bare 202 with no server-side action. The phone uses this
		 * softkey as a UI-state signal when leaving a softkey-driven
		 * flow; any feature-level cancel semantics live inside the
		 * relevant feature handler, not here. */
		return 202;
	}

	if (!strcmp(softkey, "MCID")) {
		return handle_mcid(endpoint, endpoint_id, dialog_id);
	}

	if (!strcmp(softkey, "Park") || !strcmp(softkey, "ParkMonitor")) {
		return handle_park(endpoint, endpoint_id, dialog_id,
			!strcmp(softkey, "ParkMonitor"));
	}

	if (!strcmp(softkey, "StartRecording")
		|| !strcmp(softkey, "StopRecording")) {
		return handle_record(endpoint, endpoint_id, dialog_id,
			!strcmp(softkey, "StartRecording"));
	}

	if (!strcmp(softkey, "HLog")) {
		hlog_in = !cisco_huntgroup_is_in(endpoint_id);
		cisco_huntgroup_set(endpoint_id, hlog_in);
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s set hunt-group login %s (HLog softkey)\n",
			endpoint_id, hlog_in ? "on" : "off");
		queue_hlog_update(endpoint, hlog_in);
		return 202;
	}

	ast_log(LOG_NOTICE,
		"cisco-remotecc: %s sent unsupported softkey '%s'\n",
		endpoint_id, softkey);
	return 603;
}

static int handle_remotecc_xml(struct ast_sip_endpoint *endpoint,
	const char *endpoint_id, pjsip_msg_body *body)
{
	struct ast_xml_doc *doc;
	struct ast_xml_node *root;
	struct ast_xml_node *softkey_msg;
	int response_code = 400;

	if (!body || !body->data || body->len == 0) {
		return 400;
	}

	if (body->len > REMOTECC_MAX_BODY) {
		ast_log(LOG_WARNING,
			"cisco-remotecc: rejecting oversized request body (%u bytes)\n",
			(unsigned) body->len);
		return 413;
	}

	doc = cisco_xml_read_body(body);
	if (!doc) {
		ast_debug(2, "cisco-remotecc: XML parse failed\n");
		return 400;
	}

	root = ast_xml_get_root(doc);
	if (!root || strcasecmp(ast_xml_node_get_name(root),
			"x-cisco-remotecc-request")) {
		ast_debug(2, "cisco-remotecc: missing x-cisco-remotecc-request root\n");
		response_code = 400;
		goto done;
	}

	softkey_msg = ast_xml_find_element(ast_xml_node_get_children(root),
		"softkeyeventmsg", NULL, NULL);
	if (softkey_msg) {
		char softkey[64];
		struct remotecc_dialog_id dialog_id;

		cisco_xml_copy_child_text(softkey_msg, "softkeyevent", softkey,
			sizeof(softkey));
		copy_dialog_id(softkey_msg, &dialog_id);
		response_code = handle_softkey_event(endpoint, endpoint_id,
			softkey, &dialog_id);
		goto done;
	}

	if (ast_xml_find_element(ast_xml_node_get_children(root),
			"x-cisco-location", NULL, NULL)) {
		ast_debug(2,
			"cisco-remotecc: %s sent x-cisco-location notification\n",
			endpoint_id);
		response_code = 202;
		goto done;
	}

	if (ast_xml_find_element(ast_xml_node_get_children(root),
			"datapassthroughreq", NULL, NULL)) {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s sent unsupported datapassthrough request\n",
			endpoint_id);
		response_code = 603;
		goto done;
	}

	ast_log(LOG_NOTICE,
		"cisco-remotecc: %s sent unsupported request element under %s\n",
		endpoint_id, ast_xml_node_get_name(root));
	response_code = 603;

done:
	ast_xml_close(doc);
	return response_code;
}

static pj_bool_t remotecc_on_rx_request(pjsip_rx_data *rdata)
{
	struct ast_sip_endpoint *endpoint;
	struct cisco_endpoint *cisco;
	const char *endpoint_id;
	pjsip_msg_body *remotecc_body;
	int response_code;

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

	if (header_value_matches(rdata, "Refer-To",
			"<urn:x-cisco-remotecc:token-registration>")) {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: accepted token registration from %s\n",
			endpoint_id);
		cisco_send_refer_response(rdata, 202, endpoint);
		ao2_cleanup(endpoint);
		return PJ_TRUE;
	}

	if (request_content_type_is(rdata, "application", "x-cisco-alarm+xml")) {
		/* Phone-originated alarm event (firmware-reported device-side
		 * condition: registration loss, line-state change, error,
		 * etc.). Worth a NOTICE — these correspond to real things
		 * happening on the device. */
		ast_log(LOG_NOTICE,
			"cisco-remotecc: accepted alarm from %s\n", endpoint_id);
		cisco_send_refer_response(rdata, 202, endpoint);
		ao2_cleanup(endpoint);
		return PJ_TRUE;
	}

	if (request_content_type_is(rdata, "application",
			"x-cisco-remotecc-response+xml")) {
		/* The phone's 202-style ack of a REFER we sent (bulkupdate,
		 * optionsind, unsolicited NOTIFY, service-control, etc.).
		 * Routine: one per outgoing REFER per registered contact —
		 * a single bulkupdate against an endpoint with N contacts
		 * generates N of these, every REGISTER refresh. ast_debug
		 * rather than NOTICE so it doesn't dominate the log. */
		ast_debug(2,
			"cisco-remotecc: accepted remotecc-response from %s\n",
			endpoint_id);
		cisco_send_refer_response(rdata, 202, endpoint);
		ao2_cleanup(endpoint);
		return PJ_TRUE;
	}

	remotecc_body = cisco_find_remotecc_request_body(rdata);
	if (!remotecc_body) {
		ao2_cleanup(endpoint);
		return PJ_FALSE;
	}

	response_code = handle_remotecc_xml(endpoint, endpoint_id, remotecc_body);
	cisco_send_refer_response(rdata, response_code, endpoint);

	ao2_cleanup(endpoint);
	return PJ_TRUE;
}

static pjsip_module remotecc_module = {
	.name             = { "cisco-remotecc", 14 },
	.id               = -1,
	/* After authentication (-2), before out-of-dialog/session REFER hooks. */
	.priority         = PJSIP_MOD_PRIORITY_APPLICATION - 1,
	.on_rx_request    = remotecc_on_rx_request,
};

static int load_module(void)
{
	remotecc_serializer = ast_sip_create_serializer("pjsip/cisco-remotecc");
	if (!remotecc_serializer) {
		return AST_MODULE_LOAD_DECLINE;
	}
	park_serializer = ast_sip_create_serializer("pjsip/cisco-park");
	if (!park_serializer) {
		ast_taskprocessor_unreference(remotecc_serializer);
		remotecc_serializer = NULL;
		return AST_MODULE_LOAD_DECLINE;
	}

	if (ast_sip_register_service(&remotecc_module)) {
		ast_taskprocessor_unreference(park_serializer);
		park_serializer = NULL;
		ast_taskprocessor_unreference(remotecc_serializer);
		remotecc_serializer = NULL;
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	if (!ast_shutdown_final()) {
		return -1;
	}

	ast_sip_unregister_service(&remotecc_module);
	ast_taskprocessor_unreference(park_serializer);
	park_serializer = NULL;
	ast_taskprocessor_unreference(remotecc_serializer);
	remotecc_serializer = NULL;
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco RemoteCC REFER handler",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_session,res_pjsip_cisco_endpoint",
	/* res_parking is optional. Without it, the Park softkey returns
	 * 501 (handle_park gates on ast_module_check). The parking
	 * symbols we touch live in the asterisk binary itself, so this
	 * .so links + loads cleanly without res_parking; .optional_modules
	 * just ensures res_parking loads first when it IS configured. */
	.optional_modules = "res_parking",
);
