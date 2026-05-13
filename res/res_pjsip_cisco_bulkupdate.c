/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_bulkupdate
 *
 * After a successful REGISTER from a Cisco Enterprise SIP firmware
 * phone (7975 / 8861 / 8865 etc.), send an unsolicited REFER carrying
 * the multipart application/x-cisco-remotecc-request+xml bulkupdate
 * body. This is what classifies each line button as BLF Speed Dial
 * (hook icon) instead of plain Speed Dial (keypad icon) in the
 * firmware's UI.
 *
 * Without this REFER, the optionsind body in the REGISTER 200 OK
 * (sent by res_pjsip_cisco_register_optionsind) tells the firmware
 * "this server supports BLF Speed Dial in principle" but the firmware
 * never receives per-line config to act on it. This is the third and
 * final piece of the chan_sip cisco-usecallmanager patch's REGISTER
 * flow. Spec is the patch's
 * channels/sip/peers.c:2824 sip_peer_send_bulk_update.
 *
 * Body sent (one REFER, multipart/mixed body, three parts):
 *
 *   --uniqueBoundary
 *   Content-Type: application/x-cisco-remotecc-request+xml
 *
 *   <x-cisco-remotecc-request>
 *     <dndupdate>
 *       <state>disable</state>
 *       <option>ringeroff</option>
 *     </dndupdate>
 *   </x-cisco-remotecc-request>
 *
 *   --uniqueBoundary
 *   Content-Type: application/x-cisco-remotecc-request+xml
 *
 *   <x-cisco-remotecc-request>
 *     <hlogupdate><status>off</status></hlogupdate>
 *   </x-cisco-remotecc-request>
 *
 *   --uniqueBoundary
 *   Content-Type: application/x-cisco-remotecc-request+xml
 *
 *   <x-cisco-remotecc-request>
 *     <bulkupdate>
 *       <contact line="1">
 *         <mwi>no</mwi>
 *         <emwi><voice-msg new="0" old="0" /></emwi>
 *         <cfwdallupdate>
 *           <fwdaddress></fwdaddress>
 *           <tovoicemail>off</tovoicemail>
 *         </cfwdallupdate>
 *       </contact>
 *     </bulkupdate>
 *   </x-cisco-remotecc-request>
 *
 *   --uniqueBoundary--
 *
 * Runtime values:
 *   - line_index and dnd_busy come from [name] type=cisco
 *   - DND, hunt-group and call-forward state come from astdb
 *   - MWI counts come from endpoint/AOR mailbox configuration
 *
 * Mechanism: a session_supplement on REGISTER would be cleanest but
 * REGISTER isn't a session, so we use a regular ast_sip_supplement
 * with outgoing_response and queue a deferred task only for successful
 * REGISTER 200 OK responses. That keeps the REFER tied to a registrar
 * success path and to contacts that have already been updated.
 */

/*** MODULEINFO
	<depend>res_pjsip</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip/sip_multipart.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/cli.h"
#include "asterisk/taskprocessor.h"
#include "asterisk/astdb.h"
#include "asterisk/app.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"

#include "cisco_endpoint.h"

/*
 * Per-part XML format strings. The three parts together get wrapped
 * in a multipart/mixed body by pjsip_multipart_create + add_part. We
 * use pjsip's proper multipart API rather than hand-rolling the
 * boundary/Content-Type lines because pjsip asserts internally that
 * any body with a multipart/<anything> type uses its own internal
 * multipart_print_body callback
 * callback, and a hand-rolled body with a custom print_body trips
 * that assert in pjproject's transport layer.
 */
#define DND_PART_FMT                                                    \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                  \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <dndupdate>\n"                                               \
	"    <state>%s</state>\n"                                       \
	"    <option>%s</option>\n"                                     \
	"  </dndupdate>\n"                                              \
	"</x-cisco-remotecc-request>\n"

#define HLOG_PART_FMT                                                   \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                  \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <hlogupdate>\n"                                              \
	"    <status>%s</status>\n"                                     \
	"  </hlogupdate>\n"                                             \
	"</x-cisco-remotecc-request>\n"

