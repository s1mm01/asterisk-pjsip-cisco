/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_call_extras
 *
 * Adds Cisco Enterprise SIP call-signaling compatibility that is not
 * part of BLF bootstrap or RemoteCC softkey handling:
 *   - Supported: X-cisco-sis-10.0.0
 *   - Call-Info: <urn:x-cisco-remotecc:callinfo>;orientation=...;security=...
 *   - Remote-Party-ID URI parameter x-cisco-callback-number from the
 *     CISCO_CALLBACK_NUMBER channel variable
 *   - Cisco H.264 SDP hints: b=TIAS:4000000 and default imageattr
 *
 * Wire shapes mirror the cisco-usecallmanager chan_sip patch:
 *   channels/sip/message.c sip_message_add_call_info()
 *   channels/sip/message.c sip_message_add_supported()
 *   channels/sip/message.c sip_message_add_identity()
 *   channels/sip/sdp.c     sip_sdp_add_video_codec()
 */

/*** MODULEINFO
	<depend>res_pjsip</depend>
	<depend>res_pjsip_session</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_ua.h>
#include <pjmedia/sdp.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/callerid.h"
#include "asterisk/pbx.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/sorcery.h"

#include "cisco_endpoint.h"
#include "cisco_session.h"

#define CISCO_SIS_SUPPORTED "X-cisco-sis-10.0.0"
#define CISCO_CALLINFO_URN "<urn:x-cisco-remotecc:callinfo>"
#define CISCO_CALLBACK_VAR "CISCO_CALLBACK_NUMBER"
#define CISCO_HUNTPILOT_VAR "CISCO_HUNTPILOT"
#define CISCO_H264_TIAS 4000000
#define CISCO_H264_IMAGEATTR "[x=640,y=480,q=0.50]"

static int endpoint_is_cisco(struct ast_sip_endpoint *endpoint)
{
	struct cisco_endpoint *cisco;

	if (!endpoint) {
		return 0;
	}

	cisco = cisco_endpoint_get(ast_sorcery_object_get_id(endpoint));
	if (!cisco) {
		return 0;
	}
	ao2_cleanup(cisco);
	return 1;
}

static int channel_var_copy(struct ast_sip_session *session, const char *name,
	char *buf, size_t buflen)
{
	struct ast_channel *channel;
	const char *value;

	if (!buf || buflen == 0) {
		return 0;
	}
	buf[0] = '\0';

	channel = cisco_session_channel_ref(session);
	if (!channel) {
		return 0;
	}

	ast_channel_lock(channel);
	value = pbx_builtin_getvar_helper(channel, name);
	if (!ast_strlen_zero(value)) {
		ast_copy_string(buf, value, buflen);
	}
	ast_channel_unlock(channel);
	ast_channel_unref(channel);

	return !ast_strlen_zero(buf);
}

static int connected_id_copy(struct ast_sip_session *session,
	struct ast_party_id *connected_id)
{
	struct ast_channel *channel;
	struct ast_party_id effective_id;

	ast_party_id_init(connected_id);

	channel = cisco_session_channel_ref(session);
	if (!channel) {
		return -1;
	}

	ast_channel_lock(channel);
	effective_id = ast_channel_connected_effective_id(channel);
	ast_party_id_copy(connected_id, &effective_id);
	ast_channel_unlock(channel);
	ast_channel_unref(channel);

	return 0;
}

static int add_supported_x_cisco_sis(pjsip_tx_data *tdata)
{
	static pj_str_t sis = { CISCO_SIS_SUPPORTED,
		sizeof(CISCO_SIS_SUPPORTED) - 1 };
	pjsip_supported_hdr *hdr;
	unsigned int i;

	hdr = pjsip_msg_find_hdr(tdata->msg, PJSIP_H_SUPPORTED, NULL);
	if (!hdr) {
		hdr = pjsip_supported_hdr_create(tdata->pool);
		if (!hdr) {
			return -1;
		}
		pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr *) hdr);
	}

	for (i = 0; i < hdr->count; ++i) {
		if (!pj_stricmp(&hdr->values[i], &sis)) {
			return 0;
		}
	}

	if (hdr->count >= PJSIP_GENERIC_ARRAY_MAX_COUNT) {
		return -1;
	}

	pj_strassign(&hdr->values[hdr->count++], &sis);
	return 0;
}

static int has_cisco_call_info(pjsip_tx_data *tdata)
{
	static const pj_str_t call_info_name = { "Call-Info", 9 };
	pjsip_hdr *hdr = NULL;
	char value[512];

	while ((hdr = pjsip_msg_find_hdr_by_name(tdata->msg, &call_info_name,
			hdr ? hdr->next : NULL))) {
		if (hdr->type != PJSIP_H_OTHER) {
			continue;
		}
		ast_copy_pj_str(value, &((pjsip_generic_string_hdr *) hdr)->hvalue,
			sizeof(value));
		if (strstr(value, CISCO_CALLINFO_URN)) {
			return 1;
		}
	}

	return 0;
}

static const char *callinfo_security(struct ast_sip_session *session,
	pjsip_tx_data *tdata)
{
	struct ast_sip_session_media_state *media_state;
	struct ast_sip_session_media *audio;
	int encrypted = 0;

	if (!tdata->tp_info.transport || !PJSIP_TRANSPORT_IS_SECURE(tdata->tp_info.transport)) {
		return "NotAuthenticated";
	}

	media_state = session->pending_media_state ?: session->active_media_state;
	if (media_state) {
		audio = media_state->default_session[AST_MEDIA_TYPE_AUDIO];
		encrypted = audio && audio->encryption != AST_SIP_MEDIA_ENCRYPT_NONE;
	}

	return encrypted ? "Encrypted" : "Authenticated";
}

static const char *local_domain(struct ast_sip_session *session,
	pjsip_tx_data *tdata, char *buf, size_t buflen)
{
	/* Reuse the From URI Asterisk/PJSIP already built for this message.
	 * That preserves endpoint from_domain and the actual selected local
	 * transport while still bracket-wrapping IPv6 when splicing the
	 * value into Cisco's Call-Info huntpilot URI. */
	return cisco_endpoint_local_domain(session->endpoint, tdata, buf, buflen);
}

