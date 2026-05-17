/*
 * Asterisk -- An open source telephony toolkit.
 *
 * REGISTER 200-OK address-change tracking for the res_pjsip_cisco_*
 * supplements (optionsind / bulkupdate / unsolicited_blf).
 *
 * Each REGISTER supplement keeps a per-module ao2 hash of "endpoint id
 * -> canonical-Contact-set last fired" so refresh REGISTERs (which
 * carry the same Contact every ~60s) become no-ops. Mirrors the
 * chan_sip cisco-usecallmanager patch's addrchanged guard in
 * parse_register_contact — see the section comment on
 * cisco_register_address_changed below for the lifecycle.
 *
 * Bodies live in res/res/cisco_endpoint/register.c, compiled into
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

struct ao2_container *cisco_addr_cache_alloc(void);

/*!
 * \brief Render the canonical contact-set string for the given REGISTER
 *        200 OK into a dynamic ast_str. Iterates every Contact header
 *        with expires > 0 (skips deregister rows). Caller frees with
 *        ast_free().
 *
 * \retval NULL on alloc failure, no contacts, or Contact: * (deregister)
 */
struct ast_str *cisco_response_contacts_canonical(pjsip_msg *msg);

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
int cisco_register_address_changed(pjsip_msg *msg,
	const char *endpoint_id, struct ao2_container *cache);

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
void cisco_register_address_remember_str(const char *endpoint_id,
	struct ao2_container *cache, const char *canonical);

/*!
 * \brief Convenience: compute the canonical Contact string from \a msg
 *        and remember it. Use for synchronous flows (optionsind) where
 *        the guarded operation completes before this call returns.
 */
void cisco_register_address_remember(pjsip_msg *msg,
	const char *endpoint_id, struct ao2_container *cache);

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
int cisco_register_should_fire(struct ast_sip_endpoint *endpoint,
	pjsip_tx_data *tdata, struct ao2_container *addr_cache,
	const char **endpoint_id_out, char **canonical_out);
/* @} */

#endif /* _RES_PJSIP_CISCO_REGISTER_H */
