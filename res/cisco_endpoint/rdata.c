/*
 * Asterisk -- An open source telephony toolkit.
 *
 * URI / transport / XML / media-type / rdata-resolver utilities for the
 * res_pjsip_cisco_* family of modules. Bodies for the declarations in
 * cisco/rdata.h; linked into res_pjsip_cisco_endpoint.so and resolved
 * by the other cisco_* modules at load time via the dynamic symbol
 * table.
 */

#include "asterisk.h"

#include <pjlib.h>
#include <pjsip.h>
#include <pjsip/sip_multipart.h>

#include "asterisk/astobj2.h"
#include "asterisk/netsock2.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/xml.h"

#include "cisco/endpoint.h"
#include "cisco/rdata.h"

pjsip_sip_uri *cisco_tdata_from_sip_uri(pjsip_tx_data *tdata)
{
	pjsip_fromto_hdr *from;
	pjsip_uri *uri;

	if (!tdata || !tdata->msg) {
		return NULL;
	}

	from = PJSIP_MSG_FROM_HDR(tdata->msg);
	if (!from || !from->uri) {
		return NULL;
	}

	uri = pjsip_uri_get_uri(from->uri);
	if (!uri || (!PJSIP_URI_SCHEME_IS_SIP(uri)
			&& !PJSIP_URI_SCHEME_IS_SIPS(uri))) {
		return NULL;
	}

	return (pjsip_sip_uri *) uri;
}

int cisco_copy_sip_uri_hostport(const pjsip_sip_uri *uri,
	char *buf, size_t buflen)
{
	int used;

	if (!uri || !buf || buflen == 0 || !uri->host.slen) {
		return -1;
	}

	if (pj_strchr(&uri->host, ':')) {
		used = snprintf(buf, buflen, "[%.*s]",
			(int) uri->host.slen, uri->host.ptr);
	} else {
		used = snprintf(buf, buflen, "%.*s",
			(int) uri->host.slen, uri->host.ptr);
	}
	if (used < 0 || used >= (int) buflen) {
		return -1;
	}

	if (uri->port) {
		int port_used = snprintf(buf + used, buflen - used, ":%d",
			uri->port);

		if (port_used < 0 || port_used >= (int) (buflen - used)) {
			return -1;
		}
	}

	return 0;
}

const char *cisco_transport_state_domain(
	struct ast_sip_transport_state *tstate, char *buf, size_t buflen)
{
	if (!tstate) {
		return NULL;
	}
	if (!ast_sockaddr_isnull(&tstate->external_signaling_address)) {
		ast_copy_string(buf,
			ast_sockaddr_stringify_host_remote(
				&tstate->external_signaling_address),
			buflen);
		return buf;
	}
	if (pj_sockaddr_has_addr(&tstate->host)) {
		pj_sockaddr_print(&tstate->host, buf, (int) buflen, 2);
		return buf;
	}
	return NULL;
}

const char *cisco_endpoint_local_domain(
	struct ast_sip_endpoint *endpoint, pjsip_tx_data *tdata, char *buf,
	size_t buflen)
{
	pjsip_sip_uri *from_uri;
	struct ast_sip_transport_state *tstate;
	const char *result = NULL;

	if (!endpoint || !buf || buflen == 0) {
		return "localhost";
	}

	from_uri = cisco_tdata_from_sip_uri(tdata);
	if (!cisco_copy_sip_uri_hostport(from_uri, buf, buflen)) {
		return buf;
	}

	if (!ast_strlen_zero(endpoint->fromdomain)) {
		return endpoint->fromdomain;
	}

	/* The endpoint's own transport=, if it has one. */
	if (!ast_strlen_zero(endpoint->transport)
		&& (tstate = ast_sip_get_transport_state(endpoint->transport))) {
		result = cisco_transport_state_domain(tstate, buf, buflen);
		ao2_ref(tstate, -1);
		if (result) {
			return result;
		}
	}

	ast_copy_string(buf, "localhost", buflen);
	return buf;
}

struct ast_xml_doc *cisco_xml_read_body(const pjsip_msg_body *body)
{
	if (!body || !body->data || body->len == 0) {
		return NULL;
	}
	return ast_xml_read_memory((char *) body->data, body->len);
}

int cisco_media_type_is(const pjsip_media_type *media,
	const char *type, const char *subtype)
{
	return media
		&& !pj_stricmp2(&media->type, type)
		&& !pj_stricmp2(&media->subtype, subtype);
}

