/*
 * PATH B: DND PUBLISH presence handler + Authorization-username
 * endpoint identifier, split out from res_pjsip_cisco_feature_events.c.
 *
 * The flow: Cisco firmware on the live fleet (CP7975 / CP8861) emits
 * an `Event: presence` PUBLISH (`application/pidf+xml` carrying a
 * `<ce:dnd/>` activity) on every DND on/off press. The phone's
 * From-URI user is its MAC, not the line id, so stock res_pjsip
 * identifiers can't map the request to an endpoint and the
 * distributor 401s it. cisco_authorization_identify rescues the
 * post-401 retry by matching on the username in the Digest
 * Authorization header — full RFC-clean auth, no bypass — and the
 * PUBLISH handler then writes DND/<endpoint-id> in astdb. Same key
 * res_pjsip_cisco_bulkupdate reads.
 *
 * See the file header essay in res_pjsip_cisco_feature_events.c for
 * the full rationale.
 */

#include "asterisk.h"

#include <pjsip.h>

#include "asterisk/module.h"
#include "asterisk/astobj2.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/astdb.h"
#include "asterisk/xml.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"

#include "cisco/endpoint.h"
#include "cisco/rdata.h"
#include "feature_events_private.h"

#define DND_PUBLISH_MAX_BODY 4096

/*!
 * \brief Endpoint identifier that matches by the username in
 *        Authorization: Digest. Restricted to Cisco endpoints so it
 *        doesn't change identification semantics elsewhere.
 *
 * Stock res_pjsip identifiers match by From URI user, source IP, or
 * fall back to anonymous. None work for Cisco PUBLISH because the
 * From URI user is the phone's MAC. With this identifier registered,
 * the second (post-401) PUBLISH attempt — which carries
 * Authorization: Digest username="1010" — gets identified as endpoint
 * 1010, then res_pjsip's normal digest auth verifies the response
 * hash against 1010's password. Full RFC-clean auth, no bypass.
 */
static struct ast_sip_endpoint *cisco_authorization_identify(pjsip_rx_data *rdata)
{
	pjsip_authorization_hdr *auth_hdr;
	struct ast_sip_endpoint *endpoint;
	struct cisco_endpoint *cisco;
	char username[128];

	if (!rdata || !rdata->msg_info.msg) {
		return NULL;
	}

	auth_hdr = (pjsip_authorization_hdr *) pjsip_msg_find_hdr(
		rdata->msg_info.msg, PJSIP_H_AUTHORIZATION, NULL);
	if (!auth_hdr || pj_stricmp2(&auth_hdr->scheme, "Digest")) {
		return NULL;
	}

	ast_copy_pj_str(username, &auth_hdr->credential.digest.username,
		sizeof(username));
	/* No ast_strlen_zero guard: an empty username falls through to
	 * ast_sorcery_retrieve_by_id(..., "") which cleanly returns NULL. */

	endpoint = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(), "endpoint",
		username);
	if (!endpoint) {
		return NULL;
	}

	/* Only claim Cisco endpoints. Non-Cisco peers stay on whichever
	 * standard identifier matches them. */
	cisco = cisco_endpoint_get(username);
	if (!cisco) {
		ao2_cleanup(endpoint);
		return NULL;
	}
	ao2_cleanup(cisco);

	return endpoint;
}

static struct ast_sip_endpoint_identifier cisco_authorization_identifier = {
	.identify_endpoint = cisco_authorization_identify,
};

/*!
 * \brief Parse a PIDF presence body for DND state and update astdb.
 *
 * Body shape (full example from a Cisco-CP7975G/9.4.2 wire trace):
 *
 *   <presence xmlns="urn:ietf:params:xml:ns:pidf"
 *             xmlns:dm="urn:ietf:params:xml:ns:pidf:data-model"
 *             xmlns:e="urn:ietf:params:xml:ns:pidf:status:rpid"
 *             xmlns:ce="urn:cisco:params:xml:ns:pidf:rpid"
 *             ...>
 *     <tuple id="1"><status><basic>open</basic></status></tuple>
 *     <dm:person><e:activities><ce:dnd/></e:activities></dm:person>
 *   </presence>
 *
 * <ce:dnd/> in activities  -> DND on
 * empty activities         -> DND off
 *
 * libxml2 (via Asterisk's ast_xml_*) matches local element names
 * regardless of namespace prefix, so we just look for "dnd".
 *
 * \retval 1  successfully parsed and applied
 * \retval 0  body not recognised (caller should still 200 OK; PUBLISH
 *            refreshes legitimately have no body)
 */
