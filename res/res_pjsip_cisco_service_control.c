/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_service_control
 *
 * Operator CLI for a registered Cisco Enterprise SIP firmware phone:
 * service-control NOTIFYs (check-sync / restart / reset / prt-report)
 * plus a read-only status diagnostic.
 *
 *   pjsip cisco status <endpoint>
 *      Read-only dump of the cisco sorcery config + registered
 *      contacts (URI / User-Agent / Via / expiry) + MWI counts
 *      bulkupdate would send + astdb feature state (DND / HuntGroup
 *      / CF). The shape you want when debugging "why don't the BLF
 *      lamps light" or "why doesn't DND register on the phone" or
 *      "why is the MWI lamp wrong".
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
 *   pjsip cisco prt-report <endpoint> [contact]
 *      Same service-control envelope, body action=prt-report (just
 *      action + RegisterCallId — no version stamps). Tells the phone
 *      to run its Problem Report Tool and upload the bundle to the
 *      URL configured in its SEP file. No reboot.
 *
 * The optional [contact] is a substring matched against each
 * registered contact's URI or its @hash — restarts one phone of a
 * shared line (max_contacts>1). Omitted = every registered contact.
 * Tab-completion: arg 1 = type=cisco endpoint ids, arg 2 = that
 * endpoint's contact URIs.
 *
 * These replace chan_sip + cisco-usecallmanager-patch's
 *   sip notify cisco-check-cfg   <peer>
 *   sip notify cisco-restart     <peer>
 *   sip notify cisco-reset       <peer>
 *   sip notify cisco-prt-report  <peer>
 * which were driven from sip_notify.conf templates.
 *
 * Gating: only operates on endpoints that have a [name] type=cisco
 * sorcery section (defined by res_pjsip_cisco_endpoint).
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
#include "asterisk/time.h"
#include "asterisk/cli.h"
#include "asterisk/astobj2.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"

#include "cisco/endpoint.h"
#include "cisco/rdata.h"

enum sc_action {
	SC_CHECK_SYNC,
	SC_RESTART,
	SC_RESET,
	SC_PRT_REPORT,
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
	case SC_PRT_REPORT:
		/* Same service-control envelope as restart/reset; the phone
		 * runs its Problem Report Tool and uploads the bundle to the
		 * URL configured in its SEP file (problemReportServerUrl /
		 * problemReportUploadURL). No reboot. */
		event_hdr     = "service-control";
		sub_state_hdr = "active";
		action_str    = "prt-report";
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
		 * four-stamp shape that's known to work.
		 *
		 * prt-report is a two-line variant: the chan_sip patch's
		 * [cisco-prt-report] template carries only action and
		 * RegisterCallId (no version stamps), and that's what
		 * Cisco firmware accepts here. */
		if (action == SC_PRT_REPORT) {
			ast_str_set(&body_str, 0,
				"action=%s\r\n"
				"RegisterCallId={%s}\r\n",
				action_str, register_cid);
		} else {
			ast_str_set(&body_str, 0,
				"action=%s\r\n"
				"RegisterCallId={%s}\r\n"
				"ConfigVersionStamp={00000000-0000-0000-0000-000000000000}\r\n"
				"DialplanVersionStamp={00000000-0000-0000-0000-000000000000}\r\n"
				"SoftkeyVersionStamp={00000000-0000-0000-0000-000000000000}\r\n",
				action_str, register_cid);
		}

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
		action == SC_CHECK_SYNC ? "check-sync" : action_str,
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

static char *cli_cisco_prt_report(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip cisco prt-report";
		e->usage =
			"Usage: pjsip cisco prt-report <endpoint> [contact]\n"
			"   Send a Cisco service-control prt-report NOTIFY so the\n"
			"   phone runs its Problem Report Tool and uploads the bundle\n"
			"   to the URL configured in its SEP file\n"
			"   (problemReportServerUrl / problemReportUploadURL). No\n"
			"   reboot. With [contact] (a URI substring or @hash) only\n"
			"   that phone of a shared line is asked; omit it for every\n"
			"   registered contact.\n";
		return NULL;
	case CLI_GENERATE:
		return cli_cisco_generate(a);
	}
	return cli_cisco_run(a, SC_PRT_REPORT);
}

/*!
 * \brief `pjsip cisco status <endpoint>`. Read-only diagnostic dump:
 *        cisco sorcery config + registered contacts (URI, User-Agent,
 *        Via source, expiry) + astdb feature state (DND / HuntGroup /
 *        CF). The shape an operator wants when debugging "why don't
 *        the BLF lamps light?" / "why isn't the DND softkey doing
 *        anything?" / "is the phone behind NAT showing the right Via?".
 *
 * Nothing is mutated. Output is human-readable, mirroring
 * `pjsip show endpoint X`'s style. Stops short of cross-`.so` private
 * state (bulkupdate's address-change cache, feature_events' MAC map);
 * those would need cisco_* exports that don't exist today and aren't
 * worth adding just for this CLI.
 */
