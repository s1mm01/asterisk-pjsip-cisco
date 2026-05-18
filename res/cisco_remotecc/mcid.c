/*
 * MCID (Malicious Call ID) softkey, split out from
 * res_pjsip_cisco_remotecc.c. The flow:
 *
 *   <softkeyeventmsg><softkeyevent>MCID</softkeyevent><dialogid>…
 *
 *   - Resolve <dialogid> to an ast_sip_session and that session's
 *     ast_channel via cisco_dialog_session_lookup +
 *     cisco_session_channel_ref.
 *   - If the channel is bridged, queue AST_CONTROL_MCID on it (stock
 *     Asterisk publishes the trace event via the channel-driver
 *     handler; for chan_pjsip that's ast_pjsip_send_mcid_message_via
 *     the bridge-frame relay).
 *   - Queue a server->phone feedback REFER (statusline glyph +
 *     confirmation tone) so the press has a visible/audible result.
 *
 * Body shapes are line-mapped to the chan_sip cisco-usecallmanager
 * patch's MCID emitter (channels/sip/sip_remotecc_handler.c
 * sip_remotecc_handle_mcid).
 */

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip/sip_multipart.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/astobj2.h"
#include "asterisk/channel.h"
#include "asterisk/bridge.h"
#include "asterisk/frame.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/xml.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/sorcery.h"

#include "cisco/endpoint.h"
#include "cisco/rdata.h"
#include "cisco/refer.h"
#include "cisco/session.h"
#include "remotecc_private.h"

/*
 * MCID body templates live in remotecc_private.h so the
 * tests/unit/test_xml_bodies.c regression can validate them.
 *
 * MCID_STATUS_PART_FMT carries a Cisco-private inline-format escape
 * byte:
 *   <statustext>\200T</statustext>     translates on the phone into a
 *                                       localised "trace" glyph on the
 *                                       status line. The byte is not
 *                                       UTF-8, which is why this
 *                                       sub-body intentionally omits
 *                                       encoding="UTF-8" on the XML
 *                                       declaration. <displaytimeout>
 *                                       is seconds the glyph stays on
 *                                       screen before the firmware
 *                                       restores the previous text.
 *   <linenumber>0</linenumber>          0 = active line. The firmware
 *                                       resolves the line from the
 *                                       <dialogid> regardless.
 *   <priority>1</priority>              status-line priority slot.
 *
 * Don't tune these without checking the patch — values are paired
 * with firmware behaviour, not configurable knobs.
 *
 * MCID_TONE_PART_FMT: tonetype DtZipZip is Cisco's standard "function
 * activated" double-zip on the receiver — same value the chan_sip
 * patch emits for MCID. direction=all plays it both ways.
 */

struct mcid_feedback_task_data {
	struct ast_sip_endpoint *endpoint;
	struct remotecc_dialog_id dialog_id;
};

static void mcid_feedback_task_data_destroy(void *obj)
{
	struct mcid_feedback_task_data *data = obj;
	ao2_cleanup(data->endpoint);
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

int handle_mcid(struct ast_sip_endpoint *endpoint, const char *endpoint_id,
	const struct remotecc_dialog_id *dialog_id)
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
