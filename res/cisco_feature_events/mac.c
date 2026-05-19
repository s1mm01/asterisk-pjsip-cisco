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
 * identifier in cisco_feature_events_dnd.c (PATH B) only rescues the
 * requests the phone actually retries with a usable Authorization
 * username.
 *
 * On every authenticated REGISTER from a Cisco endpoint we harvest
 * the device MAC out of the Contact header parameters
 * (+sip.instance's urn:uuid node, Cisco's
 * +u.sip!devicename.ccm.cisco.com="SEPxxxxxxxxxxxx", or a bare 12-hex
 * value) AND the device name + firmware versions out of the Reason
 * header (when the phone is configured for chan_sip-style
 * cisco-usecallmanager Reason reporting, the firmware sends e.g.:
 *
 *   Reason: SIP;cause=200;text="Name=SEPAABBCCDDEEFF
 *                                ActiveLoad=sip78xx.14-1-1-0123
 *                                InactiveLoad=sip78xx.13-1-0-1234"
 *
 * — single-line in the wire format; broken here for readability). All
 * of these facts go into the shared cisco_mac_info container that lives
 * in res_pjsip_cisco_endpoint.so (see include/cisco/endpoint.h), so
 * cisco_mac_identify (this file, PATH C) and 'pjsip cisco status' both
 * see the same data.
 *
 * cisco_mac_identify then resolves a later request whose From-URI user
 * is one of those MACs back to that endpoint, gated on the request
 * arriving from the same source IP the REGISTER did.
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

/* Parse one "Name=foo" / "ActiveLoad=foo" / etc. token out of \a src.
 * Token format: KEY=VALUE separated from siblings by whitespace; VALUE
 * may have a ".loads" suffix that the chan_sip patch strips (firmware
 * versions in the wire form are e.g. "sip78xx.14-1-1-0123.loads"; the
 * canonical CLI form is the ".loads"-less prefix).
 *
 * Returns 1 if \a key was found and \a out filled, 0 otherwise. \a out
 * is sized to hold the chan_sip patch's largest field
 * (cisco_inactiveload is the longest in practice). */
static int reason_extract(const char *src, const char *key,
	char *out, size_t outlen)
{
	const char *hit;
	const char *end;
	size_t klen = strlen(key);
	char buf[128];
	char *suffix;

	if (!src || !*src) {
		out[0] = '\0';
		return 0;
	}
	/* Match " <key>=" so e.g. "ActiveLoad=" doesn't pick up the tail
	 * of a longer key. The space gate also stops false matches inside
	 * quoted-string content; the Reason text= field is itself a quoted
	 * string in the wire format but our caller strips the quotes. */
	{
		char needle[64];

		snprintf(needle, sizeof(needle), " %s=", key);
		hit = strstr(src, needle);
		if (!hit) {
			/* Also try at the very start (no leading space). */
			if (!strncmp(src, needle + 1, klen + 1)) {
				hit = src - 1;            /* point one char before the key */
			} else {
				out[0] = '\0';
				return 0;
			}
		}
	}
	hit += 1 + klen + 1;   /* past " <key>=" */

	end = strpbrk(hit, " \t\"");
	if (!end) {
		end = hit + strlen(hit);
	}
	if ((size_t) (end - hit) >= sizeof(buf)) {
		out[0] = '\0';
		return 0;        /* pathologically long; ignore */
	}
	memcpy(buf, hit, end - hit);
	buf[end - hit] = '\0';

	/* Trim trailing ".loads" suffix on firmware-version fields. */
	suffix = strstr(buf, ".loads");
	if (suffix) {
		*suffix = '\0';
	}

	ast_copy_string(out, buf, outlen);
	return out[0] != '\0';
}

/* Parse the REGISTER's Reason header (text= field, when it follows the
 * chan_sip patch's "SIP;cause=200;text=\"...\"" shape) and update
 * Cisco's Name= / ActiveLoad= / Load= / InactiveLoad= tokens in the
 * info->device_name / active_load / inactive_load fields.
 *
 * Preserves fields whose token is absent (or whose Reason header is
 * absent entirely) — real Cisco firmware sends Reason on every
 * REGISTER when cisco-usecallmanager mode is enabled, but a REGISTER
 * without it shouldn't be treated as "phone forgot its identity": we
 * leave whatever was previously harvested intact and only refresh the
 * fields the new Reason actually carries. The harvest caller
 * pre-fills info from the existing map entry so this fall-through
 * preservation works across re-REGISTERs. */
static void parse_reason_header(pjsip_msg *msg, struct cisco_mac_info *info)
{
	pj_str_t hdr_name = pj_str("Reason");
	pjsip_generic_string_hdr *hdr;
	char reason[512];
	const char *text;
	size_t len;
	char tmp[sizeof(info->inactive_load)];

	hdr = (pjsip_generic_string_hdr *) pjsip_msg_find_hdr_by_name(msg,
		&hdr_name, NULL);
	if (!hdr) {
		return;
	}
	ast_copy_pj_str(reason, &hdr->hvalue, sizeof(reason));

	/* The chan_sip patch only acts when the prefix is exactly
	 * "SIP;cause=200;text=" — anything else is treated as a non-200
	 * reason and ignored. Replicate. */
	if (strncmp(reason, "SIP;cause=200;text=", 19)) {
		return;
	}
	text = reason + 19;
	if (*text == '"') {
		text++;
		len = strlen(text);
		if (len && text[len - 1] == '"') {
			reason[19 + 1 + len - 1] = '\0';
		}
	}

	/* Each field updates only when its token is present in this
	 * Reason; reason_extract() returns 0 (and zeroes tmp) on miss.
	 * Routing the result through tmp keeps info->* intact when the
	 * token isn't there, so partial Reasons preserve previously-
	 * harvested values. */
	if (reason_extract(text, "Name", tmp, sizeof(tmp))) {
		ast_copy_string(info->device_name, tmp, sizeof(info->device_name));
	}
	if (reason_extract(text, "ActiveLoad", tmp, sizeof(tmp))
		|| reason_extract(text, "Load", tmp, sizeof(tmp))) {
		/* Older firmware uses Load= rather than ActiveLoad=. */
		ast_copy_string(info->active_load, tmp, sizeof(info->active_load));
	}
	if (reason_extract(text, "InactiveLoad", tmp, sizeof(tmp))) {
		ast_copy_string(info->inactive_load, tmp, sizeof(info->inactive_load));
	}
}

