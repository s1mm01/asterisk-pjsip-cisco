/*
 * Per-Cisco-device runtime facts container. Shared by:
 *
 *   - res_pjsip_cisco_feature_events: REGISTER-time MAC harvest +
 *     Reason-header parse populate the container.
 *   - res_pjsip_cisco_feature_events: cisco_mac_identify (PATH C)
 *     looks up by MAC to resolve REFER/PUBLISH with a sip:<mac>@...
 *     From-URI back to a Cisco endpoint.
 *   - res_pjsip_cisco_service_control: 'pjsip cisco status' looks up
 *     by endpoint id to print Device Name / Active Load / Inactive
 *     Load alongside the rest of the diagnostic dump.
 *
 * The map lives in res_pjsip_cisco_endpoint.so (this file is compiled
 * into that .so) so its symbols are reachable from any consumer via
 * the cisco_* export glob. A previous version of this code kept the
 * map private to feature_events; lifting it here is what 'pjsip cisco
 * status' needed to surface Cisco firmware versions without breaking
 * the "endpoint is the shared-helpers library" pattern.
 *
 * Keying: by MAC (12 lowercase hex chars). PATH C lookup is O(1).
 * The lookup-by-endpoint helper is a linear walk — fine for a CLI
 * verb, never used on the data path.
 *
 * Lifecycle: the container itself is allocated by
 * cisco_mac_container_init() called from res_pjsip_cisco_endpoint's
 * load_module; freed by cisco_mac_container_shutdown() from
 * unload_module. Both functions are visible only inside this .so —
 * see cisco/endpoint.h for the public surface consumers actually use.
 */

#include "asterisk.h"

#include "asterisk/astobj2.h"
#include "asterisk/strings.h"
#include "asterisk/time.h"
#include "asterisk/utils.h"

#include "cisco/endpoint.h"

/* Hash-bucket count. 13 mirrors the prior cisco_mac_map size that
 * lived in feature_events_mac.c (small fleet of ≤ ~50 phones; the
 * map is rebuilt on REGISTER, no need for many buckets). */
#define CISCO_MAC_BUCKETS 13

static struct ao2_container *cisco_mac_map;

