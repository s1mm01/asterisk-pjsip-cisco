/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_pidf_body_generator
 *
 * Cisco-flavoured PIDF (RFC 3863 + RFC 4480 RPID + Cisco's private RPID
 * namespace) for chan_pjsip presence subscriptions.
 *
 * Cisco Enterprise SIP firmware (7975, 8861, 8865, etc.) only renders a
 * line button as a BLF watcher (hook icon, lit LED) when the inbound
 * NOTIFY body for Event: presence carries activity elements inside
 * <dm:person>/<e:activities>, including the Cisco-private extensions
 * <ce:alerting/> (ringing) and <ce:dnd/>. State and element mapping
 * mirror the cisco-usecallmanager chan_sip patch's
 * sip_request_send_notify_with_extension_state.
 *
 * This module ships TWO things:
 *
 * 1. A body **supplement** registered for application/pidf+xml.
 *    Stock res_pjsip_pidf_body_generator already builds the basic
 *    <presence>/<note>/<tuple>/<status> PIDF; we add
 *    <dm:person>/<e:activities> with the RPID + Cisco rpid elements
 *    on top of it. Stock and us coexist cleanly: no noload, no
 *    runtime unload, no slot conflict.
 *
 * 2. A body **generator** registered for application/cpim-pidf+xml,
 *    plus a matching supplement that decorates the body it produces.
 *    Cisco firmware advertises only application/cpim-pidf+xml in its
 *    SUBSCRIBE Accept header; stock res_pjsip_xpidf_body_generator
 *    would respond with the older RFC 2779 XPIDF format which
 *    doesn't carry RPID activities. We hijack the subtype label and
 *    emit the same modern-PIDF body (built by re-using pjpidf the
 *    same way stock pidf does), which Cisco accepts. Pubsub allows
 *    only one generator per content type, so we set load_pri lower
 *    than stock xpidf's so we register first; stock xpidf logs one
 *    WARNING at startup ("already registered") and declines.
 *
 * Side effect: PIDF NOTIFYs to *non-Cisco* watchers also pick up the
 * <dm:person>/<e:activities> additions. That is RFC-clean (RPID is
 * standard) and other watchers ignore Cisco's xmlns:ce extensions.
 */

/*** MODULEINFO
	<depend>pjproject</depend>
	<depend>res_pjsip</depend>
	<depend>res_pjsip_pubsub</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_simple.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/pbx.h"
#include "asterisk/presencestate.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_pubsub.h"
#include "asterisk/res_pjsip_presence_xml.h"
#include "asterisk/res_pjsip_body_generator_types.h"

#define XMLNS_DM      "urn:ietf:params:xml:ns:pidf:data-model"
#define XMLNS_E_RPID  "urn:ietf:params:xml:ns:pidf:status:rpid"
#define XMLNS_CE_RPID "urn:cisco:params:xml:ns:pidf:rpid"

/*
 * Add an xmlns prefix attribute to a node only if the same prefix
 * isn't already declared. The stock pidf eyebeam supplement
 * (res_pjsip_pidf_eyebeam_body_supplement) declares xmlns:dm itself;
 * if it loaded ahead of us we'd otherwise emit the attribute twice and
 * produce malformed XML.
 */
static void xmlns_if_absent(pj_pool_t *pool, pj_xml_node *node,
	const char *prefix, const char *value)
{
	pj_str_t name;

	if (pj_xml_find_attr(node, pj_cstr(&name, prefix), NULL)) {
		return;
	}
	ast_sip_presence_xml_create_attr(pool, node, prefix, value);
}

/*
 * The supplement: runs after a generator built a pjpidf_pres body.
 * Adds <dm:person>/<e:activities> with RFC 4480 RPID activity elements
 * + Cisco's private xmlns:ce extensions.
 */