/* BULK_PART split into header / per-contact / footer so we can emit
 * a <contact line="N"> element for each line button on multi-line
 * Cisco phones. The chan_sip patch's
 * channels/sip/chan_sip.c sip_send_bulkupdate uses the same shape —
 * one <bulkupdate> block wrapping N <contact> elements, one per
 * sip_alias / peer->aliases entry. */
#define BULK_PART_HEADER                                                \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                  \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <bulkupdate>\n"

#define BULK_CONTACT_FMT                                                \
	"    <contact line=\"%d\">\n"                                   \
	"      <mwi>%s</mwi>\n"                                         \
	"      <emwi><voice-msg new=\"%d\" old=\"%d\" /></emwi>\n"      \
	"      <cfwdallupdate>\n"                                       \
	"        <fwdaddress>%s</fwdaddress>\n"                         \
	"        <tovoicemail>%s</tovoicemail>\n"                       \
	"      </cfwdallupdate>\n"                                      \
	"    </contact>\n"

#define BULK_PART_FOOTER                                                \
	"  </bulkupdate>\n"                                             \
	"</x-cisco-remotecc-request>\n"

static struct ast_taskprocessor *bulkupdate_serializer;
static struct ao2_container *bulkupdate_addr_cache;

/*!
 * \brief Build a multipart/mixed body with the literal "uniqueBoundary"
 *        boundary, exactly matching what the chan_sip patch sends.
 *
 * Feature-state getters (DND / HuntGroup / CF) live in cisco_endpoint.h
 * as cisco_{dnd,huntgroup,cfwd}_{is_enabled,is_in,get,set}() — same
 * astdb keys, shared with feature_events and remotecc. See README for
 * key conventions.
 */

/*!
 * \brief Aggregate MWI counts for an endpoint by walking its mailboxes=
 *        and AOR mailboxes= settings (the standard PJSIP convention).
 */
static void compute_mwi(struct ast_sip_endpoint *endpoint,
	int *mwi_new, int *mwi_old)
{
	*mwi_new = *mwi_old = 0;

	if (!ast_strlen_zero(endpoint->subscription.mwi.mailboxes)) {
		ast_app_inboxcount(endpoint->subscription.mwi.mailboxes,
			mwi_new, mwi_old);
		return;
	}
	if (ast_strlen_zero(endpoint->aors)) {
		return;
	}

	{
		struct ast_str *all_mb = ast_str_create(256);
		char *aors_for_mwi;
		char *aor_for_mwi;

		if (!all_mb) {
			return;
		}
		aors_for_mwi = ast_strdupa(endpoint->aors);
		while ((aor_for_mwi = ast_strip(strsep(&aors_for_mwi, ",")))) {
			struct ast_sip_aor *aor;
			if (ast_strlen_zero(aor_for_mwi)) {
				continue;
			}
			aor = ast_sip_location_retrieve_aor(aor_for_mwi);
			if (!aor) {
				continue;
			}
			if (!ast_strlen_zero(aor->mailboxes)) {
				if (ast_str_strlen(all_mb)) {
					ast_str_append(&all_mb, 0, ",");
				}
				ast_str_append(&all_mb, 0, "%s", aor->mailboxes);
			}
			ao2_ref(aor, -1);
		}
		if (ast_str_strlen(all_mb)) {
			ast_app_inboxcount(ast_str_buffer(all_mb), mwi_new, mwi_old);
		}
		ast_free(all_mb);
	}
}

/*!
 * \brief Append one <contact line="N">...</contact> block for \a endpoint
 *        (with its own MWI / CF state) to the bulkupdate body buffer.
 *
 * Called once for the primary endpoint and once per cisco->aliases
 * entry. Each alias's <contact> carries its OWN per-line data —
 * cisco line_index, MWI counts from its own AOR mailboxes, CF state
 * from its own astdb CF/<alias-endpoint-id> key.
 */
