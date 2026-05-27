/*
 * Asterisk -- An open source telephony toolkit.
 *
 * REGISTER 200-OK address-change tracking for the res_pjsip_cisco_*
 * supplements (optionsind / bulkupdate / unsolicited_blf).
 *
 * Each REGISTER supplement keeps a per-module ao2 hash of "endpoint id
 * -> set of Contact URIs we've successfully fired follow-up traffic to".
 * Refresh REGISTERs (carrying the same Contact set every ~60s) become
 * no-ops, AND when a new contact joins an endpoint that already has one
 * registered, only the new contact gets the bootstrap REFER/NOTIFY —
 * the already-bootstrapped contact is not re-pushed. Mirrors (with the
 * per-contact refinement) the chan_sip cisco-usecallmanager patch's
 * addrchanged guard in parse_register_contact.
 *
 * Bodies live in res/cisco_endpoint/register.c, compiled into
 * res_pjsip_cisco_endpoint.so; other cisco_* modules resolve the
 * symbols at load time.
 *
 * Depends on cisco/rdata.h (and through it cisco/endpoint.h):
 * cisco_register_should_fire calls cisco_endpoint_get to gate on the
 * Cisco flag, and the helpers here are shaped around pjsip_msg /
 * pjsip_tx_data plumbing exposed by the response hook.
 */

#ifndef _RES_PJSIP_CISCO_REGISTER_H
#define _RES_PJSIP_CISCO_REGISTER_H

#include "asterisk.h"

#include <pjsip.h>

#include "asterisk/res_pjsip.h"
#include "asterisk/strings.h"

#include "cisco/endpoint.h"
#include "cisco/rdata.h"

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
int cisco_response_registers_contact(pjsip_msg *msg);

/*!
 * \name Address-change cache for REGISTER supplements
 *
 * The three REGISTER supplements (optionsind / bulkupdate /
 * unsolicited_blf) only need to fire follow-up traffic at a contact
 * the first time it appears in the endpoint's registered Contact set.
 * Refresh REGISTERs every ~60s carry the same Contact set; a second
 * device registering against the same endpoint (e.g. home phone joining
 * an office phone) adds one URI without changing the existing one.
 *
 * Cache holds, per endpoint, the SET of Contact URIs to which the
 * supplement has successfully delivered its follow-up traffic. The
 * helpers below let a caller answer two questions cheaply:
 *
 *   1. Has the canonical Contact set changed at all (boolean)? — used
 *      by the synchronous optionsind path where the supplement writes
 *      a body onto the 200 OK itself, not per-contact follow-up.
 *   2. Which Contact URIs are new since last fire (URI list)? — used by
 *      the async bulkupdate / unsolicited_blf paths that fan out one
 *      REFER / NOTIFY per registered contact.
 *
 * Cache is per-module, in-memory (NOT persistent astdb): asterisk
 * restart, module unload, and module reload all dump the cache,
 * guaranteeing the next REGISTER from each phone is correctly treated
 * as "everything is new" and re-bootstraps. The chan_sip patch gets
 * this for free because its peer->addr is in-memory; we have to be
 * explicit because the Cisco endpoint registry on PJSIP is sorcery-
 * backed and survives reloads.
 *
 * Commit policy:
 *   - optionsind (sync): on success, cisco_register_address_remember()
 *     stores the whole current Contact set. Failure leaves the cache
 *     untouched and the next refresh REGISTER retries.
 *   - bulkupdate / unsolicited_blf (async): on per-contact success,
 *     cisco_register_address_remember_uri() adds that one URI. Failure
 *     for one contact leaves the others marked as fired and the failed
 *     one untouched — the next refresh REGISTER finds it in the delta
 *     and retries just that contact. Strictly better than the prior
 *     all-or-nothing commit policy, which re-fanned-out every contact
 *     on any partial failure.
 *
 * Lifecycle:
 *   cisco_register_address_changed()      - read-only "anything new?"
 *   cisco_register_new_contacts()         - delta list (caller frees)
 *   cisco_register_address_remember()     - sync: commit whole set
 *   cisco_register_address_remember_uri() - async: commit one URI
 *   cisco_register_address_forget()       - clear cache for an endpoint
 *                                           on its deregister, so the
 *                                           next re-register (even with
 *                                           the same Contact URI)
 *                                           re-bootstraps
 *
 * Each supplement creates its own ao2 container via
 * cisco_addr_cache_alloc() at load_module time and ao2_cleanup()s it
 * at unload — keeping the three modules' caches independent so each
 * supplement's success/failure doesn't suppress the others.
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
	/* NULL-terminated array of heap-allocated URI strings, kept sorted
	 * by strcmp() so set diff is a linear merge walk. May be NULL when
	 * the entry was just created and no URI has been remembered yet. */
	char **contacts;
};

struct ao2_container *cisco_addr_cache_alloc(void);

/*!
 * \brief Free a NULL-terminated array of heap-allocated URI strings as
 *        returned by cisco_register_new_contacts(). Safe to call on NULL.
 */