static void append_huntpilot_uri(struct ast_str **call_info,
	struct ast_sip_session *session, pjsip_tx_data *tdata)
{
	char huntpilot[256];
	char callerid[256];
	char domain[256];
	char *name;
	char *number;
	/* Encoded buffers sized 3x source: ast_uri_encode worst-case is
	 * percent-escaping every byte (1 -> "%XX" = 3x). 256 was tight —
	 * pathological names/numbers got silently truncated. */
	char encoded_user[768];

	/* The chan_sip patch's analogue uses dialog->outgoing && dialog->channel
	 * as the gate — that's a leg/request-direction flag, not call-direction.
	 * AST_SIP_SESSION_OUTGOING_CALL is the call-direction equivalent. The
	 * two only diverge on re-INVITE responses; the huntpilot URI is only
	 * meaningful on the initial INVITE so this works in practice. */
	if (session->call_direction != AST_SIP_SESSION_OUTGOING_CALL
		|| !channel_var_copy(session, CISCO_HUNTPILOT_VAR, huntpilot,
			sizeof(huntpilot))) {
		return;
	}

	ast_copy_string(callerid, huntpilot, sizeof(callerid));
	if (ast_callerid_parse(callerid, &name, &number) || ast_strlen_zero(number)) {
		return;
	}

	ast_str_append(call_info, 0, ";huntpiloturi=\"");
	if (!ast_strlen_zero(name)) {
		char encoded_name[768];

		ast_uri_encode(name, encoded_name, sizeof(encoded_name),
			ast_uri_sip_user);
		/* %22 = percent-encoded '"' wrapping the display-name. The
		 * surrounding huntpiloturi value is itself a quoted-string,
		 * so the inner quotes have to be %-escaped to satisfy the
		 * chan_sip patch's wire shape. */
		ast_str_append(call_info, 0, "%%22%s%%22 ", encoded_name);
	}

	ast_uri_encode(number, encoded_user, sizeof(encoded_user),
		ast_uri_sip_user);
	ast_str_append(call_info, 0, "<sip:%s@%s>\"", encoded_user,
		local_domain(session, tdata, domain, sizeof(domain)));
}

