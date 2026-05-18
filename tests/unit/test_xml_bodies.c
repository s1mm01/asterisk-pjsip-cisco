/*
 * XML body well-formedness regression test.
 *
 * Sprintf'd vendor-specific XML bodies sit at the top of each
 * server->phone REFER (bulkupdate, optionsind, MCID feedback, Park
 * toast, ParkMonitor orbit, HLog update). A format-string typo —
 * missing close tag, unbalanced attribute quote, dropped angle
 * bracket — lands in the wire body, and the phone silently
 * mis-renders the line UI / status glyph / BLF lamp. The bench
 * verifies a few bodies once at release time but won't notice a
 * regression in a body that nobody tested for that release.
 *
 * What this test does: snprintf each body template (pulled in via
 * the no-deps {bulkupdate,remotecc}_bodies.h headers the runtime
 * also uses) with bench-realistic substitutions, then xmlReadMemory-
 * parse the result. Any parse error fails the test.
 *
 * Caught:
 *   - Missing close tag / unbalanced angle bracket
 *   - Wrong attribute quote
 *   - Substitution that accidentally writes outside the template
 *     bounds (snprintf truncation visible as malformed XML)
 *
 * Not caught:
 *   - Wrong namespace (libxml2 accepts any URI; we keep the Cisco
 *     "parmams" typo verbatim)
 *   - Semantic correctness vs Cisco's schema (no published schema)
 *   - Missing ast_xml_escape() at the caller site (we provide
 *     XML-special substitutions explicitly; if a body breaks under
 *     them, that's the bug — but if the caller skips escaping in
 *     production code, this test won't catch that)
 *
 * The Cisco-private \200 (0x80) glyph bytes embedded in some bodies
 * (MCID_STATUS_PART_FMT, PARK_TOAST_PARKED, PARK_TOAST_CLEARED) are
 * deliberate firmware behaviour, not UTF-8. We parse with
 * XML_PARSE_RECOVER so libxml2 emits an encoding warning but still
 * walks the structure; we assert only that the root element parsed,
 * not that the document is UTF-8-clean.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "bulkupdate_bodies.h"
#include "remotecc_bodies.h"

/* Buffer larger than any body we emit. Real callers use 1-2 KiB stack
 * buffers; we go bigger so a runaway substitution would visibly land
 * in the parse rather than silently truncate. */
#define BUF_BYTES 8192

static int parse_errors;

/* Single libxml2 error handler: counts each report (so the test can
 * decide pass/fail) and discards the message text (so the test
 * output stays clean). Generic-error signature is variadic per
 * libxml2's xmlGenericErrorFunc typedef. */
static void counting_error_handler(void *ctx, const char *fmt, ...)
{
	(void) ctx;
	(void) fmt;
	parse_errors++;
}

/* Parse with recovery so libxml2 walks the structure even when the
 * Cisco-private \200 byte triggers an encoding error. Returns 1 if
 * a root element was produced, 0 otherwise. *had_errors counts
 * everything libxml2 reported during parsing. */
static int xml_parses(const char *xml, size_t len, int *had_errors)
{
	xmlDocPtr doc;
	xmlNodePtr root;
	int ok;

	parse_errors = 0;
	doc = xmlReadMemory(xml, (int) len, "test", NULL, XML_PARSE_RECOVER);
	*had_errors = parse_errors;

	if (!doc) {
		return 0;
	}
	root = xmlDocGetRootElement(doc);
	ok = root != NULL;
	xmlFreeDoc(doc);
	return ok;
}

/* Assert that \a xml is XML libxml2 can structurally parse. The
 * \a tolerate_encoding_errors flag exists for the Cisco-private
 * \200-bearing bodies — encoding errors are expected and not fatal. */
static void assert_xml_ok(const char *name, const char *xml, size_t len,
	int tolerate_encoding_errors)
{
	int had_errors = 0;
	int ok = xml_parses(xml, len, &had_errors);

	if (!ok) {
		fprintf(stderr, "FAIL: %s — libxml2 produced no root element\n",
			name);
		fprintf(stderr, "----- body -----\n%.*s\n----------------\n",
			(int) len, xml);
		assert(0 && "XML body failed to parse");
	}
	if (had_errors && !tolerate_encoding_errors) {
		fprintf(stderr, "FAIL: %s — %d parse error(s)\n",
			name, had_errors);
		fprintf(stderr, "----- body -----\n%.*s\n----------------\n",
			(int) len, xml);
		assert(0 && "XML body parsed with errors");
	}
	printf("  OK   %s (%zu bytes)\n", name, len);
}

/* Two callsite categories. UTF-8-clean templates use this. */
static void render_and_check(const char *name, char *buf, int n)
{
	assert(n > 0);
	assert(n < BUF_BYTES);
	assert_xml_ok(name, buf, (size_t) n, 0 /* strict */);
}

/* Cisco-private \200-bearing templates use this (recover from
 * encoding warnings; require only that the structure parses). */
static void render_and_check_lax(const char *name, char *buf, int n)
{
	assert(n > 0);
	assert(n < BUF_BYTES);
	assert_xml_ok(name, buf, (size_t) n, 1 /* lax */);
}

/*
 * Bulkupdate bodies. The runtime joins three parts into a
 * multipart/mixed body via pjsip_multipart_create, but each part is
 * an independent XML document by itself — we validate each
 * separately.
 */
