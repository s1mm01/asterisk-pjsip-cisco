/*
 * CLI verbs for res_pjsip_cisco_bulkupdate.so, split out from
 * res_pjsip_cisco_bulkupdate.c:
 *
 *   pjsip cisco bulkupdate    <endpoint>
 *   pjsip cisco donotdisturb  {on,off} <endpoint>
 *   pjsip cisco huntgroup     {on,off} <endpoint>
 *   pjsip cisco callforward   on  <endpoint> <target>
 *   pjsip cisco callforward   off <endpoint>
 *
 * Each one updates an astdb feature-state key and queues a fresh
 * bulkupdate REFER via cisco_bulkupdate_queue_refer() so the phone's
 * UI reflects the change immediately. For DND, cisco_dnd_set() also
 * fires ast_presence_state_changed so BLF-watcher lamps update.
 *
 * Mirrors chan_sip cisco-usecallmanager's sip
 * {donotdisturb,huntgroup,callforward} commands
 * (https://usecallmanager.nz/command-line.html).
 */

#include "asterisk.h"

#include "asterisk/module.h"
#include "asterisk/cli.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"

#include "cisco/endpoint.h"
#include "bulkupdate_private.h"

static char *cli_cisco_bulkupdate(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	struct ast_sip_endpoint *endpoint;

	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip cisco bulkupdate";
		e->usage =
			"Usage: pjsip cisco bulkupdate <endpoint>\n"
			"   Send a Cisco bulkupdate REFER to <endpoint> carrying the\n"
			"   current astdb DND / HuntGroup / call-forward state plus\n"
			"   live MWI counts. Use after dialplan / AMI writes to the\n"
			"   DND/<endpoint> or CF/<endpoint> astdb keys to push the\n"
			"   new state to the phone's local UI.\n";
		return NULL;
	case CLI_GENERATE:
		return a->pos == 3 ? cisco_bulkupdate_complete_endpoint(a->word, a->n)
			: NULL;
	}

	if (a->argc != 4) {
		return CLI_SHOWUSAGE;
	}

	endpoint = cisco_bulkupdate_resolve_endpoint(a->fd, a->argv[3]);
	if (!endpoint) {
		return CLI_FAILURE;
	}
	if (ast_strlen_zero(endpoint->aors)) {
		ast_cli(a->fd, "Endpoint '%s' has no AORs configured\n", a->argv[3]);
		ao2_cleanup(endpoint);
		return CLI_FAILURE;
	}
	if (cisco_bulkupdate_queue_refer(a->fd, endpoint)) {
		ao2_cleanup(endpoint);
		return CLI_FAILURE;
	}
	ast_cli(a->fd, "Bulkupdate REFER queued for '%s'\n", a->argv[3]);
	ao2_cleanup(endpoint);
	return CLI_SUCCESS;
}

/* DND on/off — argv = ["pjsip","cisco","donotdisturb",{"on"|"off"},<ext>]. */
static char *cli_cisco_donotdisturb_run(struct ast_cli_args *a, int enable)
{
	struct ast_sip_endpoint *endpoint;

	if (a->argc != 5) {
		return CLI_SHOWUSAGE;
	}
	endpoint = cisco_bulkupdate_resolve_endpoint(a->fd, a->argv[4]);
	if (!endpoint) {
		return CLI_FAILURE;
	}
	cisco_dnd_set(a->argv[4], enable);
	ast_cli(a->fd, "DND %s for endpoint '%s'\n",
		enable ? "enabled" : "disabled", a->argv[4]);
	cisco_bulkupdate_queue_refer(a->fd, endpoint);
	ao2_cleanup(endpoint);
	return CLI_SUCCESS;
}

static char *cli_cisco_donotdisturb_on(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip cisco donotdisturb on";
		e->usage =
			"Usage: pjsip cisco donotdisturb on <endpoint>\n"
			"   Enable DND for <endpoint>. Updates DND/<endpoint> in\n"
			"   astdb, fires a PJSIP:<endpoint> presence change so BLF\n"
			"   watcher lamps update, and queues a bulkupdate REFER so\n"
			"   the phone's own DND glyph turns on.\n";
		return NULL;
	case CLI_GENERATE:
		return a->pos == 4 ? cisco_bulkupdate_complete_endpoint(a->word, a->n)
			: NULL;
	}
	return cli_cisco_donotdisturb_run(a, 1);
}

static char *cli_cisco_donotdisturb_off(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip cisco donotdisturb off";
		e->usage =
			"Usage: pjsip cisco donotdisturb off <endpoint>\n"
			"   Disable DND for <endpoint> (counterpart to "
			"`donotdisturb on`).\n";
		return NULL;
	case CLI_GENERATE:
		return a->pos == 4 ? cisco_bulkupdate_complete_endpoint(a->word, a->n)
			: NULL;
	}
	return cli_cisco_donotdisturb_run(a, 0);
}

/* HuntGroup on/off — argv = ["pjsip","cisco","huntgroup",{"on"|"off"},<ext>]. */
static char *cli_cisco_huntgroup_run(struct ast_cli_args *a, int login)
{
	struct ast_sip_endpoint *endpoint;

	if (a->argc != 5) {
		return CLI_SHOWUSAGE;
	}
	endpoint = cisco_bulkupdate_resolve_endpoint(a->fd, a->argv[4]);
	if (!endpoint) {
		return CLI_FAILURE;
	}
	cisco_huntgroup_set(a->argv[4], login);
	ast_cli(a->fd, "HuntGroup %s for endpoint '%s'\n",
		login ? "logged in" : "logged out", a->argv[4]);
	cisco_bulkupdate_queue_refer(a->fd, endpoint);
	ao2_cleanup(endpoint);
	return CLI_SUCCESS;
}

