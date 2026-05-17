/*
 * StartRecording / StopRecording softkeys, split out from
 * res_pjsip_cisco_remotecc.c.
 *
 * On chan_sip, Gareth's patch creates a second SIP dialog back to the
 * phone with connected-line "Record" and dispatches to dialplan
 * extension "record" — the operator supplies MixMonitor/Monitor there.
 * We don't need the second dialog: in chan_pjsip the call is already
 * on an Asterisk bridge, so we MixMonitor the phone's channel
 * directly. Filename defaults to "cisco-<endpoint>-<uniqueid>.wav"
 * (resolved against the configured MixMonitor directory, normally
 * /var/spool/asterisk/monitor/). Override per-call from dialplan by
 * setting CISCO_RECORD_FILENAME on the channel before the softkey is
 * pressed. The 'b' MixMonitor option means recording is paused when
 * the channel isn't bridged.
 */

#include "asterisk.h"

#include <pjsip.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/astobj2.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/taskprocessor.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/sorcery.h"

#include "cisco/session.h"
#include "remotecc_private.h"

struct record_task_data {
	struct ast_channel *chan;        /* ref'd — the phone's channel */
	char endpoint_id[64];            /* for the log line */
	int start;                       /* 1 = StartRecording, 0 = StopRecording */
};

static void record_task_data_destroy(void *obj)
{
	struct record_task_data *d = obj;

	ast_channel_cleanup(d->chan);
}

static int record_app_task(void *obj)
{
	struct record_task_data *d = obj;

	if (d->start) {
		char filename[512];
		char args[600];
		const char *override;

		ast_channel_lock(d->chan);
		override = pbx_builtin_getvar_helper(d->chan, "CISCO_RECORD_FILENAME");
		if (!ast_strlen_zero(override)) {
			ast_copy_string(filename, override, sizeof(filename));
		} else {
			snprintf(filename, sizeof(filename), "cisco-%s-%s.wav",
				d->endpoint_id, ast_channel_uniqueid(d->chan));
		}
		ast_channel_unlock(d->chan);

		snprintf(args, sizeof(args), "%s,b", filename);
		ast_pbx_exec_application(d->chan, "MixMonitor", args);
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s started recording to %s (channel %s)\n",
			d->endpoint_id, filename, ast_channel_name(d->chan));
	} else {
		ast_pbx_exec_application(d->chan, "StopMixMonitor", "");
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s stopped recording (channel %s)\n",
			d->endpoint_id, ast_channel_name(d->chan));
	}

	ao2_cleanup(d);
	return 0;
}

int handle_record(struct ast_sip_endpoint *endpoint, const char *endpoint_id,
	const struct remotecc_dialog_id *dialog_id, int start)
{
	struct ast_sip_session *session;
	struct ast_channel *chan;
	struct record_task_data *d;
	const char *event_name = start ? "StartRecording" : "StopRecording";

	if (!dialog_id || ast_strlen_zero(dialog_id->call_id)
		|| ast_strlen_zero(dialog_id->local_tag)
		|| ast_strlen_zero(dialog_id->remote_tag)) {
		ast_log(LOG_WARNING,
			"cisco-remotecc: %s sent %s without a complete dialogid\n",
			endpoint_id, event_name);
		return 400;
	}

	session = cisco_dialog_session_lookup(dialog_id->call_id,
		dialog_id->local_tag, dialog_id->remote_tag);
	if (!session) {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s sent %s for unknown dialog (callid=%s)\n",
			endpoint_id, event_name, dialog_id->call_id);
		return 481;
	}
	chan = cisco_session_channel_ref(session);
	ao2_cleanup(session);
	if (!chan) {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s sent %s for dialog with no channel\n",
			endpoint_id, event_name);
		return 481;
	}
	if (ast_channel_state(chan) != AST_STATE_UP) {
		ast_log(LOG_NOTICE,
			"cisco-remotecc: %s sent %s for a non-answered call\n",
			endpoint_id, event_name);
		ast_channel_unref(chan);
		return 481;
	}

	d = ao2_alloc(sizeof(*d), record_task_data_destroy);
	if (!d) {
		ast_channel_unref(chan);
		return 500;
	}
	d->chan = chan;   /* hand the ref to d */
	ast_copy_string(d->endpoint_id, endpoint_id, sizeof(d->endpoint_id));
	d->start = start;

	if (ast_sip_push_task(remotecc_serializer, record_app_task, d)) {
		ast_log(LOG_WARNING,
			"cisco-remotecc: %s — failed to queue %s task\n",
			endpoint_id, event_name);
		ao2_cleanup(d);
		return 500;
	}

	return 202;
}