static void add_call_info(struct ast_sip_session *session, pjsip_tx_data *tdata)
{
	struct ast_str *call_info = ast_str_alloca(512);

	if (has_cisco_call_info(tdata)) {
		return;
	}

	ast_str_set(&call_info, 0, "%s;orientation=%s;security=%s",
		CISCO_CALLINFO_URN,
		session->call_direction == AST_SIP_SESSION_OUTGOING_CALL ? "from" : "to",
		callinfo_security(session, tdata));
	append_huntpilot_uri(&call_info, session, tdata);

	ast_sip_add_header(tdata, "Call-Info", ast_str_buffer(call_info));
	ast_debug(2, "cisco-call-extras: attached Call-Info '%s'\n",
		ast_str_buffer(call_info));
}

static int has_rpid_header(pjsip_tx_data *tdata)
{
	static const pj_str_t rpid_name = { "Remote-Party-ID", 15 };

	return pjsip_msg_find_hdr_by_name(tdata->msg, &rpid_name, NULL) ? 1 : 0;
}

static void remove_rpid_headers(pjsip_tx_data *tdata)
{
	static const pj_str_t rpid_name = { "Remote-Party-ID", 15 };
	pjsip_hdr *hdr;

	while ((hdr = pjsip_msg_find_hdr_by_name(tdata->msg, &rpid_name, NULL))) {
		pj_list_erase(hdr);
	}
}

static void add_rpid_with_callback(struct ast_sip_session *session,
	pjsip_tx_data *tdata)
{
	struct ast_party_id connected_id;
	struct ast_str *rpid = ast_str_alloca(512);
	char callback[256];
	/* Encoded buffers sized 3x source for the worst-case ast_uri_encode
	 * expansion ("%XX" per byte). 256 silently truncated pathological
	 * callback / number values, producing malformed RPIDs on the wire. */
	char encoded_callback[768];
	char encoded_user[768];
	char domain[256];
	int presentation;

	if (!has_rpid_header(tdata)
		|| !channel_var_copy(session, CISCO_CALLBACK_VAR, callback,
			sizeof(callback))
		|| connected_id_copy(session, &connected_id)) {
		return;
	}

	remove_rpid_headers(tdata);

	if (connected_id.name.valid && !ast_strlen_zero(connected_id.name.str)) {
		char escaped_name[256];

		ast_escape_quoted(connected_id.name.str, escaped_name,
			sizeof(escaped_name));
		ast_str_append(&rpid, 0, "\"%s\" ", escaped_name);
	}

	if (connected_id.number.valid && !ast_strlen_zero(connected_id.number.str)) {
		ast_uri_encode(connected_id.number.str, encoded_user,
			sizeof(encoded_user), ast_uri_sip_user);
	} else {
		encoded_user[0] = '\0';
	}

	ast_uri_encode(callback, encoded_callback, sizeof(encoded_callback),
		ast_uri_sip_user);

	ast_str_append(&rpid, 0, "<sip:%s%s%s;x-cisco-callback-number=%s>",
		encoded_user, !ast_strlen_zero(encoded_user) ? "@" : "",
		local_domain(session, tdata, domain, sizeof(domain)),
		encoded_callback);

	presentation = ast_party_id_presentation(&connected_id);
	ast_str_append(&rpid, 0, ";party=%s;privacy=%s;screen=%s",
		session->inv_session && session->inv_session->role == PJSIP_ROLE_UAC
			? "calling" : "called",
		(presentation & AST_PRES_RESTRICTION) != AST_PRES_ALLOWED ? "full" : "off",
		(presentation & AST_PRES_NUMBER_TYPE) == AST_PRES_USER_NUMBER_PASSED_SCREEN
			? "yes" : "no");

	ast_sip_add_header(tdata, "Remote-Party-ID", ast_str_buffer(rpid));
	ast_debug(2, "cisco-call-extras: attached Remote-Party-ID '%s'\n",
		ast_str_buffer(rpid));
	ast_party_id_free(&connected_id);
}

static int method_is(pjsip_tx_data *tdata, const char *method)
{
	pjsip_cseq_hdr *cseq;

	cseq = pjsip_msg_find_hdr(tdata->msg, PJSIP_H_CSEQ, NULL);
	return cseq && pj_stricmp2(&cseq->method.name, method) == 0;
}