static char *cli_cisco_huntgroup_on(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip cisco huntgroup on";
		e->usage =
			"Usage: pjsip cisco huntgroup on <endpoint>\n"
			"   Log <endpoint> into its hunt group (sets HuntGroup/<endpoint>\n"
			"   in astdb and pushes a bulkupdate REFER so the HLog softkey\n"
			"   on the phone reflects the change).\n";
		return NULL;
	case CLI_GENERATE:
		return a->pos == 4 ? cisco_bulkupdate_complete_endpoint(a->word, a->n)
			: NULL;
	}
	return cli_cisco_huntgroup_run(a, 1);
}

static char *cli_cisco_huntgroup_off(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip cisco huntgroup off";
		e->usage =
			"Usage: pjsip cisco huntgroup off <endpoint>\n"
			"   Log <endpoint> out of its hunt group (counterpart to "
			"`huntgroup on`).\n";
		return NULL;
	case CLI_GENERATE:
		return a->pos == 4 ? cisco_bulkupdate_complete_endpoint(a->word, a->n)
			: NULL;
	}
	return cli_cisco_huntgroup_run(a, 0);
}

/* Call-forward on <endpoint> <target> / off <endpoint>. */
static char *cli_cisco_callforward_on(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	struct ast_sip_endpoint *endpoint;

	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip cisco callforward on";
		e->usage =
			"Usage: pjsip cisco callforward on <endpoint> <target>\n"
			"   Set CFwdALL on <endpoint> to ring <target>. Updates\n"
			"   CF/<endpoint> in astdb and pushes a bulkupdate REFER so\n"
			"   the phone shows the call-forward banner.\n";
		return NULL;
	case CLI_GENERATE:
		return a->pos == 4 ? cisco_bulkupdate_complete_endpoint(a->word, a->n)
			: NULL;
	}

	if (a->argc != 6) {
		return CLI_SHOWUSAGE;
	}
	endpoint = cisco_bulkupdate_resolve_endpoint(a->fd, a->argv[4]);
	if (!endpoint) {
		return CLI_FAILURE;
	}
	cisco_cfwd_set(a->argv[4], a->argv[5]);
	ast_cli(a->fd, "CFwdALL for '%s' set to %s\n", a->argv[4], a->argv[5]);
	cisco_bulkupdate_queue_refer(a->fd, endpoint);
	ao2_cleanup(endpoint);
	return CLI_SUCCESS;
}

static char *cli_cisco_callforward_off(struct ast_cli_entry *e, int cmd,
	struct ast_cli_args *a)
{
	struct ast_sip_endpoint *endpoint;

	switch (cmd) {
	case CLI_INIT:
		e->command = "pjsip cisco callforward off";
		e->usage =
			"Usage: pjsip cisco callforward off <endpoint>\n"
			"   Clear CFwdALL on <endpoint> (deletes CF/<endpoint> in\n"
			"   astdb, pushes a bulkupdate REFER so the phone clears the\n"
			"   call-forward banner).\n";
		return NULL;
	case CLI_GENERATE:
		return a->pos == 4 ? cisco_bulkupdate_complete_endpoint(a->word, a->n)
			: NULL;
	}

	if (a->argc != 5) {
		return CLI_SHOWUSAGE;
	}
	endpoint = cisco_bulkupdate_resolve_endpoint(a->fd, a->argv[4]);
	if (!endpoint) {
		return CLI_FAILURE;
	}
	cisco_cfwd_set(a->argv[4], NULL);
	ast_cli(a->fd, "CFwdALL cleared for '%s'\n", a->argv[4]);
	cisco_bulkupdate_queue_refer(a->fd, endpoint);
	ao2_cleanup(endpoint);
	return CLI_SUCCESS;
}

static struct ast_cli_entry bulkupdate_cli_cmds[] = {
	AST_CLI_DEFINE(cli_cisco_bulkupdate,
		"Push a fresh Cisco bulkupdate REFER (DND/CF/MWI state)"),
	AST_CLI_DEFINE(cli_cisco_donotdisturb_on,  "Enable Cisco DND for an endpoint"),
	AST_CLI_DEFINE(cli_cisco_donotdisturb_off, "Disable Cisco DND for an endpoint"),
	AST_CLI_DEFINE(cli_cisco_huntgroup_on,     "Log a Cisco endpoint into its hunt group"),
	AST_CLI_DEFINE(cli_cisco_huntgroup_off,    "Log a Cisco endpoint out of its hunt group"),
	AST_CLI_DEFINE(cli_cisco_callforward_on,   "Set Cisco CFwdALL target for an endpoint"),
	AST_CLI_DEFINE(cli_cisco_callforward_off,  "Clear Cisco CFwdALL on an endpoint"),
};

int cisco_bulkupdate_cli_init(void)
{
	ast_cli_register_multiple(bulkupdate_cli_cmds,
		ARRAY_LEN(bulkupdate_cli_cmds));
	return 0;
}

void cisco_bulkupdate_cli_shutdown(void)
{
	ast_cli_unregister_multiple(bulkupdate_cli_cmds,
		ARRAY_LEN(bulkupdate_cli_cmds));
}