static void append_bulk_contact(struct ast_str **out,
	struct ast_sip_endpoint *endpoint, int line_index)
{
	int mwi_new, mwi_old;
	char cf_buf[64];
	char cf_escaped[512] = "";
	const char *cf_target;
	int cf_to_vm = 0;
	char contact_buf[1024];
	const char *endpoint_id = ast_sorcery_object_get_id(endpoint);

	compute_mwi(endpoint, &mwi_new, &mwi_old);

	cf_target = cisco_cfwd_get(endpoint_id, cf_buf, sizeof(cf_buf));
	if (!ast_strlen_zero(cf_target)
		&& endpoint->subscription.mwi.voicemail_extension
		&& !strcmp(cf_target,
			endpoint->subscription.mwi.voicemail_extension)) {
		cf_to_vm = 1;
	}
	if (!ast_strlen_zero(cf_target)
		&& ast_xml_escape(cf_target, cf_escaped, sizeof(cf_escaped))) {
		ast_log(LOG_WARNING,
			"cisco-bulkupdate: %s call-forward target too long after "
			"XML escaping; omitting\n", endpoint_id);
		cf_escaped[0] = '\0';
	}

	snprintf(contact_buf, sizeof(contact_buf), BULK_CONTACT_FMT,
		line_index,
		mwi_new ? "yes" : "no",
		mwi_new, mwi_old,
		cf_escaped,
		cf_to_vm ? "on" : "off");

	ast_str_append(out, 0, "%s", contact_buf);
}

static pjsip_msg_body *bulkupdate_make_body(pj_pool_t *pool, int dnd_enabled,
	int dnd_busy, int hg_in, const char *contacts_xml)
{
	pjsip_msg_body *multipart;
	pj_str_t boundary = pj_str("uniqueBoundary");
	struct ast_str *bulk;
	char buf[2048];

	multipart = pjsip_multipart_create(pool, NULL, &boundary);
	if (!multipart) {
		return NULL;
	}

	snprintf(buf, sizeof(buf), DND_PART_FMT,
		dnd_enabled ? "enable" : "disable",
		dnd_busy    ? "callreject" : "ringeroff");
	cisco_remotecc_multipart_add_part(pool, multipart, buf);

	snprintf(buf, sizeof(buf), HLOG_PART_FMT, hg_in ? "on" : "off");
	cisco_remotecc_multipart_add_part(pool, multipart, buf);

	/* Wrap the caller-supplied <contact> blocks in the bulkupdate
	 * header + footer. One block per line button on multi-line phones
	 * (see cisco->aliases). MUST return NULL if we can't allocate —
	 * Cisco firmware does NOT tolerate a multipart REFER missing its
	 * <bulkupdate> part (which carries the per-line MWI / CF state). */
	bulk = ast_str_create(4096);
	if (!bulk) {
		ast_log(LOG_ERROR,
			"cisco-bulkupdate: ast_str_create failed for bulkupdate body — "
			"aborting REFER rather than sending a partial multipart\n");
		return NULL;
	}
	ast_str_set(&bulk, 0, "%s%s%s",
		BULK_PART_HEADER, contacts_xml, BULK_PART_FOOTER);
	cisco_remotecc_multipart_add_part(pool, multipart,
		ast_str_buffer(bulk));
	ast_free(bulk);

	return multipart;
}

/*! Container we pass to the task processor. */
struct bulkupdate_task_data {
	struct ast_sip_endpoint *endpoint;
	/* Canonical Contact-set string captured in the response hook
	 * (where tdata->msg is still alive). NULL for CLI-driven
	 * invocations — those bypass the address-change cache entirely. */
	char *canonical;
};

static void bulkupdate_task_data_destroy(void *obj)
{
	struct bulkupdate_task_data *data = obj;
	ast_free(data->canonical);
	ao2_cleanup(data->endpoint);
}

struct bulkupdate_build_ctx {
	int dnd_enabled;
	int dnd_busy;
	int hg_in;
	const char *contacts_xml;
};