static int media_is_video(const pjmedia_sdp_media *media)
{
	return media && pj_stricmp2(&media->desc.media, "video") == 0;
}

static int rtpmap_is_h264(const pjmedia_sdp_attr *attr)
{
	pjmedia_sdp_rtpmap rtpmap;

	if (!attr || pjmedia_sdp_attr_get_rtpmap(attr, &rtpmap) != PJ_SUCCESS) {
		return 0;
	}
	return !pj_stricmp2(&rtpmap.enc_name, "H264");
}

static int media_has_tias(const pjmedia_sdp_media *media)
{
	unsigned int i;

	for (i = 0; i < media->bandw_count; ++i) {
		if (!pj_stricmp2(&media->bandw[i]->modifier, "TIAS")) {
			return 1;
		}
	}

	return 0;
}

static int add_tias_bandwidth(pj_pool_t *pool, pjmedia_sdp_media *media)
{
	pjmedia_sdp_bandw *bandw;

	if (media_has_tias(media)) {
		return 0;
	}
	if (media->bandw_count >= PJMEDIA_MAX_SDP_BANDW) {
		return -1;
	}

	bandw = PJ_POOL_ALLOC_T(pool, pjmedia_sdp_bandw);
	if (!bandw) {
		return -1;
	}

	pj_strdup2(pool, &bandw->modifier, "TIAS");
	bandw->value = CISCO_H264_TIAS;
	media->bandw[media->bandw_count++] = bandw;
	return 1;
}

static int imageattr_matches_payload(const pjmedia_sdp_attr *attr,
	const pj_str_t *fmt)
{
	/* imageattr values can be long: the multi-format `*` syntax with
	 * several xywh/q tuples easily exceeds 256. Truncating the source
	 * here false-negatives (we'd think there's no existing match and
	 * add a duplicate). 1024 covers any realistic Cisco firmware
	 * imageattr line. */
	char value[1024];
	char *payload;
	size_t fmt_len;

	if (!attr || pj_stricmp2(&attr->name, "imageattr") || !fmt) {
		return 0;
	}

	ast_copy_pj_str(value, &attr->value, sizeof(value));
	payload = value;
	while (*payload == ' ' || *payload == '\t') {
		++payload;
	}

	if (*payload == '*') {
		return 1;
	}

	fmt_len = fmt->slen;
	return !strncmp(payload, fmt->ptr, fmt_len)
		&& (payload[fmt_len] == ' ' || payload[fmt_len] == '\t');
}

static int media_has_imageattr(const pjmedia_sdp_media *media,
	const pj_str_t *fmt)
{
	unsigned int i;

	for (i = 0; i < media->attr_count; ++i) {
		if (imageattr_matches_payload(media->attr[i], fmt)) {
			return 1;
		}
	}

	return 0;
}

static int add_imageattr(pj_pool_t *pool, pjmedia_sdp_media *media,
	const pj_str_t *fmt)
{
	pjmedia_sdp_attr *attr;
	pj_str_t value;
	char value_buf[128];
	char fmt_buf[16];

	if (media_has_imageattr(media, fmt)) {
		return 0;
	}

	ast_copy_pj_str(fmt_buf, fmt, sizeof(fmt_buf));
	snprintf(value_buf, sizeof(value_buf), "%s recv %s", fmt_buf,
		CISCO_H264_IMAGEATTR);

	pj_cstr(&value, value_buf);
	attr = pjmedia_sdp_attr_create(pool, "imageattr", &value);
	if (!attr) {
		return -1;
	}

	return pjmedia_sdp_attr_add(&media->attr_count, media->attr, attr)
		== PJ_SUCCESS ? 1 : -1;
}

static int patch_video_media(pj_pool_t *pool, pjmedia_sdp_media *media)
{
	unsigned int i;
	int changed = 0;

	if (!media_is_video(media)) {
		return 0;
	}

	for (i = 0; i < media->desc.fmt_count; ++i) {
		pj_str_t *fmt = &media->desc.fmt[i];
		pjmedia_sdp_attr *rtpmap;
		int res;

		rtpmap = pjmedia_sdp_media_find_attr2(media, "rtpmap", fmt);
		if (!rtpmap_is_h264(rtpmap)) {
			continue;
		}

		res = add_tias_bandwidth(pool, media);
		if (res < 0) {
			ast_log(LOG_WARNING,
				"cisco-call-extras: unable to add H.264 TIAS bandwidth\n");
		} else if (res > 0) {
			changed = 1;
		}

		res = add_imageattr(pool, media, fmt);
		if (res < 0) {
			ast_log(LOG_WARNING,
				"cisco-call-extras: unable to add H.264 imageattr\n");
		} else if (res > 0) {
			changed = 1;
		}
	}

	return changed;
}

