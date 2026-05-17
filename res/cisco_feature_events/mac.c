/*
 * PATH C: REGISTER-time MAC harvest + endpoint identifier for the
 * MAC-address From-URI Cisco firmware puts on device-level REFER /
 * PUBLISH. Split out from res_pjsip_cisco_feature_events.c.
 *
 * Cisco firmware puts the device MAC (not the line id) in the
 * From-URI user of device-level REFERs — RemoteCC token registration,
 * alarm reports, RemoteCC responses — and sometimes PUBLISH. Stock
 * res_pjsip identifiers can't map sip:aabbccddeeff@phone-ip to an
 * endpoint, so the distributor 401s the request before
 * res_pjsip_cisco_remotecc ever sees it; the Authorization-username
 * identifier in res/cisco_feature_events/dnd.c (PATH B) only rescues the
 * requests the phone actually retries with a usable Authorization
 * username.
 *
 * On every authenticated REGISTER from a Cisco endpoint we harvest
 * the device MAC out of the Contact header parameters
 * (+sip.instance's urn:uuid node, Cisco's
 * +u.sip!devicename.ccm.cisco.com="SEPxxxx", or a bare 12-hex value)
 * and remember MAC -> {endpoint id, source IP, expiry}.
 * cisco_mac_identify then resolves a later request whose From-URI
 * user is one of those MACs back to that endpoint, gated on the
 * request arriving from the same source IP the REGISTER did.
 *
 * See the file header essay in res_pjsip_cisco_feature_events.c for
 * the full rationale.
 */

#include "asterisk.h"

#include <ctype.h>

#include <pjsip.h>

#include "asterisk/module.h"
#include "asterisk/astobj2.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/time.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"

#include "cisco/endpoint.h"
#include "cisco/rdata.h"
#include "feature_events_private.h"

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
void cisco_feature_events_mac_harvest_on_rx_request(pjsip_rx_data *rdata)
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
		return;
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
		return;
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
		return;
	}
	if (ttl > 86400) {
		ttl = 86400;
	}

	entry = ao2_alloc_options(sizeof(*entry), NULL, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!entry) {
		ao2_cleanup(endpoint);
		return;
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
}

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

int cisco_feature_events_mac_init(void)
{
	cisco_mac_map = ao2_container_alloc_hash(AO2_ALLOC_OPT_LOCK_MUTEX, 0, 13,
		cisco_mac_hash_fn, NULL, cisco_mac_cmp_fn);
	if (!cisco_mac_map) {
		ast_log(LOG_ERROR,
			"cisco-feature-events: failed to allocate MAC -> endpoint map\n");
		return -1;
	}
	if (ast_sip_register_endpoint_identifier_with_name(
			&cisco_mac_identifier, "cisco_mac")) {
		ao2_cleanup(cisco_mac_map);
		cisco_mac_map = NULL;
		ast_log(LOG_ERROR,
			"cisco-feature-events: failed to register Cisco MAC-address "
			"endpoint identifier\n");
		return -1;
	}
	return 0;
}

void cisco_feature_events_mac_shutdown(void)
{
	ast_sip_unregister_endpoint_identifier(&cisco_mac_identifier);
	ao2_cleanup(cisco_mac_map);
	cisco_mac_map = NULL;
}
