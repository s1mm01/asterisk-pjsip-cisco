/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Foundation helpers for the res_pjsip_cisco_* family of modules.
 *
 * Implements the cisco_endpoint_get accessor and the astdb-backed
 * feature-state accessors (DND, HuntGroup, call-forward-all) declared
 * in cisco/endpoint.h. Linked into res_pjsip_cisco_endpoint.so; the
 * other nine cisco_* modules pick the symbols up via the dynamic
 * symbol table once res_pjsip_cisco_endpoint.so is loaded with
 * AST_MODFLAG_GLOBAL_SYMBOLS (the same pattern stock res_pjsip uses).
 */

#include "asterisk.h"

#include "asterisk/app.h"
#include "asterisk/astdb.h"
#include "asterisk/astobj2.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"
#include "asterisk/stringfields.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/presencestate.h"

#include "cisco/endpoint.h"

struct cisco_endpoint *cisco_endpoint_get(const char *id)
{
	if (ast_strlen_zero(id)) {
		return NULL;
	}
	return ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(), "cisco", id);
}

int cisco_dnd_is_enabled(const char *endpoint_id)
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

void cisco_dnd_set(const char *endpoint_id, int enabled)
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

	/* Surface the change to BLF watchers. A hint of the form
	 *   exten => N,hint,PJSIP/N,PJSIP:N
	 * picks this up through the "PJSIP" presence-state provider
	 * res_pjsip_cisco_endpoint registers (see that module), and
	 * res_pjsip_exten_state then NOTIFYs the watching phones — whose
	 * Cisco firmware renders <ce:dnd/> as a red lamp. Mirrors the
	 * chan_sip cisco-usecallmanager patch, which fired
	 * ast_presence_state_changed from sip_handle_publish_presence and
	 * the `sip donotdisturb` CLI. DND off reports NOT_SET rather than
	 * AVAILABLE so this provider only ever adds the DND signal and never
	 * masks another presence source '&'-combined into the same hint. */
	ast_presence_state_changed(enabled ? AST_PRESENCE_DND : AST_PRESENCE_NOT_SET,
		NULL, NULL, "PJSIP:%s", endpoint_id);
}

int cisco_huntgroup_is_in(const char *endpoint_id)
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

void cisco_huntgroup_set(const char *endpoint_id, int enabled)
{
	/* See cisco_dnd_set re: no NULL guard. */
	if (enabled) {
		ast_db_put("HuntGroup", endpoint_id, "YES");
	} else {
		ast_db_del("HuntGroup", endpoint_id);
	}
}

const char *cisco_cfwd_get(const char *endpoint_id, char *buf, size_t buflen)
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

void cisco_cfwd_set(const char *endpoint_id, const char *target)
{
	/* See cisco_dnd_set re: no NULL guard on endpoint_id. */
	if (ast_strlen_zero(target)) {
		ast_db_del("CF", endpoint_id);
	} else {
		ast_db_put("CF", endpoint_id, target);
	}
}

void cisco_endpoint_mwi_count(struct ast_sip_endpoint *endpoint,
	int *mwi_new, int *mwi_old)
{
	*mwi_new = *mwi_old = 0;

	if (!endpoint) {
		return;
	}

	/* Stock PJSIP resolution order: endpoint->subscription.mwi.mailboxes
	 * overrides everything if set; otherwise union the mailboxes= field
	 * from each AOR in endpoint->aors. */
	if (!ast_strlen_zero(endpoint->subscription.mwi.mailboxes)) {
		ast_app_inboxcount(endpoint->subscription.mwi.mailboxes,
			mwi_new, mwi_old);
		return;
	}
	if (ast_strlen_zero(endpoint->aors)) {
		return;
	}

	{
		struct ast_str *all_mb = ast_str_create(256);
		char *aors_for_mwi;
		char *aor_for_mwi;

		if (!all_mb) {
			return;
		}
		aors_for_mwi = ast_strdupa(endpoint->aors);
		while ((aor_for_mwi = ast_strip(strsep(&aors_for_mwi, ",")))) {
			struct ast_sip_aor *aor;
			if (ast_strlen_zero(aor_for_mwi)) {
				continue;
			}
			aor = ast_sip_location_retrieve_aor(aor_for_mwi);
			if (!aor) {
				continue;
			}
			if (!ast_strlen_zero(aor->mailboxes)) {
				if (ast_str_strlen(all_mb)) {
					ast_str_append(&all_mb, 0, ",");
				}
				ast_str_append(&all_mb, 0, "%s", aor->mailboxes);
			}
			ao2_ref(aor, -1);
		}
		if (ast_str_strlen(all_mb)) {
			ast_app_inboxcount(ast_str_buffer(all_mb), mwi_new, mwi_old);
		}
		ast_free(all_mb);
	}
}
