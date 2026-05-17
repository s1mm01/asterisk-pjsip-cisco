/*
 * Asterisk -- An open source telephony toolkit.
 *
 * URI / transport / XML / media-type / rdata-resolver utilities for the
 * res_pjsip_cisco_* family of modules.
 *
 * Lives separately from cisco/endpoint.h so modules that only need the
 * sorcery struct + DND/HuntGroup/CF accessors don't pull in libxml2 +
 * pjsip multipart + transport-state plumbing. The pjsip_module on_rx_request
 * gate (cisco_pjsip_module_match) and the multipart Cisco RemoteCC request
 * body locator live here too — both are rdata-shaped helpers consumed by
 * the modules that hook incoming SIP requests.
 *
 * Bodies live in res/res/cisco_endpoint/rdata.c, compiled into
 * res_pjsip_cisco_endpoint.so; other cisco_* modules resolve the
 * symbols at load time.
 *
 * Depends on cisco/endpoint.h for cisco_endpoint_get (the Cisco-flag check
 * inside cisco_pjsip_module_match).
 */

#ifndef _RES_PJSIP_CISCO_RDATA_H
#define _RES_PJSIP_CISCO_RDATA_H

#include "asterisk.h"

#include <pjlib.h>
#include <pjsip.h>

#include "asterisk/res_pjsip.h"
#include "asterisk/xml.h"

#include "cisco/endpoint.h"

/*!
 * \brief Return the SIP URI inside an outgoing request's From header.
 */
pjsip_sip_uri *cisco_tdata_from_sip_uri(pjsip_tx_data *tdata);

/*!
 * \brief Copy a SIP URI host[:port] into \a buf.
 *
 * PJSIP stores IPv6 URI hosts without brackets. Add them back before
 * callers splice the value into another SIP URI string.
 */
int cisco_copy_sip_uri_hostport(const pjsip_sip_uri *uri,
	char *buf, size_t buflen);

/* Copy a usable host string out of an explicitly configured transport
 * state into buf: external_signaling_address (NAT) if set, else the
 * bind address, skipping the wildcard 0.0.0.0 / :: case. Returns buf,
 * or NULL if the transport offers nothing usable. */
const char *cisco_transport_state_domain(
	struct ast_sip_transport_state *tstate, char *buf, size_t buflen);

/*!
 * \brief Resolve the SIP domain string to use in Cisco-generated URI
 *        fragments.
 *
 * Prefer the From URI Asterisk/PJSIP already built on \a tdata. That
 * preserves endpoint from_domain, selected transport, local address,
 * port, and IPv6 bracketing decisions made by core PJSIP code. The
 * endpoint transport fallback is only for paths that do not have a
 * tx_data yet.
 *
 * \param endpoint  Cisco endpoint receiving the message
 * \param tdata     outgoing request, after ast_sip_create_request()
 * \param buf       caller-supplied scratch buffer
 * \param buflen    size of buf
 * \return pointer to a NUL-terminated string. Lifetime is bounded by
 *         \a endpoint or \a buf depending on which case matched.
 */
const char *cisco_endpoint_local_domain(
	struct ast_sip_endpoint *endpoint, pjsip_tx_data *tdata, char *buf,
	size_t buflen);

/*!
 * \brief Parse a pjsip_msg_body's data as XML.
 *
 * \return ast_xml_doc on success (caller \c ast_xml_close()s),
 *         NULL on empty body or parse failure.
 *
 * libxml2 (under \c ast_xml_read_memory) parses into its own internal
 * representation and treats the input buffer read-only — the pjsip
 * pool buffer is safe to use directly without a defensive copy. The
 * (void *) → (char *) cast is required by the asterisk/xml.h
 * signature; libxml2 does not write through.
 */
struct ast_xml_doc *cisco_xml_read_body(const pjsip_msg_body *body);

/*!
 * \name Cisco RemoteCC body / rdata helpers
 *
 * Common scaffolding consumed by feature_events, remotecc, conference
 * — and historically reimplemented in each. Lifting here so a future
 * fourth consumer can't drift the shape.
 */
/* @{ */

/*!
 * \brief Case-insensitive match of a pjsip_media_type against type/subtype.
 */
int cisco_media_type_is(const pjsip_media_type *media,
	const char *type, const char *subtype);

/*!
 * \brief Locate the application/x-cisco-remotecc-request+xml body in an
 *        incoming request, walking into a multipart/mixed wrapper if
 *        present.
 *
 * Belt-and-suspenders: checks both \c msg_info.ctype and the body's own
 * \c content_type. Both fields are populated from the same Content-Type
 * header under normal parsing, but they have diverged in the wild on
 * malformed or oddly-framed REFERs from Cisco firmware (multipart
 * wrappers around a single x-cisco-* part, REFERs whose Content-Type
 * was rebuilt by an intermediary). Checking both costs nothing and
 * avoids dropping legitimate Cisco traffic on the floor.
 */
pjsip_msg_body *cisco_find_remotecc_request_body(pjsip_rx_data *rdata);

/*!
 * \brief Copy a named XML child's text into \a buf.
 * \return 1 on success (buf non-empty), 0 on failure.
 */
int cisco_xml_copy_child_text(struct ast_xml_node *parent,
	const char *name, char *buf, size_t buflen);

/*!
 * \brief Resolve the endpoint from an incoming SIP request. Prefers
 *        rdata's already-attached endpoint reference (set during
 *        authentication); falls back to the To-header user part for
 *        the auth-bypassed paths (PUBLISH-from-MAC, etc.).
 *
 * Caller takes an ao2 reference and must ao2_cleanup() the result.
 */
struct ast_sip_endpoint *cisco_rdata_get_endpoint(pjsip_rx_data *rdata);
/* @} */

/*!
 * \brief Common on_rx_request gate for pjsip_modules at
 *        PJSIP_MOD_PRIORITY_APPLICATION - 1.
 *
 * Combines the four steps every Cisco pjsip_module opens with:
 *   1. is it a REQUEST?
 *   2. does the method match \a method_name (case-insensitive)?
 *   3. (if \a opt_event_name is non-NULL) does the Event header value
 *      match it (case-insensitive)?
 *   4. is there an identified endpoint, and is it Cisco-flagged?
 *
 * \param rdata          the on_rx_request rdata
 * \param method_name    e.g. "REGISTER", "SUBSCRIBE", "PUBLISH"
 * \param opt_event_name e.g. "as-feature-event" or "presence"; NULL
 *                       skips the Event-header check
 *
 * \return ao2-ref'd \c ast_sip_endpoint on match (caller must
 *         \c ao2_cleanup), NULL on any miss. The Cisco-flag check uses
 *         \c cisco_endpoint_get and releases its ref internally — the
 *         returned endpoint is the standard PJSIP endpoint.
 */
struct ast_sip_endpoint *cisco_pjsip_module_match(
	pjsip_rx_data *rdata, const char *method_name,
	const char *opt_event_name);

#endif /* _RES_PJSIP_CISCO_RDATA_H */
