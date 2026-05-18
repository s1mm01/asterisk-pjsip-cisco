/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_unsolicited_blf
 *
 * Sends UNSOLICITED Event: presence NOTIFYs to a Cisco Enterprise SIP
 * firmware phone, one per watched extension, on two triggers:
 *
 *   1. Every successful REGISTER from the phone (refresh on session
 *      restart / connection re-establishment).
 *   2. Every state change of a watched extension while the phone is
 *      registered (real-time mid-call BLF updates).
 *
 * Cisco firmware does NOT send SUBSCRIBE for BLF — the original
 * cisco-usecallmanager chan_sip patch documents this explicitly in
 * its sample sip.conf comment: "cisco_usecallmanager phones don't
 * SUBSCRIBE to hints so they need to be configured" (via subscribe=).
 * The server is solely responsible for pushing presence state via
 * unsolicited NOTIFYs.
 *
 * For trigger (1) we hook REGISTER's outgoing_response supplement.
 * For trigger (2) we register an ast_extension_state callback per
 * (endpoint, watched-extension) pair on the first REGISTER and keep
 * it alive for the lifetime of the module; the callback queues a
 * fresh NOTIFY whenever Asterisk reports a state change.
 *
 * Spec is the chan_sip cisco-usecallmanager patch's
 *   channels/sip/peers.c:1840+ sip_peer_update_subscriptions  (initial
 *     push at REGISTER) and
 *   channels/sip/chan_sip.c cb_extensionstate           (per-state-change
 *     callback registered via ast_extension_state_add_extended).
 *
 * Watch list is read from the [name] type=cisco sorcery object's
 * 'subscribe' (CSV) and 'subscribe_context' fields. Outgoing NOTIFYs
 * keep the From URI host/port selected by ast_sip_create_request();
 * only the From user is replaced with the watched extension because
 * Cisco firmware maps unsolicited BLF state by that URI.
 */

/*** MODULEINFO
	<depend>res_pjsip</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/pbx.h"
#include "asterisk/presencestate.h"
#include "asterisk/taskprocessor.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_presence_xml.h"
#include "asterisk/sorcery.h"

#include "cisco/endpoint.h"
#include "cisco/rdata.h"
#include "cisco/register.h"

#include "pidf.h"

static struct ast_taskprocessor *unsolicited_serializer;
static struct ao2_container *unsolicited_addr_cache;

/*
 * Per-NOTIFY payload, kept alive across the whole transaction so the
 * response callback can identify which (endpoint, exten, contact)
 * tuple a non-2xx response corresponds to. Without it the failed
 * NOTIFY produces no log line at all — only a tcpdump capture would
 * reveal the rejection. The callback turns silent BLF rejections
 * into a visible NOTICE in messages.
 *
 * Ownership: the only ao2 ref is the one ast_sip_send_request gets
 * via its 'token' argument. PJSIP's internal send_request_cb wrapper
 * fires the user callback exactly once on transaction termination
 * (PJSIP_EVENT_TIMER / PJSIP_EVENT_TRANSPORT_ERROR / PJSIP_EVENT_RX_MSG
 * — see res_pjsip.c:1882-1928), which is where the payload is
 * released. Synchronous send_request failure consumes tdata and
 * returns -1 before any transaction is created, so the user callback
 * never fires; send_unsolicited_notify dec_refs the token manually
 * on that path. Matches stock callers — res_pjsip_messaging.c:771-776
 * is the canonical pattern.
 */
struct unsolicited_notify_payload {
	char *endpoint_id;
	char *contact_uri;
	char *exten;
};

static void unsolicited_notify_payload_destroy(void *obj)
{
	struct unsolicited_notify_payload *p = obj;

	ast_free(p->endpoint_id);
	ast_free(p->contact_uri);
	ast_free(p->exten);
}

