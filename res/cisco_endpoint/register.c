/*
 * Asterisk -- An open source telephony toolkit.
 *
 * REGISTER 200-OK address-change tracking for the res_pjsip_cisco_*
 * supplements (optionsind / bulkupdate / unsolicited_blf).
 *
 * Bodies for the declarations in cisco/register.h. Linked into
 * res_pjsip_cisco_endpoint.so; other cisco_* modules resolve the
 * symbols at load time via the dynamic symbol table.
 *
 * The cache stores, per endpoint, the SET of Contact URIs to which the
 * supplement has successfully delivered its follow-up traffic. New
 * contacts (URIs not in the set) are reported as the delta on the next
 * REGISTER 200 OK; refresh REGISTERs with the same set become no-ops.
 * Mirrors (with the per-contact refinement) the chan_sip cisco-
 * usecallmanager patch's addrchanged guard in parse_register_contact.
 */

#include "asterisk.h"

#include <pjsip.h>
#include <string.h>
#include <stdlib.h>

#include "asterisk/astobj2.h"
#include "asterisk/logger.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"

#include "cisco/endpoint.h"
#include "cisco/register.h"

int cisco_response_registers_contact(pjsip_msg *msg)
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

/* ---------------------------------------------------------------------
 * URI list helpers (NULL-terminated char**, sorted ascending by strcmp)
 * ------------------------------------------------------------------ */

void cisco_uri_list_free(char **uris)
{
	char **p;

	if (!uris) {
		return;
	}
	for (p = uris; *p; p++) {
		ast_free(*p);
	}
	ast_free(uris);
}

static int uri_cmp_qsort(const void *a, const void *b)
{
	return strcmp(*(const char **) a, *(const char **) b);
}

/*!
 * \brief Read the endpoint's currently-registered Contact URIs from the
 *        AOR into a sorted NULL-terminated char** of heap-allocated URI
 *        strings. Caller frees with cisco_uri_list_free.
 *
 * Sourcing from the AOR (rather than parsing the outgoing 200 OK
 * Contact header) is deliberate: the bulkupdate / unsolicited_blf
 * fanout consumers iterate ast_sip_contact* from the same AOR, and
 * compare contact->uri against the strings the cache returned. Pulling
 * from a different source — even though "the same" URI logically —
 * means a different URI encoding (param order, port presence, instance
 * params) and the strcmp inside the fanout helper misses every
 * contact, silently dropping every REFER / NOTIFY. Observed across
 * Asterisk 20 vs 22/23 in CI: same module .so, different URI strings
 * out of pjsip_uri_print(PJSIP_URI_IN_CONTACT_HDR, ...) versus
 * ast_sip_contact.uri, depending on the version's NAT / contact-
 * rewriting plumbing.
 *
 * Returns NULL on alloc failure or no contacts.
 */
static char **collect_aor_contacts(struct ast_sip_endpoint *endpoint)
{
	struct ao2_container *contacts;
	struct ao2_iterator iter;
	struct ast_sip_contact *contact;
	char **uris = NULL;
	size_t n = 0;
	size_t cap = 0;

	if (!endpoint || ast_strlen_zero(endpoint->aors)) {
		return NULL;
	}

	contacts = ast_sip_location_retrieve_contacts_from_aor_list(endpoint->aors);
	if (!contacts) {
		return NULL;
	}

	iter = ao2_iterator_init(contacts, 0);
	while ((contact = ao2_iterator_next(&iter))) {
		if (!ast_strlen_zero(contact->uri)) {
			char *copy;

			if (n + 1 >= cap) {
				char **bigger;
				size_t newcap = cap ? cap * 2 : 4;

				bigger = ast_realloc(uris, newcap * sizeof(*uris));
				if (!bigger) {
					cisco_uri_list_free(uris);
					ao2_cleanup(contact);
					ao2_iterator_destroy(&iter);
					ao2_cleanup(contacts);
					return NULL;
				}
				uris = bigger;
				cap = newcap;
			}
			copy = ast_strdup(contact->uri);
			if (!copy) {
				cisco_uri_list_free(uris);
				ao2_cleanup(contact);
				ao2_iterator_destroy(&iter);
				ao2_cleanup(contacts);
				return NULL;
			}
			uris[n++] = copy;
		}
		ao2_cleanup(contact);
	}
	ao2_iterator_destroy(&iter);
	ao2_cleanup(contacts);

	if (n == 0) {
		ast_free(uris);
		return NULL;
	}

	uris[n] = NULL;
	qsort(uris, n, sizeof(*uris), uri_cmp_qsort);
	return uris;
}

static size_t uri_list_len(char **uris)
{
	size_t n = 0;
	if (!uris) {
		return 0;
	}
	while (uris[n]) {
		n++;
	}
	return n;
}

/*!
 * \brief Sets-equal predicate for two sorted NULL-terminated URI lists.
 */
