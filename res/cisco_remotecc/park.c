/*
 * Park / ParkMonitor softkey, split out from
 * res_pjsip_cisco_remotecc.c. The flow:
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
 *
 * res_parking is an optional dep (declared in .optional_modules, not
 * .requires). The parking symbols themselves live in the asterisk
 * binary, not res_parking.so, so this module loads either way; we gate
 * handle_park on ast_module_check("res_parking") so a missing
 * res_parking returns 501 instead of subscribing + queuing a transfer
 * that's going to fail at bridge time.
 */

#include "asterisk.h"

#include <pjsip.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/astobj2.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/stasis.h"
#include "asterisk/stasis_channels.h"
#include "asterisk/parking.h"
#include "asterisk/bridge.h"
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
#include "remotecc_private.h"

/*
 * Park body templates live in remotecc_private.h so the
 * tests/unit/test_xml_bodies.c regression can validate them.
 *
 * PARK_TOAST_FMT statustext is a Cisco-private inline-format string:
 * "\200! N" renders as a parked-call glyph plus the slot number;
 * "\200^" is the "operation cleared" glyph used when parking
 * failed/ended without a slot. The \200 (0x80) byte is not UTF-8 —
 * same as the MCID status part, so this body omits encoding="UTF-8"
 * on the XML declaration (the chan_sip patch uses encoding="iso-8859-1"
 * here; either way the byte is what matters and the firmware parses
 * by element). <dialogid> echoes back exactly what the phone sent in
 * the Park REFER so it lands on the right call appearance.
 * notify_display / displaytimeout / linenumber / priority are
 * wire-fidelity to the patch's statuslineupdatereq emitter — not
 * tunables. Mirrors channels/sip/chan_sip.c remotecc_park_notify().
 *
 * PARK_ORBIT_FMT (ParkMonitor) is an Event: refer NOTIFY carrying
 * application/dialog-info+xml that tells the parking phone the state
 * of the slot it parked a call into, so a "Park slot N" line button
 * lights/clears. <call:park><event> is parked|retrieved|forwarded|
 * abandoned|error; <state> is confirmed while occupied, terminated
 * once the call leaves the lot; version increments per NOTIFY. The
 * "parmams" namespace typo is Cisco's, in the firmware — keep it
 * verbatim. Args: version, slot, domain, slot, state, event, slot,
 * domain, slot, domain. Mirrors channels/sip/chan_sip.c
 * remotecc_park_notify()'s monitor branch.
 */

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

int handle_park(struct ast_sip_endpoint *endpoint, const char *endpoint_id,
	const struct remotecc_dialog_id *dialog_id, int monitor)
{
	struct ast_sip_session *session;
	struct ast_channel *parker;
	struct ast_channel *parkee;
	struct park_request_data *prd;
	struct park_xfer_task_data *txdata;
	char parkext[AST_MAX_EXTENSION];
	char xfer_context[AST_MAX_CONTEXT];

	/* res_parking is optional. Without it the blind transfer to parkext
	 * has no parking bridge feature to land on; refuse cleanly so the
	 * phone shows "feature unsupported" instead of us subscribing +
	 * queuing a transfer that fails asynchronously. */
	if (!ast_module_check("res_parking.so")) {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s sent Park but res_parking is not loaded — "
			"declining\n", endpoint_id);
		return 501;
	}

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