static void unsolicited_response_cb(void *token, pjsip_event *e)
{
	struct unsolicited_notify_payload *p = token;
	int status_code;

	if (e->body.tsx_state.type == PJSIP_EVENT_TIMER) {
		ast_log(LOG_NOTICE,
			"cisco-unsolicited-blf: NOTIFY for '%s' timed out — "
			"endpoint='%s' contact='%s' (BLF buttons watching this "
			"extension on this contact will not light)\n",
			p->exten, p->endpoint_id, p->contact_uri);
		ao2_ref(p, -1);
		return;
	}
	if (e->body.tsx_state.type == PJSIP_EVENT_TRANSPORT_ERROR) {
		ast_log(LOG_NOTICE,
			"cisco-unsolicited-blf: NOTIFY for '%s' failed (transport error) — "
			"endpoint='%s' contact='%s' (BLF buttons watching this "
			"extension on this contact will not light)\n",
			p->exten, p->endpoint_id, p->contact_uri);
		ao2_ref(p, -1);
		return;
	}
	if (e->body.tsx_state.type != PJSIP_EVENT_RX_MSG
		|| !e->body.tsx_state.tsx) {
		ao2_ref(p, -1);
		return;
	}

	status_code = e->body.tsx_state.tsx->status_code;
	if (status_code >= 300) {
		ast_log(LOG_NOTICE,
			"cisco-unsolicited-blf: NOTIFY for '%s' rejected with %d — "
			"endpoint='%s' contact='%s' (BLF buttons watching this "
			"extension on this contact will not light)\n",
			p->exten, status_code, p->endpoint_id, p->contact_uri);
	} else {
		ast_debug(2,
			"cisco-unsolicited-blf: NOTIFY for '%s' accepted (%d) — "
			"endpoint='%s' contact='%s'\n",
			p->exten, status_code, p->endpoint_id, p->contact_uri);
	}
	ao2_ref(p, -1);
}

/*
 * Send a single unsolicited Event: presence NOTIFY to the phone for
 * the given watched extension.
 *
 * The Request-URI / To-URI rewrite for NAT'd contacts (so Cisco
 * firmware on the WAN side stops 400-ing the NOTIFYs) is handled by
 * the global on_tx_request hook in res_pjsip_cisco_endpoint.so. See
 * res/cisco_orig_host.{c,h} — it applies to every outbound SIP
 * request whose RURI carries an x-ast-orig-host parameter, so this
 * module doesn't need to opt in.
 */
