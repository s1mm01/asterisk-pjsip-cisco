/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_feature_events
 *
 * Closes the phone -> server side of the Cisco DND softkey loop and
 * resolves the MAC-address From-URI that Cisco firmware uses on
 * device-level REFER/PUBLISH. Two distinct signalling paths plus an
 * unrelated SUBSCRIBE Expires clamp that piggybacks on the same
 * inbound-request module slot:
 *
 *   PATH B: PUBLISH Event: presence — the form Cisco firmware on the
 *   live fleet (CP7975 / CP8861) emits on every DND on/off press.
 *   Body application/pidf+xml:
 *
 *     <presence ...>
 *       <dm:person><e:activities><ce:dnd/></e:activities></dm:person>
 *     </presence>
 *
 *   This path is structurally tricky because Cisco firmware uses its
 *   MAC address as the From-URI user (e.g. sip:aabbccddeeff@server)
 *   instead of the endpoint id, so res_pjsip's stock endpoint
 *   identifiers (user, ip, anonymous) don't match the request to any
 *   endpoint, the distributor has no endpoint to hand to the auth
 *   module, and the request is 401'd. The chan_sip
 *   cisco-usecallmanager patch worked around this by skipping auth
 *   entirely for PUBLISH from Cisco peers (chan_sip.c:10053:
 *   "Buggy Cisco phones can't auth REFER or PUBLISH correctly").
 *
 *   We do better than the bypass: register a new endpoint identifier
 *   (cisco_authorization_identify) that matches incoming requests by
 *   the username in their Authorization: Digest header. The phone's
 *   first PUBLISH attempt arrives without Authorization, gets 401,
 *   the phone retries with Authorization: Digest username="1010" — at
 *   which point our identifier matches endpoint 1010, res_pjsip's
 *   normal digest auth verifies the response hash against the
 *   endpoint's password, and the request proceeds with full auth.
 *
 *   Our identifier only matches endpoints that have a [name]
 *   type=cisco sorcery section so it doesn't change identification
 *   semantics for non-Cisco endpoints.
 *
 *   The PUBLISH-presence handler then runs at priority APPLICATION-1
 *   (after auth has succeeded), pulls the now-identified endpoint
 *   via ast_pjsip_rdata_get_endpoint, parses the PIDF body for
 *   <ce:dnd/> vs empty activities, writes DND/<endpoint-id> to
 *   astdb, and replies 200 OK with SIP-ETag + Expires.
 *
 * Writes to astdb in the same convention as res_pjsip_cisco_bulkupdate:
 *   DND/<endpoint-id>  = "YES" / (deleted)
 *
 * So a server-side toggle (database put DND 1010 YES) and a
 * phone-side softkey press land in the same store.
 *
 *   PATH C: MAC-address From-URI identification. Cisco firmware puts
 *   the device MAC (not the line id) in the From-URI user of
 *   device-level REFERs — RemoteCC token registration, alarm reports,
 *   RemoteCC responses — and sometimes PUBLISH. Stock res_pjsip
 *   endpoint identifiers (user / ip / anonymous) can't map
 *   sip:aabbccddeeff@phone-ip to an endpoint, so the distributor logs
 *   "No matching endpoint found" and 401s the request before
 *   res_pjsip_cisco_remotecc ever sees it; cisco_authorization_identify
 *   (PATH B) only rescues the requests the phone actually retries with
 *   a usable Authorization username. So on every authenticated REGISTER
 *   from a Cisco endpoint we harvest the device MAC out of the Contact
 *   header parameters (+sip.instance's urn:uuid node, Cisco's
 *   +u.sip!devicename.ccm.cisco.com="SEPxxxx", or a bare 12-hex value)
 *   and remember MAC -> {endpoint id, source IP, expiry}.
 *   cisco_mac_identify then resolves a later request whose From-URI
 *   user is one of those MACs back to that endpoint, gated on the
 *   request arriving from the same source IP the REGISTER did.
 *
 *   This only gets the request *identified*. Whether it survives auth
 *   afterward depends on what the phone sends after the 401 — the
 *   chan_sip patch gave up here entirely ("Buggy Cisco phones can't
 *   auth REFER or PUBLISH correctly", chan_sip.c:10053); skipping the
 *   re-challenge for a MAC-identified Cisco REFER would be the next
 *   step if these phones turn out not to re-auth.
 *
 * NOT YET IMPLEMENTED (deferred):
 *   - Automatic server -> phone push when astdb changes out of band.
 *     The CISCO_* dialplan functions and matching `pjsip cisco ...`
 *     CLI verbs already push a bulkupdate REFER from
 *     res_pjsip_cisco_bulkupdate; direct DB()/AMI/database writes
 *     still require an explicit `pjsip cisco bulkupdate` because
 *     there is no astdb change notification.
 *   - Call-forward signalling from the phone-side softkey. The
 *     chan_sip patch handled CFwdALL via an `as-feature-event`
 *     SUBSCRIBE with a SetForwarding body; we never observed that
 *     traffic on the live fleet (see the "Removed: as-feature-event
 *     SUBSCRIBE handler" entry in ARCHITECTURE.md). CF state is
 *     currently only writable via `pjsip cisco cfwd ...` or DB() from
 *     the dialplan.
 *
 * Spec for PATH B: chan_sip patch's
 * channels/sip/handlers.c:13370+ sip_handle_publish_presence and the
 * auth-bypass at chan_sip.c:10053 (which we deliberately do NOT
 * replicate — the Authorization-identifier approach is stronger).
 *
 * File layout: this entry owns the pjsip_module + the SUBSCRIBE
 * Expires clamp (tiny, no datastructures of its own). PATH B lives
 * in res/cisco_feature_events/dnd.c; PATH C in res/cisco_feature_events/mac.c;
 * see feature_events_private.h for the cross-file surface.
 */

/*** MODULEINFO
	<depend>res_pjsip</depend>
	<depend>libxml2</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>

#include "asterisk/module.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"

#include "cisco/endpoint.h"
#include "cisco/rdata.h"
#include "feature_events_private.h"

/* ----------------------------------------------------------------------
 * Cisco SUBSCRIBE Expires clamp.
 *
 * Cisco Enterprise SIP firmware (verified against CP-7975G/9.4.2)
 * sends Expires: 2147483647 (INT_MAX seconds, ≈ 68 years) on its
 * BLF presence SUBSCRIBEs — effectively asking for a permanent
 * subscription. PJSIP's res_pjsip_pubsub then fails to create a
 * scheduler expiration timer because the value overflows the
 * scheduler's internal representation:
 *
 *   ERROR res_pjsip_pubsub.c: Unable to create expiration timer of
 *   2147357964 seconds for 1010->1006/presence ...
 *
 * Subscriptions are still ACCEPTED but the timer-driven NOTIFY-on-
 * state-change path is broken, so mid-call BLF state propagation
 * never reaches the phone. The unsolicited NOTIFYs we send at
 * REGISTER time still work (refreshing state on every re-REGISTER),
 * but real-time state changes don't.
 *
 * Fix: intercept incoming SUBSCRIBEs from Cisco endpoints before
 * pubsub processes them, clamp the Expires header to a sane value
 * (2 hours, so the phone refreshes its subscription periodically
 * but the scheduler timer is well within range). Pubsub then sees
 * the clamped value, creates a normal timer, and the in-dialog
 * NOTIFY path works.
 * ---------------------------------------------------------------------- */

/* 2h subscription lifetime. Phone will refresh well before expiry,
 * just like any other SIP UA. Long enough that re-SUBSCRIBE traffic
 * is negligible, short enough that pjsip's scheduler is happy. */
#define CISCO_MAX_SUBSCRIBE_EXPIRES 7200

static pj_bool_t subscribe_expires_clamp_on_rx_request(pjsip_rx_data *rdata)
{
	struct ast_sip_endpoint *endpoint;
	pjsip_expires_hdr *expires_hdr;

	/* Only clamp for Cisco endpoints — don't change subscription
	 * lifecycle for unrelated peers that happen to ask for long
	 * expiries. No Event-header filter: clamp applies to any
	 * Cisco SUBSCRIBE that asks for INT_MAX. */
	endpoint = cisco_pjsip_module_match(rdata, "SUBSCRIBE", NULL);
	if (!endpoint) {
		return PJ_FALSE;
	}

	expires_hdr = (pjsip_expires_hdr *) pjsip_msg_find_hdr(
		rdata->msg_info.msg, PJSIP_H_EXPIRES, NULL);
	if (!expires_hdr || expires_hdr->ivalue <= CISCO_MAX_SUBSCRIBE_EXPIRES) {
		ao2_cleanup(endpoint);
		return PJ_FALSE;
	}

	ast_debug(2,
		"cisco-feature-events: clamping SUBSCRIBE Expires from %d to %d "
		"for endpoint %s (firmware INT_MAX would overflow pubsub timer)\n",
		(int) expires_hdr->ivalue, CISCO_MAX_SUBSCRIBE_EXPIRES,
		ast_sorcery_object_get_id(endpoint));

	expires_hdr->ivalue = CISCO_MAX_SUBSCRIBE_EXPIRES;

	ao2_cleanup(endpoint);

	/* Don't claim — we just modified the header in-place; let pubsub
	 * (and any other pjsip_modules at our priority or higher) handle
	 * it normally. */
	return PJ_FALSE;
}

/* All inbound-request hooks share one pjsip_module slot rather than
 * burning three against pjproject's PJSIP_MAX_MODULE cap. Each
 * sub-handler self-filters by SIP method and is a no-op for anything
 * that isn't its request type, so calling them in sequence reproduces
 * the previous separate-modules-at-equal-priority behaviour exactly:
 *
 *   publish_dnd     (PUBLISH   Event: presence)          — may consume
 *   subscribe_clamp (SUBSCRIBE, any Event)               — mutates only
 *   mac_harvest     (REGISTER)                           — observes only
 *
 * publish_dnd must run before subscribe_clamp on consume-vs-mutate
 * ordering (if dnd ever consumed a SUBSCRIBE — currently it doesn't —
 * we wouldn't want clamp to mutate the request first). */
static pj_bool_t cisco_feature_events_on_rx_request(pjsip_rx_data *rdata)
{
	if (cisco_feature_events_dnd_on_rx_request(rdata)) {
		return PJ_TRUE;
	}
	subscribe_expires_clamp_on_rx_request(rdata);
	cisco_feature_events_mac_harvest_on_rx_request(rdata);
	return PJ_FALSE;
}

static pjsip_module cisco_feature_events_module = {
	.name             = { "cisco-feature-events", 20 },
	.id               = -1,
	.priority         = PJSIP_MOD_PRIORITY_APPLICATION - 1,
	.on_rx_request    = cisco_feature_events_on_rx_request,
};

static int load_module(void)
{
	/* Bring the per-PATH state up first, then register the pjsip
	 * module so request delivery starts. Tear down in reverse on
	 * failure. */
	if (cisco_feature_events_dnd_init()) {
		return AST_MODULE_LOAD_DECLINE;
	}
	if (cisco_feature_events_mac_init()) {
		cisco_feature_events_dnd_shutdown();
		return AST_MODULE_LOAD_DECLINE;
	}
	if (ast_sip_register_service(&cisco_feature_events_module)) {
		cisco_feature_events_mac_shutdown();
		cisco_feature_events_dnd_shutdown();
		ast_log(LOG_ERROR,
			"cisco-feature-events: failed to register pjsip module\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	/* See res_pjsip_cisco_bulkupdate.c for why we refuse runtime
	 * unload. PJSIP module + endpoint-identifier unregistration races
	 * with in-flight SUBSCRIBE / PUBLISH traffic. Reverse of load_module:
	 * stop the request dispatcher first, then the per-PATH state. */
	if (!ast_shutdown_final()) {
		return -1;
	}
	ast_sip_unregister_service(&cisco_feature_events_module);
	cisco_feature_events_mac_shutdown();
	cisco_feature_events_dnd_shutdown();
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco DND PUBLISH handler + MAC-address From-URI identifier",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_cisco_endpoint",
);