static char *cli_cisco_status(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	struct ast_sip_endpoint *endpoint;
	struct cisco_endpoint *cisco;
	struct ao2_container *contacts = NULL;
	char cfwd_target[64];
	int contact_count = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip cisco status";
		e->usage =
			"Usage: pjsip cisco status <endpoint>\n"
			"   Show config, registration, and astdb feature state for\n"
			"   <endpoint>. Read-only — nothing is changed. Useful when\n"
			"   debugging BLF lamps, DND, hunt-group, or call-forward\n"
			"   behaviour, or when checking the User-Agent / Via that the\n"
			"   phone last REGISTERed with.\n";
		return NULL;
	case CLI_GENERATE:
		return a->pos == 3 ? complete_cisco_endpoint(a->word, a->n) : NULL;
	}

	if (a->argc != 4) {
		return CLI_SHOWUSAGE;
	}

	endpoint = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(),
		"endpoint", a->argv[3]);
	cisco = cisco_endpoint_get(a->argv[3]);

	ast_cli(a->fd, "Endpoint: %s\n", a->argv[3]);
	ast_cli(a->fd, "  type=endpoint section:    %s\n",
		endpoint ? "yes" : "no");
	ast_cli(a->fd, "  type=cisco section:       %s\n", cisco ? "yes" : "no");

	if (!cisco) {
		ast_cli(a->fd, "\n(no [type=cisco] section — endpoint is not "
			"managed by this package; use 'pjsip show endpoint %s' for "
			"stock diagnostics)\n", a->argv[3]);
		ao2_cleanup(endpoint);
		return CLI_SUCCESS;
	}

	ast_cli(a->fd, "\n[type=cisco] config:\n");
	ast_cli(a->fd, "  line_index:               %d\n", cisco->line_index);
	ast_cli(a->fd, "  subscribe:                %s\n",
		S_OR(cisco->subscribe, "(none)"));
	ast_cli(a->fd, "  subscribe_context:        %s\n",
		S_OR(cisco->subscribe_context, "(default: local_sip_phone)"));
	ast_cli(a->fd, "  aliases:                  %s\n",
		S_OR(cisco->aliases, "(none)"));
	ast_cli(a->fd, "  dnd_busy:                 %s\n",
		cisco->dnd_busy ? "callreject" : "ringeroff");
	ast_cli(a->fd, "  parkext:                  %s\n",
		S_OR(cisco->parkext, "700"));
	ast_cli(a->fd, "  keep_conference:          %s\n",
		cisco->keep_conference ? "yes" : "no");
	ao2_cleanup(cisco);

	ast_cli(a->fd, "\nRegistration:\n");
	if (endpoint && !ast_strlen_zero(endpoint->aors)) {
		contacts = ast_sip_location_retrieve_contacts_from_aor_list(
			endpoint->aors);
	}
	if (!contacts || !ao2_container_count(contacts)) {
		if (!endpoint) {
			ast_cli(a->fd, "  (no [type=endpoint] section — config drift; "
				"phone can't register)\n");
		} else if (ast_strlen_zero(endpoint->aors)) {
			ast_cli(a->fd, "  (endpoint has no AORs configured)\n");
		} else {
			ast_cli(a->fd, "  (no registered contacts)\n");
		}
	} else {
		struct ao2_iterator iter;
		struct ast_sip_contact *contact;

		iter = ao2_iterator_init(contacts, 0);
		while ((contact = ao2_iterator_next(&iter))) {
			struct timeval now = ast_tvnow();
			int64_t expires_ms = ast_tvdiff_ms(contact->expiration_time, now);

			++contact_count;
			ast_cli(a->fd, "  Contact #%d:\n", contact_count);
			ast_cli(a->fd, "    URI:        %s\n", contact->uri);
			ast_cli(a->fd, "    User-Agent: %s\n",
				S_OR(contact->user_agent, "(unknown)"));
			ast_cli(a->fd, "    Via:        %s:%d\n",
				S_OR(contact->via_addr, "(unknown)"), contact->via_port);
			ast_cli(a->fd, "    REGISTER Call-ID: %s\n",
				S_OR(contact->call_id, "(unknown)"));
			if (expires_ms > 0) {
				ast_cli(a->fd, "    Expires in: %lld seconds\n",
					(long long) (expires_ms / 1000));
			} else {
				ast_cli(a->fd,
					"    Expires in: (expired %lld seconds ago)\n",
					(long long) (-expires_ms / 1000));
			}
			ao2_cleanup(contact);
		}
		ao2_iterator_destroy(&iter);
	}
	ao2_cleanup(contacts);

	/* Cisco device facts — what we learned from REGISTER (Contact
	 * +sip.instance for MAC + src_host; Reason header for device_name /
	 * active_load / inactive_load when the phone is configured for
	 * cisco-usecallmanager-style Reason reporting). Mirrors the per-peer
	 * fields the chan_sip patch exposes via 'sip show peer'. */
	{
		struct cisco_mac_info dev;

		ast_cli(a->fd, "\nCisco device (from REGISTER):\n");
		if (cisco_mac_lookup_by_endpoint(a->argv[3], &dev)) {
			ast_cli(a->fd,
				"  (no entry — endpoint hasn't REGISTERed since module "
				"load, or its Contact carried no +sip.instance MAC)\n");
		} else {
			if (strcmp(dev.endpoint_id, a->argv[3])) {
				/* Multi-line phone: the queried endpoint is the primary
				 * (the one with aliases=), but a sibling line registered
				 * most recently and won the MAC-keyed slot. The device
				 * facts (MAC, firmware) still apply — same physical
				 * phone — but flag the indirection so the operator knows. */
				ast_cli(a->fd,
					"  (matched via alias '%s' — same physical phone)\n",
					dev.endpoint_id);
			}
			ast_cli(a->fd, "  MAC:                      %s\n", dev.mac);
			ast_cli(a->fd, "  Source host:              %s\n", dev.src_host);
			ast_cli(a->fd, "  Device name:              %s\n",
				S_OR(dev.device_name, "(no Reason header)"));
			ast_cli(a->fd, "  Active firmware load:     %s\n",
				S_OR(dev.active_load, "(no Reason header)"));
			ast_cli(a->fd, "  Inactive firmware load:   %s\n",
				S_OR(dev.inactive_load, "(no Reason header)"));
		}
	}

	/* MWI counts — what bulkupdate puts in <emwi>. Resolution shares
	 * cisco_endpoint_mwi_count() with bulkupdate's body builder, so the
	 * counts reported here match exactly what the next REGISTER 200 OK
	 * will trigger on the wire. NULL endpoint = both zero (no entry,
	 * no panic). */
	{
		int mwi_new, mwi_old;

		cisco_endpoint_mwi_count(endpoint, &mwi_new, &mwi_old);
		ast_cli(a->fd, "\nMWI:\n");
		ast_cli(a->fd, "  new:                      %d\n", mwi_new);
		ast_cli(a->fd, "  old:                      %d\n", mwi_old);
	}

	ao2_cleanup(endpoint);

	{
		/* astdb keys carry the endpoint id, so column alignment needs
		 * the full key built first and padded as a single field —
		 * matches the value column the [type=cisco] config block above
		 * lands in. */
		char dnd_key[128], hg_key[128], cf_key[128];

		snprintf(dnd_key, sizeof(dnd_key), "DND/%s:", a->argv[3]);
		snprintf(hg_key,  sizeof(hg_key),  "HuntGroup/%s:", a->argv[3]);
		snprintf(cf_key,  sizeof(cf_key),  "CF/%s:", a->argv[3]);

		cisco_cfwd_get(a->argv[3], cfwd_target, sizeof(cfwd_target));

		ast_cli(a->fd, "\nFeature state (astdb):\n");
		ast_cli(a->fd, "  %-24s  %s\n", dnd_key,
			cisco_dnd_is_enabled(a->argv[3]) ? "ON" : "off");
		ast_cli(a->fd, "  %-24s  %s\n", hg_key,
			cisco_huntgroup_is_in(a->argv[3]) ? "logged-in" : "logged-out");
		ast_cli(a->fd, "  %-24s  %s\n", cf_key,
			ast_strlen_zero(cfwd_target) ? "off" : cfwd_target);
	}

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_cisco_cmds[] = {
	AST_CLI_DEFINE(cli_cisco_status,     "Show Cisco endpoint config, registration, and feature state"),
	AST_CLI_DEFINE(cli_cisco_check_sync, "Send Cisco check-sync NOTIFY"),
	AST_CLI_DEFINE(cli_cisco_restart,    "Send Cisco service-control restart NOTIFY"),
	AST_CLI_DEFINE(cli_cisco_reset,      "Send Cisco service-control reset NOTIFY"),
	AST_CLI_DEFINE(cli_cisco_prt_report, "Send Cisco service-control prt-report NOTIFY"),
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
	"PJSIP Cisco service-control + status CLI commands",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_cisco_endpoint",
);
