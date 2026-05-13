/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_service_control
 *
 * CLI commands that send Cisco-flavoured service-control NOTIFYs to a
 * registered Cisco Enterprise SIP firmware phone:
 *
 *   pjsip cisco check-sync <endpoint> [contact]
 *      Event: check-sync, Subscription-State: terminated, no body.
 *      Tells the phone to re-fetch its config from TFTP/HTTP.
 *
 *   pjsip cisco restart <endpoint> [contact]
 *      Event: service-control, Subscription-State: active,
 *      Content-Type: text/plain, body action=restart.
 *      Soft restart (preserves call state where possible).
 *
 *   pjsip cisco reset <endpoint> [contact]
 *      As restart but body action=reset. Hard reboot.
 *
 * The optional [contact] is a substring matched against each
 * registered contact's URI or its @hash — restarts one phone of a
 * shared line (max_contacts>1). Omitted = every registered contact.
 * Tab-completion: arg 1 = type=cisco endpoint ids, arg 2 = that
 * endpoint's contact URIs.
 *
 * These replace chan_sip + cisco-usecallmanager-patch's
 *   sip notify cisco-check-cfg <peer>
 *   sip notify cisco-restart <peer>
 *   sip notify cisco-reset <peer>
 * which were driven from sip_notify.conf templates.
 *
 * Gating: only operates on endpoints that have a [name] type=cisco
 * section (per Phase 1 sorcery in res_pjsip_cisco_endpoint).
 *
 * RegisterCallId: the chan_sip patch's restart/reset bodies include
 *   RegisterCallId={<call-id from last REGISTER>}
 * along with three version-stamp lines. The version stamps are
 * advisory (the patch sends them all-zero; so do we), but the Cisco
 * firmware DOES validate RegisterCallId against the Call-ID of its
 * own REGISTER transaction and 400s a mismatch — so it must be the
 * real per-phone value. We take it straight from contact->call_id,
 * which the registrar records on every REGISTER. Keying it per-contact
 * (not per-endpoint) matters for shared lines: a [name] with
 * max_contacts>1 has several phones, each with a distinct REGISTER
 * Call-ID, and each NOTIFY must carry the Call-ID of the phone it's
 * addressed to. (The chan_sip patch's
 *   content=RegisterCallId={${SIPPEER(${PEERNAME},regcallid)}}
 * sip_notify.conf substitution was per-peer and had this same blind
 * spot.) check-sync needs neither RegisterCallId nor version stamps.
 */

