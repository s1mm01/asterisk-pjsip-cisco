/*
 * Cisco-flavoured PIDF body builder for res_pjsip_cisco_unsolicited_blf.
 *
 * See include/pidf.h for the public interface. Body shape is line-
 * mapped to chan_sip patch's channels/sip/request.c:549-583;
 * test coverage lives in tests/unit/test_blf_pidf.c.
 */

#include <asterisk.h>
#include <asterisk/strings.h>
#include <asterisk/utils.h>
#include <asterisk/pbx.h>
#include <asterisk/presencestate.h>
#include <asterisk/res_pjsip.h>
#include <asterisk/res_pjsip_presence_xml.h>

#include "pidf.h"

/* Big enough for any plausible exten@domain pair; keeps the helper
 * standalone (no pjsip header pull-in just to reach PJSIP_MAX_URL_SIZE). */
#define CISCO_PIDF_URI_BUFSIZE 256

char *cisco_blf_build_pidf(const char *exten, const char *domain,
	int exten_state, int presence_state)
{
	struct ast_str *out;
	char *result;
	char exten_xml[CISCO_PIDF_URI_BUFSIZE];
	char domain_xml[CISCO_PIDF_URI_BUFSIZE];

	out = ast_str_create(1024);
	if (!out) {
		return NULL;
	}

	ast_sip_sanitize_xml(exten, exten_xml, sizeof(exten_xml));
	ast_sip_sanitize_xml(domain, domain_xml, sizeof(domain_xml));

	ast_str_set(&out, 0,
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<presence xmlns=\"urn:ietf:params:xml:ns:pidf\""
		" xmlns:dm=\"urn:ietf:params:xml:ns:pidf:data-model\""
		" xmlns:e=\"urn:ietf:params:xml:ns:pidf:status:rpid\""
		" xmlns:ce=\"urn:cisco:params:xml:ns:pidf:rpid\""
		" entity=\"sip:%s@%s\">\n"
		"  <dm:person>\n"
		"    <e:activities>\n",
		exten_xml, domain_xml);

	if (exten_state & AST_EXTENSION_RINGING) {
		ast_str_append(&out, 0, "      <ce:alerting/>\n");
	} else if (exten_state & (AST_EXTENSION_INUSE | AST_EXTENSION_ONHOLD | AST_EXTENSION_BUSY)) {
		ast_str_append(&out, 0, "      <e:on-the-phone/>\n");
	}
	if (exten_state & AST_EXTENSION_BUSY) {
		ast_str_append(&out, 0, "      <e:busy/>\n");
	}
	/* DND propagates via presence state, not device state — chan_sip's
	 * sip_devicestate (channel_tech.c:1490+) and PJSIP's equivalent
	 * both ignore peer->do_not_disturb. Mirror what
	 * res_pjsip_cisco_pidf_body_generator does for in-dialog NOTIFYs:
	 * emit Cisco's private <ce:dnd/> activity when the watched
	 * extension's hint reports AST_PRESENCE_DND. */
	if (presence_state == AST_PRESENCE_DND) {
		ast_str_append(&out, 0, "      <ce:dnd/>\n");
	}

	ast_str_append(&out, 0,
		"    </e:activities>\n"
		"  </dm:person>\n"
		"  <tuple id=\"%s\">\n"
		"    <status>\n"
		"      <basic>%s</basic>\n"
		"    </status>\n"
		"  </tuple>\n"
		"</presence>\n",
		exten_xml,
		(exten_state == AST_EXTENSION_UNAVAILABLE) ? "closed" : "open");

	result = ast_strdup(ast_str_buffer(out));
	ast_free(out);
	return result;
}
