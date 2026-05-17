/*
 * Cisco conference state containers + channel-state helpers shared by
 * the ConfList action / Confrn / Join paths. Compiled into
 * res_pjsip_cisco_conference.so; no symbols exported across module
 * boundaries.
 */

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_ua.h>
#include <pjlib.h>

#include "asterisk/astobj2.h"
#include "asterisk/bridge.h"
#include "asterisk/bridge_channel.h"
#include "asterisk/bridge_features.h"
#include "asterisk/channel.h"
#include "asterisk/callerid.h"
#include "asterisk/pbx.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"

#include "conference_private.h"

int conflist_pending_hash(const void *obj, int flags)
{
	const struct conflist_pending *p;
	const char *id;

	if (flags & OBJ_KEY) {
		id = obj;
	} else {
		p = obj;
		id = p->endpoint_id;
	}
	return ast_str_case_hash(id);
}

int conflist_pending_cmp(void *obj, void *arg, int flags)
{
	const struct conflist_pending *l = obj;
	const struct conflist_pending *r;
	const char *id;

	if (flags & OBJ_KEY) {
		id = arg;
	} else {
		r = arg;
		id = r->endpoint_id;
	}
	return strcasecmp(l->endpoint_id, id) ? 0 : CMP_MATCH | CMP_STOP;
}

/*!
 * \brief Find-or-create the pending entry for \a endpoint_id. Returns the
 *        entry with an extra ao2 ref the caller must release.
 *
 * The entry is left in the container so subsequent lookups see the
 * latest state. Caller fills in fields under the entry's ao2 lock if
 * concurrent access is possible.
 */
static struct conflist_pending *conflist_pending_get_or_create(
	const char *endpoint_id)
{
	struct conflist_pending *p;

	if (!conflist_pending_actions || ast_strlen_zero(endpoint_id)) {
		return NULL;
	}
	p = ao2_find(conflist_pending_actions, endpoint_id, OBJ_KEY);
	if (p) {
		return p;
	}
	p = ao2_alloc(sizeof(*p), NULL);
	if (!p) {
		return NULL;
	}
	ast_copy_string(p->endpoint_id, endpoint_id, sizeof(p->endpoint_id));
	ao2_link(conflist_pending_actions, p);
	return p;
}

void conflist_pending_set_dialog(const char *endpoint_id,
	const struct conference_dialog_id *dialog_id)
{
	struct conflist_pending *p = conflist_pending_get_or_create(endpoint_id);
	if (!p) {
		return;
	}
	ao2_lock(p);
	p->dialog_id = *dialog_id;
	p->dialog_id_valid = 1;
	ao2_unlock(p);
	ao2_ref(p, -1);
}

void conflist_pending_set_action(const char *endpoint_id, const char *action)
{
	struct conflist_pending *p = conflist_pending_get_or_create(endpoint_id);
	if (!p) {
		return;
	}
	ao2_lock(p);
	ast_copy_string(p->action, S_OR(action, ""), sizeof(p->action));
	ao2_unlock(p);
	ao2_ref(p, -1);
}

/*!
 * \brief Look up the stored ConfList state for an endpoint.
 *
 * \param[out] out_dialog_id  Receives the saved dialog_id when present.
 *                            May be NULL if caller only wants the action.
 * \param[out] out_action     Receives the pending action (or empty), AND
 *                            consumes it (clears the entry's action) so
 *                            the next pick falls through to default-Mute.
 *                            Pass NULL + 0 to peek without consuming.
 * \retval 1 if a valid dialog_id was found.
 * \retval 0 otherwise (no prior ConfList press from this endpoint).
 */
int conflist_pending_lookup(const char *endpoint_id,
	struct conference_dialog_id *out_dialog_id,
	char *out_action, size_t out_action_len)
{
	struct conflist_pending *p;
	int found_dialog;

	if (out_action && out_action_len) {
		out_action[0] = '\0';
	}
	if (!conflist_pending_actions || ast_strlen_zero(endpoint_id)) {
		return 0;
	}
	p = ao2_find(conflist_pending_actions, endpoint_id, OBJ_KEY);
	if (!p) {
		return 0;
	}
	ao2_lock(p);
	found_dialog = p->dialog_id_valid;
	if (found_dialog && out_dialog_id) {
		*out_dialog_id = p->dialog_id;
	}
	if (out_action && out_action_len) {
		ast_copy_string(out_action, p->action, out_action_len);
		p->action[0] = '\0';
	}
	ao2_unlock(p);
	ao2_ref(p, -1);
	return found_dialog;
}

