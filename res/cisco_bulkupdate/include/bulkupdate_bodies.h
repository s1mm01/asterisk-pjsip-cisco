/*
 * Wire-format XML body templates for res_pjsip_cisco_bulkupdate.so.
 *
 * Kept as a no-dependency header so tests/unit/test_xml_bodies.c can
 * include just these strings and validate well-formedness without
 * dragging in <pjsip.h> / asterisk/res_pjsip.h. The runtime code path
 * picks them up via bulkupdate_private.h's include of this file.
 *
 * Line-mapped to chan_sip patch's channels/sip/peers.c
 * sip_peer_send_bulk_update — do not edit the wire format without
 * cross-referencing.
 *
 * If you add a new substitution field carrying free-text dynamic
 * content, make sure the runtime caller ast_xml_escape()'s it AND
 * that tests/unit/test_xml_bodies.c exercises it with XML-special
 * inputs (& < > ").
 */

#ifndef CISCO_BULKUPDATE_BODIES_H
#define CISCO_BULKUPDATE_BODIES_H

#define DND_PART_FMT                                                    \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                  \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <dndupdate>\n"                                               \
	"    <state>%s</state>\n"                                       \
	"    <option>%s</option>\n"                                     \
	"  </dndupdate>\n"                                              \
	"</x-cisco-remotecc-request>\n"

#define HLOG_PART_FMT                                                   \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                  \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <hlogupdate>\n"                                              \
	"    <status>%s</status>\n"                                     \
	"  </hlogupdate>\n"                                             \
	"</x-cisco-remotecc-request>\n"

/* BULK_PART splits into header / per-contact / footer so we can emit
 * a <contact line="N"> element for each line button on multi-line
 * phones (see cisco->aliases). The full body assembles as
 *   BULK_PART_HEADER + N × BULK_CONTACT_FMT + BULK_PART_FOOTER. */
#define BULK_PART_HEADER                                                \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                  \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <bulkupdate>\n"

#define BULK_CONTACT_FMT                                                \
	"    <contact line=\"%d\">\n"                                   \
	"      <mwi>%s</mwi>\n"                                         \
	"      <emwi><voice-msg new=\"%d\" old=\"%d\" /></emwi>\n"      \
	"      <cfwdallupdate>\n"                                       \
	"        <fwdaddress>%s</fwdaddress>\n"                         \
	"        <tovoicemail>%s</tovoicemail>\n"                       \
	"      </cfwdallupdate>\n"                                      \
	"    </contact>\n"

#define BULK_PART_FOOTER                                                \
	"  </bulkupdate>\n"                                             \
	"</x-cisco-remotecc-request>\n"

#endif /* CISCO_BULKUPDATE_BODIES_H */
