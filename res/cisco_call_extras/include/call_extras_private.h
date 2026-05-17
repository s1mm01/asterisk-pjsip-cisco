/*
 * Internal header for res_pjsip_cisco_call_extras.so. Declarations
 * shared across the module's sibling .c files (res_pjsip_cisco_
 * call_extras.c, res/cisco_call_extras/video.c) — all compiled into one .so.
 * Nothing here is exported across module boundaries; the .exports
 * version script keeps the symbol table local.
 */

#ifndef CISCO_CALL_EXTRAS_H
#define CISCO_CALL_EXTRAS_H

#include <pjsip.h>

#include "asterisk/res_pjsip_session.h"

/*
 * res/cisco_call_extras/video.c — H.264 SDP hints (b=TIAS + imageattr) plus the
 * peer-state datastore used to mirror peer-offered imageattr across
 * the bridge and to suppress phantom outgoing-answer video when the
 * bridge peer never offered any.
 *
 * cisco_call_video_capture_incoming() is deliberately ungated — any
 * chan_pjsip call's incoming SDP gets a tiny per-channel datastore
 * so the bridge peer's outgoing hook can mirror it. The patch path
 * runs from the Cisco-gated outgoing hook in the entry, so the
 * consume side stays Cisco-only.
 */
void cisco_call_video_patch_sdp(struct ast_sip_session *session,
	pjsip_tx_data *tdata);
void cisco_call_video_capture_incoming(struct ast_sip_session *session,
	pjsip_rx_data *rdata);

#endif /* CISCO_CALL_EXTRAS_H */