static int uri_lists_equal(char **a, char **b)
{
	size_t na = uri_list_len(a);
	size_t nb = uri_list_len(b);
	size_t i;

	if (na != nb) {
		return 0;
	}
	for (i = 0; i < na; i++) {
		if (strcmp(a[i], b[i]) != 0) {
			return 0;
		}
	}
	return 1;
}

/*!
 * \brief Compute current \ cached as a fresh heap-allocated sorted
 *        NULL-terminated URI list. Caller frees with cisco_uri_list_free.
 *        Returns NULL if every URI in current is also in cached (empty
 *        delta — not an error).
 */
static char **uri_list_diff(char **current, char **cached)
{
	size_t nc = uri_list_len(current);
	size_t nh = uri_list_len(cached);
	char **out;
	size_t ci = 0, hi = 0, oi = 0;

	if (nc == 0) {
		return NULL;
	}
	if (nh == 0) {
		/* All of current is new — duplicate the array. */
		out = ast_calloc(nc + 1, sizeof(*out));
		if (!out) {
			return NULL;
		}
		while (ci < nc) {
			out[oi] = ast_strdup(current[ci]);
			if (!out[oi]) {
				cisco_uri_list_free(out);
				return NULL;
			}
			oi++;
			ci++;
		}
		return out;
	}

	out = ast_calloc(nc + 1, sizeof(*out));
	if (!out) {
		return NULL;
	}
	while (ci < nc && hi < nh) {
		int c = strcmp(current[ci], cached[hi]);
		if (c == 0) {
			ci++;
			hi++;
		} else if (c < 0) {
			out[oi] = ast_strdup(current[ci]);
			if (!out[oi]) {
				cisco_uri_list_free(out);
				return NULL;
			}
			oi++;
			ci++;
		} else {
			hi++;
		}
	}
	while (ci < nc) {
		out[oi] = ast_strdup(current[ci]);
		if (!out[oi]) {
			cisco_uri_list_free(out);
			return NULL;
		}
		oi++;
		ci++;
	}

	if (oi == 0) {
		ast_free(out);
		return NULL;
	}
	return out;
}

/* ---------------------------------------------------------------------
 * ao2 cache plumbing
 * ------------------------------------------------------------------ */

