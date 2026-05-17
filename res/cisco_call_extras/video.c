/*
 * H.264 SDP hints + peer-state datastore, split out from
 * res_pjsip_cisco_call_extras.c.
 *
 * Two things this file does:
 *
 *   1. b=TIAS:4000000 + a=imageattr on outgoing H.264 video media.
 *      Mirrors the chan_sip cisco-usecallmanager patch's
 *      sip_sdp_add_video_codec(). When the bridge peer has previously
 *      offered an a=imageattr, we mirror it (rewritten with our own
 *      H.264 payload number) instead of using the static fallback —
 *      Gareth's chan_sip patch does the equivalent by widening
 *      struct ast_format_h264 inside Asterisk core. We can't replace
 *      the H.264 format interface from out-of-tree
 *      (ast_format_interface_register refuses a second "h264"
 *      registration and there's no unregister), so the supplement-
 *      boundary mirroring here covers the native chan_pjsip-to-
 *      chan_pjsip case.
 *
 *   2. Phantom outgoing-answer video suppression. When chan_pjsip's
 *      keep:all codec preference has produced an outgoing-answer
 *      m=video against a bridge peer that didn't itself offer any
 *      video, the Cisco caller opens its receive pane on the
 *      advertised port and waits forever for frames that never
 *      arrive ("black video box on voice-only calls"). We track
 *      every peer leg's most-recent incoming SDP in a per-channel
 *      datastore (cisco_h264_imageattr_state.had_video_media), then
 *      reject our outgoing m=video with port=0 when no observed
 *      peer offered video.
 *
 * Peer-channel discovery is a two-phase walk encapsulated in
 * with_peer_state(): fast bridge_peer() lookup, then a linkedid walk
 * for the pre-bridge window where ast_answer() emits the 200 OK
 * before app_dial calls ast_bridge_call().
 */

#include "asterisk.h"

#include <pjsip.h>
#include <pjmedia/sdp.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/datastore.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"

#include "cisco/session.h"
#include "call_extras_private.h"

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
	struct ast_channel *peer,
	const struct cisco_h264_imageattr_state *state, void *cb_arg);

static peer_cb_result invoke_cb_on_channel(struct ast_channel *peer,
	peer_state_cb cb, void *cb_arg)
{
	struct ast_datastore *ds;
	peer_cb_result rc;

	ast_channel_lock(peer);
	ds = ast_channel_datastore_find(peer, &cisco_h264_imageattr_info, NULL);
	rc = cb(peer, ds ? ds->data : NULL, cb_arg);
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

static peer_cb_result peer_imageattr_cb(struct ast_channel *peer,
	const struct cisco_h264_imageattr_state *state, void *cb_arg)
{
	char **out = cb_arg;
	(void) peer;

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
 *        the walk completes.
 *
 *   pjsip_siblings   — count of linkedid-matched (or bridge_peer)
 *                      channels whose technology is "PJSIP". Helper
 *                      channels (Local;1/;2, parking, dialplan-app
 *                      hangers-on) are deliberately not counted —
 *                      they share linkedid but carry no SDP and
 *                      therefore can't be a video sink/source.
 *   any_observed     — at least one PJSIP sibling carried our
 *                      datastore (i.e. its incoming-SDP capture
 *                      hook has fired).
 *   any_had_video    — among the observed PJSIP siblings, at least
 *                      one offered live video media.
 */
struct had_video_acc {
	int pjsip_siblings;
	int any_observed;
	int any_had_video;
};

static peer_cb_result peer_had_video_cb(struct ast_channel *peer,
	const struct cisco_h264_imageattr_state *state, void *cb_arg)
{
	struct had_video_acc *acc = cb_arg;
	const struct ast_channel_tech *tech;

	tech = ast_channel_tech(peer);
	if (!tech || strcasecmp(tech->type, "PJSIP")) {
		/* Local channels and other helpers contribute nothing —
		 * they have no SDP. Don't count them as "a SIP peer". */
		return PEER_CB_CONTINUE;
	}
	++acc->pjsip_siblings;
	if (state && state->observed) {
		acc->any_observed = 1;
		if (state->had_video_media) {
			acc->any_had_video = 1;
		}
	}
	return PEER_CB_CONTINUE;
}

/*!
 * \brief Did at least one peer leg's most-recent incoming SDP carry
 *        live video media?
 *
 * \retval  1  one or more PJSIP peers observed, at least one had
 *             video. There's a real source to relay; don't suppress.
 * \retval  0  EITHER (a) one or more PJSIP peers observed and NONE
 *             had video (voice-only callee) OR (b) zero PJSIP
 *             siblings at all (call terminates in dialplan —
 *             MusicOnHold, VoiceMail, IVR, ConfBridge, etc — no
 *             video sink exists). Both cases: safe to reject our
 *             outgoing video stream.
 * \retval -1  PJSIP siblings exist but none have an SDP observation
 *             yet (pre-setup window between channel creation and
 *             the dial leg's incoming hook firing). Caller must
 *             treat as "leave the stream alone" — suppressing here
 *             would race the late observation and misfire.
 */
static int cisco_h264_peer_had_video(struct ast_sip_session *session)
{
	struct had_video_acc acc = { 0, 0, 0 };

	with_peer_state(session, peer_had_video_cb, &acc);

	if (acc.pjsip_siblings == 0) {
		/* No SIP peer leg in this call. Asterisk is the
		 * terminator — MusicOnHold / VoiceMail / IVR / etc —
		 * and there's no video to relay either way. Suppress
		 * the phantom outgoing-answer video so the caller's
		 * receive pane doesn't paint black waiting for frames
		 * that nothing will ever generate. */
		return 0;
	}
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

void cisco_call_video_patch_sdp(struct ast_sip_session *session,
	pjsip_tx_data *tdata)
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

void cisco_call_video_capture_incoming(struct ast_sip_session *session,
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