/*** MODULEINFO
	<depend>res_pjsip</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/cli.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"

#include "cisco_endpoint.h"

enum sc_action {
	SC_CHECK_SYNC,
	SC_RESTART,
	SC_RESET,
};

static int sc_send_notify(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact, enum sc_action action)
{
	pjsip_tx_data *tdata = NULL;
	const char *event_hdr;
	const char *sub_state_hdr;
	const char *action_str = NULL;
	struct ast_str *body_str = NULL;

	switch (action) {
	case SC_CHECK_SYNC:
		event_hdr     = "check-sync";
		sub_state_hdr = "terminated";
		break;
	case SC_RESTART:
		event_hdr     = "service-control";
		sub_state_hdr = "active";
		action_str    = "restart";
		break;
	case SC_RESET:
		event_hdr     = "service-control";
		sub_state_hdr = "active";
		action_str    = "reset";
		break;
	default:
		return -1;
	}

	if (ast_sip_create_request("NOTIFY", NULL, endpoint, NULL, contact, &tdata)) {
		ast_log(LOG_WARNING, "cisco-svc-ctrl: create_request failed for %s\n",
			contact->uri);
		return -1;
	}

	/* Cisco firmware (verified against CP-7975G/9.4.2) validates the
	 * From URI of unsolicited service-control NOTIFYs as a server
	 * identity URI WITHOUT a user part. ast_sip_create_request defaults
	 * the From URI's user to the endpoint id ("1010"), which the phone
	 * rejects with 400 Bad Request. Preserve the host/port/transport
	 * Asterisk selected and clear only the user part. */
	{
		pjsip_sip_uri *from_uri;

		from_uri = cisco_tdata_from_sip_uri(tdata);
		if (from_uri) {
			from_uri->user.ptr = NULL;
			from_uri->user.slen = 0;
			from_uri->passwd.ptr = NULL;
			from_uri->passwd.slen = 0;
		} else {
			ast_log(LOG_WARNING,
				"cisco-svc-ctrl: unable to clear From user for %s\n",
				contact->uri);
		}
	}

	ast_sip_add_header(tdata, "Event", event_hdr);
	ast_sip_add_header(tdata, "Subscription-State", sub_state_hdr);

	if (action_str) {
		pj_str_t body_type;
		pj_str_t body_subtype;
		pj_str_t body_text;
		char synth_cid[64];
		/* The phone validates RegisterCallId against the Call-ID of
		 * its own REGISTER and 400s a mismatch. The registrar records
		 * that per contact, so use this contact's value — on a shared
		 * line (max_contacts>1) each phone has a different one, and the
		 * NOTIFY must carry the Call-ID of the phone it's addressed to.
		 * A registrar-created contact always has it; only fall back to
		 * a synthetic value (which the phone will reject) if it's
		 * somehow empty — re-REGISTER the phone to recover. */
		const char *register_cid = contact->call_id;

		body_str = ast_str_create(512);
		if (!body_str) {
			pjsip_tx_data_dec_ref(tdata);
			return -1;
		}

		if (ast_strlen_zero(register_cid)) {
			snprintf(synth_cid, sizeof(synth_cid), "%08x@asterisk",
				(unsigned) ast_random());
			register_cid = synth_cid;
			ast_log(LOG_NOTICE,
				"cisco-svc-ctrl: contact %s has no recorded REGISTER "
				"Call-ID — sending synthetic, phone will likely reject "
				"the %s; re-REGISTER it (power-cycle / kill its TCP) "
				"and retry\n", contact->uri, action_str);
		}
		/* Body matches the four-stamp shape that the chan_sip
		 * sip_notify.conf [cisco-restart] template produces and
		 * that Cisco firmware accepts on this fleet (verified
		 * 200 OK + reboot via tcpdump against a CP-8861 running
		 * 14.1.1 over chan_sip on 2026-05-10). The
		 * cisco-usecallmanager patch's source has a fifth
		 * FeatureControlVersionStamp line, but adding it makes
		 * the firmware return 400 Bad Request — sticking to the
		 * four-stamp shape that's known to work. */
		ast_str_set(&body_str, 0,
			"action=%s\r\n"
			"RegisterCallId={%s}\r\n"
			"ConfigVersionStamp={00000000-0000-0000-0000-000000000000}\r\n"
			"DialplanVersionStamp={00000000-0000-0000-0000-000000000000}\r\n"
			"SoftkeyVersionStamp={00000000-0000-0000-0000-000000000000}\r\n",
			action_str, register_cid);

		pj_strset2(&body_type, "text");
		pj_strset2(&body_subtype, "plain");
		pj_strdup2(tdata->pool, &body_text, ast_str_buffer(body_str));
		ast_free(body_str);
		tdata->msg->body = pjsip_msg_body_create(tdata->pool, &body_type,
			&body_subtype, &body_text);
		if (!tdata->msg->body) {
			pjsip_tx_data_dec_ref(tdata);
			return -1;
		}
	}

	if (ast_sip_send_request(tdata, NULL, endpoint, NULL, NULL)) {
		ast_log(LOG_WARNING, "cisco-svc-ctrl: send_request failed for %s\n",
			contact->uri);
		return -1;
	}

	ast_log(LOG_NOTICE, "cisco-svc-ctrl: sent %s NOTIFY (Event: %s) to %s\n",
		action == SC_CHECK_SYNC ? "check-sync" :
			action == SC_RESTART ? "restart" : "reset",
		event_hdr, contact->uri);
	return 0;
}

/*!
 * \brief Send the requested service-control NOTIFY to an endpoint's
 *        registered contacts.
 *
 * \param contact_filter when non-NULL, only contacts whose URI or
 *        sorcery id (the @hash) contains this substring are targeted —
 *        lets you restart one phone of a shared line. NULL = all
 *        contacts.
 *
 * \retval 0 at least one NOTIFY was dispatched
 * \retval -1 endpoint not found / not Cisco / no matching contacts
 */