static void test_bulkupdate_bodies(void)
{
	char buf[BUF_BYTES];
	int n;

	n = snprintf(buf, sizeof(buf), DND_PART_FMT, "enable", "callreject");
	render_and_check("DND_PART_FMT (enable/callreject)", buf, n);

	n = snprintf(buf, sizeof(buf), DND_PART_FMT, "disable", "ringeroff");
	render_and_check("DND_PART_FMT (disable/ringeroff)", buf, n);

	n = snprintf(buf, sizeof(buf), HLOG_PART_FMT, "on");
	render_and_check("HLOG_PART_FMT (on)", buf, n);

	n = snprintf(buf, sizeof(buf), HLOG_PART_FMT, "off");
	render_and_check("HLOG_PART_FMT (off)", buf, n);

	/* Full bulkupdate body: header + N contacts + footer. Exercise
	 * with N=1 and N=2 (alias-line scenario). Include an empty
	 * <fwdaddress> AND a populated one to cover both branches. */
	{
		char contacts[BUF_BYTES / 2];
		char body[BUF_BYTES];
		int c;

		/* N=1, no call-forward. */
		c = snprintf(contacts, sizeof(contacts),
			BULK_CONTACT_FMT,
			1 /* line */,
			"yes" /* mwi */,
			3 /* new */,
			1 /* old */,
			"" /* fwdaddress empty */,
			"off" /* tovoicemail */);
		assert(c > 0 && c < (int) sizeof(contacts));

		n = snprintf(body, sizeof(body), "%s%s%s",
			BULK_PART_HEADER, contacts, BULK_PART_FOOTER);
		render_and_check("BULK body (1 contact, no CF)", body, n);

		/* N=2, with a populated and pre-escaped CF target.
		 * The runtime calls ast_xml_escape on the target before
		 * splicing — we provide an already-safe string. */
		c = snprintf(contacts, sizeof(contacts),
			BULK_CONTACT_FMT,
			1, "no", 0, 0, "", "off");
		c += snprintf(contacts + c, sizeof(contacts) - c,
			BULK_CONTACT_FMT,
			2, "yes", 1, 0, "0418123456", "on");
		assert(c > 0 && c < (int) sizeof(contacts));

		n = snprintf(body, sizeof(body), "%s%s%s",
			BULK_PART_HEADER, contacts, BULK_PART_FOOTER);
		render_and_check("BULK body (2 contacts, +CF)", body, n);
	}
}

/*
 * RemoteCC bodies. HLOG / MCID-tone / Park-orbit are UTF-8-clean.
 * MCID-status / Park-toast (with the \200-bearing statustext) are
 * lax-checked. The dialog-id triplets carry XML-special characters
 * (&, <, >) in some real-world calls — those are ast_xml_escape'd
 * by the runtime, so we test with pre-escaped equivalents to mirror
 * the actual wire content.
 */
static void test_remotecc_bodies(void)
{
	char buf[BUF_BYTES];
	int n;
	/* Plausibly-special dialog id triplet, post-escape. */
	const char *call_id    = "abc.123&amp;@server";
	const char *local_tag  = "tag-AAA";
	const char *remote_tag = "tag-BBB&lt;0&gt;";

	n = snprintf(buf, sizeof(buf), HLOG_UPDATE_FMT, "on");
	render_and_check("HLOG_UPDATE_FMT (on)", buf, n);

	n = snprintf(buf, sizeof(buf), MCID_STATUS_PART_FMT,
		call_id, local_tag, remote_tag);
	render_and_check_lax("MCID_STATUS_PART_FMT (with \\200T glyph)",
		buf, n);

	n = snprintf(buf, sizeof(buf), MCID_TONE_PART_FMT,
		call_id, local_tag, remote_tag);
	render_and_check("MCID_TONE_PART_FMT", buf, n);

	/* PARK_TOAST_FMT with two payloads: a parked-slot glyph and
	 * the "cleared" glyph. Build statustext into a buffer first. */
	{
		char statustext[64];
		int s;

		s = snprintf(statustext, sizeof(statustext),
			PARK_TOAST_PARKED, 701);
		assert(s > 0 && s < (int) sizeof(statustext));
		n = snprintf(buf, sizeof(buf), PARK_TOAST_FMT,
			call_id, local_tag, remote_tag, statustext);
		render_and_check_lax("PARK_TOAST_FMT (parked slot 701)",
			buf, n);

		n = snprintf(buf, sizeof(buf), PARK_TOAST_FMT,
			call_id, local_tag, remote_tag, PARK_TOAST_CLEARED);
		render_and_check_lax("PARK_TOAST_FMT (cleared glyph)",
			buf, n);
	}

	n = snprintf(buf, sizeof(buf), PARK_ORBIT_FMT,
		7u /* version */,
		701 /* slot for entity */, "pbx.example.com",
		701 /* slot for <dialog id> */,
		"confirmed",
		"parked",
		701, "pbx.example.com",
		701, "pbx.example.com");
	render_and_check("PARK_ORBIT_FMT (parked, confirmed)", buf, n);

	n = snprintf(buf, sizeof(buf), PARK_ORBIT_FMT,
		8u, 701, "pbx.example.com", 701,
		"terminated", "retrieved",
		701, "pbx.example.com", 701, "pbx.example.com");
	render_and_check("PARK_ORBIT_FMT (retrieved, terminated)", buf, n);
}

int main(void)
{
	LIBXML_TEST_VERSION

	/* The single counting handler wins both routes — generic
	 * (xmlError-less, the older variadic API) and structured
	 * (xmlError-rich, the newer API). Both fire on libxml2's
	 * internal report paths; routing one to the counter and
	 * leaving the other at libxml2's default would result in
	 * stderr noise on the Cisco-private \200 byte cases. */
	xmlSetGenericErrorFunc(NULL, counting_error_handler);
	xmlSetStructuredErrorFunc(NULL, NULL);

	printf("test_xml_bodies: validating sprintf'd body templates...\n");

	test_bulkupdate_bodies();
	test_remotecc_bodies();

	xmlCleanupParser();

	printf("test_xml_bodies: all body templates parse correctly.\n");
	return 0;
}
