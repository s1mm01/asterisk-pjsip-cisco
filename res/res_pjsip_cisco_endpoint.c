/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_endpoint
 *
 * Registers a 'cisco' sorcery type on the PJSIP sorcery instance so
 * Cisco-specific config can live alongside the standard endpoint /
 * aor / auth sections in pjsip.conf:
 *
 *     [1010]
 *     type            = endpoint
 *     ...
 *
 *     [1010]
 *     type            = cisco
 *     line_index      = 1
 *     subscribe       = 1001,1002,1003
 *
 * The cisco_* family of modules
 * (res_pjsip_cisco_register_optionsind, res_pjsip_cisco_bulkupdate,
 * res_pjsip_cisco_unsolicited_blf) gate their behaviour on the
 * existence of a [name] type=cisco section for the registering
 * endpoint. Endpoints without one fall through unchanged.
 *
 * The body generator (res_pjsip_cisco_pidf_body_generator) is global
 * — it serves Cisco-flavoured PIDF to ANY endpoint subscribing to
 * presence — because non-Cisco SIP softphones consuming PIDF will
 * simply ignore the additional rpid activity elements. Adding a
 * per-endpoint gate there would needlessly fragment the body
 * generator; if you want stock PIDF for non-Cisco endpoints,
 * `noload` this set of modules entirely instead.
 */

/*** MODULEINFO
	<depend>res_pjsip</depend>
	<support_level>extended</support_level>
 ***/

/*** DOCUMENTATION
	<configInfo name="res_pjsip_cisco_endpoint" language="en_US">
		<synopsis>Cisco-specific per-endpoint configuration for chan_pjsip</synopsis>
		<description><para>
			Adds a 'cisco' sorcery type. Sibling res_pjsip_cisco_*
			modules (register_optionsind, bulkupdate,
			unsolicited_blf, remotecc) consult this object to decide whether
			to apply Cisco-specific REGISTER-time behaviour to a
			given endpoint, and to source per-endpoint values
			(line_index, subscribe list).
		</para></description>
		<configFile name="pjsip.conf">
			<configObject name="cisco">
				<synopsis>Cisco Enterprise SIP firmware behaviour for an endpoint</synopsis>
				<description><para>
					Section name MUST match the endpoint
					section name. Existence of this section is
					the gating signal for Cisco-specific
					behaviour; endpoints without one are
					treated as non-Cisco.
				</para></description>
				<configOption name="type">
					<synopsis>Must be 'cisco'.</synopsis>
				</configOption>
				<configOption name="line_index" default="1">
					<synopsis>Cisco line button index for the
					primary line on the phone.</synopsis>
				</configOption>
				<configOption name="subscribe" default="">
					<synopsis>Comma-separated list of extensions
					whose state is pushed to the phone via
					unsolicited NOTIFY at REGISTER time.</synopsis>
				</configOption>
				<configOption name="subscribe_context" default="local_sip_phone">
					<synopsis>Dialplan context to look up the
					subscribe= extensions in.</synopsis>
				</configOption>
				<configOption name="dnd_busy" default="no">
					<synopsis>When DND is enabled on the phone,
					reject calls with busy (yes) vs let the call
					through with the ringer off (no). The on/off
					state of DND itself is read at runtime from
					astdb key DND/&lt;endpoint-id&gt;, so a dialplan
					feature code can toggle it.</synopsis>
				</configOption>
				<configOption name="aliases" default="">
					<synopsis>Comma-separated list of OTHER
					cisco-endpoint IDs that share the same physical
					phone (multi-line phone, one endpoint per line
					button). Empty (default) = single-line phone.
					When set, the bulkupdate REFER emits a
					&lt;contact line="N"&gt; element for each alias,
					sourcing line_index from each alias's own
					cisco sorcery object.</synopsis>
				</configOption>
				<configOption name="parkext" default="700">
					<synopsis>Dialplan extension the RemoteCC Park
					softkey blind-transfers the call to — i.e.
					res_parking's parkext. Change to match parkext
					in res_parking.conf if a site uses a different
					one. The context is resolved per call
					(TRANSFER_CONTEXT chan var, else the endpoint's
					context).</synopsis>
				</configOption>
			</configObject>
		</configFile>
	</configInfo>
 ***/

