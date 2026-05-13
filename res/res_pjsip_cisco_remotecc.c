/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_remotecc
 *
 * Handles the first slice of Cisco RemoteCC phone -> server REFER
 * traffic. Cisco Enterprise SIP firmware sends several proprietary
 * REFER bodies with Content-Type application/x-cisco-remotecc-*. This
 * module claims those REFERs for endpoints with a matching [name]
 * type=cisco section so the stock PJSIP REFER transfer handler does
 * not try to treat them as normal blind/attended transfers.
 *
 * Implemented in this first pass:
 *   - token-registration REFERs
 *   - alarm and remotecc-response REFERs
 *   - x-cisco-location notifications
 *   - softkeyeventmsg HLog, stored in astdb HuntGroup/<endpoint>
 *   - softkeyeventmsg MCID, queued as AST_CONTROL_MCID on the
 *     dialog-matched channel when the call is bridged
 *   - softkeyeventmsg Park / ParkMonitor: the call named by <dialogid>
 *     is parked by blind-transferring the bridge peer to the parkext
 *     (700, res_parking's parkext) in the parker's transfer context —
 *     the same path a plain "transfer to 700" takes, so res_parking
 *     handles the slot allocation, comeback-to-origin and the parker-leg
 *     teardown. The blind transfer runs on its own serializer
 *     (pjsip/cisco-park), not the SIP rx thread, so a slow/stuck bridge
 *     op can't wedge SIP processing (the rx handler just 202s the REFER
 *     and queues the work). We subscribe to ast_parking_topic() to learn
 *     the slot, then: for Park, push the phone one <statuslineupdatereq>
 *     REFER toast announcing the slot; for ParkMonitor, push an
 *     "Event: refer" / application/dialog-info+xml NOTIFY for every
 *     parked-call lifecycle event (parked / retrieved / forwarded /
 *     abandoned / error) so a "Park slot N" BLF button tracks the
 *     orbit. (Earlier this used ast_bridge_channel_write_park
 *     inline on the rx thread — line-mapped from chan_sip's park_thread —
 *     which left the parker leg stuck and could wedge the phone.)
 *
 * Stubbed — accepted on the wire (202) but no Asterisk-side action yet:
 *   - softkeyeventmsg Cancel. Should cancel the in-progress operation
 *     identified by <dialogid> (e.g. a transfer the user started but
 *     hasn't completed). Needs an active-session lookup via
 *     cisco_dialog_session_lookup() plus the corresponding
 *     ast_sip_session_terminate / channel-side cancel. See the TODO
 *     at handle_softkey_event.
 *
 * Other RemoteCC softkeys are parsed and declined with 603 until their
 * underlying Asterisk integrations are ported.
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
#include "asterisk/pbx.h"
#include "asterisk/stasis.h"
#include "asterisk/parking.h"
#include "asterisk/stasis_channels.h"
#include "asterisk/bridge.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/sorcery.h"

#include "cisco_endpoint.h"
#include "cisco_session.h"

#define REMOTECC_MAX_BODY 8192

#define HLOG_UPDATE_FMT                                                  \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                 \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <hlogupdate>\n"                                              \
	"    <status>%s</status>\n"                                     \
	"  </hlogupdate>\n"                                             \
	"</x-cisco-remotecc-request>\n"

/*
 * MCID status-line update body. Constants here are wire-fidelity to
 * the chan_sip cisco-usecallmanager patch's MCID emitter
 * (channels/sip/sip_remotecc_handler.c sip_remotecc_handle_mcid):
 *
 *   <statustext>\200T</statustext>     Cisco-private inline-format
 *                                       escape — the phone translates
 *                                       \200T into a localised "trace"
 *                                       glyph on the status line. The
 *                                       byte is not UTF-8, which is
 *                                       why this sub-body intentionally
 *                                       omits encoding="UTF-8" on the
 *                                       XML declaration.
 *   <displaytimeout>7</displaytimeout>  seconds the glyph stays on
 *                                       screen before the firmware
 *                                       restores the previous text.
 *   <linenumber>0</linenumber>          0 = active line. The firmware
 *                                       resolves the line from the
 *                                       <dialogid> regardless.
 *   <priority>1</priority>              status-line priority slot.
 *
 * Don't tune these without checking the patch — values are paired
 * with firmware behaviour, not configurable knobs.
 */
#define MCID_STATUS_PART_FMT                                             \
	"<?xml version=\"1.0\"?>\n"                                    \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <statuslineupdatereq>\n"                                     \
	"    <action>notify_display</action>\n"                         \
	"    <dialogid>\n"                                              \
	"      <callid>%s</callid>\n"                                   \
	"      <localtag>%s</localtag>\n"                               \
	"      <remotetag>%s</remotetag>\n"                             \
	"    </dialogid>\n"                                             \
	"    <statustext>\200T</statustext>\n"                          \
	"    <displaytimeout>7</displaytimeout>\n"                      \
	"    <linenumber>0</linenumber>\n"                              \
	"    <priority>1</priority>\n"                                  \
	"  </statuslineupdatereq>\n"                                    \
	"</x-cisco-remotecc-request>\n"

/*
 * MCID confirmation-tone body. tonetype DtZipZip is Cisco's standard
 * "function activated" double-zip on the receiver — same value the
 * chan_sip patch emits for MCID. direction=all plays it both ways.
 */
#define MCID_TONE_PART_FMT                                               \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                 \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <playtonereq>\n"                                             \
	"    <dialogid>\n"                                              \
	"      <callid>%s</callid>\n"                                   \
	"      <localtag>%s</localtag>\n"                               \
	"      <remotetag>%s</remotetag>\n"                             \
	"    </dialogid>\n"                                             \
	"    <tonetype>DtZipZip</tonetype>\n"                           \
	"    <direction>all</direction>\n"                              \
	"  </playtonereq>\n"                                            \
	"</x-cisco-remotecc-request>\n"

/*
 * Park status-line toast. Sent to the phone that pressed Park once the
 * call has landed in a slot. The <statustext> is a Cisco-private
 * inline-format string: "\200! N" renders as a parked-call glyph plus
 * the slot number; "\200^" is the generic "operation cleared" glyph
 * used when parking failed/ended without a slot. The \200 (0x80) byte
 * is not UTF-8 — same as the MCID status part, so this body omits
 * encoding="UTF-8" on the XML declaration (the chan_sip patch uses
 * encoding="iso-8859-1" here; either way the byte is what matters and
 * the firmware parses by element). <dialogid> echoes back exactly what
 * the phone sent in the Park REFER so it lands on the right call
 * appearance. notify_display / displaytimeout / linenumber / priority
 * are wire-fidelity to the patch's statuslineupdatereq emitter — not
 * tunables. Mirrors channels/sip/chan_sip.c remotecc_park_notify().
 */
#define PARK_TOAST_FMT                                                   \
	"<?xml version=\"1.0\"?>\n"                                     \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <statuslineupdatereq>\n"                                     \
	"    <action>notify_display</action>\n"                         \
	"    <dialogid>\n"                                              \
	"      <callid>%s</callid>\n"                                   \
	"      <localtag>%s</localtag>\n"                               \
	"      <remotetag>%s</remotetag>\n"                             \
	"    </dialogid>\n"                                             \
	"    <statustext>%s</statustext>\n"                             \
	"    <displaytimeout>10</displaytimeout>\n"                     \
	"    <linenumber>0</linenumber>\n"                              \
	"    <priority>1</priority>\n"                                  \
	"  </statuslineupdatereq>\n"                                    \
	"</x-cisco-remotecc-request>\n"

/* statustext payloads for PARK_TOAST_FMT. "%d" gets the slot number. */
#define PARK_TOAST_PARKED  "\200! %d"
#define PARK_TOAST_CLEARED "\200^"

/*
 * Park-orbit BLF body (ParkMonitor). An Event: refer NOTIFY carrying
 * this application/dialog-info+xml tells the parking phone the state of
 * the slot it parked a call into, so a "Park slot N" line button
 * lights/clears. <call:park><event> is parked|retrieved|forwarded|
 * abandoned|error; <state> is confirmed while occupied,
 * terminated once the call leaves the lot; version increments per
 * NOTIFY. The "parmams" namespace typo is Cisco's, in the firmware —
 * keep it verbatim. Args: version, slot, domain, slot, state, event,
 * slot, domain, slot, domain. Mirrors channels/sip/chan_sip.c
 * remotecc_park_notify()'s monitor branch.
 */
#define PARK_ORBIT_FMT                                                    \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                   \
	"<dialog-info xmlns=\"urn:ietf:parmams:xml:ns:dialog-info\""     \
	" xmlns:call=\"urn:x-cisco:parmams:xml:ns:dialog-info:dialog:callinfo-dialog\"" \
	" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""       \
	" version=\"%u\" state=\"full\" entity=\"%d@%s\">\n"            \
	"  <dialog id=\"%d\">\n"                                         \
	"    <state>%s</state>\n"                                        \
	"    <call:park><event>%s</event></call:park>\n"                \
	"    <local><identity display=\"\">sip:%d@%s</identity></local>\n"   \
	"    <remote><identity display=\"\">sip:%d@%s</identity></remote>\n" \
	"  </dialog>\n"                                                  \
	"</dialog-info>\n"

static struct ast_taskprocessor *remotecc_serializer;
/* The Park blind-transfer runs here, not on remotecc_serializer, so a
 * stuck bridge op can't stall queued HLog/MCID/park-toast/orbit sends. */
static struct ast_taskprocessor *park_serializer;

struct hlog_task_data {
	struct ast_sip_endpoint *endpoint;
	int huntgroup_in;
};

struct remotecc_dialog_id {
	char call_id[256];
	char local_tag[128];  /* Cisco phone's local tag. */
	char remote_tag[128]; /* Cisco phone's remote tag, Asterisk local tag. */
};

struct mcid_feedback_task_data {
	struct ast_sip_endpoint *endpoint;
	struct remotecc_dialog_id dialog_id;
};

static void hlog_task_data_destroy(void *obj)
{
	struct hlog_task_data *data = obj;
	ao2_cleanup(data->endpoint);
}

static void mcid_feedback_task_data_destroy(void *obj)
{
	struct mcid_feedback_task_data *data = obj;
	ao2_cleanup(data->endpoint);
}

/*
 * Local belt-and-suspenders Content-Type check used by the alarm
 * dispatch path (which feeds in non-remotecc types). The
 * x-cisco-remotecc-request+xml lookup is in cisco_endpoint.h's
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

static pjsip_msg_body *make_mcid_feedback_body(pj_pool_t *pool,
	const struct remotecc_dialog_id *dialog_id)
{
	pj_str_t boundary = pj_str("uniqueBoundary");
	pjsip_msg_body *multipart;
	/* Worst-case XML escape is 5x: '&' -> "&amp;". Source field sizes
	 * are 256 (call_id) and 128 (each tag), so we need 1280 / 640 to
	 * guarantee the escape never truncates. Truncation here would
	 * silently drop MCID feedback for legitimate Cisco dialog ids that
	 * happen to contain ampersands. */
	char call_id[1280];
	char local_tag[640];
	char remote_tag[640];
	char xml[4096];

	if (ast_xml_escape(dialog_id->call_id, call_id, sizeof(call_id))
		|| ast_xml_escape(dialog_id->local_tag, local_tag, sizeof(local_tag))
		|| ast_xml_escape(dialog_id->remote_tag, remote_tag, sizeof(remote_tag))) {
		ast_log(LOG_WARNING,
			"cisco-remotecc: MCID dialog id too long after XML escaping\n");
		return NULL;
	}

	multipart = pjsip_multipart_create(pool, NULL, &boundary);
	if (!multipart) {
		return NULL;
	}

	snprintf(xml, sizeof(xml), MCID_STATUS_PART_FMT,
		call_id, local_tag, remote_tag);
	cisco_remotecc_multipart_add_part(pool, multipart, xml);

	snprintf(xml, sizeof(xml), MCID_TONE_PART_FMT,
		call_id, local_tag, remote_tag);
	cisco_remotecc_multipart_add_part(pool, multipart, xml);

	return multipart;
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

static pjsip_msg_body *mcid_build_adapter(pj_pool_t *pool, void *vctx)
{
	struct mcid_feedback_task_data *data = vctx;
	return make_mcid_feedback_body(pool, &data->dialog_id);
}

static int mcid_feedback_send_task(void *obj)
{
	struct mcid_feedback_task_data *data = obj;

	cisco_endpoint_send_refer_to_all_contacts(data->endpoint,
		"cisco-remotecc", "cisco-mcid", "MCID feedback",
		mcid_build_adapter, data, NULL, NULL);

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

static void queue_mcid_feedback(struct ast_sip_endpoint *endpoint,
	const struct remotecc_dialog_id *dialog_id)
{
	struct mcid_feedback_task_data *data;

	data = ao2_alloc(sizeof(*data), mcid_feedback_task_data_destroy);
	if (!data) {
		return;
	}

	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	data->dialog_id = *dialog_id;

	if (ast_sip_push_task(remotecc_serializer, mcid_feedback_send_task, data)) {
		ast_log(LOG_WARNING,
			"cisco-remotecc: failed to queue MCID feedback task\n");
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

static int handle_mcid(struct ast_sip_endpoint *endpoint,
	const char *endpoint_id, const struct remotecc_dialog_id *dialog_id)
{
	struct ast_sip_session *target;
	struct ast_channel *channel;
	struct ast_channel *bridge_channel;

	if (!dialog_id || ast_strlen_zero(dialog_id->call_id)
		|| ast_strlen_zero(dialog_id->local_tag)
		|| ast_strlen_zero(dialog_id->remote_tag)) {
		ast_log(LOG_WARNING,
			"cisco-remotecc: %s sent MCID without a complete dialogid\n",
			endpoint_id);
		return 400;
	}

	target = cisco_dialog_session_lookup(dialog_id->call_id,
		dialog_id->local_tag, dialog_id->remote_tag);
	if (!target) {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s sent MCID for unknown dialog "
			"(callid=%s localtag=%s remotetag=%s)\n",
			endpoint_id, dialog_id->call_id, dialog_id->local_tag,
			dialog_id->remote_tag);
		/* 481 (Call/Transaction Does Not Exist) reads as a race —
		 * the call ended slightly before the softkey reached us —
		 * rather than 603 (Decline) which the firmware presents as
		 * "feature unsupported". */
		return 481;
	}

	channel = cisco_session_channel_ref(target);
	if (!channel) {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s sent MCID for dialog with no channel\n",
			endpoint_id);
		ao2_cleanup(target);
		/* Same reasoning as above: session exists but its channel is
		 * gone, so the dialog effectively no longer exists. */
		return 481;
	}

	if ((bridge_channel = ast_channel_bridge_peer(channel))) {
		ast_verb(3, "%s has a malicious call from '%s'\n",
			endpoint_id, ast_channel_name(bridge_channel));

		if (ast_queue_control(channel, AST_CONTROL_MCID)) {
			ast_log(LOG_WARNING,
				"cisco-remotecc: failed to queue MCID on %s\n",
				ast_channel_name(channel));
		} else {
			ast_log(LOG_NOTICE,
				"cisco-remotecc: %s queued MCID on %s\n",
				endpoint_id, ast_channel_name(channel));
		}

		ast_channel_unref(bridge_channel);
	} else {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s sent MCID for unbridged channel %s\n",
			endpoint_id, ast_channel_name(channel));
	}

	queue_mcid_feedback(endpoint, dialog_id);

	ast_channel_unref(channel);
	ao2_cleanup(target);
	return 202;
}

/* ---------------------------------------------------------------------
 * Park / ParkMonitor softkeys. See the module top comment for the flow.
 *
 * The phone REFERs <softkeyevent>Park</softkeyevent> (or ParkMonitor) +
 * a <dialogid>. handle_park resolves that to the phone's channel and its
 * bridge peer (synchronously, returning the REFER response), subscribes
 * to ast_parking_topic() for the peer, and queues a task that blind-
 * transfers the peer to the parkext (700) in the parker's transfer
 * context. That transfer — running on its own serializer
 * (pjsip/cisco-park), off the SIP rx thread — is what actually parks
 * the call: res_parking allocates the slot, arms
 * comeback-to-origin (from the BLINDTRANSFER var), and drops the phone's
 * leg as part of the transfer. The parking-topic subscription then, per
 * parked-call event:
 *   - Park        -> one <statuslineupdatereq> REFER toast (slot text),
 *                    sub dropped after the first event.
 *   - ParkMonitor -> an Event: refer / dialog-info+xml NOTIFY per event
 *                    (orbit-BLF for a "Park slot N" line button); sub
 *                    kept alive while occupied, dropped on a terminal
 *                    event (retrieved/forwarded/abandoned/error).
 *
 * (Previously this drove res_parking directly via
 * ast_bridge_channel_write_park() inline on the rx thread — line-mapped
 * from chan_sip's park_thread — which left the parker leg stuck in a
 * 1-party bridge and could wedge the phone / the rx thread. The blind-
 * transfer-to-700 path is the same one a plain "transfer to 700" takes,
 * so res_parking owns the parker-leg cleanup and we don't.)
 * ------------------------------------------------------------------- */

struct park_request_data {
	struct ast_sip_endpoint *endpoint;       /* ref'd */
	struct remotecc_dialog_id dialog_id;     /* echoed back in the toast */
	char parkee_uniqueid[AST_MAX_UNIQUEID];  /* filters our parked-call events */
	int monitor;                             /* ParkMonitor: send orbit NOTIFYs */
	unsigned int version;                    /* dialog-info NOTIFY version counter */
	struct stasis_subscription *sub;
};

static void park_request_data_destroy(void *obj)
{
	struct park_request_data *prd = obj;

	ao2_cleanup(prd->endpoint);
}

/* Serializer task that builds + sends the toast REFER to the phone's
 * contacts. PJSIP request-sending is kept on remotecc_serializer, same
 * as the HLog / MCID feedback paths. */
struct park_toast_task_data {
	struct ast_sip_endpoint *endpoint;       /* ref'd */
	struct remotecc_dialog_id dialog_id;
	char statustext[24];                     /* pre-formatted, e.g. "\200! 701" */
};

static void park_toast_task_data_destroy(void *obj)
{
	struct park_toast_task_data *data = obj;

	ao2_cleanup(data->endpoint);
}

static pjsip_msg_body *park_toast_build_adapter(pj_pool_t *pool, void *vobj)
{
	struct park_toast_task_data *data = vobj;
	pj_str_t type = pj_str("application");
	pj_str_t subtype = pj_str("x-cisco-remotecc-request+xml");
	pj_str_t text;
	/* Worst-case 5x XML escape of call_id(256)/tags(128) — same sizing
	 * rationale as make_mcid_feedback_body. statustext is server-built. */
	char call_id[1280];
	char local_tag[640];
	char remote_tag[640];
	char xml[4096];

	if (ast_xml_escape(data->dialog_id.call_id, call_id, sizeof(call_id))
		|| ast_xml_escape(data->dialog_id.local_tag, local_tag, sizeof(local_tag))
		|| ast_xml_escape(data->dialog_id.remote_tag, remote_tag, sizeof(remote_tag))) {
		ast_log(LOG_WARNING,
			"cisco-remotecc: park toast dialogid too long after XML escaping\n");
		return NULL;
	}

	snprintf(xml, sizeof(xml), PARK_TOAST_FMT,
		call_id, local_tag, remote_tag, data->statustext);
	pj_strdup2(pool, &text, xml);
	return pjsip_msg_body_create(pool, &type, &subtype, &text);
}

static int park_toast_send_task(void *obj)
{
	struct park_toast_task_data *data = obj;

	cisco_endpoint_send_refer_to_all_contacts(data->endpoint,
		"cisco-remotecc", "cisco-park", "Park notify",
		park_toast_build_adapter, data, NULL, NULL);

	ao2_cleanup(data);
	return 0;
}

static void queue_park_toast(struct ast_sip_endpoint *endpoint,
	const struct remotecc_dialog_id *dialog_id, const char *statustext)
{
	struct park_toast_task_data *data;

	data = ao2_alloc(sizeof(*data), park_toast_task_data_destroy);
	if (!data) {
		return;
	}
	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	data->dialog_id = *dialog_id;
	ast_copy_string(data->statustext, statustext, sizeof(data->statustext));

	if (ast_sip_push_task(remotecc_serializer, park_toast_send_task, data)) {
		ast_log(LOG_WARNING,
			"cisco-remotecc: failed to queue park toast task\n");
		ao2_cleanup(data);
	}
}

/* ParkMonitor orbit-BLF NOTIFY: builds + sends one Event: refer /
 * dialog-info+xml NOTIFY to every contact of the parking endpoint.
 * Runs on remotecc_serializer like the other server->phone sends. */
struct park_orbit_task_data {
	struct ast_sip_endpoint *endpoint;   /* ref'd */
	int parkingspace;
	unsigned int version;
	unsigned int expires;                /* for active;expires= (non-terminal) */
	int terminated;                      /* 1 -> Subscription-State: terminated */
	char event[16];                      /* "parked" / "retrieved" / ... */
	char state[16];                      /* "confirmed" / "terminated" */
};

static void park_orbit_task_data_destroy(void *obj)
{
	struct park_orbit_task_data *data = obj;

	ao2_cleanup(data->endpoint);
}

static int park_orbit_send_task(void *obj)
{
	struct park_orbit_task_data *data = obj;
	struct ao2_container *contacts;
	struct ao2_iterator iter;
	struct ast_sip_contact *contact;
	char domainbuf[PJSIP_MAX_URL_SIZE];
	const char *domain;
	char xml[4096];
	char substate[64];

	domain = cisco_endpoint_local_domain(data->endpoint, NULL, domainbuf,
		sizeof(domainbuf));

	snprintf(xml, sizeof(xml), PARK_ORBIT_FMT,
		data->version,
		data->parkingspace, domain,        /* entity */
		data->parkingspace,                /* <dialog id> */
		data->state,
		data->event,
		data->parkingspace, domain,        /* <local><identity> */
		data->parkingspace, domain);       /* <remote><identity> */

	if (data->terminated) {
		ast_copy_string(substate, "terminated;reason=noresource", sizeof(substate));
	} else {
		snprintf(substate, sizeof(substate), "active;expires=%u", data->expires);
	}

	if (ast_strlen_zero(data->endpoint->aors)
		|| !(contacts = ast_sip_location_retrieve_contacts_from_aor_list(
			data->endpoint->aors))) {
		ao2_cleanup(data);
		return 0;
	}

	iter = ao2_iterator_init(contacts, 0);
	while ((contact = ao2_iterator_next(&iter))) {
		pjsip_tx_data *tdata = NULL;
		pj_str_t btype = pj_str("application");
		pj_str_t bsubtype = pj_str("dialog-info+xml");
		pj_str_t btext;

		if (ast_sip_create_request("NOTIFY", NULL, data->endpoint, NULL,
				contact, &tdata)) {
			ao2_cleanup(contact);
			continue;
		}
		ast_sip_add_header(tdata, "Event", "refer");
		ast_sip_add_header(tdata, "Subscription-State", substate);
		pj_strdup2(tdata->pool, &btext, xml);
		tdata->msg->body = pjsip_msg_body_create(tdata->pool, &btype,
			&bsubtype, &btext);
		if (!tdata->msg->body) {
			pjsip_tx_data_dec_ref(tdata);
			ao2_cleanup(contact);
			continue;
		}
		if (ast_sip_send_request(tdata, NULL, data->endpoint, NULL, NULL)) {
			ast_log(LOG_WARNING,
				"cisco-remotecc: park orbit NOTIFY send failed for %s\n",
				contact->uri);
		} else {
			ast_log(LOG_NOTICE,
				"cisco-remotecc: park orbit NOTIFY (slot %d, %s) sent to %s\n",
				data->parkingspace, data->event, contact->uri);
		}
		ao2_cleanup(contact);
	}
	ao2_iterator_destroy(&iter);
	ao2_cleanup(contacts);
	ao2_cleanup(data);
	return 0;
}

static void queue_park_orbit_notify(struct ast_sip_endpoint *endpoint,
	int parkingspace, unsigned int version, unsigned int expires,
	int terminated, const char *event, const char *state)
{
	struct park_orbit_task_data *data;

	data = ao2_alloc(sizeof(*data), park_orbit_task_data_destroy);
	if (!data) {
		return;
	}
	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	data->parkingspace = parkingspace;
	data->version = version;
	data->expires = expires;
	data->terminated = terminated;
	ast_copy_string(data->event, event, sizeof(data->event));
	ast_copy_string(data->state, state, sizeof(data->state));

	if (ast_sip_push_task(remotecc_serializer, park_orbit_send_task, data)) {
		ast_log(LOG_WARNING,
			"cisco-remotecc: failed to queue park orbit NOTIFY task\n");
		ao2_cleanup(data);
	}
}

/* The event-type subset we surface to the phone. Stock Asterisk releases
 * stop at PARKED_CALL_SWAP; the cisco-usecallmanager patch adds
 * PARKED_CALL_REMINDER for a periodic re-confirm of an occupied slot.
 * Anything not listed here falls to the default and is ignored. The name
 * is the <call:park><event> value for the orbit-BLF body. */
static const char *park_event_name(enum ast_parked_call_event_type t)
{
	switch (t) {
	case PARKED_CALL:          return "parked";
	case PARKED_CALL_UNPARKED: return "retrieved";
	case PARKED_CALL_TIMEOUT:  return "forwarded";
	case PARKED_CALL_GIVEUP:   return "abandoned";
	case PARKED_CALL_FAILED:   return "error";
	default:                   return NULL;
	}
}

static void park_stasis_cb(void *data, struct stasis_subscription *sub,
	struct stasis_message *message)
{
	struct park_request_data *prd = data;
	struct ast_parked_call_payload *payload;
	const char *event;
	int terminal;

	if (stasis_subscription_final_message(sub, message)) {
		ao2_cleanup(prd);
		return;
	}
	if (stasis_message_type(message) != ast_parked_call_type()) {
		return;
	}
	payload = stasis_message_data(message);
	if (!payload || !payload->parkee || !payload->parkee->base
		|| strcmp(prd->parkee_uniqueid, payload->parkee->base->uniqueid)) {
		return;   /* not the call we parked */
	}
	event = park_event_name(payload->event_type);
	if (!event) {
		return;   /* an event type we don't surface (e.g. PARKED_CALL_SWAP) */
	}
	/* PARKED_CALL = still in the lot; everything park_event_name() names
	 * besides that (retrieved, forwarded, abandoned, error) means the
	 * call has left. */
	terminal = payload->event_type != PARKED_CALL;

	if (prd->monitor) {
		queue_park_orbit_notify(prd->endpoint, payload->parkingspace,
			++prd->version, terminal ? 0 : payload->timeout,
			terminal, event, terminal ? "terminated" : "confirmed");
	} else {
		char statustext[24];

		if (payload->event_type == PARKED_CALL) {
			snprintf(statustext, sizeof(statustext), PARK_TOAST_PARKED,
				payload->parkingspace);
		} else {
			ast_copy_string(statustext, PARK_TOAST_CLEARED, sizeof(statustext));
		}
		queue_park_toast(prd->endpoint, &prd->dialog_id, statustext);
	}

	/* No parker-leg teardown here — handle_park parks the call by blind-
	 * transferring the peer to 700, and that transfer already dropped the
	 * phone's leg as part of completing the transfer. */

	/* The plain-Park toast is one-shot: act on the first relevant event
	 * and drop the subscription. ParkMonitor keeps the subscription
	 * alive while the slot is occupied (PARKED_CALL) and drops it on the
	 * terminal event, so the slot button tracks the whole lifecycle. */
	if (!prd->monitor || terminal) {
		prd->sub = stasis_unsubscribe(prd->sub);
	}
}

/* Default parkext, used when an endpoint's cisco section doesn't set the
 * `parkext` option (it's registered with this default, so in practice
 * d->parkext is always populated; this is a belt-and-braces fallback if
 * the cisco object can't be retrieved). The blind transfer goes to
 * <parkext>@<the parker's transfer context> — the same path a plain
 * "transfer to 700" takes — so res_parking allocates the slot, arms
 * comeback-to-origin from BLINDTRANSFER, and tears down the transferer
 * leg. The context is resolved per call (TRANSFER_CONTEXT chan var if
 * set, else the endpoint's configured context — same as stock
 * res_pjsip_refer). */
#define PARK_BLIND_XFER_EXTEN "700"

/* Off-the-rx-thread task that actually parks the call. Carries a ref to
 * the parker (transferer) channel, the resolved parkext + transfer
 * context, the endpoint (for logging) and the parking-topic
 * subscription's prd (so a failed transfer can give the phone error
 * feedback and drop the now-orphaned subscription). */
struct park_xfer_task_data {
	struct ast_channel *parker;          /* ref'd — the transferer */
	struct ast_sip_endpoint *endpoint;   /* ref'd — for the log line */
	struct park_request_data *prd;       /* ref'd */
	char parkext[AST_MAX_EXTENSION];     /* cisco parkext= (default "700") */
	char context[AST_MAX_CONTEXT];       /* TRANSFER_CONTEXT or endpoint context */
};

static void park_xfer_task_data_destroy(void *obj)
{
	struct park_xfer_task_data *d = obj;

	ast_channel_cleanup(d->parker);
	ao2_cleanup(d->endpoint);
	ao2_cleanup(d->prd);
}

static int park_xfer_task(void *obj)
{
	struct park_xfer_task_data *d = obj;
	const char *endpoint_id = ast_sorcery_object_get_id(d->endpoint);
	struct ast_channel *peer;
	enum ast_transfer_result res;

	/* The bridge peer was the parkee when handle_park ran; re-check it
	 * here. Between then and now (typically ms, on park_serializer) the
	 * call could have changed peers — an attended transfer, a pickup, a
	 * parked-call retrieval bridging a different channel in — and a bare
	 * ast_bridge_transfer_blind() would park whoever the peer is *now*
	 * while we sit waiting for events about the original parkee
	 * (orphaning the subscription, no toast, wrong call parked). */
	peer = ast_channel_bridge_peer(d->parker);
	if (peer && !strcmp(d->prd->parkee_uniqueid, ast_channel_uniqueid(peer))) {
		res = ast_bridge_transfer_blind(1 /* external (SIP-initiated) */,
			d->parker, d->parkext, d->context, NULL, NULL);
	} else {
		res = AST_BRIDGE_TRANSFER_INVALID;   /* peer changed or call gone */
	}
	ast_channel_cleanup(peer);

	if (res == AST_BRIDGE_TRANSFER_SUCCESS) {
		ast_log(LOG_NOTICE, "cisco-remotecc: %s parked %s (-> %s@%s)%s\n",
			endpoint_id, ast_channel_name(d->parker), d->parkext,
			d->context, d->prd->monitor ? " [orbit-monitor]" : "");
	} else {
		ast_log(LOG_WARNING,
			"cisco-remotecc: %s — Park of %s aborted (bridge peer changed/"
			"gone, or transfer to %s@%s failed — result %d)\n", endpoint_id,
			ast_channel_name(d->parker), d->parkext, d->context, (int) res);
		/* Give the phone failure feedback — it already got 202 — then
		 * drop the now-orphaned parking subscription (no PARKED_CALL is
		 * coming). For ParkMonitor send a terminal "error" orbit NOTIFY
		 * so the slot button clears; for plain Park the cleared toast. */
		if (d->prd->monitor) {
			queue_park_orbit_notify(d->prd->endpoint, 0, ++d->prd->version,
				0, 1 /* terminated */, "error", "terminated");
		} else {
			queue_park_toast(d->prd->endpoint, &d->prd->dialog_id,
				PARK_TOAST_CLEARED);
		}
		d->prd->sub = stasis_unsubscribe(d->prd->sub);
	}

	ao2_cleanup(d);
	return 0;
}

/*! \brief Handle the RemoteCC Park / ParkMonitor softkey.
 *  \param monitor non-zero for ParkMonitor (send orbit-BLF NOTIFYs);
 *         zero for plain Park (one slot-announce toast).
 *
 * Validates and resolves the call synchronously (so the REFER response
 * is meaningful), subscribes to the parking topic, then hands the actual
 * park — a blind transfer of the peer to PARK_BLIND_XFER_EXTEN in the
 * resolved transfer context — to a task on park_serializer so it never
 * runs on the SIP rx thread. */
static int handle_park(struct ast_sip_endpoint *endpoint,
	const char *endpoint_id, const struct remotecc_dialog_id *dialog_id,
	int monitor)
{
	struct ast_sip_session *session;
	struct ast_channel *parker;
	struct ast_channel *parkee;
	struct park_request_data *prd;
	struct park_xfer_task_data *txdata;
	char parkext[AST_MAX_EXTENSION];
	char xfer_context[AST_MAX_CONTEXT];

	if (!dialog_id || ast_strlen_zero(dialog_id->call_id)
		|| ast_strlen_zero(dialog_id->local_tag)
		|| ast_strlen_zero(dialog_id->remote_tag)) {
		ast_log(LOG_WARNING,
			"cisco-remotecc: %s sent Park without a complete dialogid\n",
			endpoint_id);
		return 400;
	}

	session = cisco_dialog_session_lookup(dialog_id->call_id,
		dialog_id->local_tag, dialog_id->remote_tag);
	if (!session) {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s sent Park for unknown dialog (callid=%s)\n",
			endpoint_id, dialog_id->call_id);
		/* 481 reads as "the call ended before the softkey reached us",
		 * not "feature unsupported" (which is how the firmware shows
		 * 603). Same reasoning as handle_mcid. */
		return 481;
	}
	parker = cisco_session_channel_ref(session);
	ao2_cleanup(session);
	if (!parker) {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s sent Park for dialog with no channel\n",
			endpoint_id);
		return 481;
	}

	if (ast_channel_state(parker) != AST_STATE_UP) {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s sent Park for a non-answered call\n",
			endpoint_id);
		ast_channel_unref(parker);
		return 481;
	}

	parkee = ast_channel_bridge_peer(parker);
	if (!parkee) {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s sent Park for an unbridged call\n",
			endpoint_id);
		ast_channel_unref(parker);
		return 481;
	}

	/* parkext: the cisco object's parkext= (default "700"). Fall back to
	 * PARK_BLIND_XFER_EXTEN only if the cisco object can't be retrieved
	 * (shouldn't happen — remotecc_on_rx_request already gated on it). */
	{
		struct cisco_endpoint *cisco = cisco_endpoint_get(endpoint_id);

		ast_copy_string(parkext,
			(cisco && !ast_strlen_zero(cisco->parkext))
				? cisco->parkext : PARK_BLIND_XFER_EXTEN, sizeof(parkext));
		ao2_cleanup(cisco);
	}
	/* Target context for that parkext, resolved the way stock
	 * res_pjsip_refer does: TRANSFER_CONTEXT chan var (read AND copied
	 * under the channel lock — pbx_builtin_getvar_helper's returned
	 * pointer is only valid while the channel is locked), else the
	 * endpoint's configured context (not the live channel context,
	 * which a Goto could have moved). */
	ast_channel_lock(parker);
	{
		const char *cv = pbx_builtin_getvar_helper(parker, "TRANSFER_CONTEXT");

		ast_copy_string(xfer_context,
			!ast_strlen_zero(cv) ? cv
				: (!ast_strlen_zero(endpoint->context) ? endpoint->context
					: "parkedcalls"),
			sizeof(xfer_context));
	}
	ast_channel_unlock(parker);

	/* Reject up front if the parkext doesn't resolve — like stock REFER
	 * does for a transfer to a non-existent extension — rather than
	 * 202'ing and then failing asynchronously. Accept if it's a
	 * res_parking parkext in this context (ast_bridge_transfer_blind
	 * will take the parking path) OR at least exists in the dialplan
	 * (the common "include => parkedcalls" / "_70[0-9] -> Goto" setups
	 * land the transferee in Park() via a normal blind transfer — see
	 * main/bridge.c). It can't, without running the dialplan, prove a
	 * mere-exists target actually reaches Park(); a parkext misconfigured
	 * to some other extension will transfer the peer there and leave the
	 * parking subscription waiting — that's an operator config error. */
	if (!ast_parking_is_exten_park(xfer_context, parkext)
		&& !ast_exists_extension(NULL, xfer_context, parkext, 1, NULL)) {
		ast_log(LOG_WARNING,
			"cisco-remotecc: %s — Park target %s@%s has no dialplan extension\n",
			endpoint_id, parkext, xfer_context);
		ast_channel_unref(parkee);
		ast_channel_unref(parker);
		return 404;
	}

	/* ast_bridge_transfer_blind() will set BLINDTRANSFER on the parkee
	 * itself (to the parker's channel name); set it (and PARKINGLOT)
	 * here too so res_parking's comeback-to-origin and lot pick are
	 * unambiguous regardless. */
	pbx_builtin_setvar_helper(parkee, "BLINDTRANSFER", ast_channel_name(parker));
	pbx_builtin_setvar_helper(parkee, "PARKINGLOT", ast_channel_parkinglot(parker));

	prd = ao2_alloc(sizeof(*prd), park_request_data_destroy);
	if (!prd) {
		ast_channel_unref(parkee);
		ast_channel_unref(parker);
		return 500;
	}
	ao2_ref(endpoint, +1);
	prd->endpoint = endpoint;
	prd->dialog_id = *dialog_id;
	prd->monitor = monitor;
	ast_copy_string(prd->parkee_uniqueid, ast_channel_uniqueid(parkee),
		sizeof(prd->parkee_uniqueid));

	/* Subscribe before queuing the transfer so PARKED_CALL can't be
	 * missed. The subscription "owns" the prd ref from ao2_alloc;
	 * park_stasis_cb releases it on the final message after
	 * stasis_unsubscribe(). Selective filter: only parked-call events
	 * and the subscription-change (final) message. */
	prd->sub = stasis_subscribe(ast_parking_topic(), park_stasis_cb, prd);
	if (!prd->sub) {
		ast_channel_unref(parkee);
		ast_channel_unref(parker);
		ao2_cleanup(prd);   /* drops the endpoint ref + frees prd */
		return 500;
	}
	stasis_subscription_accept_message_type(prd->sub, ast_parked_call_type());
	stasis_subscription_accept_message_type(prd->sub,
		stasis_subscription_change_type());
	stasis_subscription_set_filter(prd->sub, STASIS_SUBSCRIPTION_FILTER_SELECTIVE);
	ast_channel_unref(parkee);   /* not needed past here */

	txdata = ao2_alloc(sizeof(*txdata), park_xfer_task_data_destroy);
	if (!txdata) {
		ast_channel_unref(parker);
		prd->sub = stasis_unsubscribe(prd->sub);   /* final message frees prd */
		return 500;
	}
	txdata->parker = parker;   /* hand the ref to txdata */
	ao2_ref(endpoint, +1);
	txdata->endpoint = endpoint;
	ao2_ref(prd, +1);
	txdata->prd = prd;
	ast_copy_string(txdata->parkext, parkext, sizeof(txdata->parkext));
	ast_copy_string(txdata->context, xfer_context, sizeof(txdata->context));

	if (ast_sip_push_task(park_serializer, park_xfer_task, txdata)) {
		ast_log(LOG_WARNING,
			"cisco-remotecc: %s — failed to queue park task\n", endpoint_id);
		ao2_cleanup(txdata);                       /* drops parker/endpoint/prd refs */
		prd->sub = stasis_unsubscribe(prd->sub);   /* final message frees prd */
		return 500;
	}

	return 202;
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
		/* TODO: Cancel is currently a stub. The phone expects the
		 * server to cancel the in-progress operation identified by
		 * <dialogid> (typically a half-completed transfer). To do
		 * that properly we need to look up the active session via
		 * cisco_dialog_session_lookup(dialog_id->...) and drive a
		 * cancel through ast_sip_session — neither of which is
		 * wired up yet. We accept (202) so the firmware doesn't
		 * surface an error to the user, but the in-flight operation
		 * is not actually cancelled server-side. */
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s sent Cancel softkey (accepted as no-op; "
			"server-side cancel not yet implemented)\n",
			endpoint_id);
		return 202;
	}

	if (!strcmp(softkey, "MCID")) {
		return handle_mcid(endpoint, endpoint_id, dialog_id);
	}

	if (!strcmp(softkey, "Park") || !strcmp(softkey, "ParkMonitor")) {
		return handle_park(endpoint, endpoint_id, dialog_id,
			!strcmp(softkey, "ParkMonitor"));
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

	if (request_content_type_is(rdata, "application", "x-cisco-alarm+xml")
		|| request_content_type_is(rdata, "application",
			"x-cisco-remotecc-response+xml")) {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: accepted device notification from %s\n",
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
	/* res_parking: the Park softkey blind-transfers to its parkext and
	 * subscribes to ast_parking_topic()/ast_parked_call_type() (symbols
	 * exported by res_parking.so) — so it's a hard dep now. Listing it
	 * makes a missing/noload'd res_parking decline cleanly instead of
	 * failing on an unresolved symbol. */
	.requires = "res_pjsip,res_pjsip_session,res_pjsip_cisco_endpoint,res_parking",
);