pjsip_msg_body *cisco_find_remotecc_request_body(pjsip_rx_data *rdata)
{
	pjsip_msg_body *body;
	const pjsip_media_type *body_ct;
	const pjsip_media_type *info_ct = NULL;

	if (!rdata || !rdata->msg_info.msg || !rdata->msg_info.msg->body) {
		return NULL;
	}
	body    = rdata->msg_info.msg->body;
	body_ct = &body->content_type;
	if (rdata->msg_info.ctype) {
		info_ct = &rdata->msg_info.ctype->media;
	}

	if (cisco_media_type_is(body_ct, "application", "x-cisco-remotecc-request+xml")
		|| cisco_media_type_is(info_ct, "application", "x-cisco-remotecc-request+xml")) {
		return body;
	}

	if (cisco_media_type_is(body_ct, "multipart", "mixed")
		|| cisco_media_type_is(info_ct, "multipart", "mixed")) {
		pjsip_media_type remotecc_type;
		pjsip_multipart_part *part;

		pjsip_media_type_init2(&remotecc_type, "application",
			"x-cisco-remotecc-request+xml");
		part = pjsip_multipart_find_part(body, &remotecc_type, NULL);
		if (part && part->body) {
			return part->body;
		}
	}
	return NULL;
}

int cisco_xml_copy_child_text(struct ast_xml_node *parent,
	const char *name, char *buf, size_t buflen)
{
	struct ast_xml_node *child;
	const char *text;

	if (!buf || buflen == 0) {
		return 0;
	}
	buf[0] = '\0';

	child = ast_xml_find_element(ast_xml_node_get_children(parent), name,
		NULL, NULL);
	if (!child) {
		return 0;
	}

	text = ast_xml_get_text(child);
	if (!text) {
		return 0;
	}

	ast_copy_string(buf, text, buflen);
	ast_xml_free_text(text);
	return !ast_strlen_zero(buf);
}

struct ast_sip_endpoint *cisco_rdata_get_endpoint(pjsip_rx_data *rdata)
{
	struct ast_sip_endpoint *endpoint;
	pjsip_to_hdr *to;
	pjsip_sip_uri *uri;
	char id[128];

	if (!rdata || !rdata->msg_info.msg) {
		return NULL;
	}

	endpoint = ast_pjsip_rdata_get_endpoint(rdata);
	if (endpoint) {
		return endpoint;
	}

	to = (pjsip_to_hdr *) pjsip_msg_find_hdr(rdata->msg_info.msg,
		PJSIP_H_TO, NULL);
	if (!to || !to->uri) {
		return NULL;
	}
	if (!PJSIP_URI_SCHEME_IS_SIP(to->uri)
		&& !PJSIP_URI_SCHEME_IS_SIPS(to->uri)) {
		return NULL;
	}
	uri = (pjsip_sip_uri *) pjsip_uri_get_uri(to->uri);
	if (!uri) {
		return NULL;
	}
	ast_copy_pj_str(id, &uri->user, sizeof(id));
	return ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(), "endpoint", id);
}

struct ast_sip_endpoint *cisco_pjsip_module_match(
	pjsip_rx_data *rdata, const char *method_name,
	const char *opt_event_name)
{
	pj_str_t event_hdr_name;
	pjsip_generic_string_hdr *event_hdr;
	struct ast_sip_endpoint *endpoint;
	struct cisco_endpoint *cisco;

	if (!rdata || !rdata->msg_info.msg
		|| rdata->msg_info.msg->type != PJSIP_REQUEST_MSG) {
		return NULL;
	}

	if (pj_stricmp2(&rdata->msg_info.msg->line.req.method.name,
			method_name)) {
		return NULL;
	}

	if (opt_event_name) {
		pj_cstr(&event_hdr_name, "Event");
		event_hdr = (pjsip_generic_string_hdr *)
			pjsip_msg_find_hdr_by_name(rdata->msg_info.msg,
				&event_hdr_name, NULL);
		if (!event_hdr
			|| pj_stricmp2(&event_hdr->hvalue, opt_event_name)) {
			return NULL;
		}
	}

	endpoint = cisco_rdata_get_endpoint(rdata);
	if (!endpoint) {
		return NULL;
	}

	cisco = cisco_endpoint_get(ast_sorcery_object_get_id(endpoint));
	if (!cisco) {
		ao2_cleanup(endpoint);
		return NULL;
	}
	ao2_cleanup(cisco);

	return endpoint;
}