static int sc_dispatch(const char *endpoint_id, const char *contact_filter,
	enum sc_action action, struct ast_cli_args *a)
{
	struct ast_sip_endpoint *endpoint;
	struct cisco_endpoint *cisco;
	int sent = 0;

	endpoint = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(),
		"endpoint", endpoint_id);
	if (!endpoint) {
		ast_cli(a->fd, "Endpoint '%s' not found\n", endpoint_id);
		return -1;
	}

	cisco = cisco_endpoint_get(endpoint_id);
	if (!cisco) {
		ast_cli(a->fd, "Endpoint '%s' has no [type=cisco] section "
			"in pjsip.conf — service-control NOTIFY skipped\n",
			endpoint_id);
		ao2_cleanup(endpoint);
		return -1;
	}
	ao2_cleanup(cisco);

	if (ast_strlen_zero(endpoint->aors)) {
		ast_cli(a->fd, "Endpoint '%s' has no AORs configured\n", endpoint_id);
		ao2_cleanup(endpoint);
		return -1;
	}

	{
		struct ao2_container *contacts;
		struct ao2_iterator iter;
		struct ast_sip_contact *contact;

		contacts = ast_sip_location_retrieve_contacts_from_aor_list(
			endpoint->aors);
		if (contacts) {
			iter = ao2_iterator_init(contacts, 0);
			while ((contact = ao2_iterator_next(&iter))) {
				if (contact_filter
					&& !strstr(contact->uri, contact_filter)
					&& !strstr(ast_sorcery_object_get_id(contact),
						contact_filter)) {
					ao2_cleanup(contact);
					continue;
				}
				if (sc_send_notify(endpoint, contact, action) == 0) {
					sent++;
					ast_cli(a->fd, "  -> %s\n", contact->uri);
				}
				ao2_cleanup(contact);
			}
			ao2_iterator_destroy(&iter);
			ao2_cleanup(contacts);
		}
	}

	ao2_cleanup(endpoint);

	if (!sent) {
		if (contact_filter) {
			ast_cli(a->fd, "No registered contact of '%s' matches '%s'\n",
				endpoint_id, contact_filter);
		} else {
			ast_cli(a->fd, "No registered contacts found for '%s'\n",
				endpoint_id);
		}
		return -1;
	}
	return 0;
}

/* Tab-completion: the Nth Cisco endpoint id (i.e. id of a type=cisco
 * sorcery object) that starts with \a word, or NULL when exhausted. */
static char *complete_cisco_endpoint(const char *word, int n)
{
	struct ao2_container *objs;
	struct ao2_iterator iter;
	void *obj;
	char *ret = NULL;
	int which = 0;
	size_t wlen = strlen(word);

	objs = ast_sorcery_retrieve_by_fields(ast_sip_get_sorcery(), "cisco",
		AST_RETRIEVE_FLAG_MULTIPLE | AST_RETRIEVE_FLAG_ALL, NULL);
	if (!objs) {
		return NULL;
	}
	iter = ao2_iterator_init(objs, 0);
	while ((obj = ao2_iterator_next(&iter))) {
		const char *id = ast_sorcery_object_get_id(obj);

		if (!strncasecmp(id, word, wlen) && which++ == n) {
			ret = ast_strdup(id);
		}
		ao2_ref(obj, -1);
		if (ret) {
			break;
		}
	}
	ao2_iterator_destroy(&iter);
	ao2_ref(objs, -1);
	return ret;
}

/* Tab-completion: the Nth registered-contact URI of \a endpoint_id whose
 * URI starts with \a word, or NULL when exhausted. */