static int send_unsolicited_notify(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact, const char *exten, const char *context)
{
	pjsip_tx_data *tdata = NULL;
	pj_str_t type;
	pj_str_t subtype;
	pj_str_t text;
	char *xml = NULL;
	char from_user[PJSIP_MAX_URL_SIZE];
	char domain_buf[PJSIP_MAX_URL_SIZE];
	const char *local_domain;
	int exten_state;
	int presence_state;

	/* Skip if the contact is known-dead. When a phone falls off the
	 * network its registration lingers (~1h) and asterisk keeps the
	 * contact marked UNAVAILABLE. Firing a NOTIFY at a dead
	 * transport=tcp contact makes asterisk attempt a fresh outbound
	 * TCP connect that just hangs in SYN-SENT — repeated on every
	 * state change of every watched exten. Suppress only when we KNOW
	 * it's gone (UNAVAILABLE/REMOVED); UNKNOWN/CREATED/AVAILABLE all
	 * still send, and a missing status object (NULL) is treated as
	 * "send anyway". */
	{
		struct ast_sip_contact_status *cs = ast_sip_get_contact_status(contact);

		if (cs) {
			enum ast_sip_contact_status_type st = cs->status;

			ao2_cleanup(cs);
			if (st == UNAVAILABLE || st == REMOVED) {
				ast_debug(2,
					"cisco-unsolicited-blf: skipping NOTIFY for %s -> %s "
					"(contact status %s)\n", exten, contact->uri,
					ast_sip_get_contact_status_label(st));
				return -1;
			}
		}
	}

	exten_state = ast_extension_state(NULL, context, exten);
	if (exten_state < 0) {
		exten_state = AST_EXTENSION_UNAVAILABLE;
	}

	/* DND state lives in the hint's presence channel (set by
	 * cisco_dnd_set / res_pjsip_cisco_endpoint's PJSIP: provider, or
	 * any ast_presence_state_changed source). Query separately —
	 * extension state alone doesn't reflect it.
	 *
	 * presencestate.c:165 unconditionally dereferences the subtype/
	 * message out-params (no NULL guard), so we MUST pass addresses of
	 * real char* locals — passing NULL,NULL crashes any presence
	 * lookup against a hint that has a non-empty presence component
	 * (e.g. "PJSIP/1010,PJSIP:1010"). The strings, if any, are
	 * allocated by the provider callback and we own them. */
	{
		char *p_subtype = NULL;
		char *p_message = NULL;

		presence_state = ast_hint_presence_state(NULL, context, exten,
			&p_subtype, &p_message);
		ast_free(p_subtype);
		ast_free(p_message);
	}
	if (presence_state < 0) {
		presence_state = AST_PRESENCE_NOT_SET;
	}

	if (ast_sip_create_request("NOTIFY", NULL, endpoint, NULL, contact, &tdata)) {
		ast_log(LOG_WARNING,
			"cisco-unsolicited-blf: create_request failed for %s\n", exten);
		return -1;
	}

	/*
	 * Force From header user to be the watched extension's name, not
	 * the endpoint's own. Cisco firmware uses the From URI user to
	 * bind the NOTIFY to the right line button. From host stays as
	 * whatever PJSIP picked at create time (endpoint bind or
	 * from_domain) — the phone does not validate it, per empirical
	 * testing against CP8861/14.1.1 and CP7975G/9.4.2.
	 */
	{
		pjsip_fromto_hdr *from;
		pjsip_sip_uri *from_uri;

		from = PJSIP_MSG_FROM_HDR(tdata->msg);
		from_uri = cisco_tdata_from_sip_uri(tdata);
		if (from_uri) {
			ast_uri_encode(exten, from_user, sizeof(from_user),
				ast_uri_sip_user);
			pj_strdup2(tdata->pool, &from_uri->user, from_user);
			from_uri->passwd.ptr = NULL;
			from_uri->passwd.slen = 0;
		} else {
			ast_log(LOG_WARNING,
				"cisco-unsolicited-blf: could not locate SIP From URI for %s\n",
				exten);
		}
		/* Re-randomise the from-tag so multiple parallel NOTIFYs don't collide. */
		if (from) {
			pj_create_unique_string(tdata->pool, &from->tag);
		}
	}

	local_domain = cisco_endpoint_local_domain(endpoint, tdata,
		domain_buf, sizeof(domain_buf));
	if (!strcmp(local_domain, "localhost")) {
		ast_log(LOG_NOTICE,
			"cisco-unsolicited-blf: endpoint '%s' has no usable From URI "
			"domain; using 'localhost' in PIDF body\n",
			ast_sorcery_object_get_id(endpoint));
	}

	xml = cisco_blf_build_pidf(exten, local_domain, exten_state, presence_state);
	if (!xml) {
		pjsip_tx_data_dec_ref(tdata);
		return -1;
	}

	ast_sip_add_header(tdata, "Event", "presence");
	ast_sip_add_header(tdata, "Subscription-State", "active;expires=3600");
	ast_sip_add_header(tdata, "Allow-Events", "presence, dialog, message-summary, refer");

	pj_strset2(&type, "application");
	pj_strset2(&subtype, "pidf+xml");
	pj_strdup2(tdata->pool, &text, xml);
	tdata->msg->body = pjsip_msg_body_create(tdata->pool, &type, &subtype, &text);
	ast_free(xml);
	if (!tdata->msg->body) {
		pjsip_tx_data_dec_ref(tdata);
		return -1;
	}

	/* Allocate the per-NOTIFY payload that lives across the whole
	 * transaction. The response callback owns the only ao2 ref (via
	 * the token argument to ast_sip_send_request) and releases it
	 * when the transaction terminates — see the struct comment for
	 * full lifetime rationale. The mod_data slot is a raw borrow the
	 * hook reads but does not refcount. */
	{
		struct unsolicited_notify_payload *payload;

		payload = ao2_alloc(sizeof(*payload),
			unsolicited_notify_payload_destroy);
		if (!payload) {
			pjsip_tx_data_dec_ref(tdata);
			return -1;
		}
		payload->endpoint_id = ast_strdup(
			ast_sorcery_object_get_id(endpoint));
		payload->contact_uri = ast_strdup(contact->uri);
		payload->exten = ast_strdup(exten);
		if (!payload->endpoint_id || !payload->contact_uri
			|| !payload->exten) {
			ao2_ref(payload, -1);
			pjsip_tx_data_dec_ref(tdata);
			return -1;
		}

		if (ast_sip_send_request(tdata, NULL, endpoint, payload,
				unsolicited_response_cb)) {
			ast_log(LOG_WARNING,
				"cisco-unsolicited-blf: send_request failed for %s\n",
				exten);
			/* No transaction created → response callback won't
			 * fire → token never released by anything but us.
			 * tdata's ref is already consumed by send_request
			 * (matches stock callers — res_pjsip_messaging.c:771,
			 * res_pjsip_notify.c:781). */
			ao2_ref(payload, -1);
			return -1;
		}
	}

	ast_debug(2,
		"cisco-unsolicited-blf: unsolicited NOTIFY sent for %s@%s -> %s "
		"(exten_state=0x%x presence=%s)\n",
		exten, context, contact->uri, exten_state,
		ast_presence_state2str(presence_state));
	return 0;
}

