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
 *   - Connected-Line "Conference" display token on Conference-marked
 *     legs (RPID + PAI rewrite)
 *   - Cisco-side H.264 SDP hints (b=TIAS, imageattr) and the phantom
 *     outgoing-answer video suppression
 *
 * Header/body wire shapes mirror the cisco-usecallmanager chan_sip patch:
 *   channels/sip/message.c sip_message_add_call_info()
 *   channels/sip/message.c sip_message_add_supported()
 *   channels/sip/message.c sip_message_add_identity()
 *   channels/sip/sdp.c     sip_sdp_add_video_codec()
 *
 * File layout: this entry file owns the supplement registrations and
 * the outgoing-header decoration (Call-Info, RPID, Conference display
 * token, Supported). The H.264 SDP hints + the phantom-video
 * suppression datastore live in res/cisco_call_extras/video.c, called from
 * handle_session_outgoing on INVITEs and from the incoming-SDP hooks
 * (capture is deliberately ungated — the bridge peer's outgoing hook
 * mirrors imageattr regardless of which side it came from). See
 * call_extras_private.h for the cross-file surface.
 */

/*** MODULEINFO
	<depend>res_pjsip</depend>
	<depend>res_pjsip_session</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_ua.h>
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

#include "cisco/endpoint.h"
#include "cisco/rdata.h"
#include "cisco/session.h"
#include "call_extras_private.h"

#define CISCO_SIS_SUPPORTED "X-cisco-sis-10.0.0"
#define CISCO_CALLINFO_URN "<urn:x-cisco-remotecc:callinfo>"
#define CISCO_CALLBACK_VAR "CISCO_CALLBACK_NUMBER"
#define CISCO_HUNTPILOT_VAR "CISCO_HUNTPILOT"
#define CISCO_CONFERENCE_DISPLAY_TOKEN "\2004"

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
		cisco_call_video_patch_sdp(session, tdata);
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
	cisco_call_video_capture_incoming(session, rdata);
	return 0;
}

static void call_extras_incoming_response(struct ast_sip_session *session,
	pjsip_rx_data *rdata)
{
	cisco_call_video_capture_incoming(session, rdata);
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