int cisco_selected_hash(const void *obj, int flags)
{
	const struct cisco_selected *s = obj;
	/* Hash on endpoint_id ^ call_id — neither is enough alone but the
	 * combination distributes well across our small bucket count. */
	return ast_str_case_hash(s->endpoint_id)
		^ ast_str_hash(s->dialog_id.call_id);
}

int cisco_selected_cmp(void *obj, void *arg, int flags)
{
	const struct cisco_selected *l = obj;
	const struct cisco_selected *r = arg;

	if (strcasecmp(l->endpoint_id, r->endpoint_id)
		|| strcmp(l->dialog_id.call_id, r->dialog_id.call_id)
		|| strcmp(l->dialog_id.local_tag, r->dialog_id.local_tag)
		|| strcmp(l->dialog_id.remote_tag, r->dialog_id.remote_tag)) {
		return 0;
	}
	return CMP_MATCH | CMP_STOP;
}

void cisco_selected_add(const char *endpoint_id,
	const struct conference_dialog_id *dialog_id)
{
	struct cisco_selected key;
	struct cisco_selected *existing;
	struct cisco_selected *fresh;

	if (!cisco_selected_calls || ast_strlen_zero(endpoint_id)) {
		return;
	}
	ast_copy_string(key.endpoint_id, endpoint_id, sizeof(key.endpoint_id));
	key.dialog_id = *dialog_id;

	existing = ao2_find(cisco_selected_calls, &key, OBJ_SEARCH_OBJECT);
	if (existing) {
		ao2_ref(existing, -1);
		return;
	}

	fresh = ao2_alloc(sizeof(*fresh), NULL);
	if (!fresh) {
		return;
	}
	ast_copy_string(fresh->endpoint_id, endpoint_id,
		sizeof(fresh->endpoint_id));
	fresh->dialog_id = *dialog_id;
	ao2_link(cisco_selected_calls, fresh);
	ao2_cleanup(fresh);
}

void cisco_selected_remove(const char *endpoint_id,
	const struct conference_dialog_id *dialog_id)
{
	struct cisco_selected key;
	struct cisco_selected *existing;

	if (!cisco_selected_calls || ast_strlen_zero(endpoint_id)) {
		return;
	}
	ast_copy_string(key.endpoint_id, endpoint_id, sizeof(key.endpoint_id));
	key.dialog_id = *dialog_id;

	existing = ao2_find(cisco_selected_calls, &key,
		OBJ_SEARCH_OBJECT | OBJ_UNLINK);
	ao2_cleanup(existing);
}

struct selected_iter_filter {
	const char *endpoint_id;
	cisco_selected_visitor visitor;
	void *visitor_arg;
};

static int cisco_selected_iter_cb(void *obj, void *arg, int flags)
{
	struct cisco_selected *s = obj;
	struct selected_iter_filter *f = arg;

	if (strcasecmp(s->endpoint_id, f->endpoint_id)) {
		return 0;
	}
	return f->visitor(&s->dialog_id, f->visitor_arg) ? CMP_STOP : 0;
}

void cisco_selected_iterate_for_endpoint(const char *endpoint_id,
	cisco_selected_visitor visitor, void *visitor_arg)
{
	struct selected_iter_filter f = {
		.endpoint_id = endpoint_id,
		.visitor     = visitor,
		.visitor_arg = visitor_arg,
	};

	if (!cisco_selected_calls || ast_strlen_zero(endpoint_id) || !visitor) {
		return;
	}
	ao2_callback(cisco_selected_calls, OBJ_NODATA, cisco_selected_iter_cb,
		&f);
}

static int cisco_selected_clear_endpoint_cb(void *obj, void *arg, int flags)
{
	const struct cisco_selected *s = obj;
	const char *endpoint_id = arg;
	return strcasecmp(s->endpoint_id, endpoint_id) ? 0 : CMP_MATCH;
}