/* Forward declaration — the watcher registry is defined below the
 * REGISTER fanout task that calls into it. */
static void ensure_ext_state_watcher(const char *endpoint_id,
	const char *extension, const char *context);

struct unsolicited_task_data {
	struct ast_sip_endpoint *endpoint;
	/* When non-NULL: state-change path — send one NOTIFY per registered
	 * contact for this single watched (extension, context) pair.
	 * When NULL: REGISTER-fanout path — fan out the cisco->subscribe
	 * CSV under cisco->subscribe_context. */
	char *extension;
	char *context;
	/* Canonical Contact-set captured at REGISTER time. When set, the
	 * task commits to addr_cache after every per-(contact, exten) send
	 * succeeds. NULL for the state-change path (not cache-gated). */
	char *canonical;
};

static void unsolicited_task_data_destroy(void *obj)
{
	struct unsolicited_task_data *data = obj;
	ast_free(data->extension);
	ast_free(data->context);
	ast_free(data->canonical);
	ao2_cleanup(data->endpoint);
}

static int unsolicited_send_task(void *obj)
{
	struct unsolicited_task_data *data = obj;
	const char *endpoint_id;
	struct cisco_endpoint *cisco = NULL;
	const char *fanout_list;       /* CSV (REGISTER path) or single exten (state-change) */
	const char *fanout_context;
	int attempted = 0;
	int succeeded = 0;
	int is_fanout = (data->extension == NULL);

	endpoint_id = ast_sorcery_object_get_id(data->endpoint);

	if (ast_strlen_zero(data->endpoint->aors)) {
		ao2_cleanup(data);
		return 0;
	}

	if (is_fanout) {
		/* REGISTER-time path: fan out the cisco->subscribe list. */
		cisco = cisco_endpoint_get(endpoint_id);
		if (!cisco || ast_strlen_zero(cisco->subscribe)) {
			ao2_cleanup(cisco);
			ao2_cleanup(data);
			return 0;
		}
		fanout_list    = cisco->subscribe;
		fanout_context = cisco->subscribe_context;
	} else {
		/* State-change path: one (extension, context) pair. The
		 * strsep loop below trivially passes a single token through. */
		fanout_list    = data->extension;
		fanout_context = data->context;
	}

	{
		struct ao2_container *contacts;
		struct ao2_iterator iter;
		struct ast_sip_contact *contact;

		contacts = ast_sip_location_retrieve_contacts_from_aor_list(
			data->endpoint->aors);
		if (contacts) {
			iter = ao2_iterator_init(contacts, 0);
			while ((contact = ao2_iterator_next(&iter))) {
				char *list_copy = ast_strdupa(fanout_list);
				char *exten;
				while ((exten = ast_strip(strsep(&list_copy, ",")))) {
					if (ast_strlen_zero(exten)) {
						continue;
					}
					/* Per-(contact, exten) pair: each pair owes one
					 * NOTIFY. attempted++ before send so a failure
					 * leaves attempted > succeeded and the cache
					 * stays uncommitted. */
					attempted++;
					if (!send_unsolicited_notify(data->endpoint, contact,
							exten, fanout_context)) {
						succeeded++;
					}
					/* Register the state-change watcher only on the
					 * REGISTER-fanout entry — the state-change path
					 * IS that callback firing, so re-registering would
					 * be a no-op anyway. */
					if (is_fanout) {
						ensure_ext_state_watcher(endpoint_id, exten,
							fanout_context);
					}
				}
				ao2_cleanup(contact);
			}
			ao2_iterator_destroy(&iter);
			ao2_cleanup(contacts);
		}
	}

	ao2_cleanup(cisco);

	/* Commit the address-change cache only when ALL intended NOTIFYs
	 * (one per (contact, watched-exten) pair) actually went on the wire.
	 * Partial success with max_contacts > 1 or a partially-resolvable
	 * subscribe list would otherwise mark the endpoint "fired" and the
	 * failed pair would never get retried until the Contact set
	 * changed — cache is per-endpoint, so "all or nothing" is the only
	 * correct policy. State-change path has data->canonical == NULL and
	 * skips the commit entirely. */
	if (attempted > 0 && attempted == succeeded && data->canonical) {
		cisco_register_address_remember_str(endpoint_id,
			unsolicited_addr_cache, data->canonical);
	} else if (attempted != succeeded && data->canonical) {
		ast_log(LOG_NOTICE,
			"cisco-unsolicited-blf: %d/%d NOTIFYs delivered for '%s' — "
			"leaving address cache uncommitted so the next REGISTER retries\n",
			succeeded, attempted, endpoint_id);
	}

	ao2_cleanup(data);
	return 0;
}