static pjsip_msg_body *bulkupdate_build_adapter(pj_pool_t *pool, void *vctx)
{
	struct bulkupdate_build_ctx *ctx = vctx;
	return bulkupdate_make_body(pool, ctx->dnd_enabled, ctx->dnd_busy,
		ctx->hg_in, ctx->contacts_xml);
}

/*!
 * \brief Task body: walk the endpoint's AOR contacts and send a REFER
 *        with the bulkupdate multipart body to each registered contact.
 */
static int bulkupdate_send_task(void *obj)
{
	struct bulkupdate_task_data *data = obj;
	struct cisco_endpoint *cisco;
	const char *endpoint_id;
	int line_index = 1;
	int dnd_busy = 0;
	int dnd_enabled;
	int hg_in;
	struct ast_str *contacts_xml;
	char *aliases_copy = NULL;
	char *alias_id;
	int alias_count = 0;
	int attempted = 0;
	int succeeded = 0;

	endpoint_id = ast_sorcery_object_get_id(data->endpoint);

	if (ast_strlen_zero(data->endpoint->aors)) {
		ao2_cleanup(data);
		return 0;
	}

	cisco = cisco_endpoint_get(endpoint_id);
	if (cisco) {
		if (cisco->line_index > 0) {
			line_index = cisco->line_index;
		}
		dnd_busy = cisco->dnd_busy;
		if (!ast_strlen_zero(cisco->aliases)) {
			aliases_copy = ast_strdupa(cisco->aliases);
		}
		ao2_cleanup(cisco);
	}

	dnd_enabled = cisco_dnd_is_enabled(endpoint_id);
	hg_in       = cisco_huntgroup_is_in(endpoint_id);

	/* Build the <contact> blocks: primary endpoint first, then one
	 * per alias listed in cisco->aliases. Each block carries its own
	 * line_index, MWI counts, and CF state — sourced from that
	 * specific alias's cisco sorcery / AOR mailboxes / astdb. */
	contacts_xml = ast_str_create(4096);
	if (!contacts_xml) {
		ao2_cleanup(data);
		return 0;
	}
	append_bulk_contact(&contacts_xml, data->endpoint, line_index);

	if (aliases_copy) {
		while ((alias_id = ast_strip(strsep(&aliases_copy, ",")))) {
			struct ast_sip_endpoint *alias_ep;
			struct cisco_endpoint *alias_cisco;
			int alias_line;

			if (ast_strlen_zero(alias_id)) {
				continue;
			}
			alias_ep = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(),
				"endpoint", alias_id);
			if (!alias_ep) {
				ast_log(LOG_WARNING,
					"cisco-bulkupdate: alias endpoint '%s' (declared "
					"on %s) not found — skipping\n",
					alias_id, endpoint_id);
				continue;
			}
			alias_cisco = cisco_endpoint_get(alias_id);
			if (!alias_cisco) {
				ast_log(LOG_WARNING,
					"cisco-bulkupdate: alias endpoint '%s' has no "
					"[name] type=cisco section — skipping\n", alias_id);
				ao2_cleanup(alias_ep);
				continue;
			}
			alias_line = alias_cisco->line_index > 0
				? alias_cisco->line_index : 1;
			ao2_cleanup(alias_cisco);

			append_bulk_contact(&contacts_xml, alias_ep, alias_line);
			alias_count++;
			ao2_cleanup(alias_ep);
		}
	}

	ast_log(LOG_NOTICE,
		"cisco-bulkupdate: building body for '%s' "
		"(primary line=%d, dnd=%s, dnd_busy=%s, hg=%s, +%d alias line%s)\n",
		endpoint_id, line_index,
		dnd_enabled ? "enable" : "disable",
		dnd_busy    ? "callreject" : "ringeroff",
		hg_in       ? "on" : "off",
		alias_count, alias_count == 1 ? "" : "s");

	{
		struct bulkupdate_build_ctx ctx = {
			.dnd_enabled  = dnd_enabled,
			.dnd_busy     = dnd_busy,
			.hg_in        = hg_in,
			.contacts_xml = ast_str_buffer(contacts_xml),
		};
		cisco_endpoint_send_refer_to_all_contacts(data->endpoint,
			"cisco-bulkupdate", "cisco-bulkupdate", "REFER",
			bulkupdate_build_adapter, &ctx, &attempted, &succeeded);
	}

	ast_free(contacts_xml);

	/* Commit the address-change cache only when ALL intended REFERs
	 * (one per registered contact) actually went on the wire. With
	 * max_contacts > 1, a partial success would leave the cache marked
	 * "fired" and the failed contact would never get retried until the
	 * Contact set itself changed — caching per-endpoint, not per-contact,
	 * makes "all or nothing" the only correct commit policy here.
	 * Skip when data->canonical is NULL (CLI-triggered path — not
	 * cache-gated; it's a forced push). */
	if (attempted > 0 && attempted == succeeded && data->canonical) {
		cisco_register_address_remember_str(endpoint_id,
			bulkupdate_addr_cache, data->canonical);
	} else if (attempted != succeeded) {
		ast_log(LOG_NOTICE,
			"cisco-bulkupdate: %d/%d REFERs delivered for '%s' — leaving "
			"address cache uncommitted so the next REGISTER retries\n",
			succeeded, attempted, endpoint_id);
	}

	ao2_cleanup(data);
	return 0;
}