static int handle_dnd_publish_body(pjsip_rx_data *rdata, const char *endpoint_id)
{
	pjsip_msg_body *body;
	pjsip_ctype_hdr *ctype;
	struct ast_xml_doc *doc;
	struct ast_xml_node *root, *person, *activities;
	int dnd_on = -1;

	if (!rdata || !rdata->msg_info.msg) {
		return 0;
	}
	body = rdata->msg_info.msg->body;

	if (!body || !body->data || body->len == 0) {
		return 0;
	}
	if (body->len > DND_PUBLISH_MAX_BODY) {
		ast_log(LOG_WARNING,
			"cisco-feature-events: rejecting oversized PUBLISH body "
			"(%u bytes) from %s\n", (unsigned) body->len, endpoint_id);
		return 0;
	}

	ctype = (pjsip_ctype_hdr *) pjsip_msg_find_hdr(rdata->msg_info.msg,
		PJSIP_H_CONTENT_TYPE, NULL);
	if (!ctype || pj_stricmp2(&ctype->media.type, "application")
		|| pj_stricmp2(&ctype->media.subtype, "pidf+xml")) {
		return 0;
	}

	doc = cisco_xml_read_body(body);
	if (!doc) {
		return 0;
	}

	root = ast_xml_get_root(doc);
	if (root && !strcasecmp(ast_xml_node_get_name(root), "presence")) {
		person = ast_xml_find_element(ast_xml_node_get_children(root),
			"person", NULL, NULL);
		if (person) {
			activities = ast_xml_find_element(
				ast_xml_node_get_children(person),
				"activities", NULL, NULL);
			if (activities) {
				dnd_on = ast_xml_find_element(
					ast_xml_node_get_children(activities),
					"dnd", NULL, NULL) ? 1 : 0;
			}
		}
	}

	ast_xml_close(doc);

	if (dnd_on == 1) {
		cisco_dnd_set(endpoint_id, 1);
		ast_log(LOG_NOTICE,
			"cisco-feature-events: %s set DND on (from PUBLISH presence)\n",
			endpoint_id);
		return 1;
	}
	if (dnd_on == 0) {
		cisco_dnd_set(endpoint_id, 0);
		ast_log(LOG_NOTICE,
			"cisco-feature-events: %s cleared DND (from PUBLISH presence)\n",
			endpoint_id);
		return 1;
	}
	return 0;
}

static void send_publish_response(pjsip_rx_data *rdata,
	struct ast_sip_endpoint *endpoint)
{
	pjsip_tx_data *tdata = NULL;
	char etag_buf[24];

	if (ast_sip_create_response(rdata, 200, NULL, &tdata)) {
		return;
	}

	/* RFC 3903 requires SIP-ETag on PUBLISH 2xx. We don't actually
	 * track per-publication state — phones just re-publish on state
	 * change — so a fresh random tag per response is fine. */
	snprintf(etag_buf, sizeof(etag_buf), "%08x", (unsigned) ast_random());
	ast_sip_add_header(tdata, "SIP-ETag", etag_buf);

	/* Echo a sensible expiration. Cisco firmware sends INT_MAX
	 * (effectively forever) but a 1h grant keeps the publication
	 * lifecycle bounded. */
	ast_sip_add_header(tdata, "Expires", "3600");

	ast_sip_send_stateful_response(rdata, tdata, endpoint);
}

pj_bool_t cisco_feature_events_dnd_on_rx_request(pjsip_rx_data *rdata)
{
	struct ast_sip_endpoint *endpoint;

	/* By the time we run (priority APPLICATION-1, after auth at
	 * APPLICATION-2), auth has accepted the request and identified the
	 * endpoint, or 401'd and we never see it. cisco_pjsip_module_match
	 * does the standard rdata→endpoint lookup. */
	endpoint = cisco_pjsip_module_match(rdata, "PUBLISH", "presence");
	if (!endpoint) {
		return PJ_FALSE;
	}

	handle_dnd_publish_body(rdata, ast_sorcery_object_get_id(endpoint));
	send_publish_response(rdata, endpoint);

	ao2_cleanup(endpoint);
	return PJ_TRUE;
}

int cisco_feature_events_dnd_init(void)
{
	if (ast_sip_register_endpoint_identifier_with_name(
			&cisco_authorization_identifier, "cisco_auth")) {
		ast_log(LOG_ERROR,
			"cisco-feature-events: failed to register Cisco "
			"Authorization-username endpoint identifier\n");
		return -1;
	}
	return 0;
}

void cisco_feature_events_dnd_shutdown(void)
{
	ast_sip_unregister_endpoint_identifier(&cisco_authorization_identifier);
}
