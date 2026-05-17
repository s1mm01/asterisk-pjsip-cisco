/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Shared session helpers for res_pjsip_cisco_* modules that work
 * with active SIP dialogs. Bodies for the declarations in
 * cisco/session.h; linked into res_pjsip_cisco_endpoint.so and
 * resolved by the other cisco_* modules at load time.
 */

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_ua.h>

#include "asterisk/astobj2.h"
#include "asterisk/bridge.h"
#include "asterisk/channel.h"
#include "asterisk/datastore.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/strings.h"
#include "asterisk/time.h"
#include "asterisk/utils.h"

#include "cisco/session.h"

struct ast_channel *cisco_session_channel_ref(struct ast_sip_session *session)
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

struct ast_sip_session *cisco_dialog_session_lookup(
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

void cisco_send_refer_response(pjsip_rx_data *rdata, int code,
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

/* ---- conference join-time tracking (RmLastConf support) ---- */

/* Datastore payload: a single timeval representing when the channel
 * was added to its current Cisco conference. Tiny (~16 bytes), one
 * per tracked channel. */
static void cisco_conf_join_time_destroy(void *data)
{
	ast_free(data);
}

static const struct ast_datastore_info cisco_conf_join_time_info = {
	.type = "cisco-conf-join-time",
	.destroy = cisco_conf_join_time_destroy,
};

void cisco_conf_mark_joined(struct ast_channel *chan)
{
	struct ast_datastore *ds;
	struct timeval *payload;

	if (!chan) {
		return;
	}

	ast_channel_lock(chan);
	ds = ast_channel_datastore_find(chan, &cisco_conf_join_time_info, NULL);
	if (ds && ds->data) {
		*(struct timeval *) ds->data = ast_tvnow();
		ast_channel_unlock(chan);
		return;
	}
	ast_channel_unlock(chan);

	/* New attachment. Allocate outside the channel lock — ast_calloc
	 * may sleep, and the datastore add is the only step that needs
	 * the lock. */
	payload = ast_calloc(1, sizeof(*payload));
	if (!payload) {
		return;
	}
	*payload = ast_tvnow();

	ds = ast_datastore_alloc(&cisco_conf_join_time_info, NULL);
	if (!ds) {
		ast_free(payload);
		return;
	}
	ds->data = payload;

	ast_channel_lock(chan);
	if (ast_channel_datastore_add(chan, ds)) {
		ast_channel_unlock(chan);
		/* add() failure: the datastore's destroy callback won't run
		 * because the datastore was never attached, so we have to
		 * free the payload + datastore ourselves. */
		ast_datastore_free(ds);
		return;
	}
	ast_channel_unlock(chan);
}

struct ast_channel *cisco_conf_find_last_joined(struct ast_bridge *bridge,
	struct ast_channel *exclude)
{
	struct ao2_container *peers;
	struct ao2_iterator iter;
	struct ast_channel *chan;
	struct ast_channel *best = NULL;
	struct timeval best_time = { 0, 0 };

	if (!bridge) {
		return NULL;
	}

	peers = ast_bridge_peers(bridge);
	if (!peers) {
		return NULL;
	}

	iter = ao2_iterator_init(peers, 0);
	while ((chan = ao2_iterator_next(&iter))) {
		struct ast_datastore *ds;
		struct timeval *t;

		if (exclude && chan == exclude) {
			ast_channel_unref(chan);
			continue;
		}

		ast_channel_lock(chan);
		ds = ast_channel_datastore_find(chan,
			&cisco_conf_join_time_info, NULL);
		t = ds ? (struct timeval *) ds->data : NULL;
		if (t && ast_tvcmp(*t, best_time) > 0) {
			best_time = *t;
			ast_channel_unlock(chan);
			if (best) {
				ast_channel_unref(best);
			}
			/* Transfer the iterator's ref to `best`; the loop's
			 * uniform unref-on-drop happens via the else below. */
			best = chan;
			continue;
		}
		ast_channel_unlock(chan);
		ast_channel_unref(chan);
	}
	ao2_iterator_destroy(&iter);
	ao2_ref(peers, -1);

	return best;
}