/*!
 * \brief Hook on outgoing REGISTER 200 OK. If it's a Cisco endpoint,
 *        queue an async task to send the bulkupdate REFER.
 */
static void bulkupdate_outgoing_response(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact, pjsip_tx_data *tdata)
{
	struct bulkupdate_task_data *data;
	char *canonical = NULL;

	if (!cisco_register_should_fire(endpoint, tdata, bulkupdate_addr_cache,
			NULL, &canonical)) {
		return;
	}

	data = ao2_alloc(sizeof(*data), bulkupdate_task_data_destroy);
	if (!data) {
		ast_free(canonical);
		return;
	}
	ao2_ref(endpoint, +1);
	data->endpoint  = endpoint;
	data->canonical = canonical;  /* take ownership */

	if (ast_sip_push_task(bulkupdate_serializer, bulkupdate_send_task, data)) {
		ast_log(LOG_WARNING, "cisco-bulkupdate: failed to queue task\n");
		ao2_cleanup(data);
		return;
	}

	(void) contact;
}

static struct ast_sip_supplement bulkupdate_supplement = {
	.method            = "REGISTER",
	.priority          = AST_SIP_SUPPLEMENT_PRIORITY_LAST,
	.outgoing_response = bulkupdate_outgoing_response,
};

/*!
 * \brief CLI: pjsip cisco bulkupdate <endpoint>
 *
 * Manually trigger a fresh bulkupdate REFER to a Cisco endpoint —
 * the same one a successful REGISTER would have produced. Reads
 * current astdb DND / HuntGroup / CF state at send time, so dialplan
 * toggles (e.g. business-hours DND, on-call rotation CFwdALL) that
 * write the astdb keys directly can push the new state to the phone
 * without waiting for the next REGISTER refresh.
 *
 * Typical dialplan use:
 *   exten => *78,1,Set(DB(DND/${CALLERID(num)})=YES)
 *    same => n,System(asterisk -rx 'pjsip cisco bulkupdate ${CALLERID(num)}')
 *
 * Mirrors the chan_sip cisco-usecallmanager patch's behaviour where
 * DND / CF state mutations are pushed back to the phone via fresh
 * REFER traffic. The patch fires implicitly from chan_sip's own peer
 * mutation hooks; on PJSIP the source of truth is astdb, which has
 * no built-in change notification, so we expose an explicit trigger
 * instead.
 */