/* On every authenticated REGISTER from a Cisco endpoint, learn (or
 * refresh) the device MAC + device-name + firmware versions. expires=0
 * / Contact: * is a de-registration: forget any hint for that MAC.
 * Never claims the request — the registrar still does its job. */
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
	struct cisco_mac_info info;

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
		cisco_mac_forget(mac);
		ast_debug(2, "cisco-mac-identify: forgot MAC %s "
			"(de-registration from endpoint '%s')\n", mac, endpoint_id);
		ao2_cleanup(endpoint);
		return;
	}
	if (ttl > 86400) {
		ttl = 86400;
	}

	memset(&info, 0, sizeof(info));
	ast_copy_string(info.mac, mac, sizeof(info.mac));
	ast_copy_string(info.endpoint_id, endpoint_id, sizeof(info.endpoint_id));
	ast_copy_string(info.src_host, rdata->pkt_info.src_name,
		sizeof(info.src_host));
	/* Call-ID lets 'pjsip cisco status' attribute device facts to the
	 * specific contact that produced them, instead of conflating
	 * multiple contacts under one endpoint-keyed lookup. */
	if (rdata->msg_info.cid) {
		ast_copy_pj_str(info.call_id, &rdata->msg_info.cid->id,
			sizeof(info.call_id));
	}
	info.expires = ast_tvnow();
	info.expires.tv_sec += ttl + 60;     /* small grace past the registration */

	/* Pre-fill the Reason-derived fields from any existing entry for
	 * this MAC, so a REGISTER without a Reason header (or with a
	 * partial Reason that only carries some of the tokens) preserves
	 * previously-harvested device facts instead of wiping them.
	 * parse_reason_header() overwrites only the fields the new
	 * Reason actually carries. */
	{
		struct cisco_mac_info existing;
		if (!cisco_mac_lookup_by_mac(mac, &existing)) {
			ast_copy_string(info.device_name, existing.device_name,
				sizeof(info.device_name));
			ast_copy_string(info.active_load, existing.active_load,
				sizeof(info.active_load));
			ast_copy_string(info.inactive_load, existing.inactive_load,
				sizeof(info.inactive_load));
		}
	}

	/* Reason header (device name + firmware) is optional — older firmware
	 * or phones not configured for cisco-usecallmanager Reason reporting
	 * will simply leave these fields empty, which 'pjsip cisco status'
	 * renders as "(unknown)". */
	parse_reason_header(msg, &info);

	cisco_mac_register(&info);
	ast_debug(2, "cisco-mac-identify: learned MAC %s -> endpoint '%s' "
		"from %s (ttl %lds, device='%s' active='%s' inactive='%s')\n",
		mac, endpoint_id, info.src_host, ttl,
		info.device_name, info.active_load, info.inactive_load);

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
	struct cisco_mac_info info;
	struct ast_sip_endpoint *endpoint;
	struct cisco_endpoint *cisco;

	if (!rdata || !rdata->msg_info.msg
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

	if (cisco_mac_lookup_by_mac(mac, &info)) {
		return NULL;
	}
	if (strcmp(info.src_host, rdata->pkt_info.src_name)) {
		ast_debug(2, "cisco-mac-identify: MAC %s learned from %s but request "
			"arrived from %s — not matching\n",
			mac, info.src_host, rdata->pkt_info.src_name);
		return NULL;
	}

	endpoint = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(), "endpoint",
		info.endpoint_id);
	if (!endpoint) {
		return NULL;
	}
	cisco = cisco_endpoint_get(info.endpoint_id);
	if (!cisco) {
		ao2_cleanup(endpoint);
		return NULL;
	}
	ao2_cleanup(cisco);

	ast_debug(2, "cisco-mac-identify: %.*s from MAC %s identified as "
		"endpoint '%s'\n",
		(int) rdata->msg_info.msg->line.req.method.name.slen,
		rdata->msg_info.msg->line.req.method.name.ptr,
		mac, info.endpoint_id);
	return endpoint;
}

static struct ast_sip_endpoint_identifier cisco_mac_identifier = {
	.identify_endpoint = cisco_mac_identify,
};

int cisco_feature_events_mac_init(void)
{
	/* The container itself lives in res_pjsip_cisco_endpoint.so —
	 * see res/cisco_endpoint/device.c. This module just registers
	 * the PATH C identifier and contributes REGISTER-time facts. */
	if (ast_sip_register_endpoint_identifier_with_name(
			&cisco_mac_identifier, "cisco_mac")) {
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
}
