/*
 * Internal header for res_pjsip_cisco_bulkupdate.so. Declarations
 * shared across the module's sibling .c files (res_pjsip_cisco_
 * bulkupdate.c, res/cisco_bulkupdate/cli.c, res/cisco_bulkupdate/func.c) —
 * all compiled into one .so. Nothing here is exported across module
 * boundaries; the .exports version script keeps the symbol table
 * local.
 */

#ifndef CISCO_BULKUPDATE_H
#define CISCO_BULKUPDATE_H

#include "asterisk/res_pjsip.h"

/*
 * Helpers defined in res_pjsip_cisco_bulkupdate.c and consumed by
 * both the CLI verbs (res/cisco_bulkupdate/cli.c) and the dialplan
 * functions (res/cisco_bulkupdate/func.c). Keep the REGISTER-hook task
 * plumbing private to the entry — these are the only operations
 * the per-feature siblings need.
 */

/*!
 * \brief Queue a fresh bulkupdate REFER for \a endpoint. Reads the
 *        current DND / HuntGroup / CF state from astdb at send time.
 *
 * \param fd       CLI fd for caller-visible diagnostics. -1 suppresses
 *                 (e.g. dialplan-function path).
 * \param endpoint Already-ref'd endpoint. The task takes its own ref;
 *                 the caller still owns theirs.
 * \retval 0 task queued (or no AORs — state stored, no REFER pushed).
 * \retval -1 allocation / queue failure.
 */
int cisco_bulkupdate_queue_refer(int fd, struct ast_sip_endpoint *endpoint);

/*!
 * \brief Resolve \a id to an ast_sip_endpoint, requiring a matching
 *        [name] type=cisco sorcery section. Prints a diagnostic to
 *        \a fd on any miss (no such endpoint id; non-Cisco endpoint).
 *        Returns the +1-ref'd endpoint, or NULL.
 */
struct ast_sip_endpoint *cisco_bulkupdate_resolve_endpoint(int fd,
	const char *id);

/*!
 * \brief Tab-completion helper: Nth Cisco endpoint id starting with
 *        \a word.
 */
char *cisco_bulkupdate_complete_endpoint(const char *word, int n);

/* CLI verbs + dialplan-function init/teardown, called from
 * load_module / unload_module in res_pjsip_cisco_bulkupdate.c. Each
 * keeps its registrations private (ast_cli_entry / ast_custom_function
 * tables stay static-internal to the matching .c file). */
int cisco_bulkupdate_cli_init(void);
void cisco_bulkupdate_cli_shutdown(void);
int cisco_bulkupdate_funcs_init(void);
void cisco_bulkupdate_funcs_shutdown(void);

/* Body templates separated into bulkupdate_bodies.h so the test
 * harness can include them without dragging in asterisk/res_pjsip.h.
 * Runtime callers transitively pick them up via this header. */
#include "bulkupdate_bodies.h"

#endif /* CISCO_BULKUPDATE_H */