/* ----------------------------------------------------------------------
 * Per-(endpoint, watched-extension) state-change subscription
 *
 * Mirrors the chan_sip patch's per-watched-exten ast_extension_state_add
 * call. We keep one callback per pair so that state changes on the
 * watched extension trigger a fresh unsolicited NOTIFY to the endpoint.
 *
 * Callbacks are created on demand at REGISTER time (when we see a
 * registering Cisco endpoint with a non-empty subscribe= list) and
 * never torn down — module unload removes them all. The dedupe is by
 * key "endpoint_id|extension"; subsequent REGISTERs find the existing
 * watcher and don't create a duplicate.
 * ---------------------------------------------------------------------- */

struct ext_state_watcher {
	/* All strings heap-allocated. Asterisk imposes no upper bound on
	 * endpoint ids, extension names, or context names — a fixed buffer
	 * here would silently truncate, mis-hashing the dedupe key and
	 * letting duplicate watchers register for the same logical pair.
	 * Same rationale as cisco_addr_cache_entry in cisco/endpoint.h. */
	char *key;              /* "endpoint_id|extension", for ao2 hash */
	char *endpoint_id;
	char *extension;
	char *context;
	int state_cb_id;
};

static struct ao2_container *ext_state_watchers;

static int ext_state_watcher_hash(const void *obj, const int flags)
{
	const struct ext_state_watcher *w;
	const char *key;

	switch (flags & OBJ_SEARCH_MASK) {
	case OBJ_SEARCH_KEY:
		key = obj;
		break;
	case OBJ_SEARCH_OBJECT:
		w = obj;
		key = w->key;
		break;
	default:
		ast_assert(0);
		return 0;
	}
	return ast_str_hash(key);
}

