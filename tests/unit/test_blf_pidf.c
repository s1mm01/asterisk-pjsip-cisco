/*
 * Golden-output test for cisco_blf_build_pidf.
 *
 * The body builder takes (exten, domain, exten_state, presence_state)
 * and returns a Cisco-flavoured PIDF XML document. This test exercises
 * each state path against a checked-in expected string — any change
 * to the wire format (added/removed activity tag, attribute reorder,
 * namespace edit) breaks the test and gets caught before it reaches
 * a real Cisco phone.
 *
 * The body shape is line-mapped to the chan_sip patch's
 * channels/sip/request.c:549-583. Don't edit a `expect` literal here
 * without updating the corresponding rationale comment in
 * res/cisco_unsolicited_blf/pidf.c and cross-checking the chan_sip
 * patch.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <asterisk/pbx.h>
#include <asterisk/presencestate.h>
#include <asterisk/utils.h>

#include "pidf.h"

/* Build expected bodies via a single helper so the surrounding
 * scaffolding (xmlns header, basic-tuple footer) is one source of
 * truth — the test exists to catch changes to the *activity content*
 * and the open/closed basic state, not to triple-quote the same
 * namespace block six times. */
static char *build_expected(const char *exten_esc, const char *domain_esc,
	const char *activities, const char *basic)
{
	char *out;
	int n;
	const char *fmt =
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<presence xmlns=\"urn:ietf:params:xml:ns:pidf\""
		" xmlns:dm=\"urn:ietf:params:xml:ns:pidf:data-model\""
		" xmlns:e=\"urn:ietf:params:xml:ns:pidf:status:rpid\""
		" xmlns:ce=\"urn:cisco:params:xml:ns:pidf:rpid\""
		" entity=\"sip:%s@%s\">\n"
		"  <dm:person>\n"
		"    <e:activities>\n"
		"%s"
		"    </e:activities>\n"
		"  </dm:person>\n"
		"  <tuple id=\"%s\">\n"
		"    <status>\n"
		"      <basic>%s</basic>\n"
		"    </status>\n"
		"  </tuple>\n"
		"</presence>\n";

	n = snprintf(NULL, 0, fmt, exten_esc, domain_esc, activities,
		exten_esc, basic);
	out = malloc((size_t) n + 1);
	if (!out) {
		return NULL;
	}
	snprintf(out, (size_t) n + 1, fmt, exten_esc, domain_esc, activities,
		exten_esc, basic);
	return out;
}

static void check(const char *case_name, int exten_state, int presence_state,
	const char *expected_activities, const char *expected_basic)
{
	char *got;
	char *expected;

	got = cisco_blf_build_pidf("6001", "pbx.example.com",
		exten_state, presence_state);
	assert(got != NULL);

	expected = build_expected("6001", "pbx.example.com",
		expected_activities, expected_basic);
	assert(expected != NULL);

	if (strcmp(got, expected) != 0) {
		fprintf(stderr, "FAIL: %s\n", case_name);
		fprintf(stderr, "----- expected -----\n%s\n", expected);
		fprintf(stderr, "----- got      -----\n%s\n", got);
		fprintf(stderr, "--------------------\n");
		assert(0 && "PIDF body did not match golden output");
	}

	printf("  OK   %s\n", case_name);
	free(got);
	free(expected);
}

int main(void)
{
	printf("test_blf_pidf: checking PIDF body against golden output...\n");

	check("idle (NOT_INUSE → no activity, basic=open)",
		AST_EXTENSION_NOT_INUSE, AST_PRESENCE_AVAILABLE,
		"",
		"open");

	check("ringing → <ce:alerting/>",
		AST_EXTENSION_RINGING, AST_PRESENCE_AVAILABLE,
		"      <ce:alerting/>\n",
		"open");

	check("inuse → <e:on-the-phone/>",
		AST_EXTENSION_INUSE, AST_PRESENCE_AVAILABLE,
		"      <e:on-the-phone/>\n",
		"open");

	check("onhold → <e:on-the-phone/>",
		AST_EXTENSION_ONHOLD, AST_PRESENCE_AVAILABLE,
		"      <e:on-the-phone/>\n",
		"open");

	check("busy → <e:on-the-phone/> + <e:busy/>",
		AST_EXTENSION_BUSY, AST_PRESENCE_AVAILABLE,
		"      <e:on-the-phone/>\n      <e:busy/>\n",
		"open");

	check("ringing + DND → <ce:alerting/> + <ce:dnd/>",
		AST_EXTENSION_RINGING, AST_PRESENCE_DND,
		"      <ce:alerting/>\n      <ce:dnd/>\n",
		"open");

	check("idle + DND → just <ce:dnd/>",
		AST_EXTENSION_NOT_INUSE, AST_PRESENCE_DND,
		"      <ce:dnd/>\n",
		"open");

	check("unavailable → no activity, basic=closed",
		AST_EXTENSION_UNAVAILABLE, AST_PRESENCE_NOT_SET,
		"",
		"closed");

	/* XML-special chars in exten / domain must be escaped before
	 * they hit the wire — otherwise an apostrophe in a DID, or an
	 * ampersand in a vanity name, breaks the document. */
	{
		char *got = cisco_blf_build_pidf("o'malley", "a&b.com",
			AST_EXTENSION_NOT_INUSE, AST_PRESENCE_AVAILABLE);
		assert(got != NULL);
		if (!strstr(got, "entity=\"sip:o&apos;malley@a&amp;b.com\"")) {
			fprintf(stderr, "FAIL: XML-special escaping\n"
				"----- got -----\n%s\n", got);
			assert(0 && "exten/domain XML escaping broke");
		}
		if (!strstr(got, "<tuple id=\"o&apos;malley\">")) {
			fprintf(stderr, "FAIL: XML-special escaping in tuple id\n"
				"----- got -----\n%s\n", got);
			assert(0 && "tuple-id XML escaping broke");
		}
		printf("  OK   XML-special chars in exten/domain are escaped\n");
		free(got);
	}

	printf("test_blf_pidf: all PIDF bodies match golden output.\n");
	return 0;
}
