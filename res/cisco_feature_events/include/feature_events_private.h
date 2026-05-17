/*
 * Internal header for res_pjsip_cisco_feature_events.so. Declarations
 * shared across the module's sibling .c files (res_pjsip_cisco_
 * feature_events.c, res/cisco_feature_events/dnd.c, cisco_feature_events_
 * mac.c) — all compiled into one .so. Nothing here is exported across
 * module boundaries; the .exports version script keeps the symbol
 * table local.
 */

#ifndef CISCO_FEATURE_EVENTS_H
#define CISCO_FEATURE_EVENTS_H

#include <pjsip.h>

/*
 * res/cisco_feature_events/dnd.c — PATH B: DND PUBLISH presence handler
 * + cisco_authorization_identify endpoint identifier. The identifier
 * matches by the username in the Authorization: Digest header, so
 * the second (post-401) PUBLISH attempt from a Cisco phone (whose
 * From-URI carries the device MAC, not the line id) lands on the
 * correct endpoint and gets normal digest auth verification.
 */
int cisco_feature_events_dnd_init(void);
void cisco_feature_events_dnd_shutdown(void);
pj_bool_t cisco_feature_events_dnd_on_rx_request(pjsip_rx_data *rdata);

/*
 * res/cisco_feature_events/mac.c — PATH C: REGISTER-time MAC harvest +
 * cisco_mac_identify endpoint identifier. cisco_authorization_identify
 * (PATH B) rescues only the requests the phone retries with a usable
 * Authorization header; the MAC identifier covers the unauthenticated
 * device-level REFERs (RemoteCC token reg, alarm, etc) that Cisco
 * firmware sources from sip:<mac>@phone-ip From-URI.
 */
int cisco_feature_events_mac_init(void);
void cisco_feature_events_mac_shutdown(void);
void cisco_feature_events_mac_harvest_on_rx_request(pjsip_rx_data *rdata);

#endif /* CISCO_FEATURE_EVENTS_H */