static int ext_state_watcher_cmp(void *obj, void *arg, int flags)
{
	const struct ext_state_watcher *left = obj;
	const char *right_key;

	switch (flags & OBJ_SEARCH_MASK) {
	case OBJ_SEARCH_OBJECT:
		right_key = ((const struct ext_state_watcher *) arg)->key;
		break;
	case OBJ_SEARCH_KEY:
		right_key = arg;
		break;
	default:
		ast_assert(0);
		return 0;
	}
	return strcmp(left->key, right_key) ? 0 : CMP_MATCH | CMP_STOP;
}

static void ext_state_watcher_destroy(void *obj)
{
	struct ext_state_watcher *w = obj;
	if (w->state_cb_id != -1) {
		ast_extension_state_del(w->state_cb_id, NULL);
	}
	ast_free(w->key);
	ast_free(w->endpoint_id);
	ast_free(w->extension);
	ast_free(w->context);
}

/*!
 * \brief Hint state changed for a watched extension — enqueue an
 *        unsolicited NOTIFY for the (endpoint, exten) pair through the
 *        shared task path. Sets extension / context so the task picks
 *        the single-pair branch and skips the cache commit.
 */
static int ext_state_cb(const char *context, const char *exten,
	struct ast_state_cb_info *info, void *data)
{
	struct ext_state_watcher *w = data;
	struct ast_sip_endpoint *endpoint;
	struct unsolicited_task_data *task_data;

	(void) info;

	endpoint = ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(), "endpoint",
		w->endpoint_id);
	if (!endpoint) {
		return 0;
	}

	task_data = ao2_alloc(sizeof(*task_data), unsolicited_task_data_destroy);
	if (!task_data) {
		ao2_cleanup(endpoint);
		return 0;
	}
	task_data->endpoint  = endpoint;  /* hand off the ref */
	task_data->extension = ast_strdup(exten);
	task_data->context   = ast_strdup(context);
	if (!task_data->extension || !task_data->context) {
		ao2_cleanup(task_data);
		return 0;
	}

	if (ast_sip_push_task(unsolicited_serializer, unsolicited_send_task,
			task_data)) {
		ast_log(LOG_WARNING,
			"cisco-unsolicited-blf: failed to queue state-change "
			"NOTIFY task for %s -> %s\n", exten, w->endpoint_id);
		ao2_cleanup(task_data);
	}

	return 0;
}

/*!
 * \brief Register a state-change callback for (endpoint, extension) if
 *        not already present. Idempotent; safe to call on every REGISTER.
 */
