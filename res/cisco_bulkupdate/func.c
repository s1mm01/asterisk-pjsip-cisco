/*
 * CISCO_DND / CISCO_HUNTGROUP / CISCO_CALLFORWARD dialplan functions,
 * split out from res_pjsip_cisco_bulkupdate.c.
 *
 * Same astdb-write semantics as the matching `pjsip cisco …` CLI
 * verbs in res/cisco_bulkupdate/cli.c — and the write paths queue a
 * bulkupdate REFER via cisco_bulkupdate_queue_refer() so the phone's
 * own DND glyph / HLog softkey / CFwdALL banner reflects the change
 * without waiting for the next REGISTER. cisco_dnd_set()
 * additionally fires ast_presence_state_changed for BLF watchers
 * (cisco/endpoint.h).
 *
 * These live in the bulkupdate .so rather than feature_events (which
 * owns the phone→server SUBSCRIBE/PUBLISH paths) because
 * bulkupdate_serializer + bulkupdate_send_task are static to this
 * .so, and CLAUDE.md's "no cross-module symbol exports" convention
 * means the write paths can't reach them from another module.
 *
 * Every write path goes through cisco_func_resolve() which rejects
 * an empty argument and any endpoint id with no [name] type=cisco
 * section. Without that contract a dialplan typo would create orphan
 * DND/<typo> / HuntGroup/<typo> / CF/<typo> astdb entries and (for
 * DND) publish a PJSIP:<typo> presence event that lingers in stasis
 * cache. Reads use cisco_func_validate() (no endpoint object needed).
 */

/*** DOCUMENTATION
	<function name="CISCO_DND" language="en_US">
		<synopsis>
			Read or set the Cisco DND state for an endpoint.
		</synopsis>
		<syntax>
			<parameter name="endpoint" required="true">
				<para>The cisco endpoint id (must have a
				[name] type=cisco section in pjsip.conf).</para>
			</parameter>
		</syntax>
		<description>
			<para>Read returns <literal>YES</literal> when DND is
			enabled, an empty string otherwise.</para>
			<para>Write accepts any boolean Asterisk understands
			(<literal>on</literal>/<literal>off</literal>,
			<literal>yes</literal>/<literal>no</literal>,
			<literal>1</literal>/<literal>0</literal>,
			<literal>true</literal>/<literal>false</literal>).
			Writes update the <literal>DND/&lt;endpoint&gt;</literal>
			astdb key, fire an
			<literal>ast_presence_state_changed</literal> on the
			<literal>PJSIP:&lt;endpoint&gt;</literal> provider so BLF
			hints whose presence component is
			<literal>PJSIP:&lt;endpoint&gt;</literal> re-NOTIFY their
			watchers, and queue a Cisco bulkupdate REFER to the
			endpoint so its own DND glyph updates without waiting
			for the next REGISTER.</para>
			<example title="Server-side DND toggle feature code">
exten => *78,1,Set(CISCO_DND(${CALLERID(num)})=YES)
 same =>      ,n,Playback(do-not-disturb&amp;activated)
exten => *79,1,Set(CISCO_DND(${CALLERID(num)})=NO)
 same =>      ,n,Playback(do-not-disturb&amp;de-activated)
			</example>
		</description>
	</function>
	<function name="CISCO_HUNTGROUP" language="en_US">
		<synopsis>
			Read or set the Cisco hunt-group login state for an endpoint.
		</synopsis>
		<syntax>
			<parameter name="endpoint" required="true">
				<para>The cisco endpoint id.</para>
			</parameter>
		</syntax>
		<description>
			<para>Read returns <literal>YES</literal> when the
			endpoint is logged into its hunt group, empty
			otherwise. Write takes a boolean, updates
			<literal>HuntGroup/&lt;endpoint&gt;</literal> in astdb,
			and queues a Cisco bulkupdate REFER so the phone's HLog
			softkey UI reflects the change.</para>
		</description>
	</function>
	<function name="CISCO_CALLFORWARD" language="en_US">
		<synopsis>
			Read or set the Cisco call-forward-all target for an endpoint.
		</synopsis>
		<syntax>
			<parameter name="endpoint" required="true">
				<para>The cisco endpoint id.</para>
			</parameter>
		</syntax>
		<description>
			<para>Read returns the current
			<literal>CF/&lt;endpoint&gt;</literal> target (empty
			when call-forward is off). Write to a non-empty target
			to enable forwarding; write an empty value (or any
			Asterisk-recognised false value: <literal>off</literal>,
			<literal>no</literal>, <literal>0</literal>,
			<literal>false</literal>) to clear it. Either way queues
			a Cisco bulkupdate REFER so the phone's CFwdALL banner
			updates immediately.</para>
		</description>
	</function>
 ***/

#include "asterisk.h"

#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/channel.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"

#include "cisco/endpoint.h"
#include "bulkupdate_private.h"

static int cisco_func_validate(const char *cmd, const char *data)
{
	struct cisco_endpoint *cisco;

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "%s requires an endpoint id\n", cmd);
		return -1;
	}
	cisco = cisco_endpoint_get(data);
	if (!cisco) {
		ast_log(LOG_WARNING,
			"%s: endpoint '%s' has no [name] type=cisco section "
			"in pjsip.conf — refusing to mutate astdb\n", cmd, data);
		return -1;
	}
	ao2_cleanup(cisco);
	return 0;
}