static int cisco_pidf_supplement_body(void *body, void *data)
{
	pjpidf_pres *pres = body;
	struct ast_sip_exten_state_data *state_data = data;
	pj_xml_node *person, *activities;
	int exten_state = state_data->exten_state;
	int presence_state = state_data->presence_state;

	xmlns_if_absent(state_data->pool, pres, "xmlns:dm", XMLNS_DM);
	xmlns_if_absent(state_data->pool, pres, "xmlns:e",  XMLNS_E_RPID);
	xmlns_if_absent(state_data->pool, pres, "xmlns:ce", XMLNS_CE_RPID);

	person = ast_sip_presence_xml_create_node(state_data->pool, pres, "dm:person");
	if (!person) {
		return -1;
	}
	activities = ast_sip_presence_xml_create_node(state_data->pool, person, "e:activities");
	if (!activities) {
		return -1;
	}

	/* State -> activity element mapping. Mostly mirrors the chan_sip
	 * cisco-usecallmanager patch (channels/sip/request.c:556-568):
	 *   INUSE | ONHOLD | BUSY         -> <e:on-the-phone/>   (RFC 4480)
	 *   BUSY                          -> <e:busy/>           (RFC 4480, in addition)
	 *   RINGING                       -> <ce:alerting/>      (Cisco-private)
	 *   presence == AST_PRESENCE_DND  -> <ce:dnd/>           (Cisco-private)
	 *
	 * Deliberate divergence from chan_sip: the upstream patch emits
	 * <ce:alerting/> whenever RINGING is set even if the line is
	 * already INUSE/BUSY/ONHOLD or the watched extension is DND. That
	 * makes a busy line flash 'alerting' on every other phone's BLF
	 * when a second call arrives, which reads as "available to pick
	 * up" to operators — the opposite of what's actually true.
	 * Suppress <ce:alerting/> when the line is already engaged or DND
	 * so the BLF stays on on-the-phone / dnd through the whole
	 * second-call setup. */
	if (exten_state & (AST_EXTENSION_INUSE
			| AST_EXTENSION_ONHOLD
			| AST_EXTENSION_BUSY)) {
		ast_sip_presence_xml_create_node(state_data->pool, activities, "e:on-the-phone");
	} else if (presence_state != AST_PRESENCE_DND
			&& (exten_state & AST_EXTENSION_RINGING)) {
		ast_sip_presence_xml_create_node(state_data->pool, activities, "ce:alerting");
	}

	if (exten_state & AST_EXTENSION_BUSY) {
		ast_sip_presence_xml_create_node(state_data->pool, activities, "e:busy");
	}

	if (presence_state == AST_PRESENCE_DND) {
		ast_sip_presence_xml_create_node(state_data->pool, activities, "ce:dnd");
	}

	return 0;
}

/*
 * cpim-pidf+xml generator. Body is identical to stock pidf+xml — same
 * pjpidf_pres tree, same pjpidf_print serialisation. Only the
 * Content-Type label on the wire differs. We register this so Cisco
 * firmware (which Accepts only cpim-pidf+xml) gets a body our
 * supplement can decorate; stock xpidf would emit the older RFC 2779
 * format that lacks RPID and Cisco doesn't render it as BLF.
 */
static void *cisco_cpim_allocate_body(void *data)
{
	struct ast_sip_exten_state_data *state_data = data;
	char *local = ast_strdupa(state_data->local);
	pj_str_t entity;

	return pjpidf_create(state_data->pool,
		pj_cstr(&entity, ast_strip_quoted(local, "<", ">")));
}

static int cisco_cpim_generate_body_content(void *body, void *data)
{
	pjpidf_pres *pres = body;
	struct ast_sip_exten_state_data *state_data = data;
	pjpidf_tuple *tuple;
	pj_str_t note, id, contact, priority;
	char *statestring = NULL, *pidfstate = NULL, *pidfnote = NULL;
	enum ast_sip_pidf_state local_state;
	char sanitized[PJSIP_MAX_URL_SIZE];

	ast_sip_presence_exten_state_to_str(state_data->exten_state, &statestring,
			&pidfstate, &pidfnote, &local_state, 0);

	if (!pjpidf_pres_add_note(state_data->pool, pres, pj_cstr(&note, pidfnote))) {
		ast_log(LOG_WARNING, "Unable to add note to PIDF presence\n");
		return -1;
	}
	if (!(tuple = pjpidf_pres_add_tuple(state_data->pool, pres,
			pj_cstr(&id, state_data->exten)))) {
		ast_log(LOG_WARNING, "Unable to create PIDF tuple\n");
		return -1;
	}
	ast_sip_sanitize_xml(state_data->remote, sanitized, sizeof(sanitized));
	pjpidf_tuple_set_contact(state_data->pool, tuple, pj_cstr(&contact, sanitized));
	pjpidf_tuple_set_contact_prio(state_data->pool, tuple, pj_cstr(&priority, "1"));
	pjpidf_status_set_basic_open(pjpidf_tuple_get_status(tuple),
			local_state == NOTIFY_OPEN || local_state == NOTIFY_INUSE);

	return 0;
}