static void ensure_ext_state_watcher(const char *endpoint_id,
	const char *extension, const char *context)
{
	struct ext_state_watcher *w;
	char *key = NULL;

	if (!ext_state_watchers || ast_strlen_zero(endpoint_id)
		|| ast_strlen_zero(extension) || ast_strlen_zero(context)) {
		return;
	}

	if (ast_asprintf(&key, "%s|%s", endpoint_id, extension) < 0) {
		return;
	}

	w = ao2_find(ext_state_watchers, key, OBJ_SEARCH_KEY);
	if (w) {
		ast_free(key);
		ao2_cleanup(w);
		return;
	}

	w = ao2_alloc(sizeof(*w), ext_state_watcher_destroy);
	if (!w) {
		ast_free(key);
		return;
	}

	w->key         = key;       /* take ownership */
	w->endpoint_id = ast_strdup(endpoint_id);
	w->extension   = ast_strdup(extension);
	w->context     = ast_strdup(context);
	w->state_cb_id = -1;

	if (!w->endpoint_id || !w->extension || !w->context) {
		ao2_cleanup(w);
		return;
	}

	w->state_cb_id = ast_extension_state_add(context, extension,
		ext_state_cb, w);
	if (w->state_cb_id < 0) {
		ast_log(LOG_WARNING,
			"cisco-unsolicited-blf: ast_extension_state_add failed for "
			"%s@%s (watched by %s) — hint missing in dialplan?\n",
			extension, context, endpoint_id);
		ao2_cleanup(w);
		return;
	}

	ao2_link(ext_state_watchers, w);
	ast_debug(2,
		"cisco-unsolicited-blf: state watcher registered for %s@%s -> %s\n",
		extension, context, endpoint_id);
	ao2_cleanup(w);
}

static void unsolicited_outgoing_response(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact, pjsip_tx_data *tdata)
{
	struct unsolicited_task_data *data;
	char *canonical = NULL;

	if (!cisco_register_should_fire(endpoint, tdata, unsolicited_addr_cache,
			NULL, &canonical)) {
		return;
	}

	data = ao2_alloc(sizeof(*data), unsolicited_task_data_destroy);
	if (!data) {
		ast_free(canonical);
		return;
	}
	ao2_ref(endpoint, +1);
	data->endpoint  = endpoint;
	data->canonical = canonical;  /* take ownership */

	if (ast_sip_push_task(unsolicited_serializer, unsolicited_send_task, data)) {
		ast_log(LOG_WARNING, "cisco-unsolicited-blf: failed to queue task\n");
		ao2_cleanup(data);
		return;
	}

	(void) contact;
}

static struct ast_sip_supplement unsolicited_supplement = {
	.method            = "REGISTER",
	.priority          = AST_SIP_SUPPLEMENT_PRIORITY_LAST,
	.outgoing_response = unsolicited_outgoing_response,
};

static int load_module(void)
{
	unsolicited_serializer = ast_sip_create_serializer("pjsip/cisco-unsolicited-blf");
	if (!unsolicited_serializer) {
		return AST_MODULE_LOAD_DECLINE;
	}

	ext_state_watchers = ao2_container_alloc_hash(AO2_ALLOC_OPT_LOCK_MUTEX, 0,
		257, ext_state_watcher_hash, NULL, ext_state_watcher_cmp);
	if (!ext_state_watchers) {
		ast_taskprocessor_unreference(unsolicited_serializer);
		unsolicited_serializer = NULL;
		return AST_MODULE_LOAD_DECLINE;
	}

	unsolicited_addr_cache = cisco_addr_cache_alloc();
	if (!unsolicited_addr_cache) {
		ao2_cleanup(ext_state_watchers);
		ext_state_watchers = NULL;
		ast_taskprocessor_unreference(unsolicited_serializer);
		unsolicited_serializer = NULL;
		return AST_MODULE_LOAD_DECLINE;
	}

	ast_sip_register_supplement(&unsolicited_supplement);
	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	/* See res_pjsip_cisco_bulkupdate.c for rationale. Same pattern.
	 * Cleanup also tears down all ast_extension_state callbacks via
	 * the watcher destructor. */
	if (!ast_shutdown_final()) {
		return -1;
	}
	ast_sip_unregister_supplement(&unsolicited_supplement);
	ao2_cleanup(unsolicited_addr_cache);
	unsolicited_addr_cache = NULL;
	ao2_cleanup(ext_state_watchers);
	ext_state_watchers = NULL;
	ast_taskprocessor_unreference(unsolicited_serializer);
	unsolicited_serializer = NULL;
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco unsolicited BLF NOTIFY post-REGISTER",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_cisco_endpoint",
);