void cisco_uri_list_free(char **uris);

/*!
 * \brief Render the canonical contact-set string for the given REGISTER
 *        200 OK into a dynamic ast_str. Iterates every Contact header
 *        with expires > 0 (skips deregister rows). Caller frees with
 *        ast_free().
 *
 * Retained for callers that want a single-string representation of the
 * 200 OK's view (e.g. for logging). The cache itself reads URIs from
 * the AOR, not this msg-based string, so these two sources may differ
 * across Asterisk versions in URI parameter ordering / escaping — the
 * cache deliberately commits to the AOR form because that's what the
 * fanout consumers (REFER/NOTIFY iterators) work with too.
 *
 * \retval NULL on alloc failure, no contacts, or Contact: * (deregister)
 */
struct ast_str *cisco_response_contacts_canonical(pjsip_msg *msg);

/*!
 * \brief Read-only: does the current AOR Contact set differ from what
 *        \a cache last remembered for this endpoint?
 *
 * Returns 1 (changed, caller should fire) when any URI in the current
 * AOR contact set is missing from the cached set OR vice versa.
 * Returns 0 only when the two sets are equal. Does NOT update the cache.
 *
 * Use for the synchronous optionsind path where the supplement writes
 * one body onto the 200 OK and doesn't fan out per-contact. Async
 * callers should use cisco_register_new_contacts() to get the delta
 * directly.
 */
int cisco_register_address_changed(struct ast_sip_endpoint *endpoint,
	struct ao2_container *cache);

/*!
 * \brief Compute the list of AOR Contact URIs that are NOT yet in
 *        \a cache for this endpoint.
 *
 * Returns a NULL-terminated array of heap-allocated URI strings, sorted
 * by strcmp() order. Caller frees with cisco_uri_list_free(). On no-new-
 * contacts the function returns NULL (treat as empty list, not error).
 *
 * URIs are sourced from ast_sip_location_retrieve_contacts_from_aor_list,
 * matching the source the bulkupdate / unsolicited_blf fanout consumers
 * use — this keeps the URI strings byte-identical across the gate and
 * the iterator regardless of how the response hook's 200 OK Contact
 * was formatted.
 *
 * Cache is NOT mutated. Caller commits via
 * cisco_register_address_remember_uri() per URI AFTER the async work
 * for that URI has actually succeeded.
 */
char **cisco_register_new_contacts(struct ast_sip_endpoint *endpoint,
	struct ao2_container *cache);

/*!
 * \brief Persist the entire current AOR Contact set as "last fired".
 *
 * Use for synchronous callers (optionsind) that succeed atomically over
 * the whole REGISTER response.
 */
void cisco_register_address_remember(struct ast_sip_endpoint *endpoint,
	struct ao2_container *cache);

/*!
 * \brief Add a single URI to the per-endpoint cached set.
 *
 * Idempotent: re-adding a URI already in the set is a no-op. Use from
 * async per-contact success paths (bulkupdate / unsolicited_blf).
 */
void cisco_register_address_remember_uri(const char *endpoint_id,
	struct ao2_container *cache, const char *uri);

/*!
 * \brief Forget the cached Contact set for an endpoint. Call when the
 *        endpoint deregisters so the next REGISTER (even with the
 *        same Contact URI) re-bootstraps optionsind / bulkupdate /
 *        unsolicited_blf for the fresh session.
 */
void cisco_register_address_forget(const char *endpoint_id,
	struct ao2_container *cache);

/*!
 * \brief Combined gate for REGISTER outgoing_response supplements.
 *
 * Encapsulates the four-step preamble open-coded by every REGISTER
 * supplement (optionsind / bulkupdate / unsolicited_blf):
 *
 *   1. Is it a 200 OK to a REGISTER?
 *   2. Is the endpoint flagged Cisco?
 *   3. Is it a deregister? If so, forget() the cache and skip.
 *   4. Has the canonical Contact set added any URI since last fire? If
 *      not, skip.
 *   5. (optional) Capture the list of new URIs for async paths that
 *      stash it in task data and commit per-URI after each successful
 *      send.
 *
 * \param endpoint        the supplement's endpoint argument
 * \param tdata           the supplement's tdata argument
 * \param addr_cache      the per-supplement address-change cache
 * \param endpoint_id_out written on return-1; lifetime bound by \a endpoint
 * \param new_contacts_out if non-NULL on entry, the new-URI list is captured
 *                        and heap-allocated here (caller frees with
 *                        cisco_uri_list_free). Pass NULL for the synchronous
 *                        optionsind path that doesn't need it.
 * \retval 1 caller should proceed with the supplement's body of work
 * \retval 0 caller should bail; the cache lifecycle (forget on dereg,
 *           skip on unchanged) has already been handled internally
 */
int cisco_register_should_fire(struct ast_sip_endpoint *endpoint,
	pjsip_tx_data *tdata, struct ao2_container *addr_cache,
	const char **endpoint_id_out, char ***new_contacts_out);
/* @} */

#endif /* _RES_PJSIP_CISCO_REGISTER_H */
