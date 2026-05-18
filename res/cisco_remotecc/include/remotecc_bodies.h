/*
 * Wire-format XML body templates for res_pjsip_cisco_remotecc.so.
 *
 * Kept as a no-dependency header so tests/unit/test_xml_bodies.c can
 * include just these strings and validate well-formedness without
 * dragging in <pjsip.h> / asterisk/res_pjsip.h. Runtime code picks
 * them up via remotecc_private.h's include of this file.
 *
 * MCID_STATUS_PART_FMT / PARK_TOAST_PARKED / PARK_TOAST_CLEARED carry
 * a Cisco-private \200 (0x80) display-glyph byte inside element
 * content. That's deliberate firmware behaviour (chan_sip parity); see
 * the file-header comments in res/cisco_remotecc/mcid.c and park.c.
 * The test handles those via libxml2's recovery mode rather than
 * strict UTF-8 parsing.
 *
 * Line-mapped to chan_sip patch's
 *   channels/sip/sip_remotecc_handler.c  (MCID / Park toast)
 *   channels/sip/chan_sip.c              (remotecc_park_notify orbit)
 * Don't edit the wire format without cross-referencing.
 */

#ifndef CISCO_REMOTECC_BODIES_H
#define CISCO_REMOTECC_BODIES_H

#define HLOG_UPDATE_FMT                                                 \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                  \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <hlogupdate>\n"                                              \
	"    <status>%s</status>\n"                                     \
	"  </hlogupdate>\n"                                             \
	"</x-cisco-remotecc-request>\n"

#define MCID_STATUS_PART_FMT                                             \
	"<?xml version=\"1.0\"?>\n"                                     \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <statuslineupdatereq>\n"                                     \
	"    <action>notify_display</action>\n"                         \
	"    <dialogid>\n"                                              \
	"      <callid>%s</callid>\n"                                   \
	"      <localtag>%s</localtag>\n"                               \
	"      <remotetag>%s</remotetag>\n"                             \
	"    </dialogid>\n"                                             \
	"    <statustext>\200T</statustext>\n"                          \
	"    <displaytimeout>7</displaytimeout>\n"                      \
	"    <linenumber>0</linenumber>\n"                              \
	"    <priority>1</priority>\n"                                  \
	"  </statuslineupdatereq>\n"                                    \
	"</x-cisco-remotecc-request>\n"

#define MCID_TONE_PART_FMT                                               \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                  \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <playtonereq>\n"                                             \
	"    <dialogid>\n"                                              \
	"      <callid>%s</callid>\n"                                   \
	"      <localtag>%s</localtag>\n"                               \
	"      <remotetag>%s</remotetag>\n"                             \
	"    </dialogid>\n"                                             \
	"    <tonetype>DtZipZip</tonetype>\n"                           \
	"    <direction>all</direction>\n"                              \
	"  </playtonereq>\n"                                            \
	"</x-cisco-remotecc-request>\n"

#define PARK_TOAST_FMT                                                   \
	"<?xml version=\"1.0\"?>\n"                                     \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <statuslineupdatereq>\n"                                     \
	"    <action>notify_display</action>\n"                         \
	"    <dialogid>\n"                                              \
	"      <callid>%s</callid>\n"                                   \
	"      <localtag>%s</localtag>\n"                               \
	"      <remotetag>%s</remotetag>\n"                             \
	"    </dialogid>\n"                                             \
	"    <statustext>%s</statustext>\n"                             \
	"    <displaytimeout>10</displaytimeout>\n"                     \
	"    <linenumber>0</linenumber>\n"                              \
	"    <priority>1</priority>\n"                                  \
	"  </statuslineupdatereq>\n"                                    \
	"</x-cisco-remotecc-request>\n"

#define PARK_TOAST_PARKED  "\200! %d"
#define PARK_TOAST_CLEARED "\200^"

#define PARK_ORBIT_FMT                                                    \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                   \
	"<dialog-info xmlns=\"urn:ietf:parmams:xml:ns:dialog-info\""     \
	" xmlns:call=\"urn:x-cisco:parmams:xml:ns:dialog-info:dialog:callinfo-dialog\"" \
	" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\""       \
	" version=\"%u\" state=\"full\" entity=\"%d@%s\">\n"            \
	"  <dialog id=\"%d\">\n"                                         \
	"    <state>%s</state>\n"                                        \
	"    <call:park><event>%s</event></call:park>\n"                \
	"    <local><identity display=\"\">sip:%d@%s</identity></local>\n"   \
	"    <remote><identity display=\"\">sip:%d@%s</identity></remote>\n" \
	"  </dialog>\n"                                                  \
	"</dialog-info>\n"

#endif /* CISCO_REMOTECC_BODIES_H */
