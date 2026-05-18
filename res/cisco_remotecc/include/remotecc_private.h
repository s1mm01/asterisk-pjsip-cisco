/*
 * Internal header for res_pjsip_cisco_remotecc.so. Declarations
 * shared across the module's sibling .c files (res_pjsip_cisco_
 * remotecc.c, res/cisco_remotecc/mcid.c, res/cisco_remotecc/park.c,
 * res/cisco_remotecc/record.c) — all compiled into one .so. Nothing
 * here is exported across module boundaries; the .exports version
 * script keeps the symbol table local.
 */

#ifndef CISCO_REMOTECC_H
#define CISCO_REMOTECC_H

#include <pjsip.h>

#include "asterisk/taskprocessor.h"
#include "asterisk/res_pjsip.h"

struct remotecc_dialog_id {
	char call_id[256];
	char local_tag[128];  /* Cisco phone's local tag. */
	char remote_tag[128]; /* Cisco phone's remote tag, Asterisk local tag. */
};

/*
 * Serializers, owned by load_module() in res_pjsip_cisco_remotecc.c.
 *
 *   remotecc_serializer — server->phone REFER/NOTIFY sends and the
 *                         MixMonitor exec. The shared queue for every
 *                         feature path that doesn't do bridge surgery.
 *   park_serializer     — only the Park blind transfer. Kept separate
 *                         so a stuck bridge op can't stall the other
 *                         server->phone sends queued on
 *                         remotecc_serializer.
 */
extern struct ast_taskprocessor *remotecc_serializer;
extern struct ast_taskprocessor *park_serializer;

/* res/cisco_remotecc/mcid.c — MCID softkey. */
int handle_mcid(struct ast_sip_endpoint *endpoint, const char *endpoint_id,
	const struct remotecc_dialog_id *dialog_id);

/*
 * res/cisco_remotecc/park.c — Park / ParkMonitor softkey.
 *
 * Returns 501 (Not Implemented) when res_parking is not loaded — the
 * symbols ast_parking_topic / ast_parked_call_type / ast_parking_is_
 * exten_park live in the asterisk binary itself, so the module loads
 * fine without res_parking.so, but the actual park (a blind transfer
 * to parkext) needs res_parking's bridge-feature registration to land
 * the call in a slot. Gating up-front avoids subscribing + queuing a
 * transfer that's going to fail asynchronously.
 */
int handle_park(struct ast_sip_endpoint *endpoint, const char *endpoint_id,
	const struct remotecc_dialog_id *dialog_id, int monitor);

/* res/cisco_remotecc/record.c — StartRecording / StopRecording softkeys. */
int handle_record(struct ast_sip_endpoint *endpoint, const char *endpoint_id,
	const struct remotecc_dialog_id *dialog_id, int start);

/* Body templates separated into remotecc_bodies.h so the test
 * harness can include them without dragging in <pjsip.h>. Runtime
 * callers transitively pick them up via this header. */
#include "remotecc_bodies.h"

#endif /* CISCO_REMOTECC_H */
