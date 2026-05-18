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
		</para><para>
			Also registers a <literal>PJSIP</literal> presence-state
			provider so a BLF hint can carry a line's DND state in its
			presence component, e.g.
			<literal>exten => 1010,hint,PJSIP/1010,PJSIP:1010</literal>.
			The colon form (<literal>PJSIP:1010</literal>) reaches this
			provider; the feature-events module fires a presence change
			on every DND toggle so watching phones re-render the lamp.
			Non-Cisco endpoints report <literal>NOT_SET</literal>.
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
				<configOption name="keep_conference" default="no">
					<synopsis>When the Confrn initiator (this phone)
					hangs up while ≥2 other parties are still mixed
					in the conference: no (default, matches chan_sip)
					= tear the conference down, BYE the remaining
					legs. yes = leave the remaining parties talking
					(receptionist-style "drop in, connect them, drop
					out"). Per-endpoint.</synopsis>
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
#include "asterisk/presencestate.h"
#include "asterisk/res_pjsip.h"

#include "cisco/endpoint.h"
#include "cisco/orig_host.h"

/* device.c — internal to this .so; only the entry file needs the
 * lifecycle hooks (the public API in cisco/endpoint.h is what other
 * modules call). Forward-declared here rather than promoted to the
 * public header so they don't pollute the cross-.so surface. */
int cisco_mac_container_init(void);
void cisco_mac_container_shutdown(void);

/*
 * Presence-state provider for DND. Lets a BLF hint of the form
 *
 *     exten => 1010,hint,PJSIP/1010,PJSIP:1010
 *
 * carry the watched line's DND state in its presence half (the second
 * comma-separated component), the same way the chan_sip cisco-usecallmanager
 * patch used SIP/<peer> there. We can't reuse the channel-tech "PJSIP/<id>"
 * spelling for that — core chan_pjsip has no .presencestate callback and we
 * don't patch core — so the hint uses a colon ("PJSIP:1010") to reach this
 * registered provider instead of a slash. cisco_dnd_set() (cisco/endpoint.h)
 * fires ast_presence_state_changed("PJSIP:<id>") on every toggle so live
 * changes propagate; this callback answers the cold-cache / "core show hints"
 * lookups. Endpoints with no [name] type=cisco section report NOT_SET so the
 * provider is inert for non-Cisco peers.
 */
static enum ast_presence_state cisco_dnd_presence_state(const char *data,
	char **subtype, char **message)
{
	struct cisco_endpoint *cisco;

	cisco = cisco_endpoint_get(data);
	if (!cisco) {
		return AST_PRESENCE_NOT_SET;
	}
	ao2_cleanup(cisco);

	return cisco_dnd_is_enabled(data) ? AST_PRESENCE_DND : AST_PRESENCE_NOT_SET;
}

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
 * Note: cisco_endpoint_get() and the other shared cisco_* helpers are
 * implemented in res/res/cisco_endpoint/endpoint.c / res/cisco_endpoint/rdata.c / res/cisco_endpoint/register.c
 * / res/cisco_endpoint/refer.c / res/cisco_endpoint/session.c — all compiled into this same .so.
 * Sibling cisco_* modules pick the symbols up via the dynamic symbol
 * table; this module is loaded with AST_MODFLAG_GLOBAL_SYMBOLS so its
 * exports are visible to subsequent dlopens, exactly as stock res_pjsip
 * exports ast_sip_* to every PJSIP submodule.
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
	ast_sorcery_object_field_register(sorcery, "cisco", "keep_conference",
		"no", OPT_BOOL_T, 1,
		FLDSET(struct cisco_endpoint, keep_conference));

	ast_sorcery_load_object(sorcery, "cisco");

	/* Fatal: cisco_dnd_set() (cisco/endpoint.h) publishes presence
	 * changes for "PJSIP:<endpoint>" unconditionally on every toggle,
	 * which assumes this provider is the canonical answer for that
	 * label. If registration failed, hint cold-cache lookups would
	 * fall through to "no provider found" and BLF DND state would
	 * desync from the cached publications — better to fail load than
	 * ship that inconsistency. In practice the only failure mode is
	 * calloc returning NULL at startup, which is fatal anyway. */
	if (ast_presence_state_prov_add("PJSIP", cisco_dnd_presence_state)) {
		ast_log(LOG_ERROR, "cisco_endpoint: could not register PJSIP "
			"presence-state provider\n");
		return AST_MODULE_LOAD_DECLINE;
	}

	/* Global on_tx_request hook that rewrites NAT'd contacts' RURI +
	 * To-URI back to the phone's self-advertised host:port (saved by
	 * res_pjsip_nat as the x-ast-orig-host URI parameter). No-op when
	 * the parameter isn't present, so it costs nothing for LAN
	 * contacts and trunk-bound traffic. See cisco/orig_host.h. */
	if (cisco_orig_host_register()) {
		ast_log(LOG_ERROR, "cisco_endpoint: could not register "
			"cisco-orig-host-rewrite pjsip module\n");
		ast_presence_state_prov_del("PJSIP");
		return AST_MODULE_LOAD_DECLINE;
	}

	/* Shared cisco_mac_info container, populated at REGISTER time by
	 * res_pjsip_cisco_feature_events.so and read by both that module's
	 * cisco_mac_identify (PATH C) and 'pjsip cisco status'. See
	 * res/cisco_endpoint/device.c. */
	if (cisco_mac_container_init()) {
		ast_log(LOG_ERROR, "cisco_endpoint: could not allocate "
			"cisco_mac_info container\n");
		cisco_orig_host_unregister();
		ast_presence_state_prov_del("PJSIP");
		return AST_MODULE_LOAD_DECLINE;
	}

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
	cisco_mac_container_shutdown();
	cisco_orig_host_unregister();
	ast_presence_state_prov_del("PJSIP");
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER | AST_MODFLAG_GLOBAL_SYMBOLS,
	"PJSIP Cisco endpoint sorcery type",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.reload = reload_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND - 1,
	.requires = "res_pjsip",
);
