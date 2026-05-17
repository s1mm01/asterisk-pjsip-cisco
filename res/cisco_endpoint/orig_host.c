/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Implementation of the cisco_orig_host on_tx_request hook.
 *
 * See cisco/orig_host.h for the rationale (why this is needed and
 * where x-ast-orig-host comes from).
 *
 * Compiled into res_pjsip_cisco_endpoint.so; activated by the
 * endpoint module's load_module.
 */

#include "asterisk.h"

#include <pjsip.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/res_pjsip.h"

#include "cisco/orig_host.h"

/*
 * Parse "host:port" (or bare "host") out of a URI parameter value,
 * copy the host into the supplied pool, return 1 on success with
 * *out_port set when a port was present (0 otherwise). Returns 0 if
 * the value is empty.
 */
static int cisco_parse_uri_param_hostport(const pj_str_t *value,
	pj_pool_t *pool, pj_str_t *out_host, int *out_port)
{
	int n = (int) value->slen;
	const char *s = value->ptr;
	int colon = -1;
	int i;

	if (n <= 0 || !s) {
		return 0;
	}
	for (i = 0; i < n; i++) {
		if (s[i] == ':') {
			colon = i;
			break;
		}
	}
	if (colon >= 0) {
		pj_str_t host = { (char *) s, colon };
		pj_str_t port_str = { (char *) s + colon + 1, n - colon - 1 };

		pj_strdup(pool, out_host, &host);
		*out_port = (int) pj_strtoul(&port_str);
	} else {
		pj_str_t whole = { (char *) s, n };

		pj_strdup(pool, out_host, &whole);
		*out_port = 0;
	}
	return 1;
}

/*
 * on_tx_request: applied to every outbound SIP request. Looks for
 * x-ast-orig-host on the Request-URI; if present, rewrites RURI and
 * To-URI host:port to the parameter's value and strips it from the
 * wire-visible URI.
 *
 * Timing: pjsip fires on_tx_request *after* transport selection but
 * *before* serialisation. Rewriting URIs here only changes what
 * appears on the wire, not where the bytes are routed — pjsip has
 * already committed to the cached/just-acquired transport (which
 * was chosen based on the public NAT mapping in the original RURI).
 *
 * Strict no-op when the parameter isn't present: LAN-registered
 * contacts have never been NAT-rewritten by res_pjsip_nat, so their
 * URIs carry no x-ast-orig-host. Outbound to upstream trunks and
 * BLF-subscribed targets is unaffected for the same reason.
 *
 * Idempotency: the param is stripped after first application, so
 * pjsip-level retransmits and auth retries are no-ops on subsequent
 * passes through this hook.
 */
static pj_status_t cisco_orig_host_tx_request(pjsip_tx_data *tdata)
{
	static const pj_str_t orig_host_name = { "x-ast-orig-host", 15 };
	pjsip_sip_uri *ruri;
	pjsip_param *orig_p;
	pj_str_t orig_host = { NULL, 0 };
	int orig_port = 0;
	pjsip_fromto_hdr *to_hdr;
	pjsip_sip_uri *to_uri;

	if (!tdata || !tdata->msg || tdata->msg->type != PJSIP_REQUEST_MSG) {
		return PJ_SUCCESS;
	}

	ruri = (pjsip_sip_uri *) pjsip_uri_get_uri(tdata->msg->line.req.uri);
	if (!ruri
		|| !(PJSIP_URI_SCHEME_IS_SIP(ruri)
			|| PJSIP_URI_SCHEME_IS_SIPS(ruri))) {
		return PJ_SUCCESS;
	}
	orig_p = pjsip_param_find(&ruri->other_param, &orig_host_name);
	if (!orig_p
		|| !cisco_parse_uri_param_hostport(&orig_p->value, tdata->pool,
			&orig_host, &orig_port)) {
		return PJ_SUCCESS;
	}

	pj_strassign(&ruri->host, &orig_host);
	ruri->port = orig_port;
	pj_list_erase(orig_p);

	to_hdr = (pjsip_fromto_hdr *) pjsip_msg_find_hdr(tdata->msg,
		PJSIP_H_TO, NULL);
	if (to_hdr && to_hdr->uri) {
		to_uri = (pjsip_sip_uri *) pjsip_uri_get_uri(to_hdr->uri);
		if (to_uri && (PJSIP_URI_SCHEME_IS_SIP(to_uri)
				|| PJSIP_URI_SCHEME_IS_SIPS(to_uri))) {
			pj_strassign(&to_uri->host, &orig_host);
			to_uri->port = orig_port;
			/* In case res_pjsip_nat populated To-URI's
			 * x-ast-orig-host too (it doesn't currently, but
			 * harmless to strip). */
			while ((orig_p = pjsip_param_find(&to_uri->other_param,
					&orig_host_name))) {
				pj_list_erase(orig_p);
			}
		}
	}

	return PJ_SUCCESS;
}

static pjsip_module cisco_orig_host_module = {
	.name           = { "cisco-orig-host-rewrite", 23 },
	.id             = -1,
	/* Application priority — pure URI rewrite, no transaction state
	 * inspection. Order vs. res_pjsip_nat is irrelevant: res_pjsip_nat
	 * touches Contact and Via on outbound; this hook touches RURI and
	 * To-URI. Disjoint header sets. */
	.priority       = PJSIP_MOD_PRIORITY_APPLICATION,
	.on_tx_request  = cisco_orig_host_tx_request,
};

int cisco_orig_host_register(void)
{
	if (ast_sip_register_service(&cisco_orig_host_module)) {
		return -1;
	}
	return 0;
}

void cisco_orig_host_unregister(void)
{
	ast_sip_unregister_service(&cisco_orig_host_module);
}
