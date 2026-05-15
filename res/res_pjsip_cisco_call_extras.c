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
 *   - Cisco-side H.264 SDP hints: b=TIAS:4000000 and default imageattr
 *
 * Header/body wire shapes mirror the cisco-usecallmanager chan_sip patch:
 *   channels/sip/message.c sip_message_add_call_info()
 *   channels/sip/message.c sip_message_add_supported()
 *   channels/sip/message.c sip_message_add_identity()
 *   channels/sip/sdp.c     sip_sdp_add_video_codec()
 *
 * H.264 imageattr propagation: incoming offer/answer SDP is parsed for
 * a=imageattr on H.264 payloads and the value is stashed on the
 * receiving leg's ast_channel via ast_datastore. When emitting outgoing
 * SDP on the bridge peer leg, we mirror that stashed value (rewritten
 * with our own payload number) instead of the static CISCO_H264_IMAGEATTR
 * fallback. Gareth's chan_sip patch does the equivalent by widening
 * struct ast_format_h264 inside Asterisk core; replacing that format
 * interface from an out-of-tree module isn't workable —
 * ast_format_interface_register() in main/format.c refuses a second
 * registration for "h264" and there's no unregister, so the only path
 * is to noload res_format_attr_h264.so and ship a replacement, which
 * this project explicitly avoids. The supplement-boundary mirroring
 * here covers the native chan_pjsip-to-chan_pjsip case. The static
 * fallback still applies when there is no bridge peer yet (first
 * INVITE), the peer never advertised imageattr, or the peer isn't on
 * a Cisco-flagged session.
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
#include "cisco_rdata.h"
#include "cisco_session.h"

#define CISCO_SIS_SUPPORTED "X-cisco-sis-10.0.0"
#define CISCO_CALLINFO_URN "<urn:x-cisco-remotecc:callinfo>"
#define CISCO_CALLBACK_VAR "CISCO_CALLBACK_NUMBER"
#define CISCO_HUNTPILOT_VAR "CISCO_HUNTPILOT"
#define CISCO_CONFERENCE_DISPLAY_TOKEN "\2004"
#define CISCO_H264_TIAS 4000000
#define CISCO_H264_IMAGEATTR "[x=640,y=480,q=0.50]"

/*!
 * \brief Datastore that pins media-negotiation observations from the
 *        peer's most-recently-seen incoming SDP onto its ast_channel.
 *
 * Set by the incoming offer/answer hook on the leg whose SDP arrived;
 * consumed by the outgoing offer/answer hook on the bridge peer leg
 * via ast_channel_bridge_peer().
 *
 * Fields:
 *   value           — peer's H.264 imageattr value (leading payload
 *                     number stripped). NULL when peer's SDP had no
 *                     a=imageattr on its H.264 payloads. Owned by the
 *                     datastore; freed in the destroy callback.
 *   had_video_media — 1 if peer's last incoming SDP carried any
 *                     m=video line at all (with non-zero port and
 *                     active direction), 0 if it had none. Used to
 *                     suppress phantom outgoing-answer video streams
 *                     where chan_pjsip's keep:all codec preference
 *                     would otherwise emit m=video <port> sendonly
 *                     against a peer leg that has no video to relay
 *                     — Cisco firmware then opens its receive pane
 *                     and waits forever for frames that never arrive
 *                     (the "black video box on voice-only calls"
 *                     symptom).
 *   observed        — 1 once we've parsed at least one incoming SDP
 *                     for this channel. Distinguishes "peer had no
 *                     video" (observed=1, had_video_media=0) from
 *                     "we never saw an SDP for this leg yet"
 *                     (observed=0), which the suppression logic
 *                     conservatively skips.
 */
struct cisco_h264_imageattr_state {
	char *value;
	int   had_video_media;
	int   observed;
};

static void cisco_h264_imageattr_destroy(void *data)
{
	struct cisco_h264_imageattr_state *state = data;

	if (!state) {
		return;
	}
	ast_free(state->value);
	ast_free(state);
}