static void cisco_addr_cache_entry_destroy(void *obj)
{
	struct cisco_addr_cache_entry *e = obj;
	ast_free(e->endpoint_id);
	cisco_uri_list_free(e->contacts);
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

struct ao2_container *cisco_addr_cache_alloc(void)
{
	return ao2_container_alloc_hash(AO2_ALLOC_OPT_LOCK_MUTEX, 0, 31,
		cisco_addr_cache_hash, NULL, cisco_addr_cache_cmp);
}

/* ---------------------------------------------------------------------
 * Public helpers
 * ------------------------------------------------------------------ */

struct ast_str *cisco_response_contacts_canonical(pjsip_msg *msg)
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

int cisco_register_address_changed(struct ast_sip_endpoint *endpoint,
	struct ao2_container *cache)
{
	struct cisco_addr_cache_entry *entry;
	char **current;
	const char *endpoint_id;
	int changed = 1;

	if (!endpoint || !cache) {
		return 1;
	}
	endpoint_id = ast_sorcery_object_get_id(endpoint);
	if (ast_strlen_zero(endpoint_id)) {
		return 1;
	}

	current = collect_aor_contacts(endpoint);
	if (!current) {
		return 1;
	}

	entry = ao2_find(cache, endpoint_id, OBJ_SEARCH_KEY);
	if (entry) {
		if (uri_lists_equal(current, entry->contacts)) {
			changed = 0;
		}
		ao2_cleanup(entry);
	}

	cisco_uri_list_free(current);
	return changed;
}

char **cisco_register_new_contacts(struct ast_sip_endpoint *endpoint,
	struct ao2_container *cache)
{
	struct cisco_addr_cache_entry *entry;
	char **current;
	char **delta;
	const char *endpoint_id;

	if (!endpoint || !cache) {
		return NULL;
	}
	endpoint_id = ast_sorcery_object_get_id(endpoint);
	if (ast_strlen_zero(endpoint_id)) {
		return NULL;
	}

	current = collect_aor_contacts(endpoint);
	if (!current) {
		return NULL;
	}

	entry = ao2_find(cache, endpoint_id, OBJ_SEARCH_KEY);
	if (!entry) {
		/* Whole current set is new. */
		return current;
	}

	delta = uri_list_diff(current, entry->contacts);
	cisco_uri_list_free(current);
	ao2_cleanup(entry);
	return delta;
}

void cisco_register_address_remember(struct ast_sip_endpoint *endpoint,
	struct ao2_container *cache)
{
	struct cisco_addr_cache_entry *entry;
	char **current;
	const char *endpoint_id;

	if (!endpoint || !cache) {
		return;
	}
	endpoint_id = ast_sorcery_object_get_id(endpoint);
	if (ast_strlen_zero(endpoint_id)) {
		return;
	}
	current = collect_aor_contacts(endpoint);
	if (!current) {
		return;
	}

	ao2_find(cache, endpoint_id,
		OBJ_SEARCH_KEY | OBJ_UNLINK | OBJ_NODATA);

	entry = ao2_alloc(sizeof(*entry), cisco_addr_cache_entry_destroy);
	if (!entry) {
		cisco_uri_list_free(current);
		return;
	}
	entry->endpoint_id = ast_strdup(endpoint_id);
	entry->contacts    = current;       /* take ownership */
	if (!entry->endpoint_id) {
		ao2_cleanup(entry);
		return;
	}
	ao2_link(cache, entry);
	ao2_cleanup(entry);
}

void cisco_register_address_remember_uri(const char *endpoint_id,
	struct ao2_container *cache, const char *uri)
{
	struct cisco_addr_cache_entry *entry;
	char **bigger;
	size_t n;
	size_t i;
	int insert_at;

	if (!cache || ast_strlen_zero(endpoint_id) || ast_strlen_zero(uri)) {
		return;
	}

	entry = ao2_find(cache, endpoint_id, OBJ_SEARCH_KEY);
	if (!entry) {
		/* First URI for this endpoint — create the entry. */
		entry = ao2_alloc(sizeof(*entry), cisco_addr_cache_entry_destroy);
		if (!entry) {
			return;
		}
		entry->endpoint_id = ast_strdup(endpoint_id);
		entry->contacts    = ast_calloc(2, sizeof(*entry->contacts));
		if (!entry->endpoint_id || !entry->contacts) {
			ao2_cleanup(entry);
			return;
		}
		entry->contacts[0] = ast_strdup(uri);
		entry->contacts[1] = NULL;
		if (!entry->contacts[0]) {
			ao2_cleanup(entry);
			return;
		}
		ao2_link(cache, entry);
		ao2_cleanup(entry);
		return;
	}

	/* Idempotent on existing URI; sorted insert otherwise. */
	ao2_lock(entry);
	n = uri_list_len(entry->contacts);
	insert_at = (int) n;
	for (i = 0; i < n; i++) {
		int c = strcmp(entry->contacts[i], uri);
		if (c == 0) {
			ao2_unlock(entry);
			ao2_cleanup(entry);
			return;
		}
		if (c > 0) {
			insert_at = (int) i;
			break;
		}
	}

	bigger = ast_realloc(entry->contacts, (n + 2) * sizeof(*bigger));
	if (!bigger) {
		ao2_unlock(entry);
		ao2_cleanup(entry);
		return;
	}
	entry->contacts = bigger;
	for (i = n; i > (size_t) insert_at; i--) {
		entry->contacts[i] = entry->contacts[i - 1];
	}
	entry->contacts[insert_at] = ast_strdup(uri);
	entry->contacts[n + 1] = NULL;
	if (!entry->contacts[insert_at]) {
		/* Restore the array shape so we don't leave a hole. */
		for (i = (size_t) insert_at; i < n; i++) {
			entry->contacts[i] = entry->contacts[i + 1];
		}
		entry->contacts[n] = NULL;
	}
	ao2_unlock(entry);
	ao2_cleanup(entry);
}

void cisco_register_address_forget(const char *endpoint_id,
	struct ao2_container *cache)
{
	if (!cache || ast_strlen_zero(endpoint_id)) {
		return;
	}
	ao2_find(cache, endpoint_id,
		OBJ_SEARCH_KEY | OBJ_UNLINK | OBJ_NODATA);
}

int cisco_register_should_fire(struct ast_sip_endpoint *endpoint,
	pjsip_tx_data *tdata, struct ao2_container *addr_cache,
	const char **endpoint_id_out, char ***new_contacts_out)
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
	 * the supplement's work unless something new appeared. Mirrors
	 * the chan_sip patch's addrchanged guard, refined to per-contact
	 * granularity. Reads URIs from the AOR (matching the source used
	 * by the fanout consumers below) — see collect_aor_contacts for
	 * why we deliberately avoid parsing tdata->msg here. */
	if (!cisco_register_address_changed(endpoint, addr_cache)) {
		return 0;
	}

	if (new_contacts_out) {
		*new_contacts_out = cisco_register_new_contacts(endpoint,
			addr_cache);
		if (!*new_contacts_out) {
			/* address_changed returned 1 but the delta is empty —
			 * possible only if a URI was removed without a deregister
			 * (e.g. a contact expired). No new contacts to push at;
			 * skip the fanout. */
			return 0;
		}
	}

	if (endpoint_id_out) {
		*endpoint_id_out = endpoint_id;
	}
	return 1;
}