#include "asterisk.h"

#include "asterisk/module.h"
#include "asterisk/sorcery.h"
#include "asterisk/stringfields.h"
#include "asterisk/astobj2.h"
#include "asterisk/utils.h"
#include "asterisk/res_pjsip.h"

#include "cisco_endpoint.h"

static void cisco_endpoint_destructor(void *obj)
{
	struct cisco_endpoint *cisco = obj;
	ast_string_field_free_memory(cisco);
}

static void *cisco_endpoint_alloc(const char *name)
{
	struct cisco_endpoint *cisco;

	cisco = ast_sorcery_generic_alloc(sizeof(*cisco), cisco_endpoint_destructor);
	if (!cisco) {
		return NULL;
	}
	if (ast_string_field_init(cisco, 128)) {
		ao2_cleanup(cisco);
		return NULL;
	}
	cisco->line_index = 1;
	return cisco;
}

/*
 * Note: cisco_endpoint_get() is implemented inline in cisco_endpoint.h,
 * not here. See that header for rationale.
 */

static int load_module(void)
{
	struct ast_sorcery *sorcery = ast_sip_get_sorcery();

	if (!sorcery) {
		ast_log(LOG_ERROR, "cisco_endpoint: pjsip sorcery instance not available\n");
		return AST_MODULE_LOAD_DECLINE;
	}

	ast_sorcery_apply_default(sorcery, "cisco", "config",
		"pjsip.conf,criteria=type=cisco");

	if (ast_sorcery_object_register(sorcery, "cisco", cisco_endpoint_alloc,
			NULL, NULL)) {
		ast_log(LOG_ERROR, "cisco_endpoint: failed to register sorcery type\n");
		return AST_MODULE_LOAD_DECLINE;
	}

	ast_sorcery_object_field_register(sorcery, "cisco", "type", "",
		OPT_NOOP_T, 0, 0);
	ast_sorcery_object_field_register(sorcery, "cisco", "line_index", "1",
		OPT_INT_T, 0, FLDSET(struct cisco_endpoint, line_index));
	ast_sorcery_object_field_register(sorcery, "cisco", "subscribe", "",
		OPT_STRINGFIELD_T, 0, STRFLDSET(struct cisco_endpoint, subscribe));
	ast_sorcery_object_field_register(sorcery, "cisco", "subscribe_context",
		"local_sip_phone", OPT_STRINGFIELD_T, 0,
		STRFLDSET(struct cisco_endpoint, subscribe_context));
	ast_sorcery_object_field_register(sorcery, "cisco", "dnd_busy", "no",
		OPT_BOOL_T, 1, FLDSET(struct cisco_endpoint, dnd_busy));
	ast_sorcery_object_field_register(sorcery, "cisco", "aliases", "",
		OPT_STRINGFIELD_T, 0, STRFLDSET(struct cisco_endpoint, aliases));
	ast_sorcery_object_field_register(sorcery, "cisco", "parkext", "700",
		OPT_STRINGFIELD_T, 0, STRFLDSET(struct cisco_endpoint, parkext));

	ast_sorcery_load_object(sorcery, "cisco");

	return AST_MODULE_LOAD_SUCCESS;
}

static int reload_module(void)
{
	ast_sorcery_reload_object(ast_sip_get_sorcery(), "cisco");
	return 0;
}

static int unload_module(void)
{
	/*
	 * Refuse runtime unload. Sorcery does not provide a clean way to
	 * unregister a type, and sibling cisco_* modules hold references
	 * to objects of this type. Letting this module unload while sibling
	 * modules are still running guarantees crashes. Reloading the
	 * configuration is supported via the reload_module callback.
	 */
	if (!ast_shutdown_final()) {
		return -1;
	}
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco endpoint sorcery type",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.reload = reload_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND - 1,
	.requires = "res_pjsip",
);