static void patch_sdp(pjsip_tx_data *tdata)
{
	pjsip_sdp_info *sdp_info;
	pjmedia_sdp_session *sdp;
	unsigned int i;
	int changed = 0;

	if (!tdata->msg->body) {
		return;
	}

	sdp_info = pjsip_get_sdp_info(tdata->pool, tdata->msg->body, NULL,
		&pjsip_media_type_application_sdp);
	if (sdp_info->sdp_err != PJ_SUCCESS || !sdp_info->sdp) {
		return;
	}

	sdp = sdp_info->sdp;
	for (i = 0; i < sdp->media_count; ++i) {
		if (patch_video_media(tdata->pool, sdp->media[i]) > 0) {
			changed = 1;
		}
	}

	if (changed) {
		pjsip_tx_data_invalidate_msg(tdata);
		ast_debug(2,
			"cisco-call-extras: patched outgoing SDP with H.264 hints\n");
	}
}

static void handle_session_outgoing(struct ast_sip_session *session,
	pjsip_tx_data *tdata)
{
	if (!endpoint_is_cisco(session->endpoint)) {
		return;
	}

	add_supported_x_cisco_sis(tdata);
	add_rpid_with_callback(session, tdata);

	if (method_is(tdata, "INVITE")) {
		add_call_info(session, tdata);
		patch_sdp(tdata);
	}
}

static void call_extras_outgoing_request(struct ast_sip_session *session,
	pjsip_tx_data *tdata)
{
	handle_session_outgoing(session, tdata);
}

static void call_extras_outgoing_response(struct ast_sip_session *session,
	pjsip_tx_data *tdata)
{
	handle_session_outgoing(session, tdata);
}

static void call_extras_endpoint_outgoing_request(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact, pjsip_tx_data *tdata)
{
	if (endpoint_is_cisco(endpoint)) {
		add_supported_x_cisco_sis(tdata);
	}
	(void) contact;
}

static void call_extras_endpoint_outgoing_response(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact, pjsip_tx_data *tdata)
{
	if (endpoint_is_cisco(endpoint)) {
		add_supported_x_cisco_sis(tdata);
	}
	(void) contact;
}

static struct ast_sip_session_supplement call_extras_session_supplement = {
	.method            = "INVITE,UPDATE",
	.priority          = AST_SIP_SUPPLEMENT_PRIORITY_CHANNEL - 900,
	.outgoing_request  = call_extras_outgoing_request,
	.outgoing_response = call_extras_outgoing_response,
};

static struct ast_sip_supplement call_extras_endpoint_supplement = {
	.priority          = AST_SIP_SUPPLEMENT_PRIORITY_LAST,
	.outgoing_request  = call_extras_endpoint_outgoing_request,
	.outgoing_response = call_extras_endpoint_outgoing_response,
};

static int load_module(void)
{
	ast_sip_session_register_supplement(&call_extras_session_supplement);
	ast_sip_register_supplement(&call_extras_endpoint_supplement);
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	/* Refuse runtime unload. ast_sip_(session_)unregister_supplement
	 * removes us from the supplement list but does not synchronise
	 * against in-flight callbacks running on other servant threads;
	 * if one fires after the unregister but before our .so is
	 * dlclose'd, it jumps into freed memory. Same idiom every other
	 * cisco_* module uses — see res_pjsip_cisco_bulkupdate.c for the
	 * canonical comment. */
	if (!ast_shutdown_final()) {
		return -1;
	}
	ast_sip_unregister_supplement(&call_extras_endpoint_supplement);
	ast_sip_session_unregister_supplement(&call_extras_session_supplement);
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco rich call signaling extras",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_session,res_pjsip_cisco_endpoint",
);