/* Validate + retrieve the matching ast_sip_endpoint (caller ao2_cleanups
 * on success; NULL with LOG_WARNING on miss). Write paths need the
 * endpoint object to feed cisco_bulkupdate_queue_refer. */
static struct ast_sip_endpoint *cisco_func_resolve(const char *cmd, const char *data)
{
	struct ast_sip_endpoint *endpoint;

	if (cisco_func_validate(cmd, data)) {
		return NULL;
	}
	endpoint = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(),
		"endpoint", data);
	if (!endpoint) {
		ast_log(LOG_WARNING,
			"%s: endpoint '%s' has a cisco section but no matching "
			"type=endpoint section — config drift; astdb update "
			"skipped\n", cmd, data);
	}
	return endpoint;
}

static int cisco_dnd_func_read(struct ast_channel *chan, const char *cmd,
	char *data, char *buf, size_t buflen)
{
	if (cisco_func_validate(cmd, data)) {
		return -1;
	}
	ast_copy_string(buf, cisco_dnd_is_enabled(data) ? "YES" : "", buflen);
	return 0;
}

static int cisco_dnd_func_write(struct ast_channel *chan, const char *cmd,
	char *data, const char *value)
{
	struct ast_sip_endpoint *endpoint = cisco_func_resolve(cmd, data);

	if (!endpoint) {
		return -1;
	}
	cisco_dnd_set(data, ast_true(value));
	cisco_bulkupdate_queue_refer(-1, endpoint);
	ao2_cleanup(endpoint);
	return 0;
}

static struct ast_custom_function cisco_dnd_function = {
	.name  = "CISCO_DND",
	.read  = cisco_dnd_func_read,
	.write = cisco_dnd_func_write,
};

static int cisco_huntgroup_func_read(struct ast_channel *chan, const char *cmd,
	char *data, char *buf, size_t buflen)
{
	if (cisco_func_validate(cmd, data)) {
		return -1;
	}
	ast_copy_string(buf, cisco_huntgroup_is_in(data) ? "YES" : "", buflen);
	return 0;
}

static int cisco_huntgroup_func_write(struct ast_channel *chan, const char *cmd,
	char *data, const char *value)
{
	struct ast_sip_endpoint *endpoint = cisco_func_resolve(cmd, data);

	if (!endpoint) {
		return -1;
	}
	cisco_huntgroup_set(data, ast_true(value));
	cisco_bulkupdate_queue_refer(-1, endpoint);
	ao2_cleanup(endpoint);
	return 0;
}

static struct ast_custom_function cisco_huntgroup_function = {
	.name  = "CISCO_HUNTGROUP",
	.read  = cisco_huntgroup_func_read,
	.write = cisco_huntgroup_func_write,
};

static int cisco_cfwd_func_read(struct ast_channel *chan, const char *cmd,
	char *data, char *buf, size_t buflen)
{
	if (cisco_func_validate(cmd, data)) {
		return -1;
	}
	cisco_cfwd_get(data, buf, buflen);
	return 0;
}

static int cisco_cfwd_func_write(struct ast_channel *chan, const char *cmd,
	char *data, const char *value)
{
	struct ast_sip_endpoint *endpoint = cisco_func_resolve(cmd, data);

	if (!endpoint) {
		return -1;
	}
	/* Treat empty / false-y values as clear; anything else is the
	 * forward target. ast_false catches off/no/0/false; an empty
	 * string falls into the same bucket via ast_strlen_zero. */
	cisco_cfwd_set(data,
		(ast_strlen_zero(value) || ast_false(value)) ? NULL : value);
	cisco_bulkupdate_queue_refer(-1, endpoint);
	ao2_cleanup(endpoint);
	return 0;
}

static struct ast_custom_function cisco_cfwd_function = {
	.name  = "CISCO_CALLFORWARD",
	.read  = cisco_cfwd_func_read,
	.write = cisco_cfwd_func_write,
};

/* Dialplan-function registration is non-fatal in the caller
 * (load_module): the supplement + CLI verbs are the module's main
 * job and stay live even if pbx-function registration trips. Any
 * failure here is logged at WARNING and the partial set rolled
 * back. */
int cisco_bulkupdate_funcs_init(void)
{
	if (ast_custom_function_register(&cisco_dnd_function)
		|| ast_custom_function_register(&cisco_huntgroup_function)
		|| ast_custom_function_register(&cisco_cfwd_function)) {
		ast_custom_function_unregister(&cisco_cfwd_function);
		ast_custom_function_unregister(&cisco_huntgroup_function);
		ast_custom_function_unregister(&cisco_dnd_function);
		ast_log(LOG_WARNING,
			"cisco-bulkupdate: failed to register CISCO_* dialplan "
			"functions; CLI verbs still work\n");
		return -1;
	}
	return 0;
}

void cisco_bulkupdate_funcs_shutdown(void)
{
	ast_custom_function_unregister(&cisco_cfwd_function);
	ast_custom_function_unregister(&cisco_huntgroup_function);
	ast_custom_function_unregister(&cisco_dnd_function);
}
