/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_register_optionsind
 *
 * Attaches the Cisco RemoteCC "options indication" body to outgoing
 * 200 OK responses to REGISTER requests. This is what tells Cisco
 * Enterprise SIP firmware (7975 / 8861 / 8865) to classify line
 * buttons that subscribe to Event: presence as BLF Speed Dial type
 * (hook icon, lit/unlit LED) instead of plain Speed Dial (keypad icon,
 * no presence reaction).
 *
 * Without this body, the server-side body generator can emit
 * perfectly-formed Cisco-flavoured PIDF and the firmware will still
 * render every monitored button as a plain Speed Dial: the BLF
 * "feature class" is decided at REGISTER time from this options
 * indication, not at SUBSCRIBE/NOTIFY time from body content.
 *
 * Body and behaviour are a direct port of the chan_sip
 * cisco-usecallmanager patch's
 * channels/sip/response.c:276-310 sip_response_send_with_options_ind,
 * which in the patch is invoked from
 * channels/sip/dialog.c:2894 when the peer has cisco_support set and
 * the registration causes an address change.
 *
 * Scope: this is attached only for endpoints with a matching
 * [name] type=cisco sorcery object. Endpoint identity comes from
 * Asterisk's REGISTER response supplement hook, not from the To user,
 * so SIP user/AOR names can differ from endpoint ids.
 */

/*** MODULEINFO
	<depend>res_pjsip</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/sorcery.h"
#include "asterisk/res_pjsip.h"

#include "cisco_endpoint.h"

#define CISCO_REMOTECC_OPTIONSIND_BODY                                  \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                  \
	"<x-cisco-remotecc-response>\n"                                 \
	"  <response>\n"                                                \
	"    <code>200</code>\n"                                        \
	"    <optionsind>\n"                                            \
	"      <combine max=\"6\">\n"                                   \
	"        <remotecc>\n"                                          \
	"          <status />\n"                                        \
	"        </remotecc>\n"                                         \
	"        <service-control />\n"                                 \
	"      </combine>\n"                                            \
	"      <dialog usage=\"hook status\">\n"                        \
	"        <unot />\n"                                            \
	"      </dialog>\n"                                             \
	"      <dialog usage=\"shared line\">\n"                        \
	"        <unot />\n"                                            \
	"      </dialog>\n"                                             \
	"      <presence usage=\"blf speed dial\">\n"                   \
	"        <unot />\n"                                            \
	"        <sub />\n"                                             \
	"      </presence>\n"                                           \
	"      <joinreq />\n"                                           \
	"      <cfwdall-anyline>Yes</cfwdall-anyline>\n"                \
	"    </optionsind>\n"                                           \
	"  </response>\n"                                               \
	"</x-cisco-remotecc-response>\n"

static struct ao2_container *optionsind_addr_cache;

static void cisco_optionsind_outgoing_response(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact, pjsip_tx_data *tdata)
{
	pj_str_t type;
	pj_str_t subtype;
	pj_str_t text;
	const char *endpoint_id;

	if (!cisco_register_should_fire(endpoint, tdata, optionsind_addr_cache,
			&endpoint_id, NULL)) {
		return;
	}

	/* Never overwrite an existing body. NOTE: don't commit the
	 * address-change cache here — if the body wasn't actually
	 * attached we want the next refresh REGISTER to retry, not to
	 * silently skip on the cached "we fired" mark. */
	if (tdata->msg->body) {
		ast_log(LOG_NOTICE, "cisco-optionsind: REGISTER 200 OK already has a "
			"body, not overwriting\n");
		return;
	}

	pj_strset2(&type, "application");
	pj_strset2(&subtype, "x-cisco-remotecc-response+xml");
	pj_strset2(&text, (char *) CISCO_REMOTECC_OPTIONSIND_BODY);

	tdata->msg->body = pjsip_msg_body_create(tdata->pool, &type, &subtype, &text);
	if (!tdata->msg->body) {
		ast_log(LOG_WARNING, "cisco-optionsind: pjsip_msg_body_create failed for "
			"REGISTER 200 OK to '%s'\n", endpoint_id);
		return;
	}

	cisco_register_address_remember(tdata->msg, endpoint_id,
		optionsind_addr_cache);

	ast_log(LOG_NOTICE, "cisco-optionsind: attached optionsind body to "
		"REGISTER 200 OK for '%s'\n", endpoint_id);

	(void) contact;
}

static struct ast_sip_supplement cisco_optionsind_supplement = {
	.method            = "REGISTER",
	.priority          = AST_SIP_SUPPLEMENT_PRIORITY_LAST,
	.outgoing_response = cisco_optionsind_outgoing_response,
};

