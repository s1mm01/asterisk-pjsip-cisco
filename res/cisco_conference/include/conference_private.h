/*
 * Internal header for res_pjsip_cisco_conference.so. Declarations
 * shared across the module's sibling .c files (res_pjsip_cisco_
 * conference.c, res/cisco_conference/state.c, res/cisco_conference/list.c, cisco_conf_
 * confrn.c) — all compiled into one .so. Nothing here is exported
 * across module boundaries; the .exports version script keeps the
 * symbol table local.
 */

#ifndef CISCO_CONFERENCE_H
#define CISCO_CONFERENCE_H

#include <pjsip.h>
#include <pjlib.h>

#include "asterisk/astobj2.h"
#include "asterisk/bridge.h"
#include "asterisk/channel.h"
#include "asterisk/taskprocessor.h"
#include "asterisk/res_pjsip.h"

/* application_id values mirror the chan_sip patch's
 * enum sip_remotecc_application in channels/sip/include/remotecc.h.
 * SIP_REMOTECC_NONE = 0, SIP_REMOTECC_CONF_LIST = 1. */
#define CONFERENCE_APP_ID_CONF_LIST 1

#define CONFERENCE_MAX_BODY 8192
#define CONFERENCE_MENU_PARTICIPANT_NAME 64
#define CONFLIST_PENDING_BUCKETS 31
#define CISCO_SELECTED_BUCKETS 31
#define JOIN_MAX_SELECTED 8

struct conference_dialog_id {
	char call_id[256];
	char local_tag[128];
	char remote_tag[128];
};

/*!
 * \brief Per-endpoint pending ConfList state, set when the phone presses
 *        ConfList and consumed by subsequent action REFERs (which carry
 *        no <dialogid> themselves, only <confid>).
 *
 * Two fields with different lifetimes:
 *   dialog_id  — refreshed on every ConfList press, kept across action
 *                REFERs. Lets the action handler relocate the conference
 *                bridge via the same dialog → session → bridge path the
 *                initial ConfList press uses.
 *   action     — set when the phone presses the Mute/Remove softkey
 *                ("sticky action"), consumed on the next participant
 *                pick. Empty otherwise — chan_sip patch's default in
 *                that case is Mute.
 */
struct conflist_pending {
	char endpoint_id[128];
	struct conference_dialog_id dialog_id;
	int dialog_id_valid;
	char action[16];
};

/*!
 * \brief Per-endpoint per-dialog "I want to merge this call" marker,
 *        set by the Select softkey and consumed by the next Join press.
 */
struct cisco_selected {
	char endpoint_id[128];
	struct conference_dialog_id dialog_id;
};

enum remotecc_softkey_kind {
	REMOTECC_SOFTKEY_NONE = 0,
	REMOTECC_SOFTKEY_CONFLIST,
	REMOTECC_SOFTKEY_CONFERENCE,
	REMOTECC_SOFTKEY_SELECT,
	REMOTECC_SOFTKEY_UNSELECT,
	REMOTECC_SOFTKEY_JOIN,
	REMOTECC_SOFTKEY_RMLASTCONF,
	REMOTECC_SOFTKEY_CONFLIST_ACTION,
};

struct remotecc_softkey_msg {
	enum remotecc_softkey_kind kind;
	struct conference_dialog_id dialog_id;
	struct conference_dialog_id consult_dialog_id;
	unsigned int conf_id;
	char user_call_data[64];
};

typedef int (*cisco_selected_visitor)(
	const struct conference_dialog_id *dialog_id, void *arg);

/* Globals: owned by load_module() in res_pjsip_cisco_conference.c. */
extern struct ast_taskprocessor *conference_serializer;
extern struct ao2_container *conflist_pending_actions;
extern struct ao2_container *cisco_selected_calls;
extern pjsip_module conference_module;

/* res/cisco_conference/state.c — state container ops + cross-cutting channel
 * helpers shared by ConfList action, Confrn, and Join. */
int conflist_pending_hash(const void *obj, int flags);
int conflist_pending_cmp(void *obj, void *arg, int flags);
void conflist_pending_set_dialog(const char *endpoint_id,
	const struct conference_dialog_id *dialog_id);
void conflist_pending_set_action(const char *endpoint_id, const char *action);
int conflist_pending_lookup(const char *endpoint_id,
	struct conference_dialog_id *out_dialog_id,
	char *out_action, size_t out_action_len);

int cisco_selected_hash(const void *obj, int flags);
int cisco_selected_cmp(void *obj, void *arg, int flags);
void cisco_selected_add(const char *endpoint_id,
	const struct conference_dialog_id *dialog_id);
void cisco_selected_remove(const char *endpoint_id,
	const struct conference_dialog_id *dialog_id);
void cisco_selected_iterate_for_endpoint(const char *endpoint_id,
	cisco_selected_visitor visitor, void *visitor_arg);
void cisco_selected_clear_endpoint(const char *endpoint_id);

struct ast_channel *bridge_peer_channel_ref(struct ast_channel *self,
	struct ast_bridge *bridge, int index);
void mark_channel_as_conference(struct ast_channel *channel,
	const char *endpoint_id);
void indicate_remote_unhold(struct ast_channel *channel,
	const char *endpoint_id, const char *role);
void set_dissolve_on_initiator_hangup(struct ast_channel *channel,
	const char *endpoint_id);

/* res/cisco_conference/list.c — ConfList menu + action softkeys. */
void queue_conflist(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *dialog_id);
void queue_conflist_action(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *dialog_id,
	const char *user_call_data);

/* res/cisco_conference/confrn.c — Confrn + Join + RmLastConf. */
pjsip_dialog *conference_open_uas_dialog_and_202(pjsip_rx_data *rdata,
	const char *endpoint_id);
void queue_conference(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *active_dialog,
	const struct conference_dialog_id *consult_dialog,
	pjsip_dialog *dlg, int keep_conference);
void queue_join(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *active_dialog,
	int keep_conference);
void queue_rmlastconf(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *active_dialog);

#endif /* CISCO_CONFERENCE_H */
