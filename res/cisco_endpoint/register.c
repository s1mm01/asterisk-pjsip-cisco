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
 * Each REGISTER supplement keeps a per-module ao2 hash of "endpoint id
 * -> canonical-Contact-set last fired" so refresh REGISTERs (which
 * carry the same Contact every ~60s) become no-ops. Mirrors the
 * chan_sip cisco-usecallmanager patch's addrchanged guard in
 * parse_register_contact.
 */

#include "asterisk.h"

#include <pjsip.h>

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

struct ao2_container *cisco_addr_cache_alloc(void)
{
	return ao2_container_alloc_hash(AO2_ALLOC_OPT_LOCK_MUTEX, 0, 31,
		cisco_addr_cache_hash, NULL, cisco_addr_cache_cmp);
}

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

int cisco_register_address_changed(pjsip_msg *msg,
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

void cisco_register_address_remember_str(const char *endpoint_id,
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

void cisco_register_address_remember(pjsip_msg *msg,
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