/* ----------------------------------------------------------------------
 * Cisco USECALLMANAGER failover keep-alive REGISTER handler.
 *
 * Cisco phones in CUCM-style multi-server deployments send a REGISTER
 * with Contact: <...>;expires=0;cisco-keep-alive as a *heartbeat to
 * the primary server* — NOT a deregistration. The chan_sip
 * cisco-usecallmanager patch detects this marker
 * (channels/sip/chan_sip.c line 2531) and just replies 200 OK without
 * touching the registered contact.
 *
 * Without this guard, our normal REGISTER processing sees expires=0,
 * removes the contact, and the phone has to re-register fresh on the
 * next cycle — defeating the failover heartbeat's purpose and
 * causing brief gaps in reachability.
 *
 * Implemented as a pjsip_module at PJSIP_MOD_PRIORITY_APPLICATION - 1
 * (after auth at -2, before the registrar at APPLICATION). Claims
 * (returns PJ_TRUE) on a match so the registrar never sees it.
 * ---------------------------------------------------------------------- */

static int contact_has_keepalive_marker(pjsip_contact_hdr *contact)
{
	static const pj_str_t marker = { "cisco-keep-alive", 16 };

	if (!contact || contact->star || contact->expires != 0) {
		return 0;
	}
	return pjsip_param_find(&contact->other_param, &marker) ? 1 : 0;
}

static pj_bool_t cisco_keepalive_on_rx_request(pjsip_rx_data *rdata)
{
	pjsip_contact_hdr *contact;
	struct ast_sip_endpoint *endpoint;
	pjsip_tx_data *tdata = NULL;
	int found = 0;

	/* Restricted to Cisco endpoints so we don't accidentally swallow
	 * any non-Cisco UA's marker (vanishingly unlikely but cheap). */
	endpoint = cisco_pjsip_module_match(rdata, "REGISTER", NULL);
	if (!endpoint) {
		return PJ_FALSE;
	}

	contact = (pjsip_contact_hdr *) pjsip_msg_find_hdr(rdata->msg_info.msg,
		PJSIP_H_CONTACT, NULL);
	while (contact) {
		if (contact_has_keepalive_marker(contact)) {
			found = 1;
			break;
		}
		contact = (pjsip_contact_hdr *) pjsip_msg_find_hdr(
			rdata->msg_info.msg, PJSIP_H_CONTACT, contact->next);
	}
	if (!found) {
		ao2_cleanup(endpoint);
		return PJ_FALSE;
	}

	if (ast_sip_create_response(rdata, 200, NULL, &tdata) == 0) {
		/* res_pjsip_registrar's normal REGISTER 200 OK includes a
		 * Date header (and the chan_sip patch's keepalive response
		 * uses sip_response_send_with_date for the same reason —
		 * Cisco firmware uses the server Date to drive its TLS
		 * certificate validity window). Add it explicitly since
		 * we're bypassing the registrar. */
		ast_sip_add_date_header(tdata);
		ast_sip_send_stateful_response(rdata, tdata, endpoint);
		ast_log(LOG_NOTICE,
			"cisco-optionsind: replied 200 OK to cisco-keep-alive "
			"REGISTER from %s without disturbing registration\n",
			ast_sorcery_object_get_id(endpoint));
	}

	ao2_cleanup(endpoint);
	return PJ_TRUE;
}

static pjsip_module cisco_keepalive_module = {
	.name             = { "cisco-keepalive-reg", 19 },
	.id               = -1,
	/* APPLICATION-1 = 63, after auth at -2, before res_pjsip_registrar
	 * at APPLICATION. The registrar would otherwise see expires=0 and
	 * remove the contact. */
	.priority         = PJSIP_MOD_PRIORITY_APPLICATION - 1,
	.on_rx_request    = cisco_keepalive_on_rx_request,
};

static int load_module(void)
{
	optionsind_addr_cache = cisco_addr_cache_alloc();
	if (!optionsind_addr_cache) {
		return AST_MODULE_LOAD_DECLINE;
	}
	ast_sip_register_supplement(&cisco_optionsind_supplement);
	if (ast_sip_register_service(&cisco_keepalive_module)) {
		ast_sip_unregister_supplement(&cisco_optionsind_supplement);
		ao2_cleanup(optionsind_addr_cache);
		optionsind_addr_cache = NULL;
		ast_log(LOG_ERROR,
			"cisco-optionsind: failed to register cisco-keep-alive REGISTER module\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	if (!ast_shutdown_final()) {
		return -1;
	}
	ast_sip_unregister_service(&cisco_keepalive_module);
	ast_sip_unregister_supplement(&cisco_optionsind_supplement);
	ao2_cleanup(optionsind_addr_cache);
	optionsind_addr_cache = NULL;
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco RemoteCC OptionsInd in REGISTER 200 OK",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_cisco_endpoint",
);
