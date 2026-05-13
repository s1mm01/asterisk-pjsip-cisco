/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_feature_events
 *
 * Closes the phone -> server side of the Cisco DND / call-forward
 * softkey loop, and resolves the MAC-address From-URI that Cisco
 * firmware uses on device-level REFER/PUBLISH. Handles both DND
 * signaling paths Cisco Enterprise SIP firmware uses, depending on the
 * phone's <dndControl> SEP setting:
 *
 *   PATH A: SUBSCRIBE Event: as-feature-event (server-controlled DND,
 *   <dndControl>1</dndControl>). Body application/x-as-feature-event+xml:
 *
 *     <SetDoNotDisturb xmlns="...">
 *       <doNotDisturbOn>true|false</doNotDisturbOn>
 *     </SetDoNotDisturb>
 *
 *     <SetForwarding xmlns="...">
 *       <forwardingType>forwardImmediate</forwardingType>
 *       <activateForward>true|false</activateForward>
 *       <forwardDN>...</forwardDN>     <!-- target extension -->
 *     </SetForwarding>
 *
 *   We intercept via a pjsip_module on_rx_request hook at priority
 *   APPLICATION-1 (after res_pjsip's auth, before res_pjsip_pubsub).
 *
 *   PATH B: PUBLISH Event: presence (default DND mode on most modern
 *   firmware, <dndControl>0</dndControl>). Body application/pidf+xml:
 *
 *     <presence ...>
 *       <dm:person><e:activities><ce:dnd/></e:activities></dm:person>
 *     </presence>
 *
 *   This path is structurally tricky because Cisco firmware uses its
 *   MAC address as the From-URI user (e.g. sip:aabbccddeeff@server)
 *   instead of the endpoint id, so res_pjsip's stock endpoint
 *   identifiers (user, ip, anonymous) don't match the request to any
 *   endpoint, the distributor has no endpoint to hand to the auth
 *   module, and the request is 401'd. The chan_sip
 *   cisco-usecallmanager patch worked around this by skipping auth
 *   entirely for PUBLISH from Cisco peers (chan_sip.c:10053:
 *   "Buggy Cisco phones can't auth REFER or PUBLISH correctly").
 *
 *   We do better than the bypass: register a new endpoint identifier
 *   (cisco_authorization_identify) that matches incoming requests by
 *   the username in their Authorization: Digest header. The phone's
 *   first PUBLISH attempt arrives without Authorization, gets 401,
 *   the phone retries with Authorization: Digest username="1010" — at
 *   which point our identifier matches endpoint 1010, res_pjsip's
 *   normal digest auth verifies the response hash against the
 *   endpoint's password, and the request proceeds with full auth.
 *
 *   Our identifier only matches endpoints that have a [name]
 *   type=cisco sorcery section so it doesn't change identification
 *   semantics for non-Cisco endpoints.
 *
 *   The PUBLISH-presence handler then runs at priority APPLICATION-1
 *   (after auth has succeeded), pulls the now-identified endpoint
 *   via ast_pjsip_rdata_get_endpoint, parses the PIDF body for
 *   <ce:dnd/> vs empty activities, writes DND/<endpoint-id> to
 *   astdb, and replies 200 OK with SIP-ETag + Expires.
 *
 * Both paths write to the same astdb keys:
 *   DND/<endpoint-id>  = "YES" / (deleted)
 *   CF/<endpoint-id>   = <target> / (deleted)
 *
 * Same convention as res_pjsip_cisco_bulkupdate, so a server-side
 * toggle (database put DND 1010 YES) and a phone-side softkey press
 * now write to the same store regardless of which signaling path the
 * firmware uses.
 *
 *   PATH C: MAC-address From-URI identification. Cisco firmware puts
 *   the device MAC (not the line id) in the From-URI user of
 *   device-level REFERs — RemoteCC token registration, alarm reports,
 *   RemoteCC responses — and sometimes PUBLISH. Stock res_pjsip
 *   endpoint identifiers (user / ip / anonymous) can't map
 *   sip:aabbccddeeff@phone-ip to an endpoint, so the distributor logs
 *   "No matching endpoint found" and 401s the request before
 *   res_pjsip_cisco_remotecc ever sees it; cisco_authorization_identify
 *   (PATH B) only rescues the requests the phone actually retries with
 *   a usable Authorization username. So on every authenticated REGISTER
 *   from a Cisco endpoint we harvest the device MAC out of the Contact
 *   header parameters (+sip.instance's urn:uuid node, Cisco's
 *   +u.sip!devicename.ccm.cisco.com="SEPxxxx", or a bare 12-hex value)
 *   and remember MAC -> {endpoint id, source IP, expiry}.
 *   cisco_mac_identify then resolves a later request whose From-URI
 *   user is one of those MACs back to that endpoint, gated on the
 *   request arriving from the same source IP the REGISTER did.
 *
 *   This only gets the request *identified*. Whether it survives auth
 *   afterward depends on what the phone sends after the 401 — the
 *   chan_sip patch gave up here entirely ("Buggy Cisco phones can't
 *   auth REFER or PUBLISH correctly", chan_sip.c:10053); skipping the
 *   re-challenge for a MAC-identified Cisco REFER would be the next
 *   step if these phones turn out not to re-auth.
 *
 * NOT YET IMPLEMENTED (deferred):
 *   - Server -> phone push when astdb changes via dialplan toggle.
 *     Would require holding the SUBSCRIBE dialog open and pushing a
 *     NOTIFY into it whenever the astdb key changes. The chan_sip
 *     patch's peer->feature_events_dialog branch is the reference.
 *   - PATH B call-forward (the SetForwarding equivalent in PUBLISH).
 *     Cisco firmware appears to use as-feature-event for CFwdALL
 *     even when DND is on PUBLISH, so PATH A handles CF in practice;
 *     revisit if we see a fleet that puts CF on PUBLISH too.
 *
 * Spec for PATH A: chan_sip patch's
 * channels/sip/handlers.c:1444+ (SUBSCRIBE dispatch) and
 * channels/sip/handlers.c:2315+ (sip_handle_subscribe_feature_event).
 * Spec for PATH B: chan_sip patch's
 * channels/sip/handlers.c:13370+ sip_handle_publish_presence and the
 * auth-bypass at chan_sip.c:10053 (which we deliberately do NOT
 * replicate — the Authorization-identifier approach is stronger).
 */

/*** MODULEINFO
	<depend>res_pjsip</depend>
	<depend>libxml2</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <ctype.h>

#include <pjsip.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/astdb.h"
#include "asterisk/astobj2.h"
#include "asterisk/time.h"
#include "asterisk/xml.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"

#include "cisco_endpoint.h"

#define FEATURE_EVENT_MAX_BODY 8192
#define DND_PUBLISH_MAX_BODY 4096

/*!
 * \brief Parse the SUBSCRIBE body, update astdb if it's a recognised
 *        feature-event request.
 *
 * \retval 1 we handled it (caller should send 200 OK + return PJ_TRUE)
 * \retval 0 not for us (caller should ignore, return PJ_FALSE)
 */
static int handle_feature_event_body(pjsip_rx_data *rdata, const char *endpoint_id)
{
	pjsip_msg_body *body = rdata->msg_info.msg->body;
	pjsip_ctype_hdr *ctype;
	struct ast_xml_doc *doc;
	struct ast_xml_node *root;
	const char *root_name;

	if (!body || !body->data || body->len == 0) {
		/* Empty SUBSCRIBE = "tell me current state" (bulk-update
		 * request). We don't push state back at the moment; just
		 * accept the subscription quietly. */
		return 1;
	}

	ctype = (pjsip_ctype_hdr *) pjsip_msg_find_hdr(rdata->msg_info.msg,
		PJSIP_H_CONTENT_TYPE, NULL);
	if (!ctype
		|| pj_stricmp2(&ctype->media.type, "application")
		|| pj_stricmp2(&ctype->media.subtype, "x-as-feature-event+xml")) {
		ast_debug(2, "cisco-feature-events: unknown Content-Type, skipping\n");
		return 0;
	}

	if (body->len > FEATURE_EVENT_MAX_BODY) {
		ast_log(LOG_WARNING,
			"cisco-feature-events: rejecting oversized feature-event body (%u bytes)\n",
			(unsigned) body->len);
		return 1;
	}

	doc = cisco_xml_read_body(body);
	if (!doc) {
		ast_debug(2, "cisco-feature-events: XML parse failed\n");
		return 0;
	}

	root = ast_xml_get_root(doc);
	if (!root) {
		ast_xml_close(doc);
		return 0;
	}
	root_name = ast_xml_node_get_name(root);

	if (!strcmp(root_name, "SetDoNotDisturb")) {
		struct ast_xml_node *child;
		const char *value;

		child = ast_xml_find_element(ast_xml_node_get_children(root), "doNotDisturbOn", NULL, NULL);
		value = child ? ast_xml_get_text(child) : NULL;
		if (value) {
			if (!strcmp(value, "true")) {
				cisco_dnd_set(endpoint_id, 1);
				ast_log(LOG_NOTICE,
					"cisco-feature-events: %s set DND on (from softkey)\n",
					endpoint_id);
			} else if (!strcmp(value, "false")) {
				cisco_dnd_set(endpoint_id, 0);
				ast_log(LOG_NOTICE,
					"cisco-feature-events: %s set DND off (from softkey)\n",
					endpoint_id);
			}
			ast_xml_free_text(value);
		}
	} else if (!strcmp(root_name, "SetForwarding")) {
		struct ast_xml_node *act_node, *dn_node;
		const char *activate;
		const char *target;

		act_node = ast_xml_find_element(ast_xml_node_get_children(root), "activateForward", NULL, NULL);
		activate = act_node ? ast_xml_get_text(act_node) : NULL;
		if (activate && !strcmp(activate, "true")) {
			dn_node = ast_xml_find_element(ast_xml_node_get_children(root), "forwardDN", NULL, NULL);
			target = dn_node ? ast_xml_get_text(dn_node) : NULL;
			if (!ast_strlen_zero(target)) {
				cisco_cfwd_set(endpoint_id, target);
				ast_log(LOG_NOTICE,
					"cisco-feature-events: %s set call-forward to %s "
					"(from softkey)\n", endpoint_id, target);
			}
			if (target) {
				ast_xml_free_text(target);
			}
		} else {
			cisco_cfwd_set(endpoint_id, NULL);
			ast_log(LOG_NOTICE,
				"cisco-feature-events: %s cleared call-forward (from softkey)\n",
				endpoint_id);
		}
		if (activate) {
			ast_xml_free_text(activate);
		}
	} else {
		ast_debug(2, "cisco-feature-events: unrecognised root element <%s>\n",
			root_name);
		ast_xml_close(doc);
		return 0;
	}

	ast_xml_close(doc);
	return 1;
}

/*!
 * \brief Send a minimal 200 OK to a SUBSCRIBE we've handled.
 *
 * No body, Subscription-State: terminated;reason=noresource so the
 * phone doesn't expect us to push NOTIFYs into the dialog. The
 * softkey-driven SUBSCRIBE is one-shot from the firmware's POV.
 */
static void send_subscribe_response(pjsip_rx_data *rdata,
	struct ast_sip_endpoint *endpoint)
{
	pjsip_tx_data *tdata;

	if (ast_sip_create_response(rdata, 200, NULL, &tdata)) {
		return;
	}

	ast_sip_add_header(tdata, "Subscription-State", "terminated;reason=noresource");

	ast_sip_send_stateful_response(rdata, tdata, endpoint);
}

static pj_bool_t feature_events_on_rx_request(pjsip_rx_data *rdata)
{
	struct ast_sip_endpoint *endpoint;
	int handled;

	endpoint = cisco_pjsip_module_match(rdata, "SUBSCRIBE", "as-feature-event");
	if (!endpoint) {
		return PJ_FALSE;
	}

	handled = handle_feature_event_body(rdata,
		ast_sorcery_object_get_id(endpoint));
	if (handled) {
		send_subscribe_response(rdata, endpoint);
	}

	ao2_cleanup(endpoint);
	return handled ? PJ_TRUE : PJ_FALSE;
}

/* ----------------------------------------------------------------------
 * PATH B: PUBLISH presence handler with Authorization-username
 * endpoint identification.  See file header comment for the design.
 * ---------------------------------------------------------------------- */

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

static pj_bool_t publish_dnd_on_rx_request(pjsip_rx_data *rdata)
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

/* ----------------------------------------------------------------------
 * Cisco SUBSCRIBE Expires clamp.
 *
 * Cisco Enterprise SIP firmware (verified against CP-7975G/9.4.2)
 * sends Expires: 2147483647 (INT_MAX seconds, ≈ 68 years) on its
 * BLF presence SUBSCRIBEs — effectively asking for a permanent
 * subscription. PJSIP's res_pjsip_pubsub then fails to create a
 * scheduler expiration timer because the value overflows the
 * scheduler's internal representation:
 *
 *   ERROR res_pjsip_pubsub.c: Unable to create expiration timer of
 *   2147357964 seconds for 1010->1006/presence ...
 *
 * Subscriptions are still ACCEPTED but the timer-driven NOTIFY-on-
 * state-change path is broken, so mid-call BLF state propagation
 * never reaches the phone. The unsolicited NOTIFYs we send at
 * REGISTER time still work (refreshing state on every re-REGISTER),
 * but real-time state changes don't.
 *
 * Fix: intercept incoming SUBSCRIBEs from Cisco endpoints before
 * pubsub processes them, clamp the Expires header to a sane value
 * (2 hours, so the phone refreshes its subscription periodically
 * but the scheduler timer is well within range). Pubsub then sees
 * the clamped value, creates a normal timer, and the in-dialog
 * NOTIFY path works.
 * ---------------------------------------------------------------------- */

/* 2h subscription lifetime. Phone will refresh well before expiry,
 * just like any other SIP UA. Long enough that re-SUBSCRIBE traffic
 * is negligible, short enough that pjsip's scheduler is happy. */
#define CISCO_MAX_SUBSCRIBE_EXPIRES 7200

static pj_bool_t subscribe_expires_clamp_on_rx_request(pjsip_rx_data *rdata)
{
	struct ast_sip_endpoint *endpoint;
	pjsip_expires_hdr *expires_hdr;

	/* Only clamp for Cisco endpoints — don't change subscription
	 * lifecycle for unrelated peers that happen to ask for long
	 * expiries. No Event-header filter: clamp applies to any
	 * Cisco SUBSCRIBE that asks for INT_MAX. */
	endpoint = cisco_pjsip_module_match(rdata, "SUBSCRIBE", NULL);
	if (!endpoint) {
		return PJ_FALSE;
	}

	expires_hdr = (pjsip_expires_hdr *) pjsip_msg_find_hdr(
		rdata->msg_info.msg, PJSIP_H_EXPIRES, NULL);
	if (!expires_hdr || expires_hdr->ivalue <= CISCO_MAX_SUBSCRIBE_EXPIRES) {
		ao2_cleanup(endpoint);
		return PJ_FALSE;
	}

	ast_debug(2,
		"cisco-feature-events: clamping SUBSCRIBE Expires from %d to %d "
		"for endpoint %s (firmware INT_MAX would overflow pubsub timer)\n",
		(int) expires_hdr->ivalue, CISCO_MAX_SUBSCRIBE_EXPIRES,
		ast_sorcery_object_get_id(endpoint));

	expires_hdr->ivalue = CISCO_MAX_SUBSCRIBE_EXPIRES;

	ao2_cleanup(endpoint);

	/* Don't claim — we just modified the header in-place; let pubsub
	 * (and any other pjsip_modules at our priority or higher) handle
	 * it normally. */
	return PJ_FALSE;
}

/* ----------------------------------------------------------------------
 * PATH C: REGISTER-time MAC harvest + endpoint identifier for the
 * MAC-address From-URI Cisco firmware puts on device-level REFER /
 * PUBLISH. See the file header essay for the full rationale.
 * ---------------------------------------------------------------------- */

#define CISCO_MAC_LEN 12

/* MAC -> endpoint hint, learned from authenticated REGISTERs. Purely a
 * lookup aid for the distributor; rebuilt on the next REGISTER, so a
 * stale or missing entry just costs one failed identification. Entries
 * are immutable once linked (a re-REGISTER replaces rather than mutates),
 * so readers need no per-entry lock. */
struct cisco_mac_entry {
	struct timeval expires;          /* when this hint goes stale */
	char src_host[64];               /* source IP the REGISTER came from */
	char endpoint_id[128];           /* the cisco endpoint that REGISTERed */
	char mac[CISCO_MAC_LEN + 1];     /* 12 lowercase hex digits, NUL-term */
};

static struct ao2_container *cisco_mac_map;

static int cisco_mac_hash_fn(const void *obj, int flags)
{
	const struct cisco_mac_entry *entry = obj;
	const char *key;

	switch (flags & OBJ_SEARCH_MASK) {
	case OBJ_SEARCH_KEY:
		key = obj;
		break;
	case OBJ_SEARCH_OBJECT:
		key = entry->mac;
		break;
	default:
		ast_assert(0);
		return 0;
	}
	return ast_str_hash(key);
}

static int cisco_mac_cmp_fn(void *obj, void *arg, int flags)
{
	const struct cisco_mac_entry *left = obj;
	const char *right_key = arg;

	switch (flags & OBJ_SEARCH_MASK) {
	case OBJ_SEARCH_OBJECT:
		right_key = ((struct cisco_mac_entry *) arg)->mac;
		/* Fall through */
	case OBJ_SEARCH_KEY:
		if (strcmp(left->mac, right_key)) {
			return 0;
		}
		break;
	case OBJ_SEARCH_PARTIAL_KEY:
		if (strncmp(left->mac, right_key, strlen(right_key))) {
			return 0;
		}
		break;
	default:
		return 0;
	}
	return CMP_MATCH | CMP_STOP;
}

/* Copy a 12-hex-digit lowercase MAC out of \a in into \a out (caller
 * buffer >= CISCO_MAC_LEN + 1). Succeeds only when \a in is exactly 12
 * hex digits, so this never claims a request whose user part is an
 * ordinary line id or other alphanumeric string. */
static int cisco_mac_normalize(const char *in, char *out)
{
	int i;

	if (!in || strlen(in) != CISCO_MAC_LEN) {
		return -1;
	}
	for (i = 0; i < CISCO_MAC_LEN; i++) {
		if (!isxdigit((unsigned char) in[i])) {
			return -1;
		}
		out[i] = tolower((unsigned char) in[i]);
	}
	out[CISCO_MAC_LEN] = '\0';
	return 0;
}

/* Pull a device MAC out of one Contact header parameter value.
 * Recognises (after stripping one layer of surrounding double-quotes):
 *   <urn:uuid:00000000-0000-0000-0000-aabbccddeeff>  (+sip.instance node,
 *                                                     or a GRUU gr= value)
 *   SEPAABBCCDDEEFF                                   (+u.sip!devicename...)
 *   aabbccddeeff                                      (bare 12-hex)
 * Fills \a out (>= CISCO_MAC_LEN + 1) and returns 0 on a match. */
static int cisco_mac_from_param_value(const pj_str_t *pjval, char *out)
{
	char buf[128];
	char *v, *uuid, *dash, *gt;
	size_t len;

	if (!pjval || pjval->slen <= 0) {
		return -1;
	}
	ast_copy_pj_str(buf, pjval, sizeof(buf));
	v = buf;

	len = strlen(v);
	if (len >= 2 && v[0] == '"' && v[len - 1] == '"') {
		v[len - 1] = '\0';
		v++;
	}

	uuid = strstr(v, "urn:uuid:");
	if (uuid) {
		uuid += 9;                       /* past "urn:uuid:" */
		gt = strchr(uuid, '>');
		if (gt) {
			*gt = '\0';
		}
		dash = strrchr(uuid, '-');
		return cisco_mac_normalize(dash ? dash + 1 : uuid, out);
	}
	if (!strncasecmp(v, "SEP", 3)) {
		return cisco_mac_normalize(v + 3, out);
	}
	return cisco_mac_normalize(v, out);
}

/* On every authenticated REGISTER from a Cisco endpoint, learn (or
 * refresh) the device MAC -> endpoint hint. expires=0 / Contact: * is a
 * de-registration: forget any hint for that MAC. Never claims the
 * request — the registrar still does its job. */
static pj_bool_t cisco_mac_harvest_on_rx_request(pjsip_rx_data *rdata)
{
	struct ast_sip_endpoint *endpoint;
	const char *endpoint_id;
	pjsip_msg *msg;
	pjsip_contact_hdr *contact;
	pjsip_expires_hdr *expires_hdr;
	pjsip_param *param;
	void *iter;
	char mac[CISCO_MAC_LEN + 1];
	int have_mac = 0;
	long ttl = -1;
	struct cisco_mac_entry *entry;

	endpoint = cisco_pjsip_module_match(rdata, "REGISTER", NULL);
	if (!endpoint) {
		return PJ_FALSE;
	}
	endpoint_id = ast_sorcery_object_get_id(endpoint);
	msg = rdata->msg_info.msg;

	/* First MAC found in any Contact's header params wins; track the
	 * longest Contact expiry along the way, and treat Contact: * as a
	 * full de-registration. */
	iter = NULL;
	while ((contact = (pjsip_contact_hdr *) pjsip_msg_find_hdr(msg,
			PJSIP_H_CONTACT, iter))) {
		iter = contact->next;
		if (contact->star) {
			ttl = 0;
			break;
		}
		if (contact->expires != PJSIP_EXPIRES_NOT_SPECIFIED
			&& (long) contact->expires > ttl) {
			ttl = (long) contact->expires;
		}
		if (have_mac || !contact->uri) {
			continue;
		}
		for (param = contact->other_param.next;
				param != &contact->other_param; param = param->next) {
			if (!cisco_mac_from_param_value(&param->value, mac)) {
				have_mac = 1;
				break;
			}
		}
	}

	if (!have_mac) {
		ao2_cleanup(endpoint);
		return PJ_FALSE;
	}

	if (ttl < 0) {
		expires_hdr = (pjsip_expires_hdr *) pjsip_msg_find_hdr(msg,
			PJSIP_H_EXPIRES, NULL);
		ttl = expires_hdr ? expires_hdr->ivalue : 3600;
	}
	if (ttl <= 0) {
		ao2_find(cisco_mac_map, mac, OBJ_SEARCH_KEY | OBJ_UNLINK | OBJ_NODATA);
		ast_debug(2, "cisco-mac-identify: forgot MAC %s "
			"(de-registration from endpoint '%s')\n", mac, endpoint_id);
		ao2_cleanup(endpoint);
		return PJ_FALSE;
	}
	if (ttl > 86400) {
		ttl = 86400;
	}

	entry = ao2_alloc_options(sizeof(*entry), NULL, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!entry) {
		ao2_cleanup(endpoint);
		return PJ_FALSE;
	}
	ast_copy_string(entry->mac, mac, sizeof(entry->mac));
	ast_copy_string(entry->endpoint_id, endpoint_id, sizeof(entry->endpoint_id));
	ast_copy_string(entry->src_host, rdata->pkt_info.src_name,
		sizeof(entry->src_host));
	entry->expires = ast_tvnow();
	entry->expires.tv_sec += ttl + 60;     /* small grace past the registration */

	/* Replace any prior hint for this MAC (re-REGISTER, possibly from a
	 * new address or under a different endpoint id). */
	ao2_find(cisco_mac_map, mac, OBJ_SEARCH_KEY | OBJ_UNLINK | OBJ_NODATA);
	ao2_link(cisco_mac_map, entry);
	ast_debug(2, "cisco-mac-identify: learned MAC %s -> endpoint '%s' "
		"from %s (ttl %lds)\n", mac, endpoint_id, entry->src_host, ttl);

	ao2_ref(entry, -1);
	ao2_cleanup(endpoint);
	return PJ_FALSE;
}

/* All four phone→server hooks above run at the same pjsip priority
 * (APPLICATION-1: after res_pjsip's auth at APPLICATION-2, before
 * res_pjsip_pubsub / res_pjsip_registrar at APPLICATION) and only ever
 * inspect inbound requests, so they share a single pjsip_module slot
 * rather than burning four against pjproject's PJSIP_MAX_MODULE cap.
 * Each sub-handler self-filters by SIP method (via cisco_pjsip_module_match
 * / its own method check) and is a no-op for anything that isn't its
 * request type, so calling them in sequence reproduces the previous
 * four-separate-modules-at-equal-priority behaviour exactly:
 *
 *   feature_events  (SUBSCRIBE Event: as-feature-event)  — may consume
 *   publish_dnd     (PUBLISH   Event: presence)          — may consume
 *   subscribe_clamp (SUBSCRIBE, any Event)               — mutates only
 *   mac_harvest     (REGISTER)                           — observes only
 *
 * feature_events must run before subscribe_clamp so a consumed
 * feature-event SUBSCRIBE (which we answer 200 terminated, so pubsub
 * never sees it) isn't pointlessly clamped. */
static pj_bool_t cisco_feature_events_on_rx_request(pjsip_rx_data *rdata)
{
	if (feature_events_on_rx_request(rdata)) {
		return PJ_TRUE;
	}
	if (publish_dnd_on_rx_request(rdata)) {
		return PJ_TRUE;
	}
	subscribe_expires_clamp_on_rx_request(rdata);
	cisco_mac_harvest_on_rx_request(rdata);
	return PJ_FALSE;
}

static pjsip_module cisco_feature_events_module = {
	.name             = { "cisco-feature-events", 20 },
	.id               = -1,
	.priority         = PJSIP_MOD_PRIORITY_APPLICATION - 1,
	.on_rx_request    = cisco_feature_events_on_rx_request,
};

/* Resolve a request whose From-URI user is a device MAC we learned at
 * REGISTER time, gated on the request arriving from the same source IP
 * the REGISTER did and on the endpoint still being a Cisco endpoint.
 * Only ever claims MAC-shaped user parts, so the stock identifiers keep
 * handling everything else unchanged. */
static struct ast_sip_endpoint *cisco_mac_identify(pjsip_rx_data *rdata)
{
	pjsip_fromto_hdr *from;
	pjsip_sip_uri *from_uri;
	char user[64];
	char mac[CISCO_MAC_LEN + 1];
	struct cisco_mac_entry *entry;
	struct ast_sip_endpoint *endpoint;
	struct cisco_endpoint *cisco;

	if (!cisco_mac_map || !rdata || !rdata->msg_info.msg
		|| rdata->msg_info.msg->type != PJSIP_REQUEST_MSG) {
		return NULL;
	}
	from = rdata->msg_info.from;
	if (!from || !from->uri
		|| (!PJSIP_URI_SCHEME_IS_SIP(from->uri)
			&& !PJSIP_URI_SCHEME_IS_SIPS(from->uri))) {
		return NULL;
	}
	from_uri = pjsip_uri_get_uri(from->uri);
	if (from_uri->user.slen <= 0) {
		return NULL;
	}
	ast_copy_pj_str(user, &from_uri->user, sizeof(user));
	if (cisco_mac_normalize(user, mac)) {
		return NULL;       /* not a 12-hex MAC URI — nothing of ours */
	}

	entry = ao2_find(cisco_mac_map, mac, OBJ_SEARCH_KEY);
	if (!entry) {
		return NULL;
	}
	if (ast_tvdiff_ms(entry->expires, ast_tvnow()) <= 0) {
		ao2_unlink(cisco_mac_map, entry);
		ao2_ref(entry, -1);
		return NULL;
	}
	if (strcmp(entry->src_host, rdata->pkt_info.src_name)) {
		ast_debug(2, "cisco-mac-identify: MAC %s learned from %s but request "
			"arrived from %s — not matching\n",
			mac, entry->src_host, rdata->pkt_info.src_name);
		ao2_ref(entry, -1);
		return NULL;
	}

	endpoint = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(), "endpoint",
		entry->endpoint_id);
	if (!endpoint) {
		ao2_ref(entry, -1);
		return NULL;
	}
	cisco = cisco_endpoint_get(entry->endpoint_id);
	if (!cisco) {
		ao2_cleanup(endpoint);
		ao2_ref(entry, -1);
		return NULL;
	}
	ao2_cleanup(cisco);

	ast_debug(2, "cisco-mac-identify: %.*s from MAC %s identified as "
		"endpoint '%s'\n",
		(int) rdata->msg_info.msg->line.req.method.name.slen,
		rdata->msg_info.msg->line.req.method.name.ptr,
		mac, entry->endpoint_id);
	ao2_ref(entry, -1);
	return endpoint;
}

static struct ast_sip_endpoint_identifier cisco_mac_identifier = {
	.identify_endpoint = cisco_mac_identify,
};

static int load_module(void)
{
	/* Bring everything the request dispatcher depends on up first — the
	 * MAC map and both endpoint identifiers — and register the pjsip
	 * module last. Once cisco_feature_events_module is live an inbound
	 * REGISTER reaches cisco_mac_harvest_on_rx_request, which touches
	 * cisco_mac_map unconditionally; a request whose From-URI is a
	 * device MAC reaches cisco_mac_identify. Tear down in reverse on
	 * failure. */
	cisco_mac_map = ao2_container_alloc_hash(AO2_ALLOC_OPT_LOCK_MUTEX, 0, 13,
		cisco_mac_hash_fn, NULL, cisco_mac_cmp_fn);
	if (!cisco_mac_map) {
		ast_log(LOG_ERROR,
			"cisco-feature-events: failed to allocate MAC -> endpoint map\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	if (ast_sip_register_endpoint_identifier_with_name(
			&cisco_authorization_identifier, "cisco_auth")) {
		ao2_cleanup(cisco_mac_map);
		cisco_mac_map = NULL;
		ast_log(LOG_ERROR,
			"cisco-feature-events: failed to register Cisco "
			"Authorization-username endpoint identifier\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	if (ast_sip_register_endpoint_identifier_with_name(
			&cisco_mac_identifier, "cisco_mac")) {
		ast_sip_unregister_endpoint_identifier(&cisco_authorization_identifier);
		ao2_cleanup(cisco_mac_map);
		cisco_mac_map = NULL;
		ast_log(LOG_ERROR,
			"cisco-feature-events: failed to register Cisco MAC-address "
			"endpoint identifier\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	if (ast_sip_register_service(&cisco_feature_events_module)) {
		ast_sip_unregister_endpoint_identifier(&cisco_mac_identifier);
		ast_sip_unregister_endpoint_identifier(&cisco_authorization_identifier);
		ao2_cleanup(cisco_mac_map);
		cisco_mac_map = NULL;
		ast_log(LOG_ERROR,
			"cisco-feature-events: failed to register pjsip module\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	/* See res_pjsip_cisco_bulkupdate.c for why we refuse runtime
	 * unload. PJSIP module + endpoint-identifier unregistration races
	 * with in-flight SUBSCRIBE / PUBLISH traffic. Reverse of load_module:
	 * stop the request dispatcher first, then the identifiers, then free
	 * the map. */
	if (!ast_shutdown_final()) {
		return -1;
	}
	ast_sip_unregister_service(&cisco_feature_events_module);
	ast_sip_unregister_endpoint_identifier(&cisco_mac_identifier);
	ast_sip_unregister_endpoint_identifier(&cisco_authorization_identifier);
	ao2_cleanup(cisco_mac_map);
	cisco_mac_map = NULL;
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco as-feature-event SUBSCRIBE handler (DND / call-forward softkey)",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_cisco_endpoint",
);