void cisco_selected_clear_endpoint(const char *endpoint_id)
{
	if (!cisco_selected_calls || ast_strlen_zero(endpoint_id)) {
		return;
	}
	ao2_callback(cisco_selected_calls, OBJ_UNLINK | OBJ_MULTIPLE | OBJ_NODATA,
		cisco_selected_clear_endpoint_cb, (void *) endpoint_id);
}

struct ast_channel *bridge_peer_channel_ref(struct ast_channel *self,
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
			match = peer;
			break;
		}
		++i;
		ao2_cleanup(peer);
	}
	ao2_iterator_destroy(&iter);
	ao2_cleanup(peers);
	return match;
}

void mark_channel_as_conference(struct ast_channel *channel,
	const char *endpoint_id)
{
	struct ast_party_connected_line connected;

	ast_party_connected_line_init(&connected);
	connected.id.name.str = "Conference";
	connected.id.name.valid = 1;
	connected.id.number.str = "";
	connected.id.number.valid = 1;
	connected.id.name.presentation =
		AST_PRES_ALLOWED | AST_PRES_USER_NUMBER_PASSED_SCREEN;
	connected.id.number.presentation =
		AST_PRES_ALLOWED | AST_PRES_USER_NUMBER_PASSED_SCREEN;
	/* Stock Asterisk's enum doesn't include CONFERENCE (it's added only
	 * by the chan_sip cisco-usecallmanager patch we deliberately avoid
	 * requiring). UNKNOWN is the inert value — the actual "this is a
	 * Conference leg" signal we use is the CISCO_CONFERENCE chan_var
	 * below, which call_extras' rewrite_conference_identity_headers
	 * reads. */
	connected.source = AST_CONNECTED_LINE_UPDATE_SOURCE_UNKNOWN;

	/* Plumb the Conference flag via a channel variable too. The
	 * call_extras hook keys off this rather than the connected-line
	 * source enum, so we stay ABI-compatible with stock Asterisk. */
	pbx_builtin_setvar_helper(channel, "CISCO_CONFERENCE", "1");

	ast_channel_update_connected_line(channel, &connected, NULL);
	ast_log(LOG_NOTICE,
		"cisco-conference: %s — marked %s connected-line as Conference\n",
		endpoint_id, ast_channel_name(channel));
}

void indicate_remote_unhold(struct ast_channel *channel,
	const char *endpoint_id, const char *role)
{
	if (ast_indicate(channel, AST_CONTROL_UNHOLD)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — failed to indicate UNHOLD on "
			"%s %s\n", endpoint_id, role, ast_channel_name(channel));
	} else {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s — indicated UNHOLD on %s %s\n",
			endpoint_id, role, ast_channel_name(channel));
	}
}

/*!
 * \brief Flag chan_phone_A's bridge-channel so the bridge framework
 *        dissolves the entire conf when this channel hangs up.
 *
 * Implements the cisco_keep_conference=no behaviour: when the Confrn
 * initiator drops, the remaining legs get BYE'd by the bridge dissolve
 * (the chan_sip patch's default — "the user left, the conference is
 * over"). With cisco_keep_conference=yes we don't call this and the
 * bridge sticks around as long as ≥1 channel remains, per the
 * AST_BRIDGE_FLAG_DISSOLVE_EMPTY on the bridge itself.
 *
 * The flag is per-bridge-channel; pjsip session refreshes do not
 * trigger it, only an actual hangup. Setting it after ast_bridge_move
 * is safe because the bridge framework consults the flag at hangup
 * time, not at impart time.
 */
void set_dissolve_on_initiator_hangup(struct ast_channel *channel,
	const char *endpoint_id)
{
	struct ast_bridge_channel *bridge_chan;

	bridge_chan = ast_channel_get_bridge_channel(channel);
	if (!bridge_chan) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — no bridge_channel for %s; "
			"cisco_keep_conference=no won't fire on initiator "
			"hangup\n", endpoint_id, ast_channel_name(channel));
		return;
	}

	ast_bridge_features_set_flag(bridge_chan->features,
		AST_BRIDGE_CHANNEL_FLAG_DISSOLVE_HANGUP);
	ao2_ref(bridge_chan, -1);

	ast_log(LOG_NOTICE,
		"cisco-conference: %s — initiator hangup will dissolve the "
		"conference (%s flagged DISSOLVE_HANGUP)\n",
		endpoint_id, ast_channel_name(channel));
}
