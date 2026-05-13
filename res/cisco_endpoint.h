/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Shared header for the res_pjsip_cisco_* family of modules.
 *
 * Defines the 'cisco' sorcery type and helper accessors so the
 * body-generator, optionsind, bulkupdate, unsolicited-BLF and RemoteCC modules
 * can pull per-endpoint config from pjsip.conf and shared astdb
 * conventions instead of carrying local per-module defaults.
 *
 * pjsip.conf usage:
 *
 *     [1010]
 *     type           = cisco
 *     line_index     = 1
 *     subscribe      = 1001,1002,1003,1004,1005,1006,1007
 *
 * The section name MUST match the endpoint section name. The presence
 * of a [name] type=cisco section is the gating signal — endpoints
 * without one are treated as non-Cisco and the cisco_* modules leave
 * them alone.
 */

#ifndef _RES_PJSIP_CISCO_ENDPOINT_H
#define _RES_PJSIP_CISCO_ENDPOINT_H

#include "asterisk.h"

#include <pjlib.h>
#include <pjsip/sip_multipart.h>
#include <string.h>

#include "asterisk/stringfields.h"
#include "asterisk/sorcery.h"
#include "asterisk/astobj2.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/netsock2.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/astdb.h"
#include "asterisk/xml.h"

struct cisco_endpoint {
	SORCERY_OBJECT(details);
	AST_DECLARE_STRING_FIELDS(
		/*! Comma-separated list of extensions whose state we push to
		 * the phone via unsolicited NOTIFY at REGISTER time. */
		AST_STRING_FIELD(subscribe);
		/*! Default context to look up subscribe= extensions in.
		 * Empty -> use the default "local_sip_phone" fallback. */
		AST_STRING_FIELD(subscribe_context);
		/*! Comma-separated list of OTHER cisco-endpoint IDs that
		 * share the same physical phone (multi-line phone, one
		 * endpoint per line button). The bulkupdate REFER emits a
		 * <contact line="N"> element for each, sourcing line_index
		 * from each alias's own cisco sorcery object. Empty
		 * (default) = single-line phone. */
		AST_STRING_FIELD(aliases);
		/*! Dialplan extension the RemoteCC Park softkey blind-transfers
		 * the call to — i.e. res_parking's parkext. Default "700";
		 * change to match res_parking.conf if a site uses a different
		 * parkext. The context is resolved per call (TRANSFER_CONTEXT
		 * chan var, else the endpoint's context). */
		AST_STRING_FIELD(parkext);
	);
	/*! Cisco line button index (1 for primary line on most phones). */
	int line_index;
	/*! When DND is enabled, reject calls (1) vs ring silently (0).
	 * This is a static preference; the on/off state itself is read
	 * at runtime from astdb (key DND/<endpoint-id>). */
	int dnd_busy;
};

/*!
 * \brief Inline helper: retrieve cisco config for an endpoint by id.
 * \retval ao2 ref-bumped cisco_endpoint, NULL if endpoint isn't
 *         configured as Cisco.
 *
 * Caller must ao2_cleanup() the returned object.
 *
 * Implemented inline so each consuming module gets its own copy and
 * we don't need to export a symbol from res_pjsip_cisco_endpoint.so
 * — Asterisk's per-module .exports linker scripts hide everything by
 * default, and exporting cross-module helpers reliably is a fight
 * with the build system. The struct definition above is the shared
 * contract; the sorcery type registration is the only thing
 * res_pjsip_cisco_endpoint.so uniquely owns.
 */
static inline struct cisco_endpoint *cisco_endpoint_get(const char *id)
{
	if (ast_strlen_zero(id)) {
		return NULL;
	}
	return ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(), "cisco", id);
}

/*!
 * \brief Return the SIP URI inside an outgoing request's From header.
 */
static inline pjsip_sip_uri *cisco_tdata_from_sip_uri(pjsip_tx_data *tdata)
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

/*!
 * \brief Copy a SIP URI host[:port] into \a buf.
 *
 * PJSIP stores IPv6 URI hosts without brackets. Add them back before
 * callers splice the value into another SIP URI string.
 */
