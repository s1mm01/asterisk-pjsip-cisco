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

int cisco_endpoint_send_refer_to_contact(
	struct ast_sip_endpoint *endpoint, struct ast_sip_contact *contact,
	const char *log_prefix, const char *cid_suffix, const char *subject,
	cisco_refer_body_builder build, void *ctx)
{
	pjsip_tx_data *tdata = NULL;
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

	if (ast_sip_send_request(tdata, NULL, endpoint, NULL, NULL)) {
		ast_log(LOG_WARNING, "%s: %s send failed for %s\n",
			log_prefix, subject, contact->uri);
		return -1;
	}

	ast_log(LOG_NOTICE, "%s: %s sent to %s\n",
		log_prefix, subject, contact->uri);
	return 0;
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