static char *cli_cisco_bulkupdate(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	struct ast_sip_endpoint *endpoint;
	struct cisco_endpoint *cisco;
	struct bulkupdate_task_data *data;

	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip cisco bulkupdate";
		e->usage =
			"Usage: pjsip cisco bulkupdate <endpoint>\n"
			"   Send a Cisco bulkupdate REFER to <endpoint> carrying the\n"
			"   current astdb DND / HuntGroup / call-forward state plus\n"
			"   live MWI counts. Use after dialplan / AMI writes to the\n"
			"   DND/<endpoint> or CF/<endpoint> astdb keys to push the\n"
			"   new state to the phone's local UI.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 4) {
		return CLI_SHOWUSAGE;
	}

	endpoint = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(),
		"endpoint", a->argv[3]);
	if (!endpoint) {
		ast_cli(a->fd, "Endpoint '%s' not found\n", a->argv[3]);
		return CLI_FAILURE;
	}

	cisco = cisco_endpoint_get(a->argv[3]);
	if (!cisco) {
		ast_cli(a->fd, "Endpoint '%s' has no [name] type=cisco "
			"section in pjsip.conf — bulkupdate not applicable\n",
			a->argv[3]);
		ao2_cleanup(endpoint);
		return CLI_FAILURE;
	}
	ao2_cleanup(cisco);

	if (ast_strlen_zero(endpoint->aors)) {
		ast_cli(a->fd, "Endpoint '%s' has no AORs configured\n", a->argv[3]);
		ao2_cleanup(endpoint);
		return CLI_FAILURE;
	}

	data = ao2_alloc(sizeof(*data), bulkupdate_task_data_destroy);
	if (!data) {
		ao2_cleanup(endpoint);
		return CLI_FAILURE;
	}
	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;

	if (ast_sip_push_task(bulkupdate_serializer, bulkupdate_send_task, data)) {
		ast_cli(a->fd, "Failed to queue bulkupdate task for '%s'\n",
			a->argv[3]);
		ao2_cleanup(data);
		ao2_cleanup(endpoint);
		return CLI_FAILURE;
	}

	ast_cli(a->fd, "Bulkupdate REFER queued for '%s'\n", a->argv[3]);
	ao2_cleanup(endpoint);
	return CLI_SUCCESS;
}

static struct ast_cli_entry bulkupdate_cli_cmds[] = {
	AST_CLI_DEFINE(cli_cisco_bulkupdate,
		"Push a fresh Cisco bulkupdate REFER (DND/CF/MWI state)"),
};

static int load_module(void)
{
	bulkupdate_serializer = ast_sip_create_serializer("pjsip/cisco-bulkupdate");
	if (!bulkupdate_serializer) {
		return AST_MODULE_LOAD_DECLINE;
	}
	bulkupdate_addr_cache = cisco_addr_cache_alloc();
	if (!bulkupdate_addr_cache) {
		ast_taskprocessor_unreference(bulkupdate_serializer);
		bulkupdate_serializer = NULL;
		return AST_MODULE_LOAD_DECLINE;
	}
	ast_sip_register_supplement(&bulkupdate_supplement);
	ast_cli_register_multiple(bulkupdate_cli_cmds,
		ARRAY_LEN(bulkupdate_cli_cmds));
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	/*
	 * Refuse to unload at runtime. The supplement we register feeds
	 * tasks (function pointers into our .so) into bulkupdate_serializer.
	 * Pending tasks may still be in the serializer's queue when unload
	 * runs, and the framework will dlclose() our .so as soon as we
	 * return - if a queued task then fires we'd jump into freed memory.
	 *
	 * Same idiom res_pjsip_mwi uses for the same reason. Live edits
	 * require a full asterisk restart; this is the documented pattern
	 * for asterisk-pjsip-cisco anyway.
	 */
	if (!ast_shutdown_final()) {
		return -1;
	}
	ast_cli_unregister_multiple(bulkupdate_cli_cmds,
		ARRAY_LEN(bulkupdate_cli_cmds));
	ast_sip_unregister_supplement(&bulkupdate_supplement);
	ao2_cleanup(bulkupdate_addr_cache);
	bulkupdate_addr_cache = NULL;
	ast_taskprocessor_unreference(bulkupdate_serializer);
	bulkupdate_serializer = NULL;
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco RemoteCC bulkupdate REFER post-REGISTER",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_cisco_endpoint",
);