static inline int cisco_copy_sip_uri_hostport(const pjsip_sip_uri *uri,
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

/* Copy a usable host string out of an explicitly configured transport
 * state into buf: external_signaling_address (NAT) if set, else the
 * bind address, skipping the wildcard 0.0.0.0 / :: case. Returns buf,
 * or NULL if the transport offers nothing usable. */
static inline const char *cisco_transport_state_domain(
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
static inline const char *cisco_endpoint_local_domain(
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

/*!
 * \brief Did this REGISTER 200 OK actually register a contact?
 *
 * Returns 0 for deregistration responses (Contact: * present, or all
 * Contact headers carry expires=0). Returns 1 for any 200 OK that
 * registers at least one contact with a non-zero lifetime.
 *
 * The cisco_* REGISTER supplements use this to skip post-REGISTER
 * follow-up traffic (bulkupdate REFER, unsolicited NOTIFYs,
 * optionsind body) when the phone is going away — those would
 * either fail or race with contact removal.
 */
static inline int cisco_response_registers_contact(pjsip_msg *msg)
{
	pjsip_contact_hdr *contact;

	if (!msg) {
		return 0;
	}

	contact = (pjsip_contact_hdr *) pjsip_msg_find_hdr(msg,
		PJSIP_H_CONTACT, NULL);
	while (contact) {
		if (contact->star) {
			/* Contact: * unambiguously means deregister. */
			return 0;
		}
		if (contact->expires > 0) {
			return 1;
		}
		contact = (pjsip_contact_hdr *) pjsip_msg_find_hdr(msg,
			PJSIP_H_CONTACT, contact->next);
	}
	return 0;
}

/*!
 * \name Address-change cache for REGISTER supplements
 *
 * The three REGISTER supplements (optionsind / bulkupdate /
 * unsolicited_blf) only need to re-fire their heavy follow-up traffic
 * when an endpoint's set of registered Contact URIs actually changed
 * (initial REGISTER, phone moved IP, NAT pinhole churn). Refresh
 * REGISTERs every ~60s carry the same Contact set and should be no-ops
 * — chan_sip's cisco-usecallmanager patch mirrors this via its
 * addrchanged guard in parse_register_contact.
 *
 * Cache is per-module, in-memory (NOT persistent astdb): asterisk
 * restart, module unload, and module reload all dump the cache,
 * guaranteeing the next REGISTER from each phone is correctly treated
 * as "changed" and re-bootstraps. The chan_sip patch gets this for
 * free because its peer->addr is in-memory; we have to be explicit
 * because the Cisco endpoint registry on PJSIP is sorcery-backed and
 * survives reloads.
 *
 * The cached canonical-contact-set string includes EVERY non-star,
 * non-expires=0 Contact in the REGISTER 200 OK, so the helper is
 * correct under max_contacts > 1 — a new or removed contact later in
 * the response changes the string and the supplement re-fires.
 *
 * Lifecycle:
 *   cisco_register_address_changed()  - read-only check
 *   cisco_register_address_remember() - persist the new value on
 *                                       successful guarded operation
 *   cisco_register_address_forget()   - clear cache for an endpoint
 *                                       on its deregister, so the
 *                                       next re-register with the
 *                                       same Contact URI re-bootstraps
 *
 * Each supplement creates its own ao2 container via
 * cisco_addr_cache_alloc() at load_module time and ao2_cleanup()s it
 * at unload — keeping the three modules' caches independent of each
 * other (so they consistently skip-or-fire as a group regardless of
 * which supplement runs first).
 */
/* @{ */

struct cisco_addr_cache_entry {
	/* Heap-allocated so endpoint IDs longer than any fixed buffer
	 * (Asterisk imposes no upper bound on sorcery object IDs) hash
	 * and compare correctly. A fixed-size key would silently
	 * truncate on remember() while ao2_find() hashes the full ID —
	 * resulting in long-ID endpoints never hitting the cache and
	 * re-firing every refresh REGISTER. */
	char *endpoint_id;
	char *contacts;  /* canonical sorted-by-insertion-order concat */
};

static void cisco_addr_cache_entry_destroy(void *obj)
{
	struct cisco_addr_cache_entry *e = obj;
	ast_free(e->endpoint_id);
	ast_free(e->contacts);
}

static int cisco_addr_cache_hash(const void *obj, const int flags)
{
	const struct cisco_addr_cache_entry *e;
	const char *key;

	switch (flags & OBJ_SEARCH_MASK) {
	case OBJ_SEARCH_KEY:
		key = obj;
		break;
	case OBJ_SEARCH_OBJECT:
		e = obj;
		key = e->endpoint_id;
		break;
	default:
		ast_assert(0);
		return 0;
	}
	return ast_str_hash(key);
}

static int cisco_addr_cache_cmp(void *obj, void *arg, int flags)
{
	const struct cisco_addr_cache_entry *left = obj;
	const char *right_key;

	switch (flags & OBJ_SEARCH_MASK) {
	case OBJ_SEARCH_OBJECT:
		right_key = ((const struct cisco_addr_cache_entry *) arg)->endpoint_id;
		break;
	case OBJ_SEARCH_KEY:
		right_key = arg;
		break;
	default:
		ast_assert(0);
		return 0;
	}
	return strcmp(left->endpoint_id, right_key) ? 0 : CMP_MATCH | CMP_STOP;
}

static inline struct ao2_container *cisco_addr_cache_alloc(void)
{
	return ao2_container_alloc_hash(AO2_ALLOC_OPT_LOCK_MUTEX, 0, 31,
		cisco_addr_cache_hash, NULL, cisco_addr_cache_cmp);
}

/*!
 * \brief Render the canonical contact-set string for the given REGISTER
 *        200 OK into a dynamic ast_str. Iterates every Contact header
 *        with expires > 0 (skips deregister rows). Caller frees with
 *        ast_free().
 *
 * \retval NULL on alloc failure, no contacts, or Contact: * (deregister)
 */
static inline struct ast_str *cisco_response_contacts_canonical(pjsip_msg *msg)
{
	pjsip_contact_hdr *contact;
	struct ast_str *out;
	int saw_any = 0;
	char one[PJSIP_MAX_URL_SIZE];

	if (!msg) {
		return NULL;
	}

	out = ast_str_create(512);
	if (!out) {
		return NULL;
	}

	contact = (pjsip_contact_hdr *) pjsip_msg_find_hdr(msg,
		PJSIP_H_CONTACT, NULL);
	while (contact) {
		int len;

		if (contact->star) {
			/* Deregister-all sentinel — nothing to remember. */
			ast_free(out);
			return NULL;
		}
		if (contact->expires == 0) {
			contact = (pjsip_contact_hdr *) pjsip_msg_find_hdr(msg,
				PJSIP_H_CONTACT, contact->next);
			continue;
		}

		len = pjsip_uri_print(PJSIP_URI_IN_CONTACT_HDR, contact->uri,
			one, sizeof(one) - 1);
		if (len > 0) {
			one[len] = '\0';
			if (saw_any) {
				ast_str_append(&out, 0, "|");
			}
			ast_str_append(&out, 0, "%s", one);
			saw_any = 1;
		}

		contact = (pjsip_contact_hdr *) pjsip_msg_find_hdr(msg,
			PJSIP_H_CONTACT, contact->next);
	}

	if (!saw_any) {
		ast_free(out);
		return NULL;
	}
	return out;
}

/*!
 * \brief Read-only: does the canonical Contact set in \a msg differ
 *        from what \a cache last remembered for this endpoint?
 *
 * Returns 1 (changed, caller should fire) when:
 *   - no cache entry exists for this endpoint, OR
 *   - the canonical Contact set differs from the cached one.
 * Returns 0 (unchanged, caller should skip) only when the canonical
 * Contact set is byte-identical to what was last remembered.
 *
 * Does NOT update the cache. Caller commits via
 * cisco_register_address_remember() AFTER successfully performing the
 * guarded operation; that way a failed body-build or task-queue
 * doesn't leave a "we fired" mark that suppresses the next retry.
 */
static inline int cisco_register_address_changed(pjsip_msg *msg,
	const char *endpoint_id, struct ao2_container *cache)
{
	struct cisco_addr_cache_entry *entry;
	struct ast_str *current;
	int changed = 1;

	if (!msg || !cache || ast_strlen_zero(endpoint_id)) {
		return 1;
	}

	current = cisco_response_contacts_canonical(msg);
	if (!current) {
		return 1;
	}

	entry = ao2_find(cache, endpoint_id, OBJ_SEARCH_KEY);
	if (entry) {
		if (entry->contacts
			&& !strcmp(entry->contacts, ast_str_buffer(current))) {
			changed = 0;
		}
		ao2_cleanup(entry);
	}

	ast_free(current);
	return changed;
}

/*!
 * \brief Persist a precomputed canonical Contact string as "last fired".
 *
 * Use when the work being guarded is asynchronous: compute the canonical
 * string in the response hook (where the pjsip_msg is still alive), stash
 * it in the task data, and call this from inside the task only AFTER the
 * task has actually succeeded. Avoids both the "tdata->msg gone after the
 * hook returns" problem and the "remembered before async work succeeded"
 * problem.
 */
static inline void cisco_register_address_remember_str(const char *endpoint_id,
	struct ao2_container *cache, const char *canonical)
{
	struct cisco_addr_cache_entry *entry;

	if (!cache || ast_strlen_zero(endpoint_id) || ast_strlen_zero(canonical)) {
		return;
	}

	/* Replace any existing entry. */
	ao2_find(cache, endpoint_id,
		OBJ_SEARCH_KEY | OBJ_UNLINK | OBJ_NODATA);

	entry = ao2_alloc(sizeof(*entry), cisco_addr_cache_entry_destroy);
	if (!entry) {
		return;
	}
	entry->endpoint_id = ast_strdup(endpoint_id);
	entry->contacts    = ast_strdup(canonical);
	if (!entry->endpoint_id || !entry->contacts) {
		ao2_cleanup(entry);
		return;
	}
	ao2_link(cache, entry);
	ao2_cleanup(entry);
}

/*!
 * \brief Convenience: compute the canonical Contact string from \a msg
 *        and remember it. Use for synchronous flows (optionsind) where
 *        the guarded operation completes before this call returns.
 */
static inline void cisco_register_address_remember(pjsip_msg *msg,
	const char *endpoint_id, struct ao2_container *cache)
{
	struct ast_str *current;

	if (!msg || !cache || ast_strlen_zero(endpoint_id)) {
		return;
	}
	current = cisco_response_contacts_canonical(msg);
	if (!current) {
		return;
	}
	cisco_register_address_remember_str(endpoint_id, cache,
		ast_str_buffer(current));
	ast_free(current);
}

/*!
 * \brief Forget the cached Contact set for an endpoint. Call when the
 *        endpoint deregisters so the next REGISTER (even with the
 *        same Contact URI) re-bootstraps optionsind / bulkupdate /
 *        unsolicited_blf for the fresh session.
 */
static inline void cisco_register_address_forget(const char *endpoint_id,
	struct ao2_container *cache)
{
	if (!cache || ast_strlen_zero(endpoint_id)) {
		return;
	}
	ao2_find(cache, endpoint_id,
		OBJ_SEARCH_KEY | OBJ_UNLINK | OBJ_NODATA);
}

/*!
 * \brief Combined gate for REGISTER outgoing_response supplements.
 *
 * Encapsulates the four-step preamble open-coded by every REGISTER
 * supplement (optionsind / bulkupdate / unsolicited_blf):
 *
 *   1. Is it a 200 OK to a REGISTER?
 *   2. Is the endpoint flagged Cisco?
 *   3. Is it a deregister? If so, forget() the cache and skip.
 *   4. Has the canonical Contact set changed since last fire? If not, skip.
 *   5. (optional) Capture the canonical string for async paths that
 *      stash it in task data and commit after successful send.
 *
 * \param endpoint        the supplement's endpoint argument
 * \param tdata           the supplement's tdata argument
 * \param addr_cache      the per-supplement address-change cache
 * \param endpoint_id_out written on return-1; lifetime bound by \a endpoint
 * \param canonical_out   if non-NULL on entry, the canonical string is
 *                        captured and heap-allocated here (caller frees
 *                        with ast_free). Pass NULL for the synchronous
 *                        optionsind path that doesn't stash it.
 * \retval 1 caller should proceed with the supplement's body of work
 * \retval 0 caller should bail; the cache lifecycle (forget on dereg,
 *           skip on unchanged) has already been handled internally
 */
static inline int cisco_register_should_fire(struct ast_sip_endpoint *endpoint,
	pjsip_tx_data *tdata, struct ao2_container *addr_cache,
	const char **endpoint_id_out, char **canonical_out)
{
	struct cisco_endpoint *cisco;
	const char *endpoint_id;

	if (!endpoint || !tdata || !tdata->msg) {
		return 0;
	}
	if (tdata->msg->type != PJSIP_RESPONSE_MSG
		|| tdata->msg->line.status.code != 200) {
		return 0;
	}

	endpoint_id = ast_sorcery_object_get_id(endpoint);

	cisco = cisco_endpoint_get(endpoint_id);
	if (!cisco) {
		return 0;
	}
	ao2_cleanup(cisco);

	/* Deregister responses: clear cache so the next re-register
	 * (even at the same URI) re-bootstraps. Sending follow-up traffic
	 * at a phone that just deregistered races with contact removal. */
	if (!cisco_response_registers_contact(tdata->msg)) {
		cisco_register_address_forget(endpoint_id, addr_cache);
		return 0;
	}

	/* Refresh REGISTERs carry the same Contact set every ~60s; skip
	 * the supplement's work unless something actually changed. Mirrors
	 * the chan_sip patch's addrchanged guard. */
	if (!cisco_register_address_changed(tdata->msg, endpoint_id,
			addr_cache)) {
		return 0;
	}

	if (canonical_out) {
		struct ast_str *canonical;

		canonical = cisco_response_contacts_canonical(tdata->msg);
		if (!canonical) {
			return 0;
		}
		*canonical_out = ast_strdup(ast_str_buffer(canonical));
		ast_free(canonical);
		if (!*canonical_out) {
			return 0;
		}
	}

	if (endpoint_id_out) {
		*endpoint_id_out = endpoint_id;
	}
	return 1;
}
/* @} */

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
static inline struct ast_xml_doc *cisco_xml_read_body(
	const pjsip_msg_body *body)
{
	if (!body || !body->data || body->len == 0) {
		return NULL;
	}
	return ast_xml_read_memory((char *) body->data, body->len);
}

/*!
 * \name REFER fan-out to all registered contacts
 *
 * Cisco unsolicited REFERs (bulkupdate, HLog state push, MCID feedback,
 * ConfList menu) all share the same loop: walk every AOR on the
 * endpoint, fetch each AOR's registered contacts, build one REFER per
 * contact with a Content-ID-keyed body. Lift the scaffolding here so
 * the call sites collapse to: (1) define a ctx struct, (2) write a
 * one-line body-builder adapter, (3) call this helper.
 *
 * The per-iter log is uniform: "<prefix>: <subject> sent to <uri>".
 * If a caller wants additional context (endpoint_id, state flags) on
 * the success line, log it once before the helper call at the
 * task-start boundary.
 */
/* @{ */

typedef pjsip_msg_body *(*cisco_refer_body_builder)(pj_pool_t *pool, void *ctx);

/*!
 * \brief Send one REFER per registered contact across every AOR on
 *        \a endpoint, with the body produced by \a build(pool, ctx).
 *
 * \param endpoint       endpoint to fan out across
 * \param log_prefix     module log tag, e.g. "cisco-bulkupdate"
 * \param cid_suffix     trailing @suffix on the Content-ID, e.g.
 *                       "cisco-bulkupdate" — phones don't care about
 *                       the value, only that it matches Refer-To
 * \param subject        short label for log lines, e.g. "REFER" or
 *                       "HLog update"
 * \param build          body-builder callback (called per contact;
 *                       returning NULL aborts that contact, increments
 *                       attempted but not succeeded)
 * \param ctx            opaque builder ctx (passed through unchanged)
 * \param attempted_out  optional; total contacts the loop attempted
 * \param succeeded_out  optional; subset where ast_sip_send_request
 *                       returned success
 *
 * Both counter pointers may be NULL — callers that don't gate a cache
 * commit on all-or-nothing don't need them.
 */
static inline void cisco_endpoint_send_refer_to_all_contacts(
	struct ast_sip_endpoint *endpoint,
	const char *log_prefix, const char *cid_suffix, const char *subject,
	cisco_refer_body_builder build, void *ctx,
	int *attempted_out, int *succeeded_out)
{
	struct ao2_container *contacts;
	struct ao2_iterator iter;
	struct ast_sip_contact *contact;
	int attempted = 0;
	int succeeded = 0;

	if (!endpoint || ast_strlen_zero(endpoint->aors) || !build) {
		goto out;
	}

	contacts = ast_sip_location_retrieve_contacts_from_aor_list(endpoint->aors);
	if (!contacts) {
		goto out;
	}

	iter = ao2_iterator_init(contacts, 0);
	while ((contact = ao2_iterator_next(&iter))) {
		pjsip_tx_data *tdata = NULL;
		char cid[64];
		char refer_to[128];

		/* Count attempts the moment a contact is yielded — any
		 * failure below leaves attempted > succeeded so a partial
		 * multi-contact fan-out doesn't get marked "fully fired"
		 * by callers that gate on equality. */
		attempted++;

		if (ast_sip_create_request("REFER", NULL, endpoint, NULL,
				contact, &tdata)) {
			ast_log(LOG_WARNING,
				"%s: unable to create %s REFER for %s\n",
				log_prefix, subject, contact->uri);
			ao2_cleanup(contact);
			continue;
		}

		snprintf(cid, sizeof(cid), "%08x@%s",
			(unsigned) ast_random(), cid_suffix);
		snprintf(refer_to, sizeof(refer_to), "cid:%s", cid);

		ast_sip_add_header(tdata, "Refer-To", refer_to);
		ast_sip_add_header(tdata, "Require", "norefersub");
		ast_sip_add_header(tdata, "Content-ID", cid);

		tdata->msg->body = build(tdata->pool, ctx);
		if (!tdata->msg->body) {
			ast_log(LOG_ERROR, "%s: failed to build %s body\n",
				log_prefix, subject);
			pjsip_tx_data_dec_ref(tdata);
			ao2_cleanup(contact);
			continue;
		}

		if (ast_sip_send_request(tdata, NULL, endpoint, NULL, NULL)) {
			ast_log(LOG_WARNING, "%s: %s send failed for %s\n",
				log_prefix, subject, contact->uri);
		} else {
			ast_log(LOG_NOTICE, "%s: %s sent to %s\n",
				log_prefix, subject, contact->uri);
			succeeded++;
		}

		ao2_cleanup(contact);
	}
	ao2_iterator_destroy(&iter);
	ao2_cleanup(contacts);

out:
	if (attempted_out) {
		*attempted_out = attempted;
	}
	if (succeeded_out) {
		*succeeded_out = succeeded;
	}
}
/* @} */

/*!
 * \brief Add one application/x-cisco-remotecc-request+xml part to an
 *        existing multipart/mixed body.
 *
 * The XML payload is duplicated into \a pool so the caller can free
 * its source buffer immediately. Used by bulkupdate and remotecc; if
 * a third multipart consumer shows up, this is the shared call site.
 *
 * pjsip's multipart API requires using its own multipart_print_body
 * callback for any multipart subtype — hand-rolling boundaries trips
 * an internal assert in pjproject's transport layer. That's why we
 * go through pjsip_multipart_create_part / add_part rather than
 * just appending to a string.
 */
static inline void cisco_remotecc_multipart_add_part(pj_pool_t *pool,
	pjsip_msg_body *multipart, const char *xml)
{
	pj_str_t part_type    = pj_str("application");
	pj_str_t part_subtype = pj_str("x-cisco-remotecc-request+xml");
	pj_str_t text;
	pjsip_multipart_part *part;

	pj_strdup2(pool, &text, xml);
	part = pjsip_multipart_create_part(pool);
	if (!part) {
		return;
	}
	part->body = pjsip_msg_body_create(pool, &part_type, &part_subtype, &text);
	pjsip_multipart_add_part(pool, multipart, part);
}

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
static inline int cisco_media_type_is(const pjsip_media_type *media,
	const char *type, const char *subtype)
{
	return media
		&& !pj_stricmp2(&media->type, type)
		&& !pj_stricmp2(&media->subtype, subtype);
}

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
static inline pjsip_msg_body *cisco_find_remotecc_request_body(pjsip_rx_data *rdata)
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

/*!
 * \brief Copy a named XML child's text into \a buf.
 * \return 1 on success (buf non-empty), 0 on failure.
 */
static inline int cisco_xml_copy_child_text(struct ast_xml_node *parent,
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

/*!
 * \brief Resolve the endpoint from an incoming SIP request. Prefers
 *        rdata's already-attached endpoint reference (set during
 *        authentication); falls back to the To-header user part for
 *        the auth-bypassed paths (PUBLISH-from-MAC, etc.).
 *
 * Caller takes an ao2 reference and must ao2_cleanup() the result.
 */
static inline struct ast_sip_endpoint *cisco_rdata_get_endpoint(pjsip_rx_data *rdata)
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
static inline struct ast_sip_endpoint *cisco_pjsip_module_match(
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

/*!
 * \name astdb feature-state accessors
 *
 * Cisco feature state (DND, hunt-group login, call-forward-all) is
 * stored in astdb so the dialplan, the feature-event SUBSCRIBE handler,
 * and the bulkupdate / PIDF body builders all share one source of
 * truth. The astdb key shape is part of the documented project
 * contract — see README.md.
 *
 * Setter convention: pass enabled=0 (or NULL/empty target for CF) to
 * delete the key rather than store a "no" value.
 */
/* @{ */

static inline int cisco_dnd_is_enabled(const char *endpoint_id)
{
	char value[16];

	if (ast_strlen_zero(endpoint_id)) {
		return 0;
	}
	if (ast_db_get("DND", endpoint_id, value, sizeof(value))) {
		return 0;
	}
	return ast_true(value);
}

static inline void cisco_dnd_set(const char *endpoint_id, int enabled)
{
	/* No NULL guard: matches the original direct ast_db_put/del call
	 * sites. A defensive ast_strlen_zero(endpoint_id) here gives GCC
	 * grounds to infer that endpoint_id may be NULL at every caller,
	 * which trips -Wformat-overflow on subsequent ast_log("%s", ...). */
	if (enabled) {
		ast_db_put("DND", endpoint_id, "YES");
	} else {
		ast_db_del("DND", endpoint_id);
	}
}

static inline int cisco_huntgroup_is_in(const char *endpoint_id)
{
	char value[16];

	if (ast_strlen_zero(endpoint_id)) {
		return 0;
	}
	if (ast_db_get("HuntGroup", endpoint_id, value, sizeof(value))) {
		return 0;
	}
	return ast_true(value);
}

static inline void cisco_huntgroup_set(const char *endpoint_id, int enabled)
{
	/* See cisco_dnd_set re: no NULL guard. */
	if (enabled) {
		ast_db_put("HuntGroup", endpoint_id, "YES");
	} else {
		ast_db_del("HuntGroup", endpoint_id);
	}
}

/*!
 * \brief Read the call-forward-all target into \a buf (empty when unset).
 * \return the (NUL-terminated) buf pointer, never NULL.
 */
static inline const char *cisco_cfwd_get(const char *endpoint_id,
	char *buf, size_t buflen)
{
	if (!buf || buflen == 0) {
		return "";
	}
	buf[0] = '\0';
	if (ast_strlen_zero(endpoint_id)) {
		return buf;
	}
	if (ast_db_get("CF", endpoint_id, buf, buflen)) {
		buf[0] = '\0';
	}
	return buf;
}

static inline void cisco_cfwd_set(const char *endpoint_id, const char *target)
{
	/* See cisco_dnd_set re: no NULL guard on endpoint_id. */
	if (ast_strlen_zero(target)) {
		ast_db_del("CF", endpoint_id);
	} else {
		ast_db_put("CF", endpoint_id, target);
	}
}
/* @} */

#endif /* _RES_PJSIP_CISCO_ENDPOINT_H */