static const struct ast_datastore_info cisco_h264_imageattr_info = {
	.type    = "cisco_h264_imageattr",
	.destroy = cisco_h264_imageattr_destroy,
};

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

static int connected_line_copy(struct ast_sip_session *session,
	struct ast_party_connected_line *connected)
{
	struct ast_channel *channel;

	ast_party_connected_line_init(connected);

	channel = cisco_session_channel_ref(session);
	if (!channel) {
		return -1;
	}

	ast_channel_lock(channel);
	ast_party_connected_line_copy(connected, ast_channel_connected(channel));
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

static void remove_headers_by_name(pjsip_tx_data *tdata, const char *name)
{
	pj_str_t hdr_name;
	pjsip_hdr *hdr;

	pj_cstr(&hdr_name, name);
	while ((hdr = pjsip_msg_find_hdr_by_name(tdata->msg, &hdr_name, NULL))) {
		pj_list_erase(hdr);
	}
}

/*!
 * \brief Decide whether the session's channel has been marked as a
 *        Conference leg by res_pjsip_cisco_conference.
 *
 * We use a channel variable (CISCO_CONFERENCE=1) as the authoritative
 * signal rather than ast_party_connected_line.source ==
 * AST_CONNECTED_LINE_UPDATE_SOURCE_CONFERENCE. That enum value is
 * defined only by the chan_sip cisco-usecallmanager patch we
 * deliberately do not require — stock Asterisk 20/22/23 doesn't have
 * it, so referencing it directly broke the CI build against vanilla
 * trees. The chan_var path is ABI-free.
 *
 * The connected-line name is still set to "Conference" by the
 * conference module (so chan_pjsip's From: display name comes out
 * right); we don't need to cross-check it here.
 */
static int session_channel_is_conference(struct ast_sip_session *session)
{
	struct ast_channel *channel;
	const char *flag;
	int is_conf;

	channel = cisco_session_channel_ref(session);
	if (!channel) {
		return 0;
	}
	ast_channel_lock(channel);
	flag = pbx_builtin_getvar_helper(channel, "CISCO_CONFERENCE");
	is_conf = !ast_strlen_zero(flag) && !strcmp(flag, "1");
	ast_channel_unlock(channel);
	ast_channel_unref(channel);
	return is_conf;
}

static void append_identity_uri(struct ast_str **identity,
	struct ast_sip_session *session, pjsip_tx_data *tdata,
	const struct ast_party_connected_line *connected,
	const char *callback)
{
	char domain[256];
	char encoded_user[768];
	const char *number = connected->id.number.valid
		? connected->id.number.str : "";

	if (!ast_strlen_zero(number)) {
		ast_uri_encode(number, encoded_user, sizeof(encoded_user),
			ast_uri_sip_user);
	} else {
		encoded_user[0] = '\0';
	}

	ast_str_append(identity, 0, "<sip:%s%s%s",
		encoded_user, !ast_strlen_zero(encoded_user) ? "@" : "",
		local_domain(session, tdata, domain, sizeof(domain)));

	if (!ast_strlen_zero(callback)) {
		char encoded_callback[768];

		ast_uri_encode(callback, encoded_callback,
			sizeof(encoded_callback), ast_uri_sip_user);
		ast_str_append(identity, 0, ";x-cisco-callback-number=%s",
			encoded_callback);
	}

	ast_str_append(identity, 0, ">");
}

static void add_conference_identity_header(struct ast_sip_session *session,
	pjsip_tx_data *tdata, const char *name,
	const struct ast_party_connected_line *connected, int remote_party_id,
	const char *callback)
{
	struct ast_str *identity = ast_str_alloca(512);

	ast_str_set(&identity, 0, "\"%s\" ", CISCO_CONFERENCE_DISPLAY_TOKEN);
	append_identity_uri(&identity, session, tdata, connected,
		remote_party_id ? callback : NULL);

	if (remote_party_id) {
		int presentation = ast_party_id_presentation(&connected->id);

		ast_str_append(&identity, 0, ";party=%s;privacy=%s;screen=%s",
			session->inv_session && session->inv_session->role == PJSIP_ROLE_UAC
				? "calling" : "called",
			(presentation & AST_PRES_RESTRICTION) != AST_PRES_ALLOWED
				? "full" : "off",
			(presentation & AST_PRES_NUMBER_TYPE) == AST_PRES_USER_NUMBER_PASSED_SCREEN
				? "yes" : "no");
	}

	ast_sip_add_header(tdata, name, ast_str_buffer(identity));
}

static void rewrite_conference_identity_headers(struct ast_sip_session *session,
	pjsip_tx_data *tdata)
{
	struct ast_party_connected_line connected;
	char callback[256];

	if (!session->channel || !session->endpoint->id.send_connected_line
		|| !session_channel_is_conference(session)
		|| connected_line_copy(session, &connected)) {
		return;
	}

	channel_var_copy(session, CISCO_CALLBACK_VAR, callback, sizeof(callback));

	/* RPID is unconditional here — even with send_rpid=no on the endpoint,
	 * a Conference-marked leg must carry "\2004" in Remote-Party-ID so the
	 * phone renders the Conference glyph + localised label. Without this
	 * the merge still works on the wire (audio is 3-way, no held lines)
	 * but the line appears as a regular call. PAI stays gated on send_pai
	 * because Cisco's display logic keys off RPID; PAI presence on a phone
	 * that isn't configured for it would be surprising. */
	remove_headers_by_name(tdata, "Remote-Party-ID");
	add_conference_identity_header(session, tdata,
		"Remote-Party-ID", &connected, 1, callback);

	if (session->endpoint->id.send_pai) {
		remove_headers_by_name(tdata, "P-Asserted-Identity");
		add_conference_identity_header(session, tdata,
			"P-Asserted-Identity", &connected, 0, NULL);
	}

	ast_debug(2,
		"cisco-call-extras: mapped Conference connected-line to "
		"Cisco display token on identity headers\n");
	ast_party_connected_line_free(&connected);
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

/*!
 * \brief Duplicate the trailing tuples of an imageattr value, stripping
 *        the leading payload number (or '*') and surrounding whitespace.
 *
 * Returns a malloced string the caller must ast_free(), or NULL when
 * the value carries nothing past the payload selector. Sized to cover
 * the multi-tuple '*' form (recv + send + multiple xywh/q sets).
 */
static char *imageattr_strip_fmt(const pj_str_t *value)
{
	char buf[1024];
	char *p;

	if (!value || value->slen == 0) {
		return NULL;
	}

	ast_copy_pj_str(buf, value, sizeof(buf));
	p = buf;
	while (*p == ' ' || *p == '\t') {
		++p;
	}

	if (*p == '*') {
		++p;
	} else {
		while (*p && *p != ' ' && *p != '\t') {
			++p;
		}
	}
	while (*p == ' ' || *p == '\t') {
		++p;
	}

	return ast_strlen_zero(p) ? NULL : ast_strdup(p);
}

static char *media_first_h264_imageattr(const pjmedia_sdp_media *media)
{
	unsigned int i;
	unsigned int j;

	if (!media_is_video(media)) {
		return NULL;
	}

	for (i = 0; i < media->desc.fmt_count; ++i) {
		const pj_str_t *fmt = &media->desc.fmt[i];
		pjmedia_sdp_attr *rtpmap;

		rtpmap = pjmedia_sdp_media_find_attr2((pjmedia_sdp_media *) media,
			"rtpmap", fmt);
		if (!rtpmap_is_h264(rtpmap)) {
			continue;
		}

		for (j = 0; j < media->attr_count; ++j) {
			pjmedia_sdp_attr *attr = media->attr[j];

			if (pj_stricmp2(&attr->name, "imageattr")) {
				continue;
			}
			if (imageattr_matches_payload(attr, fmt)) {
				return imageattr_strip_fmt(&attr->value);
			}
		}
	}

	return NULL;
}

/*!
 * \brief Has \a media any direction attribute (sendrecv/sendonly/
 *        recvonly/inactive)? Caller uses this together with the port
 *        check to decide whether the m=video line is "live" enough
 *        that the peer leg should treat it as a real video offer.
 *
 * An SDP without any explicit direction attribute defaults to
 * sendrecv per RFC 3264 §5.1, so absence-of-attribute counts as
 * "live". Only an explicit a=inactive turns it off.
 */
static int media_is_inactive(const pjmedia_sdp_media *media)
{
	unsigned int i;

	for (i = 0; i < media->attr_count; ++i) {
		if (!pj_stricmp2(&media->attr[i]->name, "inactive")) {
			return 1;
		}
	}
	return 0;
}

static int sdp_has_active_video_media(const pjmedia_sdp_session *sdp)
{
	unsigned int i;

	if (!sdp) {
		return 0;
	}
	for (i = 0; i < sdp->media_count; ++i) {
		const pjmedia_sdp_media *m = sdp->media[i];

		if (!media_is_video(m)) {
			continue;
		}
		if (m->desc.port == 0) {
			continue;        /* port=0 = peer rejected this stream */
		}
		if (media_is_inactive(m)) {
			continue;        /* a=inactive = stream is muted both ways */
		}
		return 1;
	}
	return 0;
}

/*!
 * \brief Stash everything we observed about \a sdp on the session's
 *        ast_channel: the peer's H.264 imageattr value (if any), and
 *        whether the peer's SDP carried any live video media at all.
 *
 * Always creates/updates the datastore once we've successfully parsed
 * an SDP for this leg, even when the SDP carried no H.264 imageattr —
 * the had_video_media + observed fields are independently useful for
 * suppressing phantom outgoing-answer video to the bridge peer.
 *
 * imageattr value is only overwritten when this SDP actually carried
 * one (a re-INVITE that omits the attribute keeps the previously-
 * negotiated shape). had_video_media is overwritten unconditionally —
 * it reflects the most recent observation, including transitions like
 * a re-INVITE that drops the video stream.
 */
static void cisco_h264_save_peer_imageattr(struct ast_sip_session *session,
	const pjmedia_sdp_session *sdp)
{
	struct ast_channel *channel;
	struct ast_datastore *ds;
	struct cisco_h264_imageattr_state *state;
	char *value = NULL;
	int had_video;
	unsigned int i;

	if (!sdp) {
		return;
	}
	for (i = 0; i < sdp->media_count && !value; ++i) {
		value = media_first_h264_imageattr(sdp->media[i]);
	}
	had_video = sdp_has_active_video_media(sdp);

	channel = cisco_session_channel_ref(session);
	if (!channel) {
		ast_free(value);
		return;
	}

	ast_channel_lock(channel);
	ds = ast_channel_datastore_find(channel, &cisco_h264_imageattr_info, NULL);
	if (ds) {
		state = ds->data;
		if (value) {
			ast_free(state->value);
			state->value = value;
		}
		state->had_video_media = had_video;
		state->observed        = 1;
	} else {
		state = ast_calloc(1, sizeof(*state));
		ds = state ? ast_datastore_alloc(&cisco_h264_imageattr_info, NULL) : NULL;
		if (!state || !ds) {
			ast_free(value);
			ast_free(state);
			if (ds) {
				ast_datastore_free(ds);
			}
			ast_channel_unlock(channel);
			ast_channel_unref(channel);
			return;
		}
		state->value           = value;
		state->had_video_media = had_video;
		state->observed        = 1;
		ds->data               = state;
		ast_channel_datastore_add(channel, ds);
	}
	ast_debug(2,
		"cisco-call-extras: captured peer SDP on %s — imageattr='%s' "
		"had_video=%d\n", ast_channel_name(channel),
		state->value ? state->value : "(none)", had_video);
	ast_channel_unlock(channel);
	ast_channel_unref(channel);
}

/*!
 * \brief Visitor callback for with_peer_state(). Invoked once per
 *        candidate peer channel; \a state is the channel's
 *        cisco_h264_imageattr_state, or NULL if the channel has no
 *        such datastore (Local helper channel, parking lot, etc).
 *
 * Return PEER_CB_DONE to stop the walk (e.g. once the imageattr
 * lookup has its answer), PEER_CB_CONTINUE to accumulate across
 * further candidates (e.g. the had_video lookup, which has to
 * inspect every observed peer before deciding suppression). The
 * \a cb_arg holds the caller's accumulator state.
 */
typedef enum {
	PEER_CB_CONTINUE,
	PEER_CB_DONE,
} peer_cb_result;

typedef peer_cb_result (*peer_state_cb)(
	const struct cisco_h264_imageattr_state *state, void *cb_arg);

static peer_cb_result invoke_cb_on_channel(struct ast_channel *peer,
	peer_state_cb cb, void *cb_arg)
{
	struct ast_datastore *ds;
	peer_cb_result rc;

	ast_channel_lock(peer);
	ds = ast_channel_datastore_find(peer, &cisco_h264_imageattr_info, NULL);
	rc = cb(ds ? ds->data : NULL, cb_arg);
	ast_channel_unlock(peer);
	return rc;
}

/*!
 * \brief Run \a cb against the cisco_h264_imageattr_state for each
 *        viable peer channel of \a session, until \a cb signals DONE
 *        or we run out of candidates.
 *
 * Two phases:
 *
 *   1. Fast path — ast_channel_bridge_peer(). Precise when the
 *      bridge exists (post-answer, post-impart). Returns NULL for
 *      3+ party bridges, in which case we fall through to phase 2.
 *
 *   2. Linkedid walk. Iterates every live channel sharing the local
 *      channel's linkedid (set by app_dial when it spawns each
 *      outbound channel), runs \a cb on each match. Required for
 *      the pre-bridge window: chan_pjsip emits the outgoing 200 OK
 *      during ast_answer(), which app_dial calls BEFORE
 *      ast_bridge_call() — bridge_peer() returns NULL at that
 *      moment, but linkedid is already propagated to both legs.
 *      Verified on the bench: without this fallback the video
 *      suppression guard kept missing the initial 200 OK and the
 *      8865 painted a black video pane for the full call.
 *
 * Aggregation lives in the callback. Helper channels in the dial
 * chain (Local;1/;2, parking, etc) share linkedid but have no
 * cisco_h264_* datastore, so they call \a cb with state=NULL —
 * had_video accumulation conservatively treats those as "no signal"
 * rather than "no video", and the imageattr lookup naturally
 * skips them. That's the fix for "first channel with linkedid
 * wins" picking up a Local helper and shadowing the real PJSIP
 * peer's state.
 *
 * Multi-party scenarios: phase 1 returns NULL (bridge isn't 2-
 * party), phase 2 visits every leg and the callback's accumulator
 * decides the outcome. For had_video this means "suppress only
 * when every observed peer offered no video" — correct for the
 * voice-only-extension-in-a-conference case where SOME participant
 * has video.
 */
static void with_peer_state(struct ast_sip_session *session,
	peer_state_cb cb, void *cb_arg)
{
	struct ast_channel *local;
	struct ast_channel *peer;
	struct ast_channel_iterator *iter;
	struct ast_channel *candidate;
	char local_linkedid[AST_MAX_UNIQUEID];

	local = cisco_session_channel_ref(session);
	if (!local) {
		return;
	}

	/* Phase 1: ast_channel_bridge_peer's documented contract is
	 * "no channel locks should be held when calling" — we don't
	 * hold any here (cisco_session_channel_ref returned us a
	 * locked-and-unlocked ref'd channel). Bridge_peer returns
	 * non-NULL only for a 2-party formed bridge. */
	peer = ast_channel_bridge_peer(local);
	if (peer) {
		ast_channel_unref(local);
		invoke_cb_on_channel(peer, cb, cb_arg);
		ast_channel_unref(peer);
		return;
	}

	/* Phase 2: linkedid walk. Snapshot linkedid under the channel
	 * lock so a destruction races can't pull the bytes out from
	 * under us — the const char * returned by ast_channel_linkedid
	 * lives in the channel struct. */
	ast_channel_lock(local);
	ast_copy_string(local_linkedid,
		S_OR(ast_channel_linkedid(local), ""),
		sizeof(local_linkedid));
	ast_channel_unlock(local);

	if (ast_strlen_zero(local_linkedid)) {
		ast_channel_unref(local);
		return;
	}

	iter = ast_channel_iterator_all_new();
	if (!iter) {
		ast_channel_unref(local);
		return;
	}

	while ((candidate = ast_channel_iterator_next(iter))) {
		const char *c_linkedid;
		int match;
		peer_cb_result rc;

		if (candidate == local) {
			ast_channel_unref(candidate);
			continue;
		}

		ast_channel_lock(candidate);
		c_linkedid = ast_channel_linkedid(candidate);
		match = !ast_strlen_zero(c_linkedid)
			&& !strcmp(c_linkedid, local_linkedid);
		ast_channel_unlock(candidate);

		if (!match) {
			ast_channel_unref(candidate);
			continue;
		}

		rc = invoke_cb_on_channel(candidate, cb, cb_arg);
		ast_channel_unref(candidate);
		if (rc == PEER_CB_DONE) {
			break;
		}
	}
	ast_channel_iterator_destroy(iter);
	ast_channel_unref(local);
}

static peer_cb_result peer_imageattr_cb(
	const struct cisco_h264_imageattr_state *state, void *cb_arg)
{
	char **out = cb_arg;

	if (state && state->value) {
		*out = ast_strdup(state->value);
		return PEER_CB_DONE;        /* first non-NULL value wins */
	}
	return PEER_CB_CONTINUE;
}

/*!
 * \brief Dup the peer's stashed H.264 imageattr tuples, or NULL if
 *        none / no peer / peer never sent imageattr / all candidates
 *        were helper channels with no datastore. Caller ast_free()s
 *        the result.
 */
static char *cisco_h264_peer_imageattr(struct ast_sip_session *session)
{
	char *value = NULL;

	with_peer_state(session, peer_imageattr_cb, &value);
	return value;
}

/*!
 * \brief Accumulator for the had_video lookup. Walked across every
 *        viable peer candidate; the final answer is derived after
 *        the walk completes (any_observed ? any_had_video : -1).
 */
struct had_video_acc {
	int any_observed;
	int any_had_video;
};

static peer_cb_result peer_had_video_cb(
	const struct cisco_h264_imageattr_state *state, void *cb_arg)
{
	struct had_video_acc *acc = cb_arg;

	if (state && state->observed) {
		acc->any_observed = 1;
		if (state->had_video_media) {
			acc->any_had_video = 1;
		}
	}
	/* Always continue: helper channels (Local;1/;2, parking) share
	 * linkedid but carry no datastore — we want to walk past them
	 * and find the real PJSIP peer's observation. The accumulator
	 * naturally skips state=NULL candidates without polluting the
	 * answer. */
	return PEER_CB_CONTINUE;
}

/*!
 * \brief Did at least one peer leg's most-recent incoming SDP carry
 *        live video media?
 *
 * \retval  1  some peer had video. Don't suppress: there's a real
 *             source to relay.
 * \retval  0  one or more peers observed, NONE had video. Safe to
 *             reject our outgoing video stream.
 * \retval -1  no peer observation at all — either no peer channel
 *             exists yet, every candidate was a helper channel
 *             without our datastore, or the peer leg's incoming
 *             hook hasn't fired. Callers must treat as "leave the
 *             stream alone" (don't suppress on uncertainty).
 */
static int cisco_h264_peer_had_video(struct ast_sip_session *session)
{
	struct had_video_acc acc = { 0, 0 };

	with_peer_state(session, peer_had_video_cb, &acc);
	if (!acc.any_observed) {
		return -1;
	}
	return acc.any_had_video ? 1 : 0;
}

static int add_imageattr(pj_pool_t *pool, pjmedia_sdp_media *media,
	const pj_str_t *fmt, struct ast_sip_session *session)
{
	pjmedia_sdp_attr *attr;
	pj_str_t value;
	/* 1024 mirrors imageattr_matches_payload()'s buffer — the
	 * peer-mirrored multi-tuple form (send + recv with several xywh/q
	 * sets) easily exceeds the old 128. */
	char value_buf[1024];
	char fmt_buf[16];
	char *peer_value;

	if (media_has_imageattr(media, fmt)) {
		return 0;
	}

	ast_copy_pj_str(fmt_buf, fmt, sizeof(fmt_buf));
	peer_value = session ? cisco_h264_peer_imageattr(session) : NULL;
	if (peer_value) {
		snprintf(value_buf, sizeof(value_buf), "%s %s", fmt_buf, peer_value);
		ast_debug(2,
			"cisco-call-extras: mirroring peer H.264 imageattr -> '%s'\n",
			value_buf);
		ast_free(peer_value);
	} else {
		snprintf(value_buf, sizeof(value_buf), "%s recv %s", fmt_buf,
			CISCO_H264_IMAGEATTR);
	}

	pj_cstr(&value, value_buf);
	attr = pjmedia_sdp_attr_create(pool, "imageattr", &value);
	if (!attr) {
		return -1;
	}

	return pjmedia_sdp_attr_add(&media->attr_count, media->attr, attr)
		== PJ_SUCCESS ? 1 : -1;
}

/*!
 * \brief Set \a media's port to 0, the RFC 3264 §6 "I reject this
 *        media stream" signal. Returns 1 if the port was actually
 *        changed (caller marks the SDP dirty), 0 if it was already 0.
 *
 * Per RFC 3264 §6 the answerer MAY also strip the attribute list down
 * to just the required rtpmap entries, but neither requirement nor
 * Cisco firmware care — leaving the existing attributes alone keeps
 * the diff to a single field change and means we don't have to
 * allocate fresh attr storage out of the pool.
 */
static int media_set_rejected(pjmedia_sdp_media *media)
{
	if (media->desc.port == 0) {
		return 0;
	}
	media->desc.port = 0;
	return 1;
}

static int patch_video_media(pj_pool_t *pool, pjmedia_sdp_media *media,
	struct ast_sip_session *session)
{
	unsigned int i;
	int changed = 0;
	int has_h264 = 0;

	if (!media_is_video(media)) {
		return 0;
	}
	if (media->desc.port == 0) {
		return 0;        /* already rejected upstream; nothing to do */
	}

	for (i = 0; i < media->desc.fmt_count && !has_h264; ++i) {
		pjmedia_sdp_attr *rtpmap = pjmedia_sdp_media_find_attr2(
			media, "rtpmap", &media->desc.fmt[i]);
		has_h264 = rtpmap_is_h264(rtpmap);
	}
	if (!has_h264) {
		return 0;        /* not an H.264 stream we'd be augmenting */
	}

	/* Suppress the phantom video stream: when chan_pjsip's keep:all
	 * codec preference has emitted an outgoing-answer m=video against
	 * a bridge peer that didn't itself offer any video, the Cisco
	 * caller opens its receive pane on the advertised port and waits
	 * forever for frames that never arrive (the "black video box on
	 * voice-only calls" symptom). Reject our video with port=0 so the
	 * caller drops the pane immediately. peer_had_video == -1 means
	 * we haven't observed the peer's SDP yet (mid-setup); leave the
	 * stream alone in that case rather than guess. */
	if (session && cisco_h264_peer_had_video(session) == 0) {
		if (media_set_rejected(media)) {
			ast_debug(2,
				"cisco-call-extras: rejected outgoing H.264 video "
				"(port -> 0) — bridge peer offered no video to "
				"relay\n");
			return 1;
		}
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

		res = add_imageattr(pool, media, fmt, session);
		if (res < 0) {
			ast_log(LOG_WARNING,
				"cisco-call-extras: unable to add H.264 imageattr\n");
		} else if (res > 0) {
			changed = 1;
		}
	}

	return changed;
}

static void patch_sdp(struct ast_sip_session *session, pjsip_tx_data *tdata)
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
		if (patch_video_media(tdata->pool, sdp->media[i], session) > 0) {
			changed = 1;
		}
	}