#define MAX_STRING_GROWTHS 5

static void cisco_cpim_to_string(void *body, struct ast_str **str)
{
	pjpidf_pres *pres = body;
	int growths = 0;
	int size;

	do {
		size = pjpidf_print(pres, ast_str_buffer(*str), ast_str_size(*str) - 1);
		if (size <= AST_PJSIP_XML_PROLOG_LEN) {
			ast_str_make_space(str, ast_str_size(*str) * 2);
			++growths;
		}
	} while (size <= AST_PJSIP_XML_PROLOG_LEN && growths < MAX_STRING_GROWTHS);
	if (size <= AST_PJSIP_XML_PROLOG_LEN) {
		ast_log(LOG_WARNING, "PIDF body text too large\n");
		return;
	}
	*(ast_str_buffer(*str) + size) = '\0';
	ast_str_update(*str);
}

static struct ast_sip_pubsub_body_generator cisco_cpim_pidf_body_generator = {
	.type = "application",
	.subtype = "cpim-pidf+xml",
	.body_type = AST_SIP_EXTEN_STATE_DATA,
	.allocate_body = cisco_cpim_allocate_body,
	.generate_body_content = cisco_cpim_generate_body_content,
	.to_string = cisco_cpim_to_string,
	/* No destroy_body — pj pool cleans up */
};

static struct ast_sip_pubsub_body_supplement cisco_pidf_supplement = {
	.type = "application",
	.subtype = "pidf+xml",
	.supplement_body = cisco_pidf_supplement_body,
};

static struct ast_sip_pubsub_body_supplement cisco_cpim_pidf_supplement = {
	.type = "application",
	.subtype = "cpim-pidf+xml",
	.supplement_body = cisco_pidf_supplement_body,
};

static int load_module(void)
{
	if (ast_sip_pubsub_register_body_generator(&cisco_cpim_pidf_body_generator)) {
		ast_log(LOG_ERROR,
			"Cisco PIDF: cpim-pidf+xml generator could not register. "
			"This means stock res_pjsip_xpidf_body_generator.so registered "
			"first (load order surprise). Either noload that module or "
			"raise this module's load_pri.\n");
		return AST_MODULE_LOAD_DECLINE;
	}
	if (ast_sip_pubsub_register_body_supplement(&cisco_pidf_supplement)) {
		ast_sip_pubsub_unregister_body_generator(&cisco_cpim_pidf_body_generator);
		return AST_MODULE_LOAD_DECLINE;
	}
	if (ast_sip_pubsub_register_body_supplement(&cisco_cpim_pidf_supplement)) {
		ast_sip_pubsub_unregister_body_supplement(&cisco_pidf_supplement);
		ast_sip_pubsub_unregister_body_generator(&cisco_cpim_pidf_body_generator);
		return AST_MODULE_LOAD_DECLINE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	/* Refuse runtime unload — pubsub caches body-generator/supplement
	 * pointers across active subscriptions; pulling them out from
	 * under live SUBSCRIBE state caused use-after-free crashes early
	 * in development. Same guard pattern as res_pjsip_mwi.c. */
	if (!ast_shutdown_final()) {
		return -1;
	}
	ast_sip_pubsub_unregister_body_supplement(&cisco_cpim_pidf_supplement);
	ast_sip_pubsub_unregister_body_supplement(&cisco_pidf_supplement);
	ast_sip_pubsub_unregister_body_generator(&cisco_cpim_pidf_body_generator);
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco-flavoured PIDF Body Generator",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	/* Lower than AST_MODPRI_CHANNEL_DEPEND (60) so our cpim-pidf+xml
	 * generator registers BEFORE stock res_pjsip_xpidf_body_generator
	 * tries to. Stock xpidf then logs one "already registered" WARNING
	 * at startup and declines. Operator needs no noload directive. */
	.load_pri = AST_MODPRI_CHANNEL_DEPEND - 5,
	.requires = "res_pjsip,res_pjsip_pubsub",
);
