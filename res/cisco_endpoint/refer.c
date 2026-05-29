/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Out-of-band Cisco RemoteCC REFER + multipart-body sending helpers
 * for the res_pjsip_cisco_* family. Bodies for the declarations in
 * cisco/refer.h; linked into res_pjsip_cisco_endpoint.so and resolved
 * by the other cisco_* modules at load time.
 */

#include "asterisk.h"

#include <pjlib.h>
#include <pjsip.h>
#include <pjsip/sip_multipart.h>

#include "asterisk/astobj2.h"
#include "asterisk/logger.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"

#include "cisco/refer.h"

/*
 * Optional per-REFER delivery confirmation. When a caller wants to know
 * the phone actually accepted a REFER (not just that the request was
 * queued), we carry a copy of the caller's key and the target URI across
 * the async transaction and fire the callback from the response handler
 * on a 2xx. Copies are mandatory: the callback can run long after the
 * originating task — and the strings it borrowed — have gone.
 */
struct refer_confirm_payload {
	cisco_uri_confirm_cb cb;
	char *key;
	char *uri;
};

static void refer_confirm_payload_destroy(void *obj)
{
	struct refer_confirm_payload *p = obj;
	ast_free(p->key);
	ast_free(p->uri);
}

static void refer_confirm_response_cb(void *token, pjsip_event *e)
{
	struct refer_confirm_payload *p = token;

	/* Commit only on a 2xx final response. A timeout, transport error,
	 * or >=300 rejection leaves the URI unconfirmed so the caller's cache
	 * stays uncommitted and the next REGISTER re-targets it. */
	if (e->body.tsx_state.type == PJSIP_EVENT_RX_MSG
		&& e->body.tsx_state.tsx) {
		int status_code = e->body.tsx_state.tsx->status_code;

		if (status_code >= 200 && status_code < 300 && p->cb) {
			p->cb(p->key, p->uri);
		}
	}
	ao2_ref(p, -1);
}

static int send_refer_to_contact_impl(
	struct ast_sip_endpoint *endpoint, struct ast_sip_contact *contact,
	const char *log_prefix, const char *cid_suffix, const char *subject,
	cisco_refer_body_builder build, void *ctx,
	cisco_uri_confirm_cb on_confirm, const char *confirm_key)
{
	pjsip_tx_data *tdata = NULL;
	struct refer_confirm_payload *confirm = NULL;
	char cid[64];
	char refer_to[128];

	if (!endpoint || !contact || !build) {
		return -1;
	}

	if (ast_sip_create_request("REFER", NULL, endpoint, NULL,
			contact, &tdata)) {
		ast_log(LOG_WARNING,
			"%s: unable to create %s REFER for %s\n",
			log_prefix, subject, contact->uri);
		return -1;
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
		return -1;
	}

	if (on_confirm) {
		confirm = ao2_alloc(sizeof(*confirm), refer_confirm_payload_destroy);
		if (!confirm) {
			pjsip_tx_data_dec_ref(tdata);
			return -1;
		}
		confirm->cb  = on_confirm;
		confirm->key = ast_strdup(confirm_key);
		confirm->uri = ast_strdup(contact->uri);
		if (!confirm->key || !confirm->uri) {
			ao2_ref(confirm, -1);
			pjsip_tx_data_dec_ref(tdata);
			return -1;
		}
	}

	if (ast_sip_send_request(tdata, NULL, endpoint, confirm,
			confirm ? refer_confirm_response_cb : NULL)) {
		ast_log(LOG_WARNING, "%s: %s send failed for %s\n",
			log_prefix, subject, contact->uri);
		/* tdata is consumed by send_request; with no transaction created
		 * the response cb never fires, so release the token here. */
		if (confirm) {
			ao2_ref(confirm, -1);
		}
		return -1;
	}

	ast_log(LOG_NOTICE, "%s: %s sent to %s\n",
		log_prefix, subject, contact->uri);
	return 0;
}

int cisco_endpoint_send_refer_to_contact(
	struct ast_sip_endpoint *endpoint, struct ast_sip_contact *contact,
	const char *log_prefix, const char *cid_suffix, const char *subject,
	cisco_refer_body_builder build, void *ctx)
{
	return send_refer_to_contact_impl(endpoint, contact, log_prefix,
		cid_suffix, subject, build, ctx, NULL, NULL);
}

void cisco_endpoint_send_refer_to_all_contacts(
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
		/* Count attempts the moment a contact is yielded — any
		 * failure below leaves attempted > succeeded so a partial
		 * multi-contact fan-out doesn't get marked "fully fired"
		 * by callers that gate on equality. */
		attempted++;
		if (!cisco_endpoint_send_refer_to_contact(endpoint, contact,
				log_prefix, cid_suffix, subject, build, ctx)) {
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

void cisco_endpoint_send_refer_to_contact_uris(
	struct ast_sip_endpoint *endpoint,
	char **target_uris,
	const char *log_prefix, const char *cid_suffix, const char *subject,
	cisco_refer_body_builder build, void *ctx,
	cisco_uri_confirm_cb on_confirm, const char *confirm_key,
	int *attempted_out, int *succeeded_out)
{
	struct ao2_container *contacts;
	struct ao2_iterator iter;
	struct ast_sip_contact *contact;
	int attempted = 0;
	int succeeded = 0;
	char **u;

	if (!endpoint || ast_strlen_zero(endpoint->aors) || !build || !target_uris) {
		goto out;
	}
	if (!target_uris[0]) {
		goto out;
	}

	contacts = ast_sip_location_retrieve_contacts_from_aor_list(endpoint->aors);
	if (!contacts) {
		goto out;
	}

	iter = ao2_iterator_init(contacts, 0);
	while ((contact = ao2_iterator_next(&iter))) {
		int targeted = 0;

		for (u = target_uris; *u; u++) {
			if (!strcmp(contact->uri, *u)) {
				targeted = 1;
				break;
			}
		}
		if (!targeted) {
			ao2_cleanup(contact);
			continue;
		}

		/* succeeded counts REFERs that went on the wire; on_confirm fires
		 * later, from the transaction-response handler, only for those
		 * the phone answers 2xx — see refer_confirm_response_cb. */
		attempted++;
		if (!send_refer_to_contact_impl(endpoint, contact,
				log_prefix, cid_suffix, subject, build, ctx,
				on_confirm, confirm_key)) {
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

struct ast_sip_contact *cisco_endpoint_find_contact_from_rdata(
	struct ast_sip_endpoint *endpoint, pjsip_rx_data *rdata)
{
	struct ao2_container *contacts;
	struct ao2_iterator iter;
	struct ast_sip_contact *contact, *match = NULL;
	char src[64];

	if (!endpoint || !rdata || ast_strlen_zero(endpoint->aors)) {
		return NULL;
	}

	snprintf(src, sizeof(src), "%s:%d", rdata->pkt_info.src_name,
		rdata->pkt_info.src_port);

	contacts = ast_sip_location_retrieve_contacts_from_aor_list(endpoint->aors);
	if (!contacts) {
		return NULL;
	}

	iter = ao2_iterator_init(contacts, 0);
	while ((contact = ao2_iterator_next(&iter))) {
		if (strstr(contact->uri, src)) {
			match = contact;       /* keep the +1 ref */
			break;
		}
		ao2_cleanup(contact);
	}
	ao2_iterator_destroy(&iter);
	ao2_cleanup(contacts);
	return match;
}

void cisco_remotecc_multipart_add_part(pj_pool_t *pool,
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