	if (changed) {
		pjsip_tx_data_invalidate_msg(tdata);
		ast_debug(2,
			"cisco-call-extras: patched outgoing SDP with H.264 hints\n");
	}
}

static void capture_incoming_sdp(struct ast_sip_session *session,
	pjsip_rx_data *rdata)
{
	pjsip_sdp_info *sdp_info;

	if (!rdata || !rdata->msg_info.msg || !rdata->msg_info.msg->body) {
		return;
	}

	sdp_info = pjsip_get_sdp_info(rdata->tp_info.pool, rdata->msg_info.msg->body,
		NULL, &pjsip_media_type_application_sdp);
	if (sdp_info->sdp_err != PJ_SUCCESS || !sdp_info->sdp) {
		return;
	}

	cisco_h264_save_peer_imageattr(session, sdp_info->sdp);
}

static void handle_session_outgoing(struct ast_sip_session *session,
	pjsip_tx_data *tdata)
{
	if (!endpoint_is_cisco(session->endpoint)) {
		return;
	}

	add_supported_x_cisco_sis(tdata);
	add_rpid_with_callback(session, tdata);
	rewrite_conference_identity_headers(session, tdata);

	if (method_is(tdata, "INVITE")) {
		add_call_info(session, tdata);
		patch_sdp(session, tdata);
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

/*
 * Capture is intentionally NOT gated on endpoint_is_cisco — we want
 * to stash a peer's imageattr regardless of which side it came from,
 * so the bridge peer's outgoing hook can mirror it. The cost is one
 * ast_datastore per chan_pjsip call that happens to carry an H.264
 * imageattr; the consume path is still Cisco-only.
 */
static int call_extras_incoming_request(struct ast_sip_session *session,
	pjsip_rx_data *rdata)
{
	capture_incoming_sdp(session, rdata);
	return 0;
}

static void call_extras_incoming_response(struct ast_sip_session *session,
	pjsip_rx_data *rdata)
{
	capture_incoming_sdp(session, rdata);
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

/* Outgoing-only supplement, runs before chan_pjsip's CHANNEL-priority
 * step. The outgoing-side hooks (SDP patching + Cisco header
 * decoration) don't care about channel existence — the channel is
 * already up by the time anything leaves on this leg. */
static struct ast_sip_session_supplement call_extras_session_supplement = {
	.method            = "INVITE,UPDATE",
	.priority          = AST_SIP_SUPPLEMENT_PRIORITY_CHANNEL - 900,
	.outgoing_request  = call_extras_outgoing_request,
	.outgoing_response = call_extras_outgoing_response,
};

/* Incoming SDP capture has to run AFTER chan_pjsip creates
 * session->channel, otherwise the very first inbound INVITE's offer
 * is observed with session->channel == NULL and silently dropped —
 * which then makes later cross-leg lookups via ast_channel_bridge_peer
 * fall back to the "unknown" branch and bypass both imageattr
 * mirroring and the video-suppress guard for any call whose only SDP
 * came from that initial offer. PRIORITY_CHANNEL + 100 places us
 * after channel creation but still ahead of PRIORITY_LAST consumers,
 * so the captured state is on session->channel by the time the next
 * supplement (or this module's own outgoing-side patching) needs it. */
static struct ast_sip_session_supplement call_extras_capture_supplement = {
	.method            = "INVITE,UPDATE",
	.priority          = AST_SIP_SUPPLEMENT_PRIORITY_CHANNEL + 100,
	.incoming_request  = call_extras_incoming_request,
	.incoming_response = call_extras_incoming_response,
};

static struct ast_sip_supplement call_extras_endpoint_supplement = {
	.priority          = AST_SIP_SUPPLEMENT_PRIORITY_LAST,
	.outgoing_request  = call_extras_endpoint_outgoing_request,
	.outgoing_response = call_extras_endpoint_outgoing_response,
};

static int load_module(void)
{
	ast_sip_session_register_supplement(&call_extras_session_supplement);
	ast_sip_session_register_supplement(&call_extras_capture_supplement);
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
	ast_sip_session_unregister_supplement(&call_extras_capture_supplement);
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