static int cisco_mac_hash_fn(const void *obj, int flags)
{
	const struct cisco_mac_info *entry = obj;
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
	const struct cisco_mac_info *left = obj;
	const char *right_key = arg;

	switch (flags & OBJ_SEARCH_MASK) {
	case OBJ_SEARCH_OBJECT:
		right_key = ((struct cisco_mac_info *) arg)->mac;
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

int cisco_mac_register(const struct cisco_mac_info *info)
{
	struct cisco_mac_info *entry;

	if (!cisco_mac_map || !info || ast_strlen_zero(info->mac)) {
		return -1;
	}

	entry = ao2_alloc_options(sizeof(*entry), NULL, AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!entry) {
		return -1;
	}
	*entry = *info;

	/* Replace any prior entry for this MAC (re-REGISTER, possibly with
	 * a new endpoint_id under a multi-line phone, or a new firmware
	 * version). */
	ao2_find(cisco_mac_map, entry->mac,
		OBJ_SEARCH_KEY | OBJ_UNLINK | OBJ_NODATA);
	ao2_link(cisco_mac_map, entry);
	ao2_ref(entry, -1);
	return 0;
}

void cisco_mac_forget(const char *mac)
{
	if (!cisco_mac_map || ast_strlen_zero(mac)) {
		return;
	}
	ao2_find(cisco_mac_map, mac,
		OBJ_SEARCH_KEY | OBJ_UNLINK | OBJ_NODATA);
}

int cisco_mac_lookup_by_mac(const char *mac, struct cisco_mac_info *out)
{
	struct cisco_mac_info *entry;

	if (!cisco_mac_map || ast_strlen_zero(mac) || !out) {
		return -1;
	}
	entry = ao2_find(cisco_mac_map, mac, OBJ_SEARCH_KEY);
	if (!entry) {
		return -1;
	}
	if (ast_tvdiff_ms(entry->expires, ast_tvnow()) <= 0) {
		/* Expired — unlink so future lookups stay consistent. */
		ao2_unlink(cisco_mac_map, entry);
		ao2_ref(entry, -1);
		return -1;
	}
	*out = *entry;
	ao2_ref(entry, -1);
	return 0;
}

/* Linear-walk predicate for cisco_mac_lookup_by_endpoint: returns
 * CMP_MATCH on the first LIVE entry whose endpoint_id equals \a arg.
 *
 * Expired entries are skipped (return 0) so the walk continues past
 * them — a multi-MAC endpoint (e.g. phone replacement before the old
 * MAC's TTL elapsed, or two-phone shared line with max_contacts>1
 * where the older REGISTER's entry hasn't aged out yet) can have an
 * expired entry sitting at a lower hash-bucket position than the
 * live one. Returning STOP on the expired hit would falsely report
 * "no entry" to the caller. We don't unlink the expired entry inline
 * — ao2_callback forbids container mutation during the walk — but
 * the regular by-MAC path (lookup_by_mac, register-time replace) will
 * collect it on next contact. */
static int cisco_mac_endpoint_match(void *obj, void *arg, int flags)
{
	const struct cisco_mac_info *entry = obj;
	const char *want = arg;

	(void) flags;
	if (strcmp(entry->endpoint_id, want)) {
		return 0;
	}
	if (ast_tvdiff_ms(entry->expires, ast_tvnow()) <= 0) {
		return 0;        /* expired — keep walking */
	}
	return CMP_MATCH | CMP_STOP;
}

int cisco_mac_lookup_by_endpoint(const char *endpoint_id,
	struct cisco_mac_info *out)
{
	struct cisco_mac_info *entry;
	struct cisco_endpoint *cisco;

	if (!cisco_mac_map || ast_strlen_zero(endpoint_id) || !out) {
		return -1;
	}

	/* Direct match: this endpoint is what we stored at REGISTER. */
	entry = ao2_callback(cisco_mac_map, 0, cisco_mac_endpoint_match,
		(void *) endpoint_id);
	if (entry) {
		*out = *entry;
		ao2_ref(entry, -1);
		return 0;
	}

	/* Alias fallback: this endpoint is the primary on a multi-line
	 * phone; one of its aliases REGISTERed most recently and won the
	 * MAC-keyed slot. The physical device is the same, so its facts
	 * (MAC, src_host, device_name, firmware) are correct to return —
	 * the stored endpoint_id field will name the alias rather than
	 * the queried primary, which the caller can mention or ignore.
	 *
	 * Limit to one direction: queried endpoint X is primary, walk X's
	 * aliases=. We don't walk the reverse direction (X is itself an
	 * alias of some primary Y) because that would need an O(N_endpoints)
	 * scan of every cisco object's aliases field per status lookup; the
	 * operator-facing workaround is "run status on the primary" and is
	 * documented in the CLI usage. */
	cisco = cisco_endpoint_get(endpoint_id);
	if (cisco && !ast_strlen_zero(cisco->aliases)) {
		char *aliases = ast_strdupa(cisco->aliases);
		char *alias;

		while ((alias = ast_strip(strsep(&aliases, ",")))) {
			if (ast_strlen_zero(alias)) {
				continue;
			}
			entry = ao2_callback(cisco_mac_map, 0,
				cisco_mac_endpoint_match, alias);
			if (entry) {
				*out = *entry;
				ao2_ref(entry, -1);
				ao2_cleanup(cisco);
				return 0;
			}
		}
	}
	ao2_cleanup(cisco);
	return -1;
}

static int cisco_mac_call_id_match(void *obj, void *arg, int flags)
{
	const struct cisco_mac_info *entry = obj;
	const char *want = arg;

	(void) flags;
	if (strcmp(entry->call_id, want)) {
		return 0;
	}
	if (ast_tvdiff_ms(entry->expires, ast_tvnow()) <= 0) {
		return 0;        /* expired — keep walking */
	}
	return CMP_MATCH | CMP_STOP;
}

int cisco_mac_lookup_by_call_id(const char *call_id,
	struct cisco_mac_info *out)
{
	struct cisco_mac_info *entry;

	if (!cisco_mac_map || ast_strlen_zero(call_id) || !out) {
		return -1;
	}
	entry = ao2_callback(cisco_mac_map, 0, cisco_mac_call_id_match,
		(void *) call_id);
	if (!entry) {
		return -1;
	}
	*out = *entry;
	ao2_ref(entry, -1);
	return 0;
}

/* Internal — called from res_pjsip_cisco_endpoint.c's load_module. */
int cisco_mac_container_init(void);
int cisco_mac_container_init(void)
{
	cisco_mac_map = ao2_container_alloc_hash(AO2_ALLOC_OPT_LOCK_MUTEX, 0,
		CISCO_MAC_BUCKETS, cisco_mac_hash_fn, NULL, cisco_mac_cmp_fn);
	return cisco_mac_map ? 0 : -1;
}

void cisco_mac_container_shutdown(void);
void cisco_mac_container_shutdown(void)
{
	ao2_cleanup(cisco_mac_map);
	cisco_mac_map = NULL;
}