static char *complete_cisco_contact(const char *endpoint_id,
	const char *word, int n)
{
	struct ast_sip_endpoint *endpoint;
	struct ao2_container *contacts;
	struct ao2_iterator iter;
	struct ast_sip_contact *contact;
	char *ret = NULL;
	int which = 0;
	size_t wlen = strlen(word);

	endpoint = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(),
		"endpoint", endpoint_id);
	if (!endpoint) {
		return NULL;
	}
	if (ast_strlen_zero(endpoint->aors)) {
		ao2_cleanup(endpoint);
		return NULL;
	}
	contacts = ast_sip_location_retrieve_contacts_from_aor_list(endpoint->aors);
	ao2_cleanup(endpoint);
	if (!contacts) {
		return NULL;
	}
	iter = ao2_iterator_init(contacts, 0);
	while ((contact = ao2_iterator_next(&iter))) {
		if (!strncasecmp(contact->uri, word, wlen) && which++ == n) {
			ret = ast_strdup(contact->uri);
		}
		ao2_cleanup(contact);
		if (ret) {
			break;
		}
	}
	ao2_iterator_destroy(&iter);
	ao2_cleanup(contacts);
	return ret;
}

/* Shared CLI_GENERATE handler: arg 3 = endpoint, optional arg 4 = contact. */
static char *cli_cisco_generate(struct ast_cli_args *a)
{
	if (a->pos == 3) {
		return complete_cisco_endpoint(a->word, a->n);
	}
	if (a->pos == 4) {
		return complete_cisco_contact(a->argv[3], a->word, a->n);
	}
	return NULL;
}

/* Shared command handler: "pjsip cisco <verb> <endpoint> [contact]". */
static char *cli_cisco_run(struct ast_cli_args *a, enum sc_action action)
{
	if (a->argc < 4 || a->argc > 5) {
		return CLI_SHOWUSAGE;
	}
	if (sc_dispatch(a->argv[3], a->argc == 5 ? a->argv[4] : NULL,
			action, a) < 0) {
		return CLI_FAILURE;
	}
	return CLI_SUCCESS;
}

static char *cli_cisco_check_sync(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip cisco check-sync";
		e->usage =
			"Usage: pjsip cisco check-sync <endpoint> [contact]\n"
			"   Send a Cisco check-sync NOTIFY so the phone re-fetches\n"
			"   its TFTP/HTTP config without restarting. With [contact]\n"
			"   (a URI substring or @hash) only that phone of a shared\n"
			"   line is targeted; omit it to hit every registered contact.\n";
		return NULL;
	case CLI_GENERATE:
		return cli_cisco_generate(a);
	}
	return cli_cisco_run(a, SC_CHECK_SYNC);
}

static char *cli_cisco_restart(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip cisco restart";
		e->usage =
			"Usage: pjsip cisco restart <endpoint> [contact]\n"
			"   Send a Cisco service-control restart NOTIFY (soft\n"
			"   restart that preserves call state where possible). With\n"
			"   [contact] (a URI substring or @hash) only that phone of a\n"
			"   shared line is restarted; omit it for every registered\n"
			"   contact.\n";
		return NULL;
	case CLI_GENERATE:
		return cli_cisco_generate(a);
	}
	return cli_cisco_run(a, SC_RESTART);
}

static char *cli_cisco_reset(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip cisco reset";
		e->usage =
			"Usage: pjsip cisco reset <endpoint> [contact]\n"
			"   Send a Cisco service-control reset NOTIFY (hard reboot\n"
			"   equivalent). With [contact] (a URI substring or @hash)\n"
			"   only that phone of a shared line is reset; omit it for\n"
			"   every registered contact.\n";
		return NULL;
	case CLI_GENERATE:
		return cli_cisco_generate(a);
	}
	return cli_cisco_run(a, SC_RESET);
}

static struct ast_cli_entry cli_cisco_cmds[] = {
	AST_CLI_DEFINE(cli_cisco_check_sync, "Send Cisco check-sync NOTIFY"),
	AST_CLI_DEFINE(cli_cisco_restart,    "Send Cisco service-control restart NOTIFY"),
	AST_CLI_DEFINE(cli_cisco_reset,      "Send Cisco service-control reset NOTIFY"),
};

static int load_module(void)
{
	ast_cli_register_multiple(cli_cisco_cmds, ARRAY_LEN(cli_cisco_cmds));
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	ast_cli_unregister_multiple(cli_cisco_cmds, ARRAY_LEN(cli_cisco_cmds));
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco service-control CLI commands",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_cisco_endpoint",
);
